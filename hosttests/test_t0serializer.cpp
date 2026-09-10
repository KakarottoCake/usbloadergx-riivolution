// T0 serializer driver: the REAL FstBuilder over the EXACT GXDiag T0
// workload (gxdiag.xml `gx_t0`: two created files, sizes from the pack).
//
// Two modes:
//   selftest (default, CI-safe): synthetic throwaway base. Asserts the
//     workload model itself: added==2, addedDirs==1, replaced==0, and a
//     size delta of exactly +144 bytes (3 entries x12 + dir name 12 +
//     two 47-char names +NUL). The delta is base-independent: entry and
//     string bytes only.
//   base mode (T0_BASE_FST=/path/to/153792-byte SB4E01 FST): parses the
//     real base, applies the same redirects, and asserts the rebuilt
//     table is EXACTLY 153934 bytes - the T0 card-log value. Writes
//     $OUT/t0-rebuilt.fst plus CRC32. Each buffer is hashed under its
//     own label and pinned: plain 153934 = 13401a49, compacted 144323 =
//     0409fd62. Section 3 drives the same workload through the REAL
//     ValidateTable under mod-region placement (so its bytes differ
//     from §1/§2) and requires the staged buffer to equal that tree's
//     own re-compacted bytes - content identity, not just size.
//     (The base FST bytes were the missing input, AES-locked in the ISO
//     without the console key; see BYPASSES.md / HANDOFF.md.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFstWalk.hpp"
#include "riivo/RiivoReconcile.hpp"
#include "riivo/RiivoValidate.hpp"

using namespace Riivo;

static int failures = 0;
static int checks = 0;

static void ck(bool cond, const char *what)
{
	++checks;
	if (!cond) { printf("  FAIL: %s\n", what); ++failures; }
}

//! Exact T0 workload from GXDiag-SB4E01 gxdiag.xml (patch gx_t0).
static const char *kT0A =
	"/GXDiagProbe/a0123456789abcdef0123456789abcdef0123456789.bin";
static const char *kT0B =
	"/GXDiagProbe/b0123456789abcdef0123456789abcdef0123456789.bin";
static const u32 kT0ASize = 1;   // T0/a.bin
static const u32 kT0BSize = 558; // T0/b.bin

//! Minimal synthetic base: root + dir D + files f.bin, g.bin.
static void BuildBase(std::vector<u8> &out)
{
	struct E { u8 t; const char *n; u32 a, b; };
	const E e[] = {
		{1, "",      0, 4},
		{1, "D",     0, 4},
		{0, "f.bin", 0x1000 >> 2, 0x800},
		{0, "top.bin", 0x4000 >> 2, 0x40},
	};
	const u32 n = 4;
	out.assign(n * 12, 0);
	std::string strings;
	for (u32 i = 0; i < n; ++i) {
		u32 no = (u32) strings.size();
		strings += e[i].n; strings += '\0';
		out[i*12] = e[i].t;
		out[i*12+1] = no >> 16; out[i*12+2] = no >> 8; out[i*12+3] = no;
		u32 a = e[i].a, b = e[i].b;
		out[i*12+4] = a >> 24; out[i*12+5] = a >> 16;
		out[i*12+6] = a >> 8; out[i*12+7] = a;
		out[i*12+8] = b >> 24; out[i*12+9] = b >> 16;
		out[i*12+10] = b >> 8; out[i*12+11] = b;
	}
	out.insert(out.end(), strings.begin(), strings.end());
}

static bool ApplyT0(FstBuilder &b)
{
	bool w1 = false, w2 = false;
	if (!b.AddOrReplace(kT0A, kT0ASize, &w1)) return false;
	if (!b.AddOrReplace(kT0B, kT0BSize, &w2)) return false;
	return w1 && w2;
}

int main()
{
	const char *basePath = getenv("T0_BASE_FST");
	std::vector<u8> base;
	if (basePath && basePath[0]) {
		printf("1. real base: %s\n", basePath);
		FILE *f = fopen(basePath, "rb");
		if (!f) { printf("  FAIL: cannot open base\n"); return 1; }
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		base.resize((size_t) len);
		if (len <= 0 || fread(&base[0], 1, (size_t) len, f) != (size_t) len) {
			printf("  FAIL: cannot read base\n");
			fclose(f);
			return 1;
		}
		fclose(f);
		printf("  base bytes: %ld\n", len);
	} else {
		printf("1. selftest base (synthetic, delta-only)\n");
		BuildBase(base);
	}

	FstBuilder b;
	ck(b.Parse(&base[0], (u32) base.size(), true), "base parses");
	ck(ApplyT0(b), "T0 redirects apply, both new");
	const FstBuildStats &pre = b.Stats();
	ck(pre.added == 2, "added==2");
	ck(pre.addedDirs == 1, "addedDirs==1");
	ck(pre.replaced == 0, "replaced==0");

	u64 region = (b.OriginalExtent() + 32767) & ~(u64) 32767;
	if (region < 0x1000000) region = 0x1000000;
	b.Layout(region, 32768);
	std::vector<u8> out;
	b.Serialize(out, true);
	const FstBuildStats &st = b.Stats();
	printf("  orig %u -> rebuilt %u (entries %u)\n",
		   (unsigned) base.size(), st.fstSize, st.entryCount);

	if (basePath && basePath[0]) {
		ck(st.fstSize == 153934, "rebuilt is exactly 153934 (T0 log value)");
		const u32 crc = Crc32(&out[0], (u32) out.size());
		printf("  plain rebuilt CRC32: %08x\n", crc);
		ck(crc == 0x13401a49,
		   "plain buffer is the 13401a49 table (153934 bytes)");
		const char *od = getenv("OUT");
		std::string dst = od ? od : "/tmp";
		dst += "/t0-rebuilt.fst";
		FILE *f = fopen(dst.c_str(), "wb");
		ck(f != 0, "rebuilt table written to disk");
		if (f) {
			ck(fwrite(&out[0], 1, out.size(), f) == out.size(),
			   "rebuilt table fully written");
			fclose(f);
			printf("  wrote %s\n", dst.c_str());
		} else {
			printf("  FAIL: cannot open %s (create OUT dir first)\n",
				   dst.c_str());
		}
	} else {
		ck(st.fstSize == (u32) base.size() + 144, "delta is exactly +144");
	}

	Fst c;
	ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
	const FstFile *fa = c.FindFile("/gxdiagprobe/a0123456789abcdef0123456789abcdef0123456789.bin");
	const FstFile *fb = c.FindFile("/gxdiagprobe/b0123456789abcdef0123456789abcdef0123456789.bin");
	ck(fa && fa->length == kT0ASize, "a.bin entry, real size");
	ck(fb && fb->length == kT0BSize, "b.bin entry, real size");

	printf("2. suffix-compacted serialization of the same tree\n");
	{
		FstBuilder b2;
		ck(b2.Parse(&base[0], (u32) base.size(), true), "base re-parses");
		ck(ApplyT0(b2), "T0 redirects re-apply");
		u64 region2 = (b2.OriginalExtent() + 32767) & ~(u64) 32767;
		if (region2 < 0x1000000) region2 = 0x1000000;
		b2.Layout(region2, 32768);
		std::vector<u8> plain, compact;
		b2.Serialize(plain, true);
		clock_t t0 = clock();
		bool okc = b2.SerializeCompacted(compact, true);
		clock_t t1 = clock();
		ck(okc, "compacted serializes");
		printf("  plain %u -> compacted %u (saved %d, %.2fs)\n",
			   (unsigned) plain.size(), (unsigned) compact.size(),
			   (int) plain.size() - (int) compact.size(),
			   (double) (t1 - t0) / CLOCKS_PER_SEC);
		ck(compact.size() <= plain.size(), "never bigger than plain");
		Fst p, q;
		ck(p.Parse(&plain[0], (u32) plain.size(), true), "plain parses");
		ck(q.Parse(&compact[0], (u32) compact.size(), true), "compacted parses");
		ck(p.FileCount() == q.FileCount(), "same entry count");
		bool same = (p.FileCount() == q.FileCount());
		for (u32 i = 0; same && i < p.FileCount(); ++i)
		{
			const FstFile &a = p.FileAt(i);
			const FstFile &b = q.FileAt(i);
			same = (a.path == b.path && a.offset == b.offset &&
					a.length == b.length);
		}
		ck(same, "every path, offset and length identical");
		if (basePath && basePath[0]) {
			ck(compact.size() <= 153792,
			   "compacted T0 fits the original reservation");
			const char *od = getenv("OUT");
			std::string dst = od ? od : "/tmp";
			dst += "/t0-rebuilt-compact.fst";
			FILE *f = fopen(dst.c_str(), "wb");
			ck(f != 0, "compacted table written to disk");
			if (f) {
				ck(fwrite(&compact[0], 1, compact.size(), f) == compact.size(),
				   "compacted table fully written");
				fclose(f);
				const u32 ccrc = Crc32(&compact[0], (u32) compact.size());
				printf("  wrote %s compacted CRC32 %08x\n", dst.c_str(),
					   ccrc);
				ck(ccrc == 0x0409fd62,
				   "compacted buffer is the 0409fd62 table (144323 bytes)");
			}
		}
	}

	printf("3. production ValidateTable over the same workload\n");
	{
		// Rebuild the T0 tree exactly as section 1 did, then drive the
		// REAL production window (not a mirror of it).
		FstBuilder b;
		ck(b.Parse(&base[0], (u32) base.size(), true), "base re-parses");
		ck(ApplyT0(b), "T0 redirects re-apply");
		u64 regionStart = 0x0180000000ULL;
		const u32 align = 32768;
		u64 cursor = regionStart;
		const u64 mask = (u64) align - 1;
		std::map<std::string, u64> modOffsets;
		std::map<std::string, u32> modSizes;
		std::vector<RedirectSpec> redirects;
		std::vector<CreatedFile> created;
		std::vector<RegRecord> records;
		const char *paths[2] = { kT0A, kT0B };
		u32 sizes[2] = { kT0ASize, kT0BSize };
		char lower[160];
		for (int i = 0; i < 2; ++i)
		{
			snprintf(lower, sizeof(lower), "%s", paths[i]);
			for (char *c = lower; *c; ++c)
				if (*c >= 'A' && *c <= 'Z')
					*c += 32;
			cursor = (cursor + mask) & ~mask;
			modOffsets[lower] = cursor;
			cursor += (sizes[i] + mask) & ~mask;
			modSizes[lower] = sizes[i];
			CreatedFile c;
			c.disc = lower;
			c.external = "usb1:/riivolution/gxdiag/T0/x.bin";
			created.push_back(c);
			RegRecord r;
			r.disc = lower;
			r.external = c.external;
			r.offset = modOffsets[lower];
			r.length = sizes[i];
			records.push_back(r);
		}
		ck(b.LayoutFrom(modOffsets) == 0, "early placement clean");
		std::vector<u8> plain;
		b.Serialize(plain, true);
		std::map<std::string, SkipReason> addFails;
		ValidateRequest vreq;
		vreq.builder = &b;
		{
			// The disc baseline for expectations: parse the same base.
			static Fst discFst;
			ck(discFst.Parse(&base[0], (u32) base.size(), true),
			   "disc baseline parses");
			vreq.fst = &discFst;
		}
		vreq.plainFst = &plain;
		vreq.modOffsets = &modOffsets;
		vreq.expectedModSizes = &modSizes;
		vreq.fstReserve = (basePath && basePath[0]) ? 153792 : 0;
		vreq.region = regionStart;
		vreq.modRegionStart = regionStart;
		vreq.redirects = &redirects;
		vreq.created = &created;
		vreq.modRecords = &records;
		vreq.modAddFails = &addFails;
		vreq.imageBytes = 4685037568ULL;
		vreq.sectorSize = 512;
		vreq.usedFrags = 3;
		ValidateResult vres;
		int trace = -1;
		ValidateTable(vreq, vres, &trace);
		ck(!vres.oom, "no OOM flag");
		ck(vres.fstWalkOK, "production walk passes");
		ck(vres.plan.ok, "production region plan passes");
		ck(vres.placed.size() == 2, "both files placed");
		ck(vres.modSkips.empty(), "no skips");
		ck(trace == VOP_NONE, "trace runs to completion");
		if (basePath && basePath[0])
		{
			ck(vres.useCompact, "production stages compacted");
			ck(vres.staged.size() == 144323,
			   "production staged bytes are the 144323-byte table");
			ck(vres.stats.fstSize == 144323, "stats match staged bytes");
			// Content identity, not just size: re-serialize THIS
			// tree (§3 uses LayoutFrom mod-region placement, so its
			// bytes legitimately differ from §1/§2's contiguous
			// layout: plain 9390660a vs 13401a49) and require the
			// staged buffer to BE that compacted table, byte for byte.
			std::vector<u8> compact3;
			ck(b.SerializeCompacted(compact3, true),
			   "section-3 tree re-compacts");
			const u32 plainCrc =
				Crc32(&plain[0], (u32) plain.size());
			const u32 stagedCrc =
				Crc32(&vres.staged[0], (u32) vres.staged.size());
			const u32 compact3Crc =
				Crc32(&compact3[0], (u32) compact3.size());
			printf("  §3 plain CRC32: %08x, staged CRC32: %08x, "
				   "re-compacted CRC32: %08x\n",
				   plainCrc, stagedCrc, compact3Crc);
			ck(compact3.size() == vres.staged.size(),
			   "staged size matches this tree's compacted size");
			ck(compact3Crc == stagedCrc &&
			   memcmp(&compact3[0], &vres.staged[0],
					  vres.staged.size()) == 0,
			   "staged buffer IS this tree's compacted table");
		}
		else
		{
			ck(!vres.useCompact, "unknown reservation keeps plain");
			ck(vres.staged.empty(), "no extra staged copy on plain path");
		}
	}

	printf("t0serializer: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
