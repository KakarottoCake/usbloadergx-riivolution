/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Pure validation window (see the header for the contract). Moved
 * verbatim out of RiivoBoot.cpp::PrepareFileRedirects; the only
 * changes are mechanical: inputs arrive as parameters, outcomes leave
 * in the result struct, and report text stays with the caller (which
 * owns the log). Behavior is unchanged - host tests pin the bytes.
 ***************************************************************************/
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include "RiivoValidate.hpp"

namespace Riivo
{
	static bool ByPlacedOffset(const PlacedFile &a, const PlacedFile &b)
	{
		return a.offset < b.offset;
	}

	void CollectPlaced(const FstBuilder &builder, u64 region,
					  const std::vector<RedirectSpec> &redirects,
					  const std::vector<CreatedFile> &created,
					  std::vector<PlacedFile> &out)
	{
		out.clear();

		//! Keyed by disc path, because the same disc file can legitimately be
		//! claimed more than once: a mod with overlapping <folder> rules - Newer
		//! SMBW has 38 of them - names some files twice. Both claims resolve to
		//! the same assigned offset, so emitting both put two extents at the
		//! same place and the whole plan was refused with "two files overlap".
		//! Last one wins, matching how AddOrReplace resolved it when the table
		//! was built, so the file that serves the read is the file the game's
		//! table describes.
		std::map<std::string, PlacedFile> byDisc;

		for (size_t i = 0; i < redirects.size() + created.size(); ++i)
		{
			const bool isRedirect = i < redirects.size();
			const std::string &disc = isRedirect
									  ? redirects[i].disc
									  : created[i - redirects.size()].disc;
			const std::string &ext = isRedirect
									 ? redirects[i].external
									 : created[i - redirects.size()].external;
			u64 off = 0;
			u32 len = 0;
			if (!builder.FindAssigned(disc, &off, &len))
				continue;
			if (len == 0 || off < region)
				continue;
			PlacedFile f;
			f.offset = off;
			f.length = len;
			f.external = ext;
			byDisc[disc] = f;
		}

		out.reserve(byDisc.size());
		for (std::map<std::string, PlacedFile>::const_iterator it = byDisc.begin();
			 it != byDisc.end(); ++it)
			out.push_back(it->second);

		std::sort(out.begin(), out.end(), ByPlacedOffset);
	}

	void ToExtents(const std::vector<PlacedFile> &in,
				   std::vector<ModExtent> &out)
	{
		out.clear();
		out.reserve(in.size());
		for (size_t i = 0; i < in.size(); ++i)
		{
			ModExtent e;
			e.offset = in[i].offset;
			e.length = in[i].length;
			out.push_back(e);
		}
	}

	std::string NormaliseDiscPath(const std::string &path)
	{
		std::string out;
		size_t i = 0;
		while (i < path.size())
		{
			while (i < path.size() && path[i] == '/')
				++i;
			size_t j = i;
			while (j < path.size() && path[j] != '/')
				++j;
			if (j > i)
			{
				out += '/';
				for (size_t k = i; k < j; ++k)
					out += (char) tolower((unsigned char) path[k]);
			}
			i = j;
		}
		return out;
	}

	static void TraceOp(int *opTrace, int op)
	{
		if (opTrace)
			*opTrace = op;
	}

	void ValidateTable(const ValidateRequest &req, ValidateResult &res,
					   int *opTrace)
	{
		res = ValidateResult();
		if (!req.builder || !req.fst || !req.plainFst || !req.modOffsets ||
			!req.expectedModSizes || !req.redirects || !req.created ||
			!req.modRecords || !req.modAddFails)
			return;

		const FstBuilder &builder = *req.builder;
		const std::vector<u8> &newFst = *req.plainFst;
		const u64 region =
			req.modOffsets->empty() ? req.region : req.modRegionStart;

		try
		{
			//! Stats matching the pre-call plain Serialize, captured first:
			//! refusal paths report these (same values production's
			//! walk-refusal output shows); the compaction choice below
			//! overwrites them when it stages compacted bytes.
			res.stats = builder.Stats();
			//! Reserved up front, INSIDE the guard: at Spectral scale the
			//! expectation list is ~6300 paths, and growing it by repeated
			//! reallocation fragments the heap right before the two FST
			//! walks and the compaction buffer. (Churn reduction only.)
			TraceOp(opTrace, VOP_EXPECT_RESERVE);
			std::vector<FstWalkExpectation> expectedFst;
			expectedFst.reserve(req.fst->FileCount() +
								req.expectedModSizes->size());
			for (size_t i = 0; i < req.fst->FileCount(); ++i)
			{
				const FstFile &original = req.fst->FileAt(i);
				if (req.expectedModSizes->find(original.path) ==
					req.expectedModSizes->end())
					expectedFst.push_back(FstWalkExpectation(
						original.path, original.offset, original.length));
			}
			bool expectedComplete = true;
			for (std::map<std::string, u32>::const_iterator it =
					 req.expectedModSizes->begin();
				 it != req.expectedModSizes->end(); ++it)
			{
				std::map<std::string, u64>::const_iterator offset =
					req.modOffsets->find(it->first);
				if (offset == req.modOffsets->end())
				{
					expectedComplete = false;
					continue;
				}
				expectedFst.push_back(FstWalkExpectation(
					it->first, offset->second, it->second));
			}
			res.expectedComplete = expectedComplete;
			res.expectedPaths = (u32) expectedFst.size();
			TraceOp(opTrace, VOP_WALK);
			FstWalk gameWalk;
			std::string walkError;
			res.fstWalkOK = expectedComplete && !newFst.empty() &&
				gameWalk.Open(&newFst[0], newFst.size(), true, &walkError) &&
				gameWalk.Check(expectedFst, &walkError);
			res.walkPaths = (u32) expectedFst.size();
			if (!res.fstWalkOK)
				res.walkError = walkError;

			//! Suffix-compacted variant of the same tree (same entries,
			//! paths, offsets and sizes; shared string tails stored once).
			//! If the plain table outgrows the apploader's reservation but
			//! the compacted one fits, the caller stages the compacted
			//! bytes instead: they install in place, out of reach of the
			//! startup clearing below the reservation that kills relocated
			//! tables on SB4E01. Anything else keeps today's bytes exactly
			//! - unknown reservation, fitting plain table, failed build,
			//! failed walk, or still-overflowing compaction.
			bool useCompact = false;
			//! Stats matching the plain bytes, captured before the attempt
			//! below overwrites them: on a kept-plain outcome the report
			//! must show plain stats (production re-serializes for this;
			//! restoring the saved copy is the same values with no extra
			//! 230 KB rewrite).
			const FstBuildStats plainStats = builder.Stats();
			if (req.fstReserve > 0 && res.fstWalkOK &&
				newFst.size() > req.fstReserve)
			{
				TraceOp(opTrace, VOP_COMPACT);
				std::vector<u8> compactFst;
				bool compactOK =
					builder.SerializeCompacted(compactFst, true);
				std::string cwErr;
				FstWalk cw;
				TraceOp(opTrace, VOP_COMPACT_WALK);
				if (compactOK)
					compactOK = !compactFst.empty() &&
						cw.Open(&compactFst[0], compactFst.size(), true,
								&cwErr) &&
						cw.Check(expectedFst, &cwErr);
				if (!compactOK && cwErr.empty())
					cwErr = "build failed";
				useCompact = compactOK &&
					compactFst.size() <= req.fstReserve;
				res.compactOK = compactOK;
				res.compactBytes =
					compactOK ? (u32) compactFst.size() : 0;
				if (!compactOK)
					res.compactWhy = cwErr;
				if (useCompact)
					res.staged.swap(compactFst);
				// Plain path: staged stays empty and the caller keeps its
				// own plain bytes (no extra 230 KB copy on the common path).
			}
			else
			{
				TraceOp(opTrace, VOP_STAGE);
			}
			res.useCompact = useCompact;

			//! Every modded entry must have been given an offset in SetupDisc.
			//! One that was not is pointed at whatever happens to be there, so
			//! the whole table has to be refused.
			TraceOp(opTrace, VOP_COLLECT);
			std::vector<PlacedFile> placed;
			CollectPlaced(builder, region, *req.redirects, *req.created,
						  placed);

			TraceOp(opTrace, VOP_PLAN);
			std::vector<ModExtent> extents;
			ToExtents(placed, extents);

			//! Registration records that never became placed entries, with
			//! reasons. A tail-recovered offset landing on one of these is
			//! verified through its retained record instead of refused
			//! anonymously - nothing reads those offsets, so they are
			//! harmless, but the bytes still get proven (see Activate).
			std::vector<u64> lateOffsets;
			lateOffsets.reserve(placed.size());
			for (size_t i = 0; i < placed.size(); ++i)
				lateOffsets.push_back(placed[i].offset);
			std::map<std::string, char> hasRedirect;
			for (size_t i = 0; i < req.redirects->size(); ++i)
				hasRedirect[NormaliseDiscPath((*req.redirects)[i].disc)] = 1;
			for (size_t i = 0; i < req.created->size(); ++i)
				hasRedirect[NormaliseDiscPath((*req.created)[i].disc)] = 1;
			std::vector<SkipRecord> modSkips;
			FindSkips(*req.modRecords, lateOffsets, region, hasRedirect,
					  *req.modAddFails, modSkips);

			//! Feed the placed files through the same checks that would gate a
			//! fresh layout: ordering, alignment, the read ceiling, the table.
			res.plan = PlanFragRegion(req.imageBytes, req.sectorSize,
									  req.usedFrags, extents);
			res.placed.swap(placed);
			res.extents.swap(extents);
			res.modSkips.swap(modSkips);
			res.stats = useCompact ? builder.Stats() : plainStats;
			TraceOp(opTrace, VOP_NONE);
		}
		catch (const std::exception &)
		{
			res.oom = true;
		}
		catch (...)
		{
			res.oom = true;
		}
	}
}
