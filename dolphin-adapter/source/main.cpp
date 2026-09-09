/****************************************************************************
 * fstadapter - Dolphin-side known-answer harness for the REAL Riivo
 * FST placement/install code.
 *
 * This is NOT production code and NOT part of USB Loader GX. It exists so
 * the exact `Riivo::PlaceFst` / `Riivo::InstallFst` translation units that
 * ship in the loader can run under Dolphin, where the GDB stub can
 * watchpoint the destination span and catch data-storage exceptions.
 *
 * Method: feed the captured Super Smash Bros. Brawl (SB4E01) T0 boot words
 * as constants, plan with the real PlaceFst, install with the real
 * InstallFst into emulated MEM1, then read everything back and report
 * PASS/FAIL on screen. Expected answer (from v3.36+ card logs and the
 * host fixture): destination 0x817B2DE0, 153934 bytes, arena high lowered
 * to the table, 162912 bytes reserved.
 *
 * See BYPASSES.md for what this bypasses (all cIOS/disc/apploader
 * behavior) and what it therefore can and cannot prove.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <gccore.h>

#include "RiivoFstInstall.hpp"

//! Satisfies shim/gecko.h for the real RiivoFstInstall.cpp TU, which logs
//! through gprintf. Here that is the video console.
extern "C" void gprintf(const char *str, ...)
{
	va_list ap;
	va_start(ap, str);
	vprintf(str, ap);
	va_end(ap);
}

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

//! Captured SB4E01 T0 boot words (v3.36+ card logs). arena low is unset (0)
//! on this title; the table sits at 0x817DA740 with 153792 bytes reserved.
static const u32 CAP_ARENA_LO = 0;
static const u32 CAP_ARENA_HI = 0x817DA740;
static const u32 CAP_FST_ADDR = 0x817DA740;
static const u32 CAP_FST_MAX  = 153792;
static const u32 CAP_WANT     = 153934;

//! Captured obstacles: the 8 KB apploader block ending exactly where the
//! table begins, and the game's BSS from its DOL header.
static const u32 CAP_BLOCK_LO = 0x817D8740;
static const u32 CAP_BLOCK_HI = 0x817DA740;
static const u32 CAP_BSS_LO   = 0x80728680;
static const u32 CAP_BSS_HI   = 0x807E3188;

//! Synthetic stale-table sample INSIDE the captured reservation
//! [0x817DA740, 0x817DA740+153792): exercises the expected-overlap skip
//! counter. Labeled synthetic so it is never mistaken for a capture.
static const u32 SYN_STALE_LO = 0x817DA800;
static const u32 SYN_STALE_HI = 0x817DA900;

//! Expected answer. 0x817DA740 - 0x817B2DE0 = 0x27960 = 162912.
static const u32 EXP_DEST     = 0x817B2DE0;
static const u32 EXP_RESERVED = 162912;

//! Staging buffer: the rebuilt table arrives from MEM2 on hardware; here a
//! marker pattern stands in. Static so its address is fixed and the
//! self-overlap guard below can test against it.
static u8 stage[CAP_WANT];

static int checksFailed = 0;

static void Check(bool cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++checksFailed;
}

int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
				 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	printf("\x1b[2;0H");
	printf("fstadapter: Riivo PlaceFst/InstallFst known-answer\n");
	printf("==================================================\n");

	printf("captured SB4E01 T0 inputs:\n");
	printf("  arena lo %08x hi %08x fst %08x max %u\n",
		   CAP_ARENA_LO, CAP_ARENA_HI, CAP_FST_ADDR, CAP_FST_MAX);
	printf("  want %u align 32\n", CAP_WANT);
	printf("  block [%08x, %08x)  BSS [%08x, %08x)\n",
		   CAP_BLOCK_LO, CAP_BLOCK_HI, CAP_BSS_LO, CAP_BSS_HI);
	printf("live boot words (context only, inputs are the captures):\n");
	printf("  %08x %08x %08x %08x\n",
		   *(vu32 *) 0x80000030, *(vu32 *) 0x80000034,
		   *(vu32 *) 0x80000038, *(vu32 *) 0x8000003C);

	Riivo::ArenaInfo arena;
	arena.arenaLo = CAP_ARENA_LO;
	arena.arenaHi = CAP_ARENA_HI;
	arena.fstAddr = CAP_FST_ADDR;
	arena.fstMaxSize = CAP_FST_MAX;

	const Riivo::OccupiedRange occ[3] = {
		Riivo::OccupiedRange(CAP_BLOCK_LO, CAP_BLOCK_HI),
		Riivo::OccupiedRange(CAP_BSS_LO, CAP_BSS_HI),
		Riivo::OccupiedRange(SYN_STALE_LO, SYN_STALE_HI),
	};

	printf("planning...\n");
	const Riivo::FstPlacement place =
		Riivo::PlaceFst(arena, CAP_WANT, 32, occ, 3);
	printf("  ok=%d inPlace=%d addr=%08x newArenaHi=%08x reserved=%u\n",
		   (int) place.ok, (int) place.inPlace, place.fstAddr,
		   place.newArenaHi, place.reserved);
	printf("  ignored=%u malformed=%u\n",
		   place.ignoredRanges, place.malformedRanges);
	if (!place.ok)
		printf("  refused: %s\n", place.why.c_str());

	Check(place.ok, "placement accepted");
	Check(!place.inPlace, "placement is grown, not in-place");
	Check(place.fstAddr == EXP_DEST, "destination is 0x817B2DE0");
	Check(place.newArenaHi == EXP_DEST, "arena high follows the table down");
	Check(place.reserved == EXP_RESERVED, "162912 bytes reserved");
	Check(place.ignoredRanges == 1, "one stale-table range skipped");
	Check(place.malformedRanges == 0, "no malformed ranges");

	// Marker-fill the staged table, with sentinels at both ends so a
	// short or shifted write reads back wrong.
	for (u32 i = 0; i < CAP_WANT; ++i)
		stage[i] = (u8) (0xA5 ^ (i & 0xFF) ^ ((i >> 8) & 0xFF));
	*(u32 *) &stage[0] = 0xF574AB1E;
	*(u32 *) &stage[4] = CAP_WANT;
	*(u32 *) &stage[CAP_WANT - 8] = EXP_DEST;
	*(u32 *) &stage[CAP_WANT - 4] = 0xE17FAB1E;
	DCFlushRange(stage, CAP_WANT);

	// Self-overlap guard: the destination must not touch this adapter's
	// own staging buffer. A clobbered harness could print PASS falsely.
	const u32 destLo = place.fstAddr, destHi = place.fstAddr + CAP_WANT;
	const u32 stgLo = (u32) stage, stgHi = (u32) (stage + CAP_WANT);
	Check(!Riivo::RangesOverlap(destLo, destHi, stgLo, stgHi),
		  "destination clear of the staging buffer");
	printf("  stage [%08x, %08x) dest [%08x, %08x)\n",
		   stgLo, stgHi, destLo, destHi);

	printf("installing...\n");
	const bool installed = Riivo::InstallFst(place, stage, CAP_WANT);
	Check(installed, "InstallFst returned true");

	printf("verifying...\n");
	Check(memcmp((const void *) destLo, stage, CAP_WANT) == 0,
		  "destination bytes read back identical");
	Check(*(vu32 *) 0x80000038 == EXP_DEST, "boot word 0x38 repoints at dest");
	Check(*(vu32 *) 0x8000003C == CAP_WANT, "boot word 0x3C is the new size");
	Check(*(vu32 *) 0x80000034 == EXP_DEST, "boot word 0x34 is the new arena");

	printf("==================================================\n");
	if (checksFailed == 0)
		printf("RESULT: PASS (all checks)\n");
	else
		printf("RESULT: FAIL (%d check(s))\n", checksFailed);
	printf("halting in place for debugger inspection.\n");

	while (1)
		VIDEO_WaitVSync();
	return 0;
}
