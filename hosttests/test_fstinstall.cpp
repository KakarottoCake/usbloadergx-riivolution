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

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
