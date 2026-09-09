/****************************************************************************
 * fstadapter v4 - production install + probe jump.
 *
 * v3 linked the REAL RiivoBoot.cpp TU (compiled unmodified,
 * unmodified, garbage-collected at link so only InstallPendingFst's
 * closure is retained) and calls the REAL Riivo::InstallPendingFst()
 * for both modes. The file-static staging state (pendingFst,
 * pendingFstSize, pendingFstCrc, pendingPlace, pendingPlaceOk,
 * fileWorkLive, skipFstInstall) is poked by the debugger by symbol
 * address - no production seam, no copy of the logic. The place-blob
 * bytes below come from the REAL Riivo::PlaceFst on captured inputs,
 * and the staged bytes are the v2 synthetic valid mini-FSTs (the real
 * serializer + real mod XML is staged separately; see BYPASSES.md).
 *
 * Flow per mode is GDB-driven: the harness prints the poke values,
 * waits on adGo, runs the production call once, reports its return +
 * InstallFailCode, then parks for inspection.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <gccore.h>

#include "RiivoFstInstall.hpp"
#include "RiivoReconcile.hpp" // real Crc32
#include "RiivoBoot.hpp"      // real InstallPendingFst / InstallFailCode

extern "C" void gprintf(const char *str, ...)
{
	va_list ap;
	va_start(ap, str);
	vprintf(str, ap);
	va_end(ap);
}

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

//! GDB rendezvous (globals, nm-visible): poke staging, set adGo=1.
//! adPhase removes all timing races: the harness publishes which wait it
//! sits in (1=unchanged done, 2=mode wait, 4=finished) and GDB polls it
//! with waitmem instead of sleeping. (A sleep-synced round once executed
//! the in-place install in the reloc slot after adGo was cleared by a
//! wait the script had not seen yet - same bytes, wrong label.)
volatile u32 adGo = 0;
volatile u32 adModeDone = 0;
volatile u32 adPhase = 0;

static const u32 CAP_ARENA_LO = 0;
static const u32 CAP_ARENA_HI = 0x817DA740;
static const u32 CAP_FST_ADDR = 0x817DA740;
static const u32 CAP_FST_MAX  = 153792;
static const u32 CAP_BLOCK_LO = 0x817D8740;
static const u32 CAP_BLOCK_HI = 0x817DA740;
static const u32 CAP_BSS_LO   = 0x80728680;
static const u32 CAP_BSS_HI   = 0x807E3188;
static const u32 SYN_STALE_LO = 0x817DA800;
static const u32 SYN_STALE_HI = 0x817DA900;
static const u32 RELOC_WANT   = 153934;
static const u32 INPLACE_WANT = 153792;
static const u32 EXP_RELOC    = 0x817B2DE0;
static const u32 EXP_INPLACE  = 0x817DA740;

static int checksFailed = 0;

static void Check(bool cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++checksFailed;
}

static inline void wbe32(u8 *p, u32 v)
{
	p[0] = (u8) (v >> 24);
	p[1] = (u8) (v >> 16);
	p[2] = (u8) (v >> 8);
	p[3] = (u8) v;
}

static inline u32 rbe32(const u8 *p)
{
	return ((u32) p[0] << 24) | ((u32) p[1] << 16) |
		   ((u32) p[2] << 8) | (u32) p[3];
}

static u8 *mem2Bump = 0;
static u32 mem2Lo = 0, mem2Hi = 0;

//! Probe "game" (source/probe.S): absolute-address consumer entered by
//! branch-to-CTR after cache maintenance, modeling the entry jump. Uses
//! no stack/TOC/nonvolatiles, returns the entry count (0 = reject) and
//! fills a result block at 0x80000200 for GDB.
extern "C" u8 probe_start[], probe_end[];
static const u32 PROBE_ADDR = 0x80004000;
static const u32 PROBE_RESULT = 0x80000200;

static void InstallProbe(void)
{
	const u32 n = (u32) (probe_end - probe_start);
	memcpy((void *) PROBE_ADDR, probe_start, n);
	DCFlushRange((void *) PROBE_ADDR, n);
	ICInvalidateRange((void *) PROBE_ADDR, n);
	__asm__ volatile ("sync; isync" ::: "memory");
}

//! Branch to the probe and take back its r3. Clobbers are the volatile
//! set the probe is allowed; TOC/stack/nonvolatiles survive by
//! construction (probe.S never touches them).
static u32 JumpProbe(void)
{
	u32 st;
	DCFlushRange((void *) 0x80000030, 0x20);
	__asm__ volatile ("sync; isync" ::: "memory");
	__asm__ volatile ("mtctr %1; bctrl; mr %0,3"
					  : "=r"(st) : "r"(PROBE_ADDR)
					  : "ctr", "lr", "cc", "r0",
						"r3", "r4", "r5", "r6", "r7", "r8", "r9",
						"r10", "r11", "r12", "memory");
	return st;
}
static u8 *Mem2Stage(u32 size)
{
	size = (size + 31) & ~31u;
	if (!mem2Bump)
	{
		mem2Lo = ((u32) SYS_GetArena2Lo() + 31) & ~31u;
		mem2Hi = (u32) SYS_GetArena2Hi();
		mem2Bump = (u8 *) mem2Lo;
	}
	u8 *p = mem2Bump;
	if ((u32) (p + size) > mem2Hi || (u32) (p + size) < (u32) p)
		return 0;
	mem2Bump = p + size;
	return p;
}

static u32 BuildMiniFst(u8 *buf, u32 size, char tag)
{
	u32 n = (size - 16 - 13) / 19;
	if (n < 2)
		n = 2;
	const u32 count = n + 1;
	char name[8];
	wbe32(buf + 0, (1u << 24) | 0);
	wbe32(buf + 4, 0);
	wbe32(buf + 8, count);
	for (u32 i = 1; i <= n; ++i)
	{
		u8 *e = buf + 12 * i;
		wbe32(e + 0, 1 + 7 * (i - 1));
		wbe32(e + 4, 0x200000u + i * 0x10000u);
		wbe32(e + 8, 0x10000u);
	}
	u8 *str = buf + 12 * count;
	str[0] = 0;
	u32 at = 1;
	for (u32 i = 1; i <= n; ++i)
	{
		snprintf(name, sizeof(name), "%c%05u", tag, (unsigned) (i - 1));
		const u32 l = (u32) strlen(name) + 1;
		memcpy(str + at, name, l);
		at += l;
	}
	const u32 hdr = 12 * count + at;
	for (u32 i = hdr; i < size; ++i)
		buf[i] = (u8) (0xA5 ^ (i & 0xFF) ^ ((i >> 8) & 0xFF));
	return hdr;
}

static u32 CheckMiniFst(u32 base, u32 maxsize)
{
	if (base < 0x80000000u || maxsize == 0 || maxsize > 0x800000u)
		return 0;
	if (base + maxsize < base)
		return 0;
	const u8 *b = (const u8 *) base;
	const u32 count = rbe32(b + 8);
	if (count < 2 || 12 * count >= maxsize)
		return 0;
	if (b[0] != 1)
		return 0;
	const u32 strBase = 12 * count;
	for (u32 i = 1; i < count; ++i)
	{
		const u8 *e = b + 12 * i;
		if (e[0] != 0 && e[0] != 1)
			return 0;
		if ((rbe32(e) & 0x00FFFFFFu) >= maxsize - strBase)
			return 0;
	}
	return count;
}

static void WriteBootWords(u32 lo, u32 hi, u32 fst, u32 max)
{
	*(vu32 *) 0x80000030 = lo;
	*(vu32 *) 0x80000034 = hi;
	*(vu32 *) 0x80000038 = fst;
	*(vu32 *) 0x8000003C = max;
	DCFlushRange((void *) 0x80000030, 0x20);
}

//! GDB mailbox at a fixed scratch address (video is invisible under
//! Null backend): the debugger reads the poke values here instead of
//! the console. Layout: stageR crcR stageI crcI sizeR sizeI blobR blobI
//! blobLen@+24+2*sizeof blobLenCopy@+144. The length travels twice
//! because sizeof(FstPlacement) is toolchain-dependent - assuming it
//! once poisoned the neighboring staging words through an 8-byte
//! overrun and cost a full debugging round (see BYPASSES.md).
static const u32 MAILBOX = 0x80000100;

static void WriteMailbox(u8 *stageR, u32 crcR, u8 *stageI, u32 crcI,
						 const Riivo::FstPlacement &placeR,
						 const Riivo::FstPlacement &placeI)
{
	u8 *m = (u8 *) MAILBOX;
	wbe32(m + 0, (u32) stageR);
	wbe32(m + 4, crcR);
	wbe32(m + 8, (u32) stageI);
	wbe32(m + 12, crcI);
	wbe32(m + 16, RELOC_WANT);
	wbe32(m + 20, INPLACE_WANT);
	memcpy(m + 24, &placeR, sizeof(placeR));
	memcpy(m + 24 + sizeof(placeR), &placeI, sizeof(placeI));
	wbe32(m + 24 + 2 * sizeof(placeR), (u32) sizeof(placeR));
	wbe32(m + 144, (u32) sizeof(placeR));
	DCFlushRange(m, 148);
	printf("mailbox: placeblob len=%u\n", (unsigned) sizeof(placeR));
}
static void PrintPoke(const char *tag, const Riivo::FstPlacement &place,
					  u8 *stage, u32 size, u32 crc)
{
	printf("%s poke block (write, then set adGo=1):\n", tag);
	printf("  pendingFst=%08x size=%u crc=%08x placeOk=1 fileWorkLive=1 "
		   "skipFstInstall=0 failCode=0\n",
		   (u32) stage, size, crc);
	printf("  place: ok=%d inPlace=%d addr=%08x newArenaHi=%08x "
		   "reserved=%u heapLeft=%u ignored=%u malformed=%u\n",
		   (int) place.ok, (int) place.inPlace, place.fstAddr,
		   place.newArenaHi, place.reserved, place.heapLeft,
		   place.ignoredRanges, place.malformedRanges);
	const u8 *raw = (const u8 *) &place;
	printf("  placeblob len=%u hex=", (unsigned) sizeof(place));
	for (unsigned i = 0; i < sizeof(place); ++i)
		printf("%02x", raw[i]);
	printf("\n");
}

//! Jump to the probe consumer and check it consumed the intended table:
//! magic + count + CRC equality with the staged bytes + arena record.
//! A per-jump nonce in word 4 tells a fresh block from a stale re-read.
static u32 probeNonce = 0;

static void ConsumeCheck(const char *tag, u32 expCount, u32 expCrc)
{
	*(vu32 *) (PROBE_RESULT + 16) = ++probeNonce;
	DCFlushRange((void *) PROBE_RESULT, 20);
	const u32 got = JumpProbe();
	const vu32 *res = (const vu32 *) PROBE_RESULT;
	printf("  %s probe: r3=%u magic=%08x count=%u crc=%08x arena=%08x\n",
		   tag, got, res[0], res[1], res[2], res[3]);
	Check(res[0] == 0x50524F42u, "probe accepted the table");
	Check(got == expCount && res[1] == expCount, "probe entry count matches");
	Check(res[2] == expCrc, "probe CRC matches the staged table");
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
	printf("fstadapter v4: production install + probe jump\n");
	printf("=============================================\n");

	printf("live boot words (saved, context only):\n  %08x %08x %08x %08x\n",
		   *(vu32 *) 0x80000030, *(vu32 *) 0x80000034,
		   *(vu32 *) 0x80000038, *(vu32 *) 0x8000003C);

	InstallProbe();

	u8 *stageReloc = Mem2Stage(RELOC_WANT);
	u8 *stageInplace = Mem2Stage(INPLACE_WANT);
	u8 *stageBase = Mem2Stage(INPLACE_WANT);
	Check(stageReloc && stageInplace && stageBase, "MEM2 staging allocated");
	Check((u32) stageReloc >= mem2Lo, "stages live in Arena2, not MEM1");
	BuildMiniFst(stageReloc, RELOC_WANT, 'f');
	BuildMiniFst(stageInplace, INPLACE_WANT, 'f');
	BuildMiniFst(stageBase, INPLACE_WANT, 'g');
	DCFlushRange(stageReloc, RELOC_WANT);
	DCFlushRange(stageInplace, INPLACE_WANT);
	DCFlushRange(stageBase, INPLACE_WANT);
	const u32 crcR = Riivo::Crc32(stageReloc, RELOC_WANT);
	const u32 crcI = Riivo::Crc32(stageInplace, INPLACE_WANT);
	const u32 crcB = Riivo::Crc32(stageBase, INPLACE_WANT);
	printf("  reloc stage %08x crc %08x\n", (u32) stageReloc, crcR);
	printf("  inplace stage %08x crc %08x\n", (u32) stageInplace, crcI);
	printf("  base stage %08x crc %08x\n", (u32) stageBase, crcB);

	// Inter-phase heap churn (labeled stand-in, as v2).
	void *live1 = malloc(8192), *live2 = malloc(65536);
	for (int r = 0; r < 6; ++r)
	{
		void *a = malloc(1024 + (u32) r * 8192);
		void *b = malloc(262144);
		if (a) memset(a, 0x5A + r, 1024 + (u32) r * 8192);
		if (b) memset(b, 0xA5, 262144);
		free(b);
		free(a);
	}
	printf("churn done (live %08x %08x break %08x)\n", (u32) live1,
		   (u32) live2, (u32) (uintptr_t) sbrk(0));

	const Riivo::OccupiedRange occ[3] = {
		Riivo::OccupiedRange(CAP_BLOCK_LO, CAP_BLOCK_HI),
		Riivo::OccupiedRange(CAP_BSS_LO, CAP_BSS_HI),
		Riivo::OccupiedRange(SYN_STALE_LO, SYN_STALE_HI),
	};
	Riivo::ArenaInfo arena;
	arena.arenaLo = CAP_ARENA_LO;
	arena.arenaHi = CAP_ARENA_HI;
	arena.fstAddr = CAP_FST_ADDR;
	arena.fstMaxSize = CAP_FST_MAX;
	const Riivo::FstPlacement placeR =
		Riivo::PlaceFst(arena, RELOC_WANT, 32, occ, 3);
	const Riivo::FstPlacement placeI =
		Riivo::PlaceFst(arena, INPLACE_WANT, 32, occ, 3);
	Check(placeR.ok && !placeR.inPlace && placeR.fstAddr == EXP_RELOC,
		  "reloc plan is the known answer");
	Check(placeI.ok && placeI.inPlace && placeI.fstAddr == EXP_INPLACE,
		  "in-place plan is the known answer");
	WriteMailbox(stageReloc, crcR, stageInplace, crcI, placeR, placeI);

	// UNCHANGED: the apploader-loaded original, modeled by placing the
	// synthetic base table where the apploader would have put the real
	// one. PROD-DELTA: base bytes are synthetic ('g' scheme) because the
	// real SB4E01 FST is AES-locked without the console key; the shape
	// (address, size, words, jump, consume) is the production shape.
	printf("================ UNCHANGED (base at %08x) ================\n",
		   CAP_FST_ADDR);
	memcpy((void *) CAP_FST_ADDR, stageBase, INPLACE_WANT);
	DCFlushRange((void *) CAP_FST_ADDR, INPLACE_WANT);
	WriteBootWords(CAP_ARENA_LO, CAP_ARENA_HI, CAP_FST_ADDR, CAP_FST_MAX);
	ConsumeCheck("UNCHANGED", 8093, crcB);
	// Rendezvous so GDB can read the unchanged result block before the
	// relocated install overwrites MEM1 state. GDB sets adGo=2; the
	// mode loop below resets it to 0 first.
	printf("UNCHANGED done; waiting for adGo=2...\n");
	adPhase = 1;
	while (adGo != 2)
		__asm__ volatile ("" ::: "memory");

	const struct { const char *tag; u32 want; u32 exp; } modes[2] = {
		{ "RELOC", RELOC_WANT, EXP_RELOC },
		{ "INPLACE", INPLACE_WANT, EXP_INPLACE },
	};
	for (int m = 0; m < 2; ++m)
	{
		// Model the apploader-filled block per modeled boot.
		WriteBootWords(CAP_ARENA_LO, CAP_ARENA_HI, CAP_FST_ADDR, CAP_FST_MAX);
		if (m == 0)
			PrintPoke("RELOC", placeR, stageReloc, RELOC_WANT, crcR);
		else
			PrintPoke("INPLACE", placeI, stageInplace, INPLACE_WANT, crcI);
		printf("%s: waiting for poke (set adGo=1)...\n", modes[m].tag);
		adGo = 0;
		adModeDone = 0;
		adPhase = 2;
		// Busy spin, never VIDEO_WaitVSync: under Dolphin's Null video
		// backend VI interrupts may never arrive, which would park this
		// thread in the OS wait queue past the poke. The barrier forces
		// a fresh volatile read every iteration.
		while (!adGo)
			__asm__ volatile ("" ::: "memory");
		// THE production call. No mirror, no copy of its logic.
		const bool ret = Riivo::InstallPendingFst();
		const u32 code = Riivo::InstallFailCode();
		printf("%s: production InstallPendingFst returned %d, fail code %u\n",
			   modes[m].tag, (int) ret, code);
		Check(ret && code == 0, "production install verified");
		const u32 ptr = *(vu32 *) 0x80000038;
		const u32 max = *(vu32 *) 0x8000003C;
		const u32 count = CheckMiniFst(ptr, max);
		printf("  consumer: %u entries through repointed words\n", count);
		Check(count > 0, "installed table parses through new pointer");
		Check(ptr == modes[m].exp, "pointer word is the expected address");
		ConsumeCheck(modes[m].tag, m == 0 ? 8101 : 8093,
					 m == 0 ? crcR : crcI);
		adModeDone = 1;
		// Two-way rendezvous: GDB must ack (adGo=2) before the next mode
		// may even reset adGo. No evidence window can be missed and no
		// script timing can poison a later poke: nothing proceeds until
		// both sides have seen this mode complete.
		adGo = 0;
		while (adGo != 2)
			__asm__ volatile ("" ::: "memory");
	}

	free(live1);
	free(live2);

	// MEM2 mechanics (v5): the REAL PlaceFstMem2 + InstallFst on the real
	// code path, fully harness-driven (no poked statics needed - the
	// function takes explicit arguments). Proves the MEM2 copy, flush,
	// words and probe consumption work in emulated MEM2. What it does NOT
	// prove: game behavior with an MEM2 table (survey: 0x92000000 reads
	// fine pre-boot but the game never consumed there - Test 6 held).
	printf("================ MEM2 (experimental mechanics) ================\n");
	{
		Riivo::ArenaInfo arena2;
		arena2.arenaLo = CAP_ARENA_LO;
		arena2.arenaHi = CAP_ARENA_HI;
		arena2.fstAddr = CAP_FST_ADDR;
		arena2.fstMaxSize = CAP_FST_MAX;
		const Riivo::FstPlacement pm =
			Riivo::PlaceFstMem2(arena2, RELOC_WANT, 32);
		printf("  plan: ok=%d inPlace=%d addr=%08x arenaHi=%08x\n",
			   (int) pm.ok, (int) pm.inPlace, pm.fstAddr, pm.newArenaHi);
		Check(pm.ok, "MEM2 placement accepted");
		Check(!pm.inPlace, "MEM2 placement is grown");
		Check(pm.fstAddr == 0x92000000u, "MEM2 surveyed base address");
		Check(pm.newArenaHi == CAP_ARENA_HI, "MEM1 arena passes through");
		Check(pm.reserved == 0, "nothing taken from MEM1");
		const u32 dLo = pm.fstAddr, dHi = pm.fstAddr + RELOC_WANT;
		Check(!Riivo::RangesOverlap(dLo, dHi, (u32) stageReloc,
									(u32) (stageReloc + RELOC_WANT)),
			  "MEM2 destination clear of its stage");
		WriteBootWords(CAP_ARENA_LO, CAP_ARENA_HI, pm.fstAddr, RELOC_WANT);
		const bool ok = Riivo::InstallFst(pm, stageReloc, RELOC_WANT);
		Check(ok, "MEM2 InstallFst returned true");
		Check(memcmp((const void *) dLo, stageReloc, RELOC_WANT) == 0,
			  "MEM2 destination bytes read back identical");
		Check(*(vu32 *) 0x80000038 == pm.fstAddr, "word 0x38 points at MEM2");
		Check(*(vu32 *) 0x80000034 == CAP_ARENA_HI, "word 0x34 kept");
		ConsumeCheck("MEM2", 8101, crcR);
	}
	adPhase = 4;
	printf("=============================================\n");
	if (checksFailed == 0)
		printf("RESULT: PASS (unchanged + both production installs +\n"
			   "               MEM2 mechanics consumed)\n");
	else
		printf("RESULT: FAIL (%d check(s))\n", checksFailed);
	printf("halting in place for debugger inspection.\n");
	while (1)
		VIDEO_WaitVSync();
	return 0;
}
