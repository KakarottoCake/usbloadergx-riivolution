// Host tests for Riivo::PlanFragRegion.
//
// This decides where the mod's files live on the virtual disc the cIOS reads.
// Every failure mode here is silent on hardware - a file placed below the
// region start reads back as the game's own data, an unaligned one reads from
// the wrong sector, and one past the ceiling makes the read fail with an error
// the game reports as a bad disc. So the tests are mostly about refusing.
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "riivo/RiivoFragPlan.hpp"
#include "riivo/RiivoDiPatch.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

//! A single-layer backup: 4.7 GB of virtual disc. Note this is slightly ABOVE
//! the cIOS's own single-layer read limit - a real disc's last megabyte is
//! padding and is never read.
static const u64 DVD5_IMAGE = 4699979776ULL;

//! Where the data on a real backup actually ends, measured on the tester's
//! console. This, not the declared size, is the floor the mod has to clear:
//! __Frag_Get returns the first fragment covering an offset, so only offsets
//! the game genuinely maps would shadow the mod.
static const u64 DATA_END = 0x00ff7bffe0ULL;

static const u32 SECTOR = 512;

//! Lay n files of `size` back to back from `start`, sector aligned.
static bool ByOffset(const ModExtent &a, const ModExtent &b)
{
	return a.offset < b.offset;
}

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
		const u64 start = PlanRegionStart(DATA_END, SECTOR);
		//! 700 files averaging 512 KB - 367 MB, which is about as much as the
		//! room below the single-layer limit holds.
		std::vector<ModExtent> mods = Pack(start, 512 * 1024, 700, SECTOR);
		FragPlan p = PlanFragRegion(DATA_END, SECTOR, 300, mods);
		ck(p.ok, "accepted");
		ck(p.files == 700, "file count");
		ck(p.minFragments == 700, "one fragment per file at best");
		ck(p.fragsAvailable > 700, "the table has room");
		ck(p.regionEnd <= RIIVO_DVD5_CEILING, "ends below the single-layer limit");
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
		const u64 start = PlanRegionStart(DATA_END, SECTOR);

		//! Below the region: the game's own fragments would shadow it.
		std::vector<ModExtent> low = Pack(start, 4096, 2, SECTOR);
		low[0].offset = start - SECTOR;
		ck(!PlanFragRegion(DATA_END, SECTOR, 0, low).ok, "a file below the region");

		//! Not sector aligned: a fragment cannot start mid-sector.
		std::vector<ModExtent> odd = Pack(start, 4096, 2, SECTOR);
		odd[1].offset += 1;
		ck(!PlanFragRegion(DATA_END, SECTOR, 0, odd).ok, "an unaligned file");

		//! Overlapping, which would corrupt the cIOS's sorted lookup index.
		std::vector<ModExtent> over = Pack(start, 4096, 3, SECTOR);
		over[2].offset = over[1].offset;
		ck(!PlanFragRegion(DATA_END, SECTOR, 0, over).ok, "overlapping files");

		//! Descending, same reason.
		std::vector<ModExtent> desc = Pack(start, 4096, 3, SECTOR);
		u64 t = desc[1].offset; desc[1].offset = desc[2].offset; desc[2].offset = t;
		ck(!PlanFragRegion(DATA_END, SECTOR, 0, desc).ok, "out of order files");

		ck(!PlanFragRegion(DATA_END, SECTOR, 0, std::vector<ModExtent>()).ok,
		   "no files at all");
		ck(!PlanFragRegion(DATA_END, 500, 0, Pack(start, 4096, 2, SECTOR)).ok,
		   "a sector size that is not a power of two");
		ck(!PlanFragRegion(DATA_END, 8192, 0, Pack(start, 4096, 2, SECTOR)).ok,
		   "an out-of-range sector size");
	}

	printf("5. the ceiling and the fragment table are both hard limits\n");
	{
		const u64 start = PlanRegionStart(DATA_END, SECTOR);

		//! Enough payload to run past the single-layer limit.
		const u64 room = RIIVO_DVD5_CEILING - start;
		const u32 big = 16 * 1024 * 1024;
		const int tooMany = (int) (room / big) + 8;
		FragPlan p = PlanFragRegion(DATA_END, SECTOR, 0, Pack(start, big, tooMany, SECTOR));
		ck(!p.ok, "refused when the mod runs past the limit");
		ck(p.why.find("single-layer") != std::string::npos, "and says why");

		//! The game's own fragments leave too little of the table.
		FragPlan q = PlanFragRegion(DATA_END, SECTOR, RIIVO_FRAG_MAX - 100,
									Pack(start, 4096, 500, SECTOR));
		ck(!q.ok, "refused when the fragment table is nearly full");
		ck(q.why.find("fragments") != std::string::npos, "and says why");

		//! A completely full table leaves nothing, without underflowing.
		FragPlan r = PlanFragRegion(DATA_END, SECTOR, RIIVO_FRAG_MAX + 500,
									Pack(start, 4096, 2, SECTOR));
		ck(!r.ok, "refused when the table is already over full");
		ck(r.fragsAvailable == 0, "and the count did not wrap");
	}

	printf("6. a 4 KB sector drive works too\n");
	{
		const u32 sec = 4096;
		const u64 start = PlanRegionStart(DATA_END, sec);
		ck((start % sec) == 0, "region aligned to the larger sector");
		FragPlan p = PlanFragRegion(DATA_END, sec, 300, Pack(start, 100000, 64, sec));
		ck(p.ok, "accepted");
		ck((p.regionEnd % sec) == 0, "region end aligned");
	}

	printf("7. the declared size is never mistaken for the floor\n");
	{
		//! The regression this pins down. An earlier version raised the fragment
		//! list's declared size to make room for the mod, then read that same
		//! field back and called it "the backup's size" - so every game looked
		//! like it filled the address space and every game was refused as
		//! dual-layer. Nothing inflates the field any more, but the shape of the
		//! mistake is worth keeping: an inflated figure must still be refused,
		//! and the real one must still work.
		const u64 inflated = RIIVO_READ_CEILING;
		std::vector<ModExtent> mods =
			Pack(PlanRegionStart(inflated, SECTOR), 4096, 4, SECTOR);
		ck(!PlanFragRegion(inflated, SECTOR, 3, mods).ok,
		   "an inflated figure refuses (this was the bug)");

		//! And the floor that is actually used is where the data ends, which on
		//! both tester backups is below the 4 GiB line - so the whole gap between
		//! there and the single-layer limit is available.
		const u64 start = PlanRegionStart(DATA_END, SECTOR);
		FragPlan right = PlanFragRegion(DATA_END, SECTOR, 3,
										Pack(start, 602753, 600, SECTOR));
		ck(right.ok, "a 360 MB mod fits above the data end");
		ck(right.regionStart >= DATA_END, "region clears the game's data");
		ck(right.regionStart >= RIIVO_REGION_BYTES, "and clears the 4 GiB line");
		ck(right.files == 600, "all 600 files placed");
		printf("   region at 0x%llx, %llu bytes spare below the limit\n",
			   (unsigned long long) right.regionStart,
			   (unsigned long long) right.ceilingSpare);
	}

	printf("8. a disc file claimed twice must not become two extents\n");
	{
		//! Newer SMBW carries 38 <folder> rules and some of them overlap, so
		//! the same disc path is claimed more than once. Both claims resolve to
		//! the same assigned offset, and emitting both produced two extents at
		//! the same place - refused as "two files overlap". CollectPlaced now
		//! keys on the disc path so only one survives; this pins down what the
		//! planner does with the two shapes either side of that fix.
		const u64 start = PlanRegionStart(DATA_END, SECTOR);

		std::vector<ModExtent> dup = Pack(start, 4096, 3, SECTOR);
		dup.push_back(dup[1]);                       // the same file claimed twice
		std::sort(dup.begin(), dup.end(), ByOffset);
		ck(!PlanFragRegion(DATA_END, SECTOR, 3, dup).ok,
		   "a duplicated extent is still refused (the symptom)");

		std::vector<ModExtent> once = Pack(start, 4096, 3, SECTOR);
		FragPlan p = PlanFragRegion(DATA_END, SECTOR, 3, once);
		ck(p.ok, "the de-duplicated form is accepted (the fix)");
		ck(p.files == 3, "and keeps one extent per disc file");
	}

	printf("9. how much room there actually is, and what will not fit\n");
	{
		//! Error #001. A Wii game checks that a read past the end of the disc
		//! FAILS; one that returns zeros instead is how it concludes it is
		//! running from a copy. The SDK reads one sector just past the
		//! single-layer end and requires that error - so the disc has to stay
		//! single-layer, and the mod has to fit under that line. There is no
		//! version of "enlarge the disc a bit" that survives the check.
		const u64 declared = 4685037568ULL;
		ck(DATA_END < RIIVO_REGION_BYTES, "the game's data ends below the 4 GiB line");

		const u64 start = PlanRegionStart(DATA_END, SECTOR);
		ck(start == RIIVO_REGION_BYTES, "so the mod starts exactly at 4 GiB");
		ck(start < declared, "which is inside the disc the backup already declares");

		//! That leaves this much, and it is the whole budget.
		const u64 room = RIIVO_DVD5_CEILING - start;
		ck(room > 400000000ULL && room < 410000000ULL, "about 385 MB of room");
		printf("   room for a mod: %llu bytes (%.0f MB)\n",
			   (unsigned long long) room, room / 1048576.0);

		//! Super Mario Gravity: 8 MB. Comfortably inside.
		FragPlan small = PlanFragRegion(DATA_END, SECTOR, 3, Pack(start, 52863, 151, SECTOR));
		ck(small.ok, "an 8 MB mod is placeable");
		ck(small.regionEnd <= declared, "entirely inside the declared disc");
		ck(small.regionEnd <= RIIVO_DVD5_CEILING, "and under the single-layer limit");

		//! Newer SMBW: 645 MB. Does not fit, and has to be refused here rather
		//! than discovered as an anti-piracy screen on the console.
		FragPlan big = PlanFragRegion(DATA_END, SECTOR, 3, Pack(start, 602753, 1067, SECTOR));
		ck(!big.ok, "a 645 MB mod is refused, not squeezed in");
		ck(big.why.find("single-layer") != std::string::npos, "and says why");
		printf("   645 MB mod: %s\n", big.why.c_str());

		//! Using the declared size as the floor - what it did before - pushes
		//! even the 8 MB mod above the whole disc, which is above the limit.
		ck(PlanRegionStart(declared, SECTOR) >= declared,
		   "the old floor put the mod above the disc (this was the cause)");
		ck(RIIVO_DVD5_CEILING - PlanRegionStart(declared, SECTOR) < 16 * 1024 * 1024,
		   "leaving under 16 MB instead of 386");
		//! A 100 MB mod: fine from the data end, impossible from the declared one.
		ck(PlanFragRegion(DATA_END, SECTOR, 3, Pack(start, 524288, 200, SECTOR)).ok,
		   "a 100 MB mod fits above the data end");
		ck(!PlanFragRegion(declared, SECTOR, 3,
						   Pack(PlanRegionStart(declared, SECTOR), 524288, 200, SECTOR)).ok,
		   "but not above the declared end");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
