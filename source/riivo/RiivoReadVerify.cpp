/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <algorithm>
#include <stdint.h>
#include <string.h>

#include "RiivoReadVerify.hpp"

namespace Riivo
{
	struct FilePlan
	{
		bool full;
		bool failed;
		std::vector<u64> boundaries; // byte offsets relative to the file

		FilePlan() : full(false), failed(false) {}
	};

	static u64 RoundUp(u64 value, u32 alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	static bool LargerFile(const std::pair<u32, u32> &a,
					   const std::pair<u32, u32> &b)
	{
		if (a.first != b.first)
			return a.first > b.first;
		return a.second < b.second;
	}

	static void RecordFailure(const PlacedFile &file, FilePlan &plan,
						  u64 fileOffset, u64 discOffset,
						  u32 requestLength, u32 compareLength,
						  s32 readResult, int compareResult,
						  ReadVerifyKind kind, ReadVerifyStats &stats)
	{
		++stats.failedReads;
		if (plan.failed)
			return;
		if (stats.failureDetails.size() < READ_VERIFY_FAILURE_LIST_MAX)
		{
			ReadVerifyStats::FailureDetail detail;
			detail.path = file.external;
			detail.fileOffset = fileOffset;
			detail.discOffset = discOffset;
			detail.requestLength = requestLength;
			detail.compareLength = compareLength;
			detail.readResult = readResult;
			detail.compareResult = compareResult;
			detail.kind = kind;
			stats.failureDetails.push_back(detail);
		}
		plan.failed = true;
		++stats.failedFiles;
		if (stats.failureFiles.size() < READ_VERIFY_FAILURE_LIST_MAX)
			stats.failureFiles.push_back(file.external);
	}

	static void ReadOne(const PlacedFile &file, FilePlan &plan,
					const ReadVerifyCallbacks &callbacks,
					void *scratch, u64 fileOffset, u32 requestLength,
					u32 compareLength, ReadVerifyKind kind,
					ReadVerifyStats &stats)
	{
		if (kind == READ_VERIFY_FULL_FILE)
			++stats.fullReads;
		else
			++stats.boundaryReads;

		const u64 discOffset = file.offset + fileOffset;
		// A broken reader that returns success without writing must not compare
		// against bytes left by the preceding request.
		memset(scratch, 0xa5, requestLength);
		callbacks.pendingRead(callbacks.context, file.external.c_str(), fileOffset,
						  discOffset, requestLength, kind);
		// Keep this call adjacent to pendingRead: the device callback flushes the
		// pending record, and this is the synchronous operation that may hang.
		const s32 ret = callbacks.readDisc(callbacks.context, scratch,
									   requestLength, discOffset);
		int compare = READ_VERIFY_COMPARE_NOT_RUN;
		if (ret == 0)
			compare = callbacks.compareFile(callbacks.context,
				file.external.c_str(), fileOffset, scratch, compareLength);
		if (ret != 0 || compare != 0)
		{
			RecordFailure(file, plan, fileOffset, discOffset, requestLength,
						  compareLength, ret, compare, kind, stats);
			return;
		}

		stats.totalBytes += compareLength;
		if (requestLength > stats.largestRead)
			stats.largestRead = requestLength;
	}

	bool VerifyLargeReads(const std::vector<PlacedFile> &files,
					  const FragList &frags, u32 sectorSize,
					  void *scratch, u32 scratchSize,
					  const ReadVerifyCallbacks &callbacks,
					  ReadVerifyStats &stats)
	{
		stats = ReadVerifyStats();
		if (sectorSize < 512 || sectorSize > 4096
			|| (sectorSize & (sectorSize - 1)) || !scratch
			|| scratchSize < READ_VERIFY_CHUNK || frags.num > MAX_FRAG
			|| frags.num > frags.maxnum || frags.maxnum > MAX_FRAG
			|| ((uintptr_t)scratch & 31) || !callbacks.readDisc
			|| !callbacks.compareFile || !callbacks.pendingRead)
		{
			stats.fatal = "large-read verifier received invalid callbacks, geometry, or scratch buffer";
			return false;
		}

		std::vector<FilePlan> plans(files.size());
		std::vector<std::pair<u32, u32> > largest;
		largest.reserve(files.size());
		for (u32 f = 0; f < frags.num; ++f)
		{
			if ((u64)frags.frag[f].offset + frags.frag[f].count > 0x100000000ULL
				|| (u64)frags.frag[f].sector + frags.frag[f].count > 0x100000000ULL)
			{
				stats.fatal = "large-read verifier received an overflowing fragment";
				return false;
			}
		}

		for (u32 i = 0; i < files.size(); ++i)
		{
			const PlacedFile &file = files[i];
			if (!file.length)
				continue;
			if (file.offset % sectorSize)
			{
				stats.fatal = "large-read verifier received an unaligned file placement";
				return false;
			}

			const u64 firstSector = file.offset / sectorSize;
			const u64 sectorCount = ((u64)file.length + sectorSize - 1) / sectorSize;
			const u64 allocation = sectorCount * sectorSize;
			if (firstSector > 0xffffffffULL
				|| firstSector + sectorCount > 0x100000000ULL
				|| file.offset > ~0ULL - allocation)
			{
				stats.fatal = "large-read verifier received an unrepresentable file placement";
				return false;
			}
			largest.push_back(std::make_pair(file.length, i));

			const u64 endSector = firstSector + sectorCount;
			for (u32 f = 0; f < frags.num; ++f)
			{
				const u64 boundarySector = frags.frag[f].offset;
				// Strict inequalities are essential: a fragment belonging to the
				// next placed file is not evidence that this file is fragmented.
				if (!frags.frag[f].count || boundarySector <= firstSector
					|| boundarySector >= endSector)
					continue;
				const u64 relative = (boundarySector - firstSector) * sectorSize;
				// A transition in allocation slack is not file content and must not
				// cause either a full-file selection or a crossing read.
				if (relative >= file.length)
					continue;
				plans[i].boundaries.push_back(relative);
			}
			std::sort(plans[i].boundaries.begin(), plans[i].boundaries.end());
			plans[i].boundaries.erase(std::unique(plans[i].boundaries.begin(),
											 plans[i].boundaries.end()),
								 plans[i].boundaries.end());
			if (!plans[i].boundaries.empty())
			{
				plans[i].full = true;
				++stats.multiFragmentFiles;
			}
		}

		std::sort(largest.begin(), largest.end(), LargerFile);
		const u32 largestCount = std::min((u32)largest.size(),
									   (u32)READ_VERIFY_LARGEST_FILES);
		for (u32 i = 0; i < largestCount; ++i)
			plans[largest[i].second].full = true;

		for (u32 i = 0; i < files.size(); ++i)
		{
			const PlacedFile &file = files[i];
			FilePlan &plan = plans[i];
			if (plan.full)
			{
				++stats.fullFiles;
				for (u64 at = 0; at < file.length; at += READ_VERIFY_CHUNK)
				{
					const u32 valid = (u32)std::min((u64)READ_VERIFY_CHUNK,
											 (u64)file.length - at);
					const u32 request = (u32)RoundUp(valid, 32);
					ReadOne(file, plan, callbacks, scratch, at, request, valid,
							READ_VERIFY_FULL_FILE, stats);
				}
			}

			const u64 allocation = RoundUp(file.length, sectorSize);
			for (u32 b = 0; b < plan.boundaries.size(); ++b)
			{
				const u64 boundary = plan.boundaries[b];
				const u32 request = (u32)std::min((u64)READ_VERIFY_BOUNDARY,
											 RoundUp(file.length, 32));
				u64 start = boundary > request / 2 ? boundary - request / 2 : 0;
				start &= ~31ULL;
				if (start + request > allocation)
					start = (allocation - request) & ~31ULL;
				const u32 valid = (u32)std::min((u64)request,
										 (u64)file.length - start);
				// Both sides of the transition must be actual bytes of this file.
				if (boundary <= start || boundary >= start + valid)
					continue;
				ReadOne(file, plan, callbacks, scratch, start, request, valid,
						READ_VERIFY_FRAGMENT_BOUNDARY, stats);
			}
		}

		return stats.fatal.empty() && stats.failedReads == 0;
	}
}
