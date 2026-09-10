/****************************************************************************
 * v6 allocation-measurement window (Dolphin PPC, production allocator).
 *
 * Runs the validation window's REAL operations (FstBuilder, FstWalk,
 * PlaceFst/InstallFst, Crc32 - same TUs production links) under the
 * REAL production allocator (memory/mem2.cpp with -wrap, MEM2_init(48),
 * big-ones-to-MEM2, exactly like the loader boot) with scripted heap
 * histories standing in for the boot's retained structures. Records
 * per-op allocation sizes (vector capacities), MEM1/MEM2 free figures
 * and the first throwing op at each consumption level into a
 * GDB-readable mailbox. No disc, no cIOS, no game - this measures the
 * allocator behavior of the window, not the game outcome.
 *
 * The T0-scale case doubles as the compaction-path regression: real
 * compacted bytes, in-place PlaceFst, InstallFst, probe consumption.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <gccore.h>

#include "memory/mem2.h"
#include "RiivoFstInstall.hpp"
#include "RiivoFstBuild.hpp"
#include "RiivoFst.hpp"
#include "RiivoFstWalk.hpp"
#include "RiivoReconcile.hpp"

extern "C" void gprintf(const char *str, ...);

static int mwFailed = 0;

void MwCheck(bool cond, const char *what)
{
	printf("  [mw %s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++mwFailed;
}

void MwW32(u32 addr, u32 v)
{
	*(vu32 *) addr = v;
}

//! GDB mailbox. [0]=phase, [1]=mwFailed, then per-case slots of
//! 8 words: plain, compact, paths, failOp|crc, inPlace|consumed,
//! mem1delta, mem2pre, mem2post.
static const u32 MWMAIL = 0x80000300;

u32 MwMem1Free(void)
{
	struct mallinfo mi = mallinfo();
	return (u32) mi.fordblks;
}

void MwSlot(u32 slot, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f,
				 u32 g, u32 h)
{
	u32 base = MWMAIL + 4 + slot * 32;
	MwW32(base, a);
	MwW32(base + 4, b);
	MwW32(base + 8, c);
	MwW32(base + 12, d);
	MwW32(base + 16, e);
	MwW32(base + 20, f);
	MwW32(base + 24, g);
	MwW32(base + 28, h);
	DCFlushRange((void *) base, 32);
}

void MwPhase(u32 p)
{
	MwW32(MWMAIL, p);
	DCFlushRange((void *) MWMAIL, 4);
}

//! Last-run results for harness-side checks (GDB re-reads the mailbox).
static u32 mwLastFail = 0, mwLastPlain = 0, mwLastCompact = 0;
static u32 mwLastInPlace = 0, mwLastConsumed = 0;

//! Build a synthetic base FST image: `nfiles` files named f%05d.arc,
//! then parse it. Sizes mirror the real layout scale (entries 12 B,
//! names ~24 B) so per-op allocation sizes match the T0/Spectral shape.
static bool MwBuildBase(std::vector<u8> &img, int nfiles)
{
	char name[32];
	std::vector<u8> entries;
	entries.resize(12);
	std::string strings;
	strings += '\0';
	for (int i = 0; i < nfiles; ++i)
	{
		snprintf(name, sizeof(name), "file%05d.arc", i);
		const u32 no = (u32) strings.size();
		strings += name;
		strings += '\0';
		const u32 here = (u32) (entries.size() / 12);
		entries.resize(entries.size() + 12);
		u8 *e = &entries[here * 12];
		e[0] = 0;
		e[1] = no >> 16; e[2] = no >> 8; e[3] = no;
		const u32 off = (0x100000u + (u32) i * 0x8000u) >> 2;
		e[4] = off >> 24; e[5] = off >> 16; e[6] = off >> 8; e[7] = off;
		const u32 ln = 0x4000;
		e[8] = ln >> 24; e[9] = ln >> 16; e[10] = ln >> 8; e[11] = ln;
	}
	u8 *r = &entries[0];
	r[0] = 1;
	r[1] = r[2] = r[3] = 0;
	const u32 total = (u32) (entries.size() / 12);
	r[4] = r[5] = r[6] = r[7] = 0;
	r[8] = total >> 24; r[9] = total >> 16; r[10] = total >> 8; r[11] = total;
	img.clear();
	img.insert(img.end(), entries.begin(), entries.end());
	img.insert(img.end(), strings.begin(), strings.end());
	return true;
}

//! One mirror-window run. `nBase` disc files, `nMod` applied redirects.
//! `hogMode`: 0 none, 1 retain 64KB x `hogParam` blocks, 2 consume until
//! MEM2 free < `hogParam` bytes (failure-boundary mapping). `hogKept`
//! records what was actually retained, so a level that could not be
//! reached reads honestly.
static void MwRun(u32 slot, int nBase, int nMod, int hogMode, u32 hogParam,
				  bool doInstall, u32 expInPlace)
{
	char tag[48];
	snprintf(tag, sizeof(tag), "slot%u N=%d K=%d hog=%d:%u", slot, nBase,
			 nMod, hogMode, (unsigned) hogParam);
	printf("mw run %s install=%d\n", tag, (int) doInstall);

	std::vector<void *> hog;
	u32 hogKept = 0;
	if (hogMode == 1)
	{
		for (u32 i = 0; i < hogParam; ++i)
		{
			void *p = malloc(65536);
			if (!p)
				break;
			memset(p, 0x5A, 65536);
			hog.push_back(p);
			hogKept += 65536;
		}
	}
	else if (hogMode == 2)
	{
		while (MEM2_freesize() > hogParam)
		{
			void *p = malloc(65536);
			if (!p)
				break;
			memset(p, 0x5A, 65536);
			hog.push_back(p);
			hogKept += 65536;
			if (hogKept > 64 * 1024 * 1024)
				break;
		}
	}
	const u32 f1pre = MwMem1Free(), f2pre = MEM2_freesize();

	std::vector<u8> img;
	MwBuildBase(img, nBase);
	Riivo::FstBuilder b;
	u32 failOp = 0, plain = 0, compact = 0, paths = 0, crc = 0;
	u32 inPlace = 0, consumed = 0;
	char line[160];
	std::string out("mw report\n");
	bool parsed = false;
	try
	{
		parsed = b.Parse(&img[0], (u32) img.size(), true);
	}
	catch (...)
	{
		parsed = false;
	}
	if (!parsed)
	{
		failOp = 90; // base-image parse failed (not an alloc question)
	}
	else
	try
	{
		char dp[64], nm[32];
		for (int i = 0; i < nMod; ++i)
		{
			snprintf(nm, sizeof(nm), "modfile%05d.arc", i);
			snprintf(dp, sizeof(dp), "/gxdiagmany/%s", nm);
			bool w = false;
			b.AddOrReplace(dp, 64 + (u32) i, &w);
		}
		b.Layout(0x100000000ULL, 32768);
		std::vector<u8> newFst;
		b.Serialize(newFst, true);
		plain = (u32) newFst.size();

		// Mirror of production's guarded window (same order, same ops).
		std::vector<Riivo::FstWalkExpectation> expectedFst;
		try
		{
			expectedFst.reserve(6000);
		}
		catch (...)
		{
			failOp = 1;
			throw;
		}
		try
		{
			out.reserve(out.size() + 12288);
		}
		catch (...)
		{
			failOp = 6;
			throw;
		}
		for (int i = 0; i < nMod; ++i)
		{
			snprintf(nm, sizeof(nm), "modfile%05d.arc", i);
			snprintf(dp, sizeof(dp), "/gxdiagmany/%s", nm);
			for (char *c = dp; *c; ++c)
				if (*c >= 'A' && *c <= 'Z')
					*c += 32;
			u64 off = 0;
			u32 ln = 0;
			if (b.FindAssigned(dp, &off, &ln))
				expectedFst.push_back(
					Riivo::FstWalkExpectation(dp, off, 64 + (u32) i));
		}
		paths = (u32) expectedFst.size();
		Riivo::FstWalk gameWalk;
		std::string walkError;
		bool walkOK = false;
		try
		{
			walkOK = !newFst.empty() &&
				gameWalk.Open(&newFst[0], newFst.size(), true, &walkError) &&
				gameWalk.Check(expectedFst, &walkError);
		}
		catch (...)
		{
			failOp = 2;
			throw;
		}
		snprintf(line, sizeof(line), "walk %d paths %u\n", (int) walkOK,
				 paths);
		out += line;
		std::vector<u8> compactFst;
		bool compactOK = false;
		try
		{
			compactOK = b.SerializeCompacted(compactFst, true);
		}
		catch (...)
		{
			failOp = 3;
			throw;
		}
		Riivo::FstWalk cw;
		try
		{
			if (compactOK)
				compactOK = !compactFst.empty() &&
					cw.Open(&compactFst[0], compactFst.size(), true,
							&walkError) &&
					cw.Check(expectedFst, &walkError);
		}
		catch (...)
		{
			failOp = 4;
			throw;
		}
		compact = compactOK ? (u32) compactFst.size() : 0;
		snprintf(line, sizeof(line), "compact %d bytes %u cap %u\n",
				 (int) compactOK, compact,
				 compactOK ? (u32) compactFst.capacity() : 0);
		out += line;
		std::vector<u8> *useFst = &newFst;
		if (compactOK && compact < plain)
			useFst = &compactFst;
		std::vector<u8> stage(useFst->size());
		try
		{
			memcpy(&stage[0], &(*useFst)[0], useFst->size());
		}
		catch (...)
		{
			failOp = 5;
			throw;
		}
		crc = Riivo::Crc32(&stage[0], (u32) stage.size());
		if (doInstall)
		{
			// Captured SB4E01 arena words, as production reads them.
			Riivo::ArenaInfo arena;
			arena.arenaLo = 0;
			arena.arenaHi = 0x817DA740;
			arena.fstAddr = 0x817DA740;
			arena.fstMaxSize = 153792;
			const Riivo::OccupiedRange occ[2] = {
				Riivo::OccupiedRange(0x817D8740, 0x817DA740),
				Riivo::OccupiedRange(0x80728680, 0x807E3188),
			};
			const Riivo::FstPlacement place = Riivo::PlaceFst(
				arena, (u32) stage.size(), 32, occ, 2);
			inPlace = (place.ok && place.inPlace) ? 1 : 0;
			bool ok = false;
			try
			{
				ok = Riivo::InstallFst(place, &stage[0],
									   (u32) stage.size());
			}
			catch (...)
			{
				failOp = 7;
				throw;
			}
			consumed = ok ? 1 : 0;
			(void) expInPlace;
		}
		printf("%s", out.c_str());
	}
	catch (const std::bad_alloc &)
	{
		if (!failOp)
			failOp = 99;
		printf("mw %s: caught bad_alloc at op %u\n", tag, failOp);
	}
	catch (const std::exception &)
	{
		if (!failOp)
			failOp = 98;
		printf("mw %s: caught exception at op %u\n", tag, failOp);
	}
	const u32 f1post = MwMem1Free(), f2post = MEM2_freesize();
	MwSlot(slot, plain, compact, paths, (failOp << 24) | (crc & 0xFFFFFF),
		   (inPlace << 16) | consumed, (f1pre - f1post) & 0xFFFFFF,
		   f2pre, hogKept);
	mwLastFail = failOp;
	mwLastPlain = plain;
	mwLastCompact = compact;
	mwLastInPlace = inPlace;
	mwLastConsumed = consumed;
	// Hog freed by the caller pattern: release here so later slots
	// start from the scripted state, not accumulated garbage.
	for (size_t i = 0; i < hog.size(); ++i)
		free(hog[i]);
	(void) f2pre;
	(void) f2post;
}

//! v7 deep mirror driver (source/memdeep_run.cpp).
void RunDeepWindow(void);

void RunMemWindow(void)
{
	printf("================ v6 memwindow (production allocator) ================\n");
	MEM2_init(48);
	printf("  MEM2 free %u KB, MEM1 free %u KB\n",
		   MEM2_freesize() / 1024, MwMem1Free() / 1024);
	MwPhase(10);

	// Slot 0: T0-scale regression (4500-file base, 2 applied):
	// compaction path must verify exactly as production stages it.
	MwRun(0, 4500, 2, 0, 0, true, 1);
	MwCheck(mwLastFail == 0, "T0-scale window: no throwing op");
	MwCheck(mwLastPlain > 0 && mwLastCompact > 0, "T0-scale sizes recorded");
	MwCheck(mwLastInPlace == 1, "T0-scale placement is in-place");
	MwCheck(mwLastConsumed == 1, "T0-scale install consumed");
	MwPhase(20);

	// Slots 1-4: v7 deep full-prep-path mirror (supersedes the flat
	// ladder above, whose transcription noise is discarded).
	RunDeepWindow(); // phases 30-99
}
