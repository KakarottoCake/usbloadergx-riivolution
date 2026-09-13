// Local startup-survival regression: which placements survive game startup
// under each hypothesized clearing rule, and which rule the observed
// outcomes already exclude - using the captured SB4E01 T0 grown-table case,
// with no hardware.
//
// BACKGROUND (evidence, not assumption): in-place tables boot (Yoshi
// 2-byte-spare passing run; T0-compacted acceptance path); grown tables do
// not (T0 plain 153934, relocorig verbatim stock bytes at the same grown
// geometry); stock boots. Loader-side overlap is a live alternative cause
// for the grown deaths and is NOT excluded here - the instrument
// (early install + pre-jump re-verify) exists precisely to separate it.
//
// WHAT THIS PINS:
// 1. Production PlaceFst over the captured layout puts the grown table
//    below the apploader reservation and the compacted table in place.
// 2. The virtual boot header builder (production PatchBootHeader) emits a
//    byte-exact grown-max image; a modeled apploader echo derives grown
//    reservation words from its served 0x42C (model, not proof - the real
//    apploader's consumption is an open log read, see below).
// 3. Rule R-b (clear below reservation top) is EXCLUDED by the
//    in-place-boot observation alone: it predicts an in-place death that
//    did not happen. Rule R-a (clear below arenaHi) spares in-place but
//    would also spare a coherently-placed grown table - so the observed
//    grown death excludes R-a as a SELF-CONTAINED explanation (it needs a
//    loader-overlap co-cause).
// 4. Rule R-g (clear below the REPORTED table base) predicts all four
//    observations with no co-cause - but only if every base a startup
//    consumer reads agrees (MEM words, which InstallFst writes, AND the
//    apploader-struct copy at 0x81201b80+0x10, which nothing updates).
//    A stale struct copy kills a grown table even with coherent MEM words
//    (asserted in sim). That copy is the open consumer question.
// 5. Rule R-d (no clearing) remains possible only with an unproven
//    loader-overlap co-cause for the grown deaths.
// 6. The instrument logic works: post-install churn corruption is caught
//    by re-verification (fail), clean entry passes.
//
// WHAT STILL REQUIRES HARDWARE, explicitly:
// - Which rule real startup follows (the discriminating run is specified
//   in the report, not here).
// - Whether the apploader consumes boot.bin FST words through GX reads:
//   read the apploader yield list of any file-mod boot log for requests at
//   0x420/boot/FST ranges ("no section match" + req lines) - zero new code
//   needed. If they appear, the served header path is live; if the
//   apploader never reads them, header patching changes nothing and the
//   preventing mechanism is apploader-intrinsic reservation.
// - Whether the game (or anything post-jump) consults the +0x10 struct
//   copy: compare its logged value against the installed base on a grown
//   experiment run.
//
// SCOPE: simulation only. No console calls; production PlaceFst,
// PatchBootHeader and Crc32 run here directly (shared code, not copies).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "riivo/RiivoFstInstall.hpp"
#include "riivo/RiivoBootView.hpp"
#include "riivo/RiivoReconcile.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// ---- simulated MEM1 ------------------------------------------------------
static u8 simMem[0x1800000];

static u32 simGet32(u32 addr)
{
	const u32 o = addr - MEM1_BASE;
	return ((u32)simMem[o] << 24) | ((u32)simMem[o + 1] << 16) |
		   ((u32)simMem[o + 2] << 8) | simMem[o + 3];
}

static void simSet32(u32 addr, u32 v)
{
	const u32 o = addr - MEM1_BASE;
	simMem[o] = (u8)(v >> 24);
	simMem[o + 1] = (u8)(v >> 16);
	simMem[o + 2] = (u8)(v >> 8);
	simMem[o + 3] = (u8)v;
}

static void simFill(u32 lo, u32 hi, u8 pat)
{
	if (hi <= lo || lo < MEM1_BASE || hi > MEM1_END)
		return;
	memset(&simMem[lo - MEM1_BASE], pat, hi - lo);
}

static bool simMatches(u32 lo, const u8 *expect, u32 len)
{
	if (lo < MEM1_BASE || (u64)lo + len > MEM1_END)
		return false;
	return memcmp(&simMem[lo - MEM1_BASE], expect, len) == 0;
}

// ---- captured T0 layout (same numbers as test_installsim §1) -------------
static const u32 kArenaHi = 0x817da740;
static const u32 kFstAddr = 0x817da740;
static const u32 kFstMax = 153792;
static const u32 kWantPlain = 153934;   // grown by 142
static const u32 kWantCompact = 144323; // fits in place
static const u32 kBssLo = 0x80728680;
static const u32 kBssHi = 0x807e3188;
static const u32 kBlockLo = 0x817d8740; // 8 KB apploader block
static const u32 kBlockHi = 0x817da740;

// ---- simulated clearing rules --------------------------------------------
// Clear [clearLo, clearBound): the game zeroes heap/scratch it owns.
// R_GIVEN_BASE: below the REPORTED table base (base supplied per case).
// R_ARENA_HI: below the arenaHi word. R_RES_TOP: below the stock
// reservation top. R_NONE: startup preserves everything.
enum ClearRule { R_GIVEN_BASE, R_ARENA_HI, R_RES_TOP, R_NONE };

static void SimStartup(ClearRule rule, u32 arenaLo, u32 arenaHi,
					   u32 reportedBase, u32 resTop)
{
	u32 bound = 0;
	switch (rule)
	{
		case R_GIVEN_BASE: bound = reportedBase; break;
		case R_ARENA_HI: bound = arenaHi; break;
		case R_RES_TOP: bound = resTop; break;
		case R_NONE: return;
	}
	simFill(arenaLo, bound, 0x00);
}

// Game-view read: table bytes at the MEM words vs the expected staged
// image. Whatever the rules cleared, this is what the game would consume.
static bool GameSeesTable(u32 *outBase, const u8 *expect, u32 len)
{
	u32 base = simGet32(0x80000038);
	u32 size = simGet32(0x8000003C);
	if (outBase)
		*outBase = base;
	return size == len && simMatches(base, expect, len);
}

int main()
{
	// ---- 1. production placement over the captured layout ----
	ArenaInfo arena;
	arena.arenaLo = 0;
	arena.arenaHi = kArenaHi;
	arena.fstAddr = kFstAddr;
	arena.fstMaxSize = kFstMax;
	OccupiedRange occ[2] = { OccupiedRange(kBlockLo, kBlockHi),
							 OccupiedRange(kBssLo, kBssHi) };
	FstPlacement grown = PlaceFst(arena, kWantPlain, 32, occ, 2);
	CHECK(grown.ok && !grown.inPlace); // the failing case: cascades down
	CHECK(grown.fstAddr < kBlockLo);   // below the apploader block
	FstPlacement flat = PlaceFst(arena, kWantCompact, 32, occ, 2);
	CHECK(flat.ok && flat.inPlace); // compaction fits the reservation
	CHECK(flat.fstAddr == kFstAddr);

	// ---- 2. virtual header: production builder, byte-exact ----
	std::vector<u8> stockHdr(BOOTVIEW_HEADER_BYTES, 0);
	BootViewWriteBE32(&stockHdr[0x420], 0x1234); // dol offset (words): truth kept
	BootViewWriteBE32(&stockHdr[BOOTVIEW_FST_OFF], 0x14000); // disc offset: truth kept
	BootViewWriteBE32(&stockHdr[BOOTVIEW_FST_SIZE], 0x9600); // disc size: truth kept
	BootViewWriteBE32(&stockHdr[BOOTVIEW_FST_MAX], kFstMax >> 2);
	std::vector<u8> grownHdr;
	std::string why;
	CHECK(PatchBootHeader(&stockHdr[0], (u32)stockHdr.size(),
						  0x50000, 150000, kWantPlain, grownHdr, why));
	CHECK(grownHdr.size() == BOOTVIEW_HEADER_BYTES);
	CHECK(BootViewReadBE32(&grownHdr[BOOTVIEW_FST_OFF]) == 0x50000u >> 2); // unchanged
	CHECK(BootViewReadBE32(&grownHdr[BOOTVIEW_FST_SIZE]) == 150000u >> 2); // unchanged
	CHECK(BootViewReadBE32(&grownHdr[BOOTVIEW_FST_MAX]) == kWantPlain >> 2); // grown ask
	CHECK(BootViewReadBE32(&grownHdr[0x420]) == 0x1234); // DOL word untouched
	// Modeled apploader echo: a consumer of the served 0x42C derives grown
	// reservation words from it (model of the open question, not proof).
	u32 echoMax = BootViewReadBE32(&grownHdr[BOOTVIEW_FST_MAX]) << 2;
	CHECK(echoMax == (kWantPlain & ~3u));

	// ---- staged images (content differs per case, geometry shared) ----
	std::vector<u8> plainImg(kWantPlain, 0xA5);
	std::vector<u8> verbatimImg(kWantPlain, 0x5A); // stock bytes, grown address
	std::vector<u8> compactImg(kWantCompact, 0xC3);
	const u32 crcPlain = Crc32(&plainImg[0], (u32)plainImg.size());

	// ---- 3/4. rule matrix: install, clear, game-view read ----
	// install mirrors SimInstall (copy + coherent words + verify).
	// R_GIVEN_BASE with MEM words coherent (InstallFst behavior):
	{
		// grown plain at its cascade base, words agree:
		memset(simMem, 0x81, sizeof(simMem));
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr);
		simSet32(0x8000003C, kWantPlain);
		simSet32(0x80000034, grown.newArenaHi);
		SimStartup(R_GIVEN_BASE, 0x80003100, grown.newArenaHi, grown.fstAddr,
				   kFstAddr + kFstMax);
		CHECK(GameSeesTable(0, &plainImg[0], kWantPlain)); // survives: at/above base
		// in-place compacted under the same rule:
		memset(simMem, 0x82, sizeof(simMem));
		memcpy(&simMem[kFstAddr - MEM1_BASE], &compactImg[0], kWantCompact);
		simSet32(0x80000038, kFstAddr);
		simSet32(0x8000003C, kWantCompact);
		simSet32(0x80000034, kArenaHi);
		SimStartup(R_GIVEN_BASE, 0x80003100, kArenaHi, kFstAddr,
				   kFstAddr + kFstMax);
		CHECK(GameSeesTable(0, &compactImg[0], kWantCompact));
		// stale struct copy (stock base) as the clearing bound instead:
		memset(simMem, 0x83, sizeof(simMem));
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &verbatimImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr); // MEM words coherent...
		simSet32(0x8000003C, kWantPlain);
		simSet32(0x80000034, grown.newArenaHi);
		SimStartup(R_GIVEN_BASE, 0x80003100, grown.newArenaHi, kFstAddr,
				   kFstAddr + kFstMax); // ...but clearing reads the STALE base
		CHECK(!GameSeesTable(0, &verbatimImg[0], kWantPlain)); // erased anyway
	}
	// R_ARENA_HI spares the in-place table (it starts exactly at the bound),
	// so the in-place-boot observation does NOT exclude it. What excludes
	// R_ARENA_HI as a self-contained explanation is the grown death: with
	// the coherent lowered arenaHi the grown table also sits at its bound
	// and would survive - so under R_ARENA_HI the observed grown death
	// needs a loader-overlap co-cause.
	{
		memset(simMem, 0x84, sizeof(simMem));
		memcpy(&simMem[kFstAddr - MEM1_BASE], &compactImg[0], kWantCompact);
		simSet32(0x80000038, kFstAddr);
		simSet32(0x8000003C, kWantCompact);
		simSet32(0x80000034, kArenaHi);
		SimStartup(R_ARENA_HI, 0x80003100, kArenaHi, kFstAddr,
				   kFstAddr + kFstMax);
		CHECK(GameSeesTable(0, &compactImg[0], kWantCompact)); // spared
	}
	{
		// Grown table with coherently lowered arenaHi under R_ARENA_HI:
		memset(simMem, 0x85, sizeof(simMem));
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr);
		simSet32(0x8000003C, kWantPlain);
		simSet32(0x80000034, grown.newArenaHi);
		SimStartup(R_ARENA_HI, 0x80003100, grown.newArenaHi, grown.fstAddr,
				   kFstAddr + kFstMax);
		CHECK(GameSeesTable(0, &plainImg[0], kWantPlain)); // would survive...
		// ...but grown died on hardware, so R_ARENA_HI alone cannot explain
		// the failure (it needs loader-overlap as co-cause).
	}
	// R_RES_TOP is EXCLUDED by the in-place-boot observation: it clears
	// below the reservation top, which covers the in-place table too.
	{
		memset(simMem, 0x86, sizeof(simMem));
		memcpy(&simMem[kFstAddr - MEM1_BASE], &compactImg[0], kWantCompact);
		simSet32(0x80000038, kFstAddr);
		simSet32(0x8000003C, kWantCompact);
		simSet32(0x80000034, kArenaHi);
		SimStartup(R_RES_TOP, 0x80003100, kArenaHi, kFstAddr,
				   kFstAddr + kFstMax);
		CHECK(!GameSeesTable(0, &compactImg[0], kWantCompact)); // would die...
		// ...but in-place boots on hardware, so R_RES_TOP is out.
	}
	// R_NONE needs a loader-overlap co-cause for the grown deaths (unproven
	// here; the instrument below exists to separate it when it matters):
	{
		memset(simMem, 0x86, sizeof(simMem));
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr);
		simSet32(0x8000003C, kWantPlain);
		SimStartup(R_NONE, 0x80003100, grown.newArenaHi, grown.fstAddr,
				   kFstAddr + kFstMax);
		CHECK(GameSeesTable(0, &plainImg[0], kWantPlain)); // nothing clears it
	}

	// ---- 5. instrument logic: churn corruption is caught, clean passes ----
	{
		memset(simMem, 0x87, sizeof(simMem));
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr);
		simSet32(0x8000003C, kWantPlain);
		simSet32(0x80000034, grown.newArenaHi);
		// Simulated loader churn AFTER install: one byte + one word.
		simMem[grown.fstAddr - MEM1_BASE + 1000] ^= 0xFF;
		bool reverify = simMatches(grown.fstAddr, &plainImg[0], kWantPlain)
			&& Crc32(&simMem[grown.fstAddr - MEM1_BASE], kWantPlain) == crcPlain
			&& simGet32(0x80000038) == grown.fstAddr
			&& simGet32(0x8000003C) == kWantPlain
			&& simGet32(0x80000034) == grown.newArenaHi;
		CHECK(!reverify); // caught: would blink, never jump on corruption
		simSet32(0x80000038, kFstAddr); // plus a word fault on its own:
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		reverify = simMatches(grown.fstAddr, &plainImg[0], kWantPlain)
			&& Crc32(&simMem[grown.fstAddr - MEM1_BASE], kWantPlain) == crcPlain
			&& simGet32(0x80000038) == grown.fstAddr
			&& simGet32(0x8000003C) == kWantPlain
			&& simGet32(0x80000034) == grown.newArenaHi;
		CHECK(!reverify); // wrong base word alone also fails closed
		memcpy(&simMem[grown.fstAddr - MEM1_BASE], &plainImg[0], kWantPlain);
		simSet32(0x80000038, grown.fstAddr);
		simSet32(0x8000003C, kWantPlain);
		simSet32(0x80000034, grown.newArenaHi);
		reverify = simMatches(grown.fstAddr, &plainImg[0], kWantPlain)
			&& Crc32(&simMem[grown.fstAddr - MEM1_BASE], kWantPlain) == crcPlain
			&& simGet32(0x80000038) == grown.fstAddr
			&& simGet32(0x8000003C) == kWantPlain
			&& simGet32(0x80000034) == grown.newArenaHi;
		CHECK(reverify); // clean entry passes
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
