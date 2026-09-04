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
		std::string why;   // populated only when !ok

		FstPlacement()
			: ok(false), inPlace(false), fstAddr(0), newArenaHi(0), reserved(0),
			  heapLeft(0) {}
	};

	//! Work out where a rebuilt table of `fstSize` bytes can live, given what
	//! the apploader left behind. `align` is applied to the chosen address.
	//! Does not touch memory - call Install() for that.
	FstPlacement PlaceFst(const ArenaInfo &info, u32 fstSize, u32 align);

	//! Read the four boot-info words out of low memory. Target only.
	ArenaInfo ReadArenaInfo();

	//! Copy `fst` to the placement and repoint the boot-info block at it.
	//! Target only; does nothing and returns false unless `place.ok`.
	bool InstallFst(const FstPlacement &place, const std::vector<u8> &fst);
}

#endif
