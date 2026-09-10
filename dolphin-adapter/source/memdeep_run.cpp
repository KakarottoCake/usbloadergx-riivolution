/****************************************************************************
 * v7 deep full-prep-path mirror driver.
 *
 * Slot plan (mailbox 0x80000300, 8 words/slot: plain, compact, paths,
 * preFail|failOp|crcLo, gate|unplaced, mem1delta, mem2pre, drainKept):
 *   0 T0-scale regression (flat mini tables, existing MwRun).
 *   1 deep clean. 2 deep MEM2-drained. 3 deep MEM1-drained.
 *   4 deep both-pressured (MEM2 drained + MEM1 to ~512 KB free).
 *
 * Mirror coverage, per op: Parse / AddOrReplace / LayoutFrom(REAL) /
 * Serialize / expectations(+reserve-in-try) / Walk(REAL) /
 * Compact+Walk(REAL) / CollectPlaced(MIRROR copy) / ToExtents(MIRROR
 * copy) / FindSkips(REAL) / PlanFragRegion(REAL) / stage+CRC / gate
 * eval. NOT mirrored: BuildRedirects (card; applied set IS the
 * redirect set), DescribeProbe/IOS (IOS), Activate (cIOS; v4 flows).
 * failOp codes, window: 1 expectedFst.reserve, 6 out.reserve, 2 walk,
 * 3 compact, 4 compact-walk, 5 stage, 10 CollectPlaced, 11 FindSkips /
 * PlanFragRegion. Pre-window: 90 parse, 91 add/replace, 92 LayoutFrom,
 * 93 serialize.
 ***************************************************************************/
#include <map>
#include <algorithm>
#include <string.h>
#include <gccore.h>

#include "memory/mem2.h"      // MEM2_alloc/freesize (REAL production TU)
#include "RiivoFstBuild.hpp"  // FstBuilder (REAL TU)
#include "RiivoFstWalk.hpp"   // FstWalk (REAL TU)
#include "RiivoFile.hpp"      // RedirectSpec, CreatedFile (types only)
#include "RiivoFragBuild.hpp" // PlacedFile (type only)
#include "RiivoFragPlan.hpp"  // ModExtent, FragPlan, PlanFragRegion (REAL TU)
#include "RiivoReconcile.hpp" // RegRecord/SkipRecord/FindSkips (REAL inline)

extern void MwW32(u32 addr, u32 v);
extern u32 MwMem1Free(void);
//! Same mailbox base as memwindow.cpp (kept as a literal: that TU owns
//! the layout comment; this 9th word is documented there too).
static const u32 MWMAIL_DEEP = 0x80000300;
extern void MwSlot(u32 slot, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f,
				   u32 g, u32 h);
extern void MwPhase(u32 p);
extern void MwCheck(bool cond, const char *what);
extern void MwDeepBuild(std::vector<u8> &img,
						std::vector<std::string> &baseFiles);

extern "C" void *__real_malloc(size_t);

// --- MIRROR of RiivoBoot.cpp's static CollectPlaced (line-for-line) ---
static bool MwByPlacedOffset(const Riivo::PlacedFile &a,
							 const Riivo::PlacedFile &b)
{
	return a.offset < b.offset;
}

static void MwCollectPlaced(const Riivo::FstBuilder &builder, u64 region,
							const std::vector<Riivo::RedirectSpec> &redirects,
							const std::vector<Riivo::CreatedFile> &created,
							std::vector<Riivo::PlacedFile> &out)
{
	out.clear();
	std::map<std::string, Riivo::PlacedFile> byDisc;
	for (size_t i = 0; i < redirects.size() + created.size(); ++i)
	{
		const bool isRedirect = i < redirects.size();
		const std::string &disc = isRedirect
								  ? redirects[i].disc
								  : created[i - redirects.size()].disc;
		const std::string &ext = isRedirect
								 ? redirects[i].external
								 : created[i - redirects.size()].external;
		u64 off = 0;
		u32 len = 0;
		if (!builder.FindAssigned(disc, &off, &len))
			continue;
		if (len == 0 || off < region)
			continue;
		Riivo::PlacedFile f;
		f.offset = off;
		f.length = len;
		f.external = ext;
		byDisc[disc] = f;
	}
	out.reserve(byDisc.size());
	for (std::map<std::string, Riivo::PlacedFile>::const_iterator it = byDisc.begin();
		 it != byDisc.end(); ++it)
		out.push_back(it->second);
	std::sort(out.begin(), out.end(), MwByPlacedOffset);
}

// --- MIRROR of RiivoBoot.cpp's static ToExtents (line-for-line) ---
static void MwToExtents(const std::vector<Riivo::PlacedFile> &in,
						std::vector<Riivo::ModExtent> &out)
{
	out.clear();
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i)
	{
		Riivo::ModExtent e;
		e.offset = in[i].offset;
		e.length = in[i].length;
		out.push_back(e);
	}
}

//! USA mod dirs with real per-dir file counts (measured local tree:
//! 99/12/78/78/75/846/1/658/1/10/58 = 1916 mapped).
struct MwModDir { const char *disc; int count; int depth2frac; };
static const MwModDir kModDirs[] = {
	{"/AudioRes", 99, 3}, {"/LayoutData", 12, 0},
	{"/LocalizeData/UsEnglish", 78, 2}, {"/LocalizeData/UsSpanish", 78, 2},
	{"/LocalizeData/UsFrench", 75, 2}, {"/ObjectData", 846, 3},
	{"/ParticleData", 1, 0}, {"/StageData", 658, 3}, {"/SystemData", 1, 0},
	{"/CustomCode", 10, 0}, {"/ScenarioData", 58, 2},
};
static const char *kModStems[] = {
	"IslandFleetGalaxy", "PeachCastleGalaxy", "TitleLogoScreen",
	"ProductMapObjTable", "SaveIconBannerImg", "FileSelectMapDat",
	"StageLightScenario", "ObjectTableArc", "LayoutScreenData",
	"ParticleEffectDat", "SystemConfigData", "ScenarioTableDat",
};
static const int kNModStems = 12;

//! Last-run results for harness-side checks.
static u32 mwDeepPre = 0, mwDeepFail = 0, mwDeepPlain = 0;
static u32 mwDeepPaths = 0, mwDeepGate = 0, mwDeepUnplaced = 0;

//! Last walk failure text, GDB-readable (nm walkErrBuf) if walkOK==0.
static char walkErrBuf[128];
//! Last frag-plan refusal text (nm planWhyBuf) if plan.ok==0.
static char planWhyBuf[128];

static void MwDeepRun(u32 slot, int heapCase)
{
	printf("mw deep slot%u heapCase=%d\n", slot, heapCase);
	MwPhase(slot * 10); // entered

	// Scripted history FIRST (retained through the run). Bounded and
	// guarded: heap-inspection calls themselves are suspects if this
	// ever stops advancing, so every stage posts its own sub-phase and
	// every loop has a hard iteration cap plus a no-progress break.
	std::vector<void *> drain;
	u32 drained = 0, drainNote = 0;
	try
	{
		drain.reserve(2048);
		if (heapCase == 1 || heapCase == 3)
		{
			u32 iters = 0;
			while (1)
			{
				void *p = MEM2_alloc(65536);
				if (!p || ++iters > 4096)
					break;
				memset(p, 0x5A, 65536);
				drain.push_back(p);
				drained += 65536;
				if (drained >= 96 * 1024 * 1024)
					break;
			}
			drainNote = iters;
		}
	if (heapCase == 2 || heapCase == 3)
	{
		const u32 floor = (heapCase == 3) ? 512 * 1024 : 0;
		u32 iters = 0, nulls = 0, lastFree = MwMem1Free();
		while (MwMem1Free() > floor)
		{
			void *p = __real_malloc(65536);
			if (!p)
			{
				if (++nulls >= 3)
					break;
				continue;
			}
			nulls = 0;
			memset(p, 0x5A, 65536);
			drain.push_back(p);
			drained += 65536;
			if (++iters > 4096)
				break;
			const u32 nowFree = MwMem1Free();
			if (nowFree >= lastFree)
			{
				drainNote = 0x80000000u | iters;
				break;
			}
			lastFree = nowFree;
		}
	}
	if (heapCase == 4)
	{
		// Both heaps to TRUE NULL: MEM2 first, then MEM1 with no floor.
		u32 iters = 0;
		while (1)
		{
			void *p = MEM2_alloc(65536);
			if (!p || ++iters > 4096)
				break;
			memset(p, 0x5A, 65536);
			drain.push_back(p);
			drained += 65536;
		}
		iters = 0;
		u32 nulls = 0;
		while (1)
		{
			void *p = __real_malloc(65536);
			if (!p)
			{
				if (++nulls >= 3)
					break;
				continue;
			}
			nulls = 0;
			memset(p, 0x5A, 65536);
			drain.push_back(p);
			drained += 65536;
			if (++iters > 4096)
				break;
		}
	}
	}
	catch (...)
	{
		drainNote = 0x40000000u; // drain itself threw
	}
	MwPhase(slot * 10 + 1); // drain done
	const u32 f1pre = MwMem1Free(), f2pre = MEM2_freesize();

	u32 preFail = 0, failOp = 0;
	u32 plain = 0, compact = 0, paths = 0, crc = 0, gate = 0, unplaced = 0;
	bool walkOK = false;
	char line[160];
	//! Default-constructed (no allocation; the empty rep is static), so
	//! this declaration is safe even with both heaps dead. Assigned
	//! inside the pre-window guard below.
	std::string out;
	std::vector<Riivo::FstWalkExpectation> expectedFst;
	Riivo::FragPlan plan;
	std::vector<Riivo::ModExtent> extents;
	std::vector<Riivo::PlacedFile> placed;
	std::vector<Riivo::RedirectSpec> redirects;
	std::vector<Riivo::CreatedFile> created;
	std::vector<u32> createdSizes; // parallel to created (exact apply sizes)
	std::map<std::string, u64> modOffsets;
	const u64 region = 0x0180000000ULL;

	// ---- PRE-WINDOW (unguarded mirror): tree-build through serialize.
	// Production persists "table serialised" after this; the hardware
	// log HAS that line, so this completed on hardware - a throw here
	// is a harness-scale artifact, recorded separately as preFail.
	Riivo::FstBuilder b;
	std::vector<u8> newFst;
	try
	{
		out = "mw deep report\n";
		std::vector<std::string> baseFiles;
		std::vector<u8> img;
		MwDeepBuild(img, baseFiles);
		if (!b.Parse(&img[0], (u32) img.size(), true))
		{
			preFail = 90;
			throw std::bad_alloc();
		}
		img.clear();
		img.shrink_to_fit();
		char nm[64], dp[160], ext[96];
		size_t repl = 0;
		u64 cursor = region;
		const u64 mask = (u64) 32768 - 1;
		try
		{
			for (size_t di = 0;
				 di < sizeof(kModDirs) / sizeof(kModDirs[0]); ++di)
			{
				for (int i = 0; i < kModDirs[di].count; ++i)
				{
					snprintf(nm, sizeof(nm), "%s%03d.arc",
							 kModStems[(i + (int) di) % kNModStems], i);
					std::string disc = std::string(kModDirs[di].disc) + "/";
					if (kModDirs[di].depth2frac &&
						(i % kModDirs[di].depth2frac == 0))
						disc += "SubDir/";
					u32 sz = 1024 + (u32) ((i * 137) % 524288);
					bool w = false;
					if (repl < 240 && !baseFiles.empty())
					{
						const std::string &bpath =
							baseFiles[repl % baseFiles.size()];
						if (b.AddOrReplace(bpath, sz, &w) && !w)
						{
							Riivo::RedirectSpec r;
							r.disc = bpath;
							r.external = "usb1:/Spectral/x.arc";
							r.length = sz;
							redirects.push_back(r);
							++repl;
							continue;
						}
					}
					std::string full = disc + nm;
					for (size_t k = 0; k < full.size(); ++k)
						if (full[k] >= 'A' && full[k] <= 'Z')
							full[k] += 32;
				if (b.AddOrReplace(full, sz, &w) && w)
				{
					Riivo::CreatedFile c;
					c.disc = full;
					snprintf(ext, sizeof(ext), "usb1:/Spectral/%s", nm);
					c.external = ext;
					created.push_back(c);
					createdSizes.push_back(sz);
				}
				}
			}
		}
		catch (...)
		{
			preFail = 91;
			throw;
		}
		// Pad to the log's 2148 applied paths (explicit approximation).
		int pad = 0;
		while ((int) (redirects.size() + created.size()) < 2148)
		{
			snprintf(dp, sizeof(dp),
					 "/systemdata/gxdiagpad/pad%04dppppppppppppppp.bin",
					 pad++);
			std::string full = dp;
			bool w = false;
			if (b.AddOrReplace(full, 64, &w) && w)
			{
				Riivo::CreatedFile c;
				c.disc = full;
				c.external = "usb1:/Spectral/pad.bin";
				created.push_back(c);
				createdSizes.push_back(64); // keep parallel: the cursor
											// loop below indexes by created
											// position (missing this once
											// read OOB garbage sizes).
			}
			else
				break;
		}
		for (size_t i = 0; i < redirects.size(); ++i)
		{
			cursor = (cursor + mask) & ~mask;
			modOffsets[redirects[i].disc] = cursor;
			cursor += ((u64) redirects[i].length + mask) & ~mask;
		}
		for (size_t i = 0; i < created.size(); ++i)
		{
			cursor = (cursor + mask) & ~mask;
			modOffsets[created[i].disc] = cursor;
			cursor += ((u64) createdSizes[i] + mask) & ~mask;
		}
		try
		{
			unplaced = b.LayoutFrom(modOffsets);
		}
		catch (...)
		{
			preFail = 92;
			throw;
		}
		try
		{
			b.Serialize(newFst, true);
		}
		catch (...)
		{
			preFail = 93;
			throw;
		}
		plain = (u32) newFst.size();
	}
	catch (...)
	{
		if (!preFail)
			preFail = 99;
	}

	// ---- GUARDED WINDOW (production-shaped guard): expectations
	// through gate inputs. failOp attributes the first throwing op.
	MwPhase(slot * 10 + 2); // pre-window done
	if (!preFail)
	try
	{
		try
		{
			expectedFst.reserve(6500);
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
		for (size_t i = 0; i < redirects.size(); ++i)
		{
			u64 off = 0;
			u32 ln = 0;
			if (b.FindAssigned(redirects[i].disc, &off, &ln))
				expectedFst.push_back(Riivo::FstWalkExpectation(
					redirects[i].disc, off, redirects[i].length));
		}
		for (size_t i = 0; i < created.size(); ++i)
		{
			u64 off = 0;
			u32 ln = 0;
			if (b.FindAssigned(created[i].disc, &off, &ln))
				expectedFst.push_back(Riivo::FstWalkExpectation(
					created[i].disc, off, ln));
		}
		paths = (u32) expectedFst.size();
		Riivo::FstWalk gameWalk;
		std::string walkError;
		walkOK = false;
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
		if (!walkOK)
		{
			strncpy(walkErrBuf, walkError.c_str(), sizeof(walkErrBuf) - 1);
			walkErrBuf[sizeof(walkErrBuf) - 1] = 0;
			DCFlushRange(walkErrBuf, sizeof(walkErrBuf));
		}
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
		try
		{
			MwCollectPlaced(b, region, redirects, created, placed);
		}
		catch (...)
		{
			failOp = 10;
			throw;
		}
		try
		{
			MwToExtents(placed, extents);
			std::vector<Riivo::RegRecord> records;
			records.reserve(redirects.size() + created.size());
			for (size_t i = 0; i < redirects.size(); ++i)
			{
				Riivo::RegRecord r;
				r.disc = redirects[i].disc;
				r.external = redirects[i].external;
				r.length = redirects[i].length;
				std::map<std::string, u64>::const_iterator it =
					modOffsets.find(r.disc);
				r.offset = (it == modOffsets.end()) ? 0 : it->second;
				records.push_back(r);
			}
			for (size_t i = 0; i < created.size(); ++i)
			{
				Riivo::RegRecord r;
				r.disc = created[i].disc;
				r.external = created[i].external;
				r.length = 64;
				std::map<std::string, u64>::const_iterator it =
					modOffsets.find(r.disc);
				r.offset = (it == modOffsets.end()) ? 0 : it->second;
				records.push_back(r);
			}
			std::vector<u64> lateOffsets;
			lateOffsets.reserve(placed.size());
			for (size_t i = 0; i < placed.size(); ++i)
				lateOffsets.push_back(placed[i].offset);
			std::map<std::string, char> hasRedirect;
			for (size_t i = 0; i < redirects.size(); ++i)
				hasRedirect[redirects[i].disc] = 1;
			for (size_t i = 0; i < created.size(); ++i)
				hasRedirect[created[i].disc] = 1;
			std::map<std::string, Riivo::SkipReason> addFails;
			std::vector<Riivo::SkipRecord> skips;
			Riivo::FindSkips(records, lateOffsets, region, hasRedirect,
							 addFails, skips);
			plan = Riivo::PlanFragRegion(4685037568ULL, 512, 3, extents);
		if (!plan.ok)
		{
			strncpy(planWhyBuf, plan.why.c_str(), sizeof(planWhyBuf) - 1);
			planWhyBuf[sizeof(planWhyBuf) - 1] = 0;
			DCFlushRange(planWhyBuf, sizeof(planWhyBuf));
		}
		}
		catch (...)
		{
			failOp = 11;
			throw;
		}
		std::vector<u8> stage(newFst.size());
		try
		{
			memcpy(&stage[0], &newFst[0], newFst.size());
		}
		catch (...)
		{
			failOp = 5;
			throw;
		}
		crc = Riivo::Crc32(&stage[0], (u32) stage.size());
		gate = (walkOK && plan.ok && unplaced == 0) ? 1 : 0;
		printf("%s", out.c_str());
	}
	catch (const std::bad_alloc &)
	{
		if (!failOp)
			failOp = 99;
		printf("mw deep slot%u: caught bad_alloc at op %u\n", slot, failOp);
	}
	catch (const std::exception &)
	{
		if (!failOp)
			failOp = 98;
		printf("mw deep slot%u: caught exception at op %u\n", slot, failOp);
	}

	const u32 f1post = MwMem1Free(), f2post = MEM2_freesize();
	MwSlot(slot, plain, compact, paths,
		   (preFail << 24) | (failOp << 16) | (crc & 0xFFFF),
		   (walkOK ? 0x01000000u : 0) | ((plan.ok ? 1u : 0) << 16) |
		   (unplaced & 0xFFFF),
		   (f1pre - f1post) & 0xFFFFFF, f2pre, drained);
	// Extra words clear of the 8-word stride: f1pre, drain note.
	MwW32(MWMAIL_DEEP + 0x200 + slot * 8, f1pre);
	MwW32(MWMAIL_DEEP + 0x200 + slot * 8 + 4, drainNote);
	DCFlushRange((void *) (MWMAIL_DEEP + 0x200 + slot * 8), 8);
	mwDeepPre = preFail;
	mwDeepFail = failOp;
	mwDeepPlain = plain;
	mwDeepPaths = paths;
	mwDeepGate = gate;
	mwDeepUnplaced = unplaced;
	for (size_t i = 0; i < drain.size(); ++i)
		free(drain[i]);
	(void) f2post;
}

void RunDeepWindow(void)
{
	MwDeepRun(1, 0);
	MwCheck(mwDeepPre == 0, "deep clean: pre-window completed");
	MwCheck(mwDeepFail == 0, "deep clean: no throwing op");
	MwCheck(mwDeepPaths == 2148, "deep clean: 2148 applied paths");
	MwCheck(mwDeepGate == 1 && mwDeepUnplaced == 0, "deep clean: gate green");
	MwPhase(30);
	MwDeepRun(2, 1);
	MwPhase(40);
	MwDeepRun(3, 2);
	MwPhase(50);
	MwDeepRun(4, 3);
	MwPhase(60);
	// Slot 5: both heaps to TRUE NULL (MEM2 drained, then MEM1 drained
	// to first NULL). Closest achievable to a genuine dual-exhaustion
	// stop: exercises the REAL refusal path (failOp + gate=0 recorded)
	// rather than the window passing on fallback.
	MwDeepRun(5, 4);
	MwPhase(99);
}
