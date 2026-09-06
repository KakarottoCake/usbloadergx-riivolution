/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * The on-demand boot sequence: reserve, place, install.
 *
 * Two things have to live in memory the booting game will not allocate over:
 * the redirect table, and the module that reads it. Both come out of one
 * reservation taken off the top of the game's MEM2 arena, because taking two
 * separate ones would mean lowering the arena boundary twice and getting the
 * second one wrong is silent - the game simply allocates over the first.
 *
 * The layout inside that reservation is fixed and checked here rather than
 * assumed:
 *
 *   newArenaHi -> [ module: code + bss ][ table ] <- old arena high
 *
 * The module goes first because it must be 32-byte aligned for the buffers
 * the storage engines DMA into, and the reservation itself is cache-line
 * aligned; the table only needs to be somewhere the module can read.
 *
 * The arithmetic is separated from the install so it can be host-tested. The
 * install itself writes to IOS and to low memory and is target-only.
 ***************************************************************************/
#ifndef RIIVO_ON_DEMAND_HPP_
#define RIIVO_ON_DEMAND_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include "RiivoMem2Reserve.hpp"

namespace Riivo
{
	//! Where everything ends up. Produced without touching memory.
	struct OnDemandLayout
	{
		bool ok;
		u32 moduleAddr;   //!< module base, cache-line aligned
		u32 tableAddr;    //!< the redirect table
		u32 tableLen;
		u32 newArenaHi;   //!< what MEM2 arena high must become
		u32 reserved;     //!< total taken from the game
		u32 heapLeft;
		std::string why;

		OnDemandLayout()
			: ok(false), moduleAddr(0), tableAddr(0), tableLen(0),
			  newArenaHi(0), reserved(0), heapLeft(0) {}
	};

	//! Decide the layout for a table of `tableLen` bytes plus the module,
	//! given the game's MEM2 arena. Refuses rather than overlapping anything.
	bool PlanOnDemand(const Mem2Arena &arena, u32 tableLen,
					  OnDemandLayout &out);

	//! Carry out the layout: lower the arena, copy the table, install the
	//! module and point the hook at it. Target only.
	//!
	//! `site` is a patch site the probe found. `partLba` is the FAT partition
	//! the table's paths are relative to. Returns false with `why` set and
	//! nothing changed, except that the arena may already have been lowered -
	//! which costs the game a little MEM2 and is otherwise harmless.
	bool InstallOnDemand(u32 site, const std::vector<u8> &table, u32 partLba,
						 OnDemandLayout &layout, std::string &why);
}

#endif
