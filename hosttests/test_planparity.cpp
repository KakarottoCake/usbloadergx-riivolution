// Early/late placement identity through the production seam.
//
// The fragment list must be registered in SetupDisc before the game
// partition (and therefore the FST) can be opened; the rebuilt table is
// made to agree afterwards. This pins that both phases derive identical
// placements and source mappings from shared inputs - one authoritative
// layout, not two descriptions that happen to agree:
//
//   early: ListModFiles (REAL FsDirLister + REAL stat over a scratch card)
//          -> AssignModOffsets (the production cursor walk, shared code)
//   late:  BuildPatchPlan (same set/device/lister/sizes, FST added)
//          -> ResolveLateOffsets (pre-FST earlyKey meets early offsets)
//          -> FstBuilder::LayoutFrom -> FindAssigned
//          -> BuildPlanManifest (RIV1, self-validated)
//
// Covered: absolute files, bare basenames, relative folders, dataless
// folders (+DOL child routing + DOL exclusion early), duplicates (last
// wins both sides), zero-length cursor sharing, missing externals,
// created files, DOL composition served outside fragments, manifest
// round-trip, and the divergence tripwires (dropped offset, tampered
// placed entry). Card enumeration runs for real (POSIX scratch dir;
// PPC never compiles host tests); the FST and the sizes stand in for
// disc/card state with identical values on both sides.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <stdlib.h>

#include "riivo/RiivoTypes.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoConfig.hpp"
#include "riivo/RiivoPatchPlan.hpp"
#include "riivo/RiivoFragPlan.hpp"
#include "riivo/RiivoReconcile.hpp"
#include "riivo/RiivoManifest.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void Wr32(std::vector<u8> &v, size_t at, u32 x)
{
	v[at] = (u8)(x >> 24);
	v[at + 1] = (u8)(x >> 16);
	v[at + 2] = (u8)(x >> 8);
	v[at + 3] = (u8)x;
}

struct SEnt { u8 type; std::string name; u32 a, b; };

// root(12) x[2,6): Abs.BIN dup.bin gone.bin base.arc#2; dir[7,9):
// base.arc other.arc; deep[10,12): nest[11,12): c.bin. Mixed case.
// Duplicate basename base.arc: FST-order first hit is /x/base.arc.
static void BuildDisc(std::vector<u8> &out)
{
	std::vector<SEnt> e;
	e.push_back((SEnt){1, "", 0, 12});
	e.push_back((SEnt){1, "x", 0, 6});
	e.push_back((SEnt){0, "Abs.BIN", 0x20000 >> 2, 1000});
	e.push_back((SEnt){0, "dup.bin", 0x30000 >> 2, 10});
	e.push_back((SEnt){0, "gone.bin", 0x31000 >> 2, 300});
	e.push_back((SEnt){0, "base.arc", 0x32000 >> 2, 1500});
	e.push_back((SEnt){1, "dir", 0, 9});
	e.push_back((SEnt){0, "base.arc", 0x40000 >> 2, 2000});
	e.push_back((SEnt){0, "other.arc", 0x50000 >> 2, 500});
	e.push_back((SEnt){1, "deep", 0, 12});
	e.push_back((SEnt){1, "nest", 8, 12});
	e.push_back((SEnt){0, "c.bin", 0x60000 >> 2, 100});
	const u32 n = (u32)e.size();
	out.assign(n * 12, 0);
	std::string strings;
	for (u32 i = 0; i < n; ++i)
	{
		u32 nameOff = (u32)strings.size();
		strings += e[i].name;
		strings += '\0';
		out[i * 12] = e[i].type;
		out[i * 12 + 1] = (u8)(nameOff >> 16);
		out[i * 12 + 2] = (u8)(nameOff >> 8);
		out[i * 12 + 3] = (u8)nameOff;
		Wr32(out, i * 12 + 4, e[i].a);
		Wr32(out, i * 12 + 8, e[i].b);
	}
	out.insert(out.end(), strings.begin(), strings.end());
}

static ResolvedFile RF(const char *disc, const char *ext,
					   u32 off = 0, u32 foff = 0, u32 len = 0,
					   bool resize = true, bool create = false)
{
	ResolvedFile r;
	r.root = "";
	r.disc = disc;
	r.external = ext;
	r.resize = resize;
	r.create = create;
	r.offset = off;
	r.fileoffset = foff;
	r.length = len;
	return r;
}

static void WriteFile(const std::string &path, u32 size, u8 fill)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	for (u32 i = 0; i < size; ++i)
		fputc(fill, f);
	fclose(f);
}

static u32 Rd32le(const std::vector<u8> &v, size_t at)
{
	return (u32)v[at] | ((u32)v[at + 1] << 8) | ((u32)v[at + 2] << 16) |
		   ((u32)v[at + 3] << 24);
}

// Ascending-offset order, mirroring CollectPlaced (production sorts served
// placements; early keys and late keys order remapped files differently).
static bool ByOffset(const PlacedFile &a, const PlacedFile &b)
{
	return a.offset < b.offset;
}

struct MemSizes : public FileSizeProvider
{
	std::map<std::string, u32> m;
	virtual bool GetSize(const std::string &external, u32 *outSize)
	{
		std::map<std::string, u32>::iterator it = m.find(external);
		if (it == m.end())
			return false;
		if (outSize)
			*outSize = it->second;
		return true;
	}
};

int main()
{
	setvbuf(stdout, 0, _IONBF, 0); // crash diagnostics must not sit in a buffer
	// Shared path helpers: one implementation, asserted at the seam.
	CHECK(DiscDirPath("") == "/");
	CHECK(DiscDirPath("deep") == "/deep");
	CHECK(DiscDirPath("/deep/") == "/deep/");
	CHECK(JoinDiscPath("/", "b.arc") == "/b.arc");
	CHECK(JoinDiscPath("/deep", "nest/c.bin") == "/deep/nest/c.bin");
	CHECK(JoinDiscPath("/deep/", "c.bin") == "/deep/c.bin");
	CHECK(BaseFileName("a/b.arc") == "b.arc");
	CHECK(BaseFileName("main.dol") == "main.dol");
	CHECK(IsBootFileDisc("main.dol"));
	CHECK(IsBootFileDisc("MAIN.DOL"));
	CHECK(!IsBootFileDisc("/main.dol"));
	CHECK(!IsBootFileDisc("/x/abs.bin"));

	// Scratch card with real files (sizes mirror the MemSizes below).
	char tmp[] = "/tmp/riivo-parity-XXXXXX";
	if (!mkdtemp(tmp))
	{
		printf("SKIP: no temp dir\n");
		return 0;
	}
	std::string card = tmp;
	mkdir((card + "/sub").c_str(), 0755);
	mkdir((card + "/sub/nest").c_str(), 0755);
	mkdir((card + "/newer").c_str(), 0755);
	mkdir((card + "/newer/sub").c_str(), 0755);
	WriteFile(card + "/abs.bin", 100, 0xA1);
	WriteFile(card + "/base.bin", 200, 0xB2);
	WriteFile(card + "/dup1.bin", 10, 0xD1);
	WriteFile(card + "/dup2.bin", 20, 0xD2);
	WriteFile(card + "/empty.bin", 0, 0);
	WriteFile(card + "/dolpatch.bin", 30, 0xE4);
	WriteFile(card + "/sub/nest/c.bin", 60, 0xC3);
	WriteFile(card + "/newer/other.arc", 80, 0xF1);
	WriteFile(card + "/newer/brandnew.bin", 90, 0xF2);
	WriteFile(card + "/newer/main.dol", 0x1000, 0xF3);
	WriteFile(card + "/newer/ghost2.bin", 10, 0xF4);
	WriteFile(card + "/newer/sub/nested.bin", 40, 0xF5); // nested dataless: matches nothing, both phases

	std::vector<u8> discImg;
	BuildDisc(discImg);
	Fst fst;
	CHECK(fst.Parse(&discImg[0], (u32)discImg.size(), true));
	CHECK(fst.FileCount() == 7);

	ResolvedPatchSet set;
	set.files.push_back(RF("/x/abs.bin", "abs.bin"));
	set.files.push_back(RF("base.arc", "base.bin")); // bare basename route
	set.files.push_back(RF("/x/dup.bin", "dup1.bin"));
	set.files.push_back(RF("/x/dup.bin", "dup2.bin")); // duplicate: last wins
	set.files.push_back(RF("/z/empty.bin", "empty.bin", 0, 0, 0, true, true));
	set.files.push_back(RF("main.dol", "dolpatch.bin", 0x100, 4, 8, true));
	set.files.push_back(RF("/x/gone.bin", "gone-missing.bin")); // absent card file
	{
		ResolvedFolder g;
		g.root = "";
		g.disc = "deep";
		g.external = "sub";
		g.resize = true;
		g.create = false;
		g.recursive = true;
		g.length = 0;
		set.folders.push_back(g);
	}
	{
		ResolvedFolder g;
		g.root = "";
		g.disc = "";
		g.external = "newer";
		g.resize = true;
		g.create = true;
		g.recursive = true;
		g.length = 0;
		set.folders.push_back(g);
	}

	ClearDirListCache();
	ClearFileSizeCache();

	// ---- early: real enumeration + shared cursor walk ----
	FsDirLister fsLister;
	std::vector<ModCandidate> cand;
	std::vector<MissingExternal> earlyMissing;
	u32 dolRouted = 0;
	ListModFiles(set, card, &fsLister, cand, 0, 0, &earlyMissing, &dolRouted);
	// 8 candidates: abs, base, dup(last only), empty, deep/nest/c.bin,
	// other, brandnew, ghost2 (gone-missing absent; 2x DOL routed out).
	CHECK(cand.size() == 8);
	CHECK(dolRouted == 2); // main.dol file rule + newer/main.dol child
	bool sawDup2 = false, sawDup1 = false, sawDol = false;
	for (size_t i = 0; i < cand.size(); ++i)
	{
		if (cand[i].external.find("dup2.bin") != std::string::npos)
		{
			sawDup2 = true;
			CHECK(cand[i].disc == "/x/dup.bin");
			CHECK(cand[i].size == 20);
		}
		if (cand[i].external.find("dup1.bin") != std::string::npos)
			sawDup1 = true;
		if (cand[i].external.find("dolpatch.bin") != std::string::npos
			|| cand[i].external.find("main.dol") != std::string::npos)
			sawDol = true;
	}
	CHECK(sawDup2 && !sawDup1); // dedup keeps the last claim
	CHECK(!sawDol);             // executable bytes never fragment-mapped
	bool sawNested = false;
	for (size_t i = 0; i < cand.size(); ++i)
		if (cand[i].external.find("nested.bin") != std::string::npos)
			sawNested = true;
	CHECK(!sawNested); // nested dataless children match nothing early...
	bool sawMissing = false;
	for (size_t i = 0; i < earlyMissing.size(); ++i)
		if (earlyMissing[i].external.find("gone-missing.bin") != std::string::npos)
			sawMissing = true;
	CHECK(sawMissing);

	const u64 regionStart = PlanRegionStart(0x1000000ULL, 512);
	std::map<std::string, u64> earlyOffsets;
	std::vector<PlacedFile> earlyPlaced;
	std::vector<RegRecord> earlyRecords;
	u64 regionEnd = AssignModOffsets(cand, regionStart, 512, earlyOffsets,
									earlyPlaced, earlyRecords);
	CHECK(regionEnd > regionStart);
	CHECK(earlyOffsets.size() == 8);
	{
		bool foundEmpty = false;
		for (size_t i = 0; i < earlyRecords.size(); ++i)
		{
			if (earlyRecords[i].external.find("empty.bin") != std::string::npos)
			{
				CHECK(earlyRecords[i].length == 0);
				foundEmpty = true;
			}
		}
		CHECK(foundEmpty);
	}

	// ---- late: same set/device/lister, FST added ----
	// (MemSizes mirrors the real card sizes byte-for-byte.)
	MemSizes sizes;
	sizes.m[card + "/abs.bin"] = 100;
	sizes.m[card + "/base.bin"] = 200;
	sizes.m[card + "/dup1.bin"] = 10;
	sizes.m[card + "/dup2.bin"] = 20;
	sizes.m[card + "/empty.bin"] = 0;
	sizes.m[card + "/dolpatch.bin"] = 30;
	sizes.m[card + "/sub/nest/c.bin"] = 60;
	sizes.m[card + "/newer/other.arc"] = 80;
	sizes.m[card + "/newer/brandnew.bin"] = 90;
	sizes.m[card + "/newer/main.dol"] = 0x1000;
	sizes.m[card + "/newer/ghost2.bin"] = 10;
	PatchPlan plan;
	PlannedFile dolPlan;
	std::string why;
	CHECK(BuildPatchPlan(fst, set, card, &fsLister, &sizes, plan, why,
						 0x1000, &dolPlan));
	CHECK(why.empty());
	CHECK(plan.errors.empty());
	CHECK(!plan.hasPartial); // all whole-file (DOL partial lives outside)
	CHECK(plan.hasBootFile);
	CHECK(dolPlan.bootFile && dolPlan.finalSize == 0x1000);
	// 8 files: abs, base, dup, empty, deep/nest/c.bin, other, brandnew, ghost2
	CHECK(plan.files.size() == 8);
	{
		bool sawDeep = false, sawFirstHit = false, sawNestedLate = false;
		for (size_t i = 0; i < plan.files.size(); ++i)
		{
			if (plan.files[i].disc == "/deep/nest/c.bin")
				sawDeep = true;
			// Duplicate basename: first FST-order hit wins (/x/base.arc,
			// ahead of /dir/base.arc), reference parity.
			if (plan.files[i].earlyKey == "/base.arc")
				CHECK(plan.files[i].disc == "/x/base.arc");
			if (plan.files[i].disc == "/dir/base.arc")
				sawFirstHit = true; // must stay false: /x wins, not /dir
			if (plan.files[i].disc.find("nested") != std::string::npos)
				sawNestedLate = true;
		}
		CHECK(sawDeep); // hierarchical FST descent, not flat ranges
		CHECK(!sawFirstHit);
		CHECK(!sawNestedLate); // ...nor late: reference parity
	}
	// Directory cache replayed the early pass (same inputs, same answer).
	{
		u32 hits = 0, misses = 0;
		DirCacheStats(&hits, &misses);
		CHECK(misses > 0 && hits > 0);
	}

	// ---- identity: every plan file meets its early offset at equal size ----
	std::map<std::string, u64> layMap;
	u32 unplaced = 0, remapped = 0;
	ResolveLateOffsets(plan, earlyOffsets, layMap, unplaced, remapped);
	CHECK(unplaced == 0);
	CHECK(remapped == 2); // bare base.arc + dataless other.arc
	CHECK(layMap.size() == plan.files.size());
	{
		std::map<std::string, u32> recSize;
		for (size_t i = 0; i < earlyRecords.size(); ++i)
			recSize[earlyRecords[i].disc] = earlyRecords[i].length;
		for (size_t i = 0; i < plan.files.size(); ++i)
		{
			const PlannedFile &f = plan.files[i];
			std::map<std::string, u64>::iterator lo = layMap.find(f.disc);
			CHECK(lo != layMap.end());
			if (lo == layMap.end())
				continue;
			CHECK(lo->second == earlyOffsets[f.earlyKey]);
			std::map<std::string, u32>::iterator rs = recSize.find(f.earlyKey);
			CHECK(rs != recSize.end());
			if (rs != recSize.end())
				CHECK(rs->second == f.finalSize); // stat/compose agreement
		}
	}

	// ---- table build off the resolved map places identically ----
	{
		FstBuilder builder;
		CHECK(builder.Parse(&discImg[0], (u32)discImg.size(), true));
		bool isNew = false;
		for (size_t i = 0; i < plan.files.size(); ++i)
			CHECK(builder.AddOrReplace(plan.files[i].disc, plan.files[i].finalSize, &isNew));
		CHECK(builder.LayoutFrom(layMap) == 0);
		for (size_t i = 0; i < plan.files.size(); ++i)
		{
			u64 off = 0;
			u32 len = 0;
			CHECK(builder.FindAssigned(plan.files[i].disc, &off, &len));
			CHECK(off == layMap[plan.files[i].disc]);
			CHECK(len == plan.files[i].finalSize);
		}
	}

	// ---- manifest round-trip off served placements ----
	// Production externals carry device prefixes ("sd:"/"usbN:"), which the
	// manifest classifier requires; the scratch card has none. Rewrite both
	// sides through one mechanical bijection (agreement preserved) so this
	// exercises the shared BuildPlanManifest, not a fork of it.
	{
	// Index-coupled aliasing shared with the tripwire block below.
		std::vector<PlacedFile> placed;
		PatchPlan devPlan = plan;
		for (size_t i = 0; i < devPlan.files.size(); ++i)
		{
			if (devPlan.files[i].finalSize == 0)
				continue; // zero-length: entry, no fragments (CollectPlaced)
			char indexed[96];
			snprintf(indexed, sizeof(indexed), "sd:/parity/%u.bin", (unsigned)i);
			devPlan.files[i].external = indexed;
			for (size_t s = 0; s < devPlan.files[i].segs.size(); ++s)
				if (devPlan.files[i].segs[s].kind == PlanSegment::SEG_EXTERNAL)
					devPlan.files[i].segs[s].external = indexed;
			PlacedFile p;
			p.offset = layMap[plan.files[i].disc];
			p.length = plan.files[i].finalSize;
			p.external = indexed;
			placed.push_back(p);
		}
		std::stable_sort(placed.begin(), placed.end(), ByOffset);
		CHECK(placed.size() + 1 == plan.files.size()); // minus empty.bin
		std::vector<u8> blob;
		bool mok = BuildPlanManifest(devPlan, placed, 0x4244554D, blob, why);
		if (!mok)
			printf("  manifest why: %s\n", why.c_str());
		CHECK(mok);
		CHECK(!blob.empty());
		if (!blob.empty())
		{
			CHECK(ValidateManifestV1(&blob[0], (u32)blob.size(), why));
			CHECK(Rd32le(blob, 20) == (u32)placed.size()); // count word
		}
	}

	// ---- tripwires: dropped offset, tampered placed entry ----
	{
		std::map<std::string, u64> dropped = earlyOffsets;
		dropped.erase(dropped.begin());
		std::map<std::string, u64> late2;
		u32 un2 = 0, re2 = 0;
		ResolveLateOffsets(plan, dropped, late2, un2, re2);
		CHECK(un2 == 1);
	}
	{
		// Index-coupled aliasing: placed[k] <-> plan.files[srcIdx[k]] share
		// one device-prefixed external, so the tamper below drifts exactly
		// one pair and the rest still agree.
		std::vector<PlacedFile> placed;
		std::vector<size_t> srcIdx;
		PatchPlan devPlan = plan;
		for (size_t i = 0; i < devPlan.files.size(); ++i)
		{
			if (devPlan.files[i].finalSize == 0)
				continue;
			char indexed[96];
			snprintf(indexed, sizeof(indexed), "sd:/parity/%u.bin", (unsigned)i);
			devPlan.files[i].external = indexed;
			for (size_t s = 0; s < devPlan.files[i].segs.size(); ++s)
				if (devPlan.files[i].segs[s].kind == PlanSegment::SEG_EXTERNAL)
					devPlan.files[i].segs[s].external = indexed;
			PlacedFile p;
			p.offset = layMap[plan.files[i].disc];
			p.length = plan.files[i].finalSize;
			p.external = indexed;
			placed.push_back(p);
			srcIdx.push_back(i);
		}
		std::stable_sort(placed.begin(), placed.end(), ByOffset);
		CHECK(placed.size() == srcIdx.size());
		placed[0].external = "sd:/parity/elsewhere.bin"; // drifted source
		std::vector<u8> blob;
		CHECK(!BuildPlanManifest(devPlan, placed, 0, blob, why));
		CHECK(!why.empty());
	}

	// Cleanup scratch card (best effort; under /tmp either way).
	unlink((card + "/abs.bin").c_str());
	unlink((card + "/base.bin").c_str());
	unlink((card + "/dup1.bin").c_str());
	unlink((card + "/dup2.bin").c_str());
	unlink((card + "/empty.bin").c_str());
	unlink((card + "/dolpatch.bin").c_str());
	unlink((card + "/sub/nest/c.bin").c_str());
	unlink((card + "/newer/other.arc").c_str());
	unlink((card + "/newer/brandnew.bin").c_str());
	unlink((card + "/newer/main.dol").c_str());
	unlink((card + "/newer/ghost2.bin").c_str());
	unlink((card + "/newer/sub/nested.bin").c_str());
	rmdir((card + "/sub/nest").c_str());
	rmdir((card + "/sub").c_str());
	rmdir((card + "/newer/sub").c_str());
	rmdir((card + "/newer").c_str());
	rmdir(card.c_str());

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
