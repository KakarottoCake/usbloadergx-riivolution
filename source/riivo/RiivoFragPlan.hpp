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
 *  1. It must live inside the synthetic LOW_READ window the hook tests
 *     (see RiivoDiPatch.hpp): word offsets 0x60000000-0x80000000, i.e. bytes
 *     6-8 GiB. That span is above the DVD layer probes and anything a
 *     single-layer backup maps, below signed-word wrap, and inside d2x's
 *     fragment seek index. Only LOW_READ in this window is diverted to the
 *     mod's fragments; every other read keeps the stock decrypt/hash path,
 *     so layer detection and the anti-piracy reads behave exactly as stock.
 *  2. It must start at or above the end of the backup's own virtual disc, or
 *     the game's fragments would shadow the mod's - a lookup returns the first
 *     fragment covering an offset, and the game's come first in the table.
 *  3. It must end inside the window, which caps a mod at just under 2 GiB.
 *
 * Dual-layer images are refused outright: their payload addresses can overlap
 * the synthetic window, so routing by address range cannot tell mod from game.
 * That is a real limitation of this approach, not an oversight.
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

	//! The single-layer limit. Past this the cIOS refuses every read unless it
	//! has decided the disc is dual-layer.
	static const u64 RIIVO_DVD5_CEILING = 0x46090000ULL * 4;

	//! Where __DI_CheckDisc() probes to make that decision - word offset
	//! 0x47000000. A declared size reaching this point makes the probe hit a
	//! sparse zero block, succeed, and promote the disc to dual-layer limits.
	//! Nothing here does that on purpose; it is recorded so the reason stays
	//! visible, because promoting a single-layer game is what an anti-piracy
	//! check is looking for.
	static const u64 RIIVO_DVD9_PROBE_BYTES = 0x47000000ULL * 4;

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
