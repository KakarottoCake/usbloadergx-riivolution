/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Rebuild a Wii FST (file system table) with a mod applied.
 *
 * Every route to working <file>/<folder> replacement needs this, because the
 * measurement on real hardware showed the two things a plain read-redirect
 * can never do:
 *   - 87% of a level-hack mod's files do not exist on the disc at all, so they
 *     need new FST entries;
 *   - a quarter of the ones that do exist are BIGGER than the original, and the
 *     FST still advertises the old length, so the game would never ask for the
 *     extra bytes.
 * Both are fixed by rewriting the table: give replaced files their real size,
 * give added files an entry, and point every modded file at a fresh offset.
 *
 * The on-disc format is a flat array of 12-byte entries followed by a string
 * table. Editing that in place is painful (inserting one file shifts every
 * later index and every directory's end marker), so this parses it into a tree,
 * edits the tree, and serialises a fresh table. Entry 0 is the root, whose
 * length field is the total entry count.
 *
 * Deliberately free of any console dependency so it can be tested on a host.
 ***************************************************************************/
#ifndef RIIVO_FST_BUILD_HPP_
#define RIIVO_FST_BUILD_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{
	//! One node of the editable tree. Directories own `children`; files carry
	//! `offset`/`length`. `name` keeps its original case (the game compares
	//! case-insensitively, but the table should look untouched where it is).
	struct FstNode
	{
		std::string name;
		bool isDir;
		u64 offset;   // byte offset on the (virtual) disc; files only
		u32 length;   // size in bytes; files only
		bool modded;  // set when this entry was replaced or added by the mod
		std::vector<FstNode> children;

		FstNode() : isDir(false), offset(0), length(0), modded(false) {}
	};

	//! Result of laying the mod out over the disc.
	struct FstBuildStats
	{
		u32 replaced;      // existing entries repointed at an external file
		u32 added;         // entries that did not exist on the disc
		u32 addedDirs;     // directories created to hold added files
		u32 unchanged;     // disc entries left exactly as they were
		u32 entryCount;    // entries in the rebuilt table
		u32 fstSize;       // serialised size in bytes
		u32 origFstSize;   // size of the table we started from
		u64 highestOffset; // end of the highest byte range we handed out

		FstBuildStats()
			: replaced(0), added(0), addedDirs(0), unchanged(0), entryCount(0),
			  fstSize(0), origFstSize(0), highestOffset(0) {}
	};

	//! A file the mod supplies, already resolved to a real path on the card.
	struct ModFile
	{
		std::string discPath; // lower-cased, leading '/', e.g. "/objectdata/x.arc"
		std::string external; // "usb1:/Spectral/ObjectData/x.arc"
		u32 size;             // the external file's real size, from stat
		u64 assignedOffset;   // filled in by Layout(): where it now lives
	};

	class FstBuilder
	{
		public:
			FstBuilder() {}

			//! Parse an on-disc FST image into the editable tree.
			//! `shifted` applies the Wii offset<<2 convention. False on a
			//! malformed image.
			bool Parse(const u8 *fstData, u32 fstSize, bool shifted);

			//! Point `discPath` at an external file of `size` bytes, creating the
			//! entry (and any missing parent directories) when it is not already
			//! there. Returns false only if the path is unusable.
			//! Offsets are not assigned here - Layout() does that once, after
			//! every file is known, so the allocation stays contiguous.
			bool AddOrReplace(const std::string &discPath, u32 size, bool *outWasNew);

			//! Hand every modded file a fresh, aligned byte range starting at
			//! `regionStart`. Files are placed in tree order so a later fragment
			//! list can be built in one pass. `align` must be a power of two and
			//! at least the device sector size, otherwise a fragment cannot start
			//! at the boundary. Fills in each entry's offset.
			void Layout(u64 regionStart, u32 align);

			//! Serialise back to the on-disc format. `shifted` must match Parse.
			void Serialize(std::vector<u8> &out, bool shifted) const;

			//! Look up the offset assigned to a modded file, or false.
			bool FindAssigned(const std::string &discPath, u64 *outOffset, u32 *outLength) const;

			const FstBuildStats &Stats() const { return stats; }
			//! Highest byte offset used by any UNMODDED disc file. Layout() should
			//! start above this so nothing collides with data still on the disc.
			u64 OriginalExtent() const;

		private:
			FstNode root;
			FstBuildStats stats;
	};
}

#endif
