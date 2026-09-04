// Host tests for Riivo::PlanFragRegion.
//
// This decides where the mod's files live on the virtual disc the cIOS reads.
// Every failure mode here is silent on hardware - a file placed below the
// region start reads back as the game's own data, an unaligned one reads from
// the wrong sector, and one past the ceiling makes the read fail with an error
// the game reports as a bad disc. So the tests are mostly about refusing.
#include <stdio.h>
#include <string.h>
#include "riivo/RiivoFragPlan.hpp"
#include "riivo/RiivoDiPatch.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

//! A single-layer backup: 4.7 GB of virtual disc.
static const u64 DVD5_IMAGE = 4699979776ULL;
static const u32 SECTOR = 512;

//! Lay n files of `size` back to back from `start`, sector aligned.
static std::vector<ModExtent> Pack(u64 start, u32 size, int n, u32 sector)
{
	std::vector<ModExtent> v;
	u64 at = start;
	for (int i = 0; i < n; ++i) {
		ModExtent m;
		m.offset = at;
		m.length = size;
		v.push_back(m);
		at += (size + sector - 1) & ~(u64)(sector - 1);
	}
	return v;
}

int main()
{
	printf("1. the region clears both floors\n");
	{
		//! A small backup: the 4 GiB patch threshold is what binds.
		ck(PlanRegionStart(1024ULL * 1024 * 1024, SECTOR) == RIIVO_REGION_BYTES,
		   "a small backup still starts at 4 GiB");

		//! A normal single-layer backup: its own size is what binds.
		const u64 s = PlanRegionStart(DVD5_IMAGE, SECTOR);
		ck(s >= DVD5_IMAGE, "starts at or above the end of the backup");
		ck(s >= RIIVO_REGION_BYTES, "and still above the patch threshold");
		ck((s % SECTOR) == 0, "sector aligned");
		printf("   region starts at 0x%llx (%.2f GB)\n",
			   (unsigned long long) s, s / 1e9);
	}

	printf("2. a realistic mod is accepted\n");
	{
		const u64 start = PlanRegionStart(DVD5_IMAGE, SECTOR);
		//! 2148 files averaging 512 KB - roughly the measured Spectral shape.
		std::vector<ModExtent> mods = Pack(start, 512 * 1024, 2148, SECTOR);
		FragPlan p = PlanFragRegion(DVD5_IMAGE, SECTOR, 300, mods);
		ck(p.ok, "accepted");
		ck(p.files == 2148, "file count");
		ck(p.minFragments == 2148, "one fragment per file at best");
		ck(p.fragsAvailable > 2148, "the table has room");
		ck(p.regionEnd <= RIIVO_READ_CEILING, "ends below the ceiling");
		ck(p.ceilingSpare > 0, "with room to spare");
		printf("   %u files, %.2f GB payload, %.2f GB spare below the ceiling\n",
			   p.files, p.payloadBytes / 1e9, p.ceilingSpare / 1e9);
	}

	printf("3. a dual-layer backup leaves nowhere to put anything\n");
	{
		//! A DVD9 image already reaches the ceiling.
		const u64 dvd9 = RIIVO_READ_CEILING;
		std::vector<ModExtent> mods = Pack(PlanRegionStart(dvd9, SECTOR), 4096, 4, SECTOR);
		FragPlan p = PlanFragRegion(dvd9, SECTOR, 300, mods);
		ck(!p.ok, "refused");
		ck(p.why.find("dual-layer") != std::string::npos, "and says why");
	}

	printf("4. placement mistakes are caught, not shipped\n");
	{
		const u64 start = PlanRegionStart(DVD5_IMAGE, SECTOR);

		//! Below the region: the game's own fragments would shadow it.
		std::vector<ModExtent> low = Pack(start, 4096, 2, SECTOR);
		low[0].offset = start - SECTOR;
		ck(!PlanFragRegion(DVD5_IMAGE, SECTOR, 0, low).ok, "a file below the region");

		//! Not sector aligned: a fragment cannot start mid-sector.
		std::vector<ModExtent> odd = Pack(start, 4096, 2, SECTOR);
		odd[1].offset += 1;
		ck(!PlanFragRegion(DVD5_IMAGE, SECTOR, 0, odd).ok, "an unaligned file");

		//! Overlapping, which would corrupt the cIOS's sorted lookup index.
		std::vector<ModExtent> over = Pack(start, 4096, 3, SECTOR);
		over[2].offset = over[1].offset;
		ck(!PlanFragRegion(DVD5_IMAGE, SECTOR, 0, over).ok, "overlapping files");

		//! Descending, same reason.
		std::vector<ModExtent> desc = Pack(start, 4096, 3, SECTOR);
		u64 t = desc[1].offset; desc[1].offset = desc[2].offset; desc[2].offset = t;
		ck(!PlanFragRegion(DVD5_IMAGE, SECTOR, 0, desc).ok, "out of order files");

		ck(!PlanFragRegion(DVD5_IMAGE, SECTOR, 0, std::vector<ModExtent>()).ok,
		   "no files at all");
		ck(!PlanFragRegion(DVD5_IMAGE, 500, 0, Pack(start, 4096, 2, SECTOR)).ok,
		   "a sector size that is not a power of two");
		ck(!PlanFragRegion(DVD5_IMAGE, 8192, 0, Pack(start, 4096, 2, SECTOR)).ok,
		   "an out-of-range sector size");
	}

	printf("5. the ceiling and the fragment table are both hard limits\n");
	{
		const u64 start = PlanRegionStart(DVD5_IMAGE, SECTOR);

		//! Enough payload to run past the read ceiling.
		const u64 room = RIIVO_READ_CEILING - start;
		const u32 big = 16 * 1024 * 1024;
		const int tooMany = (int) (room / big) + 8;
		FragPlan p = PlanFragRegion(DVD5_IMAGE, SECTOR, 0, Pack(start, big, tooMany, SECTOR));
		ck(!p.ok, "refused when the mod runs past the ceiling");
		ck(p.why.find("ceiling") != std::string::npos, "and says why");

		//! The game's own fragments leave too little of the table.
		FragPlan q = PlanFragRegion(DVD5_IMAGE, SECTOR, RIIVO_FRAG_MAX - 100,
									Pack(start, 4096, 500, SECTOR));
		ck(!q.ok, "refused when the fragment table is nearly full");
		ck(q.why.find("fragments") != std::string::npos, "and says why");

		//! A completely full table leaves nothing, without underflowing.
		FragPlan r = PlanFragRegion(DVD5_IMAGE, SECTOR, RIIVO_FRAG_MAX + 500,
									Pack(start, 4096, 2, SECTOR));
		ck(!r.ok, "refused when the table is already over full");
		ck(r.fragsAvailable == 0, "and the count did not wrap");
	}

	printf("6. a 4 KB sector drive works too\n");
	{
		const u32 sec = 4096;
		const u64 start = PlanRegionStart(DVD5_IMAGE, sec);
		ck((start % sec) == 0, "region aligned to the larger sector");
		FragPlan p = PlanFragRegion(DVD5_IMAGE, sec, 300, Pack(start, 100000, 64, sec));
		ck(p.ok, "accepted");
		ck((p.regionEnd % sec) == 0, "region end aligned");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
