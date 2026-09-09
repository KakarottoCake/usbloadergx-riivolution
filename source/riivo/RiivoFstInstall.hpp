/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Put a rebuilt file table into the running game's memory.
 *
 * The apploader reads the disc's file table into MEM1 and records where it put
 * it in the boot-info block at the bottom of memory:
 *
 *   0x80000030  arena low    - bottom of the heap the game may allocate from
 *   0x80000034  arena high   - top of that heap
 *   0x80000038  FST address  - where the table was loaded
 *   0x8000003C  FST max size - how much room was set aside for it
 *
 * The table normally sits at the very top of MEM1 with arena high pointing just
 * below it, precisely so the game's allocator never walks into it. A rebuilt
 * table is bigger than the original - a mod that adds 1881 files adds 1881
 * entries and their names - so it does not fit in the room the apploader
 * reserved. The fix is to extend downwards and pull arena high down with it,
 * which hands the table memory the game has not been told it owns yet.
 *
 * That is a few hundred KB out of MEM1's 24 MB. It is taken from the game's
 * heap, so it is not free, but it is the same trick the apploader itself uses.
 *
 * Getting an address wrong here writes over the running game and looks like a
 * hang with nothing on screen, so PlaceFst() refuses rather than guesses, and
 * every reason it can refuse for is spelled out. It is pure arithmetic with no
 * console dependency, so it is covered by host tests.
 ***************************************************************************/
#ifndef RIIVO_FST_INSTALL_HPP_
#define RIIVO_FST_INSTALL_HPP_

#include <gctypes.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "RiivoMem2Reserve.hpp" // MEM2_BASE/MEM2_TOP live there; reused, not redefined

namespace Riivo
{
	//! MEM1, as the game sees it. The boot-info fields must land inside this.
	static const u32 MEM1_BASE = 0x80000000;
	static const u32 MEM1_END  = 0x81800000;

	//! MEM2's end, matching RiivoMem2Reserve's MEM2_TOP (which owns the
	//! MEM2_BASE/MEM2_TOP pair - reused here, not redefined).
	static const u32 MEM2_END = MEM2_TOP;

	//! Experimental MEM2 table base, surveyed on SB4E01 under Dolphin: the
	//! game grows its MEM2 heap bottom-up from 0x90000000 (sparse use over
	//! the first megabyte in 90 s idle) while the top faults post-boot
	//! (outside the game's mapping), so the mid window has the most margin
	//! on both sides. NOT a derived safe address - a per-title surveyed
	//! one. Never the default; only the mem2fst.txt marker selects it.
	static const u32 MEM2_FST_BASE = 0x92000000;

	//! Largest table the experimental MEM2 path accepts. Way above any
	//! real rebuild (T0: 153934, big conversions: single-digit MB at most)
	//! and small enough to keep tens of megabytes of margin everywhere.
	static const u32 MEM2_FST_CAP = 1024 * 1024;

	//! Never squeeze the game's heap below this. A Wii game that cannot get
	//! 4 MB of MEM1 is going to fail anyway, and failing here - before anything
	//! is written - is far easier to diagnose than failing later.
	static const u32 MIN_GAME_HEAP = 4 * 1024 * 1024;

	//! Some apploaders leave arena low at zero and let the game's own startup
	//! fill it in, so a zero there means "not known yet", not "invalid". The
	//! table can still be placed - it only ever moves DOWN from arena high,
	//! which is memory the game has not been handed - but MIN_GAME_HEAP cannot
	//! be checked without knowing where the heap starts. In that case the drop
	//! is capped instead: a table that wants more than this without a known
	//! floor is refused rather than guessed at.
	static const u32 MAX_BLIND_DROP = 1024 * 1024;

	//! The four boot-info words, read straight out of low memory.
	struct ArenaInfo
	{
		u32 arenaLo;
		u32 arenaHi;
		u32 fstAddr;
		u32 fstMaxSize;

		ArenaInfo() : arenaLo(0), arenaHi(0), fstAddr(0), fstMaxSize(0) {}
	};

	//! Where a rebuilt table of a given size can go, or why it cannot.
	struct FstPlacement
	{
		bool ok;
		bool inPlace;      // it fitted in the room the apploader already reserved
		u32 fstAddr;       // where to write the table
		u32 newArenaHi;    // what arena high becomes
		u32 reserved;      // bytes taken out of the game's heap (0 when inPlace)
		u32 heapLeft;      // heap the game still has afterwards
		u32 ignoredRanges; // occupied ranges skipped as stale-table space
		u32 malformedRanges; // occupied entries too broken to interpret
		std::string why;   // populated only when !ok

		FstPlacement()
			: ok(false), inPlace(false), fstAddr(0), newArenaHi(0), reserved(0),
			  heapLeft(0), ignoredRanges(0), malformedRanges(0) {}
	};

	//! A RAM range the apploader loaded and the game will read: a DOL chunk,
	//! never the file-table reservation itself (that is the stale table being
	//! replaced, so growing over it is the whole point). A grown table must
	//! not be written over any of these.
	struct OccupiedRange
	{
		u32 lo; // first byte, inclusive
		u32 hi; // one past the last byte, exclusive

		OccupiedRange() : lo(0), hi(0) {}
		OccupiedRange(u32 l, u32 h) : lo(l), hi(h) {}
	};

	//! Work out where a rebuilt table of `fstSize` bytes can live, given what
	//! the apploader left behind. `align` is applied to the chosen address.
	//! `occ` lists RAM the game will read (DOL chunks); a table that has to
	//! grow is moved down past every entry it would overwrite. Entries
	//! inside the stale-table reservation are the expected overlap and are
	//! skipped (counted in FstPlacement::ignoredRanges). Malformed entries
	//! are counted in FstPlacement::malformedRanges and refuse a GROWN
	//! placement outright - steering around ranges that cannot be read is
	//! guessing, and the caller must hand over the complete list, never a
	//! capped or pre-filtered one. An in-place table never consults the
	//! list and is unaffected by either count. Does not touch memory - call
	//! Install() for that.
	FstPlacement PlaceFst(const ArenaInfo &info, u32 fstSize, u32 align,
						 const OccupiedRange *occ = 0, u32 occCount = 0);

	//! Experimental MEM2 placement for a table that cannot stay in MEM1:
	//! grown past the reservation on a game whose startup clears below it
	//! (measured on SB4E01). Takes the arena only to pass its high word
	//! through untouched (nothing is taken from the MEM1 heap, so there is
	//! nothing to deduct) and to refuse garbage input. No occupied list:
	//! MEM2 obstacles are not scanned, they are surveyed (see MEM2_FST_BASE),
	//! which is exactly why this stays behind the mem2fst.txt marker and
	//! refuses anything above MEM2_FST_CAP. Pure arithmetic, host-tested.
	FstPlacement PlaceFstMem2(const ArenaInfo &info, u32 fstSize, u32 align);

	//! Half-open interval overlap: [aLo,aHi) against [bLo,bHi). An empty or
	//! inverted interval overlaps nothing, so a zero length always reads
	//! "no". Used by the relocation-evidence block to test the planned
	//! destination against DOL ranges, the live stack and the heap extent.
	//! Pure arithmetic, host-tested alongside PlaceFst.
	inline bool RangesOverlap(u32 aLo, u32 aHi, u32 bLo, u32 bHi)
	{
		return aLo < aHi && bLo < bHi && aLo < bHi && bLo < aHi;
	}

	//! Scan [base, base+len) for 4-byte words equal to any of vals[0,nvals),
	//! recording up to maxHits byte offsets into hitOffs. Returns the total
	//! hit count, which may exceed maxHits - compare the two to tell
	//! "unlisted" from "absent". Unaligned head bytes are skipped, never
	//! faulted: PPC raises on unaligned loads. Endian note: words compare
	//! as the running CPU reads them, which is what boot-info words are on
	//! the console; host tests construct buffers with memcpy so both sides
	//! agree by construction. Pure, host-tested.
	inline u32 FindWordRefs(const u8 *base, u32 len, const u32 *vals, u32 nvals,
							u32 *hitOffs, u32 maxHits)
	{
		u32 hits = 0;
		if (!base || !vals || !hitOffs || nvals == 0)
			return 0;
		const u32 mis = (u32) (uintptr_t) base & 3;
		for (u32 o = mis ? 4 - mis : 0; o + 4 <= len; o += 4)
		{
			u32 w = 0;
			memcpy(&w, base + o, 4);
			for (u32 v = 0; v < nvals; ++v)
			{
				if (w == vals[v])
				{
					if (hits < maxHits)
						hitOffs[hits] = o;
					++hits;
					break;
				}
			}
		}
		return hits;
	}

	//! Read the four boot-info words out of low memory. Target only.
	ArenaInfo ReadArenaInfo();

	//! Copy `fst` to the placement and repoint the boot-info block at it.
	//! Target only; does nothing and returns false unless `place.ok`.
	bool InstallFst(const FstPlacement &place, const std::vector<u8> &fst);

	//! Same, for a table already sitting in its own buffer - which is how it
	//! arrives here, since it has to survive the apploader in MEM2.
	bool InstallFst(const FstPlacement &place, const u8 *fst, u32 size);
}

#endif
