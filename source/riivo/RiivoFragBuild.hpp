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

	struct FragBuildStats
	{
		u32 files;         // placed successfully
		u32 failed;        // the filesystem would not give up their sectors
		u32 fragsBefore;   // entries the backup's own list occupied
		u32 fragsAfter;
		u32 sizeBefore;    // declared virtual disc, in sectors
		u32 sizeAfter;
		std::string firstFailure;

		FragBuildStats()
			: files(0), failed(0), fragsBefore(0), fragsAfter(0), sizeBefore(0),
			  sizeAfter(0) {}
	};

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
