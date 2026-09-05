/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Deciding which MEM2 addresses belong to the d2x DI module - pure logic,
 * no console dependency, so the host tests cover it.
 *
 * Background: the MEM2 scan used to match the loader's own .rodata copy of
 * the dispatch pattern and patch our own image. The dispatch is only searched
 * for inside a module window identified by a ceiling-constant pair plus a
 * nearby hook marker - an address shape our own image shares, which is why
 * loader-owned addresses are excluded first.
 *
 * What counts as loader-owned: everything at or below the MEM2 arena top the
 * console reports. The old ±1 MB pattern-address window stays only as a
 * fallback for when that call fails; the pattern lives in MEM1, outside the
 * scanned range, so on a healthy console it excludes nothing.
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

	//! Fragment-code and stock-DI literals for the read-path anchors.
	static const u32 PROBE_MAXFRAG   = 0x00004E20;
	static const u32 PROBE_STOCK_DVD9 = 0x7ED40000;
	static const u32 PROBE_STOCK_DVD5 = 0x460A0000;

	//! A hook marker counts only this close before the pair.
	static const u32 PROBE_THUNK_WINDOW = 512;

	//! Two MAX_FRAG hits anchor fragment code only this close together.
	static const u32 PROBE_FRAG_SPAN = 0x1000; // 4 KB

	//! A stock read-limit pair anchors only this close together.
	static const u32 PROBE_STOCK_SPAN = 64;

	//! Matches within this distance of our own pattern copy are ours.
	static const u32 PROBE_SELF_EXCL = 0x100000; // +-1 MB

	//! No round writes more dump windows than this; the rest are counted.
	static const u32 PROBE_MAX_DUMPS = 6;

	//! Dump (and dispatch-search) window around a module pair address.
	static const u32 PROBE_DUMP_BEFORE = 0x8000;  // 32 KB
	static const u32 PROBE_DUMP_AFTER  = 0x18000; // 96 KB

	//! One ceiling-pair cluster found in the scan.
	struct DiModule
	{
		u32 pairAddr;  // address of the lower word of the pair
		u32 thunkAddr; // nearest hook marker strictly before the pair, 0 if none
		bool ours;     // loader-owned: decided by IsOurs below
	};

	//! What anchored a dump window.
	enum DiAnchorKind
	{
		ANCHOR_D2X = 0,  // d2x ceiling pair + nearby hook marker
		ANCHOR_FRAG = 1, // two MAX_FRAG hits within 4 KB
		ANCHOR_STOCK = 2 // stock DVD9 + DVD5 within 64 bytes
	};

	//! One anchor for a dump window: the address the window is taken around
	//! and what found it. Ours anchors are kept for the log and never dumped.
	struct DiAnchor
	{
		u32 addr;          // anchor address: the lower word taken
		DiAnchorKind kind;
		bool ours;         // loader-owned: decided by IsOurs below
	};

	//! One window to dump, carrying the lowest contributor's anchor for the
	//! log label after merging.
	struct DumpWindow
	{
		u32 base;
		u32 size;
		u32 anchor;
		DiAnchorKind kind;
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

	//! True when an address belongs to the loader rather than IOS. The arena
	//! top decides whenever it is usable (above the scan range start); only
	//! when the call failed does the pattern window decide alone. A hit
	//! exactly at the arena top is still the loader's.
	inline bool IsOurs(u32 addr, u32 arenaHi, u32 selfAddr, u32 rangeLo)
	{
		if (arenaHi > rangeLo)
			return addr <= arenaHi;
		if (!selfAddr)
			return false;
		u32 wlo, whi;
		LoaderWindow(selfAddr, &wlo, &whi);
		return (u64) addr >= wlo && (u64) addr <= whi;
	}

	//! Find every adjacent DVD5/DVD9 ceiling pair among the DVD5 hits and
	//! classify each: ours by IsOurs, a module candidate when a hook marker
	//! sits within PROBE_THUNK_WINDOW bytes before it.
	//! Neighbour reads stay inside [rangeLo, rangeHi). Output is in ascending
	//! address order, like the scan that produced the hits.
	inline void ClassifyModules(const std::vector<u32> &dvd5Hits,
								const std::vector<u32> &thunkHits,
								u32 arenaHi, u32 selfAddr,
								u32 rangeLo, u32 rangeHi,
								ProbeReadWordFn read, void *ctx,
								std::vector<DiModule> &out)
	{
		out.clear();
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
			m.ours = IsOurs(pair, arenaHi, selfAddr, rangeLo);
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

	//! Fragment-code anchors: two MAX_FRAG hits within PROBE_FRAG_SPAN of
	//! each other. Hits pair up greedily, lowest first, each hit used once;
	//! the anchor is the lower address. Ours anchors are still emitted (for
	//! the log) but must never be dumped.
	inline void ClassifyFragAnchors(const std::vector<u32> &maxfragHits,
									u32 arenaHi, u32 selfAddr, u32 rangeLo,
									std::vector<DiAnchor> &out)
	{
		out.clear();
		size_t i = 0;
		while (i < maxfragHits.size())
		{
			if (i + 1 < maxfragHits.size()
				&& maxfragHits[i + 1] >= maxfragHits[i]
				&& maxfragHits[i + 1] - maxfragHits[i] <= PROBE_FRAG_SPAN)
			{
				DiAnchor a;
				a.addr = maxfragHits[i];
				a.kind = ANCHOR_FRAG;
				a.ours = IsOurs(a.addr, arenaHi, selfAddr, rangeLo);
				out.push_back(a);
				i += 2;
			}
			else
				++i;
		}
	}

	//! Stock-DI anchors: a stock DVD9 hit with a stock DVD5 hit within
	//! PROBE_STOCK_SPAN bytes either way. Each hit is used once, lowest pair
	//! first; the anchor is the lower address.
	inline void ClassifyStockAnchors(const std::vector<u32> &stock9Hits,
									 const std::vector<u32> &stock5Hits,
									 u32 arenaHi, u32 selfAddr, u32 rangeLo,
									 std::vector<DiAnchor> &out)
	{
		out.clear();
		std::vector<char> used(stock5Hits.size(), 0);
		for (size_t i = 0; i < stock9Hits.size(); ++i)
		{
			const u32 a = stock9Hits[i];
			for (size_t j = 0; j < stock5Hits.size(); ++j)
			{
				if (used[j])
					continue;
				const u32 b = stock5Hits[j];
				if (b > a && b - a > PROBE_STOCK_SPAN)
					break; // ascending: later hits only farther
				const bool match = (b >= a) ? (b - a <= PROBE_STOCK_SPAN)
											: (a - b <= PROBE_STOCK_SPAN);
				if (!match)
					continue;
				DiAnchor an;
				an.addr = a < b ? a : b;
				an.kind = ANCHOR_STOCK;
				an.ours = IsOurs(an.addr, arenaHi, selfAddr, rangeLo);
				out.push_back(an);
				used[j] = 1;
				break;
			}
		}
	}

	//! Merge overlapping or touching windows into ascending order, keeping
	//! the lowest contributor's anchor for the log label. Caps the total at
	//! PROBE_MAX_DUMPS and counts the rest, so nearby anchors never write
	//! the same bytes twice.
	inline void MergeDumpWindows(const std::vector<DumpWindow> &in,
								 std::vector<DumpWindow> &out, u32 *outSkipped)
	{
		out.clear();
		*outSkipped = 0;
		std::vector<DumpWindow> sorted = in;
		for (size_t i = 1; i < sorted.size(); ++i)
		{
			DumpWindow t = sorted[i];
			size_t j = i;
			while (j > 0 && sorted[j - 1].base > t.base)
			{
				sorted[j] = sorted[j - 1];
				--j;
			}
			sorted[j] = t;
		}
		for (size_t i = 0; i < sorted.size(); ++i)
		{
			const u64 end = (u64) sorted[i].base + sorted[i].size;
			if (!out.empty())
			{
				DumpWindow &last = out[out.size() - 1];
				const u64 lastEnd = (u64) last.base + last.size;
				if ((u64) sorted[i].base <= lastEnd)
				{
					if (end > lastEnd)
						last.size = (u32) (end - last.base);
					continue;
				}
			}
			out.push_back(sorted[i]);
		}
		while (out.size() > PROBE_MAX_DUMPS)
		{
			out.pop_back();
			++(*outSkipped);
		}
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
							  u32 arenaHi, u32 selfAddr, u32 rangeLo)
	{
		u32 anchor = 0;
		for (size_t i = 0; i < mods.size(); ++i)
			if (!mods[i].ours && mods[i].pairAddr > anchor)
				anchor = mods[i].pairAddr;
		if (anchor)
			return anchor;
		for (size_t i = 0; i < ceilingHits.size(); ++i)
		{
			const u32 a = ceilingHits[i];
			if (IsOurs(a, arenaHi, selfAddr, rangeLo))
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
