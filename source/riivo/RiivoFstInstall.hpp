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
#include <string>
#include <vector>

namespace Riivo
{
	//! MEM1, as the game sees it. The boot-info fields must land inside this.
	static const u32 MEM1_BASE = 0x80000000;
	static const u32 MEM1_END  = 0x81800000;

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

	//! Half-open interval overlap: [aLo,aHi) against [bLo,bHi). An empty or
	//! inverted interval overlaps nothing, so a zero length always reads
	//! "no". Used by the relocation-evidence block to test the planned
	//! destination against DOL ranges, the live stack and the heap extent.
	//! Pure arithmetic, host-tested alongside PlaceFst.
	inline bool RangesOverlap(u32 aLo, u32 aHi, u32 bLo, u32 bHi)
	{
		return aLo < aHi && bLo < bHi && aLo < bHi && bLo < aHi;
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
