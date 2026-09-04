// Scale test: rebuild an FST the size of the user's real Super Mario Galaxy 2
// disc (3935 files) with a Spectral-sized mod applied (267 replaced, 1881 added).
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"

using namespace Riivo;
static int failures = 0, checks = 0;
static void ck(bool c, const char *w){++checks; if(!c){printf("  FAIL: %s\n", w); ++failures;}}

// Build a realistic FST: 40 directories x ~98 files, offsets packed like a real
// disc (4-byte aligned, ascending, ending near 4 GB as the real one does).
static void BuildBig(std::vector<u8> &out, int nDirs, int perDir)
{
	struct E { u8 t; std::string n; u32 a, b; };
	std::vector<E> e;
	int total = 1 + nDirs * (1 + perDir);
	e.push_back((E){1, "", 0, (u32) total});

	u64 off = 0x00800000ULL;
	for (int d = 0; d < nDirs; ++d) {
		char dn[32]; snprintf(dn, sizeof(dn), "Dir%02d", d);
		int myIdx = (int) e.size();
		e.push_back((E){1, dn, 0, (u32)(myIdx + 1 + perDir)});
		for (int f = 0; f < perDir; ++f) {
			char fn[48]; snprintf(fn, sizeof(fn), "file_%02d_%03d.arc", d, f);
			u32 len = 0x400 + ((d * 37 + f * 13) % 0x20000);
			e.push_back((E){0, fn, (u32)(off >> 2), len});
			off += (len + 3) & ~3ULL;   // 4-byte packed, like the real disc
		}
	}

	u32 n = (u32) e.size();
	out.assign(n * 12, 0);
	std::string s;
	for (u32 i = 0; i < n; ++i) {
		u32 no = (u32) s.size(); s += e[i].n; s += '\0';
		out[i*12] = e[i].t;
		out[i*12+1] = no >> 16; out[i*12+2] = no >> 8; out[i*12+3] = no;
		out[i*12+4] = e[i].a >> 24; out[i*12+5] = e[i].a >> 16; out[i*12+6] = e[i].a >> 8; out[i*12+7] = e[i].a;
		out[i*12+8] = e[i].b >> 24; out[i*12+9] = e[i].b >> 16; out[i*12+10] = e[i].b >> 8; out[i*12+11] = e[i].b;
	}
	out.insert(out.end(), s.begin(), s.end());
}

int main()
{
	const int nDirs = 40, perDir = 98;   // 3920 files, close to the real 3935
	std::vector<u8> fst;
	BuildBig(fst, nDirs, perDir);
	printf("original FST: %u bytes, %d files\n", (unsigned) fst.size(), nDirs * perDir);

	clock_t t0 = clock();
	FstBuilder b;
	ck(b.Parse(&fst[0], (u32) fst.size(), true), "parse big");

	// 267 replacements spread across the disc, 64 of them larger (Spectral's shape)
	int replaced = 0, grown = 0;
	for (int d = 0; d < nDirs && replaced < 267; ++d) {
		for (int f = 0; f < perDir && replaced < 267; f += 14) {
			char p[80]; snprintf(p, sizeof(p), "/Dir%02d/file_%02d_%03d.arc", d, d, f);
			bool isNew = true;
			u32 sz = (replaced % 4 == 0) ? 0x900000 : 0x800;  // ~1/4 grow a lot
			if (replaced % 4 == 0) ++grown;
			ck(b.AddOrReplace(p, sz, &isNew) && !isNew, "replace existing");
			++replaced;
		}
	}
	// 1881 brand-new files in new directories
	for (int i = 0; i < 1881; ++i) {
		char p[96];
		snprintf(p, sizeof(p), "/StageData/New%02d/added_%04d.arc", i % 24, i);
		bool isNew = false;
		ck(b.AddOrReplace(p, 0x1000 + i, &isNew) && isNew, "add new");
	}

	ck(b.Stats().replaced == 267, "267 replaced");
	ck(b.Stats().added == 1881, "1881 added");
	printf("dirs created: %u\n", b.Stats().addedDirs);

	u64 extent = b.OriginalExtent();
	printf("original extent: 0x%llx (%.2f GB)\n",
		   (unsigned long long) extent, extent / 1073741824.0);

	// place the mod above the disc, sector aligned
	b.Layout((extent + 0x7ffff) & ~0x7ffffULL, 0x800);

	std::vector<u8> out;
	b.Serialize(out, true);
	double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

	printf("rebuilt FST: %u bytes, %u entries (was %u bytes)\n",
		   b.Stats().fstSize, b.Stats().entryCount, (unsigned) fst.size());
	printf("growth: +%d bytes\n", (int) b.Stats().fstSize - (int) fst.size());
	printf("highest assigned: 0x%llx (%.2f GB)\n",
		   (unsigned long long) b.Stats().highestOffset,
		   b.Stats().highestOffset / 1073741824.0);
	printf("rebuild took %.3f s on host\n", secs);

	// It must still be a valid FST, and every file must be findable.
	Fst c;
	ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
	printf("files in rebuilt table: %u\n", (unsigned) c.FileCount());
	ck(c.FileCount() == (size_t)(nDirs * perDir + 1881), "file count = original + added");

	ck(c.FindFile("/dir00/file_00_000.arc") != 0, "replaced file present");
	ck(c.FindFile("/stagedata/new00/added_0000.arc") != 0, "added file present");
	ck(c.FindFile("/dir39/file_39_097.arc") != 0, "last original present");

	// No two assigned ranges may overlap, and all must be aligned.
	std::vector<std::pair<u64,u64> > ranges;
	for (size_t i = 0; i < c.FileCount(); ++i) {
		const FstFile &f = c.FileAt(i);
		if (f.offset < b.Stats().highestOffset && f.offset >= extent)
			ranges.push_back(std::make_pair(f.offset, (u64) f.length));
	}
	printf("relocated ranges: %u\n", (unsigned) ranges.size());
	bool aligned = true, disjoint = true;
	for (size_t i = 0; i < ranges.size(); ++i) {
		if (ranges[i].first & 0x7ff) aligned = false;
		for (size_t j = i + 1; j < ranges.size(); ++j) {
			u64 a0 = ranges[i].first, a1 = a0 + ranges[i].second;
			u64 b0 = ranges[j].first, b1 = b0 + ranges[j].second;
			if (a0 < b1 && b0 < a1) { disjoint = false; break; }
		}
		if (!disjoint) break;
	}
	ck(aligned, "every relocated range is sector aligned");
	ck(disjoint, "no two relocated ranges overlap");
	ck(ranges.size() == 267u + 1881u, "every modded file was relocated");

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
