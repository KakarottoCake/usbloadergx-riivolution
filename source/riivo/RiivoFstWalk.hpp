/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Strict, read-only walk of a finished Wii FST image.  This deliberately does
 * not share the builder's tree or parser: it is the last check before the game
 * is handed a replacement table.
 ***************************************************************************/
#ifndef RIIVO_FST_WALK_HPP_
#define RIIVO_FST_WALK_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include <map>

namespace Riivo
{
	struct FstWalkFile
	{
		u64 offset; // byte offset; widened before applying the Wii << 2 convention
		u32 length;
		u32 entry;
	};

	struct FstWalkExpectation
	{
		std::string path;
		u64 offset;
		u32 length;

		FstWalkExpectation() : offset(0), length(0) {}
		FstWalkExpectation(const std::string &p, u64 o, u32 l)
			: path(p), offset(o), length(l) {}
	};

	class FstWalk
	{
		public:
			//! Validate the complete flat image once. `shifted` means file offsets
			//! are Wii disc words and are returned as byte offsets.
			bool Open(const u8 *fstData, u32 fstSize, bool shifted,
					  std::string *outError = 0);

			//! Resolve a path with the SDK's component-at-a-time, ASCII
			//! case-insensitive directory walk. Absolute paths, '.', and '..' have
			//! the same meaning as DVDConvertPathToEntrynum.
			bool Lookup(const std::string &path, FstWalkFile *out) const;

			//! Resolve every expected file after one validation pass. Zero-length
			//! files are intentional and are compared normally.
			bool Check(const std::vector<FstWalkExpectation> &expected,
					   std::string *outError = 0) const;

			u32 EntryCount() const { return (u32) entries.size(); }

		private:
			struct Entry
			{
				bool isDir;
				u32 parent;
				u32 next;
				u64 offset;
				u32 length;
				std::string foldedName;
				std::map<std::string, u32> children;
			};

			std::vector<Entry> entries;
	};
}

#endif
