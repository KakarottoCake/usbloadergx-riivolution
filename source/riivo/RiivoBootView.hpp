/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Patched boot view: loader-owned PPC overlay serving coherent boot
 * metadata + file-table bytes to boot-time readers.
 *
 * Dolphin contract (DirectoryBlobPartition::BuildFST): rebuilt table +
 * header 0x424 (FST offset) / 0x428 (FST size) / 0x42c (FST max size) in one
 * coherent view; boot consumers read that view from the start. GX's
 * stock-view/late-table mismatch (apploader sees stock, table installed
 * afterward) drove per-game reservation experiments. This overlay is the
 * general fix's first half: header + FST coherence before the apploader.
 *
 * Lifetime explicit: armed after the plan is built (PrepareFileRedirects),
 * consulted by WDVD_Read while the partition is open and devices are mounted
 * (the apploader window), disarmed right after Apploader_Run returns and at
 * every fresh BeginLaunch. Never a filesystem dependency after devices shut
 * down; post-boot file reads use the IOS hook, not this. Inactive => every
 * consult returns false (stock behavior preserved bit-for-bit).
 *
 * What is served, and why each is safe:
 * - DOL image range [dolBase, dolBase+dolSize): composed positionally from
 *   the plan's executable segments. Apploader responses follow the stock
 *   layout and the served image preserves the stock image size exactly, so
 *   destinations stay valid while sources change. ORIGINAL runs fall back
 *   to the stock disc read at the same offset (identical bytes); EXTERNAL
 *   runs read the mod file at the recorded source offset; ZERO runs emit
 *   zeros. Requests are split across runs with per-run error propagation;
 *   any sub-read failure fails the whole read (the caller boots nothing on
 *   partial bytes). Reads past the composed end inside the image range fail
 *   loudly rather than mixing stock bytes into a patched image.
 * - FST range [fstOffset, fstOffset+stagedSize): staged rebuilt bytes, so a
 *   post-plan reader sees the same table the game will receive in MEM1.
 * - Header [0,0x440): stock bytes (production patches no header words; the
 *   disc image is immutable and MEM1 boot words carry the new table size).
 *   Retained as a served range so header reads are coherent and verifiable.
 * Stock fallback for everything else, including crossing requests outside
 * these ranges.
 *
 * Pure logic with injected readers (no console calls); host tests exercise
 * Serve + ServeDol + header patching byte-exactly, including splits,
 * source-offset advancement, fallback identity, and error propagation.
 * Checked u64 arithmetic, explicit units (partition bytes unless named).
 *
 * Pure logic, no console calls; host tests exercise Serve + header patching
 * byte-exactly. Checked u64 arithmetic, explicit units (partition bytes).
 ***************************************************************************/
#ifndef RIIVO_BOOT_VIEW_HPP_
#define RIIVO_BOOT_VIEW_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

#include "RiivoPatchPlan.hpp"

namespace Riivo
{

//! Partition header covering the FST words. Wii shift is 2 (word->byte).
static const u32 BOOTVIEW_HEADER_BYTES = 0x440;
static const u32 BOOTVIEW_FST_OFF = 0x424;
static const u32 BOOTVIEW_FST_SIZE = 0x428;
static const u32 BOOTVIEW_FST_MAX = 0x42c;
static const u32 BOOTVIEW_SHIFT = 2;

//! Read a big-endian u32 (disc header encoding) without alignment assumptions.
inline u32 BootViewReadBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

//! Write a big-endian u32.
inline void BootViewWriteBE32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)((v >> 16) & 0xff);
	p[2] = (u8)((v >> 8) & 0xff);
	p[3] = (u8)(v & 0xff);
}

//! Patch a stock header image with rebuilt FST placement (offset/size/max
//! in words, i.e. bytes>>SHIFT). Returns false + why when the stock image
//! is too short or the rebuilt values overflow the word field.
bool PatchBootHeader(const u8 *stockHeader, u32 stockLen,
					 u64 fstOffsetBytes, u32 fstSizeBytes, u32 fstMaxBytes,
					 std::vector<u8> &outPatched, std::string &why);

//! Byte sources for DOL serving, injected so host tests run with no
//! console or card. Each must transfer EXACTLY the requested bytes or
//! report failure; short transfers are failures, never partial use.
struct BootReaders
{
	bool (*stock)(u64 absOff, u8 *dst, u32 len, void *ctx);
	bool (*fat)(const std::string &path, u64 srcOff, u8 *dst, u32 len, void *ctx);
	void *ctx;
	BootReaders() : stock(0), fat(0), ctx(0) {}
};

//! Loader-owned overlay state. Activate copies header + FST (bounded: header
//! 0x440 + table bytes, never whole-mod payloads). Serve answers only
//! fully-contained header/FST reads; all else returns false (stock path).
class BootView
{
public:
	BootView();

	//! Activate with the stock header image (for fallback bytes outside
	//! 0x424-42f) plus the patched header to serve. Copies the header
	//! (bounded, 0x440). Returns false + why when the images are too short.
	//! Clears any FST/DOL coverage: call Activate first, then ArmFst, then
	//! SetDol, so overlap is always checked against the final ranges.
	bool Activate(const u8 *stockHeader, u32 stockLen,
				  const std::vector<u8> &patchedHeader,
				  std::string &why);

	//! Arm FST coverage: serve the given table bytes at fstOffsetBytes.
	//! The served size MUST equal the ADVERTISED size
	//! (advertisedSizeBytes): the header the apploader consumed names that
	//! size, so a reader asking for the whole table gets exactly a whole
	//! table - never a truncated prefix (shorter staging) nor bytes no
	//! disc reader asked for (longer staging). In production the advertised
	//! size is the disc size, except under a grown virtual header where it
	//! is the staged size. Refuses (false + why, header/DOL coverage
	//! untouched) on mismatch, empty table, oversize copy, unaligned
	//! offset, or overlap with armed DOL coverage.
	bool ArmFst(u64 fstOffsetBytes, const std::vector<u8> &patchedFst,
				u32 advertisedSizeBytes, std::string &why);

	bool FstArmed() const { return active && !fst.empty(); }

	//! True when active (consulted). False => stock behavior, Serve refuses.
	bool Active() const { return active; }

	//! Partition-byte offset of the served FST (0 when inactive).
	u64 FstOffset() const { return fstOffset; }

	//! Serve a partition-byte read [offset, offset+length) into `buffer`.
	//! Returns true + copies when the request lies fully inside the header
	//! or FST coverage; false leaves `buffer` untouched (caller does stock).
	//! Zero-length reads return false (nothing to serve). No partial splits:
	//! crossing requests fall back to stock (the DOL splitter below owns
	//! multi-run reads; header/FST readers always ask exactly).
	bool Serve(u64 offset, u8 *buffer, u32 length) const;

	//! Read routing verdict: classifies a request WITHOUT touching storage,
	//! so the production read path and host tests share one decision.
	//! DOL touches route to ServeDol (or FAIL when straddling the image
	//! edge - a patched image must never mix stock bytes); fully-contained
	//! header/FST reads route to Serve; everything else falls through to
	//! stock. Zero-length or disarmed views route to stock. Pure.
	enum RouteVerdict
	{
		ROUTE_STOCK, // caller does the stock read (buffer untouched)
		ROUTE_DOL,   // serve via ServeDol (may still fail -> caller aborts)
		ROUTE_META,  // serve via Serve (header/FST, fully contained)
		ROUTE_FAIL   // fail loudly: caller must boot nothing
	};
	RouteVerdict Route(u64 offset, u32 length) const;

	//! Arm DOL coverage from a composed executable plan. Requires a bootFile
	//! plan whose segments tile [0,size) contiguously, a nonzero image size,
	//! a base at/above the header, no u64 overflow, and no overlap with the
	//! header or an already-active FST range. Returns false + why otherwise
	//! (state left disarmed for DOL). Production assigns dol.dolBase before
	//! calling; the plan copy is retained (small: segments only, no payload).
	bool SetDol(u64 base, u64 size, const PlannedFile &dol, std::string &why);

	bool HasDol() const { return hasDol; }
	u64 DolBase() const { return dolBase; }
	u64 DolSize() const { return dolSize; }

	//! Serve a DOL-range read through the composed segments, splitting across
	//! runs: ORIGINAL runs fall back to the stock disc read at the same
	//! absolute offset; EXTERNAL runs read the mod file at srcOffset plus the
	//! intra-run position; ZERO runs emit zeros. Returns false (with *doneOut
	//! = bytes completed, for diagnostics) when the request is outside the
	//! DOL range, readers are null, or any sub-read fails; the caller must
	//! discard the buffer on false, never boot partial bytes.
	bool ServeDol(u64 offset, u8 *buffer, u32 length,
				  const BootReaders &r, u32 *doneOut) const;

	//! Forget everything (explicit lifetime end). Idempotent.
	void Deactivate();

private:
	bool active;
	std::vector<u8> header; // BOOTVIEW_HEADER_BYTES (patched)
	u64 fstOffset;
	std::vector<u8> fst;    // patched table bytes
	bool hasDol;
	u64 dolBase;
	u64 dolSize;
	PlannedFile dol;        // composed executable (segments + paths only)
};

} // namespace Riivo

#endif
