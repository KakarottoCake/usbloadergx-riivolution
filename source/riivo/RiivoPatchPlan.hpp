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
 * multi-segment files refuse explicitly (named limitation) until the segment
 * runtime lands. Boot-file (main.dol) patches refuse explicitly until the
 * patched boot view lands. Never silently apply a different operation.
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
	u64 discOffsetOrig;   // original disc byte offset (0 when created)
	u32 discLengthOrig;   // original disc length (0 when created)
	u32 finalSize;        // composed size honoring resize (bytes, u32)
	bool isNew;           // created (no disc entry, create=true)
	bool resize;          // last patch's resize flag (for reporting)
	bool create;          // last patch's create flag (for reporting)
	bool wholeFile;       // single External covering [0,finalSize) w/ src 0
	bool bootFile;        // main.dol executable patch (unsupported: refuse)
	std::vector<PlanSegment> segs; // in order, covering [0,finalSize)
	std::string external; // wholeFile only: the single external path

	PlannedFile()
		: discOffsetOrig(0), discLengthOrig(0), finalSize(0),
		  isNew(false), resize(true), create(false),
		  wholeFile(false), bootFile(false) {}
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
	bool hasBootFile;     // a main.dol patch was requested (refuse for now)
	bool hasPartial;      // a multi-segment or sub-range patch exists
	u64 totalFinalBytes;  // sum finalSize (checked, saturates w/ error)
	u32 wholeFileCount;   // files servable by the fragment runtime

	PatchPlan()
		: discNumber(-1), revision(-1),
		  hasBootFile(false), hasPartial(false),
		  totalFinalBytes(0), wholeFileCount(0) {}
};

//! True when disc names the Wii executable (Dolphin: bare "main.dol",
//! case-insensitive, no leading '/'; also accept "/main.dol" explicitly).
//! Boot-file patches refuse until the patched boot view lands; they must
//! never silently skip or route through the FST.
bool IsBootFileDisc(const std::string &disc);

//! Build the plan. Returns false + why on whole-plan refusal (FST needed
//! but unusable, arithmetic overflow, no filesystem provider, etc.).
//! Per-file skips (missing disc w/o create, missing external, invalid
//! paths) record warnings, leave the file original, and return true.
//! Unsupported enabled ops requiring refusal (boot-file, partial segments
//! for the fragment runtime, etc.) record errors; the caller must refuse
//! file work when !errors.empty(), never launch partial.
bool BuildPatchPlan(const Fst &fst,
					const ResolvedPatchSet &set,
					const std::string &device,
					DirLister *lister,
					FileSizeProvider *sizes,
					PatchPlan &out,
					std::string &why);

//! Manifest derivation: External + Zero segments become ManifestExtents
//! (Original gaps delegated, not listed). Sorted by discOffset via the
//! caller's assigned layout (see below); this emits file-relative runs,
//! the caller adds the file's assigned base. Returns false + why on
//! refusal (missing size already checked at plan time; overflow; bad path).
bool PlanToManifestRuns(const PlannedFile &file,
						std::vector<ManifestExtent> &out,
						std::string &why);

} // namespace Riivo

#endif
