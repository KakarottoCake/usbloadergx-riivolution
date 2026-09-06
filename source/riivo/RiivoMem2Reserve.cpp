/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoMem2Reserve.hpp for what this reserves and why the boundary word
 * is the one it is.
 ***************************************************************************/
#include "RiivoMem2Reserve.hpp"

#ifdef GEKKO
#include <ogc/cache.h>
#include <ogc/system.h>
#include "gecko.h"
#endif

namespace Riivo
{
	static bool IsPowerOfTwo(u32 v)
	{
		return v != 0 && (v & (v - 1)) == 0;
	}

	Mem2Reservation ReserveMem2(const Mem2Arena &arena, u32 size, u32 align)
	{
		Mem2Reservation r;

		if (size == 0)
		{
			r.why = "nothing to reserve";
			return r;
		}
		if (size > MAX_MEM2_RESERVE)
		{
			r.why = "reservation is implausibly large";
			return r;
		}
		if (!IsPowerOfTwo(align) || align < MEM2_RESERVE_ALIGN)
		{
			r.why = "alignment must be a power of two of at least a cache line";
			return r;
		}

		//! The arena has to look like an arena before anything is taken from
		//! it. A zero here means low memory was not what we expected, and
		//! lowering a word we do not understand is how a game gets overwritten.
		if (arena.hi <= MEM2_BASE || arena.hi > MEM2_TOP)
		{
			r.why = "MEM2 arena high is outside MEM2";
			return r;
		}
		if (arena.lo < MEM2_BASE || arena.lo >= arena.hi)
		{
			r.why = "MEM2 arena low is outside MEM2 or above arena high";
			return r;
		}
		if ((arena.hi & (MEM2_RESERVE_ALIGN - 1)) != 0)
		{
			r.why = "MEM2 arena high is not cache-line aligned";
			return r;
		}

		//! Round the request up, then take it off the top. Rounding the size
		//! rather than the resulting address keeps the reservation a whole
		//! number of cache lines, which is what the flush needs.
		u32 want = (size + align - 1) & ~(align - 1);
		if (want < size)
		{
			r.why = "reservation size overflows when aligned";
			return r;
		}

		u32 avail = arena.hi - arena.lo;
		if (want >= avail)
		{
			r.why = "reservation does not fit in the game's MEM2 heap";
			return r;
		}

		u32 newHi = arena.hi - want;
		newHi &= ~(align - 1);
		if (newHi <= arena.lo)
		{
			r.why = "reservation would leave the game no MEM2 heap";
			return r;
		}

		u32 left = newHi - arena.lo;
		if (left < MIN_GAME_MEM2)
		{
			r.why = "reservation would leave the game under 8 MB of MEM2";
			return r;
		}

		r.ok = true;
		r.addr = newHi;
		r.newArenaHi = newHi;
		r.reserved = arena.hi - newHi;
		r.heapLeft = left;
		return r;
	}

#ifdef GEKKO

	Mem2Arena ReadMem2Arena()
	{
		Mem2Arena a;
		a.lo = *(vu32 *) MEM2_ARENA_LO_ADDR;
		a.hi = *(vu32 *) MEM2_ARENA_HI_ADDR;
		return a;
	}

	bool CommitMem2Reservation(const Mem2Reservation &r)
	{
		if (!r.ok)
			return false;

		//! ReserveMem2 has already proved this, but the write goes into the
		//! block the game boots from, so it is checked again here rather than
		//! trusted from a struct that could have been built any number of ways.
		if (r.newArenaHi <= MEM2_BASE || r.newArenaHi >= MEM2_TOP)
			return false;

		*(vu32 *) MEM2_ARENA_HI_ADDR = r.newArenaHi;
		DCFlushRange((void *) MEM2_ARENA_LO_ADDR, 0x20);

		gprintf("Riivo: MEM2 reserved %u bytes at %08x, arena2Hi -> %08x, "
				"%u bytes left to the game\n",
				(unsigned) r.reserved, r.addr, r.newArenaHi,
				(unsigned) r.heapLeft);
		return true;
	}

#else

	//! Host build: the tests exercise ReserveMem2, which is where the
	//! arithmetic worth checking lives. Reading and writing low memory is
	//! target-only by nature.
	Mem2Arena ReadMem2Arena() { return Mem2Arena(); }

	bool CommitMem2Reservation(const Mem2Reservation &) { return false; }

#endif
}
