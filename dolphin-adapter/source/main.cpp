/****************************************************************************
 * fstadapter v2 - late-install caller + launch tail in Dolphin.
 *
 * v1 proved the real PlaceFst/InstallFst copy bytes correctly in
 * isolation. That does not reproduce the failing boot: the adapter
 * omitted the surrounding loader state. v2 executes the actual
 * late-install caller sequence (a line-referenced mirror of
 * Riivo::InstallPendingFst, RiivoBoot.cpp:2196-2328) around the REAL
 * InstallFst/PlaceFst/Crc32, plus the launch-tail shape (inter-phase
 * heap churn between staging and install, consumer reads through the
 * repointed words, pre-jump re-read mirroring GameBooter.cpp:1173-1185).
 *
 * Priority is the IN-PLACE vs RELOCATED difference, run back-to-back as
 * two separately-modeled boots in one process:
 *   RELOC   want 153934 -> grown at 0x817B2DE0, arena lowered (the T0 case)
 *   INPLACE want 153792 -> untouched at 0x817DA740, arena kept (the T1 case)
 *
 * Memory layout preserved: the boot-info block is set to the captured
 * SB4E01 T0 words before each mode (modeling the apploader-filled block),
 * the occupied list is the captured block + BSS + one synthetic stale
 * sample, and staging lives in MEM2 (Arena2 bump) across the churn phase
 * with a stage-time CRC, exactly the lifetime pendingFst has.
 *
 * SB4E01 startup "where feasible": no game ISO exists here, so the
 * consumer is a structural read of a synthetic VALID FST head (real U8
 * entries + string table, marker pad) through the repointed 0x80000038 /
 * 0x8000003C words - the feasible slice is "reads via the new pointer
 * stay in bounds", not game execution.
 *
 * See BYPASSES.md for the full bypass ledger. Not production code.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <gccore.h>

#include "RiivoFstInstall.hpp"
#include "RiivoReconcile.hpp" // real Crc32 (header-inline, shared with host tests)

extern "C" void gprintf(const char *str, ...)
{
	va_list ap;
	va_start(ap, str);
	vprintf(str, ap);
	va_end(ap);
}

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

//! Captured SB4E01 T0 boot words (v3.36+ card logs).
static const u32 CAP_ARENA_LO = 0;
static const u32 CAP_ARENA_HI = 0x817DA740;
static const u32 CAP_FST_ADDR = 0x817DA740;
static const u32 CAP_FST_MAX  = 153792;
//! Captured obstacles.
static const u32 CAP_BLOCK_LO = 0x817D8740;
static const u32 CAP_BLOCK_HI = 0x817DA740;
static const u32 CAP_BSS_LO   = 0x80728680;
static const u32 CAP_BSS_HI   = 0x807E3188;
//! Synthetic stale-table sample inside the captured reservation (skip-path
//! exercise, labeled synthetic).
static const u32 SYN_STALE_LO = 0x817DA800;
static const u32 SYN_STALE_HI = 0x817DA900;
//! The two modeled cases.
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

static inline u32 ReadSP(void)
{
	u32 v;
	__asm__ volatile("mr %0, 1" : "=r"(v));
	return v;
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

//! MEM2 staging: GX's MEM2_alloc (source/memory/mem2.cpp) is loader code,
//! so the adapter carves the same lifetime out of Arena2 directly with a
//! bump allocator and asserts the range. What matters is the lifetime
//! (MEM2, surviving MEM1 fills and the churn phase), not whose allocator.
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

//! Synthetic VALID FST head: real U8 entries + string table, marker pad.
//! entry0 root dir; entries 1..n files named f%05d; string table follows.
//! Returns the header length (entries + strings).
static u32 BuildMiniFst(u8 *buf, u32 size)
{
	u32 n = (size - 16 - 13) / 19;
	if (n < 2)
		n = 2;
	const u32 count = n + 1;
	char name[8];
	u32 strOff = 1; // string table starts with NUL (root name)
	wbe32(buf + 0, (1u << 24) | 0); // root: dir, name 0
	wbe32(buf + 4, 0);              // root parent placeholder
	wbe32(buf + 8, count);          // root next = entry count
	for (u32 i = 1; i <= n; ++i)
	{
		snprintf(name, sizeof(name), "f%05u", (unsigned) (i - 1));
		u8 *e = buf + 12 * i;
		wbe32(e + 0, strOff); // file, name offset
		wbe32(e + 4, 0x200000u + i * 0x10000u); // fake disc offset
		wbe32(e + 8, 0x10000u);                // fake length
		strOff += 1 + (u32) strlen(name);
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
	// Marker pad from the header end (same formula as v1, offset-absolute
	// so the GDB dump check stays trivially computable).
	for (u32 i = hdr; i < size; ++i)
		buf[i] = (u8) (0xA5 ^ (i & 0xFF) ^ ((i >> 8) & 0xFF));
	return hdr;
}

//! Feasible consumer slice: parse the installed table through the
//! repointed words, bounds only. Returns entry count, 0 on any fault.
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
	if (b[0] != 1) // root must be a directory
		return 0;
	const u32 strBase = 12 * count;
	for (u32 i = 1; i < count; ++i)
	{
		const u8 *e = b + 12 * i;
		if (e[0] != 0 && e[0] != 1)
			return 0;
		const u32 no = (rbe32(e) & 0x00FFFFFFu);
		if (no >= maxsize - strBase)
			return 0;
	}
	return count;
}

//! Mirror of Riivo::InstallPendingFst (RiivoBoot.cpp:2196-2328): same
//! order, same checks, same fail codes. The REAL InstallFst/PlaceFst data
//! and REAL Crc32 flow through it. Deviations from production are labeled
//! PROD-DELTA.
static int LateInstallMirror(const char *tag, const Riivo::FstPlacement &place,
							 u8 *stage, u32 size, u32 stagedCrc, bool relocated)
{
	printf("%s: late-install caller (InstallPendingFst mirror)\n", tag);
	const u32 addr = place.fstAddr;
	if (relocated)
	{
		// Production dirt scan (evidence only, never a verdict).
		const u32 curPtr = *(vu32 *) 0x80000038;
		const u32 curMax = *(vu32 *) 0x8000003C;
		if (curMax > 0 && curPtr >= 0x80000000u && curPtr < 0x81800000u
			&& curMax <= 0x81800000u - curPtr)
		{
			u32 dirtyAt = 0;
			int dirtyCount = 0;
			for (u32 p = addr; p < curPtr && dirtyCount < 8; ++p)
			{
				const u8 b = *(const volatile u8 *) p;
				if (b && dirtyCount == 0)
					dirtyAt = p;
				if (b || dirtyCount > 0)
					++dirtyCount;
			}
			printf("  relocation span below %08x: %s (first nonzero %08x)\n",
				   curPtr, dirtyCount ? "NONZERO HELD" : "all zeros", dirtyAt);
		}
	}
	printf("  pre-copy SP %08x break %08x\n", ReadSP(),
		   (u32) (uintptr_t) sbrk(0));
	if (Riivo::Crc32(stage, size) != stagedCrc)
	{
		printf("  REFUSED code 2 (staged checksum)\n");
		return 2;
	}
	const bool ok = Riivo::InstallFst(place, stage, size);
	u32 ptr = 0, max = 0, arena = 0;
	bool verified = false;
	int code;
	if (!ok)
		code = 3;
	else
	{
		ptr = *(vu32 *) 0x80000038;
		max = *(vu32 *) 0x8000003C;
		arena = *(vu32 *) 0x80000034;
		const bool bytesOk = (memcmp((const void *) addr, stage, size) == 0)
							 && Riivo::Crc32((const u8 *) addr, size) == stagedCrc;
		const bool ptrsOk = ptr == addr && max == size
							&& arena == place.newArenaHi;
		verified = bytesOk && ptrsOk;
		code = verified ? 0 : (bytesOk ? 5 : 4);
	}
	printf("  late FST install %s at %08x, %u bytes (ptr %08x max %u arena %08x)\n",
		   verified ? "verified" : (ok ? "UNVERIFIED" : "REFUSED"),
		   addr, (unsigned) size, ptr, max, arena);
	printf("  post-verify SP %08x break %08x\n", ReadSP(),
		   (u32) (uintptr_t) sbrk(0));
	return code;
}

static void WriteBootWords(u32 lo, u32 hi, u32 fst, u32 max)
{
	*(vu32 *) 0x80000030 = lo;
	*(vu32 *) 0x80000034 = hi;
	*(vu32 *) 0x80000038 = fst;
	*(vu32 *) 0x8000003C = max;
	DCFlushRange((void *) 0x80000030, 0x20);
}

//! One modeled boot. PROD-DELTA: heap-churn sizes stand in for the loader's
//! ShutDownDevices/gamepatches/memory-patch allocations (activity, not
//! exact sizes); the consumer parses the synthetic head, not a real FST.
static void RunMode(const char *tag, u32 want, u32 expDest, bool relocated,
					u8 *stage, u32 stagedCrc, u32 stageSize)
{
	printf("================ %s (want %u) ================\n", tag, want);
	// Model the apploader-filled block: reset words to captured.
	WriteBootWords(CAP_ARENA_LO, CAP_ARENA_HI, CAP_FST_ADDR, CAP_FST_MAX);

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
	const Riivo::FstPlacement place =
		Riivo::PlaceFst(arena, want, 32, occ, 3);
	printf("  plan: ok=%d inPlace=%d addr=%08x arenaHi=%08x reserved=%u "
		   "ignored=%u malformed=%u\n",
		   (int) place.ok, (int) place.inPlace, place.fstAddr,
		   place.newArenaHi, place.reserved, place.ignoredRanges,
		   place.malformedRanges);
	Check(place.ok, "placement accepted");
	Check(place.inPlace == !relocated, relocated ?
		  "placement is grown" : "placement is in-place");
	Check(place.fstAddr == expDest, "destination is the expected address");
	Check(place.ignoredRanges == 1, "one stale-table range skipped");
	Check(place.malformedRanges == 0, "no malformed ranges");

	// Self-overlap guards: destination vs MEM2 stages and vs the live
	// churn blocks (the loader-heap half of the model-A/B question on
	// OUR heap: break must sit far below the destination).
	const u32 destLo = place.fstAddr, destHi = place.fstAddr + want;
	Check(!Riivo::RangesOverlap(destLo, destHi, (u32) stage,
								(u32) (stage + stageSize)),
		  "destination clear of its MEM2 stage");
	Check((u32) (uintptr_t) sbrk(0) < destLo,
		  "loader break is below the destination");

	const int code = LateInstallMirror(tag, place, stage, want,
									   stagedCrc, relocated);
	Check(code == 0, "late install verified (code 0)");

	// Feasible consumer slice + pre-jump re-read.
	const u32 ptr = *(vu32 *) 0x80000038;
	const u32 max = *(vu32 *) 0x8000003C;
	const u32 count = CheckMiniFst(ptr, max);
	printf("  consumer: %u entries through repointed words\n", count);
	Check(count > 0, "installed table parses through new pointer");
	Check(Riivo::Crc32((const u8 *) destLo, want) == stagedCrc,
		  "pre-jump re-read matches staged CRC");
	if (!relocated)
		Check(*(vu32 *) 0x80000034 == CAP_ARENA_HI,
			  "in-place install leaves arena high untouched");
	else
		Check(*(vu32 *) 0x80000034 == expDest,
			  "relocated install lowers arena high to the table");
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
	printf("fstadapter v2: late-install caller + launch tail\n");
	printf("================================================\n");

	const u32 oLo = *(vu32 *) 0x80000030, oHi = *(vu32 *) 0x80000034;
	const u32 oFst = *(vu32 *) 0x80000038, oMax = *(vu32 *) 0x8000003C;
	printf("live boot words (saved, context only):\n  %08x %08x %08x %08x\n",
		   oLo, oHi, oFst, oMax);

	// Stage both tables in MEM2 first: the lifetime pendingFst has.
	printf("arena2 [%08x, %08x)\n", mem2Lo ? mem2Lo : (u32) SYS_GetArena2Lo(),
		   (u32) SYS_GetArena2Hi());
	u8 *stageReloc = Mem2Stage(RELOC_WANT);
	u8 *stageInplace = Mem2Stage(INPLACE_WANT);
	Check(stageReloc && stageInplace, "MEM2 staging allocated");
	Check((u32) stageReloc >= mem2Lo && (u32) stageInplace >= mem2Lo,
		  "stages live in Arena2, not MEM1");
	const u32 hdrR = BuildMiniFst(stageReloc, RELOC_WANT);
	const u32 hdrI = BuildMiniFst(stageInplace, INPLACE_WANT);
	DCFlushRange(stageReloc, RELOC_WANT);
	DCFlushRange(stageInplace, INPLACE_WANT);
	const u32 crcR = Riivo::Crc32(stageReloc, RELOC_WANT);
	const u32 crcI = Riivo::Crc32(stageInplace, INPLACE_WANT);
	printf("  reloc stage %08x hdr %u crc %08x\n", (u32) stageReloc, hdrR, crcR);
	printf("  inplace stage %08x hdr %u crc %08x\n",
		   (u32) stageInplace, hdrI, crcI);

	// Inter-phase heap churn (models loader allocations between staging
	// and late install). All freed except two small live blocks.
	printf("heap churn: break %08x -> ", (u32) (uintptr_t) sbrk(0));
	void *live1 = malloc(8192), *live2 = malloc(65536);
	for (int r = 0; r < 6; ++r)
	{
		void *a = malloc(1024 + (u32) r * 8192);
		void *b = malloc(262144);
		void *c = malloc(204800);
		if (a) memset(a, 0x5A + r, 1024 + (u32) r * 8192);
		if (b) memset(b, 0xA5, 262144);
		if (c) memset(c, 0x3C, 204800);
		free(c);
		free(b);
		free(a);
	}
	printf("%08x (live %08x %08x)\n", (u32) (uintptr_t) sbrk(0),
		   (u32) live1, (u32) live2);

	// Relocated first: the failing case is the priority.
	RunMode("RELOC", RELOC_WANT, EXP_RELOC, true,
			stageReloc, crcR, RELOC_WANT);
	RunMode("INPLACE", INPLACE_WANT, EXP_INPLACE, false,
			stageInplace, crcI, INPLACE_WANT);

	free(live1);
	free(live2);
	printf("================================================\n");
	if (checksFailed == 0)
		printf("RESULT: PASS (in-place and relocated both verify)\n");
	else
		printf("RESULT: FAIL (%d check(s))\n", checksFailed);
	printf("halting in place for debugger inspection.\n");
	while (1)
		VIDEO_WaitVSync();
	return 0;
}
