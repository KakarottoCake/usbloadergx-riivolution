/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 1: apply <memory> patches (direct / valuefile / search / ocarina) to
 * the loaded game binary, in the pre-entry window (after gamepatches, before
 * the DOL list is cleared and the game is launched).
 *
 * Semantics mirror Dolphin's RiivolutionPatcher.cpp:
 *   - effective address = (offset | 0x80000000)
 *   - direct: optional `original` byte-check, then write value/valuefile bytes
 *   - search: scan DOL sections (stride = align) for `original`, write value
 *   - ocarina: scan for `value` pattern, advance to next blr (0x4E800020),
 *              write branch ((target - blr) & 0x03FFFFFC) | 0x48000000
 ***************************************************************************/
#ifndef RIIVO_MEMORY_HPP_
#define RIIVO_MEMORY_HPP_

#include "RiivoTypes.hpp"

namespace Riivo
{
	//! Read every <memory valuefile=> into its patch's inline `value` and clear
	//! the valuefile field. MUST be called while SD/USB is still mounted: the
	//! loader shuts the devices down (ShutDownDevices) before the patch window,
	//! so a valuefile opened at apply time would always fail. `device` is the
	//! SD/USB mount prefix (e.g. "sd:") that valuefile paths resolve against,
	//! together with each patch's root. Returns how many could NOT be read.
	int PreloadValueFiles(ResolvedPatchSet &set, const std::string &device);

	//! Outcome of checking one <memory> patch without writing anything.
	//! HARD means the setup is broken (hold the whole set back); SOFT means
	//! this patch does not fit what is in RAM (skip it, apply the rest).
	enum MemCheck
	{
		MEM_CHECK_OK,
		MEM_CHECK_HARD_NO_VALUE,     // nothing to write (valuefile failed / empty)
		MEM_CHECK_HARD_BAD_TARGET,   // target outside RAM
		MEM_CHECK_HARD_NO_PATTERN,   // nothing to look for (empty search/ocarina pattern)
		MEM_CHECK_SOFT_ORIG_MISMATCH,// direct original= bytes differ
		MEM_CHECK_SOFT_NOT_FOUND,    // search/ocarina pattern absent
		MEM_CHECK_SOFT_OVERRUN       // match found but write would overrun / no blr
	};

	//! One checked patch. For ORIG_MISMATCH, want/got hold the first bytes
	//! expected and found (lengths are full sizes, snapshots capped at 16).
	//! For NOT_FOUND the want bytes name the missing pattern. Zeroed otherwise.
	struct MemOutcome
	{
		size_t index;      // position in set.memories
		const char *kind;  // "direct" | "search" | "ocarina" (string literals)
		u32 target;        // effective address (direct/ocarina target, search match, 0 if none)
		u32 writeAddr;     // address actually (to be) written, 0 if none
		u32 writeLen;      // bytes actually (to be) written, 0 if none
		MemCheck check;
		u32 wantLen;
		u8 want[16];
		u32 gotLen;
		u8 got[16];
	};

	//! Apply every <memory> patch in `set`. `device` is accepted for
	//! compatibility but no longer read: PreloadValueFiles must already have
	//! inlined every valuefile (devices are closed at apply time, so a
	//! reopen then would fail exactly the cases the preflight flags).
	//! Returns the number of patches successfully applied. Fills `appliedOut`
	//! with one outcome per patch, in set order, using the same checks as
	//! the preflight below - so a patch the preflight called OK but the
	//! apply skipped means bytes changed in between (loader patches run
	//! between the two), never a logic skew.
	int ApplyMemoryPatches(const ResolvedPatchSet &set, const std::string &device,
						   std::vector<MemOutcome> &appliedOut);

	//! Read-only preflight over set.memories: same checks as the apply path,
	//! in the same order, but never writes, protects, or flushes. Runs while
	//! the boot log is still writable so every skip lands in the persistent
	//! log with expected-versus-actual bytes. Fills `out` (cleared first)
	//! with one outcome per patch. Returns the hard-failure count.
	int PreflightMemoryPatches(const ResolvedPatchSet &set, std::vector<MemOutcome> &out);

	//! One line per patch plus a counts line, for the persistent boot log.
	std::string DescribeMemPreflight(const std::vector<MemOutcome> &out);

	//! How many outcomes are hard failures. Anything above zero holds back
	//! the entire memory set: a broken setup must boot unmodified rather
	//! than partially patched.
	int MemPreflightHardFails(const std::vector<MemOutcome> &out);

	//! Reservation-experiment completeness: every requested patch ran and
	//! reported success. Unlike VerifyAppliedPatches (which re-reads bytes
	//! and skips non-OK outcomes), this fails on any skip: a soft original
	//! mismatch or a missing outcome means the configuration going in is
	//! not the configuration requested, and a full-mod run must refuse
	//! rather than boot a mixture. Pure logic over the two vectors.
	bool AllMemoryPatchesOk(const ResolvedPatchSet &set,
							const std::vector<MemOutcome> &app);

	//! Compact post-shutdown summary: applied/total, skips by reason, and any
	//! patch whose outcome changed between preflight and apply (loader
	//! patches run between the two, so a change names bytes they touched).
	std::string DescribeMemApplySummary(const std::vector<MemOutcome> &pre,
										const std::vector<MemOutcome> &app, int applied);

	//! Position in `out` of the most recent OK outcome in [begin, end)
	//! whose written bytes overlap [addr, addr+len), or -1. Pure address
	//! arithmetic, no memory touched: order matters, so a later write over
	//! shared bytes supersedes an earlier one. The summary searches backward
	//! (which earlier write explains a later change); the verifier searches
	//! forward (a later write owns the final bytes, skip the earlier check).
	int FindOverlappingWrite(const std::vector<MemOutcome> &out,
							 size_t begin, size_t end, u32 addr, u32 len);

	//! Re-read every applied direct write and ocarina branch slot and compare
	//! against what was written. Pure reads: safe anywhere before the jump.
	//! `stage` labels the gprintf lines ("post-apply", "pre-jump"). Returns
	//! the mismatch count (also gprintf'd per mismatch, first bytes only).
	int VerifyAppliedPatches(const ResolvedPatchSet &set,
							 const std::vector<MemOutcome> &app, const char *stage);

	//! Branch word written by ocarina patches. Factored so the verifier
	//! compares against exactly what the patcher writes, not a second copy
	//! of the formula.
	u32 EncodeOcarinaBranch(u32 target, u32 blrAddr);
}

#endif
