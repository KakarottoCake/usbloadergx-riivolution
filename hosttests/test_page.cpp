// Paged RIV1 table access + serving through the pager backend.
// The pager (ios/riivo_page.c, real code) runs over a synthetic FAT16
// volume holding a table file sliced from a PRODUCTION RIV1 blob by the
// PRODUCTION pager encoder (BuildPagedFile): no second format exists to
// drift. Paged lookups equal resident scans; the real segment reader
// (sr_init_paged) serves complete reads across page/segment boundaries
// where covered, stops with GAP at unlisted disc (delegated whole,
// never zero-filled), and reports storage failures loudly. Plan-defined
// ZERO runs still read as zero. Exit 0 = pass.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "riivo/ios/riivo_fat.h"
#include "riivo/ios/riivo_page.h"
#include "riivo/ios/riivo_segread.h"
#include "riivo/RiivoManifest.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static const u32 kDiscId = 0x53424E41u;
static const u32 kEpoch = 7;
static const u64 kDecl = 0x100000ULL; // == first extent: anti-shadow passes

struct MemDisk
{
	std::vector<u8> img;
};

static int DiskRead(void *ctx, unsigned int lba, unsigned int count, void *buf)
{
	MemDisk *d = (MemDisk *)ctx;
	if ((u64)(lba + count) * 512 > d->img.size())
		return 0;
	memcpy(buf, &d->img[(size_t)lba * 512], (size_t)count * 512);
	return 1;
}

static void Put16(std::vector<u8> &v, size_t at, unsigned x)
{
	v[at] = (u8)(x & 0xFF);
	v[at + 1] = (u8)((x >> 8) & 0xFF);
}

static void Put32(std::vector<u8> &v, size_t at, unsigned long x)
{
	v[at] = (u8)(x & 0xFF);
	v[at + 1] = (u8)((x >> 8) & 0xFF);
	v[at + 2] = (u8)((x >> 16) & 0xFF);
	v[at + 3] = (u8)((x >> 24) & 0xFF);
}

struct ImgFile
{
	std::string name83; // 11-char "PG      BIN" form
	std::vector<u8> data;
};

// FAT16 image with the given files laid sequentially from cluster 2.
// 4096 data clusters -> FAT16 (only the files' own clusters are chained;
// chaining the whole area would run past the 1-sector FAT).
static void BuildImage(MemDisk &d, const std::vector<ImgFile> &files)
{
	const unsigned clusters = 4096;
	const unsigned fatSectors = 1;
	const unsigned total = 1 + 2 * fatSectors + 1 + clusters;
	d.img.assign(total * 512, 0);
	std::vector<u8> &m = d.img;
	Put16(m, 11, 512);
	m[13] = 1;
	Put16(m, 14, 1);
	m[16] = 2;
	Put16(m, 17, 16);
	Put16(m, 19, total);
	m[21] = 0xF8;
	Put16(m, 22, fatSectors);
	m[510] = 0x55;
	m[511] = 0xAA;
	const unsigned fat0 = 512;
	Put16(m, fat0 + 0, 0xFFF8);
	Put16(m, fat0 + 2, 0xFFFF);
	unsigned clus = 2;
	size_t dir = 1536;
	for (size_t f = 0; f < files.size(); ++f)
	{
		const std::vector<u8> &data = files[f].data;
		const unsigned need = (unsigned)((data.size() + 511) / 512);
		for (unsigned c = 0; c < need; ++c)
			Put16(m, fat0 + 4 + (clus - 2 + c) * 2,
				  (c + 1 < need) ? (clus + 1 + c) : 0xFFFF);
		memcpy(&m[dir], files[f].name83.c_str(), 11);
		m[dir + 11] = 0x20;
		Put16(m, dir + 26, clus);
		Put32(m, dir + 28, (unsigned long)data.size());
		dir += 32;
		// Data area starts at LBA 4: cluster 2 lives at byte 2048.
		memcpy(&m[(2 + clus) * 512], &data[0], data.size());
		clus += need;
	}
	memcpy(&m[1024], &m[512], 512);
}

static ManifestExtent Ext(u64 off, u32 len, u16 kind, u16 src,
						  u64 srcOff, const std::string &path, u32 gen)
{
	ManifestExtent e;
	e.discOffset = off;
	e.length = len;
	e.kind = kind;
	e.source = src;
	e.srcOffset = srcOff;
	e.path = path;
	e.genOff = gen;
	return e;
}

// Resident reference scan over the production extent vector.
static bool RefFind(const std::vector<ManifestExtent> &exts, u64 off,
					const ManifestExtent **out)
{
	for (size_t i = 0; i < exts.size(); ++i)
	{
		u64 e0 = exts[i].discOffset, e1 = e0 + exts[i].length;
		if (off >= e0 && (exts[i].length == 0 ? off == e0 : off < e1))
		{
			*out = &exts[i];
			return true;
		}
	}
	return false;
}

int main()
{
	// Card file with known content (served bytes reference).
	std::vector<u8> acard(4096);
	for (size_t i = 0; i < acard.size(); ++i)
		acard[i] = (u8)(0xC0 + (i & 0x3F));
	// Second card file, large enough to back a multi-sector extent that
	// abuts its neighbour: the cross-page serving span below is fully
	// mapped, so it proves assembly without touching delegation.
	std::vector<u8> bcard(0x8000);
	for (size_t i = 0; i < bcard.size(); ++i)
		bcard[i] = (u8)(0x40 + (i & 0x3F));
	// 300 extents in 3 pages (128+128+44), with gaps, ZERO runs, one
	// dangling path (EIO, never partial), shared-prefix paths, and two
	// extents abutting across the page 0/1 boundary (extent 127 ends
	// exactly where 128 begins: the fully mapped cross-page span).
	std::vector<ManifestExtent> exts;
	char pb[96];
	for (int i = 0; i < 300; ++i)
	{
		// 10 files of 0x8000 per group (span 0x50000), groups 0x100000
		// apart: dense but non-overlapping, 3 pages at 128/page.
		u64 off = 0x100000ULL + (u64)(i / 10) * 0x100000ULL + (u64)(i % 10) * 0x8000ULL;
		if (i == 0)
			exts.push_back(Ext(off, 0x1000, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   0, "/A.BIN", 0));
		else if (i == 127)
			exts.push_back(Ext(off, 0x8000, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   0, "/B.BIN", 0));
		else if (i == 128)
			exts.push_back(Ext(off, 0x800, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   0x800, "/A.BIN", 0));
		else if (i % 25 == 12)
			exts.push_back(Ext(off, 0x1000, RIIVO_EXT_ZERO, RIIVO_SRC_NONE, 0, "", 0));
		else if (i == 299)
			exts.push_back(Ext(off, 0x100, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   0, "/NOPE.BIN", 0));
		else
		{
			snprintf(pb, sizeof(pb), "/mod/dir%02d/file%04d.bin", i % 17, i);
			exts.push_back(Ext(off, 0x8000, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   (u64)(i * 64), pb, 0));
		}
	}
	std::vector<u8> blob;
	std::string why;
	CHECK(Riivo::BuildManifestV1(exts, Riivo::RIIVO_MANIFEST_DISCOVER, kDiscId, 0,
								 Riivo::RIIVO_CAP_SPLIT_READ | Riivo::RIIVO_CAP_ZERO_FILL,
								 Riivo::RIIVO_PROV_BOUNDED, blob, why));
	// Production pager encoder (not a test-local fork): the file the
	// module opens is byte-shaped by production code on both sides.
	std::vector<u8> pgfile;
	CHECK(Riivo::BuildPagedFile(blob, kEpoch, pgfile, why));
	ImgFile pgf;
	pgf.name83 = "PG      BIN";
	pgf.data = pgfile;
	ImgFile af;
	af.name83 = "A       BIN";
	af.data = acard;
	ImgFile bf;
	bf.name83 = "B       BIN";
	bf.data = bcard;
	std::vector<ImgFile> files;
	files.push_back(pgf);
	files.push_back(af);
	files.push_back(bf);
	MemDisk d;
	BuildImage(d, files);
	rfat_vol vol;
	unsigned int plba = 0xDEADu;
	CHECK(rfat_find_partition(DiskRead, &d, 0, &plba) == RFAT_OK, "partition found");
	CHECK(rfat_mount(&vol, DiskRead, &d, plba) == RFAT_OK, "mount ok");

	pg_ctx ctx;
	pg_idx index[341];
	unsigned char page[4096];
	CHECK(pg_open(&ctx, &vol, "/PG.BIN", index, 341, page,
				  kDiscId, 0, kEpoch) == PG_OK,
		  "paged table opens");
	CHECK(ctx.nPages == 3, "300 extents need 3 pages");
	CHECK(ctx.fetches == 0, "open fetches no pages (index only)");

	// Equivalence sweep: paged locate == resident scan, fields exact,
	// including page-boundary -1/+0/+1, gaps (MISS both sides), and the
	// first/last byte of every 25th extent.
	{
		unsigned checked = 0, missed = 0;
		for (size_t i = 0; i < exts.size(); ++i)
		{
			u64 e0 = exts[i].discOffset;
			u32 trial[4] = { 0, 1, exts[i].length > 2 ? exts[i].length - 1 : 0, exts[i].length };
			for (int t = 0; t < 4; ++t)
			{
				u64 off = e0 + trial[t];
				const ManifestExtent *ref = 0;
				bool refFound = RefFind(exts, off, &ref);
				pg_entry got;
				int rc = pg_locate(&ctx, off, &got);
				if (!refFound)
				{
					CHECK(rc == PG_MISS, "gap is MISS on both sides");
					++missed;
					continue;
				}
				CHECK(rc == PG_OK, "covered offset locates");
				if (rc != PG_OK)
					continue;
				CHECK(got.discOffset == ref->discOffset, "discOffset exact");
				CHECK(got.length == ref->length, "length exact");
				CHECK(got.kind == ref->kind, "kind exact");
				CHECK(got.source == ref->source, "source exact");
				CHECK(got.srcOffset == ref->srcOffset, "srcOffset exact");
				CHECK(got.genOff == ref->genOff, "genOff exact");
				++checked;
			}
		}
		CHECK(checked > 1000, "sweep covered four figures of lookups");
		CHECK(missed > 0, "sweep crossed real gaps");
	}
	// Fetch bounds: one fetch per new page, none for repeats.
	{
		unsigned before = ctx.fetches;
		pg_entry e;
		CHECK(pg_locate(&ctx, exts[0].discOffset, &e) == PG_OK, "page 0 again");
		CHECK(ctx.fetches == before + 1, "one fetch for the evicted page");
		CHECK(pg_locate(&ctx, exts[0].discOffset + 4, &e) == PG_OK, "same page again");
		CHECK(ctx.fetches == before + 1, "cached page costs no fetch");
		CHECK(pg_locate(&ctx, exts[299].discOffset, &e) == PG_OK, "last page");
		CHECK(ctx.fetches == before + 2, "one more fetch for the new page");
	}
	// Paths resolve byte-exact, including shared prefixes; OOB refuses.
	{
		char buf[512];
		pg_entry e;
		CHECK(pg_locate(&ctx, exts[7].discOffset, &e) == PG_OK, "locate for path");
		CHECK(pg_path(&ctx, e.pathOff, buf, sizeof(buf)) == PG_OK, "path resolves");
		CHECK(exts[7].path == buf, "path bytes exact");
		CHECK(pg_path(&ctx, 0xFFFFFFu, buf, sizeof(buf)) == PG_EIO, "wild path offset refused");
	}
	// pg_range costs no fetch; pg_next costs at most one (mid-page scan).
	{
		unsigned long long lo = 0, hi = 0;
		unsigned f0 = ctx.fetches;
		pg_range(&ctx, &lo, &hi);
		CHECK(lo == exts[0].discOffset, "range low is first extent");
		CHECK(hi == exts[299].discOffset + exts[299].length, "range high is last end");
		CHECK(pg_next(&ctx, exts[0].discOffset) == exts[1].discOffset,
			  "next inside first page");
		CHECK(pg_next(&ctx, exts[299].discOffset + 0x1000) == ~(u64)0,
			  "next past the end is unbounded");
		CHECK(ctx.fetches <= f0 + 1, "range/next cost at most one fetch");
	}
	// Served reads through the real pager-backed segment reader:
	// complete bytes across page/segment boundaries, MISS delegation,
	// storage failure without partial success.
	{
		sr_ctx sc;
		CHECK(sr_init_paged(&sc, &ctx, &vol, kDecl) == SR_OK,
			  "segment reader adopts the pager");
		// Whole-file read off /A.BIN (extent 0).
		{
			std::vector<u8> out(0x400, 0xCC);
			CHECK(sr_read(&sc, exts[0].discOffset, 0x400, &out[0]) == SR_OK,
				  "file read served");
			bool ok = true;
			for (u32 i = 0; i < 0x400 && ok; ++i)
				if (out[i] != acard[i])
					ok = false;
			CHECK(ok, "served bytes equal card bytes");
		}
		// Spanning read inside one mapped extent: extent 127
		// (/B.BIN @0x700, 0x300 bytes, fully covered).
		{
			u64 a = exts[127].discOffset + 0x700;
			const u32 len = 0x300;
			std::vector<u8> out(len, 0xCC);
			unsigned f0 = ctx.fetches;
			CHECK(sr_read(&sc, a, len, &out[0]) == SR_OK, "spanning read served");
			CHECK(sr_covers(&sc, a, len) == 1, "covered span queries covered");
			bool ok = true;
			for (u32 i = 0; i < len && ok; ++i)
				if (out[i] != bcard[0x700 + i])
					ok = false;
			CHECK(ok, "mapped span bytes exact");
			CHECK(ctx.fetches <= f0 + 2, "span costs at most two page fetches");
		}
		// Cross-page span in one read: extent 127 tail (/B.BIN) then
		// extent 128 head on the next page (/A.BIN @0x800), abutting
		// with no gap between them.
		{
			u64 a = exts[127].discOffset + 0x700;
			u32 len = (u32)(exts[128].discOffset + 0x100 - a);
			CHECK(len == 0x7A00, "span reaches the next page head");
			CHECK(exts[127].discOffset + exts[127].length == exts[128].discOffset,
				  "test setup: the pair abuts across the page boundary");
			std::vector<u8> out(len, 0xCC);
			unsigned f0 = ctx.fetches;
			CHECK(sr_read(&sc, a, len, &out[0]) == SR_OK, "cross-page span served");
			CHECK(sr_covers(&sc, a, len) == 1, "abutting span queries covered");
			bool ok = true;
			for (u32 i = 0; i < len && ok; ++i)
			{
				u8 want;
				if (a + i < exts[128].discOffset)
					want = bcard[0x700 + i];
				else
					want = acard[0x800 + (u32)(a + i - exts[128].discOffset)];
				if (out[i] != want)
					ok = false;
			}
			CHECK(ok, "tail and next-page head exact in one read");
			CHECK(ctx.fetches <= f0 + 3, "cross-page span costs at most three fetches");
		}
		// Plan-defined ZERO runs serve as zeros; unlisted gaps stop
		// with GAP (original-disc bytes, delegated whole by the
		// dispatcher); MISS outside leaves the buffer.
		{
			u64 zoff = exts[12].discOffset; // ZERO kind by construction
			CHECK(exts[12].kind == RIIVO_EXT_ZERO, "test setup: extent 12 is ZERO");
			std::vector<u8> outz(0x200, 0xCC);
			CHECK(sr_read(&sc, zoff, 0x200, &outz[0]) == SR_OK, "ZERO run served");
			bool zok = true;
			for (size_t i = 0; i < outz.size() && zok; ++i)
				if (outz[i] != 0)
					zok = false;
			CHECK(zok, "plan-defined ZERO reads as zero");
			CHECK(sr_covers(&sc, zoff, 0x200) == 1, "ZERO run queries covered");
			u64 gap = exts[0].discOffset + exts[0].length + 0x10;
			std::vector<u8> out(0x40, 0xCC);
			CHECK(sr_read(&sc, gap, 0x40, &out[0]) == SR_GAP, "unlisted gap stops with GAP");
			CHECK(sr_covers(&sc, gap, 0x40) == 0, "unlisted gap queries uncovered");
			std::vector<u8> out3((size_t)exts[0].length + 0x40, 0xCC);
			CHECK(sr_read(&sc, exts[0].discOffset, exts[0].length + 0x40,
						  &out3[0]) == SR_GAP,
				  "mapped + gap stops with GAP, never zero-padded");
			std::vector<u8> out2(0x40, 0xCC);
			CHECK(sr_read(&sc, 0x100, 0x40, &out2[0]) == SR_MISS, "outside is MISS");
			bool untouched = true;
			for (size_t i = 0; i < out2.size(); ++i)
				if (out2[i] != 0xCC)
					untouched = false;
			CHECK(untouched, "MISS writes nothing");
		}
		// Storage failure (torn chain) fails loudly, never partial.
		// /A.BIN starts right after PG.BIN's clusters; break its chain
		// at the second cluster (rfat reports corrupt, not short).
		{
			MemDisk d2 = d;
			const unsigned pgClus =
				(unsigned)((pgfile.size() + 511) / 512);
			const unsigned victim = 2 + pgClus + 1; // /A.BIN cluster 2
			d2.img[512 + 4 + (victim - 2) * 2] = 0x00;
			d2.img[512 + 4 + (victim - 2) * 2 + 1] = 0x00;
			rfat_drop_cache();
			rfat_vol v2;
			CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
			pg_ctx c2;
			pg_idx idx2[341];
			unsigned char pg2[4096];
			CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2,
						  kDiscId, 0, kEpoch) == PG_OK,
				  "table still opens (chain break is in data, not table)");
			sr_ctx sc2;
			CHECK(sr_init_paged(&sc2, &c2, &v2, kDecl) == SR_OK,
				  "reader adopts over edited image");
			std::vector<u8> out(0x1000, 0xCC);
			CHECK(sr_read(&sc2, exts[0].discOffset, 0x1000, &out[0]) == SR_EIO,
				  "torn chain fails loudly");
			rfat_drop_cache();
		}
		// Dangling path fails loudly, never partial.
		{
			std::vector<u8> out(0x80, 0xCC);
			CHECK(sr_read(&sc, exts[299].discOffset, 0x80, &out[0]) == SR_EIO,
				  "missing file is EIO, never partial");
		}
		// Wrong epoch refuses at open (stale file under a new boot).
		{
			pg_ctx c3;
			pg_idx idx3[341];
			unsigned char pg3[4096];
			CHECK(pg_open(&c3, &vol, "/PG.BIN", idx3, 341, pg3,
						  kDiscId, 0, kEpoch + 1) == PG_EBADTABLE,
				  "stale epoch refused");
		}
		// Wrong identity refuses at open.
		{
			pg_ctx c3;
			pg_idx idx3[341];
			unsigned char pg3[4096];
			CHECK(pg_open(&c3, &vol, "/PG.BIN", idx3, 341, pg3,
						  0xDEADBEEFu, 0, kEpoch) == PG_EBADTABLE,
				  "foreign game refused");
		}
		// Anti-shadow: a table below the declared size refuses at adopt.
		{
			std::vector<ManifestExtent> low;
			low.push_back(Ext(0x1000, 0x100, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							  0, "/A.BIN", 0));
			std::vector<u8> lblob, lfile;
			CHECK(Riivo::BuildManifestV1(low, Riivo::RIIVO_MANIFEST_DISCOVER,
										  kDiscId, 0, Riivo::RIIVO_CAP_SPLIT_READ,
										  Riivo::RIIVO_PROV_BOUNDED, lblob, why));
			CHECK(Riivo::BuildPagedFile(lblob, kEpoch, lfile, why));
			ImgFile lf;
			lf.name83 = "LOW     BIN";
			lf.data = lfile;
			std::vector<ImgFile> lfiles;
			lfiles.push_back(lf);
			MemDisk ld;
			BuildImage(ld, lfiles);
			rfat_drop_cache();
			rfat_vol lv;
			CHECK(rfat_mount(&lv, DiskRead, &ld, plba) == RFAT_OK, "mounts low image");
			pg_ctx lc;
			pg_idx lidx[341];
			unsigned char lpg[4096];
			CHECK(pg_open(&lc, &lv, "/LOW.BIN", lidx, 341, lpg,
						  kDiscId, 0, kEpoch) == PG_OK,
				  "low table opens (bounds are the reader's job)");
			sr_ctx sc3;
			CHECK(sr_init_paged(&sc3, &lc, &lv, kDecl) == SR_EBADTABLE,
				  "table below declared size refused at adopt");
			rfat_drop_cache();
		}
	}
	// Corruptions refuse: bad magic, bad CRC, unsorted index, wrong page
	// id, missing file, undersized index buffer.
	{
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		CHECK(pg_open(&c2, &vol, "/NOPE.BIN", idx2, 341, pg2,
					  kDiscId, 0, kEpoch) == PG_EIO,
			  "missing table file is EIO");
	}
	{
		// Flip one page byte inside the file image (entry area).
		MemDisk d2 = d;
		const unsigned pagesOff = (512u + 3u * 12u + 511u) & ~511u;
		d2.img[2048 + pagesOff + 100] ^= 0xFF;
		rfat_drop_cache();
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2,
					  kDiscId, 0, kEpoch) == PG_EBADTABLE,
			  "flipped page byte fails the open CRC");
		rfat_drop_cache();
	}
	{
		// Swap two index rows (order violation).
		MemDisk d2 = d;
		unsigned char tmp[12];
		memcpy(tmp, &d2.img[2048 + 512], 12);
		memcpy(&d2.img[2048 + 512], &d2.img[2048 + 524], 12);
		memcpy(&d2.img[2048 + 524], tmp, 12);
		rfat_drop_cache();
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2,
					  kDiscId, 0, kEpoch) == PG_EBADTABLE,
			  "swapped index rows refused (order or CRC)");
		rfat_drop_cache();
	}
	{
		// Truncated magic.
		MemDisk d2 = d;
		d2.img[2048] ^= 0xFF;
		rfat_drop_cache();
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2,
					  kDiscId, 0, kEpoch) == PG_EBADTABLE,
			  "bad magic refused");
		rfat_drop_cache();
	}
	{
		// Index buffer too small for the table.
		pg_ctx c2;
		pg_idx small[2];
		unsigned char pg2[4096];
		CHECK(pg_open(&c2, &vol, "/PG.BIN", small, 2, pg2,
					  kDiscId, 0, kEpoch) == PG_EBADTABLE,
			  "undersized index buffer refused");
	}
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
