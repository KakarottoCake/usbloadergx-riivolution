/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Deciding which MEM2 addresses belong to the d2x DI module - pure logic,
 * no console dependency, so the host tests cover it.
 *
 * Background: the MEM2 scan used to match the loader's own .rodata copy of
 * the dispatch pattern and patch our own image. Two defences now combine:
 * the pattern has a single definition whose address is known at runtime,
 * and the dispatch is only searched for inside a module window identified
 * by a ceiling-constant pair plus a nearby hook marker - an address shape
 * our own image shares, which is why the loader window excludes first.
 ***************************************************************************/
#ifndef RIIVO_PROBE_CLASSIFY_HPP_
#define RIIVO_PROBE_CLASSIFY_HPP_

#include <gctypes.h>
#include <vector>

namespace Riivo
{
	//! d2x ceiling literals (word values) the module search keys on.
	static const u32 PROBE_DVD5  = 0x46090000;
	static const u32 PROBE_DVD9  = 0x7ED38000;
	static const u32 PROBE_THUNK = 0x4B004718;

	//! A hook marker counts only this close before the pair.
	static const u32 PROBE_THUNK_WINDOW = 512;

	//! Matches within this distance of our own pattern copy are ours.
	static const u32 PROBE_SELF_EXCL = 0x100000; // +-1 MB

	//! Dump (and dispatch-search) window around a module pair address.
	static const u32 PROBE_DUMP_BEFORE = 0x8000;  // 32 KB
	static const u32 PROBE_DUMP_AFTER  = 0x18000; // 96 KB

	//! One ceiling-pair cluster found in the scan.
	struct DiModule
	{
		u32 pairAddr;  // address of the lower word of the pair
		u32 thunkAddr; // nearest hook marker strictly before the pair, 0 if none
		bool ours;     // inside the loader's own exclusion window
	};

	//! Reads one word at a cached-view address. Target passes the uncached
	//! alias reader; the host tests pass a synthetic image reader.
	typedef u32 (*ProbeReadWordFn)(u32 addr, void *ctx);

	//! Loader exclusion window around our own pattern copy. Empty (lo > hi
	//! convention: lo = hi = 0) when selfAddr is 0, i.e. unknown.
	inline void LoaderWindow(u32 selfAddr, u32 *outLo, u32 *outHi)
	{
		if (!selfAddr)
		{
			*outLo = 0;
			*outHi = 0;
			return;
		}
		*outLo = selfAddr > PROBE_SELF_EXCL ? selfAddr - PROBE_SELF_EXCL : 0;
		*outHi = selfAddr + PROBE_SELF_EXCL;
	}

	//! Find every adjacent DVD5/DVD9 ceiling pair among the DVD5 hits and
	//! classify each: ours when inside the loader window, a module candidate
	//! when a hook marker sits within PROBE_THUNK_WINDOW bytes before it.
	//! Neighbour reads stay inside [rangeLo, rangeHi). Output is in ascending
	//! address order, like the scan that produced the hits.
	inline void ClassifyModules(const std::vector<u32> &dvd5Hits,
								const std::vector<u32> &thunkHits,
								u32 selfAddr,
								u32 rangeLo, u32 rangeHi,
								ProbeReadWordFn read, void *ctx,
								std::vector<DiModule> &out)
	{
		out.clear();
		u32 wlo, whi;
		LoaderWindow(selfAddr, &wlo, &whi);
		for (size_t i = 0; i < dvd5Hits.size(); ++i)
		{
			const u32 a = dvd5Hits[i];
			u32 pair = 0;
			if (a + 4 < rangeHi && read(a + 4, ctx) == PROBE_DVD9)
				pair = a;
			else if (a >= 4 && a - 4 >= rangeLo && read(a - 4, ctx) == PROBE_DVD9)
				pair = a - 4;
			if (!pair)
				continue;
			u32 thunk = 0;
			for (size_t k = 0; k < thunkHits.size(); ++k)
			{
				const u32 t = thunkHits[k];
				if (t < pair && pair - t <= PROBE_THUNK_WINDOW && t > thunk)
					thunk = t;
			}
			DiModule m;
			m.pairAddr = pair;
			m.thunkAddr = thunk;
			m.ours = (selfAddr != 0 && (u64) pair >= wlo && (u64) pair <= whi);
			out.push_back(m);
		}
	}

	//! Index of the highest-addressed surviving candidate (not ours, has a
	//! hook marker), or -1 when none survives. More than one survivor means
	//! the log must report all of them, not just this one.
	inline int BestModuleIndex(const std::vector<DiModule> &mods)
	{
		int best = -1;
		for (size_t i = 0; i < mods.size(); ++i)
		{
			if (!mods[i].ours && mods[i].thunkAddr != 0)
				best = (int) i;
		}
		return best;
	}

	//! Anchor for a fallback dump, used when no candidate survived. A round
	//! that comes home with no bytes at all is a wasted round, and the rules
	//! above - a hook marker within 512 bytes of the pair - are drawn from a
	//! single console. Prefer the highest-addressed pair outside the loader
	//! window; failing that, the highest bare ceiling hit outside it. Returns
	//! 0 when there is nothing worth dumping. The caller must never search a
	//! fallback window for a dispatch: it is evidence, not a patch target.
	inline u32 FallbackAnchor(const std::vector<DiModule> &mods,
							  const std::vector<u32> &ceilingHits,
							  u32 selfAddr)
	{
		u32 wlo, whi;
		LoaderWindow(selfAddr, &wlo, &whi);
		u32 anchor = 0;
		for (size_t i = 0; i < mods.size(); ++i)
			if (!mods[i].ours && mods[i].pairAddr > anchor)
				anchor = mods[i].pairAddr;
		if (anchor)
			return anchor;
		for (size_t i = 0; i < ceilingHits.size(); ++i)
		{
			const u32 a = ceilingHits[i];
			if (selfAddr && a >= wlo && a <= whi)
				continue;
			if (a > anchor)
				anchor = a;
		}
		return anchor;
	}

	//! Dump window around a module pair address, clamped to [rangeLo, rangeHi).
	inline void ModuleDumpWindow(u32 pairAddr, u32 rangeLo, u32 rangeHi,
								 u32 *outBase, u32 *outSize)
	{
		u32 base = pairAddr > rangeLo + PROBE_DUMP_BEFORE
				 ? pairAddr - PROBE_DUMP_BEFORE : rangeLo;
		u64 end = (u64) pairAddr + PROBE_DUMP_AFTER;
		if (end > rangeHi)
			end = rangeHi;
		*outBase = base;
		*outSize = end > base ? (u32) (end - base) : 0;
	}
}

#endif
