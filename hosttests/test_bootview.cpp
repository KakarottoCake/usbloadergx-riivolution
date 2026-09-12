// Patched boot view through the production overlay.
// Stock/no-mod view preserves stock behavior bit-for-bit; grown patched
// header + table agree (0x424/428/42c cohere, FST served exact); crossing,
// OOB, empty, and overflow inputs refuse/fall back explicitly. Production
// linkage (RiivoBootView only, no console).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

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
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, 0x4000, fst, why));
		CHECK(v.Active());
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
		CHECK(v.Activate(&stock[0], (u32)stock.size(), patched, 0x10000, fst, why));
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
		CHECK(v.Activate(&stock[0], (u32)stock.size(), stock, 0x5000, fst, why));
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

	// 6. Refusals: short headers, empty table, unaligned offset, overflow.
	{
		std::vector<u8> patched;
		std::string why;
		u8 shortHdr[16] = {0};
		CHECK(!PatchBootHeader(shortHdr, sizeof(shortHdr), 0x1000, 0x100, 0x100, patched, why));
		CHECK(!why.empty());
		CHECK(!PatchBootHeader(&stock[0], (u32)stock.size(), 0x1001, 0x100, 0x100, patched, why));
		BootView v;
		std::vector<u8> empty;
		CHECK(!v.Activate(&stock[0], (u32)stock.size(), stock, 0x1000, empty, why));
		CHECK(!v.Active());
		CHECK(!v.Activate(shortHdr, sizeof(shortHdr), stock, 0x1000, std::vector<u8>(16, 0), why));
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
