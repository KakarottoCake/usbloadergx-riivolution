/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Where the mod's files sit on the virtual disc, and whether they fit.
 *
 * The cIOS reads the game through a fragment list: a table mapping offsets on
 * a virtual disc to real sectors on the drive. Anything inside the declared
 * size but not covered by a fragment reads back as zeros, and anything past
 * the declared size is an error. So placing the mod is a matter of extending
 * that table - the mod's files become extra fragments, pointing at ordinary
 * files on the card instead of at the backup.
 *
 * Three constraints decide where the region can go, and all three are checked
 * here rather than discovered on the console:
 *
 *  1. It must start at or above 4 GiB, because that is the only threshold the
 *     four-byte read patch can express (see RiivoDiPatch.hpp).
 *  2. It must start at or above the end of the backup's own virtual disc, or
 *     the game's fragments would shadow the mod's - a lookup returns the first
 *     fragment covering an offset, and the game's come first in the table.
 *  3. It must end below the dual-layer read ceiling the cIOS enforces
 *     (0x7ED38000 words). Past that every read is refused outright.
 *
 * Constraint 2 is what rules out dual-layer games: their backup already fills
 * the address space up to the ceiling, leaving nowhere to put anything. That is
 * a real limitation of this approach, not an oversight.
 *
 * The offset the cIOS ends up using is the same one the game asked for -
 * __DI_ReadUnencrypted adds config.offset[0]+config.offset[1], and both are
 * zero here: nothing in USB Loader GX ever sends IOCTL_DI_OFFSET (WDVD_Offset
 * exists but has no callers) and IOCTL_DI_OPENPART is not handled by the
 * plugin at all, so it never touches them. That makes the mapping the identity,
 * which is why a mod file's disc offset can be used directly as its offset into
 * the fragment list.
 *
 * Pure arithmetic, no console dependency, so it is host-tested.
 ***************************************************************************/
#ifndef RIIVO_FRAG_PLAN_HPP_
#define RIIVO_FRAG_PLAN_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{
	//! The cIOS refuses reads at or past this word offset once the disc has
	//! been recognised as dual-layer (dip.h, DVD9_LENGTH).
	static const u64 RIIVO_READ_CEILING = 0x7ED38000ULL * 4;

	//! FRAG_MAX in the cIOS. Shared between the game's own fragments and ours.
	static const u32 RIIVO_FRAG_MAX = 20000;

	//! Leave the table some slack rather than filling it to the last entry.
	static const u32 RIIVO_FRAG_RESERVE = 64;

	//! One relocated file, as FstBuilder::Layout assigned it.
	struct ModExtent
	{
		u64 offset; // byte offset on the virtual disc
		u32 length;
	};

	struct FragPlan
	{
		bool ok;
		std::string why;      // set only when !ok

		u64 regionStart;      // where the mod region begins
		u64 regionEnd;        // one past its last byte, rounded to a sector
		u64 payloadBytes;
		u32 files;
		u32 minFragments;     // at best one per file; fragmentation adds more
		u32 fragsAvailable;   // what is left of the table after the game's own
		u64 ceilingSpare;     // bytes between regionEnd and the read ceiling

		FragPlan()
			: ok(false), regionStart(0), regionEnd(0), payloadBytes(0), files(0),
			  minFragments(0), fragsAvailable(0), ceilingSpare(0) {}
	};

	//! Lowest offset the mod region may start at, given a backup whose virtual
	//! disc is `imageBytes` long. Rounded up to `align`.
	u64 PlanRegionStart(u64 imageBytes, u32 align);

	//! Check a laid-out set of extents against all three constraints.
	//! `usedFrags` is how many entries the game's own fragment list occupies.
	FragPlan PlanFragRegion(u64 imageBytes, u32 sectorSize, u32 usedFrags,
							const std::vector<ModExtent> &mods);
}

#endif
