/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Turn the mod's files into fragments the cIOS can read.
 *
 * RiivoFragPlan decides WHERE on the virtual disc each file goes; this does the
 * looking-up. For every placed file it asks the filesystem driver which real
 * sectors of the drive hold that file, then re-bases those sectors onto the
 * file's virtual disc offset and appends them to the master fragment list -
 * exactly what get_frag_list_for_file already does for the backup itself, only
 * aimed at ordinary files instead.
 *
 * Two things make this work at all, and both are worth stating:
 *
 *  - The mod's files must be on the SAME partition as the backup. The fragment
 *    list holds bare sector numbers on one device; there is nowhere to record
 *    which device a fragment came from. A mod on the SD card and a game on USB
 *    cannot be combined, and this refuses rather than reading nonsense.
 *
 *  - The offset a fragment is filed under is the offset the game will ask for.
 *    The cIOS adds config.offset[0]+config.offset[1] before the lookup and both
 *    are zero here (see RiivoFragPlan.hpp), so the mapping is the identity.
 *
 * Target only: it calls into libfat/ntfs/ext. The arithmetic it relies on is
 * covered by the RiivoFragPlan tests on the host side.
 ***************************************************************************/
#ifndef RIIVO_FRAG_BUILD_HPP_
#define RIIVO_FRAG_BUILD_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include "usbloader/frag.h"

namespace Riivo
{
	//! A mod file and the place on the virtual disc it was given.
	struct PlacedFile
	{
		u64 offset;           // byte offset on the virtual disc
		u32 length;
		std::string external; // where it really is, e.g. "usb1:/Spectral/x.arc"
	};

	//! One failing file and its numbers. Collected for every failure up to
	//! FRAG_FAIL_LIST_MAX; `failed` in the stats remains the total.
	struct FragFailEntry
	{
		std::string path;
		int code;       // a FragFailCode below
		u32 length;     // stat size in bytes
		u32 limit;      // sectors expected: ceil(length / sectorSize)
		u32 next;       // sectors the driver actually covered
		int driverRet;  // what _X_get_fragments returned
		int probe;      // SHORT verdict: -1 unprobed, 0 short on the card,
						// 1 driver under-reported the file
	};

	//! How many failing files the log names before falling back to a count.
	static const u32 FRAG_FAIL_LIST_MAX = 16;

	struct FragBuildStats
	{
		u32 files;         // placed successfully
		u32 failed;        // the filesystem would not give up their sectors
		u32 fragsBefore;   // entries the backup's own list occupied
		u32 fragsAfter;
		u32 sizeBefore;    // declared virtual disc, in sectors
		u32 sizeAfter;
		std::string firstFailure; // full sentence, with the numbers (never a bare path)

		//! Why the first file failed, and the numbers that prove it. One log
		//! line cannot distinguish a file-specific oddity from a systematic
		//! arithmetic error, so every refusal carries these.
		int failCode;      // a FragFailCode below, 0 when nothing failed
		u32 failLength;    // stat size of the failed file, in bytes
		u32 failLimit;     // sectors expected from it: ceil(length / sectorSize)
		u32 failNext;      // sectors the driver actually covered
		int failDriverRet; // what _X_get_fragments returned for it
		u32 failCbOffset;  // callback args that fired the error, if any
		u32 failCbSector;
		u32 failCbCount;
		u32 failSectorSize; // drive sector size the limit was computed with
		int failProbe;     // SHORT verdict: -1 unprobed, 0 short on the card,
						   // 1 driver under-reported the file
		std::string failPath;

		//! Every failing file, bounded: the first FRAG_FAIL_LIST_MAX in file
		//! order, so the log names the files to re-copy instead of only the
		//! first. `failed` above remains the total; the difference is the
		//! count beyond the list.
		std::vector<FragFailEntry> failList;

		FragBuildStats()
			: files(0), failed(0), fragsBefore(0), fragsAfter(0), sizeBefore(0),
			  sizeAfter(0), failCode(0), failLength(0), failLimit(0), failNext(0),
			  failDriverRet(0), failCbOffset(0), failCbSector(0), failCbCount(0),
			  failSectorSize(0), failProbe(-1) {}
	};

	//! Distinct refusal reasons. The callback used to collapse four of these
	//! into one code, which made 1 failed file and 800 look identical.
	enum FragFailCode
	{
		FRAG_FAIL_NONE = 0,
		FRAG_FAIL_DRIVER = 1,      // the filesystem driver returned non-zero
		FRAG_FAIL_GAP = 2,         // a hole or overlap in the reported runs
		FRAG_FAIL_ZERO_SECTOR = 3, // the driver reported sector 0
		FRAG_FAIL_SECTOR_OVERFLOW = 4, // drive sector number does not fit 32 bits
		FRAG_FAIL_DISC_OVERFLOW = 5,   // virtual-disc sector number does not fit 32 bits
		FRAG_FAIL_SHORT = 6,       // runs covered fewer sectors than the file needs
		FRAG_FAIL_TABLE_FULL = 7   // the cIOS fragment table filled up (fatal)
	};

	//! One log-ready line describing the first failure, or empty when none.
	std::string DescribeFragFailure(const FragBuildStats &stats);

	//! Every collected failure as log-ready lines, plus a trailing count when
	//! the total runs past FRAG_FAIL_LIST_MAX. Empty when nothing failed.
	std::string DescribeFragFailList(const FragBuildStats &stats);

	//! Append every file in `files` to the master fragment list. `files` must be
	//! in ascending offset order - PlanFragRegion checks that. `fsType` is a
	//! PART_FS_* constant and `lbaOffset` the partition's start, both for the
	//! partition the game and the mod share.
	//! Returns false if the list could not be completed, in which case it has
	//! been left partially extended and must not be registered.
	bool AppendModFragments(const std::vector<PlacedFile> &files, u32 sectorSize,
							u8 fsType, u32 lbaOffset, FragBuildStats &stats);

	//! Read the first bytes of `file` back THROUGH the cIOS at `discOffset` and
	//! check they match the file on the card. This proves the whole chain -
	//! placement, sector lookup, re-basing, registration - in one go, before
	//! anything irreversible is done. Returns false and fills `why` otherwise.
	bool VerifyModFragment(u64 discOffset, u32 length, const std::string &file, std::string &why);
}

#endif
