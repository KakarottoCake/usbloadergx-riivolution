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

	//! True when an occupied entry is too broken to interpret: empty,
	//! inverted, or outside MEM1. Steering around ranges that cannot be
	//! read is guessing, so these refuse a grown placement outright (they
	//! never touch an in-place one, which consults no ranges at all).
	static bool IsMalformedRange(const OccupiedRange &r)
	{
		return r.hi <= r.lo || r.lo < MEM1_BASE || r.hi > MEM1_END;
	}

	//! True when a well-formed range sits fully inside the stale-table
	//! reservation: the expected overlap (the table being replaced, which
	//! an in-place install overwrites too). A range merely TOUCHING the
	//! reservation is a real obstacle - the T0 case on SB4E01, where an
	//! 8 KB apploader block ends exactly where the table begins.
	static bool IsStaleTableRange(const OccupiedRange &r, u32 resLo, u32 resHi)
	{
		return r.lo >= resLo && r.hi <= resHi;
	}

	FstPlacement PlaceFst(const ArenaInfo &info, u32 fstSize, u32 align,
						  const OccupiedRange *occ, u32 occCount)
	{
		if (align == 0)
			align = 32;
		if (align & (align - 1))
			return Refuse("alignment is not a power of two");

		if (fstSize == 0)
			return Refuse("the rebuilt table is empty");

		//! Everything must be somewhere the game can actually address. A zero
		//! here usually means the apploader has not run yet.
		if (info.fstAddr < MEM1_BASE || info.fstAddr >= MEM1_END)
			return Refuse("the file table is not in MEM1 - has the apploader run?");
		//! Zero means the apploader has not filled arena low in yet - common
		//! enough that refusing on it would rule out games that are otherwise
		//! perfectly placeable. Treat it as unknown and tighten up below.
		const bool blindLo = (info.arenaLo == 0);

		if (!blindLo && (info.arenaLo < MEM1_BASE || info.arenaLo >= MEM1_END))
			return Refuse("arena low is outside MEM1");
		if (info.arenaHi <= MEM1_BASE || info.arenaHi > MEM1_END)
			return Refuse("arena high is outside MEM1");
		if (!blindLo && info.arenaLo >= info.arenaHi)
			return Refuse("the arena is empty or inverted");

		//! The stale table's own reservation. Ranges overlapping it are the
		//! expected overlap and never obstacles; everything else loaded is.
		const u32 resTop = info.fstAddr + info.fstMaxSize;
		if (resTop < info.fstAddr || resTop > MEM1_END)
			return Refuse("the existing file table runs past the end of MEM1");
		if (!occ)
			occCount = 0;
		u32 skippedRanges = 0, malformedRanges = 0;
		for (u32 i = 0; i < occCount; ++i)
		{
			if (IsMalformedRange(occ[i]))
				++malformedRanges;
			else if (IsStaleTableRange(occ[i], info.fstAddr, resTop))
				++skippedRanges;
		}

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
			//! Nothing moved, so the heap is whatever it already was. With an
			//! unknown floor there is no figure to report.
			p.heapLeft = blindLo ? 0 : info.arenaHi - info.arenaLo;
			p.ignoredRanges = skippedRanges;
			p.malformedRanges = malformedRanges;
			return p;
		}

		//! A grown table steered by a list it cannot fully read would be
		//! placed around guesses. Refuse instead; the game boots unmodified.
		if (malformedRanges > 0)
		{
			FstPlacement p = Refuse("loaded ranges failed validation, so grown-table coverage is incomplete");
			p.ignoredRanges = skippedRanges;
			p.malformedRanges = malformedRanges;
			return p;
		}

		//! Case 2: it has to grow. Start with the top of the table where it is
		//! and extend downwards - then move the top down past every loaded
		//! range the table would overwrite, so nothing the apploader placed
		//! (which the game will read) has to move. The stale table's own
		//! reservation is the one exception: growing over it is the point.
		u32 top = resTop;

		//! Underflow first: on a small arena `top - fstSize` can wrap.
		if (fstSize > top - MEM1_BASE)
			return Refuse("the rebuilt table is larger than all of MEM1");

		u32 addr = 0;
		for (u32 guard = 0; ; ++guard)
		{
			if (fstSize > top - MEM1_BASE)
				return Refuse("the rebuilt table has no room below the game's loaded ranges");
			addr = (top - fstSize) & ~(align - 1);

			if (addr < MEM1_BASE)
				return Refuse("the rebuilt table does not fit below the existing one");

			//! Defensive cap: each pass moves the top strictly below at
			//! least one overlapping range, so occCount + 1 passes always
			//! suffice; anything more is a pathological list, not a layout.
			if (guard > occCount + 1)
				return Refuse("the loaded ranges leave no room for the rebuilt table");

			//! Move past every overlapping range at once, to the lowest of
			//! their bottoms: anything at or above that bottom is then clear
			//! of all of them by construction.
			u32 low = top;
			bool hit = false;
			for (u32 i = 0; i < occCount; ++i)
			{
				if (IsMalformedRange(occ[i])
					|| IsStaleTableRange(occ[i], info.fstAddr, resTop))
					continue;
				if (RangesOverlap(addr, addr + fstSize, occ[i].lo, occ[i].hi))
				{
					hit = true;
					if (occ[i].lo < low)
						low = occ[i].lo;
				}
			}
			if (!hit)
				break;
			top = low;
		}

		//! The table now starts below where arena high was, so the game's heap
		//! has to give up the difference.
		const u32 newArenaHi = addr < info.arenaHi ? addr : info.arenaHi;

		u32 heapLeft = 0;
		if (blindLo)
		{
			//! No floor to measure against, so bound the move instead. The
			//! table only ever takes memory from the top of the heap, which
			//! the game has not been given yet, and a drop this small out of
			//! MEM1's 24 MB cannot reach anything the apploader placed.
			if (info.arenaHi - newArenaHi > MAX_BLIND_DROP)
				return Refuse("the rebuilt table needs more than 1 MB and arena low "
							  "is not set, so the heap cannot be checked");
		}
		else
		{
			if (newArenaHi <= info.arenaLo)
				return Refuse("the rebuilt table would swallow the game's whole heap");

			heapLeft = newArenaHi - info.arenaLo;
			if (heapLeft < MIN_GAME_HEAP)
				return Refuse("the rebuilt table would leave the game under 4 MB of heap");
		}

		FstPlacement p;
		p.ok = true;
		p.inPlace = false;
		p.fstAddr = addr;
		p.newArenaHi = newArenaHi;
		p.reserved = info.arenaHi - newArenaHi;
		p.heapLeft = heapLeft;
		p.ignoredRanges = skippedRanges;
		p.malformedRanges = malformedRanges;
		return p;
	}

	//! True when an address range sits entirely inside MEM2.
	static bool InMem2(u32 addr, u32 size)
	{
		return size > 0 && addr >= MEM2_BASE && size <= MEM2_END - addr;
	}

	FstPlacement PlaceFstMem2(const ArenaInfo &info, u32 fstSize, u32 align)
	{
		if (align == 0)
			align = 32;
		if (align & (align - 1))
			return Refuse("alignment is not a power of two");

		if (fstSize == 0)
			return Refuse("the rebuilt table is empty");
		if (fstSize > MEM2_FST_CAP)
			return Refuse("the rebuilt table is larger than the MEM2 experiment allows");

		//! The arena passes through untouched, but it must still be sane:
		//! placing anything on garbage boot words is guessing.
		if (info.arenaHi <= MEM1_BASE || info.arenaHi > MEM1_END)
			return Refuse("arena high is outside MEM1");
		if (info.fstAddr < MEM1_BASE || info.fstAddr >= MEM1_END)
			return Refuse("the file table is not in MEM1 - has the apploader run?");

		u32 addr = (MEM2_FST_BASE + align - 1) & ~(align - 1);
		if (!InMem2(addr, fstSize) || addr + fstSize > MEM2_FST_BASE + MEM2_FST_CAP)
			return Refuse("the rebuilt table does not fit the MEM2 window");

		FstPlacement p;
		p.ok = true;
		p.inPlace = false;
		p.fstAddr = addr;
		p.newArenaHi = info.arenaHi; // untouched: MEM1 heap gives up nothing
		p.reserved = 0;
		p.heapLeft = 0; // not measured here; MEM1 heap is unchanged
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
		return fst.empty() ? false : InstallFst(place, &fst[0], (u32) fst.size());
	}

	bool InstallFst(const FstPlacement &place, const u8 *fst, u32 size)
	{
		if (!place.ok || !fst || size == 0)
			return false;

		//! PlaceFst has already proved this lands inside MEM1, but this write
		//! goes into the running game's memory, so check it again here rather
		//! than trust a struct that could have been built any number of ways.
		//! A MEM2 placement (experimental path) is checked against MEM2 the
		//! same way; anything in neither range is refused outright.
		const bool mem2 = place.fstAddr >= MEM2_BASE;
		if (mem2)
		{
			if (!InMem2(place.fstAddr, size))
				return false;
		}
		else
		{
			if (place.fstAddr < MEM1_BASE || place.fstAddr >= MEM1_END)
				return false;
			if ((u64) place.fstAddr + size > MEM1_END)
				return false;
		}

		memcpy((void *) place.fstAddr, fst, size);
		DCFlushRange((void *) place.fstAddr, size);

		//! Point the game at the new table and, if it grew, hand it the smaller
		//! heap. Order matters only in that both must be in place before the
		//! game starts; nothing is running yet.
		*(vu32 *) 0x80000038 = place.fstAddr;
		*(vu32 *) 0x8000003C = size;
		*(vu32 *) 0x80000034 = place.newArenaHi;
		DCFlushRange((void *) 0x80000030, 0x20);

		gprintf("Riivo: FST installed at %08x, %u bytes, arenaHi %08x\n",
				place.fstAddr, (unsigned) size, place.newArenaHi);
		return true;
	}

#else

	//! Host build: the tests only exercise PlaceFst, which is the part with the
	//! arithmetic worth checking.
	ArenaInfo ReadArenaInfo() { return ArenaInfo(); }
	bool InstallFst(const FstPlacement &, const std::vector<u8> &) { return false; }
	bool InstallFst(const FstPlacement &, const u8 *, u32) { return false; }

#endif
}
