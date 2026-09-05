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
		int error;     // 0, -500 (table full), or -501..-504 below
		u32 badOffset, badSector, badCount; // args that fired the error
	};

	//! Distinct refusal codes. These used to collapse into a single -501,
	//! which made a hole, a zero sector and an overflow indistinguishable.
	static const int REBASE_GAP = -501;      // offset != next
	static const int REBASE_ZERO_SECTOR = -502;
	static const int REBASE_SECTOR_OVERFLOW = -503; // sector+lba+count past 4G
	static const int REBASE_DISC_OVERFLOW = -504;   // base+offset+count past 4G

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
		if (offset != ctx->next)
		{
			ctx->badOffset = offset; ctx->badSector = sector; ctx->badCount = count;
			return ctx->error = REBASE_GAP;
		}
		if (!sector)
		{
			ctx->badOffset = offset; ctx->badSector = sector; ctx->badCount = count;
			return ctx->error = REBASE_ZERO_SECTOR;
		}
		if ((u64)sector + ctx->lba + count > 0x100000000ULL)
		{
			ctx->badOffset = offset; ctx->badSector = sector; ctx->badCount = count;
			return ctx->error = REBASE_SECTOR_OVERFLOW;
		}
		if ((u64)ctx->base + offset + count > 0xffffffffULL)
		{
			ctx->badOffset = offset; ctx->badSector = sector; ctx->badCount = count;
			return ctx->error = REBASE_DISC_OVERFLOW;
		}
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
			ctx.badOffset = ctx.badSector = ctx.badCount = 0;

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
					stats.failCode = FRAG_FAIL_TABLE_FULL;
					stats.failPath = f.external;
					stats.failLength = f.length;
					stats.failDriverRet = ret;
					stats.failSectorSize = sectorSize;
					return false;
				}
				++stats.failed;
				if (stats.firstFailure.empty())
				{
					//! Keep the numbers, not just the path: one failed file and
					//! eight hundred look identical without them, and they are
					//! the difference between a file-specific oddity and a
					//! systematic arithmetic error.
					stats.failPath = f.external;
					stats.failLength = f.length;
					stats.failLimit = ctx.limit;
					stats.failNext = ctx.next;
					stats.failDriverRet = ret;
					stats.failCbOffset = ctx.badOffset;
					stats.failCbSector = ctx.badSector;
					stats.failCbCount = ctx.badCount;
					stats.failSectorSize = sectorSize;
					if (ret)
						stats.failCode = FRAG_FAIL_DRIVER;
					else if (ctx.error == REBASE_GAP)
						stats.failCode = FRAG_FAIL_GAP;
					else if (ctx.error == REBASE_ZERO_SECTOR)
						stats.failCode = FRAG_FAIL_ZERO_SECTOR;
					else if (ctx.error == REBASE_SECTOR_OVERFLOW)
						stats.failCode = FRAG_FAIL_SECTOR_OVERFLOW;
					else if (ctx.error == REBASE_DISC_OVERFLOW)
						stats.failCode = FRAG_FAIL_DISC_OVERFLOW;
					else
						stats.failCode = FRAG_FAIL_SHORT;
					stats.firstFailure = DescribeFragFailure(stats);
				}
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

	std::string DescribeFragFailure(const FragBuildStats &stats)
	{
		if (stats.failCode == FRAG_FAIL_NONE || stats.failPath.empty())
			return std::string();
		const char *reason = "unknown mapping error";
		switch (stats.failCode)
		{
			case FRAG_FAIL_DRIVER: reason = "the filesystem driver refused the file"; break;
			case FRAG_FAIL_GAP: reason = "a hole or overlap in the reported runs"; break;
			case FRAG_FAIL_ZERO_SECTOR: reason = "the driver reported sector 0"; break;
			case FRAG_FAIL_SECTOR_OVERFLOW: reason = "a drive sector number past 4G"; break;
			case FRAG_FAIL_DISC_OVERFLOW: reason = "a virtual-disc sector past 4G"; break;
			case FRAG_FAIL_SHORT: reason = "the runs covered fewer sectors than the file needs"; break;
			case FRAG_FAIL_TABLE_FULL: reason = "the cIOS fragment table filled up"; break;
			default: break;
		}
		char buf[384];
		snprintf(buf, sizeof(buf),
			"could not map %s: %s (code %d, driver returned %d, file %u bytes, "
			"expected %u sectors of %u, driver covered %u, failing run at "
			"offset %u sector %u count %u)",
			stats.failPath.c_str(), reason, stats.failCode, stats.failDriverRet,
			stats.failLength, stats.failLimit, stats.failSectorSize, stats.failNext,
			stats.failCbOffset, stats.failCbSector, stats.failCbCount);
		return buf;
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
