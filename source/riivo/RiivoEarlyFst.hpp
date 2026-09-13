/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Early authoritative plan (general pipeline): the game's file table is
 * read through the loader's own software disc reader (WBFS_OpenDisc ->
 * wd_open_disc -> wd_extract_file("FST"), ONLY_GAME_PARTITION) BEFORE the
 * fragment list is handed to the cIOS, while the drives are still mounted
 * - the same entry point the disc browser and the save-path code use, so
 * every WBFS backend (WBFS/FAT/NTFS/EXT) is reached through one call.
 * Using the same entry point supports backend reuse; it does not prove
 * every backend works - the comparison below names the backend per boot.
 *
 * The retained table feeds the ONE patch plan, built before fragment
 * registration. The late cIOS read stays as a consistency check: size,
 * parsed file count, and digest must all match, and a DOL-patching mod
 * additionally requires the same DOL image size both sides. On agreement
 * both phases consume the same plan - no rebuild, no offset remapping.
 * On disagreement the boot aborts coherently (file work withheld, stock
 * table, memory held back): the table is never rebuilt from a second
 * source while fragments planned from the first stay registered.
 *
 * This header holds only the pure half (digest, agreement, usability
 * gate), so host tests run this exact code. The Wii-only reads live in
 * RiivoBoot.cpp next to PrepareFragList.
 ***************************************************************************/
#ifndef RIIVO_EARLYFST_HPP_
#define RIIVO_EARLYFST_HPP_

#include <gctypes.h>

namespace Riivo
{
	//! FNV-1a, 32-bit, over raw FST bytes. Pure and total: NULL or empty
	//! input digests as the offset basis, never refused.
	inline u32 FstDigest(const u8 *data, u32 size)
	{
		u32 h = 2166136261u;
		if (!data)
			return h;
		for (u32 i = 0; i < size; ++i)
		{
			h ^= data[i];
			h *= 16777619u;
		}
		return h;
	}

	//! What the early software read saw: byte size, parsed file count, and
	//! the digest above. `valid` is false when the read or the parse failed;
	//! an invalid identity agrees with nothing.
	struct EarlyFstIdentity
	{
		u32 size;
		u32 files;
		u32 digest;
		bool valid;

		EarlyFstIdentity()
			: size(0), files(0), digest(2166136261u), valid(false) {}
	};

	//! True when the late cIOS table matches the early software read on all
	//! three: byte size, parsed file count, and digest. Pure; the caller
	//! aborts file work on false, never switches sources.
	inline bool EarlyFstAgrees(const EarlyFstIdentity &early,
							   u32 lateSize, u32 lateFiles, u32 lateDigest)
	{
		if (!early.valid)
			return false;
		return early.size == lateSize
			&& early.files == lateFiles
			&& early.digest == lateDigest;
	}

	//! Usability gate for the retained early plan: a plan exists AND the
	//! late consistency read agrees with the table it was built from.
	//! Both the agreement path (proceed on the same plan) and the
	//! disagreement path (coherent abort, no source switch) are decided
	//! here; host tests exercise both through this exact function.
	inline bool EarlyPlanUsable(bool havePlan, const EarlyFstIdentity &early,
								u32 lateSize, u32 lateFiles, u32 lateDigest)
	{
		return havePlan && EarlyFstAgrees(early, lateSize, lateFiles,
										  lateDigest);
	}
} // namespace Riivo

#endif
