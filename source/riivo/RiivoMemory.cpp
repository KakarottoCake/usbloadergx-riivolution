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

		u8 *addr = (u8 *) (m.offset | 0x80000000);

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
