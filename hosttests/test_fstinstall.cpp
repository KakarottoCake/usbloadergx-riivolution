// Host tests for Riivo::PlaceFst.
//
// This is the arithmetic that decides where a rebuilt file table gets written
// into the running game's memory. Getting it wrong writes over the game and
// shows up as a hang with a black screen and nothing in any log, so the cases
// worth pinning are mostly the ones where it must REFUSE.
#include <stdio.h>
#include <string.h>
#include "riivo/RiivoFstInstall.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

//! A plausible post-apploader layout: 24 MB of MEM1, the table parked at the
//! top with 128 KB reserved, arena high just below it.
static ArenaInfo Typical()
{
	ArenaInfo a;
	a.arenaLo    = 0x80004000;
	a.fstMaxSize = 0x00020000;              // 128 KB reserved
	a.fstAddr    = MEM1_END - a.fstMaxSize; // 0x817E0000
	a.arenaHi    = a.fstAddr;
	return a;
}

int main()
{
	printf("1. a table that still fits does not move\n");
	{
		const ArenaInfo a = Typical();
		FstPlacement p = PlaceFst(a, 0x1B000, 32);
		ck(p.ok, "accepted");
		ck(p.inPlace, "reported as in-place");
		ck(p.fstAddr == a.fstAddr, "address unchanged");
		ck(p.newArenaHi == a.arenaHi, "arena untouched");
		ck(p.reserved == 0, "nothing taken from the heap");

		//! Exactly the reserved size is still a fit, not a grow.
		FstPlacement e = PlaceFst(a, a.fstMaxSize, 32);
		ck(e.ok && e.inPlace, "an exact fit is in-place");
	}

	printf("2. a bigger table extends downwards and pulls arena high with it\n");
	{
		const ArenaInfo a = Typical();
		const u32 want = 0x00050000; // 320 KB - 192 KB more than reserved
		FstPlacement p = PlaceFst(a, want, 32);
		ck(p.ok, "accepted");
		ck(!p.inPlace, "reported as grown");

		//! The top of the table stays put; only the bottom moves down.
		ck(p.fstAddr + want == a.fstAddr + a.fstMaxSize, "top edge unchanged");
		ck(p.fstAddr < a.fstAddr, "start moved down");
		ck((p.fstAddr & 31) == 0, "aligned");
		ck(p.newArenaHi == p.fstAddr, "arena high follows the table down");
		ck(p.reserved == a.arenaHi - p.fstAddr, "reserved is what the heap lost");
		ck(p.reserved == want - a.fstMaxSize, "and that is exactly the growth");
		ck(p.heapLeft == p.newArenaHi - a.arenaLo, "heap left adds up");
	}

	printf("3. alignment is respected and never rounds upward into the table\n");
	{
		const ArenaInfo a = Typical();
		//! An odd size, so the unaligned address would land mid-word.
		FstPlacement p = PlaceFst(a, 0x00030001, 32);
		ck(p.ok, "accepted");
		ck((p.fstAddr & 31) == 0, "32-byte aligned");
		//! Rounding DOWN is what keeps the table inside its own space: the end
		//! must still be at or below where the apploader's table ended.
		ck(p.fstAddr + 0x00030001 <= a.fstAddr + a.fstMaxSize, "end did not creep up");

		FstPlacement big = PlaceFst(a, 0x00030001, 0x800);
		ck(big.ok && (big.fstAddr & 0x7ff) == 0, "2 KB aligned");
	}

	printf("4. it refuses rather than guessing\n");
	{
		ArenaInfo a = Typical();

		ck(!PlaceFst(a, 0, 32).ok, "empty table");
		ck(!PlaceFst(a, 0x1000, 24).ok, "alignment not a power of two");

		//! The commonest real failure: called before the apploader has run, so
		//! the boot-info block is still all zeroes.
		ArenaInfo zero;
		FstPlacement z = PlaceFst(zero, 0x1000, 32);
		ck(!z.ok, "zeroed boot info");
		ck(z.why.find("apploader") != std::string::npos, "and says why");

		ArenaInfo bad = a;
		bad.fstAddr = 0x90000000; // MEM2, not MEM1
		ck(!PlaceFst(bad, 0x1000, 32).ok, "table outside MEM1");

		bad = a; bad.arenaLo = 0x10000000;
		ck(!PlaceFst(bad, 0x1000, 32).ok, "arena low outside MEM1");

		bad = a; bad.arenaHi = MEM1_END + 0x1000;
		ck(!PlaceFst(bad, 0x1000, 32).ok, "arena high past MEM1");

		bad = a; bad.arenaLo = a.arenaHi;
		ck(!PlaceFst(bad, 0x1000, 32).ok, "empty arena");

		bad = a; bad.arenaLo = a.arenaHi + 0x1000;
		ck(!PlaceFst(bad, 0x1000, 32).ok, "inverted arena");
	}

	printf("5. the game keeps a usable heap, or it is refused\n");
	{
		ArenaInfo a = Typical();

		//! Ask for a table so large the heap would drop under the 4 MB floor.
		const u32 heap = a.arenaHi - a.arenaLo;
		FstPlacement p = PlaceFst(a, a.fstMaxSize + heap - MIN_GAME_HEAP + 0x1000, 32);
		ck(!p.ok, "refused when the heap would fall below 4 MB");
		ck(p.why.find("heap") != std::string::npos, "and says why");

		//! Just inside the floor is still allowed.
		FstPlacement q = PlaceFst(a, a.fstMaxSize + heap - MIN_GAME_HEAP - 0x1000, 32);
		ck(q.ok, "accepted while the heap stays above 4 MB");
		ck(q.heapLeft >= MIN_GAME_HEAP, "and the heap really is above the floor");

		//! Absurd sizes must not wrap the address arithmetic around.
		ck(!PlaceFst(a, 0xFFFFFFF0u, 32).ok, "a 4 GB table is refused, not wrapped");
		ck(!PlaceFst(a, 0x02000000, 32).ok, "a table larger than MEM1 is refused");
	}

	printf("6. the real measured case from the console\n");
	{
		//! Super Mario Galaxy 2's table is 153792 bytes; the rebuild for Spectral
		//! adds 1881 entries plus their names. Take a generous 400 KB and check
		//! it is comfortably placeable on a normal layout.
		const ArenaInfo a = Typical();
		FstPlacement p = PlaceFst(a, 400 * 1024, 32);
		ck(p.ok, "a 400 KB rebuilt table is placeable");
		ck(!p.inPlace, "and it does have to grow");
		ck(p.heapLeft > 20 * 1024 * 1024, "the game still has over 20 MB of MEM1");
		printf("   takes %u KB from the heap, %u MB left\n",
			   p.reserved / 1024, p.heapLeft / (1024 * 1024));
	}

	printf("7. an apploader that leaves arena low at zero\n");
	{
		//! Straight off a tester's console: New Super Mario Bros. Wii (SMNE01)
		//! with the Newer mod. The apploader filled in everything except arena
		//! low, and treating that zero as "invalid" refused a placement that is
		//! in fact completely safe - the table only moves DOWN from arena high,
		//! into heap the game has not been handed yet.
		ArenaInfo a;
		a.arenaLo    = 0x00000000;
		a.arenaHi    = 0x817f74c0;
		a.fstAddr    = 0x817f74c0;
		a.fstMaxSize = 35628;

		FstPlacement p = PlaceFst(a, 62189, 32);
		ck(p.ok, "the real NSMBW layout is accepted");
		ck(!p.inPlace, "the table has to grow");
		ck(p.fstAddr < a.arenaHi, "and it grew downwards");
		ck(p.fstAddr >= MEM1_BASE, "still inside MEM1");
		ck(p.newArenaHi == p.fstAddr, "arena high follows it down");
		ck(a.arenaHi - p.newArenaHi < MAX_BLIND_DROP, "the drop stays within the blind cap");
		printf("   table at %08x, %u bytes taken from the top of the heap\n",
			   p.fstAddr, a.arenaHi - p.newArenaHi);

		//! Without a floor there is no way to prove a big table is safe, so a
		//! big one is still refused.
		FstPlacement big = PlaceFst(a, 35628 + MAX_BLIND_DROP + 0x1000, 32);
		ck(!big.ok, "a table needing more than the cap is refused");
		ck(big.why.find("arena low") != std::string::npos, "and says arena low is why");

		//! A known floor must still be honoured exactly as before.
		ArenaInfo known = a;
		known.arenaLo = 0x817f0000; // leaves far under 4 MB
		ck(!PlaceFst(known, 62189, 32).ok, "a known but tiny heap is still refused");
	}

	printf("8. half-open interval overlap for the evidence block\n");
	{
		ck(RangesOverlap(10, 20, 15, 25), "partial overlap");
		ck(RangesOverlap(10, 20, 10, 20), "identical intervals");
		ck(RangesOverlap(10, 30, 15, 20), "containment");
		ck(!RangesOverlap(10, 20, 20, 30), "touching edges do not overlap");
		ck(!RangesOverlap(10, 20, 0, 10), "touching edges, other side");
		ck(!RangesOverlap(10, 20, 30, 40), "disjoint intervals");
		ck(!RangesOverlap(10, 10, 5, 15), "empty first interval never overlaps");
		ck(!RangesOverlap(5, 15, 10, 10), "empty second interval never overlaps");
		ck(!RangesOverlap(20, 10, 0, 30), "inverted interval never overlaps");
		//! The T0 span against its own destination reads as overlap.
		ck(RangesOverlap(0x817da6a0, 0x817da740, 0x817da6a0, 0x817da6a0 + 153934),
		   "T0 span overlaps the T0 destination");
		//! A live stack fully above the span reads clear.
		ck(!RangesOverlap(0x817f0000, 0x817feff0, 0x817da6a0, 0x817da740),
		   "live stack fully above the span reads clear");
	}

	printf("9. grown tables move past apploader-loaded ranges (SB4E01 T0)\n");
	{
		//! Straight off the v3.35 T0 log: arena high and the table at
		//! 0x817da740 with 153792 bytes reserved, arena low unset, and an
		//! 8 KB apploader-loaded block [0x817d8740, 0x817da740) ending
		//! exactly where the table begins. A 153934-byte rebuild wants to
		//! start at 0x817da6a0 - inside that block's final 160 bytes.
		ArenaInfo a;
		a.arenaLo    = 0x00000000;
		a.arenaHi    = 0x817da740;
		a.fstAddr    = 0x817da740;
		a.fstMaxSize = 153792;
		const u32 want = 153934;
		const OccupiedRange block(0x817d8740, 0x817da740);

		//! Without the range, the old behaviour: planned overwrite.
		FstPlacement before = PlaceFst(a, want, 32);
		ck(before.ok && !before.inPlace, "T0 grows");
		ck(before.fstAddr == 0x817da6a0, "old placement starts at 0x817da6a0");
		ck(RangesOverlap(before.fstAddr, before.fstAddr + want,
						 block.lo, block.hi),
		   "and that overlaps the 8 KB block (the defect)");

		//! With the range, the table moves below it.
		FstPlacement p = PlaceFst(a, want, 32, &block, 1);
		ck(p.ok, "placement still possible");
		ck(!p.inPlace, "still a relocation");
		ck(p.fstAddr == 0x817b2de0, "table starts below the block");
		ck(p.newArenaHi == 0x817b2de0, "arena follows it down");
		ck(p.reserved == 0x27960, "heap cost is 162144 bytes");
		ck(p.ignoredRanges == 0, "the block itself is not ignored");
		ck(p.malformedRanges == 0, "nothing malformed");
		ck(!RangesOverlap(p.fstAddr, p.fstAddr + want, block.lo, block.hi),
		   "destination no longer touches the block");

		//! The stale table's own reservation is the expected overlap, not
		//! an obstacle: a range inside it changes nothing.
		const OccupiedRange stale(0x817da800, 0x817da900);
		FstPlacement q = PlaceFst(a, want, 32, &stale, 1);
		ck(q.ok && q.fstAddr == 0x817da6a0, "reservation overlap does not move the table");
		ck(q.ignoredRanges == 1, "and it is counted as ignored");

		//! A range straddling the reservation boundary is NOT just stale
		//! table: its part below the reservation is game image, so the
		//! table moves below all of it.
		const OccupiedRange span(0x817da700, 0x817da800);
		FstPlacement r = PlaceFst(a, want, 32, &span, 1);
		ck(r.ok && r.fstAddr == 0x817b4da0, "straddling range moves the table below it");
		ck(r.ignoredRanges == 0, "and is not counted as ignored");

		//! Touching is packing, not overlap: a range ending exactly where
		//! the destination begins changes nothing.
		const OccupiedRange abut(0x817d6740, 0x817da6a0);
		FstPlacement s = PlaceFst(a, want, 32, &abut, 1);
		ck(s.ok && s.fstAddr == 0x817da6a0, "abutting range does not move the table");

		//! Two obstacles: the lower one pulls the top below both at once.
		const OccupiedRange two[2] = { block, OccupiedRange(0x817c0000, 0x817c1000) };
		FstPlacement t = PlaceFst(a, want, 32, two, 2);
		ck(t.ok && t.fstAddr == 0x8179a6a0, "lower obstacle moves the table below both");

		//! In-place installs never consult the list: nothing moves, so
		//! nothing outside the reservation can be hit.
		FstPlacement u = PlaceFst(a, 1000, 32, &block, 1);
		ck(u.ok && u.inPlace && u.fstAddr == a.fstAddr, "in-place path ignores obstacles");

		//! Malformed entries refuse grown placement: steering around ranges
		//! that cannot be read is guessing. The counts still say what was
		//! seen, so the refusal names its cause instead of going anonymous.
		const OccupiedRange bad(0, 0);
		FstPlacement v = PlaceFst(a, want, 32, &bad, 1);
		ck(!v.ok, "malformed range refuses grown placement");
		ck(v.why.find("validation") != std::string::npos, "and says coverage is why");
		ck(v.malformedRanges == 1, "malformed count reported on refusal");
		ck(v.ignoredRanges == 0, "nothing counted as stale-table space");

		//! An in-place table never consults the list, so malformed entries
		//! cannot touch it - but they are still reported.
		FstPlacement w = PlaceFst(a, 1000, 32, &bad, 1);
		ck(w.ok && w.inPlace, "in-place install ignores a malformed list");
		ck(w.malformedRanges == 1, "still reported");

		//! BSS is occupancy like any loaded range: a BSS block the cascade
		//! would land in moves the table below it.
		const OccupiedRange tbb[2] = { block, OccupiedRange(0x817d0000, 0x817d2000) };
		FstPlacement x = PlaceFst(a, want, 32, tbb, 2);
		ck(x.ok && x.fstAddr == 0x817aa6a0, "BSS in the way moves the table below the BSS");

		//! BSS inside the stale reservation is expected overlap, like the
		//! table bytes themselves.
		const OccupiedRange bssIn(0x817db000, 0x817dc000);
		FstPlacement y = PlaceFst(a, want, 32, &bssIn, 1);
		ck(y.ok && y.fstAddr == 0x817da6a0, "BSS inside the reservation is ignored");
		ck(y.ignoredRanges == 1, "and counted");

		//! Capacity: far more ranges than any fixed buffer ever held are all
		//! processed - 150 valid ranges below plus the T0 block last.
		OccupiedRange many[151];
		for (u32 m = 0; m < 150; ++m)
			many[m] = OccupiedRange(0x80100000 + m * 0x10000,
									 0x80100000 + m * 0x10000 + 0x1000);
		many[150] = block;
		FstPlacement z = PlaceFst(a, want, 32, many, 151);
		ck(z.ok && z.fstAddr == 0x817b2de0, "a 151-entry list still moves past the block");
		ck(z.ignoredRanges == 0 && z.malformedRanges == 0, "with clean counts");

		//! No room anywhere below: refused, not wrapped or squeezed.
		const OccupiedRange all(MEM1_BASE, 0x817da740);
		ck(!PlaceFst(a, want, 32, &all, 1).ok, "a fully covered heap is refused");
	}

	printf("10. word-reference scan for the evidence block\n");
	{
		//! Aligned u32 backing throughout: the scanner skips unaligned head
		//! bytes, so plain byte buffers would make expectations depend on
		//! stack alignment. Buffers are filled with memcpy so writer and
		//! scanner agree on any host endianness.
		u32 wbuf[16] = { 0 };
		const u32 needle = 0x817da740;
		wbuf[3] = needle;   // bytes 12..15
		wbuf[10] = needle;  // bytes 40..43
		u32 hits[8] = { 0 };
		const u8 *buf = (const u8 *) wbuf;
		ck(FindWordRefs(buf, sizeof(wbuf), &needle, 1, hits, 8) == 2,
		   "two occurrences found");
		ck(hits[0] == 12 && hits[1] == 40, "offsets recorded");

		//! Base one past alignment: the head byte is skipped, words at
		//! relative 11 and 23 (absolute 12 and 24) still found.
		u32 words[8] = { 0 };
		words[3] = needle;
		words[6] = needle;
		ck(FindWordRefs((const u8 *) words + 1, 27, &needle, 1, hits, 8) == 2,
		   "unaligned base still finds aligned words");
		ck(hits[0] == 11 && hits[1] == 23, "offsets relative to the given base");

		//! A trailing partial word is not read.
		u8 tail[66] = { 0 };
		memcpy(tail + 12, &needle, 4);
		memcpy(tail + 62, &needle, 2);
		ck(FindWordRefs(tail, sizeof(tail), &needle, 1, hits, 8) == 1,
		   "partial tail word ignored");

		ck(FindWordRefs(buf, sizeof(wbuf), &needle, 0, hits, 8) == 0,
		   "no values means no hits");
		ck(FindWordRefs(0, sizeof(wbuf), &needle, 1, hits, 8) == 0,
		   "null base reads nothing");
		ck(FindWordRefs(buf, 0, &needle, 1, hits, 8) == 0,
		   "empty range reads nothing");

		const u32 multi[2] = { 0x817da740, 0x000258c0 };
		memcpy(tail + 4, &multi[1], 4);
		ck(FindWordRefs(tail, sizeof(tail), multi, 2, hits, 8) == 2,
		   "several values match");

		//! Twenty hits, room for five: returns 20, records the first 5.
		u32 manyw[24] = { 0 };
		for (u32 i = 0; i < 20; ++i)
			manyw[i] = needle;
		u32 few[5] = { 0 };
		ck(FindWordRefs((const u8 *) manyw, 80, &needle, 1, few, 5) == 20,
		   "total reported past the cap");
		ck(few[0] == 0 && few[4] == 16, "first hits recorded in order");
	}

	printf("11. experimental MEM2 placement is surveyed, capped and flat\n");
	{
		ArenaInfo a;
		a.arenaLo = 0;
		a.arenaHi = 0x817da740;
		a.fstAddr = 0x817da740;
		a.fstMaxSize = 153792;
		FstPlacement p = PlaceFstMem2(a, 153934, 32);
		ck(p.ok, "accepted");
		ck(!p.inPlace, "reported as grown");
		ck(p.fstAddr == 0x92000000, "surveyed base address");
		ck((p.fstAddr & 31) == 0, "aligned");
		ck(p.newArenaHi == a.arenaHi, "MEM1 arena untouched");
		ck(p.reserved == 0, "nothing taken from the MEM1 heap");
		ck(p.fstAddr + 153934 <= MEM2_END, "inside MEM2");

		//! Garbage arena in, refusal out - placing on unreadable words
		//! is guessing, even though MEM2 ignores the arena otherwise.
		ArenaInfo bad;
		bad.arenaHi = 0;
		bad.fstAddr = 0x817da740;
		FstPlacement q = PlaceFstMem2(bad, 153934, 32);
		ck(!q.ok, "garbage arena refused");

		//! The cap, the empty table and bad alignment all refuse.
		FstPlacement r = PlaceFstMem2(a, MEM2_FST_CAP + 1, 32);
		ck(!r.ok, "over-capacity refused");
		FstPlacement s = PlaceFstMem2(a, 0, 32);
		ck(!s.ok, "empty table refused");
		FstPlacement t = PlaceFstMem2(a, 153934, 24);
		ck(!t.ok, "non-power-of-two alignment refused");

		//! A MEM2 span is not MEM1: the MEM1 overlap test must not see it.
		ck(!RangesOverlap(p.fstAddr, p.fstAddr + 153934,
						  0x817da740, 0x81800000),
		   "MEM2 span disjoint from the MEM1 reservation");
	}

	printf("12. oversized grown table on SMNP01 hardware numbers\n");
	{
		//! Newer SMBW (SMNP01) grown rebuild, straight from a hardware log:
		//! reservation 36712 at 0x817f7080, arena high unset-low, want
		//! 63274. The loader planned 0x817e5940 there; pin it, plus the
		//! refusal shape around it. Compaction (60574) still overflows, so
		//! this workload keeps the grown path - the test guards the path,
		//! not the outcome.
		ArenaInfo a;
		a.arenaLo = 0;
		a.arenaHi = 0x817f7080;
		a.fstAddr = 0x817f7080;
		a.fstMaxSize = 36712;
		const u32 want = 63274;
		//! The log's 15 loaded ranges (dst,len pairs from the evidence
		//! block), BSS from the DOL header. Well-formed, none malformed,
		//! none fully inside the stale reservation.
		const OccupiedRange occ[] = {
			OccupiedRange(0x81201c60, 0x81201c80),
			OccupiedRange(0x81201dc0, 0x81201de0),
			OccupiedRange(0x817f5080, 0x817f7080),
			OccupiedRange(0x81201c80, 0x81201d80),
			OccupiedRange(0x80004000, 0x800066c0),
			OccupiedRange(0x80006780, 0x802edce0),
			OccupiedRange(0x800066c0, 0x80006720),
			OccupiedRange(0x80006720, 0x80006780),
			OccupiedRange(0x802edce0, 0x802edfc0),
			OccupiedRange(0x802edfc0, 0x802edfe0),
			OccupiedRange(0x802edfe0, 0x802fe6a0),
			OccupiedRange(0x802fe6a0, 0x80351980),
			OccupiedRange(0x80427980, 0x80429ea0),
			OccupiedRange(0x8042b360, 0x8042fec0),
			OccupiedRange(0x80351980, 0x8042ff1c), // BSS
			OccupiedRange(0x817f7080, 0x81800000), // reservation chunk, as logged
		};
		FstPlacement p = PlaceFst(a, want, 32, occ,
								  (u32)(sizeof(occ) / sizeof(occ[0])));
		ck(p.ok, "accepted");
		ck(!p.inPlace, "reported as grown");
		ck(p.fstAddr == 0x817e5940, "hardware destination reproduced");
		ck(p.newArenaHi == 0x817e5940, "arena follows down");
		ck(p.malformedRanges == 0, "no malformed ranges");
		ck(p.ignoredRanges == 0, "no stale-table overlap");
		ck(p.reserved == a.arenaHi - p.fstAddr, "reserved is the drop");
		//! Same layout, compacted size from the same log (60574): still
		//! grown, still the same mechanics - compaction helps, not saves.
		FstPlacement q = PlaceFst(a, 60574, 32, occ,
								  (u32)(sizeof(occ) / sizeof(occ[0])));
		ck(q.ok && !q.inPlace, "compacted still grown");
		ck(q.fstAddr == 0x817e63e0, "smaller drop lands higher");
	}

	printf("13. compacted T0 size installs in place (repair consequence)\n");
	{
		//! The production repair stages compacted bytes whenever plain
		//! overflows but compacted fits. For T0 that is 144323 bytes
		//! against the 153792 reservation - and the placement must then be
		//! in-place at the original address, never a cascade. Pins the
		//! hardware-verified outcome (log: STAGED compacted, in place).
		ArenaInfo a;
		a.arenaLo = 0;
		a.arenaHi = 0x817da740;
		a.fstAddr = 0x817da740;
		a.fstMaxSize = 153792;
		FstPlacement p = PlaceFst(a, 144323, 32, 0, 0);
		ck(p.ok && p.inPlace, "compacted size is in-place");
		ck(p.fstAddr == 0x817da740, "address unchanged");
		ck(p.newArenaHi == 0x817da740, "arena untouched");
		ck(p.reserved == 0, "nothing taken from the heap");
	}

	printf("14. Yoshi 2-byte-spare table installs in place (hardware pin)\n");
	{
		//! Yoshi-over-Mario on SB4E01 (hardware log, beta3 slot): 7
		//! replacements, no additions, plain table 153790 against the
		//! 153792 reservation. Two bytes spare is still in-place - this
		//! pins the exact boundary the passing hardware run took, so any
		//! future off-by-one in the fit check fails loudly here first.
		ArenaInfo a;
		a.arenaLo = 0;
		a.arenaHi = 0x817da740;
		a.fstAddr = 0x817da740;
		a.fstMaxSize = 153792;
		FstPlacement p = PlaceFst(a, 153790, 32, 0, 0);
		ck(p.ok && p.inPlace, "2-byte spare is in-place");
		ck(p.fstAddr == 0x817da740, "address unchanged");
		ck(p.newArenaHi == 0x817da740, "arena untouched");
		//! One byte over must grow, never squeeze in.
		FstPlacement q = PlaceFst(a, 153793, 32, 0, 0);
		ck(q.ok && !q.inPlace, "one byte over is grown");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
