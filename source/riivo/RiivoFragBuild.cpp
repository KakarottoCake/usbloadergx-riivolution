/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <limits.h>

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
		u32 lastSector, lastCount; // post-lba sectors of the last appended run
	};

	//! Distinct refusal codes. These used to collapse into a single -501,
	//! which made a hole, a zero sector and an overflow indistinguishable.
	static const int REBASE_GAP = -501;      // offset != next
	static const int REBASE_ZERO_SECTOR = -502;
	static const int REBASE_SECTOR_OVERFLOW = -503; // sector+lba+count past 4G
	static const int REBASE_DISC_OVERFLOW = -504;   // base+offset+count past 4G

	//! Reads one byte at `covered*sectorSize` through normal file I/O.
	//! Returns 1 when it reads back (the file has data past what the driver
	//! reported: our problem), 0 when it does not (the file is genuinely
	//! short on the card: re-copy it), -1 when the file cannot be opened.
	//! The caller only asks this when covered < ceil(length / sectorSize),
	//! so the probed byte is always inside the claimed size.
	static int ProbeShortFile(const std::string &path, u32 covered, u32 sectorSize)
	{
		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			return -1;
		const u64 at = (u64) covered * sectorSize;
		u8 b = 0;
		//! Stays -1 unless the seek itself worked. Seeking past the end of a
		//! file succeeds and the read returns nothing, so a seek that FAILS is
		//! something else entirely - and reporting that as "short on the card"
		//! would send the tester off re-copying a file on no evidence.
		int verdict = -1;
		if (at <= (u64) LONG_MAX && fseek(f, (long) at, SEEK_SET) == 0)
			verdict = (fread(&b, 1, 1, f) == 1) ? 1 : 0;
		fclose(f);
		return verdict;
	}

	//! Tester-actionable tail for a SHORT failure, or empty when unprobed.
	static std::string ShortVerdict(int probe, u32 length, u32 covered, u32 sectorSize)
	{
		if (probe == 1)
			return "; the missing bytes read back fine, so the filesystem driver "
				   "under-reported this file";
		if (probe == 0)
		{
			char buf[96];
			const u64 missing = (u64) length - (u64) covered * sectorSize;
			snprintf(buf, sizeof(buf), "; the drive cannot supply the last %llu "
					 "bytes of this file - re-copy it",
					 (unsigned long long) missing);
			return buf;
		}
		return std::string();
	}

	static const char *FailReason(int code)
	{
		switch (code)
		{
			case FRAG_FAIL_DRIVER: return "the filesystem driver refused the file";
			case FRAG_FAIL_GAP: return "a hole or overlap in the reported runs";
			case FRAG_FAIL_ZERO_SECTOR: return "the driver reported sector 0";
			case FRAG_FAIL_SECTOR_OVERFLOW: return "a drive sector number past 4G";
			case FRAG_FAIL_DISC_OVERFLOW: return "a virtual-disc sector past 4G";
			case FRAG_FAIL_SHORT: return "the runs covered fewer sectors than the file needs";
			case FRAG_FAIL_TABLE_FULL: return "the cIOS fragment table filled up";
			default: break;
		}
		return "unknown mapping error";
	}

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
		if (!ret)
		{
			ctx->next = offset + count;
			ctx->lastSector = sector + ctx->lba;
			ctx->lastCount = count;
		}
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
			ctx.lastSector = ctx.lastCount = 0;

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

			//! Tail-cluster recovery. The vendored FAT driver reports whole
			//! clusters against a floor bound, so a file whose data ends
			//! inside the first sector of a cluster comes back exactly one
			//! sector short. That sector is the head of the next cluster in
			//! the chain, which the driver never names - but when the
			//! shortfall is exactly one sector it is the sector right after
			//! the last one mapped. Append that guess contiguously (it merges
			//! into the existing entry, costing no table slot); the read-back
			//! proves it, and anything else is still refused below.
			if (!ret && !ctx.error && ctx.next && ctx.limit - ctx.next == 1
				&& (u64) ctx.lastSector + ctx.lastCount + 1 <= 0x100000000ULL
				&& (u64) ctx.base + ctx.next + 1 <= 0xffffffffULL)
			{
				const int aret = frag_append(master, ctx.base + ctx.next,
											 ctx.lastSector + ctx.lastCount, 1);
				if (!aret)
				{
					ctx.next = ctx.limit;
					stats.extended.push_back(f.offset);
				}
				else
					//! Only a full table can refuse a contiguous append, and
					//! that is fatal for the whole plan - say so, rather than
					//! letting it fall through and read as a short file.
					ctx.error = aret;
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
				int code;
				if (ret)
					code = FRAG_FAIL_DRIVER;
				else if (ctx.error == REBASE_GAP)
					code = FRAG_FAIL_GAP;
				else if (ctx.error == REBASE_ZERO_SECTOR)
					code = FRAG_FAIL_ZERO_SECTOR;
				else if (ctx.error == REBASE_SECTOR_OVERFLOW)
					code = FRAG_FAIL_SECTOR_OVERFLOW;
				else if (ctx.error == REBASE_DISC_OVERFLOW)
					code = FRAG_FAIL_DISC_OVERFLOW;
				else
					code = FRAG_FAIL_SHORT;
				//! Prove whose fault a shortfall is: a byte that reads back
				//! means the driver under-reported the file, one that does not
				//! means the file is short on the card.
				//!
				//! Only for failures the log will actually name. Each probe is
				//! another open and seek on the card, and a mod can fail in the
				//! thousands - probing all of them would stall the boot for tens
				//! of seconds behind a black screen for numbers nothing prints.
				const bool listed = stats.failList.size() < FRAG_FAIL_LIST_MAX;
				int probe = -1;
				if (listed && code == FRAG_FAIL_SHORT)
					probe = ProbeShortFile(f.external, ctx.next, sectorSize);
				if (listed)
				{
					FragFailEntry e;
					e.path = f.external;
					e.code = code;
					e.length = f.length;
					e.limit = ctx.limit;
					e.next = ctx.next;
					e.driverRet = ret;
					e.probe = probe;
					stats.failList.push_back(e);
				}
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
					stats.failCode = code;
					stats.failProbe = probe;
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
		char buf[480];
		snprintf(buf, sizeof(buf),
			"could not map %s: %s (code %d, driver returned %d, file %u bytes, "
			"expected %u sectors of %u, driver covered %u, failing run at "
			"offset %u sector %u count %u%s)",
			stats.failPath.c_str(), FailReason(stats.failCode),
			stats.failCode, stats.failDriverRet,
			stats.failLength, stats.failLimit, stats.failSectorSize, stats.failNext,
			stats.failCbOffset, stats.failCbSector, stats.failCbCount,
			stats.failCode == FRAG_FAIL_SHORT
				? ShortVerdict(stats.failProbe, stats.failLength,
							   stats.failNext, stats.failSectorSize).c_str()
				: "");
		return buf;
	}

	//! One collected failure on a single log line.
	static std::string DescribeFragFailEntry(const FragFailEntry &e, u32 sectorSize)
	{
		char buf[512];
		if (e.code == FRAG_FAIL_SHORT)
			snprintf(buf, sizeof(buf), "%s: %s (file %u bytes, expected %u "
					 "sectors, covered %u%s)",
					 e.path.c_str(), FailReason(e.code), e.length, e.limit, e.next,
					 ShortVerdict(e.probe, e.length, e.next, sectorSize).c_str());
		else
			snprintf(buf, sizeof(buf), "%s: %s (code %d, driver returned %d, "
					 "file %u bytes)",
					 e.path.c_str(), FailReason(e.code),
					 e.code, e.driverRet, e.length);
		return buf;
	}

	std::string DescribeFragFailList(const FragBuildStats &stats)
	{
		std::string out;
		if (stats.failList.empty())
			return out;
		char head[64];
		snprintf(head, sizeof(head), "  mapping failures   : %u file(s)\n",
				 (unsigned) stats.failed);
		out += head;
		for (size_t i = 0; i < stats.failList.size(); ++i)
		{
			out += "    ";
			out += DescribeFragFailEntry(stats.failList[i], stats.failSectorSize);
			out += "\n";
		}
		if (stats.failed > stats.failList.size())
		{
			char tail[64];
			snprintf(tail, sizeof(tail), "    ... and %u more\n",
					 (unsigned) (stats.failed - stats.failList.size()));
			out += tail;
		}
		return out;
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
