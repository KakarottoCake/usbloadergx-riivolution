/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Set aside a piece of MEM2 that the booting game's allocator will not claim.
 *
 * The redirect table has to outlive the loader: IOS reads it while the game
 * runs, long after everything that built it is gone. So it cannot sit in the
 * loader's own heap, and it cannot sit anywhere the game will allocate from.
 * At 2802 files the table is around 168 KB, which is far too big to hide in
 * the dead routine inside DIPP that the DI hook uses for its own storage, and
 * far too big to copy into an IOS heap that often has only tens of kilobytes
 * free.
 *
 * MEM2 is split between the two processors, and the boundary is a word in low
 * memory:
 *
 *   0x80003130  MEM2 arena low   - bottom of the game's MEM2 heap
 *   0x80003134  MEM2 arena high  - top of it; above this belongs to IOS
 *
 * A game's OSInit reads those two words once during startup and hands them to
 * its heap manager, which may then use any page between them. Lowering arena
 * high before jumping to the game's entry point hands us the gap: memory the
 * game has not been told it owns. It is the same trick the loader already uses
 * on MEM1 for the rebuilt file table (see RiivoFstInstall), and the same one
 * the apploader itself uses.
 *
 * On the address: it was checked rather than taken on trust. libruntimeiospatch
 * reads 0x80003134 and scans upward from it to 0x94000000 looking for IOS code,
 * which only makes sense if that word is the PPC/IOS boundary, and USB Loader
 * GX's own gamepatches.c uses the MEM1 equivalent the same way. An earlier
 * answer put this at 0x80003138; nothing in any shipped source supports that,
 * and 0x80003138 is left alone here.
 *
 * There is no alternative hiding place. Below arena low is the IPC page and
 * the bottom of MEM2, with nothing like 168 KB spare; above arena high is
 * Starlet's, where a PPC access faults unless AHBPROT is down and where IOS
 * puts its IPC buffers. Moving the boundary is the mechanism.
 *
 * A game that reloads IOS mid-boot would have low memory reinitialised under
 * it and lose this reservation - but such a game also loses d2x's fragment
 * list and its open partition, so USB Loader GX already blocks IOS reloads by
 * default (GameBooter passes anything but OFF to BlockIOSReload). The
 * reservation is safe under exactly the configurations in which the loader
 * works at all.
 *
 * Pure arithmetic, no console dependency, so it is host-tested. Getting it
 * wrong hands the game memory it will overwrite the table with, which shows up
 * as the mod half-working and then corrupting - so this refuses rather than
 * guesses.
 ***************************************************************************/
#ifndef RIIVO_MEM2_RESERVE_HPP_
#define RIIVO_MEM2_RESERVE_HPP_

#include <gctypes.h>
#include <string>

namespace Riivo
{
	//! The two low-memory words the game's OSInit reads for its MEM2 heap.
	static const u32 MEM2_ARENA_LO_ADDR = 0x80003130;
	static const u32 MEM2_ARENA_HI_ADDR = 0x80003134;

	//! MEM2 as the PPC sees it. The arena must lie inside this.
	static const u32 MEM2_BASE = 0x90000000;
	static const u32 MEM2_TOP  = 0x94000000;

	//! Never squeeze the game's MEM2 heap below this. A Wii game denied 8 MB
	//! of MEM2 will fail somewhere later and less legibly than here.
	static const u32 MIN_GAME_MEM2 = 8 * 1024 * 1024;

	//! IOS reads the table across the bus, so the reservation is aligned to a
	//! cache line and flushed. Anything coarser is fine; anything finer is not.
	static const u32 MEM2_RESERVE_ALIGN = 32;

	//! A sanity ceiling. Nothing legitimate asks for a quarter of MEM2, and a
	//! size this large almost certainly means a count was miscomputed.
	static const u32 MAX_MEM2_RESERVE = 8 * 1024 * 1024;

	struct Mem2Arena
	{
		u32 lo;
		u32 hi;

		Mem2Arena() : lo(0), hi(0) {}
		Mem2Arena(u32 l, u32 h) : lo(l), hi(h) {}
	};

	//! Where the reservation can go, or why it cannot.
	struct Mem2Reservation
	{
		bool ok;
		u32 addr;         // where to put the table
		u32 newArenaHi;   // what arena high becomes (== addr)
		u32 reserved;     // bytes taken from the game, after alignment
		u32 heapLeft;     // MEM2 the game still has
		std::string why;  // populated only when !ok

		Mem2Reservation()
			: ok(false), addr(0), newArenaHi(0), reserved(0), heapLeft(0) {}
	};

	//! Work out where `size` bytes can be reserved, given what the game was
	//! left. Touches no memory - call CommitMem2Reservation for that.
	Mem2Reservation ReserveMem2(const Mem2Arena &arena, u32 size, u32 align);

	//! Read the two words out of low memory. Target only.
	Mem2Arena ReadMem2Arena();

	//! Lower arena high so the game never allocates the reservation, and flush
	//! it so IOS sees the write. Target only; does nothing unless `r.ok`.
	bool CommitMem2Reservation(const Mem2Reservation &r);
}

#endif
