/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "RiivoFragBuild.hpp"
#include "usbloader/wbfs.h"
#include "usbloader/wdvd.h"
#include "libs/libfat/fatfile_frag.h"
#include "libs/libntfs/ntfsfile_frag.h"
#include "libs/libext2fs/ext2_frag.h"
#include "gecko.h"

namespace Riivo
{
	//! Passed through the filesystem driver's callback. The driver reports each
	//! run as (file-relative sector, real sector, count); we file it under the
	//! virtual disc instead by adding the file's own base.
	struct RebaseCtx
	{
		FragList *master;
		u32 base;      // the file's first sector on the virtual disc
		u32 lba;       // partition start, for drivers that report relative sectors
		int error;
	};

	static int RebaseAppend(void *data, u32 offset, u32 sector, u32 count)
	{
		RebaseCtx *ctx = (RebaseCtx *) data;
		if (ctx->error)
			return ctx->error;

		//! frag_append merges with the previous entry when both the offset and
		//! the sector run on contiguously, so a file stored in one piece costs
		//! one entry however many runs the driver reports it in.
		int ret = frag_append(ctx->master, ctx->base + offset, sector + ctx->lba, count);
		if (ret)
			ctx->error = ret;
		return ret;
	}

	bool AppendModFragments(const std::vector<PlacedFile> &files, u32 sectorSize,
							u8 fsType, u32 lbaOffset, FragBuildStats &stats)
	{
		stats = FragBuildStats();

		FragList *master = frag_list_mutable();
		if (!master)
		{
			stats.firstFailure = "there is no fragment list to extend";
			return false;
		}
		if (sectorSize == 0)
		{
			stats.firstFailure = "the drive's sector size is unknown";
			return false;
		}

		stats.fragsBefore = master->num;
		stats.sizeBefore = master->size;

		//! Only FAT reports absolute sectors; the others are relative to the
		//! partition, exactly as get_frag_list_for_file has to allow for.
		const u32 lba = (fsType == PART_FS_FAT) ? 0 : lbaOffset;

		for (size_t i = 0; i < files.size(); ++i)
		{
			const PlacedFile &f = files[i];

			RebaseCtx ctx;
			ctx.master = master;
			ctx.base = (u32) (f.offset / sectorSize);
			ctx.lba = lba;
			ctx.error = 0;

			int ret;
			switch (fsType)
			{
				case PART_FS_FAT:
					ret = _FAT_get_fragments(f.external.c_str(), &RebaseAppend, &ctx);
					break;
				case PART_FS_NTFS:
					ret = _NTFS_get_fragments(f.external.c_str(), &RebaseAppend, &ctx);
					break;
				case PART_FS_EXT:
					ret = _EXT2_get_fragments(f.external.c_str(), &RebaseAppend, &ctx);
					break;
				default:
					stats.firstFailure = "the mod is on a filesystem whose layout "
										 "cannot be read (a .wbfs partition?)";
					return false;
			}

			if (ret || ctx.error)
			{
				//! Running out of table entries is fatal for the whole plan;
				//! one unreadable file is not, but it does mean that file will
				//! read back as zeros, so it is counted and reported.
				if (ctx.error == -500)
				{
					stats.firstFailure = "the cIOS fragment table filled up";
					return false;
				}
				++stats.failed;
				if (stats.firstFailure.empty())
					stats.firstFailure = f.external;
				continue;
			}

			++stats.files;
		}

		stats.fragsAfter = master->num;
		stats.sizeAfter = master->size;

		if (stats.files == 0)
		{
			stats.firstFailure = "not one of the mod's files could be located "
								 "on the drive";
			return false;
		}

		return true;
	}

	bool VerifyModFragment(u64 discOffset, const std::string &file, std::string &why)
	{
		static const u32 CHECK = 64;

		FILE *f = fopen(file.c_str(), "rb");
		if (!f)
		{
			why = "could not reopen " + file;
			return false;
		}
		u8 want[CHECK];
		const size_t got = fread(want, 1, CHECK, f);
		fclose(f);
		if (got == 0)
		{
			why = "read nothing from " + file;
			return false;
		}

		//! Go back through the cIOS the way the game will: an unencrypted read
		//! resolves through the fragment list without decrypting, which is the
		//! path the patched dispatch sends the mod region down.
		u8 *have = (u8 *) memalign(32, 32 + CHECK);
		if (!have)
		{
			why = "out of memory for the check";
			return false;
		}
		memset(have, 0, CHECK);

		const s32 ret = WDVD_UnencryptedRead(have, CHECK, discOffset);
		const bool same = (ret >= 0) && (memcmp(have, want, got) == 0);

		if (ret < 0)
		{
			char buf[96];
			snprintf(buf, sizeof(buf), "the cIOS refused a read at 0x%010llx (%d)",
					 (unsigned long long) discOffset, (int) ret);
			why = buf;
		}
		else if (!same)
		{
			char buf[128];
			snprintf(buf, sizeof(buf),
					 "read back %02x%02x%02x%02x at 0x%010llx, expected %02x%02x%02x%02x",
					 have[0], have[1], have[2], have[3],
					 (unsigned long long) discOffset,
					 want[0], want[1], want[2], want[3]);
			why = buf;
		}

		free(have);
		return same;
	}
}
