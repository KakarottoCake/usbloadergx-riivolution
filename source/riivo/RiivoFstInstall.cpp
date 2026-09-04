/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include "RiivoFstInstall.hpp"

#ifdef GEKKO
#include <gccore.h>
#include "gecko.h"
#endif

namespace Riivo
{
	//! Small helper so the refusal reasons read as sentences rather than codes.
	static FstPlacement Refuse(const char *why)
	{
		FstPlacement p;
		p.ok = false;
		p.why = why;
		return p;
	}

	FstPlacement PlaceFst(const ArenaInfo &info, u32 fstSize, u32 align)
	{
		if (align == 0)
			align = 32;
		//! A power of two is assumed by the masking below.
		if (align & (align - 1))
			return Refuse("alignment is not a power of two");

		if (fstSize == 0)
			return Refuse("the rebuilt table is empty");

		//! Everything must be somewhere the game can actually address. A zero
		//! here usually means the apploader has not run yet.
		if (info.fstAddr < MEM1_BASE || info.fstAddr >= MEM1_END)
			return Refuse("the file table is not in MEM1 - has the apploader run?");
		if (info.arenaLo < MEM1_BASE || info.arenaLo >= MEM1_END)
			return Refuse("arena low is outside MEM1");
		if (info.arenaHi <= MEM1_BASE || info.arenaHi > MEM1_END)
			return Refuse("arena high is outside MEM1");
		if (info.arenaLo >= info.arenaHi)
			return Refuse("the arena is empty or inverted");

		//! Case 1: it fits in the room the apploader already set aside. Nothing
		//! moves, the game's heap is untouched, and this is by far the safest
		//! outcome - it happens whenever a mod only replaces files.
		if (fstSize <= info.fstMaxSize)
		{
			FstPlacement p;
			p.ok = true;
			p.inPlace = true;
			p.fstAddr = info.fstAddr;
			p.newArenaHi = info.arenaHi;
			p.reserved = 0;
			p.heapLeft = info.arenaHi - info.arenaLo;
			return p;
		}

		//! Case 2: it has to grow. Keep the top of the table where it is and
		//! extend downwards, so nothing above it (which the apploader may have
		//! placed deliberately) has to move.
		const u32 top = info.fstAddr + info.fstMaxSize;
		if (top < info.fstAddr || top > MEM1_END)
			return Refuse("the existing file table runs past the end of MEM1");

		//! Underflow first: on a small arena `top - fstSize` can wrap.
		if (fstSize > top - MEM1_BASE)
			return Refuse("the rebuilt table is larger than all of MEM1");

		u32 addr = top - fstSize;
		addr &= ~(align - 1);

		if (addr < MEM1_BASE)
			return Refuse("the rebuilt table does not fit below the existing one");

		//! The table now starts below where arena high was, so the game's heap
		//! has to give up the difference.
		const u32 newArenaHi = addr < info.arenaHi ? addr : info.arenaHi;

		if (newArenaHi <= info.arenaLo)
			return Refuse("the rebuilt table would swallow the game's whole heap");

		const u32 heapLeft = newArenaHi - info.arenaLo;
		if (heapLeft < MIN_GAME_HEAP)
			return Refuse("the rebuilt table would leave the game under 4 MB of heap");

		FstPlacement p;
		p.ok = true;
		p.inPlace = false;
		p.fstAddr = addr;
		p.newArenaHi = newArenaHi;
		p.reserved = info.arenaHi - newArenaHi;
		p.heapLeft = heapLeft;
		return p;
	}

#ifdef GEKKO

	ArenaInfo ReadArenaInfo()
	{
		ArenaInfo info;
		info.arenaLo    = *(vu32 *) 0x80000030;
		info.arenaHi    = *(vu32 *) 0x80000034;
		info.fstAddr    = *(vu32 *) 0x80000038;
		info.fstMaxSize = *(vu32 *) 0x8000003C;
		return info;
	}

	bool InstallFst(const FstPlacement &place, const std::vector<u8> &fst)
	{
		if (!place.ok || fst.empty())
			return false;

		//! PlaceFst has already proved this lands inside MEM1, but this write
		//! goes into the running game's memory, so check it again here rather
		//! than trust a struct that could have been built any number of ways.
		if (place.fstAddr < MEM1_BASE || place.fstAddr >= MEM1_END)
			return false;
		if ((u64) place.fstAddr + fst.size() > MEM1_END)
			return false;

		memcpy((void *) place.fstAddr, &fst[0], fst.size());
		DCFlushRange((void *) place.fstAddr, fst.size());

		//! Point the game at the new table and, if it grew, hand it the smaller
		//! heap. Order matters only in that both must be in place before the
		//! game starts; nothing is running yet.
		*(vu32 *) 0x80000038 = place.fstAddr;
		*(vu32 *) 0x8000003C = (u32) fst.size();
		*(vu32 *) 0x80000034 = place.newArenaHi;
		DCFlushRange((void *) 0x80000030, 0x20);

		gprintf("Riivo: FST installed at %08x, %u bytes, arenaHi %08x\n",
				place.fstAddr, (unsigned) fst.size(), place.newArenaHi);
		return true;
	}

#else

	//! Host build: the tests only exercise PlaceFst, which is the part with the
	//! arithmetic worth checking.
	ArenaInfo ReadArenaInfo() { return ArenaInfo(); }
	bool InstallFst(const FstPlacement &, const std::vector<u8> &) { return false; }

#endif
}
