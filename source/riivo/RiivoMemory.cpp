/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include "RiivoMemory.hpp"
#include "RiivoPatchGuard.hpp"
#include "RiivoConfig.hpp"
#include "patches/gamepatches.h"
#include "gecko.h"

namespace Riivo
{
	static const u32 PPC_BLR = 0x4E800020;

	//! Reject patch targets that fall outside the console's usable RAM. A bad or
	//! mismatched XML would otherwise scribble over whatever happens to live at
	//! that address (including the loader itself) and hard-crash before the game
	//! ever starts, with nothing on screen to explain why.
	static bool ValidTarget(u32 addr, size_t len)
	{
		if (len == 0)
			return false;
		const u64 end = (u64) addr + (u64) len;
		if (addr >= 0x80000000 && end <= 0x81800000) // MEM1
			return true;
		if (addr >= 0x90000000 && end <= 0x94000000) // MEM2
			return true;
		return false;
	}

	// --------------------------------------------------------------------
	// Read-only check helpers. Check*() decides WHETHER a patch fits;
	// CommitWrite() below is the single place that decides HOW bytes go in.
	// --------------------------------------------------------------------

	static void FillOutcome(MemOutcome &o, size_t index, const char *kind,
							u32 target, u32 writeAddr, MemCheck check)
	{
		o.index = index;
		o.kind = kind;
		o.target = target;
		o.writeAddr = writeAddr;
		o.writeLen = 0;
		o.check = check;
		o.wantLen = 0;
		o.gotLen = 0;
	}

	//! Copy up to 16 bytes for the preflight table. Callers set the full
	//! lengths beside it; the formatter caps the display.
	//! NOTE (recovery 2026-09-06): re-typed after a tooling accident emptied
	//! this file. Logic is pinned by hosttests/test_memcheck.cpp; exact prior
	//! wording is not guaranteed.
	static void SnapBytes(u8 *dst, const u8 *src, u32 len)
	{
		memcpy(dst, src, len < 16 ? len : 16);
	}

	//! Value bytes come only from PreloadValueFiles, which inlines every
	//! readable valuefile and clears the field while devices are up. Anything
	//! still naming a file failed then.
	//! NOTE (recovery 2026-09-06): re-typed, see SnapBytes note above.
	static bool PreflightValueReady(const ResolvedMemory &m)
	{
		return m.valuefile.empty() && !m.value.empty();
	}

	//! NOTE (recovery 2026-09-06): re-typed, see SnapBytes note above. Order
	//! is load-bearing and is pinned by tests: value first (never touches
	//! RAM), then target range, then the original-bytes read.
	static void CheckDirect(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		const u32 target = m.offset | 0x80000000;
		if (!PreflightValueReady(m))
		{
			FillOutcome(o, index, "direct", target, 0, MEM_CHECK_HARD_NO_VALUE);
			return;
		}
		const size_t span = m.value.size() > m.original.size()
			? m.value.size() : m.original.size();
		if (!ValidTarget(target, span))
		{
			FillOutcome(o, index, "direct", target, 0, MEM_CHECK_HARD_BAD_TARGET);
			return;
		}
		const u8 *addr = (const u8 *)(uintptr_t) target;
		if (!m.original.empty() && memcmp(addr, &m.original[0], m.original.size()) != 0)
		{
			FillOutcome(o, index, "direct", target, 0, MEM_CHECK_SOFT_ORIG_MISMATCH);
			o.wantLen = (u32) m.original.size();
			SnapBytes(o.want, &m.original[0], o.wantLen);
			o.gotLen = (u32) m.original.size();
			SnapBytes(o.got, addr, o.gotLen);
			return;
		}
		FillOutcome(o, index, "direct", target, target, MEM_CHECK_OK);
		o.writeLen = (u32) m.value.size();
	}

	//! Branch word written by ocarina patches. Factored so the verifier
	//! compares against exactly what the patcher writes, not a second copy
	//! of the formula.
	//! NOTE (recovery 2026-09-06): re-typed, see SnapBytes note above. The
	//! formula itself is pinned by hosttests/test_memcheck.cpp.
	u32 EncodeOcarinaBranch(u32 target, u32 blrAddr)
	{
		return ((target - blrAddr) & 0x03FFFFFC) | 0x48000000;
	}

	static bool LoadFile(const std::string &path, std::vector<u8> &out)
	{
		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz <= 0)
		{
			fclose(f);
			return false;
		}
		out.resize((size_t) sz);
		size_t rd = fread(&out[0], 1, (size_t) sz, f);
		fclose(f);
		out.resize(rd);
		return rd == (size_t) sz;
	}

	//! Value bytes come only from PreloadValueFiles, which inlines every
	//! readable valuefile and clears the field while devices are up. Anything
	//! still naming a file failed then; the preflight and the apply path both
	//! treat that as missing without touching the filesystem (PreflightValueReady),
	//! because the apply window runs after ShutDownDevices, where a reopen
	//! would fail anyway.

	// --------------------------------------------------------------------
	// The three patch kinds
	// --------------------------------------------------------------------

	//! Single write path for all three kinds: copy, protect, flush,
	//! invalidate. Checks decide *whether* to write; this decides *how*.
	static void CommitWrite(u32 addr, const u8 *bytes, u32 len)
	{
		u8 *dst = (u8 *)(uintptr_t) addr;
		memcpy(dst, bytes, len);
		ProtectAppliedPatch(addr, len);
		DCFlushRange(dst, len);
		ICInvalidateRange(dst, len);
	}

	static bool ApplyDirect(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		CheckDirect(m, index, o);
		if (o.check != MEM_CHECK_OK)
		{
			if (o.check == MEM_CHECK_HARD_NO_VALUE)
				gprintf("Riivo mem: direct has no value to write @ 0x%08x, skipped\n", o.target);
			else if (o.check == MEM_CHECK_HARD_BAD_TARGET)
				gprintf("Riivo mem: direct target 0x%08x (+%u) outside RAM, skipped\n",
						o.target, (unsigned)(m.value.size() > m.original.size()
											 ? m.value.size() : m.original.size()));
			else
				gprintf("Riivo mem: direct original mismatch @ 0x%08x, skipped\n", o.target);
			return false;
		}

		u8 *addr = (u8 *)(uintptr_t) o.target;
		CommitWrite(o.target, &m.value[0], (u32) m.value.size());
		gprintf("Riivo mem: direct wrote %u byte(s) @ %p\n", (unsigned) m.value.size(), addr);
		return true;
	}

	static void CheckSearch(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		if (m.original.empty())
		{
			FillOutcome(o, index, "search", 0, 0, MEM_CHECK_HARD_NO_PATTERN);
			return;
		}
		if (!PreflightValueReady(m))
		{
			FillOutcome(o, index, "search", 0, 0, MEM_CHECK_HARD_NO_VALUE);
			return;
		}
		const u32 stride = m.align < 1 ? 1 : m.align;
		const u32 plen = (u32) m.original.size();

		//! First fitting match wins, like Dolphin. An overrun match does not
		//! end the search (an identical pattern may sit elsewhere with room),
		//! but when nothing fits the first match is what the log names.
		u32 first = 0;
		bool seen = false;
		int count = RiivoGetDOLCount();
		for (int s = 0; s < count; ++s)
		{
			u8 *dst = RiivoGetDOLDst(s);
			int len = RiivoGetDOLLen(s);
			if (!dst || len < (int) plen)
				continue;
			for (u32 i = 0; i + plen <= (u32) len; i += stride)
			{
				if (memcmp(dst + i, &m.original[0], plen) == 0)
				{
					const u32 match = (u32)(uintptr_t)(dst + i);
					if (!seen)
					{
						seen = true;
						first = match;
					}
					//! `value` may be longer than the pattern it replaces; never
					//! let a match near the end of a section overrun it.
					if (i + (u32) m.value.size() > (u32) len)
						continue;
					FillOutcome(o, index, "search", match, match, MEM_CHECK_OK);
					o.writeLen = (u32) m.value.size();
					return;
				}
			}
		}
		if (seen)
		{
			FillOutcome(o, index, "search", first, 0, MEM_CHECK_SOFT_OVERRUN);
			o.wantLen = plen;
			SnapBytes(o.want, &m.original[0], plen);
			return;
		}
		FillOutcome(o, index, "search", 0, 0, MEM_CHECK_SOFT_NOT_FOUND);
		o.wantLen = plen;
		SnapBytes(o.want, &m.original[0], plen);
	}

	static bool ApplySearch(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		CheckSearch(m, index, o);
		if (o.check != MEM_CHECK_OK)
		{
			if (o.check == MEM_CHECK_HARD_NO_PATTERN)
				gprintf("Riivo mem: search has no pattern, skipped\n");
			else if (o.check == MEM_CHECK_HARD_NO_VALUE)
				gprintf("Riivo mem: search has no value to write, skipped\n");
			else if (o.check == MEM_CHECK_SOFT_OVERRUN)
				gprintf("Riivo mem: search match at 0x%08x would overrun the section, skipped\n",
						o.target);
			else
				gprintf("Riivo mem: search pattern not found\n");
			return false;
		}

		//! First match only, like Dolphin. The address came from the check
		//! above, against the same bytes, so this cannot disagree with it.
		u8 *dst = (u8 *)(uintptr_t) o.writeAddr;
		CommitWrite(o.writeAddr, &m.value[0], (u32) m.value.size());
		gprintf("Riivo mem: search matched @ %p, wrote %u byte(s)\n",
				dst, (unsigned) m.value.size());
		return true; // first match only, like Dolphin
	}

	static void CheckOcarina(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		if (m.value.empty())
		{
			FillOutcome(o, index, "ocarina", 0, 0, MEM_CHECK_HARD_NO_PATTERN);
			return;
		}

		const u32 target = m.offset | 0x80000000;
		const u32 plen = (u32) m.value.size();

		if (!ValidTarget(target, 4))
		{
			FillOutcome(o, index, "ocarina", target, 0, MEM_CHECK_HARD_BAD_TARGET);
			return;
		}

		int count = RiivoGetDOLCount();
		for (int s = 0; s < count; ++s)
		{
			u8 *dst = RiivoGetDOLDst(s);
			int len = RiivoGetDOLLen(s);
			if (!dst || len < (int) plen)
				continue;
			for (u32 i = 0; i + plen <= (u32) len; i += 4)
			{
				if (memcmp(dst + i, &m.value[0], plen) != 0)
					continue;
				const u32 found = (u32)(uintptr_t)(dst + i);
				// Found the pattern; advance to the next blr and branch to target.
				for (u32 j = i; j + 4 <= (u32) len; j += 4)
				{
					if (*(u32 *) (dst + j) == PPC_BLR)
					{
						u32 blrAddr = (u32)(uintptr_t)(dst + j); // final load address == runtime address
						FillOutcome(o, index, "ocarina", target, blrAddr, MEM_CHECK_OK);
						o.writeLen = 4;
						return;
					}
				}
				FillOutcome(o, index, "ocarina", target, found, MEM_CHECK_SOFT_OVERRUN);
				o.wantLen = plen;
				SnapBytes(o.want, &m.value[0], plen);
				return;
			}
		}
		FillOutcome(o, index, "ocarina", target, 0, MEM_CHECK_SOFT_NOT_FOUND);
		o.wantLen = plen;
		SnapBytes(o.want, &m.value[0], plen);
	}

	static bool ApplyOcarina(const ResolvedMemory &m, size_t index, MemOutcome &o)
	{
		CheckOcarina(m, index, o);
		if (o.check != MEM_CHECK_OK)
		{
			if (o.check == MEM_CHECK_HARD_NO_PATTERN)
				gprintf("Riivo mem: ocarina has no pattern, skipped\n");
			else if (o.check == MEM_CHECK_HARD_BAD_TARGET)
				gprintf("Riivo mem: ocarina branch target 0x%08x outside RAM, skipped\n", o.target);
			else if (o.check == MEM_CHECK_SOFT_OVERRUN)
				gprintf("Riivo mem: ocarina pattern found but no blr after it\n");
			else
				gprintf("Riivo mem: ocarina pattern not found\n");
			return false;
		}

		u32 branch = EncodeOcarinaBranch(o.target, o.writeAddr);
		CommitWrite(o.writeAddr, (const u8 *) &branch, 4);
		gprintf("Riivo mem: ocarina hooked blr @ 0x%08x -> 0x%08x\n", o.writeAddr, o.target);
		return true;
	}

	// --------------------------------------------------------------------
	// Entry point
	// --------------------------------------------------------------------

	int PreloadValueFiles(ResolvedPatchSet &set, const std::string &device)
	{
		int failed = 0;
		for (size_t i = 0; i < set.memories.size(); ++i)
		{
			ResolvedMemory &m = set.memories[i];
			if (m.valuefile.empty())
				continue;

			const std::string path = JoinPath(device, m.root, m.valuefile);
			std::vector<u8> bytes;
			if (LoadFile(path, bytes) && !bytes.empty())
			{
				m.value.swap(bytes);
				m.valuefile.clear(); // now inlined; no file access needed later
				gprintf("Riivo mem: preloaded %u byte(s) from %s\n",
						(unsigned) m.value.size(), path.c_str());
			}
			else
			{
				gprintf("Riivo mem: FAILED to preload valuefile %s\n", path.c_str());
				++failed;
			}
		}
		return failed;
	}

	int ApplyMemoryPatches(const ResolvedPatchSet &set, const std::string &device,
						   std::vector<MemOutcome> &appliedOut)
	{
		(void) device; // valuefiles are preloaded; see PreflightValueReady
		appliedOut.clear();
		int applied = 0;
		for (size_t i = 0; i < set.memories.size(); ++i)
		{
			const ResolvedMemory &m = set.memories[i];
			MemOutcome o;
			bool ok;
			if (m.ocarina)
				ok = ApplyOcarina(m, i, o);
			else if (m.search)
				ok = ApplySearch(m, i, o);
			else
				ok = ApplyDirect(m, i, o);
			appliedOut.push_back(o);
			if (ok)
				++applied;
		}
		gprintf("Riivo mem: applied %d/%u memory patch(es)\n", applied, (unsigned) set.memories.size());
		return applied;
	}

	int PreflightMemoryPatches(const ResolvedPatchSet &set, std::vector<MemOutcome> &out)
	{
		out.clear();
		int hard = 0;
		for (size_t i = 0; i < set.memories.size(); ++i)
		{
			const ResolvedMemory &m = set.memories[i];
			MemOutcome o;
			if (m.ocarina)
				CheckOcarina(m, i, o);
			else if (m.search)
				CheckSearch(m, i, o);
			else
				CheckDirect(m, i, o);
			if (o.check == MEM_CHECK_HARD_NO_VALUE
				|| o.check == MEM_CHECK_HARD_BAD_TARGET
				|| o.check == MEM_CHECK_HARD_NO_PATTERN)
				++hard;
			out.push_back(o);
		}
		return hard;
	}

	int MemPreflightHardFails(const std::vector<MemOutcome> &out)
	{
		int hard = 0;
		for (size_t i = 0; i < out.size(); ++i)
		{
			if (out[i].check == MEM_CHECK_HARD_NO_VALUE
				|| out[i].check == MEM_CHECK_HARD_BAD_TARGET
				|| out[i].check == MEM_CHECK_HARD_NO_PATTERN)
				++hard;
		}
		return hard;
	}

	bool AllMemoryPatchesOk(const ResolvedPatchSet &set,
							const std::vector<MemOutcome> &app)
	{
		if (app.size() != set.memories.size())
			return false;
		for (size_t i = 0; i < app.size(); ++i)
		{
			if (app[i].check != MEM_CHECK_OK)
				return false;
		}
		return true;
	}

	static const char *CheckName(MemCheck check)
	{
		switch (check)
		{
			case MEM_CHECK_OK: return "ok";
			case MEM_CHECK_HARD_NO_VALUE: return "HARD no-value";
			case MEM_CHECK_HARD_BAD_TARGET: return "HARD bad-target";
			case MEM_CHECK_HARD_NO_PATTERN: return "HARD no-pattern";
			case MEM_CHECK_SOFT_ORIG_MISMATCH: return "SKIP original-mismatch";
			case MEM_CHECK_SOFT_NOT_FOUND: return "SKIP not-found";
			default: break;
		}
		return "SKIP overrun";
	}

	static void HexAppend(std::string &out, const u8 *bytes, u32 len)
	{
		char buf[4];
		for (u32 i = 0; i < len; ++i)
		{
			snprintf(buf, sizeof(buf), "%02x", bytes[i]);
			out += buf;
			if (i + 1 < len)
				out += ' ';
		}
	}

	std::string DescribeMemPreflight(const std::vector<MemOutcome> &out)
	{
		std::string text;
		text += "\nMemory patch preflight (before device shutdown)\n";
		text += "Read-only dry run in apply order; SKIP outcomes here become skips at entry.\n";
		char line[128];
		u32 ok = 0, soft = 0, hard = 0;
		for (size_t i = 0; i < out.size(); ++i)
		{
			const MemOutcome &o = out[i];
			if (o.check == MEM_CHECK_OK)
				++ok;
			else if (o.check == MEM_CHECK_HARD_NO_VALUE
					 || o.check == MEM_CHECK_HARD_BAD_TARGET
					 || o.check == MEM_CHECK_HARD_NO_PATTERN)
				++hard;
			else
				++soft;
			snprintf(line, sizeof(line), "  [%u] %s @0x%08x %s",
					 (unsigned) o.index, o.kind, o.target, CheckName(o.check));
			text += line;
			if (o.check == MEM_CHECK_SOFT_ORIG_MISMATCH)
			{
				char sizes[64];
				snprintf(sizes, sizeof(sizes), " want %u: ", o.wantLen);
				text += sizes;
				HexAppend(text, o.want, o.wantLen < 16 ? o.wantLen : 16);
				text += " got: ";
				HexAppend(text, o.got, o.gotLen);
			}
			else if ((o.check == MEM_CHECK_SOFT_NOT_FOUND
					  || o.check == MEM_CHECK_SOFT_OVERRUN) && o.wantLen)
			{
				char sizes[64];
				snprintf(sizes, sizeof(sizes), " pattern %u: ", o.wantLen);
				text += sizes;
				HexAppend(text, o.want, o.wantLen < 16 ? o.wantLen : 16);
			}
			text += "\n";
		}
		snprintf(line, sizeof(line),
				 "  preflight: %u checked, %u ok, %u soft, %u hard\n",
				 (unsigned) out.size(), ok, soft, hard);
		text += line;
		return text;
	}

	//! Returns the position in `out` of the most recent OK outcome in
	//! [begin, end) whose written bytes overlap [addr, addr+len), or -1.
	//! Patch order is significant: a later write over shared bytes supersedes
	//! an earlier one, so a later patch's changed result may be caused by an
	//! earlier patch's write rather than by loader patches in between.
	int FindOverlappingWrite(const std::vector<MemOutcome> &out,
							 size_t begin, size_t end, u32 addr, u32 len)
	{
		if (!addr || !len)
			return -1;
		int found = -1;
		for (size_t j = begin; j < end && j < out.size(); ++j)
		{
			const MemOutcome &o = out[j];
			if (o.check != MEM_CHECK_OK || !o.writeAddr || !o.writeLen)
				continue;
			if (o.writeAddr < addr + len && addr < o.writeAddr + o.writeLen)
				found = (int) j;
		}
		return found;
	}

	std::string DescribeMemApplySummary(const std::vector<MemOutcome> &pre,
										const std::vector<MemOutcome> &app, int applied)
	{
		std::string text;
		char line[160];
		u32 soft = 0, hard = 0;
		for (size_t i = 0; i < app.size(); ++i)
		{
			if (app[i].check == MEM_CHECK_OK)
				continue;
			if (app[i].check == MEM_CHECK_HARD_NO_VALUE
				|| app[i].check == MEM_CHECK_HARD_BAD_TARGET
				|| app[i].check == MEM_CHECK_HARD_NO_PATTERN)
				++hard;
			else
				++soft;
		}
		snprintf(line, sizeof(line), "Riivo mem: applied %d/%u at entry",
				 applied, (unsigned) app.size());
		text += line;
		if (!pre.empty() && pre.size() == app.size())
		{
			u32 pok = 0, psoft = 0, phard = 0;
			for (size_t i = 0; i < pre.size(); ++i)
			{
				if (pre[i].check == MEM_CHECK_OK)
					++pok;
				else if (pre[i].check == MEM_CHECK_HARD_NO_VALUE
						 || pre[i].check == MEM_CHECK_HARD_BAD_TARGET
						 || pre[i].check == MEM_CHECK_HARD_NO_PATTERN)
					++phard;
				else
					++psoft;
			}
			snprintf(line, sizeof(line), " (preflight: %u ok, %u soft, %u hard)",
					 pok, psoft, phard);
			text += line;
		}
		text += "\n";
		if (soft || hard)
		{
			snprintf(line, sizeof(line), "Riivo mem: skipped %u soft, %u hard\n", soft, hard);
			text += line;
		}
		//! Loader patches run between the preflight and the apply, so a patch
		//! the preflight passed but the apply skipped names bytes they touched.
		u32 shown = 0, changed = 0;
		for (size_t i = 0; i < app.size() && i < pre.size(); ++i)
		{
			if (pre[i].check != MEM_CHECK_OK || app[i].check == MEM_CHECK_OK)
				continue;
			++changed;
			if (shown >= 8)
				continue;
			snprintf(line, sizeof(line),
					 "Riivo mem: changed after preflight: [%u] %s @0x%08x now %s\n",
					 (unsigned) app[i].index, app[i].kind, app[i].target,
					 CheckName(app[i].check));
			text += line;
			//! Attribute sequential dependency before blaming loader patches:
			//! an earlier applied write over the intended bytes is the likely cause.
			int overlap = FindOverlappingWrite(app, 0, i, pre[i].writeAddr, pre[i].writeLen);
			if (overlap >= 0)
			{
				snprintf(line, sizeof(line),
						 "Riivo mem:   overlaps earlier write [%u] %s @0x%08x, likely cause\n",
						 (unsigned) app[(size_t) overlap].index,
						 app[(size_t) overlap].kind, app[(size_t) overlap].writeAddr);
				text += line;
			}
			++shown;
		}
		if (changed > shown)
		{
			snprintf(line, sizeof(line), "Riivo mem: ... and %u more changed\n",
					 changed - shown);
			text += line;
		}
		return text;
	}

	int VerifyAppliedPatches(const ResolvedPatchSet &set,
							 const std::vector<MemOutcome> &app, const char *stage)
	{
		int mismatches = 0, checked = 0;
		char line[160];
		for (size_t i = 0; i < app.size(); ++i)
		{
			const MemOutcome &o = app[i];
			if (o.check != MEM_CHECK_OK || !o.writeAddr || o.index >= set.memories.size())
				continue;
			//! Last writer wins: a later OK outcome overwriting any of these
			//! bytes owns the final content, so this one must not be checked
			//! against its own value (that would false-positive).
			if (FindOverlappingWrite(app, i + 1, app.size(), o.writeAddr, o.writeLen) >= 0)
				continue;
			const ResolvedMemory &m = set.memories[o.index];
			u8 expect[4];
			const u8 *want = NULL;
			u32 wantLen = 0;
			if (!strcmp(o.kind, "ocarina"))
			{
				u32 branch = EncodeOcarinaBranch(o.target, o.writeAddr);
				expect[0] = (u8)(branch >> 24);
				expect[1] = (u8)(branch >> 16);
				expect[2] = (u8)(branch >> 8);
				expect[3] = (u8) branch;
				want = expect;
				wantLen = 4;
			}
			else if (!m.value.empty())
			{
				want = &m.value[0];
				wantLen = (u32) m.value.size();
			}
			else
				continue;
			++checked;
			if (memcmp((u8 *)(uintptr_t) o.writeAddr, want, wantLen) != 0)
			{
				++mismatches;
				snprintf(line, sizeof(line),
						 "Riivo mem: [%s] write at 0x%08x no longer matches (%s)",
						 stage, o.writeAddr, o.kind);
				gprintf("%s\n", line);
			}
		}
		gprintf("Riivo mem: [%s] %d mismatch(es) in %d checked write(s)\n",
				stage, mismatches, checked);
		return mismatches;
	}
}
