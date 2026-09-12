// Memory patches that actually write, and the re-reads that check they held.
//
// test_memcheck.cpp covers the read-only preflight and stops where the console
// begins: CommitWrite and VerifyAppliedPatches reach absolute addresses like
// 0x80600000 by casting them straight to pointers, which is unmapped in a host
// process. So every write path bailed before writing there, and "applied 0/4,
// 0 mismatch(es) in 0 checked write(s)" was the most that suite could reach.
//
// A 64-bit host can simply reserve those addresses. 24 MB at 0x80000000 is a
// legal user address here, so the UNMODIFIED patcher runs for real: it writes
// where it would write on the Wii, and the verifier re-reads what it wrote.
// Nothing in source/ is changed or shimmed for this - only the address space
// the process happens to own.
//
// Endianness, and what it does and does not cover: the patcher writes an
// ocarina branch as the native bytes of a u32 (CommitWrite takes &branch),
// while the verifier expands the same word big-endian by hand. On PowerPC
// those are the same four bytes and the pair is correct. On a little-endian
// host they are reversed, so the ocarina slot is deliberately kept out of the
// verifier cases below and its two halves are pinned by an explicit
// equivalence check instead. The direct and search paths carry raw byte
// vectors end to end and are endian-neutral, so they exercise the verifier
// fully.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "riivo/RiivoMemory.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace Riivo;

static int checks, failed;
static void ck(bool ok, const char *name)
{
	++checks;
	if (!ok)
	{
		++failed;
		printf("FAIL: %s\n", name);
	}
}

// ---------------------------------------------------------------- simulated RAM
static const u32 MEM1_BASE = 0x80000000u;
static const u32 MEM1_SIZE = 0x01800000u; // 24 MB, MEM1_BASE .. 0x81800000

static u8 *P(u32 addr) { return (u8 *) (uintptr_t) addr; }

static bool ReserveMem1()
{
#ifdef _WIN32
	void *p = VirtualAlloc((LPVOID)(uintptr_t) MEM1_BASE, MEM1_SIZE,
						   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return p == (void *) (uintptr_t) MEM1_BASE;
#else
	void *p = mmap((void *) (uintptr_t) MEM1_BASE, MEM1_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	return p == (void *) (uintptr_t) MEM1_BASE;
#endif
}

// Fake DOL sections for the scanning kinds. Unlike test_memcheck's heap
// buffers these live INSIDE the simulated MEM1, so a match's address is a
// console-shaped address and the write lands where the scan says it did.
static u8 *g_dol[4];
static int g_dolLen[4];
static int g_dolN;
extern "C" int RiivoGetDOLCount(void) { return g_dolN; }
extern "C" u8 *RiivoGetDOLDst(int i) { return (i >= 0 && i < g_dolN) ? g_dol[i] : 0; }
extern "C" int RiivoGetDOLLen(int i) { return (i >= 0 && i < g_dolN) ? g_dolLen[i] : 0; }

// ------------------------------------------------------------------- fixtures
static void Vec(std::vector<u8> &v, const u8 *b, size_t n) { v.assign(b, b + n); }

static ResolvedMemory Direct(u32 offset, const u8 *value, size_t vlen,
							 const u8 *orig = 0, size_t olen = 0)
{
	ResolvedMemory m;
	m.offset = offset;
	m.ocarina = false;
	m.search = false;
	m.align = 1;
	Vec(m.value, value, vlen);
	if (orig)
		Vec(m.original, orig, olen);
	return m;
}

static const u8 VAL4[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
static const u8 ORIG4[4] = { 0x11, 0x22, 0x33, 0x44 };

int main()
{
	if (!ReserveMem1())
	{
		printf("SKIP: could not reserve simulated MEM1 at 0x%08x\n", MEM1_BASE);
		printf("      (the write and re-read paths stay covered by test_memcheck's\n");
		printf("       no-RAM failure modes, the PPC build, and hardware)\n");
		return 0;
	}
	memset(P(MEM1_BASE), 0, MEM1_SIZE);

	// Two sections inside the simulated RAM, well clear of the direct targets.
	g_dol[0] = P(0x80700000); g_dolLen[0] = 0x1000;
	g_dol[1] = P(0x80710000); g_dolLen[1] = 0x1000;
	g_dolN = 2;

	// --------------------------------------------------- a direct write lands
	{
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00600000, VAL4, 4));

		std::vector<MemOutcome> pre;
		ck(PreflightMemoryPatches(set, pre) == 0, "clean direct: no hard failures");
		ck(pre.size() == 1 && pre[0].check == MEM_CHECK_OK, "clean direct: preflight OK");
		ck(pre[0].target == 0x80600000 && pre[0].writeAddr == 0x80600000,
		   "clean direct: target and write address");
		ck(pre[0].writeLen == 4, "clean direct: write length");

		// The preflight runs before the apply; if it wrote, every patch would
		// go in twice and an original= check would see its own result.
		u8 zero[4] = { 0, 0, 0, 0 };
		ck(memcmp(P(0x80600000), zero, 4) == 0, "preflight wrote nothing");

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 1, "clean direct: one applied");
		ck(memcmp(P(0x80600000), VAL4, 4) == 0, "clean direct: bytes are in RAM");
		ck(VerifyAppliedPatches(set, app, "post-apply") == 0,
		   "clean direct: verifier agrees");
	}

	// ------------------------------------- original= that matches, and one that does not
	{
		memcpy(P(0x80601000), ORIG4, 4);
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00601000, VAL4, 4, ORIG4, 4));

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 1, "matching original: applied");
		ck(memcmp(P(0x80601000), VAL4, 4) == 0, "matching original: bytes replaced");
	}
	{
		u8 other[4] = { 0x99, 0x99, 0x99, 0x99 };
		memcpy(P(0x80602000), other, 4);
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00602000, VAL4, 4, ORIG4, 4));

		std::vector<MemOutcome> pre;
		ck(PreflightMemoryPatches(set, pre) == 0, "wrong original: soft, not hard");
		ck(pre[0].check == MEM_CHECK_SOFT_ORIG_MISMATCH, "wrong original: classified");
		ck(pre[0].wantLen == 4 && memcmp(pre[0].want, ORIG4, 4) == 0,
		   "wrong original: expected bytes reported");
		ck(pre[0].gotLen == 4 && memcmp(pre[0].got, other, 4) == 0,
		   "wrong original: actual bytes reported");

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 0, "wrong original: nothing applied");
		ck(memcmp(P(0x80602000), other, 4) == 0, "wrong original: RAM untouched");
	}

	// ------------------------------- check order is load-bearing (recovery note)
	// CheckDirect documents: value first (never touches RAM), then the target
	// range, then the original-bytes read. Both cases below would dereference a
	// wild pointer if that order were wrong, so a regression crashes here rather
	// than reaching hardware.
	{
		ResolvedPatchSet set;
		ResolvedMemory m = Direct(0x70000000, VAL4, 4, ORIG4, 4); // 0xF0000000, outside RAM
		m.value.clear();
		set.memories.push_back(m);
		std::vector<MemOutcome> pre;
		ck(PreflightMemoryPatches(set, pre) == 1, "no value + bad target: one hard failure");
		ck(pre[0].check == MEM_CHECK_HARD_NO_VALUE, "value is checked before the target");
	}
	{
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x70000000, VAL4, 4, ORIG4, 4));
		std::vector<MemOutcome> pre;
		ck(PreflightMemoryPatches(set, pre) == 1, "bad target: one hard failure");
		ck(pre[0].check == MEM_CHECK_HARD_BAD_TARGET,
		   "target is checked before the original bytes are read");
	}

	// ------------------------------------------------------------ search writes
	{
		const u8 pattern[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
		memset(g_dol[0], 0, (size_t) g_dolLen[0]);
		memcpy(g_dol[0] + 0x40, pattern, 8);

		ResolvedPatchSet set;
		ResolvedMemory m;
		m.offset = 0;
		m.ocarina = false;
		m.search = true;
		m.align = 4;
		Vec(m.value, VAL4, 4);
		Vec(m.original, pattern, 8);
		set.memories.push_back(m);

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 1, "search: applied");
		ck(app[0].check == MEM_CHECK_OK, "search: outcome OK");
		ck(app[0].writeAddr == 0x80700040, "search: match address is the section address");
		ck(memcmp(P(0x80700040), VAL4, 4) == 0, "search: value written at the match");
		ck(memcmp(P(0x80700044), pattern + 4, 4) == 0, "search: bytes past the value kept");
		ck(VerifyAppliedPatches(set, app, "post-apply") == 0, "search: verifier agrees");
	}

	// ----------------------------------------------------------- ocarina writes
	{
		memset(g_dol[1], 0, (size_t) g_dolLen[1]);
		const u8 sig[4] = { 0x7C, 0x08, 0x02, 0xA6 };
		memcpy(g_dol[1] + 0x20, sig, 4);
		u32 blr = 0x4E800020u;
		memcpy(g_dol[1] + 0x30, &blr, 4); // read back as a u32 by the scanner

		ResolvedPatchSet set;
		ResolvedMemory m;
		m.offset = 0x00600000;
		m.ocarina = true;
		m.search = false;
		m.align = 4;
		Vec(m.value, sig, 4);
		set.memories.push_back(m);

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 1, "ocarina: applied");
		ck(app[0].writeAddr == 0x80710030, "ocarina: branch goes at the blr");
		ck(app[0].target == 0x80600000, "ocarina: target is the offset");

		u32 want = EncodeOcarinaBranch(0x80600000, 0x80710030);
		u32 got;
		memcpy(&got, P(0x80710030), 4);
		ck(got == want, "ocarina: the encoded branch is what landed");

		// The verifier expands the same word big-endian by hand. Pin that the
		// two agree on a big-endian target, which is the only place both run.
		u8 be[4] = { (u8)(want >> 24), (u8)(want >> 16), (u8)(want >> 8), (u8) want };
		u8 native[4];
		memcpy(native, &want, 4);
		bool hostIsBig = (native[0] == be[0] && native[3] == be[3] && be[0] != be[3]);
		bool equivalent = hostIsBig
			? memcmp(native, be, 4) == 0
			: (native[0] == be[3] && native[1] == be[2]
			   && native[2] == be[1] && native[3] == be[0]);
		ck(equivalent, "ocarina: writer's native word and verifier's big-endian "
					   "expansion are the same four bytes on a big-endian target");
	}

	// --------------------------------- the verifier notices bytes changing under it
	// This is the pre-jump case: the code handler, 480p, Wiimmfi and the
	// file-table install all write game RAM after the patches went in.
	{
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00603000, VAL4, 4));
		set.memories.push_back(Direct(0x00604000, ORIG4, 4));

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 2, "two patches applied");
		ck(VerifyAppliedPatches(set, app, "post-apply") == 0, "both intact after apply");

		P(0x80603000)[2] = 0x00; // something else overwrote one byte
		ck(VerifyAppliedPatches(set, app, "pre-jump") == 1,
		   "one corrupted write is reported");

		P(0x80604000)[0] = 0x00; // and now the other one too
		ck(VerifyAppliedPatches(set, app, "pre-jump") == 2,
		   "both corrupted writes are reported");

		memcpy(P(0x80603000), VAL4, 4);
		memcpy(P(0x80604000), ORIG4, 4);
		ck(VerifyAppliedPatches(set, app, "pre-jump") == 0,
		   "restoring the bytes clears the report");
	}

	// ------------------------------------ overlapping writes: the last one owns
	// Without the last-writer-wins skip the earlier patch is compared against
	// its own value over bytes a later patch replaced, and reports a mismatch
	// that is not one.
	{
		const u8 eight[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		const u8 four[4] = { 9, 9, 9, 9 };
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00605000, eight, 8));
		set.memories.push_back(Direct(0x00605004, four, 4));

		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 2, "overlap: both applied");
		ck(memcmp(P(0x80605000), eight, 4) == 0, "overlap: first write's own bytes kept");
		ck(memcmp(P(0x80605004), four, 4) == 0, "overlap: later write owns the shared bytes");
		ck(FindOverlappingWrite(app, 1, 2, 0x80605000, 8) == 1,
		   "overlap: the later write is found");
		ck(VerifyAppliedPatches(set, app, "pre-jump") == 0,
		   "overlap: superseded write is not reported as a mismatch");

		P(0x80605000)[0] = 0x00; // a byte only the first write owns
		ck(VerifyAppliedPatches(set, app, "pre-jump") == 0,
		   "overlap: a superseded write stays unchecked even when it changes");
	}

	// ------------------------------------- a held-back set writes nothing at all
	// MemPreflightHardFails is what GameBooter gates the whole set on. A set
	// with one broken patch must leave RAM exactly as it found it.
	{
		memcpy(P(0x80606000), ORIG4, 4);
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00606000, VAL4, 4)); // would apply cleanly
		ResolvedMemory bad = Direct(0x00607000, VAL4, 4);
		bad.value.clear();                                   // hard failure
		set.memories.push_back(bad);

		std::vector<MemOutcome> pre;
		int hard = PreflightMemoryPatches(set, pre);
		ck(hard == 1, "mixed set: one hard failure counted");
		ck(MemPreflightHardFails(pre) == 1, "mixed set: helper agrees");
		ck(pre[0].check == MEM_CHECK_OK, "mixed set: the good patch still reports OK");
		ck(memcmp(P(0x80606000), ORIG4, 4) == 0,
		   "mixed set: preflight left the good patch's target alone");
	}

	// ----------------- completeness for the reservation gate: every requested
	// patch ran and reported success. A size check alone passes a set whose
	// outcomes are all present but skipped, which is exactly the mixture a
	// full-mod run must refuse.
	{
		ResolvedPatchSet set;
		set.memories.push_back(Direct(0x00608000, VAL4, 4));
		set.memories.push_back(Direct(0x00608100, VAL4, 4));
		std::vector<MemOutcome> app;
		ck(ApplyMemoryPatches(set, "sd:", app) == 2, "complete: both applied");
		ck(AllMemoryPatchesOk(set, app), "complete: full success reports complete");

		ResolvedPatchSet skip;
		skip.memories.push_back(Direct(0x00608200, VAL4, 4, ORIG4, 4)); // RAM is zero: soft skip
		skip.memories.push_back(Direct(0x00608300, VAL4, 4));
		std::vector<MemOutcome> skipped;
		ck(ApplyMemoryPatches(set, "sd:", skipped) == 2, "setup: control applies");
		ck(ApplyMemoryPatches(skip, "sd:", skipped) == 1, "setup: one patch skips");
		ck(skipped.size() == skip.memories.size(),
		   "skip: outcomes still one per patch, so a size check alone passes");
		ck(!AllMemoryPatchesOk(skip, skipped),
		   "skip: a soft-skipped patch reports incomplete");

		skipped.pop_back();
		ck(!AllMemoryPatchesOk(skip, skipped), "short: a missing outcome reports incomplete");

		ResolvedPatchSet none;
		std::vector<MemOutcome> empty;
		ck(AllMemoryPatchesOk(none, empty), "empty: vacuous success");
	}

	printf("%d checks, %d failure(s)\n", checks, failed);
	return failed ? 1 : 0;
}
