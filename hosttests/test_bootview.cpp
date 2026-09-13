// Patched boot view through the production overlay.
// Stock/no-mod view preserves stock behavior bit-for-bit; grown patched
// header + table agree (0x424/428/42c cohere, FST served exact); crossing,
// OOB, empty, and overflow inputs refuse/fall back explicitly. Production
// linkage (RiivoBootView only, no console).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#include "riivo/RiivoBootView.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main()
{
	// Stock header image (0x440, BE words at 0x424/428/42c = stock FST).
	std::vector<u8> stock(BOOTVIEW_HEADER_BYTES, 0);
	BootViewWriteBE32(&stock[BOOTVIEW_FST_OFF], 0x1000); // offset words
	BootViewWriteBE32(&stock[BOOTVIEW_FST_SIZE], 0x200); // size words
	BootViewWriteBE32(&stock[BOOTVIEW_FST_MAX], 0x200);  // max words

	// 1. Inactive serves nothing (stock behavior preserved).
	{
		BootView v;
		CHECK(!v.Active());
		u8 buf[16];
		memset(buf, 0xA5, sizeof(buf));
		CHECK(!v.Serve(0, buf, sizeof(buf)));
		CHECK(buf[0] == 0xA5); // untouched
		CHECK(!v.Serve(0x1000, buf, 4));
	}

	// 2. Patch header: words cohere (bytes>>2), stock bytes otherwise kept.
	{
		std::vector<u8> patched;
		std::string why;
		// fst at 0x8000 bytes, size 0x1234, max 0x2000.
		CHECK(PatchBootHeader(&stock[0], (u32)stock.size(),
							  0x8000, 0x1234, 0x2000, patched, why));
		CHECK(patched.size() == BOOTVIEW_HEADER_BYTES);
		CHECK(BootViewReadBE32(&patched[BOOTVIEW_FST_OFF]) == 0x8000u >> 2);
		CHECK(BootViewReadBE32(&patched[BOOTVIEW_FST_SIZE]) == 0x1234u >> 2);
		CHECK(BootViewReadBE32(&patched[BOOTVIEW_FST_MAX]) == 0x2000u >> 2);
		// Non-FST bytes identical to stock.
		bool same = true;
		for (size_t i = 0; i < patched.size(); ++i)
		{
			if (i >= BOOTVIEW_FST_OFF && i < BOOTVIEW_FST_OFF + 12)
				continue;
			if (patched[i] != stock[i])
				same = false;
		}
		CHECK(same);
	}

	// 3. Stock-equivalence: active with identical header+FST serves identical.
	{
		std::vector<u8> fst(256, 0x5A);
		BootView v;
		std::string why;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v.Active());
		CHECK(v.ArmFst(0x4000, fst, (u32)fst.size(), why));
		CHECK(v.FstArmed());
		u8 buf[BOOTVIEW_HEADER_BYTES];
		CHECK(v.Serve(0, buf, sizeof(buf)));
		CHECK(memcmp(buf, &stock[0], sizeof(buf)) == 0);
		u8 fbuf[256];
		CHECK(v.Serve(0x4000, fbuf, sizeof(fbuf)));
		CHECK(memcmp(fbuf, &fst[0], sizeof(fbuf)) == 0);
		// Mid-FST sub-read exact.
		u8 sub[32];
		CHECK(v.Serve(0x4000 + 100, sub, sizeof(sub)));
		CHECK(memcmp(sub, &fst[100], sizeof(sub)) == 0);
		v.Deactivate();
		CHECK(!v.Active());
		CHECK(!v.FstArmed());
		CHECK(!v.Serve(0, buf, 16));
	}

	// 4. Grown coherence: patched header words match served table extent.
	{
		std::vector<u8> fst(0x3000, 0x33);
		std::vector<u8> patched;
		std::string why;
		CHECK(PatchBootHeader(&stock[0], (u32)stock.size(),
							  0x10000, (u32)fst.size(), (u32)fst.size(),
							  patched, why));
		BootView v;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), patched, why));
		CHECK(v.ArmFst(0x10000, fst, (u32)fst.size(), why));
		u8 hdr[12];
		CHECK(v.Serve(BOOTVIEW_FST_OFF, hdr, sizeof(hdr)));
		u32 offW = BootViewReadBE32(hdr);
		u32 sizeW = BootViewReadBE32(hdr + 4);
		u32 maxW = BootViewReadBE32(hdr + 8);
		CHECK((u64)offW << 2 == 0x10000);
		CHECK((u64)sizeW << 2 <= fst.size() + 4); // word truncation tolerance
		CHECK(maxW == sizeW); // Dolphin: max == size in the coherent view
		u8 tail[16];
		CHECK(v.Serve(0x10000 + (u32)fst.size() - 16, tail, sizeof(tail)));
		CHECK(memcmp(tail, &fst[fst.size() - 16], 16) == 0);
	}

	// 5. Crossing/OOB fall back to stock (no split reassembly in overlay).
	{
		std::vector<u8> fst(128, 0x11);
		BootView v;
		std::string why;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v.ArmFst(0x5000, fst, (u32)fst.size(), why));
		u8 buf[32];
		memset(buf, 0xCC, sizeof(buf));
		// Header-crossing: ends past 0x440.
		CHECK(!v.Serve(BOOTVIEW_HEADER_BYTES - 8, buf, 16));
		CHECK(buf[0] == 0xCC);
		// FST-crossing: ends past table end.
		CHECK(!v.Serve(0x5000 + 120, buf, 16));
		// Gap between header end and FST start: stock.
		CHECK(!v.Serve(BOOTVIEW_HEADER_BYTES + 16, buf, 16));
		// Zero-length: nothing to serve.
		CHECK(!v.Serve(0, buf, 0));
		CHECK(!v.Serve(0x5000, buf, 0));
	}

	// 6. Refusals: short headers, size gate, unaligned offset, overflow.
	{
		std::vector<u8> patched;
		std::string why;
		u8 shortHdr[16] = {0};
		CHECK(!PatchBootHeader(shortHdr, sizeof(shortHdr), 0x1000, 0x100, 0x100, patched, why));
		CHECK(!why.empty());
		CHECK(!PatchBootHeader(&stock[0], (u32)stock.size(), 0x1001, 0x100, 0x100, patched, why));
		BootView v;
		std::vector<u8> empty;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(!v.ArmFst(0x1000, empty, 0, why));
		CHECK(!v.FstArmed());
		CHECK(!v.Activate(shortHdr, sizeof(shortHdr), stock, why));
		CHECK(!v.Active());
		// Grown staging (256 staged vs 128 on disc): refused, header stays
		// servable, FST falls back to stock (no truncated prefix served).
		std::vector<u8> grown(256, 0x77);
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(!v.ArmFst(0x5000, grown, 128, why));
		CHECK(!why.empty());
		CHECK(!v.FstArmed());
		u8 hbuf[16];
		CHECK(v.Serve(0, hbuf, sizeof(hbuf)));
		u8 fbuf[128];
		CHECK(!v.Serve(0x5000, fbuf, sizeof(fbuf)));
		// Compacted staging (smaller): likewise refused (exact match only).
		std::vector<u8> small(64, 0x77);
		CHECK(!v.ArmFst(0x5000, small, 128, why));
		CHECK(!v.FstArmed());
		// Arming without an active view refuses (ordering contract).
		BootView v2;
		CHECK(!v2.ArmFst(0x5000, small, (u32)small.size(), why));
	}

	// ---- DOL segment serving (injected readers, no console/card) ----
	// Fake disc image (stock bytes) + fake mod files.
	static const u64 kDolBase = 0x20000;
	static const u64 kDolSize = 0x1000;
	std::vector<u8> disc(0x21000, 0);
	for (size_t i = 0; i < disc.size(); ++i)
		disc[i] = (u8)(i & 0xFF); // recognizable stock pattern
	std::map<std::string, std::vector<u8> > files;
	files["sd:/m/patch.bin"] = std::vector<u8>(0x100, 0xEE);
	struct Ctx {
		std::vector<u8> *disc;
		std::map<std::string, std::vector<u8> > *files;
		bool failStock;
		bool failFat;
	};
	Ctx ctx;
	ctx.disc = &disc;
	ctx.files = &files;
	ctx.failStock = false;
	ctx.failFat = false;
	BootReaders readers;
	readers.ctx = &ctx;
	readers.stock = [](u64 off, u8 *dst, u32 len, void *c) -> bool {
		Ctx *x = (Ctx *)c;
		if (x->failStock)
			return false;
		if (off + len > x->disc->size() || off + len < off)
			return false;
		memcpy(dst, &(*x->disc)[(size_t)off], len);
		return true;
	};
	readers.fat = [](const std::string &path, u64 src, u8 *dst, u32 len, void *c) -> bool {
		Ctx *x = (Ctx *)c;
		if (x->failFat)
			return false;
		std::map<std::string, std::vector<u8> >::iterator it = x->files->find(path);
		if (it == x->files->end() || src + len > it->second.size())
			return false;
		memcpy(dst, &it->second[(size_t)src], len);
		return true;
	};
	// Composed DOL: orig[0,0x100) ext[0x100,0x100@0) zero[0x200,0x100) orig[0x300,...).
	PlannedFile dol;
	dol.bootFile = true;
	dol.finalSize = (u32)kDolSize;
	{
		PlanSegment a;
		a.kind = PlanSegment::SEG_ORIGINAL;
		a.fileOffset = 0;
		a.length = 0x100;
		PlanSegment b;
		b.kind = PlanSegment::SEG_EXTERNAL;
		b.fileOffset = 0x100;
		b.length = 0x100;
		b.srcOffset = 0;
		b.external = "sd:/m/patch.bin";
		PlanSegment c;
		c.kind = PlanSegment::SEG_ZERO;
		c.fileOffset = 0x200;
		c.length = 0x100;
		PlanSegment d;
		d.kind = PlanSegment::SEG_ORIGINAL;
		d.fileOffset = 0x300;
		d.length = (u32)(kDolSize - 0x300);
		dol.segs.push_back(a);
		dol.segs.push_back(b);
		dol.segs.push_back(c);
		dol.segs.push_back(d);
	}

	// 7. Split read across ORIGINAL + EXTERNAL + ZERO + ORIGINAL.
	{
		BootView v;
		std::string why;
		CHECK(v.SetDol(kDolBase, kDolSize, dol, why));
		CHECK(v.HasDol());
		u8 buf[0x500];
		u32 done = 0;
		CHECK(v.ServeDol(kDolBase, buf, sizeof(buf), readers, &done));
		CHECK(done == sizeof(buf));
		CHECK(memcmp(buf, &disc[(size_t)kDolBase], 0x100) == 0); // stock head
		for (int i = 0; i < 0x100; ++i)
			if (buf[0x100 + i] != 0xEE) { CHECK(false); break; } // external
		for (int i = 0; i < 0x100; ++i)
			if (buf[0x200 + i] != 0) { CHECK(false); break; } // zeros
		CHECK(memcmp(buf + 0x300, &disc[(size_t)kDolBase + 0x300], 0x200) == 0);
	}

	// 8. Source-offset advancement: external run with nonzero srcOffset.
	// Probe only the in-file part ([0x100,0x1C0) <- src [0x40,0x100)).
	{
		PlannedFile d2 = dol;
		d2.segs[1].srcOffset = 0x40;
		BootView v;
		std::string why;
		CHECK(v.SetDol(kDolBase, kDolSize, d2, why));
		u8 buf2[0xC0];
		CHECK(v.ServeDol(kDolBase + 0x100, buf2, sizeof(buf2), readers, 0));
		CHECK(memcmp(buf2, &files["sd:/m/patch.bin"][0x40], 0xC0) == 0);
	}

	// 9. Failures: no readers, outside range, FAT error, stock error.
	// doneOut reports completed bytes; everything from done on is proven
	// untouched, so a caller discarding the buffer on false is safe.
	{
		BootView v;
		std::string why;
		CHECK(v.SetDol(kDolBase, kDolSize, dol, why));
		u8 buf[0x200];
		u32 done = 0xFFFF;
		BootReaders none;
		memset(buf, 0xCC, sizeof(buf));
		CHECK(!v.ServeDol(kDolBase, buf, sizeof(buf), none, &done)); // null readers
		CHECK(buf[0] == 0xCC && buf[sizeof(buf) - 1] == 0xCC);
		CHECK(!v.ServeDol(kDolBase - 0x100, buf, 0x100, readers, 0)); // before
		CHECK(!v.ServeDol(kDolBase + kDolSize - 0x100, buf, 0x200, readers, 0)); // past end
		CHECK(!v.ServeDol(kDolBase, buf, 0, readers, 0)); // zero length
		ctx.failFat = true;
		done = 0;
		memset(buf, 0xCC, sizeof(buf));
		CHECK(!v.ServeDol(kDolBase, buf, sizeof(buf), readers, &done));
		CHECK(done == 0x100); // original head completed, external failed
		for (size_t i = done; i < sizeof(buf); ++i)
		{
			if (buf[i] != 0xCC) { CHECK(false); break; } // tail untouched
		}
		ctx.failFat = false;
		ctx.failStock = true;
		done = 0;
		memset(buf, 0xCC, sizeof(buf));
		CHECK(!v.ServeDol(kDolBase, buf, 0x80, readers, &done));
		CHECK(done == 0);
		CHECK(buf[0] == 0xCC); // nothing completed, nothing written
		ctx.failStock = false;
	}

	// 10. SetDol refusals: non-boot plan, size mismatch, untiled segs,
	// header overlap, FST overlap, zero size.
	{
		BootView v;
		std::string why;
		PlannedFile notBoot;
		CHECK(!v.SetDol(kDolBase, kDolSize, notBoot, why));
		CHECK(!v.HasDol());
		PlannedFile wrongSize = dol;
		wrongSize.finalSize = (u32)kDolSize - 1;
		CHECK(!v.SetDol(kDolBase, kDolSize, wrongSize, why));
		PlannedFile gap = dol;
		gap.segs[1].fileOffset = 0x180; // hole
		CHECK(!v.SetDol(kDolBase, kDolSize, gap, why));
		CHECK(!v.SetDol(0x100, kDolSize, dol, why)); // header overlap
		CHECK(!v.SetDol(kDolBase, 0, dol, why)); // zero size
		std::vector<u8> fst(128, 0x11);
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v.ArmFst(kDolBase - 64, fst, (u32)fst.size(), why));
		CHECK(!v.SetDol(kDolBase, kDolSize, dol, why)); // FST overlap
		// And the reverse order: arming FST over an armed DOL refuses too.
		BootView v3;
		CHECK(v3.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v3.SetDol(kDolBase, kDolSize, dol, why));
		CHECK(!v3.ArmFst(kDolBase - 64, fst, (u32)fst.size(), why));
	}


	// 11. Route classification (shared with the production read path):
	// DOL touch -> DOL or FAIL; header/FST containment -> META; else STOCK.
	{
		BootView idle;
		CHECK(idle.Route(0, 16) == BootView::ROUTE_STOCK);
		CHECK(idle.Route(kDolBase, 16) == BootView::ROUTE_STOCK);
		std::vector<u8> fst(128, 0x11);
		BootView v;
		std::string why;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v.Route(0, 0) == BootView::ROUTE_STOCK); // zero length
		CHECK(v.Route(0, 16) == BootView::ROUTE_META); // header
		CHECK(v.Route(BOOTVIEW_HEADER_BYTES - 8, 16) == BootView::ROUTE_STOCK); // crossing
		CHECK(v.Route(0x5000, 16) == BootView::ROUTE_STOCK); // FST not armed
		CHECK(v.ArmFst(0x5000, fst, (u32)fst.size(), why));
		CHECK(v.Route(0x5000, 128) == BootView::ROUTE_META); // exact FST
		CHECK(v.Route(0x5000 + 64, 32) == BootView::ROUTE_META); // sub-read
		CHECK(v.Route(0x5000 + 120, 16) == BootView::ROUTE_STOCK); // past end
		CHECK(v.Route(0x5000 - 16, 32) == BootView::ROUTE_STOCK); // straddles start
		CHECK(v.Route(0xFFFFFFFFFFFFFFFFULL - 4, 16) == BootView::ROUTE_STOCK); // wrap
	}
	// 12. Route with DOL armed: DOL wins, straddles fail loudly.
	{
		const u64 base = 0x20000, size = 0x1000;
		PlannedFile dd;
		dd.bootFile = true;
		dd.finalSize = (u32)size;
		PlanSegment s;
		s.kind = PlanSegment::SEG_ORIGINAL;
		s.fileOffset = 0;
		s.length = (u32)size;
		dd.segs.push_back(s);
		BootView v;
		std::string why;
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, why));
		CHECK(v.SetDol(base, size, dd, why));
		CHECK(v.Route(base, 64) == BootView::ROUTE_DOL);
		CHECK(v.Route(base + size - 64, 64) == BootView::ROUTE_DOL);
		CHECK(v.Route(base - 64, 128) == BootView::ROUTE_FAIL); // straddles start
		CHECK(v.Route(base + size - 32, 64) == BootView::ROUTE_FAIL); // straddles end
		CHECK(v.Route(0, 16) == BootView::ROUTE_META); // header still served
		CHECK(v.Route(0x5000, 16) == BootView::ROUTE_STOCK); // elsewhere stock
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
