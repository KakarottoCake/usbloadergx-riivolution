/****************************************************************************
 * The on-demand boot layout: one MEM2 reservation, split two ways.
 *
 * The module and the redirect table both have to survive the game's allocator,
 * and they come out of a single reservation because lowering the arena
 * boundary twice is a way to get the second one wrong - and getting it wrong
 * is silent. The game just allocates over whichever one was left outside.
 *
 * So the checks that matter here are the overlap ones: the module must not
 * run into the table, the table must not run past what was reserved, and the
 * module must stay cache-line aligned however the table is sized, because the
 * buffers the storage engines DMA into live inside it.
 ***************************************************************************/
#include <cstdio>
#include <string>

#include "riivo/RiivoOnDemand.hpp"
#include "riivo/RiivoModuleInstall.hpp"

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

static Riivo::Mem2Arena Typical()
{
	return Riivo::Mem2Arena(0x90002000, 0x933E0000);
}

static void TestLayout()
{
	const u32 tableLen = 168136;   // 2802 files, from test_redirtable
	Riivo::OnDemandLayout L;
	check(Riivo::PlanOnDemand(Typical(), tableLen, L), "the real case plans");
	check(L.why.empty(), "no reason given on success");

	const u32 mod = Riivo::ModuleFootprint();
	check(L.moduleAddr >= L.newArenaHi, "module sits at or above the new boundary");
	check(L.moduleAddr == L.newArenaHi, "and in fact right at it");
	check((L.moduleAddr & 31) == 0, "module is cache-line aligned");
	check(L.tableAddr > L.moduleAddr, "table comes after the module");
	check(L.tableAddr >= L.moduleAddr + mod, "and does not overlap it");
	check((u64) L.tableAddr + tableLen <= (u64) L.moduleAddr + L.reserved,
		  "table ends inside the reservation");
	check(L.tableLen == tableLen, "table length recorded");
	check(L.reserved >= mod + tableLen, "enough was reserved for both");
	check(L.newArenaHi < Typical().hi, "the boundary moved down");
	check(L.heapLeft == L.newArenaHi - Typical().lo, "heap left is consistent");

	std::printf("  module %08x (%u bytes), table %08x (%u bytes), "
				"arena2Hi %08x -> %08x, %.1f MB left\n",
				L.moduleAddr, (unsigned) mod, L.tableAddr, (unsigned) tableLen,
				Typical().hi, L.newArenaHi,
				(double) L.heapLeft / (1024.0 * 1024.0));
}

//! Whatever the table size, nothing may overlap and the module stays aligned.
//! An odd table length is the case that would push the next thing off a line
//! if the module were placed after it instead of before.
static void TestSizes()
{
	const u32 mod = Riivo::ModuleFootprint();
	bool allGood = true;
	for (u32 n = 1; n <= 300000; n += 4093)
	{
		Riivo::OnDemandLayout L;
		if (!Riivo::PlanOnDemand(Typical(), n, L))
		{
			allGood = false;
			std::printf("    table of %u bytes refused: %s\n",
						(unsigned) n, L.why.c_str());
			break;
		}
		if ((L.moduleAddr & 31) != 0
			|| L.tableAddr < L.moduleAddr + mod
			|| (u64) L.tableAddr + n > (u64) L.moduleAddr + L.reserved)
		{
			allGood = false;
			std::printf("    table of %u bytes laid out badly\n", (unsigned) n);
			break;
		}
		++g_checks;   // each size is a real check
	}
	check(allGood, "every table size from 1 to 300000 lays out cleanly");
}

static void TestRefusals()
{
	Riivo::OnDemandLayout L;
	check(!Riivo::PlanOnDemand(Typical(), 0, L), "refuses an empty table");
	check(!L.why.empty(), "and says why");

	//! An arena too small for both. The module alone is ~10 KB, so this is
	//! about the floor ReserveMem2 enforces rather than the raw size.
	check(!Riivo::PlanOnDemand(Riivo::Mem2Arena(0x90002000, 0x90802000),
							   168136, L),
		  "refuses when the game would be left under its MEM2 floor");

	check(!Riivo::PlanOnDemand(Riivo::Mem2Arena(0, 0), 4096, L),
		  "refuses an arena that is not one");
	check(!Riivo::PlanOnDemand(Riivo::Mem2Arena(0x933F0000, 0x933E0000), 4096, L),
		  "refuses an inverted arena");

	//! A table so large the reservation is implausible. ReserveMem2 caps this,
	//! and the cap is what stops a miscomputed count eating MEM2.
	check(!Riivo::PlanOnDemand(Typical(), 0xF0000000u, L),
		  "refuses an implausibly large table");
	check(!L.ok, "and leaves the layout unusable");
}

//! Two reservations composed: the FST already takes MEM1, and a second
//! MEM2 request after this one must still land below what we took.
static void TestComposes()
{
	Riivo::OnDemandLayout L;
	check(Riivo::PlanOnDemand(Typical(), 4096, L), "plan once");

	Riivo::Mem2Arena after(Typical().lo, L.newArenaHi);
	Riivo::Mem2Reservation r = Riivo::ReserveMem2(after, 8192, 32);
	check(r.ok, "a later reservation still succeeds");
	check(r.addr + 8192 <= L.moduleAddr,
		  "and lands below everything the on-demand path took");
}

//! The install is target-only and must say so rather than half-run.
static void TestInstallIsTargetOnly()
{
	Riivo::OnDemandLayout L;
	std::string why;
	std::vector<u8> table(64, 0);
	check(!Riivo::InstallOnDemand(0x93800000, table, 0, L, why),
		  "install refuses on the host");
	check(!why.empty(), "and says why");
}

int main()
{
	TestLayout();
	TestSizes();
	TestRefusals();
	TestComposes();
	TestInstallIsTargetOnly();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
