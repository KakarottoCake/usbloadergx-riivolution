// Segment reader proof: the REAL ios/riivo_segread.c (+ ios/riivo_fat.c)
// served from RIV1 tables built by the PRODUCTION builders
// (BuildManifestV1 / ValidateManifestV1 / ManifestTableSize, plus RIIV
// BuildRedirectTable for cross-format parity), over a synthetic in-memory
// FAT16 volume. Exit 0 = all pass. No Wii toolchain, no reserved writes.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "riivo/ios/riivo_fat.h"
#include "riivo/ios/riivo_redirect.h"
#include "riivo/ios/riivo_segread.h"
#include "riivo/RiivoManifest.hpp"
#include "riivo/RiivoRedirectTable.hpp"

static int g_checks = 0, g_fail = 0;
static void check(bool cond, const char *what)
{
	++g_checks;
	if (!cond)
	{
		++g_fail;
		std::printf("FAIL: %s\n", what);
	}
}

struct MemDisk
{
	std::vector<u8> img;
};

static int DiskRead(void *ctx, unsigned int lba, unsigned int count, void *buf)
{
	MemDisk *d = (MemDisk *) ctx;
	if ((u64) (lba + count) * 512 > d->img.size())
		return 0;
	memcpy(buf, &d->img[(size_t) lba * 512], (size_t) count * 512);
	return 1;
}

static void Put16(std::vector<u8> &v, size_t at, unsigned x)
{
	v[at] = (u8) (x & 0xFF);
	v[at + 1] = (u8) ((x >> 8) & 0xFF);
}

static void Put32(std::vector<u8> &v, size_t at, unsigned long x)
{
	v[at] = (u8) (x & 0xFF);
	v[at + 1] = (u8) ((x >> 8) & 0xFF);
	v[at + 2] = (u8) ((x >> 16) & 0xFF);
	v[at + 3] = (u8) ((x >> 24) & 0xFF);
}

// 4100-sector FAT16 superfloppy (4096 clusters -> FAT16): reserved 1,
// 2 FATs of 1 sector, 16 root entries, data from LBA 4.
static void BuildImage(MemDisk &d)
{
	d.img.assign(4100 * 512, 0);
	std::vector<u8> &m = d.img;
	Put16(m, 11, 512);
	m[13] = 1;
	Put16(m, 14, 1);
	m[16] = 2;
	Put16(m, 17, 16);
	Put16(m, 19, 4100);
	m[21] = 0xF8;
	Put16(m, 22, 1);
	m[510] = 0x55;
	m[511] = 0xAA;
	Put16(m, 512 + 0, 0xFFF8);
	Put16(m, 512 + 2, 0xFFFF);
	Put16(m, 512 + 4, 3);
	Put16(m, 512 + 6, 0xFFFF);
	Put16(m, 512 + 8, 0xFFFF);
	memcpy(&m[1024], &m[512], 512);
	memcpy(&m[1536], "HELLO   TXT", 11);
	m[1536 + 11] = 0x20;
	Put16(m, 1536 + 26, 2);
	Put32(m, 1536 + 28, 600);
	memcpy(&m[1568], "SMALL   BIN", 11);
	m[1568 + 11] = 0x20;
	Put16(m, 1568 + 26, 4);
	Put32(m, 1568 + 28, 100);
	for (int i = 0; i < 512; ++i)
		m[2048 + i] = (u8) (i & 0xFF);
	for (int i = 0; i < 88; ++i)
		m[2560 + i] = (u8) (0x80 + (i & 0x7F));
	for (int i = 0; i < 100; ++i)
		m[3072 + i] = (u8) (0x40 + (i & 0x3F));
}

static const u32 kDiscId = 0x53424E41u;
static const u64 kDecl = 0x118000000ULL; // declared virtual-disc bytes
static const u64 kModBase = 0x180000000ULL; // mod region, above declared end

static Riivo::ManifestExtent Ext(u64 off, u32 len, u16 kind, u16 src,
								 u64 srcOff, const std::string &path, u32 gen)
{
	Riivo::ManifestExtent e;
	e.discOffset = off;
	e.length = len;
	e.kind = kind;
	e.source = src;
	e.srcOffset = srcOff;
	e.path = path;
	e.genOff = gen;
	return e;
}

// Whole-file RIV1 (single EXTENT per file, srcOffset 0) over the card files.
static bool WholeFileTable(std::vector<u8> &blob, std::string &why)
{
	std::vector<Riivo::ManifestExtent> exts;
	exts.push_back(Ext(kModBase, 0x400, Riivo::RIIVO_EXT_EXTERNAL,
					   Riivo::RIIVO_SRC_SD, 0, "/HELLO.TXT", 0));
	exts.push_back(Ext(kModBase + 0x1000, 0x100, Riivo::RIIVO_EXT_EXTERNAL,
					   Riivo::RIIVO_SRC_SD, 0, "/SMALL.BIN", 0));
	return Riivo::BuildManifestV1(exts, Riivo::RIIVO_MANIFEST_DISCOVER,
								  kDiscId, 0,
								  Riivo::RIIVO_CAP_SPLIT_READ,
								  Riivo::RIIVO_PROV_BOUNDED, blob, why);
}

// RIIV whole-file table over the same files (behavior baseline).
static bool WholeFileRiiv(std::vector<u8> &blob, std::string &why)
{
	std::vector<Riivo::RedirectEntry> entries;
	entries.push_back(Riivo::RedirectEntry(kModBase, 0x400, "/HELLO.TXT"));
	entries.push_back(Riivo::RedirectEntry(kModBase + 0x1000, 0x100, "/SMALL.BIN"));
	return Riivo::BuildRedirectTable(entries, Riivo::RIIVO_PART_DISCOVER,
									 blob, why);
}

// Multi-segment file: ORIGINAL [0,100) delegated, EXTERNAL [100,300) at
// file offset 500, ZERO [300,340), EXTERNAL [340,440) at file offset 700.
// Plus a GENERATED [0x800,0x900) run reading the reserved store.
static bool SegmentedTable(std::vector<u8> &blob, std::string &why)
{
	std::vector<Riivo::ManifestExtent> exts;
	exts.push_back(Ext(kModBase + 100, 200, Riivo::RIIVO_EXT_EXTERNAL,
					   Riivo::RIIVO_SRC_SD, 500, "/HELLO.TXT", 0));
	exts.push_back(Ext(kModBase + 300, 40, Riivo::RIIVO_EXT_ZERO,
					   Riivo::RIIVO_SRC_NONE, 0, "", 0));
	exts.push_back(Ext(kModBase + 340, 100, Riivo::RIIVO_EXT_EXTERNAL,
					   Riivo::RIIVO_SRC_SD, 700, "/HELLO.TXT", 0));
	exts.push_back(Ext(kModBase + 0x800, 0x100, Riivo::RIIVO_EXT_GENERATED,
					   Riivo::RIIVO_SRC_NONE, 0, "", 0x40));
	return Riivo::BuildManifestV1(exts, Riivo::RIIVO_MANIFEST_DISCOVER,
								  kDiscId, 0,
								  Riivo::RIIVO_CAP_SPLIT_READ
								  | Riivo::RIIVO_CAP_ZERO_FILL
								  | Riivo::RIIVO_CAP_GENERATED,
								  Riivo::RIIVO_PROV_BOUNDED, blob, why);
}

static int ServeAll(sr_ctx &ctx, u64 off, u32 len, std::vector<u8> &out)
{
	out.assign(len, 0xCC);
	if (len == 0)
		return sr_read(&ctx, off, 0, &out[0]);
	return sr_read(&ctx, off, len, &out[0]);
}

int main()
{
	MemDisk d;
	BuildImage(d);
	rfat_vol vol;
	unsigned int plba = 0xDEADu;
	check(rfat_find_partition(DiskRead, &d, 0, &plba) == RFAT_OK,
		  "superfloppy found");
	check(plba == 0, "superfloppy LBA 0");
	check(rfat_mount(&vol, DiskRead, &d, plba) == RFAT_OK, "mount ok");

	// Whole-file RIV1 served through the real segment reader.
	std::vector<u8> blob;
	std::string why;
	check(WholeFileTable(blob, why), "production builds whole-file RIV1");
	sr_ctx ctx;
	check(sr_init(&ctx, &blob[0], (u32) blob.size(), &vol,
				  0, 0, kDecl, kDiscId, 0) == SR_OK,
		  "segment reader adopts it");
	{
		unsigned long long lo = 0, hi = 0;
		sr_range(&ctx, &lo, &hi);
		check(lo == kModBase && hi == kModBase + 0x1100, "range spans extents");
	}
	{
		// Exact hit: 600 file bytes + sector-round tail pad.
		std::vector<u8> out;
		check(ServeAll(ctx, kModBase, 0x400, out) == SR_OK, "hit served");
		bool ok = true;
		for (int i = 0; i < 512 && ok; ++i)
			if (out[i] != (u8) (i & 0xFF))
				ok = false;
		for (int i = 0; i < 88 && ok; ++i)
			if (out[512 + i] != (u8) (0x80 + (i & 0x7F)))
				ok = false;
		for (u32 i = 600; i < 0x400 && ok; ++i)
			if (out[i] != 0)
				ok = false;
		check(ok, "hit bytes + tail pad exact");
	}
	{
		// MISS outside the region: untouched, no lookup spent.
		std::vector<u8> out;
		unsigned l0 = ctx.lookups;
		check(ServeAll(ctx, 0x1000, 0x100, out) == SR_MISS, "outside is MISS");
		bool untouched = true;
		for (size_t i = 0; i < out.size(); ++i)
			if (out[i] != 0xCC)
				untouched = false;
		check(untouched, "MISS writes nothing");
		check(ctx.lookups == l0, "MISS costs no lookup");
	}
	{
		// Cross-format parity: RIV1 through sr reads byte-identical to RIIV
		// through rr over the same files and reads.
		std::vector<u8> riiV;
		check(WholeFileRiiv(riiV, why), "production builds whole-file RIIV");
		rr_ctx rc;
		check(rr_init(&rc, &riiV[0], (unsigned) riiV.size(), &vol) == RR_OK,
			  "whole-file runtime adopts it");
		const u64 offs[] = { kModBase, kModBase + 0x100, kModBase + 0x300,
							 kModBase + 0x1000, kModBase + 0x1050 };
		const u32 lens[] = { 0x400, 0x200, 0x300, 0x100, 0x40 };
		bool ok = true;
		for (int r = 0; r < 5 && ok; ++r)
		{
			std::vector<u8> a(lens[r], 0xCC), b(lens[r], 0xCC);
			if (sr_read(&ctx, offs[r], lens[r], &a[0]) != SR_OK
				|| rr_read(&rc, offs[r], lens[r], &b[0]) != RR_OK
				|| a != b)
				ok = false;
		}
		check(ok, "RIV1/sr parity with RIIV/rr on whole files");
	}

	// Segmented file through the real reader: srcOffset advancement,
	// ZERO runs, ORIGINAL gaps, GENERATED store, spanning assembly.
	{
		std::vector<u8> seg;
		check(SegmentedTable(seg, why), "production builds segmented RIV1");
		std::vector<u8> gen(0x200, 0);
		for (size_t i = 0; i < gen.size(); ++i)
			gen[i] = (u8) (0xE0 + (i & 0x1F));
		sr_ctx sc;
		check(sr_init(&sc, &seg[0], (u32) seg.size(), &vol,
					  &gen[0], (u32) gen.size(), kDecl, kDiscId, 0) == SR_OK,
			  "segmented table adopted with a store");
		{
			// Spanning read: ORIGINAL gap + EXT + ZERO + EXT + gap + GEN.
			std::vector<u8> out;
			check(ServeAll(sc, kModBase, 0x900, out) == SR_OK,
				  "spanning segmented read served");
			bool ok = true;
			for (u32 i = 0; i < 100 && ok; ++i) // ORIGINAL gap: zeros
				if (out[i] != 0)
					ok = false;
			for (u32 i = 0; i < 200 && ok; ++i) // EXT at srcOffset 500
			{
				u32 fo = 500 + i;
				u8 want = 0;
				if (fo < 512)
					want = (u8) (fo & 0xFF);
				else if (fo < 600)
					want = (u8) (0x80 + ((fo - 512) & 0x7F));
				if (out[100 + i] != want)
					ok = false;
			}
			for (u32 i = 300; i < 340 && ok; ++i) // ZERO run
				if (out[i] != 0)
					ok = false;
			for (u32 i = 0; i < 100 && ok; ++i) // EXT at srcOffset 700 (short: 600B file)
			{
				u32 fo = 700 + i;
				u8 want = 0;
				if (fo < 512)
					want = (u8) (fo & 0xFF);
				else if (fo < 600)
					want = (u8) (0x80 + ((fo - 512) & 0x7F));
				if (out[340 + i] != want)
					ok = false;
			}
			for (u32 i = 440; i < 0x800 && ok; ++i) // gap to GENERATED
				if (out[i] != 0)
					ok = false;
			for (u32 i = 0; i < 0x100 && ok; ++i) // GENERATED at genOff 0x40
				if (out[0x800 + i] != (u8) (0xE0 + ((0x40 + i) & 0x1F)))
					ok = false;
			check(ok, "segment assembly exact incl srcOffset/ZERO/store");
		}
		{
			// GENERATED without a store refuses at init, never partial.
			sr_ctx bad;
			check(sr_init(&bad, &seg[0], (u32) seg.size(), &vol,
						  0, 0, kDecl, kDiscId, 0) == SR_EBADTABLE,
				  "generated without a store refused");
		}
		{
			// Wrong game identity refuses whole, never partial.
			sr_ctx bad;
			check(sr_init(&bad, &seg[0], (u32) seg.size(), &vol,
						  &gen[0], (u32) gen.size(), kDecl, 0xDEADBEEFu,
						  0) == SR_EBADTABLE,
				  "foreign game identity refused");
		}
		{
			// Table reaching below the declared size would shadow game
			// data: refused at init.
			sr_ctx bad;
			check(sr_init(&bad, &seg[0], (u32) seg.size(), &vol,
						  &gen[0], (u32) gen.size(), kModBase + 0x1000,
						  kDiscId, 0) == SR_EBADTABLE,
				  "table below declared size refused");
		}
	}

	// Init refusals: corrupt tables never become a reader.
	{
		sr_ctx bad;
		std::vector<u8> t = blob;
		check(sr_init(0, &t[0], (u32) t.size(), &vol, 0, 0, kDecl, kDiscId,
					0) == SR_EINVAL,
			  "null context refused");
		std::vector<u8> b1 = blob;
		b1[0] ^= 0xFF;
		check(sr_init(&bad, &b1[0], (u32) b1.size(), &vol, 0, 0, kDecl,
					  kDiscId, 0) == SR_EBADTABLE,
			  "bad magic refused");
		std::vector<u8> b2 = blob;
		b2[30] ^= 0x01;
		check(sr_init(&bad, &b2[0], (u32) b2.size(), &vol, 0, 0, kDecl,
					  kDiscId, 0) == SR_EBADTABLE,
			  "crc mismatch refused");
		std::vector<Riivo::ManifestExtent> ov;
		ov.push_back(Ext(kModBase, 0x400, Riivo::RIIVO_EXT_EXTERNAL,
						 Riivo::RIIVO_SRC_SD, 0, "/HELLO.TXT", 0));
		ov.push_back(Ext(kModBase + 0x200, 0x400, Riivo::RIIVO_EXT_EXTERNAL,
						 Riivo::RIIVO_SRC_SD, 0, "/SMALL.BIN", 0));
		std::vector<u8> ob;
		check(!Riivo::BuildManifestV1(ov, Riivo::RIIVO_MANIFEST_DISCOVER,
									  kDiscId, 0, Riivo::RIIVO_CAP_SPLIT_READ,
									  Riivo::RIIVO_PROV_BOUNDED, ob, why),
			  "overlapping extents never encode");
	}

	// Missing file fails loudly with the cache dropped, then repeats
	// without hanging; zero-length reads succeed untouched.
	{
		std::vector<Riivo::ManifestExtent> gone;
		gone.push_back(Ext(kModBase, 0x100, Riivo::RIIVO_EXT_EXTERNAL,
						   Riivo::RIIVO_SRC_SD, 0, "/GONE.BIN", 0));
		std::vector<u8> gb;
		check(Riivo::BuildManifestV1(gone, Riivo::RIIVO_MANIFEST_DISCOVER,
									 kDiscId, 0, Riivo::RIIVO_CAP_SPLIT_READ,
									 Riivo::RIIVO_PROV_BOUNDED, gb, why),
			  "missing-file table still encodes");
		sr_ctx gc;
		check(sr_init(&gc, &gb[0], (u32) gb.size(), &vol, 0, 0, kDecl,
					  kDiscId, 0) == SR_OK,
			  "missing-file table still adopts");
		std::vector<u8> out(0x100, 0xCC);
		check(sr_read(&gc, kModBase, 0x100, &out[0]) == SR_EIO,
			  "missing file is EIO, never partial");
		check(!gc.cached_valid, "EIO drops the cached file");
		check(sr_read(&gc, kModBase, 0x100, &out[0]) == SR_EIO,
			  "repeat still EIO, no hang");
		check(sr_read(&gc, kModBase, 0, &out[0]) == SR_OK,
			  "zero-length read ok");
	}

	// Search depth: 200 ordered extents, exact bytes against a model.
	{
		std::vector<Riivo::ManifestExtent> many;
		char pb[64];
		for (int i = 0; i < 200; ++i)
		{
			snprintf(pb, sizeof(pb), "%s", (i % 2) ? "/SMALL.BIN" : "/HELLO.TXT");
			many.push_back(Ext(kModBase + (u64) i * 0x1000, 0x200,
							   Riivo::RIIVO_EXT_EXTERNAL, Riivo::RIIVO_SRC_SD,
							   (u64) (i * 7) % 64, pb, 0));
		}
		std::vector<u8> mb;
		check(Riivo::BuildManifestV1(many, Riivo::RIIVO_MANIFEST_DISCOVER,
									 kDiscId, 0, Riivo::RIIVO_CAP_SPLIT_READ,
									 Riivo::RIIVO_PROV_BOUNDED, mb, why),
			  "200-extent table encodes");
		sr_ctx mc;
		check(sr_init(&mc, &mb[0], (u32) mb.size(), &vol, 0, 0, kDecl,
					  kDiscId, 0) == SR_OK,
			  "200-extent table adopted");
		bool ok = true;
		for (int r = 0; r < 40 && ok; ++r)
		{
			u64 off = kModBase + (u64) ((r * 37) % 200) * 0x1000 + (r * 13) % 0x180;
			std::vector<u8> out(0x180, 0xCC);
			if (sr_read(&mc, off, 0x180, &out[0]) != SR_OK)
			{
				ok = false;
				break;
			}
			for (u32 i = 0; i < 0x180 && ok; ++i)
			{
				u64 p = off + i;
				u64 idx = (p - kModBase) / 0x1000;
				u64 eoff = kModBase + idx * 0x1000;
				u8 want = 0;
				if (idx < 200 && p - eoff < 0x200)
				{
					u64 fo = (u64) (idx * 7) % 64 + (p - eoff);
					if (idx % 2)
					{
						if (fo < 100)
							want = (u8) (0x40 + (fo & 0x3F));
					}
					else if (fo < 600)
					{
						want = (fo < 512) ? (u8) (fo & 0xFF)
										  : (u8) (0x80 + ((fo - 512) & 0x7F));
					}
				}
				if (out[i] != want)
					ok = false;
			}
		}
		check(ok, "200-extent search serves model-exact bytes");
	}

	std::printf("%d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
