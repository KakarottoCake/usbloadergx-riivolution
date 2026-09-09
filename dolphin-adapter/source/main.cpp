/****************************************************************************
 * fstadapter v3 - the PRODUCTION InstallPendingFst under GDB.
 *
 * v2 mirrored the late-install caller. A mirror that passes proves the
 * model, not the code. v3 links the REAL RiivoBoot.cpp TU (compiled
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
volatile u32 adGo = 0;
volatile u32 adModeDone = 0;

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

static u32 BuildMiniFst(u8 *buf, u32 size)
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
		snprintf(name, sizeof(name), "f%05u", (unsigned) (i - 1));
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
	printf("fstadapter v3: PRODUCTION InstallPendingFst\n");
	printf("==========================================\n");

	printf("live boot words (saved, context only):\n  %08x %08x %08x %08x\n",
		   *(vu32 *) 0x80000030, *(vu32 *) 0x80000034,
		   *(vu32 *) 0x80000038, *(vu32 *) 0x8000003C);

	u8 *stageReloc = Mem2Stage(RELOC_WANT);
	u8 *stageInplace = Mem2Stage(INPLACE_WANT);
	Check(stageReloc && stageInplace, "MEM2 staging allocated");
	Check((u32) stageReloc >= mem2Lo, "stages live in Arena2, not MEM1");
	BuildMiniFst(stageReloc, RELOC_WANT);
	BuildMiniFst(stageInplace, INPLACE_WANT);
	DCFlushRange(stageReloc, RELOC_WANT);
	DCFlushRange(stageInplace, INPLACE_WANT);
	const u32 crcR = Riivo::Crc32(stageReloc, RELOC_WANT);
	const u32 crcI = Riivo::Crc32(stageInplace, INPLACE_WANT);
	printf("  reloc stage %08x crc %08x\n", (u32) stageReloc, crcR);
	printf("  inplace stage %08x crc %08x\n", (u32) stageInplace, crcI);

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
		adModeDone = 1;
	}

	free(live1);
	free(live2);
	printf("==========================================\n");
	if (checksFailed == 0)
		printf("RESULT: PASS (production call verifies both modes)\n");
	else
		printf("RESULT: FAIL (%d check(s))\n", checksFailed);
	printf("halting in place for debugger inspection.\n");
	while (1)
		VIDEO_WaitVSync();
	return 0;
}
