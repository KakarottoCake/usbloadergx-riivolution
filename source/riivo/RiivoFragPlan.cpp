/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include "RiivoFragPlan.hpp"
#include "RiivoDiPatch.hpp"

namespace Riivo
{
	static FragPlan Refuse(const char *why)
	{
		FragPlan p;
		p.ok = false;
		p.why = why;
		return p;
	}

	static u64 RoundUp(u64 v, u64 align)
	{
		return (v + align - 1) & ~(align - 1);
	}

	u64 PlanRegionStart(u64 imageBytes, u32 align)
	{
		if (align == 0)
			align = 1;
		//! Both floors have to be cleared: the synthetic LOW_READ region, and the
		//! end of the backup's own virtual disc.
		u64 start = RIIVO_REGION_BYTES;
		if (imageBytes > start)
			start = imageBytes;
		return RoundUp(start, align);
	}

	FragPlan PlanFragRegion(u64 imageBytes, u32 sectorSize, u32 usedFrags,
							const std::vector<ModExtent> &mods)
	{
		if (sectorSize == 0 || (sectorSize & (sectorSize - 1)))
			return Refuse("the drive's sector size is not a power of two");
		if (sectorSize < 512 || sectorSize > 4096)
			return Refuse("the drive's sector size is out of range");
		if (mods.empty())
			return Refuse("there are no files to place");

		const u64 start = PlanRegionStart(imageBytes, sectorSize);

		// DVD9 payload addresses can overlap the synthetic window.
		if (imageBytes >= RIIVO_DVD9_PROBE_BYTES)
			return Refuse("dual-layer images are not supported by this read hook");

		FragPlan p;
		p.regionStart = start;
		p.files = (u32) mods.size();

		u64 end = start;
		u64 prevEnd = start;
		for (size_t i = 0; i < mods.size(); ++i)
		{
			const ModExtent &m = mods[i];

			//! Below the region start the game's own fragments would win the
			//! lookup, so the file would silently read as the wrong data.
			if (m.offset < start)
				return Refuse("a file was placed below the start of the mod region");

			//! A fragment cannot begin part-way through a sector.
			if (m.offset % sectorSize)
				return Refuse("a file is not aligned to the drive's sector size");

			//! Layout hands these out in ascending order, and the cIOS's lookup
			//! index assumes the table is sorted. Overlap would corrupt both.
			if (m.offset < prevEnd && i > 0)
				return Refuse("two files overlap, or they are not in ascending order");

			const u64 fileEnd = m.offset + m.length;
			if (fileEnd < m.offset || fileEnd > RIIVO_REGION_LIMIT)
				return Refuse("a file's extent overflows");

			prevEnd = fileEnd;
			if (fileEnd > end)
				end = fileEnd;
			p.payloadBytes += m.length;
		}

		p.regionEnd = RoundUp(end, sectorSize);

		if (p.regionEnd > RIIVO_REGION_LIMIT)
			return Refuse("the mod does not fit in the 2 GiB LOW_READ window");

		//! One fragment per file is the best case - a file stored contiguously.
		//! Fragmentation on the card only ever adds more, so this is a floor,
		//! and it is worth refusing early when even the floor does not fit.
		p.minFragments = (u32) mods.size();
		p.fragsAvailable = usedFrags >= RIIVO_FRAG_MAX - RIIVO_FRAG_RESERVE
						   ? 0
						   : RIIVO_FRAG_MAX - RIIVO_FRAG_RESERVE - usedFrags;

		if (p.minFragments > p.fragsAvailable)
			return Refuse("the mod needs more fragments than the cIOS table holds");

		p.ceilingSpare = RIIVO_REGION_LIMIT - p.regionEnd;
		p.ok = true;
		return p;
	}
}
