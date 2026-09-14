// Plan-to-runtime segments: production planner -> BuildSegmentManifest ->
// real ARM segment reader, closing the partial-file loop on host.
// A production-built partial patch (original prefix -> external slice ->
// original suffix) serves complete bytes through ios/riivo_segread.c over a
// synthetic FAT volume, compared against the reference composition - plus
// whole-file parity with BuildPlanManifest, the no-silent-downgrade gate,
// and the emitter refusals. Exit 0 = all pass.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#include "riivo/RiivoTypes.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoConfig.hpp"
#include "riivo/RiivoPatchPlan.hpp"
#include "riivo/RiivoManifest.hpp"
#include "riivo/RiivoRedirectTable.hpp"
#include "riivo/ios/riivo_fat.h"
#include "riivo/ios/riivo_redirect.h"
#include "riivo/ios/riivo_segread.h"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// In-memory DirLister: fullDir -> relative files.
struct MemLister : public DirLister
{
	std::map<std::string, std::vector<std::string> > dirs;
	void Add(const std::string &dir, const std::string &rel)
	{
		dirs[dir].push_back(rel);
	}
	virtual void List(const std::string &fullDir, bool recursive,
					  std::vector<std::string> &out)
	{
		(void)recursive;
		std::map<std::string, std::vector<std::string> >::iterator it = dirs.find(fullDir);
		if (it == dirs.end())
			return;
		out.insert(out.end(), it->second.begin(), it->second.end());
	}
};

// In-memory sizes: external path -> size. Absent => missing.
struct MemSizes : public FileSizeProvider
{
	std::map<std::string, u32> m;
	void Put(const std::string &p, u32 s) { m[p] = s; }
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

// FST with /a.bin (sizeA) + /dir/b.arc (200). Offsets come from Layout.
static bool MakeFst(Fst &fst, std::vector<u8> &img, u32 sizeA)
{
	FstBuilder b;
	u8 rootImg[12] = {1, 0,0,0, 0,0,0,0, 0,0,0,1};
	u8 full[13];
	memcpy(full, rootImg, 12);
	full[12] = 0;
	if (!b.Parse(full, 13, false))
		return false;
	bool isNew = false;
	if (!b.AddOrReplace("/a.bin", sizeA, &isNew))
		return false;
	if (!b.AddOrReplace("/dir/b.arc", 200, &isNew))
		return false;
	b.Layout(0x1000, 32);
	b.Serialize(img, false);
	return fst.Parse(img.empty() ? 0 : &img[0], (u32)img.size(), false);
}

static ResolvedFile RF(const char *disc, const char *ext, const char *root = "/riivolution",
					   u32 off = 0, u32 foff = 0, u32 len = 0,
					   bool resize = true, bool create = false)
{
	ResolvedFile r;
	r.root = root;
	r.disc = disc;
	r.external = ext;
	r.resize = resize;
	r.create = create;
	r.offset = off;
	r.fileoffset = foff;
	r.length = len;
	return r;
}

static const u64 kBase = 0x180000000ULL;
static const u64 kDecl = 0x118000000ULL;
static const u32 kDiscId = 0x53424E41u;

// 4100-sector FAT16 superfloppy carrying /P.BIN (100 bytes: 0xC0+i pattern).
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
	Put16(m, 512 + 4, 0xFFFF);
	Put16(m, 512 + 6, 0xFFFF);
	memcpy(&m[1024], &m[512], 512);
	memcpy(&m[1536], "P       BIN", 11);
	m[1536 + 11] = 0x20;
	Put16(m, 1536 + 26, 2);
	Put32(m, 1536 + 28, 100);
	for (int i = 0; i < 100; ++i)
		m[2048 + i] = (u8)(0xC0 + i);
	memcpy(&m[1568], "Q       BIN", 11);
	m[1568 + 11] = 0x20;
	Put16(m, 1568 + 26, 3);
	Put32(m, 1568 + 28, 200);
	for (int i = 0; i < 200; ++i)
		m[2560 + i] = (u8)(0x80 + (i & 0x7F));
}

int main()
{
	// A. Whole-file parity: the segment emitter describes whole files
	// exactly like the placement emitter for the same inputs.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img, 100));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/a.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/a.bin", 150);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.errors.empty() && !plan.hasPartial);
		CHECK(!PlanNeedsSegments(plan));
		std::map<std::string, u64> bases;
		bases["/a.bin"] = kBase;
		std::vector<u8> seg, placed;
		GenLayout gen;
		CHECK(BuildSegmentManifest(plan, bases, &sizes, kDiscId, 0, seg, gen, why));
		CHECK(gen.total == 0 && gen.slices.empty());
		std::vector<PlacedFile> pl;
		PlacedFile p;
		p.offset = kBase;
		p.length = 150;
		p.external = "sd:/r/a.bin";
		pl.push_back(p);
		CHECK(BuildPlanManifest(plan, pl, kDiscId, placed, why));
		CHECK(seg.size() == placed.size() && memcmp(&seg[0], &placed[0], seg.size()) == 0);
	}

	// B. Partial span through the production planner: original prefix ->
	// external slice -> original suffix (resize=false keeps the tail).
	PatchPlan plan;
	MemSizes sizes;
	u64 origBase = 0;
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img, 1024));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/P.BIN", "/riivolution", 40, 10, 20, false));
		set.files.push_back(RF("/dir/b.arc", "/Q.BIN"));
		MemLister lister;
		sizes.Put("sd:/P.BIN", 100);
		sizes.Put("sd:/Q.BIN", 200);
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.errors.empty());
		CHECK(plan.hasPartial);
		CHECK(PlanNeedsSegments(plan));
		CHECK(plan.files.size() == 2);
		const PlannedFile &f = plan.files[0];
		CHECK(f.finalSize == 1024 && !f.wholeFile);
		CHECK(f.segs.size() == 3);
		if (f.segs.size() == 3)
		{
			CHECK(f.segs[0].kind == PlanSegment::SEG_ORIGINAL && f.segs[0].fileOffset == 0 && f.segs[0].length == 40);
			CHECK(f.segs[1].kind == PlanSegment::SEG_EXTERNAL && f.segs[1].fileOffset == 40 && f.segs[1].length == 20 && f.segs[1].srcOffset == 10);
			CHECK(f.segs[2].kind == PlanSegment::SEG_ORIGINAL && f.segs[2].fileOffset == 60 && f.segs[2].length == 964);
		}
		if (plan.files.size() == 2)
		{
			CHECK(plan.files[1].wholeFile && plan.files[1].finalSize == 200);
		}
		origBase = f.discOffsetOrig;
	}
	std::vector<u8> segBlob;
	GenLayout gen;
	{
		std::map<std::string, u64> bases;
		bases["/a.bin"] = kBase;
		// Second file two sectors past the first file's end: the
		// 0x1C00 interior gap is alignment-style slop the emitter
		// must name explicitly (ZERO), never leave unlisted.
		bases["/dir/b.arc"] = kBase + 0x2000;
		std::string why;
		CHECK(BuildSegmentManifest(plan, bases, &sizes, kDiscId, 0, segBlob, gen, why));
		CHECK(gen.total == 1004 && gen.slices.size() == 2);
		if (gen.slices.size() == 2)
		{
			CHECK(gen.slices[0].genOff == 0 && gen.slices[0].length == 40 && gen.slices[0].origAbs == origBase);
			CHECK(gen.slices[1].genOff == 40 && gen.slices[1].length == 964 && gen.slices[1].origAbs == origBase + 60);
		}
		CHECK(ValidateManifestV1(&segBlob[0], (u32)segBlob.size(), why));
		// The emitter names the interior gap explicitly: one ZERO run
		// covering exactly [kBase+1024, kBase+0x2000), found by walking
		// the encoded entries (off64 + len32 + kind16).
		{
			u32 n = (u32)segBlob[20] | ((u32)segBlob[21] << 8)
				  | ((u32)segBlob[22] << 16) | ((u32)segBlob[23] << 24);
			bool gapNamed = false;
			for (u32 i = 0; i < n; ++i)
			{
				size_t e = 48 + (size_t)i * 32;
				u64 off = 0;
				for (int k = 7; k >= 0; --k)
					off = (off << 8) | segBlob[e + (size_t)k];
				u32 len = (u32)segBlob[e + 8] | ((u32)segBlob[e + 9] << 8)
						| ((u32)segBlob[e + 10] << 16) | ((u32)segBlob[e + 11] << 24);
				u32 kind = (u32)segBlob[e + 12] | ((u32)segBlob[e + 13] << 8);
				if (off == kBase + 1024 && len == 0x1C00 && kind == 3)
					gapNamed = true;
			}
			CHECK(gapNamed, "interior gap emitted as an explicit ZERO run");
		}
	}

	// C. Complete bytes through the REAL segment reader: the store filled
	// from synthetic original bytes, the external from the card image,
	// compared against the reference composition for reads crossing both
	// boundaries, single bytes, source offsets, and injected failures.
	{
		// Synthetic original: distinct from the card pattern by construction.
		std::vector<u8> orig(1024);
		for (size_t i = 0; i < orig.size(); ++i)
			orig[i] = (u8)((i * 3 + 1) & 0xFF);
		std::vector<u8> genStore(gen.total, 0xCC);
		for (size_t i = 0; i < gen.slices.size(); ++i)
		{
			const GenSlice &s = gen.slices[i];
			u64 o = s.origAbs - origBase;
			CHECK(o + s.length <= orig.size());
			if (o + s.length <= orig.size())
				memcpy(&genStore[s.genOff], &orig[(size_t)o], s.length);
		}
		MemDisk d;
		BuildImage(d);
		rfat_vol vol;
		unsigned int plba = 0;
		CHECK(rfat_find_partition(DiskRead, &d, 0, &plba) == RFAT_OK);
		CHECK(rfat_mount(&vol, DiskRead, &d, plba) == RFAT_OK);
		sr_ctx ctx;
		CHECK(sr_init(&ctx, &segBlob[0], (u32)segBlob.size(), &vol,
					  &genStore[0], (u32)genStore.size(), kDecl, kDiscId, 0) == SR_OK,
			  "segmented table adopted with filled store");
		// Reference composition for a file offset.
		std::vector<u8> ref(1024);
		for (size_t i = 0; i < ref.size(); ++i)
		{
			if (i < 40 || i >= 60)
				ref[i] = orig[i];
			else
				ref[i] = (u8)(0xC0 + (10 + (i - 40)));
		}
		bool ok = true;
		// Whole file, then reads crossing each boundary, then singles.
		const struct { u64 off; u32 len; } reads[] = {
			{ kBase, 1024 }, { kBase + 30, 40 }, { kBase + 50, 30 },
			{ kBase + 39, 1 }, { kBase + 40, 1 }, { kBase + 59, 1 },
			{ kBase + 60, 1 }, { kBase + 1000, 24 }, { kBase + 10, 3 },
		};
		for (size_t r = 0; r < sizeof(reads) / sizeof(reads[0]) && ok; ++r)
		{
			std::vector<u8> out(reads[r].len, 0xCC);
			if (sr_read(&ctx, reads[r].off, reads[r].len, &out[0]) != SR_OK)
			{
				ok = false;
				break;
			}
			u64 base = reads[r].off - kBase;
			for (u32 i = 0; i < reads[r].len && ok; ++i)
				if (out[i] != ref[(size_t)(base + i)])
					ok = false;
		}
		CHECK(ok, "complete bytes match the reference composition");
		// Decisive mixed request through the production path: one read
		// spanning replacement tail (GENERATED original-suffix bytes) ->
		// named ZERO gap -> replacement head (EXTERNAL /Q.BIN bytes),
		// with three distinct byte regions. The emitter filled the gap,
		// so the reader composes the whole request exactly instead of
		// delegating away its replacements.
		{
			const u64 a = kBase + 1000;
			const u32 spanLen = 24 + 0x1C00 + 24;
			std::vector<u8> out(spanLen, 0xCC);
			CHECK(sr_covers(&ctx, a, spanLen) == 1, "mixed span queries covered");
			CHECK(sr_read(&ctx, a, spanLen, &out[0]) == SR_OK, "mixed span served whole");
			bool mok = true;
			for (u32 i = 0; i < 24 && mok; ++i) // GENERATED tail: orig[1000+i]
				if (out[i] != orig[(size_t)(1000 + i)])
					mok = false;
			for (u32 i = 24; i < 24 + 0x1C00 && mok; ++i) // named ZERO gap
				if (out[i] != 0)
					mok = false;
			for (u32 i = 0; i < 24 && mok; ++i) // EXTERNAL head: Q[0+i]
				if (out[24 + 0x1C00 + i] != (u8)(0x80 + (i & 0x7F)))
					mok = false;
			CHECK(mok, "tail + gap + head compose exactly in one request");
		}
		// Failure propagation across the same span: with /Q.BIN missing,
		// the whole composed request fails EIO - the served tail and gap
		// bytes never reach the caller as a partial success.
		{
			MemDisk noq;
			noq.img = d.img;
			noq.img[1568] = 0xE5;
			rfat_drop_cache();
			rfat_vol vol2;
			CHECK(rfat_mount(&vol2, DiskRead, &noq, plba) == RFAT_OK,
				  "remounts without /Q.BIN");
			sr_ctx nc;
			CHECK(sr_init(&nc, &segBlob[0], (u32)segBlob.size(), &vol2,
						  &genStore[0], (u32)genStore.size(), kDecl, kDiscId,
						  0) == SR_OK,
				  "adopts over the Q-less image");
			const u64 a = kBase + 1000;
			const u32 spanLen = 24 + 0x1C00 + 24;
			std::vector<u8> out(spanLen, 0xCC);
			CHECK(sr_read(&nc, a, spanLen, &out[0]) == SR_EIO,
				  "missing head file fails the composed span, never partial");
			rfat_drop_cache();
		}
		// MISS outside the span leaves the buffer alone.
		{
			std::vector<u8> out(0x40, 0xCC);
			CHECK(sr_read(&ctx, kBase + 0x3000, 0x40, &out[0]) == SR_MISS,
				  "outside the span is MISS");
			bool untouched = true;
			for (size_t i = 0; i < out.size(); ++i)
				if (out[i] != 0xCC)
					untouched = false;
			CHECK(untouched, "MISS writes nothing");
		}
		// Injected failures: short store, missing external, poisoned table.
		{
			sr_ctx shortStore;
			CHECK(sr_init(&shortStore, &segBlob[0], (u32)segBlob.size(), &vol,
						  &genStore[0], 40, kDecl, kDiscId, 0) == SR_OK,
				  "short store still adopts (presence checked)");
			std::vector<u8> out(0x100, 0xCC);
			CHECK(sr_read(&shortStore, kBase + 60, 0x100, &out[0]) == SR_EIO,
				  "read past the store fails, never partial");
		}
		{
			MemDisk empty;
			empty.img = d.img;
			// Wipe the directory entry so /P.BIN no longer opens.
			empty.img[1536] = 0xE5;
			rfat_drop_cache();
			rfat_vol vol2;
			CHECK(rfat_mount(&vol2, DiskRead, &empty, plba) == RFAT_OK,
				  "remounts the edited image");
			sr_ctx nc;
			CHECK(sr_init(&nc, &segBlob[0], (u32)segBlob.size(), &vol2,
						  &genStore[0], (u32)genStore.size(), kDecl, kDiscId,
						  0) == SR_OK,
				  "adopts over the edited image");
			std::vector<u8> out(0x40, 0xCC);
			CHECK(sr_read(&nc, kBase + 40, 0x20, &out[0]) == SR_EIO,
				  "missing external fails loudly");
			rfat_drop_cache();
		}
		{
			std::vector<u8> poison = segBlob;
			poison[0] = poison[1] = poison[2] = poison[3] = 0;
			sr_ctx pc;
			CHECK(sr_init(&pc, &poison[0], (u32)poison.size(), &vol,
						  &genStore[0], (u32)genStore.size(), kDecl, kDiscId,
						  0) == SR_EBADTABLE,
				  "poisoned table refuses (the late-fill abort shape)");
		}
	}

	// D. No silent downgrade: the whole-file path cannot express this plan,
	// so a failed RIV1 staging with segments required must refuse outright.
	{
		CHECK(PlanNeedsSegments(plan));
		std::vector<PlacedFile> pl;
		PlacedFile p;
		p.offset = kBase;
		p.length = 1024;
		p.external = "";
		pl.push_back(p);
		std::vector<u8> blob;
		std::string why;
		CHECK(!BuildPlanManifest(plan, pl, kDiscId, blob, why));
	}
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img, 100));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/P.BIN"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/P.BIN", 150);
		PatchPlan whole;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, whole, why));
		CHECK(!PlanNeedsSegments(whole));
	}

	// E. Bounded store: ORIGINAL slices past 8 MB refuse with the total.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img, 100));
		ResolvedPatchSet set;
		// 9 MB file, 8-byte patch at the front, tail preserved.
		set.files.push_back(RF("/big.bin", "/P.BIN", "/riivolution", 0, 0, 8, false));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/P.BIN", 100);
		PatchPlan big;
		std::string why;
		FstBuilder bb;
		u8 rootImg[12] = {1, 0,0,0, 0,0,0,0, 0,0,0,1};
		u8 full[13];
		memcpy(full, rootImg, 12);
		full[12] = 0;
		CHECK(bb.Parse(full, 13, false));
		bool isNew = false;
		CHECK(bb.AddOrReplace("/big.bin", 9u << 20, &isNew));
		bb.Layout(0x1000, 32);
		std::vector<u8> bimg;
		bb.Serialize(bimg, false);
		Fst bfst;
		CHECK(bfst.Parse(&bimg[0], (u32)bimg.size(), false));
		CHECK(BuildPatchPlan(bfst, set, "sd:", &lister, &sizes, big, why));
		CHECK(PlanNeedsSegments(big));
		std::map<std::string, u64> bases;
		bases["/big.bin"] = kBase;
		std::vector<u8> blob;
		GenLayout genBig;
		CHECK(!BuildSegmentManifest(big, bases, &sizes, kDiscId, 0, blob, genBig, why));
	}

	// F. Emitter refusals: no base, no sizes, original-without-source.
	{
		std::map<std::string, u64> noBases;
		std::vector<u8> blob;
		GenLayout gen;
		std::string why;
		CHECK(!BuildSegmentManifest(plan, noBases, &sizes, kDiscId, 0, blob, gen, why));
		CHECK(!BuildSegmentManifest(plan, noBases, 0, kDiscId, 0, blob, gen, why));
		PatchPlan created;
		PlannedFile cf;
		cf.disc = "/new.bin";
		cf.isNew = true;
		cf.create = true;
		cf.resize = true;
		cf.finalSize = 60;
		PlanSegment orig;
		orig.kind = PlanSegment::SEG_ORIGINAL;
		orig.fileOffset = 0;
		orig.length = 60;
		cf.segs.push_back(orig);
		created.files.push_back(cf);
		std::map<std::string, u64> cbases;
		cbases["/new.bin"] = kBase;
		CHECK(!BuildSegmentManifest(created, cbases, &sizes, kDiscId, 0, blob, gen, why));
	}

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
