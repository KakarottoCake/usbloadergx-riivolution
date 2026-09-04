/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include "RiivoMemory.hpp"
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
	// File helpers (valuefile loading); JoinPath lives in RiivoConfig.
	// --------------------------------------------------------------------

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

	//! Resolve the bytes to write: inline value, or the contents of valuefile.
	static bool ResolveBytes(const ResolvedMemory &m, const std::string &device, std::vector<u8> &out)
	{
		if (!m.valuefile.empty())
		{
			std::string path = JoinPath(device, m.root, m.valuefile);
			if (!LoadFile(path, out))
			{
				gprintf("Riivo mem: valuefile not found: %s\n", path.c_str());
				return false;
			}
			return !out.empty();
		}
		out = m.value;
		return !out.empty();
	}

	// --------------------------------------------------------------------
	// The three patch kinds
	// --------------------------------------------------------------------

	static bool ApplyDirect(const ResolvedMemory &m, const std::string &device)
	{
		std::vector<u8> value;
		if (!ResolveBytes(m, device, value))
			return false;

		const u32 target = m.offset | 0x80000000;
		const size_t span = value.size() > m.original.size() ? value.size() : m.original.size();
		if (!ValidTarget(target, span))
		{
			gprintf("Riivo mem: direct target 0x%08x (+%u) outside RAM, skipped\n",
					target, (unsigned) span);
			return false;
		}

		u8 *addr = (u8 *) target;

		if (!m.original.empty() && memcmp(addr, &m.original[0], m.original.size()) != 0)
		{
			gprintf("Riivo mem: direct original mismatch @ %p, skipped\n", addr);
			return false;
		}

		memcpy(addr, &value[0], value.size());
		DCFlushRange(addr, value.size());
		ICInvalidateRange(addr, value.size());
		gprintf("Riivo mem: direct wrote %u byte(s) @ %p\n", (unsigned) value.size(), addr);
		return true;
	}

	static bool ApplySearch(const ResolvedMemory &m, const std::string &device)
	{
		if (m.original.empty())
			return false;
		std::vector<u8> value;
		if (!ResolveBytes(m, device, value))
			return false;

		const u32 stride = m.align < 1 ? 1 : m.align;
		const u32 plen = (u32) m.original.size();

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
					//! `value` may be longer than the pattern it replaces; never
					//! let a match near the end of a section overrun it.
					if (i + (u32) value.size() > (u32) len)
					{
						gprintf("Riivo mem: search match at +%u would overrun the section, skipped\n",
								(unsigned) i);
						continue;
					}
					memcpy(dst + i, &value[0], value.size());
					DCFlushRange(dst + i, value.size());
					ICInvalidateRange(dst + i, value.size());
					gprintf("Riivo mem: search matched @ %p, wrote %u byte(s)\n",
							dst + i, (unsigned) value.size());
					return true; // first match only, like Dolphin
				}
			}
		}
		gprintf("Riivo mem: search pattern not found\n");
		return false;
	}

	static bool ApplyOcarina(const ResolvedMemory &m)
	{
		if (m.value.empty())
			return false;

		const u32 target = m.offset | 0x80000000;
		const u32 plen = (u32) m.value.size();

		if (!ValidTarget(target, 4))
		{
			gprintf("Riivo mem: ocarina branch target 0x%08x outside RAM, skipped\n", target);
			return false;
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
				// Found the pattern; advance to the next blr and branch to target.
				for (u32 j = i; j + 4 <= (u32) len; j += 4)
				{
					if (*(u32 *) (dst + j) == PPC_BLR)
					{
						u32 blrAddr = (u32) (dst + j); // final load address == runtime address
						u32 branch = ((target - blrAddr) & 0x03FFFFFC) | 0x48000000;
						*(u32 *) (dst + j) = branch;
						DCFlushRange(dst + j, 4);
						ICInvalidateRange(dst + j, 4);
						gprintf("Riivo mem: ocarina hooked blr @ 0x%08x -> 0x%08x\n", blrAddr, target);
						return true;
					}
				}
				gprintf("Riivo mem: ocarina pattern found but no blr after it\n");
				return false;
			}
		}
		gprintf("Riivo mem: ocarina pattern not found\n");
		return false;
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

	int ApplyMemoryPatches(const ResolvedPatchSet &set, const std::string &device)
	{
		int applied = 0;
		for (size_t i = 0; i < set.memories.size(); ++i)
		{
			const ResolvedMemory &m = set.memories[i];
			bool ok;
			if (m.ocarina)
				ok = ApplyOcarina(m);
			else if (m.search)
				ok = ApplySearch(m, device);
			else
				ok = ApplyDirect(m, device);
			if (ok)
				++applied;
		}
		gprintf("Riivo mem: applied %d/%u memory patch(es)\n", applied, (unsigned) set.memories.size());
		return applied;
	}
}
