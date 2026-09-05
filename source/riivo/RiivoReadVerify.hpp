/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Bounded, host-testable verification of large LOW_READ requests.
 ***************************************************************************/
#ifndef RIIVO_READ_VERIFY_HPP_
#define RIIVO_READ_VERIFY_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

#include "RiivoFragBuild.hpp"

namespace Riivo
{
	static const u32 READ_VERIFY_CHUNK = 128 * 1024;
	static const u32 READ_VERIFY_BOUNDARY = 64 * 1024;
	static const u32 READ_VERIFY_LARGEST_FILES = 16;
	static const u32 READ_VERIFY_FAILURE_LIST_MAX = 24;
	static const int READ_VERIFY_COMPARE_NOT_RUN = 0x7fffffff;

	enum ReadVerifyKind
	{
		READ_VERIFY_FULL_FILE,
		READ_VERIFY_FRAGMENT_BOUNDARY
	};

	//! The callbacks deliberately contain no FILE or Wii types. `compareFile`
	//! compares exactly `length` bytes at `fileOffset` with `data`; zero means
	//! equal and any other value is a failure. `readDisc` follows WDVD_Read's
	//! convention: zero means success.
	//!
	//! `pendingRead` is called immediately before readDisc, with no operation
	//! between them. On the console it must write and flush the pending offset
	//! and length before returning, so the last log line identifies a hung IOS
	//! request.
	struct ReadVerifyCallbacks
	{
		void *context;
		s32 (*readDisc)(void *context, void *buffer, u32 length, u64 discOffset);
		int (*compareFile)(void *context, const char *path, u64 fileOffset,
						   const void *data, u32 length);
		void (*pendingRead)(void *context, const char *path, u64 fileOffset,
							u64 discOffset, u32 length, ReadVerifyKind kind);

		ReadVerifyCallbacks()
			: context(0), readDisc(0), compareFile(0), pendingRead(0) {}
	};

	struct ReadVerifyStats
	{
		u32 fullFiles;            // unique files selected for full verification
		u32 multiFragmentFiles;   // selected because of an internal boundary
		u32 fullReads;
		u32 boundaryReads;
		u32 failedFiles;          // unique files with one or more failed reads
		u32 failedReads;
		u64 totalBytes;           // file bytes successfully compared (overlap included)
		u32 largestRead;          // largest successful single LOW_READ request
		std::vector<std::string> failureFiles; // first 24 unique names

		struct FailureDetail
		{
			std::string path;
			u64 fileOffset;
			u64 discOffset;
			u32 requestLength;
			u32 compareLength;
			s32 readResult;
			int compareResult; // READ_VERIFY_COMPARE_NOT_RUN after a read error
			ReadVerifyKind kind;
		};
		std::vector<FailureDetail> failureDetails; // first 24 failed reads
		std::string fatal;

		ReadVerifyStats()
			: fullFiles(0), multiFragmentFiles(0), fullReads(0), boundaryReads(0),
			  failedFiles(0), failedReads(0), totalBytes(0), largestRead(0) {}
	};

	//! Verify the largest 16 files and every file with a fragment transition
	//! strictly inside its own allocation. Then make one explicit crossing read
	//! at every such transition that lies inside file content. `scratch` is
	//! caller-owned, at least READ_VERIFY_CHUNK bytes, and 32-byte aligned. The
	//! console caller supplies a MEM2 allocation; host tests may supply any
	//! aligned buffer.
	bool VerifyLargeReads(const std::vector<PlacedFile> &files,
					  const FragList &frags, u32 sectorSize,
					  void *scratch, u32 scratchSize,
					  const ReadVerifyCallbacks &callbacks,
					  ReadVerifyStats &stats);
}

#endif
