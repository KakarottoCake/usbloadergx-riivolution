/****************************************************************************
 * Reserving MEM2 for the redirect table.
 *
 * The failure this guards against is quiet: hand the game memory it thinks it
 * owns, and the table is intact right up until the game's heap grows into it,
 * so the mod works and then corrupts. There is no console-side symptom that
 * points back here. So the interesting cases are the refusals - a low-memory
 * block that does not look like an arena must be left alone rather than
 * lowered by a guess.
 ***************************************************************************/
#include <cstdio>
#include <string>

#include "riivo/RiivoMem2Reserve.hpp"

static int g_checks = 0;
static int g_fail = 0;

static void check(bool cond, const char *what)
{
	++g_checks;
	if (!cond)
	{
		++g_fail;
		std::printf("FAIL: %s\n", what);
	}
}

//! A plausible retail arena: MEM2 heap from just above the IPC page to the
//! IOS boundary a cIOS leaves behind.
static Riivo::Mem2Arena Typical()
{
	return Riivo::Mem2Arena(0x90002000, 0x933E0000);
}

static void TestPlacement()
{
	Riivo::Mem2Reservation r = Riivo::ReserveMem2(Typical(), 168136, 32);
	check(r.ok, "a 168 KB table is reserved from a typical arena");
	check(r.why.empty(), "no reason given on success");

	check(r.addr == r.newArenaHi, "the table sits exactly at the new boundary");
	check(r.newArenaHi < Typical().hi, "the boundary moved down");
	check(r.newArenaHi > Typical().lo, "and stayed above arena low");
	check((r.newArenaHi & 31) == 0, "the boundary is cache-line aligned");
	check(r.reserved >= 168136, "at least the requested size was taken");
	check(r.reserved < 168136 + 32, "and no more than alignment requires");

	//! The gap must actually hold the table: this is the property that
	//! matters, and it is the one an off-by-one in either direction breaks.
	check((u64) r.addr + 168136 <= Typical().hi,
		  "the table fits between the new boundary and the old one");
	check(r.heapLeft == r.newArenaHi - Typical().lo, "heap left is consistent");

	//! Reserving twice from the already-reduced arena must compose, since the
	//! FST reservation on MEM1 works the same way and both may be wanted.
	Riivo::Mem2Arena after(Typical().lo, r.newArenaHi);
	Riivo::Mem2Reservation r2 = Riivo::ReserveMem2(after, 4096, 32);
	check(r2.ok, "a second reservation from the reduced arena");
	check(r2.addr + 4096 <= r.addr, "and it does not overlap the first");
}

static void TestAlignment()
{
	//! A coarser alignment is allowed; the reservation just costs more.
	Riivo::Mem2Reservation r = Riivo::ReserveMem2(Typical(), 100, 0x8000);
	check(r.ok, "32 KB alignment is accepted");
	check((r.addr & 0x7FFF) == 0, "and honoured");
	check(r.reserved == 0x8000, "a 100-byte table costs one aligned block");

	std::string whys[3];
	Riivo::Mem2Reservation a = Riivo::ReserveMem2(Typical(), 100, 16);
	check(!a.ok, "refuses alignment finer than a cache line");
	Riivo::Mem2Reservation b = Riivo::ReserveMem2(Typical(), 100, 48);
	check(!b.ok, "refuses a non-power-of-two alignment");
	Riivo::Mem2Reservation c = Riivo::ReserveMem2(Typical(), 100, 0);
	check(!c.ok, "refuses zero alignment");
	(void) whys;
}

static void TestRefusals()
{
	//! Nothing to do.
	check(!Riivo::ReserveMem2(Typical(), 0, 32).ok, "refuses a zero-byte table");

	//! A count that went wrong upstream should not quietly eat MEM2.
	check(!Riivo::ReserveMem2(Typical(), 16 * 1024 * 1024, 32).ok,
		  "refuses an implausibly large reservation");

	//! Low memory that does not look like an arena. Lowering a word we do not
	//! understand is how the running game gets overwritten.
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0, 0), 4096, 32).ok,
		  "refuses an all-zero arena");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x80003134), 4096, 32).ok,
		  "refuses an arena high below MEM2 - a MEM1 address by mistake");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x95000000), 4096, 32).ok,
		  "refuses an arena high past the top of MEM2");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x80000030, 0x933E0000), 4096, 32).ok,
		  "refuses an arena low outside MEM2");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x933E0000, 0x933E0000), 4096, 32).ok,
		  "refuses an empty arena");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x933F0000, 0x933E0000), 4096, 32).ok,
		  "refuses an inverted arena");
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x933E0010), 4096, 32).ok,
		  "refuses an unaligned arena high");

	//! The game must be left something to run in. An 8 MB arena minus even a
	//! single page falls under the floor; a 10 MB one does not, and is
	//! checked here too so the case is pinned from both sides.
	check(!Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x90802000), 4096, 32).ok,
		  "refuses when the game would be left under 8 MB");
	check(Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x90A02000), 4096, 32).ok,
		  "accepts when 10 MB leaves the game over the floor");

	Riivo::Mem2Reservation why = Riivo::ReserveMem2(Typical(), 0, 32);
	check(!why.why.empty(), "a refusal always says why");
}

//! The boundary between accepted and refused, checked from both sides rather
//! than assumed to be where the constant says.
static void TestHeapFloor()
{
	const u32 lo = 0x90002000;
	const u32 need = Riivo::MIN_GAME_MEM2;

	//! An arena with exactly MIN_GAME_MEM2 + 4096 usable: taking 4096 leaves
	//! exactly the floor, which is allowed.
	Riivo::Mem2Arena tight(lo, lo + need + 4096);
	Riivo::Mem2Reservation ok = Riivo::ReserveMem2(tight, 4096, 32);
	check(ok.ok, "leaving exactly the floor is allowed");
	check(ok.heapLeft == need, "and leaves exactly the floor");

	//! One cache line less, and it must refuse.
	Riivo::Mem2Arena tighter(lo, lo + need + 4096 - 32);
	check(!Riivo::ReserveMem2(tighter, 4096, 32).ok,
		  "one cache line under the floor is refused");
}

//! What the loader will actually ask for, at the size that motivated all this.
static void TestRealCase()
{
	const u32 tableBytes = 168136;   // 2802 files, from test_redirtable
	Riivo::Mem2Reservation r = Riivo::ReserveMem2(Typical(), tableBytes, 32);
	check(r.ok, "the real table reserves");
	std::printf("  2802-file table: %u bytes at %08x, arena2Hi %08x -> %08x, "
				"%.1f MB left to the game\n",
				(unsigned) r.reserved, r.addr, Typical().hi, r.newArenaHi,
				(double) r.heapLeft / (1024.0 * 1024.0));
	check(r.heapLeft > 50 * 1024 * 1024 / 2,
		  "the game keeps the great majority of MEM2");
}

int main()
{
	TestPlacement();
	TestAlignment();
	TestRefusals();
	TestHeapFloor();
	TestRealCase();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
