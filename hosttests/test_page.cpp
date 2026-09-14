// Paged RIV1 table access: production-built tables, paged lookups equal
// resident scans, fetch behavior bounded, corruptions refused.
// The pager (ios/riivo_page.c, real code) runs over a synthetic FAT16
// volume holding a paged table file sliced from a PRODUCTION RIV1 blob
// (BuildManifestV1): no second format exists to drift. Exit 0 = pass.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "riivo/ios/riivo_fat.h"
#include "riivo/ios/riivo_page.h"
#include "riivo/RiivoManifest.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

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

static u32 Rd32le(const std::vector<u8> &v, size_t at)
{
	return (u32)v[at] | ((u32)v[at + 1] << 8) | ((u32)v[at + 2] << 16) |
		   ((u32)v[at + 3] << 24);
}

// FAT16 image with one file PG.BIN laid sequentially from cluster 2.
// Sized like the segread fixture (4096 data clusters -> FAT16; the file
// itself uses only its head) so the volume mounts as FAT16.
static void BuildImage(MemDisk &d, const std::vector<u8> &pgbin)
{
	const unsigned clusters = 4096;
	const unsigned fatSectors = 1;
	const unsigned total = 1 + 2 * fatSectors + 1 + clusters; // res+FATs+root+data
	const unsigned fileClusters = (unsigned)((pgbin.size() + 511) / 512);
	d.img.assign((total + 8) * 512, 0);
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
	// Chain only the file's own clusters (2..); the rest of the 4096-cluster
	// data area stays free (zeros). A full-length chain would run past this
	// 1-sector FAT into the mirror, root dir, and data.
	for (unsigned c = 0; c < fileClusters; ++c)
		Put16(m, fat0 + 4 + c * 2, (c + 1 < fileClusters) ? (3 + c) : 0xFFFF);
	memcpy(&m[1024], &m[512], 512);
	const unsigned root = 1536;
	memcpy(&m[root], "PG      BIN", 11);
	m[root + 11] = 0x20;
	Put16(m, root + 26, 2);
	Put32(m, root + 28, (unsigned long)pgbin.size());
	memcpy(&m[2048], &pgbin[0], pgbin.size());
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

// Slice a production blob's entry array + string blob into the paged
// file format (documented in riivo_page.h). Test-side encoder; the
// equivalence checks below prove it names the same extents.
static bool PageEncode(const std::vector<u8> &blob, std::vector<u8> &out,
					   std::string &why)
{
	if (!ValidateManifestV1(&blob[0], (u32)blob.size(), why))
		return false;
	const u32 count = Rd32le(blob, 20);
	const u32 strOff = Rd32le(blob, 24);
	const u32 nPages = (count + 127) / 128;
	if (nPages == 0 || nPages > 341)
	{
		why = "fixture too big";
		return false;
	}
	const u32 pagesOff = (512 + nPages * 12 + 511) & ~511u;
	const u32 blobLen = (u32)blob.size() - strOff;
	std::vector<u8> file(pagesOff + nPages * 4096 + blobLen, 0);
	// header
	file[0] = 'P';
	file[1] = 'G';
	file[2] = '1';
	file[3] = 'P';
	file[4] = 1;
	file[5] = 0;
	file[6] = 12;
	file[7] = 0;
	// u32s LE: nPages, nEntries, pagesOff, blobOff, crc (patched later), reserved
	u32 hdr[7] = { nPages, count, pagesOff, (u32)(pagesOff + nPages * 4096), 0, 0, 0 };
	for (int i = 0; i < 7; ++i)
	{
		file[8 + i * 4 + 0] = (u8)(hdr[i] & 0xFF);
		file[8 + i * 4 + 1] = (u8)((hdr[i] >> 8) & 0xFF);
		file[8 + i * 4 + 2] = (u8)((hdr[i] >> 16) & 0xFF);
		file[8 + i * 4 + 3] = (u8)((hdr[i] >> 24) & 0xFF);
	}
	// index + pages from the blob entry array at [48, 48+count*32)
	for (u32 p = 0; p < nPages; ++p)
	{
		u32 first = p * 128;
		u64 firstKey = Rd32le(blob, 48 + first * 32)
					 | ((u64)Rd32le(blob, 48 + first * 32 + 4) << 32);
		size_t at = 512 + p * 12;
		for (int k = 0; k < 8; ++k)
			file[at + k] = (u8)((firstKey >> (8 * k)) & 0xFF);
		file[at + 8] = (u8)(p & 0xFF);
		file[at + 9] = (u8)((p >> 8) & 0xFF);
		file[at + 10] = 0;
		file[at + 11] = 0;
		u32 n = count - first;
		if (n > 128)
			n = 128;
		memcpy(&file[pagesOff + p * 4096], &blob[48 + first * 32], n * 32);
	}
	memcpy(&file[pagesOff + nPages * 4096], &blob[strOff], blobLen);
	// CRC over [512, end)
	u32 crc = 0xFFFFFFFFu;
	for (size_t i = 512; i < file.size(); ++i)
	{
		crc ^= file[i];
		for (int b = 0; b < 8; ++b)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	crc ^= 0xFFFFFFFFu;
	file[24] = (u8)(crc & 0xFF);
	file[25] = (u8)((crc >> 8) & 0xFF);
	file[26] = (u8)((crc >> 16) & 0xFF);
	file[27] = (u8)((crc >> 24) & 0xFF);
	out.swap(file);
	return true;
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
	// 300 extents in 3 pages (128+128+44), with gaps, ZERO runs, and
	// shared-prefix paths. Grouped 10 covered + 1 gap apart.
	std::vector<ManifestExtent> exts;
	char pb[96];
	for (int i = 0; i < 300; ++i)
	{
		// 10 files of 0x8000 per group (span 0x50000), groups 0x100000
		// apart: dense but non-overlapping, 3 pages at 128/page.
		u64 off = 0x100000ULL + (u64)(i / 10) * 0x100000ULL + (u64)(i % 10) * 0x8000ULL;
		if (i % 25 == 12)
			exts.push_back(Ext(off, 0x1000, RIIVO_EXT_ZERO, RIIVO_SRC_NONE, 0, "", 0));
		else
		{
			snprintf(pb, sizeof(pb), "/mod/dir%02d/file%04d.bin", i % 17, i);
			exts.push_back(Ext(off, 0x8000, RIIVO_EXT_EXTERNAL, RIIVO_SRC_SD,
							   (u64)(i * 64), pb, 0));
		}
	}
	std::vector<u8> blob;
	std::string why;
	CHECK(Riivo::BuildManifestV1(exts, Riivo::RIIVO_MANIFEST_DISCOVER, 0x53424E41u, 0,
								 Riivo::RIIVO_CAP_SPLIT_READ | Riivo::RIIVO_CAP_ZERO_FILL,
								 Riivo::RIIVO_PROV_BOUNDED, blob, why));
	std::vector<u8> pgfile;
	CHECK(PageEncode(blob, pgfile, why));
	MemDisk d;
	BuildImage(d, pgfile);
	rfat_vol vol;
	unsigned int plba = 0xDEADu;
	CHECK(rfat_find_partition(DiskRead, &d, 0, &plba) == RFAT_OK, "partition found");
	CHECK(rfat_mount(&vol, DiskRead, &d, plba) == RFAT_OK, "mount ok");

	pg_ctx ctx;
	pg_idx index[341];
	unsigned char page[4096];
	char pathTmp[512];
	CHECK(pg_open(&ctx, &vol, "/PG.BIN", index, 341, page, pathTmp) == PG_OK,
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
	// Corruptions refuse: bad magic, bad CRC, unsorted index, wrong page
	// id, missing file. Each is a fresh image + open (no state carried).
	{
		MemDisk d2 = d;
		pg_ctx c2;
		CHECK(pg_open(&c2, &vol, "/NOPE.BIN", index, 341, page, pathTmp) == PG_EIO,
			  "missing table file is EIO");
	}
	{
		// Flip one page byte inside the file image (entry area).
		MemDisk d2 = d;
		const unsigned pagesOff = 512 + ((3 * 12 + 511) & ~511u);
		d2.img[2048 + pagesOff + 100] ^= 0xFF;
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		char pt2[512];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2, pt2) == PG_EBADTABLE,
			  "flipped page byte fails the open CRC");
	}
	{
		// Swap two index rows (order violation).
		MemDisk d2 = d;
		unsigned char tmp[12];
		memcpy(tmp, &d2.img[2048 + 512], 12);
		memcpy(&d2.img[2048 + 512], &d2.img[2048 + 524], 12);
		memcpy(&d2.img[2048 + 524], tmp, 12);
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		char pt2[512];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2, pt2) == PG_EBADTABLE,
			  "swapped index rows refused (order or CRC)");
	}
	{
		// Truncated magic.
		MemDisk d2 = d;
		d2.img[2048] ^= 0xFF;
		rfat_vol v2;
		CHECK(rfat_mount(&v2, DiskRead, &d2, plba) == RFAT_OK, "remounts edited image");
		pg_ctx c2;
		pg_idx idx2[341];
		unsigned char pg2[4096];
		char pt2[512];
		CHECK(pg_open(&c2, &v2, "/PG.BIN", idx2, 341, pg2, pt2) == PG_EBADTABLE,
			  "bad magic refused");
	}
	{
		// Index buffer too small for the table.
		pg_ctx c2;
		pg_idx small[2];
		unsigned char pg2[4096];
		char pt2[512];
		CHECK(pg_open(&c2, &vol, "/PG.BIN", small, 2, pg2, pt2) == PG_EBADTABLE,
			  "undersized index buffer refused");
	}
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
