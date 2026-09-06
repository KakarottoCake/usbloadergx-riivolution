/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Describing the mod's layout to the IOS side by PATH rather than by sector.
 *
 * This is the loader half of the on-demand design. The old approach resolved
 * every file to raw storage sectors here, before the game started, which meant
 * walking a FAT cluster chain per file - thousands of them - while the screen
 * was black. This half now needs only each file's SIZE, which enumeration
 * already knows, and hands IOS the paths. Nothing walks a cluster chain until
 * the game actually asks for a file, and a game asks for a fraction of them.
 *
 * Placement is not done here. FstBuilder::Layout() already assigns each modded
 * file an aligned byte range on the virtual disc, and RiivoFragPlan already
 * checks that range against the read window; this file takes that finished
 * layout and serialises it. Duplicating the placement rules would give the two
 * halves two chances to disagree.
 *
 * The table is little-endian regardless of the console, because the reader on
 * the other side assembles every field from bytes. That keeps one format
 * working between a big-endian PowerPC writer and a big-endian ARM reader
 * without either of them depending on a struct layout. Every field is written
 * a byte at a time for the same reason - a memcpy of a u32 here would emit
 * big-endian and the reader would see a byte-swapped table.
 ***************************************************************************/
#ifndef RIIVO_REDIRECT_TABLE_HPP_
#define RIIVO_REDIRECT_TABLE_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{
	//! 'RIIV', read back as bytes by the module. Any change to the field
	//! layout below must change this, so a stale module refuses the table
	//! instead of misreading it.
	static const u32 RIIVO_TABLE_MAGIC = 0x56494952;

	//! Header: magic, count, string-blob offset, partition LBA.
	static const u32 RIIVO_TABLE_HEADER = 16;

	//! Entry: offset low, offset high, length, path index.
	static const u32 RIIVO_TABLE_ENTRY = 16;

	//! Tells the module to find the FAT partition itself by parsing the MBR.
	//! d2x keeps only the raw physical sectors of the game's fragments, not
	//! the partition it came from, so there is usually nothing to pass here.
	static const u32 RIIVO_PART_DISCOVER = 0xFFFFFFFF;

	//! Files should be laid out on 32 KB boundaries so a single disc read can
	//! never touch two of them. The reader handles a read that spans a
	//! boundary correctly, but a layout where that cannot arise is one less
	//! thing depending on the reader being right.
	static const u32 RIIVO_MOD_ALIGN = 0x8000;

	//! One entry: a byte range on the virtual disc, served from a file on the
	//! card. `path` is absolute within the FAT partition, e.g.
	//! "/riivolution/mymod/files/course.arc".
	struct RedirectEntry
	{
		u64 discOffset;
		u32 length;
		std::string path;

		RedirectEntry() : discOffset(0), length(0) {}
		RedirectEntry(u64 off, u32 len, const std::string &p)
			: discOffset(off), length(len), path(p) {}
	};

	//! Serialise the layout into the table the IOS module reads.
	//!
	//! Refuses rather than emits a table the module would have to distrust:
	//! entries must be sorted by offset, must not overlap, must have a
	//! non-empty absolute path, and must not be so numerous that the table
	//! overflows a u32. The module re-checks all of this at init - it has to,
	//! since it cannot assume the memory it was handed came from us - but
	//! catching it here is what makes the failure legible.
	//!
	//! Returns false with `why` set on refusal, leaving `out` untouched.
	bool BuildRedirectTable(const std::vector<RedirectEntry> &entries,
							u32 partLba, std::vector<u8> &out,
							std::string &why);

	//! Size the table will occupy, without building it. For deciding where in
	//! MEM2 it goes before the layout is final.
	u64 RedirectTableSize(const std::vector<RedirectEntry> &entries);
}

#endif
