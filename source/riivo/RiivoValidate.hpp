/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Pure validation of a rebuilt file table: expectations, independent
 * walk, suffix-compacted variant, placement collection, skip audit and
 * frag-region check. This is the window between "table serialised" and
 * the gate that switches the mod on - the code that must never kill a
 * boot silently, so every allocation failure withholds with a reason
 * instead of stopping with no log.
 *
 * Deliberately free of any console dependency (no WDVD, no card, no
 * log path, no clock): device-derived inputs cross as parameters, so
 * host tests and the Dolphin harness run THIS translation unit, not a
 * copy of it. Moved verbatim out of RiivoBoot.cpp; behavior unchanged.
 ***************************************************************************/
#ifndef RIIVO_VALIDATE_HPP_
#define RIIVO_VALIDATE_HPP_

#include <gctypes.h>
#include <map>
#include <string>
#include <vector>

#include "RiivoFstBuild.hpp"
#include "RiivoFst.hpp"
#include "RiivoFstWalk.hpp"
#include "RiivoFile.hpp"
#include "RiivoFragBuild.hpp"
#include "RiivoFragPlan.hpp"
#include "RiivoReconcile.hpp"

namespace Riivo
{
	//! Operation trace codes for *opTrace. The harness records which op
	//! threw; production passes NULL and skips the stores.
	enum ValidateOp
	{
		VOP_NONE = 0,
		VOP_EXPECT_RESERVE = 1,  // expectation vector reserve
		VOP_WALK = 2,            // independent walk open/check
		VOP_COMPACT = 3,         // compacted build
		VOP_COMPACT_WALK = 4,    // compacted walk
		VOP_STAGE = 5,           // staging copy
		VOP_COLLECT = 10,        // CollectPlaced
		VOP_PLAN = 11,           // FindSkips / PlanFragRegion
	};

	//! Everything the validation window reads that the caller owns.
	//! Device-derived values (offsets, sizes, verdicts) arrive resolved;
	//! nothing in here touches the disc, the card, or IOS.
	struct ValidateRequest
	{
		const FstBuilder *builder; // plain table already serialized
		const Fst *fst;            // parsed disc table (baseline)
		const std::vector<u8> *plainFst; // Serialize output (walked)
		const std::map<std::string, u64> *modOffsets;   // early placement
		const std::map<std::string, u32> *expectedModSizes;
		u32 fstReserve;            // apploader reservation, 0 when unknown
		u64 region;                // mod region start
		u64 modRegionStart;        // registered region start
		const std::vector<RedirectSpec> *redirects;
		const std::vector<CreatedFile> *created;
		const std::vector<RegRecord> *modRecords;
		const std::map<std::string, SkipReason> *modAddFails;
		u64 imageBytes;            // backup's declared size
		u32 sectorSize;            // drive geometry
		u32 usedFrags;             // game fragment entries

		ValidateRequest()
			: builder(0), fst(0), plainFst(0), modOffsets(0),
			  expectedModSizes(0), fstReserve(0), region(0), modRegionStart(0),
			  redirects(0), created(0), modRecords(0), modAddFails(0),
			  imageBytes(0), sectorSize(0), usedFrags(0) {}
	};

	//! Everything the window decided. Plain data; the caller formats the
	//! report text and owns all device contact.
	struct ValidateResult
	{
		bool oom;              // an allocation failed: refuse, do not trust
		bool expectedComplete; // every mod path had a registered placement
		bool fstWalkOK;        // independent walk passed
		u32 walkPaths;         // expectations checked
		std::string walkError; // populated only when !fstWalkOK
		bool useCompact;       // stage the compacted bytes, not plain
		bool compactOK;        // compacted build+walk passed
		u32 compactBytes;      // compacted size (0 unless compactOK)
		std::string compactWhy;// populated only when !compactOK
		u32 expectedPaths;     // expectations built (OOM-line evidence)
		std::vector<u8> staged;// staged bytes (compacted iff useCompact)
		std::vector<PlacedFile> placed;   // ascending-offset order
		std::vector<ModExtent> extents;
		FragPlan plan;         // region fit verdict
		std::vector<SkipRecord> modSkips;
		FstBuildStats stats;   // table stats matching the staged bytes

		ValidateResult()
			: oom(false), expectedComplete(false), fstWalkOK(false),
			  walkPaths(0), useCompact(false), compactOK(false),
			  compactBytes(0), expectedPaths(0) {}
	};

	//! Ask the builder where each mod file ended up, ascending by offset.
	//! Shared with the harness (was static in RiivoBoot.cpp; moved here
	//! verbatim so both run one definition).
	void CollectPlaced(const FstBuilder &builder, u64 region,
					  const std::vector<RedirectSpec> &redirects,
					  const std::vector<CreatedFile> &created,
					  std::vector<PlacedFile> &out);

	//! Flatten placed files to offset/length extents. Shared, moved verbatim.
	void ToExtents(const std::vector<PlacedFile> &in,
				   std::vector<ModExtent> &out);

	//! Case-folding slash-normalised disc path. Shared, moved verbatim
	//! from RiivoFile.cpp (pure string ops; the declaration stays in
	//! RiivoFile.hpp so existing callers are untouched).
	std::string NormaliseDiscPath(const std::string &path);

	//! Run the validation window. Never throws: every std::exception
	//! (including std::bad_alloc) sets res.oom and returns, so the
	//! caller can withhold with a reason instead of stopping silent.
	//! When opTrace is non-null it receives the ValidateOp in flight
	//! (plain stores, no allocation) for failure attribution.
	void ValidateTable(const ValidateRequest &req, ValidateResult &res,
					   int *opTrace);
}

#endif
