/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * One coherent patch plan: a resolved, validated representation of the
 * selected mod that describes game identity, resolved externals, logical
 * files with final sizes, original-disc segments, external-file segments
 * with source offsets, explicit padding, created files/dirs, boot metadata,
 * memory phases, save requirements, and storage limits.
 *
 * Boot-time reads, FST serialization, runtime serving, validation, and
 * diagnostics consume this same plan or explicitly verified derivations of
 * it. Do not maintain independent layouts that happen to agree.
 *
 * Reference contracts (mechanisms, not inspiration):
 * - Dolphin DiscIO/RiivolutionPatcher.cpp::ApplyPatchToFile: compose a file
 *   from original-disc, external-file, and explicit zero-padding segments.
 *   Split existing sources at patch boundaries, discard overlapping, insert
 *   external with source-offset advancement, zero-pad short externals,
 *   retain/truncate original per resize, mask offset low 2 bits, length 0 =>
 *   external remainder, multiple mods to one file via successive application.
 * - Dolphin DiscIO/RiivolutionPatcher.cpp::ApplyFilePatchToFST: disc starting
 *   '/' => specific path; bare "main.dol" (case-insensitive) => executable;
 *   otherwise first basename match. Folders recurse; empty folder disc =>
 *   bare filenames => basename matching (dataless).
 * - Dolphin DiscIO/DirectoryBlob.cpp::BuildFST: rebuilt table + header
 *   0x424/0x428/0x42c in one coherent view.
 * - RawkSD launcher/source/riivolution.cpp::RVL_Patch + dipmodule/source/
 *   dip.cpp::Read: shift/patch registration ordering, overlapping-patch
 *   lookup, forward-or-merge, base-read failure propagates. Provenance not
 *   certified; mechanisms as reference only. Where source is binary-only,
 *   that limit is stated, not invented.
 *
 * Production status: FST sizes and manifest extents derive from this plan.
 * Fragment runtime serves whole-file single-segment files; partial
 * multi-segment files refuse explicitly (named limitation) until a segment
 * runtime lands for post-boot reads. Executable (main.dol) patches compose
 * here against the DOL image base and are served positionally by the boot
 * view during the apploader window (devices mounted, plan complete); the
 * served image must preserve the stock image size. Never silently apply a
 * different operation.
 *
 * Pure logic + injected filesystem (DirLister, FileSizeProvider), no console
 * calls, so host tests exercise this exact code. Checked 64-bit arithmetic,
 * explicit units (bytes throughout unless named), bounded vectors.
 ***************************************************************************/
#ifndef RIIVO_PATCH_PLAN_HPP_
#define RIIVO_PATCH_PLAN_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include <map>

#include "RiivoTypes.hpp"
#include "RiivoFst.hpp"
#include "RiivoFile.hpp"
#include "RiivoFragBuild.hpp" // PlacedFile (served-placement unit)

namespace Riivo
{

//! One contiguous run within a logical file, in file-offset order.
struct PlanSegment
{
	enum Kind
	{
		SEG_ORIGINAL, // bytes from the original disc file (gap: delegated)
		SEG_EXTERNAL, // bytes from an external file (path + srcOffset)
		SEG_ZERO      // explicit zero padding (expansion/short external)
	};

	Kind kind;
	u64 fileOffset;   // offset within the logical file (bytes)
	u32 length;       // bytes in this run (may be 0 only for empty files)
	u64 srcOffset;    // External only: offset within the external file
	std::string external; // External only: full device path (e.g. "sd:/...")

	PlanSegment() : kind(SEG_ORIGINAL), fileOffset(0), length(0), srcOffset(0) {}
};

//! One logical file after all patches targeting it compose in order.
struct PlannedFile
{
	std::string disc;     // lower-cased, leading '/', e.g. "/obj/a.arc"
	                      // (FST path for files; "/main.dol" for executables)
	std::string earlyKey; // pre-FST enumeration key (NormaliseDiscPath of the
	                      // rule disc / folder-joined child), kept as composer
	                      // metadata. Placement and layout both key on disc;
	                      // equals disc for ordinary and created entries.
	u64 discOffsetOrig;   // original disc byte offset (0 when created;
	                      // DOL image base for executable patches)
	u32 discLengthOrig;   // original disc length (0 when created)
	u32 finalSize;        // composed size honoring resize (bytes, u32)
	bool isNew;           // created (no disc entry, create=true)
	bool resize;          // last patch's resize flag (for reporting)
	bool create;          // last patch's create flag (for reporting)
	bool wholeFile;       // single External covering [0,finalSize) w/ src 0
	bool bootFile;        // main.dol executable patch (boot-view served)
	u64 dolBase;          // bootFile only: partition offset of the DOL image;
	                      // ORIGINAL runs read from disc at dolBase+fileOffset
	std::vector<PlanSegment> segs; // in order, covering [0,finalSize)
	std::string external; // wholeFile only: the single external path

	PlannedFile()
		: discOffsetOrig(0), discLengthOrig(0), finalSize(0),
		  isNew(false), resize(true), create(false),
		  wholeFile(false), bootFile(false), dolBase(0) {}
};

//! Provider of external file sizes, injected so host tests run with no FS.
//! Returns false when the file is absent or not a regular file.
struct FileSizeProvider
{
	virtual ~FileSizeProvider() {}
	virtual bool GetSize(const std::string &external, u32 *outSize) = 0;
};

//! stat()-backed provider (production). Regular files only.
struct StatFileSizeProvider : public FileSizeProvider
{
	virtual bool GetSize(const std::string &external, u32 *outSize);
};

//! The single plan for one launch. Built once from FST + resolved set;
//! consumed by FST serialization (finalSize), manifest derivation (segs),
//! fragment planning (wholeFile entries), validation, and diagnostics.
struct PatchPlan
{
	std::string gameId;   // 6-char ID when known, else empty
	int discNumber;       // -1 unknown (skip check, never 0-guess)
	int revision;         // -1 unknown
	std::vector<PlannedFile> files; // in disc-path sorted order (stable)
	std::vector<MissingExternal> missingExternals; // externals not on card
	std::vector<std::string> warnings; // skipped w/ reason (no patch applied)
	std::vector<std::string> errors;   // unsupported enabled ops (refuse)
	bool hasBootFile;     // a main.dol patch was requested (served iff
	                      // composed into outDol without errors; else refused)
	bool hasPartial;      // a non-DOL multi-segment or sub-range patch exists
	                      // (served by the on-demand segment runtime; the
	                      // fragment runtime stays whole-file-only and its
	                      // path still withholds these plans)
	u64 totalFinalBytes;  // sum finalSize (checked, saturates w/ error)
	u32 wholeFileCount;   // files servable by the fragment runtime

	PatchPlan()
		: discNumber(-1), revision(-1),
		  hasBootFile(false), hasPartial(false),
		  totalFinalBytes(0), wholeFileCount(0) {}
};

//! Executable routing predicate (bare "main.dol", Dolphin-exact) lives in
//! RiivoFile.hpp with the other shared path helpers; the executable
//! composition contract (segments against the DOL image, size-preserving
//! rule) is documented on BuildPatchPlan below.

//! DOL image span from a parsed section table: max(fileOff+size) over the
//! given sections, at least 0x100 (the header apploader reads imply). Pure,
//! host-tested; production fills the arrays from its DOL section table.
u64 DolImageSize(const u32 *fileOffs, const u32 *sizes, u32 count);

//! Build the plan. Returns false + why on whole-plan refusal (FST needed
//! but unusable, arithmetic overflow, no filesystem provider, etc.).
//! Per-file skips (missing disc w/o create, missing external, invalid
//! paths) record warnings, leave the file original, and return true.
//! Partial multi-segment non-DOL files set hasPartial (served by the
//! on-demand segment runtime from per-segment extents with ORIGINAL slices
//! staged alongside; callers without segments must withhold file work,
//! never launch partial). Executable entries compose into *outDol when outDol != 0 and
//! dolSize > 0 (ORIGINAL base [0,dolSize) backed by the DOL image;
//! resize forced size-preserving, finalSize must equal dolSize or errors
//! records why); when outDol == 0 they record errors as before, so existing
//! callers keep prior behavior.
//! Unsupported enabled ops record errors; the caller must refuse file work
//! when !errors.empty(), never launch partial.
bool BuildPatchPlan(const Fst &fst,
					const ResolvedPatchSet &set,
					const std::string &device,
					DirLister *lister,
					FileSizeProvider *sizes,
					PatchPlan &out,
					std::string &why,
					u64 dolSize = 0,
					PlannedFile *outDol = 0);

//! Manifest derivation: External + Zero segments become ManifestExtents
//! (Original gaps delegated, not listed). Sorted by discOffset via the
//! caller's assigned layout (see below); this emits file-relative runs,
//! the caller adds the file's assigned base. Returns false + why on
//! refusal (missing size already checked at plan time; overflow; bad path).
bool PlanToManifestRuns(const PlannedFile &file,
						std::vector<ManifestExtent> &out,
						std::string &why);

//! Staged RIV1 contract from plan + served placements: every placed file
//! must match a whole-file plan entry by external path and length; the
//! manifest then carries (offset, length, path, source) per placement with
//! zero source offsets. Executable entries take no fragments and never
//! appear; zero-length plan files have no placements. Sorted, built and
//! self-validated here; the caller logs the digest and withholds on false.
//! Pure (no console); production and host tests run this exact code, so the
//! emitted table cannot drift from the plan it contracts.
bool BuildPlanManifest(const PatchPlan &plan,
					   const std::vector<PlacedFile> &placed,
					   u32 discId,
					   std::vector<u8> &blob,
					   std::string &why);

//! Largest ORIGINAL-slice store the segment emitter stages. Partial files
//! preserve their untouched ranges as staged slices; the store is bounded
//! because it lives in the game's MEM2 reservation alongside the table.
//! Over it refuses with the total named - an explicit resource refusal,
//! never a silent truncation.
static const u32 RIV1_GEN_MAX = 8u << 20;

//! One ORIGINAL run staged for serving: bytes [fileOffset,
//! fileOffset+length) of the plan file come from the original disc at
//! origAbs, staged at genOff in the store. Units are PartitionBytes
//! throughout; origAbs is an original-disc offset, genOff a store offset.
struct GenSlice
{
	std::string disc;
	u64 fileOffset;
	u32 length;
	u64 origAbs;
	u32 genOff;

	GenSlice()
		: fileOffset(0), length(0), origAbs(0), genOff(0) {}
};

//! The staged-slice layout: deterministic (plan file order, seg order),
//! total checked against RIV1_GEN_MAX at build.
struct GenLayout
{
	std::vector<GenSlice> slices;
	u32 total;

	GenLayout() : total(0) {}
};

//! True when the plan needs the segment runtime: any non-executable file
//! the fragment runtime cannot serve whole (multi-segment composition).
//! The whole-file-table fallback is only equivalent when this is false;
//! falling back with it true would silently drop content, so the caller
//! must refuse activation instead. Pure.
inline bool PlanNeedsSegments(const PatchPlan &plan)
{
	for (size_t i = 0; i < plan.files.size(); ++i)
	{
		const PlannedFile &f = plan.files[i];
		if (!f.bootFile && !f.wholeFile)
			return true;
	}
	return false;
}

//! Staged RIV1 contract from plan + slot bases, covering whole AND partial
//! files. Whole files emit one EXTERNAL run each (srcOffset 0), identical
//! to BuildPlanManifest for the same inputs; partial files emit per-segment
//! runs (EXTERNAL with source offsets, ZERO) with ORIGINAL runs staged as
//! GENERATED slices in `gen` (bytes filled late from the disc, pre-boot) -
//! unless `scratchExternal` names a scratch file, in which case ORIGINAL
//! runs emit as EXTERNAL runs into it (srcOffset = genOff, same layout in
//! `gen` for the file writer) and no GENERATED run is produced at all.
//! Executable and zero-length entries never appear. `bases` maps plan disc
//! key -> slot base (PartitionBytes); `sizes` verifies every referenced
//! external (missing/short refuses naming the file). Sorted, built and
//! self-validated; the caller withholds on false. Pure (no console).
bool BuildSegmentManifest(const PatchPlan &plan,
						  const std::map<std::string, u64> &bases,
						  FileSizeProvider *sizes,
						  u32 discId, u32 partIdx,
						  std::vector<u8> &blob, GenLayout &gen,
						  std::string &why,
						  const std::string &scratchExternal = "");

} // namespace Riivo

#endif
