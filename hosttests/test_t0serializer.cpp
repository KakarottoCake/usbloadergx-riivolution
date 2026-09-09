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
//     $OUT/t0-rebuilt.fst plus CRC32. This is the gate the missing input
//     (base FST bytes, AES-locked in the ISO without the console key)
//     unblocks; see BYPASSES.md / HANDOFF.md.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoReconcile.hpp"

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
		printf("  rebuilt CRC32: %08x\n", crc);
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
				printf("  wrote %s CRC32 %08x\n", dst.c_str(),
					   Crc32(&compact[0], (u32) compact.size()));
			}
		}
	}

	printf("t0serializer: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
