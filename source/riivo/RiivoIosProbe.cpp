/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <gccore.h>
#include <ogcsys.h>
#include <ogc/system.h>

#include "RiivoIosProbe.hpp"
#include "RiivoProbeClassify.hpp"
#include "RiivoDiPatch.hpp"
#include "RiivoDiHook.hpp"
#include "libs/libruntimeiospatch/runtimeiospatch.h"
#include "gecko.h"

namespace Riivo
{
	//! MEM2, as the PPC sees it. The cached view is what we scan (fast); the
	//! uncached alias is 0x40000000 higher and is what we read through, because
	//! the Broadway and the Starlet do not snoop each other's caches - anything
	//! IOS wrote after we last touched a line would otherwise come back stale.
	static const u32 MEM2_CACHED   = 0x90000000;
	static const u32 MEM2_END      = 0x94000000;
	static const u32 UNCACHED_BIAS = 0x40000000;

	//! Skip the bottom of MEM2: that is where the loader's own heap lives, and
	//! our own copy of the fragment list sits there. It does NOT keep our own
	//! image out - the image sits high in MEM2, past this skip, which is how
	//! the scan once matched our own pattern copy.
	//!
	//! What keeps it out now is the arena bound: IsOurs treats everything at or
	//! below SYS_GetArena2Hi() as the PPC's, and IOS owns what is above. The
	//! +-1 MB window around our own pattern copy is only a fallback for a dead
	//! arena call - on the console it measured, the pattern sits in MEM1 and
	//! that window does not touch the scanned range at all.
	static const u32 SCAN_FROM = MEM2_CACHED + 0x00C00000;

	//! Dump window around a module pair: 32 KB before, 96 KB after. Sized so
	//! the dispatch the hook needs is likely inside whether the compiler put
	//! the literal pool near the handler or far from it. ApplyDiPatch reuses
	//! the same window for its verification snapshot.
	static const u32 DUMP_BEFORE = PROBE_DUMP_BEFORE;
	static const u32 DUMP_AFTER  = PROBE_DUMP_AFTER;

	static const size_t MAX_HITS = 24;

	static void Addf(std::string &out, const char *fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		out += buf;
	}

	//! Read one word straight out of RAM, bypassing the data cache.
	static inline u32 ReadUncached(u32 cachedAddr)
	{
		return *(vu32 *) (cachedAddr + UNCACHED_BIAS);
	}

	//! Word reader for the classifier, through the uncached alias.
	static u32 ReadWordUncached(u32 addr, void *ctx)
	{
		(void) ctx;
		return ReadUncached(addr);
	}

	//! Search the dispatch pattern inside [winLo, winHi), skipping anything
	//! that overlaps our own image window. Appends to patchSites, bounded by
	//! MAX_HITS.
	static void SearchPatternWindow(u32 winLo, u32 winHi, u32 selfLo, u32 selfHi,
									std::vector<u32> &patchSites)
	{
		//! Thumb instructions are halfword-aligned, so step by 2. Compare the
		//! first halfword before reading the rest - that rejects almost every
		//! address without touching memory again. The branch at +6 is skipped:
		//! it is position-dependent and must not be compared.
		const u16 first = ((u16) DI_READ_HEAD[0] << 8) | DI_READ_HEAD[1];
		for (u32 addr = winLo; addr + DI_READ_SPAN < winHi; addr += 2)
		{
			if (addr < selfHi && addr + DI_READ_SPAN > selfLo)
				continue;
			if (*(vu16 *) (addr + UNCACHED_BIAS) != first)
				continue;
			bool all = true;
			for (u32 k = 2; k < DI_READ_HEAD_LEN && all; ++k)
				if (*(vu8 *) (addr + k + UNCACHED_BIAS) != DI_READ_HEAD[k])
					all = false;
			for (u32 k = 0; k < DI_READ_TAIL_LEN && all; ++k)
				if (*(vu8 *) (addr + DI_READ_TAIL_OFF + k + UNCACHED_BIAS) != DI_READ_TAIL[k])
					all = false;
			if (all && patchSites.size() < MAX_HITS)
				patchSites.push_back(addr);
		}
	}

	//! Write one module window to the card with the usual self-describing
	//! header. Returns false when nothing was written.
	static bool WriteModuleDump(u32 base, u32 size, const std::string &path,
								u32 iosVersion, u32 iosRevision,
								IosProbe::DiDump &dump)
	{
		if (!size)
			return false;
		//! Copy through the uncached alias for the same reason as the scan.
		u8 *buf = (u8 *) malloc(size);
		if (!buf)
			return false;
		for (u32 i = 0; i < size; i += 4)
			*(u32 *) (buf + i) = ReadUncached(base + i);

		bool ok = false;
		FILE *f = fopen(path.c_str(), "wb");
		if (f)
		{
			//! A tiny header so the dump is self-describing: I need to know what
			//! address these bytes came from to disassemble them usefully.
			char hdr[64];
			int n = snprintf(hdr, sizeof(hdr), "RIIVODIP1%08x%08x%08x%08x",
							 (unsigned) base, (unsigned) size,
							 (unsigned) iosVersion, (unsigned) iosRevision);
			fwrite(hdr, 1, n, f);
			fwrite(buf, 1, size, f);
			fclose(f);
			dump.base = base;
			dump.size = size;
			dump.path = path;
			ok = true;
		}
		free(buf);
		return ok;
	}

	//! Dump path for one module window. A single candidate keeps the historic
	//! name; several get the pair address in the filename so they cannot
	//! overwrite each other.
	static std::string DumpPathFor(const std::string &basePath, u32 pairAddr, bool multi)
	{
		if (!multi)
			return basePath;
		std::string out = basePath;
		const std::string ext = ".bin";
		if (out.size() > ext.size()
			&& out.compare(out.size() - ext.size(), ext.size(), ext) == 0)
			out.erase(out.size() - ext.size());
		char suffix[16];
		snprintf(suffix, sizeof(suffix), "_%08x.bin", (unsigned) pairAddr);
		out += suffix;
		return out;
	}

	//! Log label for what anchored a window.
	static const char *AnchorKindName(DiAnchorKind kind)
	{
		if (kind == ANCHOR_FRAG)
			return "fragment code";
		if (kind == ANCHOR_STOCK)
			return "stock DI";
		return "d2x plugin";
	}

	//! One ModuleDumpWindow for an anchor address, appended to the pending list.
	static void AddPendingWindow(std::vector<DumpWindow> &pending,
								 u32 anchor, DiAnchorKind kind)
	{
		u32 base, size;
		ModuleDumpWindow(anchor, SCAN_FROM, MEM2_END, &base, &size);
		DumpWindow w;
		w.base = base;
		w.size = size;
		w.anchor = anchor;
		w.kind = kind;
		pending.push_back(w);
	}

	void ProbeIosPlugin(const std::string &dumpPath, IosProbe &out, bool writeDumps)
	{
		out = IosProbe();
		out.attempted = true;
		out.dumpsEnabled = writeDumps;
		out.iosVersion  = (u32) IOS_GetVersion();
		out.iosRevision = (u32) IOS_GetRevision();
		out.ahbprot     = AHBPROT_DISABLED;
		out.scanFrom    = SCAN_FROM;
		out.scanTo      = MEM2_END;

		//! Without AHBPROT the memory protection unit hides IOS from us and every
		//! read comes back as rubbish, so there is nothing to learn here.
		if (!out.ahbprot)
			return;

		//! The values worth looking for. Each is a literal that must exist
		//! somewhere in the module that holds it, and none is a number that
		//! turns up by accident.
		//!
		//! Note the d2x plugin and the stock DI module carry DIFFERENT read
		//! ceilings - d2x's dip.h says 0x46090000/0x7ED38000 while the stock
		//! module's table (the one libruntimeiospatch's di_readlimit patch
		//! rewrites) says 0x460A0000/0x7ED40000. That difference is what tells
		//! the two apart in the results.
		static const int NPAT = 6;
		IosPattern pats[NPAT];
		pats[0].name  = "d2x DVD9";
		pats[0].what  = "d2x dual-layer ceiling, in __DI_CheckOffset's literal pool";
		pats[0].value = PROBE_DVD9;
		pats[1].name  = "d2x DVD5";
		pats[1].what  = "d2x single-layer ceiling, same function";
		pats[1].value = PROBE_DVD5;
		pats[2].name  = "DIP thunk";
		pats[2].what  = "LDR r3,[PC,#0] + BX r3, written over the stock DI module";
		pats[2].value = PROBE_THUNK;
		pats[3].name  = "FRAG_MAX";
		pats[3].what  = "20000, the fragment list's maxnum";
		pats[3].value = PROBE_MAXFRAG;
		pats[4].name  = "stock DVD9";
		pats[4].what  = "stock DI module's ceiling - locates the ORIGINAL module";
		pats[4].value = PROBE_STOCK_DVD9;
		pats[5].name  = "stock DVD5";
		pats[5].what  = "stock DI module's ceiling, same table";
		pats[5].value = PROBE_STOCK_DVD5;

		//! One pass over MEM2 checking all of them, rather than one pass each.
		//! Every read goes through the uncached alias: the Broadway and the
		//! Starlet do not snoop each other, so a cached read could hand back a
		//! line from before IOS last wrote to it.
		u32 words = 0, nonZero = 0;
		for (u32 addr = SCAN_FROM; addr < MEM2_END; addr += 4)
		{
			const u32 v = ReadUncached(addr);
			++words;
			if (v)
				++nonZero;
			for (int i = 0; i < NPAT; ++i)
			{
				if (v != pats[i].value)
					continue;
				if (pats[i].hits.size() < MAX_HITS)
					pats[i].hits.push_back(addr);
			}
		}
		out.words = words;
		out.nonZero = nonZero;
		for (int i = 0; i < NPAT; ++i)
			out.patterns.push_back(pats[i]);

		//! Runtime identity, logged unconditionally: our own pattern copy and
		//! the MEM2 arena bounds, so every address below can be placed.
		out.selfAddr = (u32) &DI_READ_HEAD[0];
		LoaderWindow(out.selfAddr, &out.selfLo, &out.selfHi);
		out.arena2Lo = (u32) SYS_GetArena2Lo();
		out.arena2Hi = (u32) SYS_GetArena2Hi();

		//! Group the ceiling hits into module candidates. The dispatch is
		//! only searched for inside a surviving d2x candidate's window: the
		//! old whole-MEM2 search matched our own .rodata copy and the hook
		//! went into our own image. The read path lives in other modules,
		//! so their anchors below are evidence for dumps, never patch targets.
		std::vector<u32> dvd5Hits, thunkHits, maxfragHits, stock9Hits, stock5Hits;
		for (size_t i = 0; i < out.patterns.size(); ++i)
		{
			if (out.patterns[i].value == PROBE_DVD5)
				dvd5Hits = out.patterns[i].hits;
			else if (out.patterns[i].value == PROBE_THUNK)
				thunkHits = out.patterns[i].hits;
			else if (out.patterns[i].value == PROBE_MAXFRAG)
				maxfragHits = out.patterns[i].hits;
			else if (out.patterns[i].value == PROBE_STOCK_DVD9)
				stock9Hits = out.patterns[i].hits;
			else if (out.patterns[i].value == PROBE_STOCK_DVD5)
				stock5Hits = out.patterns[i].hits;
		}
		ClassifyModules(dvd5Hits, thunkHits, out.arena2Hi, out.selfAddr,
						SCAN_FROM, MEM2_END, ReadWordUncached, 0, out.modules);
		ClassifyFragAnchors(maxfragHits, out.arena2Hi, out.selfAddr,
							SCAN_FROM, out.fragAnchors);
		ClassifyStockAnchors(stock9Hits, stock5Hits, out.arena2Hi, out.selfAddr,
							 SCAN_FROM, out.stockAnchors);

		//! Survivors: d2x pairs that are not ours, with a hook marker before
		//! the pair. Zero or several means no patching - the log says why.
		std::vector<u32> candidates;
		for (size_t i = 0; i < out.modules.size(); ++i)
		{
			if (!out.modules[i].ours && out.modules[i].thunkAddr != 0)
				candidates.push_back(out.modules[i].pairAddr);
		}

		//! One dump window per anchor that is not ours, whatever kind found
		//! it. Overlapping windows merge below, so nearby anchors never write
		//! the same bytes twice. Fragment anchors are logged below but never
		//! dumped: MAX_FRAG proximity fires on non-fragment modules (it found
		//! OH1 once), and the read path lives in DIPP, so there is no
		//! fragment module to photograph.
		std::vector<DumpWindow> pending;
		for (size_t i = 0; i < candidates.size(); ++i)
			AddPendingWindow(pending, candidates[i], ANCHOR_D2X);
		for (size_t i = 0; i < out.stockAnchors.size(); ++i)
		{
			if (!out.stockAnchors[i].ours)
				AddPendingWindow(pending, out.stockAnchors[i].addr, ANCHOR_STOCK);
		}
		std::vector<DumpWindow> merged;
		u32 skipped = 0;
		MergeDumpWindows(pending, merged, &skipped);
		out.dumpsSkipped = skipped;

		//! The dispatch the hook needs lives somewhere near a d2x pair - the
		//! dump window is the search window, so a site found here is always
		//! inside the bytes we bring home. Windows anchored on other modules
		//! are not searched.
		const bool multi = merged.size() > 1;
		for (size_t i = 0; i < merged.size(); ++i)
		{
			bool hasD2x = false;
			for (size_t k = 0; k < candidates.size() && !hasD2x; ++k)
				hasD2x = candidates[k] >= merged[i].base
					  && candidates[k] < merged[i].base + merged[i].size;
			if (hasD2x)
				SearchPatternWindow(merged[i].base, merged[i].base + merged[i].size,
									out.selfLo, out.selfHi, out.patchSites);
			IosProbe::DiDump dump;
			dump.base = 0;
			dump.size = 0;
			dump.fallback = false;
			dump.anchor = merged[i].anchor;
			dump.kind = merged[i].kind;
			if (writeDumps && WriteModuleDump(merged[i].base, merged[i].size,
								DumpPathFor(dumpPath, merged[i].anchor, multi),
								out.iosVersion, out.iosRevision, dump))
				out.dumps.push_back(dump);
		}

		//! Nothing survived: dump the most promising ceiling hit outside our
		//! own image anyway. The classification rules come from one console,
		//! and a diagnostic round that brings home no bytes cannot be paid
		//! for twice. It is not searched for a dispatch, so it cannot be
		//! patched blind - the log marks it as a fallback.
		if (writeDumps && out.dumps.empty())
		{
			std::vector<u32> ceilingHits;
			for (size_t i = 0; i < out.patterns.size(); ++i)
			{
				if (out.patterns[i].value != PROBE_DVD5
					&& out.patterns[i].value != PROBE_DVD9)
					continue;
				const std::vector<u32> &h = out.patterns[i].hits;
				ceilingHits.insert(ceilingHits.end(), h.begin(), h.end());
			}
			const u32 anchor = FallbackAnchor(out.modules, ceilingHits,
											  out.arena2Hi, out.selfAddr, SCAN_FROM);
			if (anchor)
			{
				u32 base, size;
				ModuleDumpWindow(anchor, SCAN_FROM, MEM2_END, &base, &size);
				IosProbe::DiDump dump;
				dump.base = 0;
				dump.size = 0;
				dump.fallback = true;
				dump.anchor = anchor;
				dump.kind = ANCHOR_D2X;
				if (WriteModuleDump(base, size, dumpPath,
									out.iosVersion, out.iosRevision, dump))
					out.dumps.push_back(dump);
			}
		}
	}

	// The loader has no outstanding DI request here. Install the dormant RX
	// helper first and redirect the dispatch only after verifying its bytes.
	// PPC cache maintenance does not invalidate the Starlet I-cache; execution
	// through LOW_READ must be tested on hardware before release.
	static bool WriteCode(u32 address, const u8 *bytes, u32 size)
	{
		DCInvalidateRange((void *)(address & ~31u), size + 64);
		memcpy((void *)address, bytes, size);
		DCFlushRange((void *)(address & ~31u), size + 64);
		for (u32 i = 0; i < size; ++i)
			if (*(vu8 *)(address + UNCACHED_BIAS + i) != bytes[i])
				return false;
		return true;
	}

	bool ApplyDiPatch(u32 site, u32 endWords, std::string &why, u32 *storage)
	{
		if (storage) *storage = 0;
		if (!AHBPROT_DISABLED) {
			why = "AHBPROT is closed, so IOS memory cannot be written";
			return false;
		}
		if (site < SCAN_FROM + DUMP_BEFORE || site >= MEM2_END - DUMP_AFTER) {
			why = "the patch window is not in IOS MEM2";
			return false;
		}
		const u32 base = (site - DUMP_BEFORE) & ~31u;
		std::vector<u8> snapshot(DUMP_BEFORE + DUMP_AFTER + 32);
		for (u32 i = 0; i < snapshot.size(); ++i)
			snapshot[i] = *(vu8 *)(base + UNCACHED_BIAS + i);
		DiHookPlan plan;
		if (!BuildDiHook(&snapshot[0], snapshot.size(), base, site, endWords, plan, why))
			return false;
		const u8 *oldCode = &snapshot[plan.storage - base];
		const u8 *oldBranch = &snapshot[plan.dispatch - base];
		if (!WriteCode(plan.storage, &plan.code[0], plan.code.size()) ||
			!WriteCode(plan.dispatch, &plan.branch[0], plan.branch.size())) {
			const bool restoredBranch = WriteCode(plan.dispatch, oldBranch, plan.branch.size());
			const bool restoredCode = WriteCode(plan.storage, oldCode, plan.code.size());
			why = restoredBranch && restoredCode
				? "IOS patch write failed; original bytes restored"
				: "IOS patch rollback failed; restart the console before booting";
			return false;
		}
		gprintf("Riivo: LOW_READ hook at %08x, RX helper %08x, limit %08x words\n",
				 site, plan.storage, endWords);
		if (storage) *storage = plan.storage;
		return true;
	}

	std::string DescribeProbe(const IosProbe &p)
	{
		std::string out;
		out += "\n\ncIOS plugin probe\n-----------------\n";

		if (!p.attempted) {
			out += "Not attempted: setup was refused before the IOS probe.\n";
			return out;
		}
		if (!p.ahbprot)
		{
			out += "AHBPROT is not open, so IOS memory is hidden from the loader and\n"
				   "nothing could be read. This is the one thing the read hook cannot\n"
				   "work without. Launch the loader from the Homebrew Channel (not from\n"
				   "a forwarder that reloads IOS) so it keeps hardware access.\n";
			return out;
		}

		Addf(out, "AHBPROT   : open, IOS memory is readable\n");
		Addf(out, "running   : IOS%u (rev %u)\n", p.iosVersion, p.iosRevision);
		Addf(out, "scanned   : %08x .. %08x  (%u words, %u non-zero)\n",
			 p.scanFrom, p.scanTo, p.words, p.nonZero);
		Addf(out, "pattern   : own copy at %08x, loader window %08x..%08x\n",
			 p.selfAddr, p.selfLo, p.selfHi);
		if (p.arena2Hi > p.scanFrom)
			Addf(out, "arena2    : lo %08x hi %08x\n", p.arena2Lo, p.arena2Hi);
		else
			Addf(out, "arena2    : lo %08x hi %08x (unusable - ours decided by loader window)\n",
				 p.arena2Lo, p.arena2Hi);

		//! If almost everything read back as zero we were not actually seeing
		//! IOS, and every "0 hits" below means nothing.
		if (p.words && p.nonZero < p.words / 100)
		{
			out += "\nWARNING: nearly every word read back as zero. That is not what a\n"
				   "running IOS looks like, so these reads were probably refused rather\n"
				   "than empty, and the hit counts below are meaningless.\n";
		}
		out += "\n";

		for (size_t i = 0; i < p.patterns.size(); ++i)
		{
			const IosPattern &pat = p.patterns[i];
			Addf(out, "%-12s %08x  %u hit(s)   %s\n",
				 pat.name, pat.value, (unsigned) pat.hits.size(), pat.what);
			for (size_t k = 0; k < pat.hits.size(); ++k)
				Addf(out, "                 %08x\n", pat.hits[k]);
		}
		out += "\n";

		//! Every ceiling pair, and why it was kept or thrown away. Adjacency
		//! alone does not discriminate - our own image holds adjacent pairs
		//! too - so the verdict names the address range and the hook marker.
		out += "Candidate DI modules (ceiling pair + nearby hook marker)\n";
		out += "--------------------------------------------------------\n";
		if (p.modules.empty())
			out += "  none found\n";
		for (size_t i = 0; i < p.modules.size(); ++i)
		{
			const DiModule &m = p.modules[i];
			if (m.ours)
				Addf(out, "  %08x  ours (loader image - excluded)\n", m.pairAddr);
			else if (!m.thunkAddr)
				Addf(out, "  %08x  no hook marker within 512 bytes - not a module\n",
					 m.pairAddr);
			else
				Addf(out, "  %08x  module candidate, hook marker at %08x\n",
					 m.pairAddr, m.thunkAddr);
		}

		//! Read-path anchors. Ours entries are logged and never dumped; kept
		//! stock anchors became dump windows in the section below. Fragment
		//! anchors are logged only, never dumped (see above).
		out += "Fragment-code anchors (MAX_FRAG pairs)\n";
		out += "--------------------------------------\n";
		if (p.fragAnchors.empty())
			out += "  none found\n";
		for (size_t i = 0; i < p.fragAnchors.size(); ++i)
		{
			if (p.fragAnchors[i].ours)
				Addf(out, "  %08x  ours (loader image - excluded)\n",
					 p.fragAnchors[i].addr);
			else
				Addf(out, "  %08x  noted\n", p.fragAnchors[i].addr);
		}
		out += "Stock-DI anchors (read-limit table)\n";
		out += "-----------------------------------\n";
		if (p.stockAnchors.empty())
			out += "  none found\n";
		for (size_t i = 0; i < p.stockAnchors.size(); ++i)
		{
			if (p.stockAnchors[i].ours)
				Addf(out, "  %08x  ours (loader image - excluded)\n",
					 p.stockAnchors[i].addr);
			else
				Addf(out, "  %08x  kept\n", p.stockAnchors[i].addr);
		}

		//! The headline result. Sites are only searched for inside module
		//! windows, so our own image can no longer contribute one.
		out += "\nThe read dispatch that has to be patched\n";
		out += "---------------------------------------\n";
		if (p.patchSites.size() == 1)
		{
			Addf(out, "  FOUND, exactly once, at %08x.\n\n", p.patchSites[0]);
			out += "  Dispatch candidate found. The extended hook separately verifies\n"
				   "  its calling convention and executable helper before writing.\n";
		}
		else if (p.patchSites.empty())
		{
			size_t candidates = 0;
			for (size_t i = 0; i < p.modules.size(); ++i)
			{
				if (!p.modules[i].ours && p.modules[i].thunkAddr != 0)
					++candidates;
			}
			if (!candidates)
				out += "  NOT FOUND. No DI module window was identified above, so\n"
					   "  there was nowhere to search. Either this cIOS is not a\n"
					   "  d2x, or its read-limit check is built differently.\n";
			else
				out += "  NOT FOUND inside the module window(s). The running cIOS\n"
					   "  lays out the read routine differently from the d2x build\n"
					   "  the patch was derived from. The dump below is what is\n"
					   "  needed to re-derive it.\n";
		}
		else
		{
			Addf(out, "  found %u times - ambiguous, so it must not be applied blind:\n",
				 (unsigned) p.patchSites.size());
			for (size_t i = 0; i < p.patchSites.size(); ++i)
				Addf(out, "    %08x\n", p.patchSites[i]);
		}

		if (!p.dumps.empty())
		{
			for (size_t i = 0; i < p.dumps.size(); ++i)
			{
				Addf(out, "\nWrote %u bytes of the surrounding code from %08x to:\n  %s\n",
					 p.dumps[i].size, p.dumps[i].base, p.dumps[i].path.c_str());
				Addf(out, "  %s window anchored at %08x\n",
					 AnchorKindName(p.dumps[i].kind), p.dumps[i].anchor);
				if (p.dumps[i].fallback)
					out += "  FALLBACK: no candidate passed the checks above, so this\n"
						   "  window was taken on the strongest ceiling hit outside our\n"
						   "  own image. It was not searched for a dispatch.\n";
			}
			if (p.dumpsSkipped)
				Addf(out, "  %u further window(s) skipped (cap of %u)\n",
					 p.dumpsSkipped, PROBE_MAX_DUMPS);
			out += "Diagnostic dumps are pre-patch; they do not show the installed hook.\n";
		}
		else
		{
			out += p.dumpsEnabled ? "\nDiagnostic dump requested but no dump was written.\n"
				: "\nIOS dumps disabled (opt in with riivolution/dumpios.txt).\n";
		}

		return out;
	}
}
