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
		u32 next, limit;
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
		// A zero-count callback is the driver's file-size marker, not data.
		if (!count) return 0;
		if (offset >= ctx->limit) return 0; // allocation slack past EOF
		if (offset != ctx->next || !sector ||
			(u64)sector + ctx->lba + count > 0x100000000ULL ||
			(u64)ctx->base + offset + count > 0xffffffffULL)
			return ctx->error = -501; // holes, overlaps, or truncated LBA
		if (count > ctx->limit - offset) count = ctx->limit - offset;
		int ret = frag_append(ctx->master, ctx->base + offset, sector + ctx->lba, count);
		if (!ret) ctx->next = offset + count;
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
			ctx.next = 0;
			ctx.limit = (u32)(((u64)f.length + sectorSize - 1) / sectorSize);
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

			if (ret || ctx.error || ctx.next != ctx.limit)
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

	bool VerifyModFragment(u64 discOffset, u32 length, const std::string &file, std::string &why)
	{
		FILE *f = fopen(file.c_str(), "rb");
		if (!f) { why = "could not reopen " + file; return false; }
		if (!length || fseek(f, 0, SEEK_END) || (u64)ftell(f) != length) {
			fclose(f);
			why = "mod file size changed after placement: " + file;
			return false;
		}
		u8 want[32] = {};
		u8 have[32] ATTRIBUTE_ALIGN(32);
		const u32 samples[] = { 0, (length - 1) & ~31u };
		bool ok = true;
		for (u32 i = 0; i < 2 && ok; ++i) {
			const u32 offset = samples[i];
			const u32 n = length - offset < 32 ? length - offset : 32;
			if (fseek(f, offset, SEEK_SET) || fread(want, 1, n, f) != n) {
				why = "could not sample " + file;
				ok = false;
				break;
			}
			memset(have, 0, sizeof(have));
			// LOW_READ, not UNENCREAD: prove the installed dispatch as well
			// as the device, LBA mapping and final file sector.
			const s32 ret = WDVD_Read(have, sizeof(have), discOffset + offset);
			if (ret != 0 || memcmp(have, want, n)) {
				char message[112];
				snprintf(message, sizeof(message),
					"LOW_READ check failed at 0x%010llx (return %d): ",
					(unsigned long long)(discOffset + offset), (int)ret);
				why = std::string(message) + file;
				ok = false;
			}
		}
		fclose(f);
		return ok;
	}
}
