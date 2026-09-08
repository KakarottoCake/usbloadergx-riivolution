// Host integration fixture: planning through installation into simulated
// memory, driven by the captured SB4E01 T0 layout.
//
// WHAT THIS VALIDATES (and nothing else):
// - PlaceFst (the real function) against the captured arena, obstacles,
//   BSS and reservation, including the exact T0 addresses.
// - The install decision sequence mirrored from InstallPendingFst: stage
//   checksum, bounds re-check, copy, low-memory word updates, byte + CRC
//   + pointer re-verification, refuse/continue outcome - using the REAL
//   Crc32 both sides stage and verify, so a checksum mismatch here means
//   the algorithm disagrees with itself, not that two implementations do.
// - Protected ranges (game image, BSS, stale reservation outside the
//   destination, live-stack zone) come back byte-identical; the
//   destination comes back byte-equal to what was staged; the four
//   boot-info words come back exactly as booked.
// - A builder-built table (real FstBuilder path) installs the same way.
// - A live stack overlapping the destination is DETECTED by the
//   StackHitsDest check. The target does NOT refuse on it today - that
//   fork is documented below, not implemented here.
//
// WHAT STILL REQUIRES WII HARDWARE, explicitly:
// - Real addresses (this MEM1 is a byte array; no MMU, BATs, or MEM1/MEM2
//   split), real cache behavior (DCFlushRange is a no-op here by
//   omission - the sequence point is preserved, the coherency it buys is
//   not tested), real IOS/cIOS (no reads served, no hook, no fragments).
// - The real apploader layout (simulated from log values), real thread
//   and stack reality (SP/stack-top arrive as logged inputs, not live
//   registers), timing and interrupt behavior.
// - Game consumption: nothing here executes game code. A green fixture
//   means the handoff is bit-exact; whether the game boots on it is the
//   T0 acceptance run, not this file.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "riivo/RiivoFstInstall.hpp"
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoReconcile.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

//! Simulated MEM1, zero-filled like RAM after a clean boot stage. All
//! addresses below are MEM1 addresses; subtract MEM1_BASE for the index.
static u8 simMem[0x1800000];

static u32 simGet32(u32 addr)
{
	const u32 o = addr - MEM1_BASE;
	return ((u32) simMem[o] << 24) | ((u32) simMem[o + 1] << 16) |
		   ((u32) simMem[o + 2] << 8) | simMem[o + 3];
}

static void simSet32(u32 addr, u32 v)
{
	const u32 o = addr - MEM1_BASE;
	simMem[o] = (u8) (v >> 24);
	simMem[o + 1] = (u8) (v >> 16);
	simMem[o + 2] = (u8) (v >> 8);
	simMem[o + 3] = (u8) v;
}

//! Paint [lo,hi) with one byte so any stray write shows up. Zero length
//! paints nothing and is always intact.
static void simProtect(u32 lo, u32 hi, u8 pat)
{
	if (hi <= lo || lo < MEM1_BASE || hi > MEM1_END)
		return;
	memset(&simMem[lo - MEM1_BASE], pat, hi - lo);
}

static bool simIntact(u32 lo, u32 hi, u8 pat)
{
	if (hi <= lo)
		return true;
	if (lo < MEM1_BASE || hi > MEM1_END)
		return false;
	for (u32 a = lo; a < hi; ++a)
		if (simMem[a - MEM1_BASE] != pat)
			return false;
	return true;
}

//! Install-failure codes mirroring InstallFailCode: 2 staged checksum,
//! 3 install bounds, 4 installed bytes/CRC, 5 low-memory pointers.
static bool SimInstall(const FstPlacement &place, const std::vector<u8> &staged,
					   u32 crcStaged, u32 &failCode)
{
	failCode = 0;
	if (!place.ok)
		return false; // refused placement installs nothing, like the target
	if (staged.empty() || place.fstAddr == 0)
		return true; // nothing staged: nothing to do, like the target
	if (Crc32(&staged[0], (u32) staged.size()) != crcStaged)
	{
		failCode = 2;
		return false;
	}
	const u32 addr = place.fstAddr;
	const u32 size = (u32) staged.size();
	if (addr < MEM1_BASE || addr >= MEM1_END
		|| (u64) addr + size > MEM1_END)
	{
		failCode = 3;
		return false;
	}
	memcpy(&simMem[addr - MEM1_BASE], &staged[0], size);
	//! Cache flush point (InstallFst DCFlushRange): sequence preserved,
	//! coherency itself not testable here.
	simSet32(0x80000038, addr);
	simSet32(0x8000003C, size);
	simSet32(0x80000034, place.newArenaHi);
	const bool bytesOk = memcmp(&simMem[addr - MEM1_BASE], &staged[0], size) == 0
		&& Crc32(&simMem[addr - MEM1_BASE], size) == crcStaged;
	const bool ptrsOk = simGet32(0x80000038) == addr
		&& simGet32(0x8000003C) == size
		&& simGet32(0x80000034) == place.newArenaHi;
	if (!bytesOk)
		failCode = 4;
	else if (!ptrsOk)
		failCode = 5;
	return bytesOk && ptrsOk;
}

//! Live-stack overlap detector for a planned destination. The target
//! computes no such check today: a TRUE here marks a destination the
//! current code would take blindly. Documented fork, not implemented
//! behavior: refuse grown tables on TRUE, or prove by measurement that
//! the live chain can never reach there.
static bool StackHitsDest(u32 sp, u32 stackTop, u32 destLo, u32 destHi)
{
	if (sp >= stackTop)
		return false; // empty (or inverted) live range
	return RangesOverlap(sp, stackTop, destLo, destHi);
}

//! Captured SB4E01 T0 layout, v3.35+ logs: arena high and table at
//! 0x817da740 with 153792 reserved, arena low unset, rebuilt 153934,
//! 8 KB block [0x817d8740, 0x817da740), BSS [0x80728680, 0x807e3188).
static ArenaInfo T0Arena()
{
	ArenaInfo a;
	a.arenaLo = 0x00000000;
	a.arenaHi = 0x817da740;
	a.fstAddr = 0x817da740;
	a.fstMaxSize = 153792;
	return a;
}

int main()
{
	printf("1. T0-exact install into simulated MEM1\n");
	{
		memset(simMem, 0, sizeof(simMem));
		const ArenaInfo a = T0Arena();
		const u32 want = 153934;
		const OccupiedRange occ[2] = {
			OccupiedRange(0x817d8740, 0x817da740), // the 8 KB block
			OccupiedRange(0x80728680, 0x807e3188), // BSS
		};
		FstPlacement p = PlaceFst(a, want, 32, occ, 2);
		ck(p.ok && !p.inPlace, "relocated placement accepted");
		ck(p.fstAddr == 0x817b2de0, "destination below the block");

		//! Staged table: exact byte count, patterned contents. Content is
		//! irrelevant to placement and copying; size and addresses are the
		//! captured quantities.
		std::vector<u8> staged(want);
		for (u32 i = 0; i < want; ++i)
			staged[i] = (u8) (0x5a + (i & 0x3f));
		const u32 crc = Crc32(&staged[0], want);

		//! Protected residents, each a distinct pattern: the stale
		//! reservation (entirely above the new destination here), the
		//! 8 KB block, BSS, a low game-image chunk, and the live stack
		//! zone (model-A-like: above the old reservation).
		simProtect(0x817da740, 0x81800000, 0xa5);
		simProtect(0x817d8740, 0x817da740, 0xb6);
		simProtect(0x80728680, 0x807e3188, 0xc7);
		simProtect(0x80004000, 0x80400000, 0xd8);
		simProtect(0x817fd000, 0x81800000, 0xe9);

		u32 fail = 0;
		ck(SimInstall(p, staged, crc, fail) && fail == 0, "install verifies");
		ck(simIntact(0x817da740, 0x817fd000, 0xa5),
		   "stale reservation untouched (below the stack zone)");
		ck(simIntact(0x817d8740, 0x817da740, 0xb6), "8 KB block untouched");
		ck(simIntact(0x80728680, 0x807e3188, 0xc7), "BSS untouched");
		ck(simIntact(0x80004000, 0x80400000, 0xd8), "low game image untouched");
		ck(simIntact(0x817fd000, 0x81800000, 0xe9), "live stack zone untouched");
		ck(memcmp(&simMem[p.fstAddr - MEM1_BASE], &staged[0], want) == 0,
		   "destination holds exactly the staged bytes");
		ck(simGet32(0x80000038) == 0x817b2de0, "FST address word repointed");
		ck(simGet32(0x8000003C) == want, "FST size word updated");
		ck(simGet32(0x80000034) == 0x817b2de0, "arena high lowered to the table");
	}

	printf("2. stack-overlap detector (documents the open fork)\n");
	{
		const u32 destLo = 0x817b2de0, destHi = 0x817b2de0 + 153934;
		ck(!StackHitsDest(0x817fd000, 0x81800000, destLo, destHi),
		   "stack fully above: clear");
		ck(StackHitsDest(0x817c0000, 0x817da740, destLo, destHi),
		   "stack reaching through: detected");
		ck(StackHitsDest(destLo, 0x817da740, destLo, destHi),
		   "SP exactly at destination start: detected");
		ck(!StackHitsDest(0x817da740, 0x817da740, destLo, destHi),
		   "empty live range: clear");
		ck(!StackHitsDest(0x81800000, 0x817da740, destLo, destHi),
		   "inverted live range: clear");
	}

	printf("3. in-place control installs inside the reservation\n");
	{
		memset(simMem, 0, sizeof(simMem));
		const ArenaInfo a = T0Arena();
		FstPlacement p = PlaceFst(a, 1000, 32);
		ck(p.ok && p.inPlace, "in place");
		std::vector<u8> staged(1000, 0x33);
		const u32 crc = Crc32(&staged[0], 1000);
		simProtect(0x817da740 + 1000, 0x81800000, 0xa5);
		simProtect(0x817d8740, 0x817da740, 0xb6);
		u32 fail = 0;
		ck(SimInstall(p, staged, crc, fail) && fail == 0, "install verifies");
		ck(simIntact(0x817da740 + 1000, 0x81800000, 0xa5),
		   "reservation past the table untouched");
		ck(simIntact(0x817d8740, 0x817da740, 0xb6), "block below untouched");
		ck(simGet32(0x80000038) == 0x817da740, "address word unchanged");
	}

	printf("4. refusal paths install nothing\n");
	{
		memset(simMem, 0, sizeof(simMem));
		const ArenaInfo a = T0Arena();
		simProtect(0x817da740, 0x81800000, 0xa5);
		FstPlacement p = PlaceFst(a, 0x02000000, 32);
		ck(!p.ok, "absurd size refused");
		u32 fail = 0;
		std::vector<u8> staged(16, 0x11);
		ck(!SimInstall(p, staged, Crc32(&staged[0], 16), fail),
		   "refused placement installs nothing");
		ck(fail == 0, "no check runs, so no check fails");
		ck(simIntact(0x817da740, 0x81800000, 0xa5), "nothing written");
	}

	printf("5. builder-built table through the same seam\n");
	{
		memset(simMem, 0, sizeof(simMem));
		FstBuilder b;
		bool isNew = false;
		ck(b.AddOrReplace("/Boot.bin", 0x20, &isNew), "base file 1");
		ck(b.AddOrReplace("/LayoutData/TitleLogo.arc", 125792, &isNew), "base file 2");
		ck(b.AddOrReplace("/GXDiagProbe/a0123456789.bin", 1, &isNew), "added file 1");
		ck(b.AddOrReplace("/GXDiagProbe/b0123456789.bin", 558, &isNew), "added file 2");
		std::vector<u8> staged;
		b.Serialize(staged, true);
		ck(!staged.empty(), "builder produced a table");
		const u32 want = (u32) staged.size();
		const u32 crc = Crc32(&staged[0], want);

		//! Synthetic arena sized around the synthetic table: reservation
		//! exactly the base size, so the additions force growth. Labeled
		//! synthetic - only the mechanism (builder to installer) is real.
		ArenaInfo a;
		a.arenaLo = 0x80004000;
		a.arenaHi = 0x817e0000;
		a.fstAddr = 0x817e0000;
		FstBuilder base;
		bool fresh = false;
		base.AddOrReplace("/Boot.bin", 0x20, &fresh);
		base.AddOrReplace("/LayoutData/TitleLogo.arc", 125792, &fresh);
		std::vector<u8> orig;
		base.Serialize(orig, true);
		a.fstMaxSize = (u32) orig.size();
		const OccupiedRange low(0x80004000, 0x81000000);
		FstPlacement p = PlaceFst(a, want, 32, &low, 1);
		ck(p.ok, "grown synthetic table placeable");
		simProtect(0x80004000, 0x81000000, 0xd8);
		u32 fail = 0;
		ck(SimInstall(p, staged, crc, fail) && fail == 0, "install verifies");
		ck(simIntact(0x80004000, 0x81000000, 0xd8), "low image untouched");
		ck(memcmp(&simMem[p.fstAddr - MEM1_BASE], &staged[0], want) == 0,
		   "bytes exact");
		ck(simGet32(0x80000038) == p.fstAddr, "address word follows placement");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
