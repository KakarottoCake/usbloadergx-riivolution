/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 3 (loader-side foundation): parse the game's FST so <file>/<folder>
 * patches can be matched to on-disc entries. The result feeds the redirect
 * table that the IOS DI read-hook consumes at runtime.
 *
 * FST layout (big-endian on disc): 12-byte entries
 *   [0]      filetype (0 = file, 1 = directory)
 *   [1..3]   name offset into the string table (24-bit)
 *   [4..7]   file: disc offset (Wii: >>2 shifted); dir: parent index
 *   [8..11]  file: length; dir: index of first entry past this subtree
 * Entry 0 is the root; its length is the total entry count. The string table
 * follows the entries at (fst + count*12).
 ***************************************************************************/
#ifndef RIIVO_FST_HPP_
#define RIIVO_FST_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{
	struct FstFile
	{
		std::string path; // full path, lower-cased, leading '/', e.g. "/stage/1-1.arc"
		//! Disc byte offset, un-shifted. 64-bit on purpose: the stored value is
		//! offset>>2, so a dual-layer partition addresses well past 4 GiB and a
		//! u32 would silently wrap. A single-layer disc already reaches 0xff72d4e0.
		u64 offset;
		u32 length;       // file size in bytes
		u32 index;        // FST entry index (for patching the entry in place)
	};

	class Fst
	{
		public:
			//! Parse an FST image. `shifted` applies the Wii offset<<2 convention.
			//! Returns false if the image is malformed / inconsistent with fstSize.
			bool Parse(const u8 *fstData, u32 fstSize, bool shifted);

			//! Match a Riivolution `disc=` value: a full path ("/a/b.arc",
			//! case-insensitive) or a bare filename (matched against any basename).
			//! Returns NULL if not found.
			const FstFile *FindFile(const std::string &discPath) const;

			//! Collect files under a directory path (case-insensitive). When
			//! recursive is false, only direct children are returned.
			void ListFolder(const std::string &dirPath, bool recursive,
							std::vector<const FstFile *> &out) const;

			size_t FileCount() const { return files.size(); }
			const FstFile &FileAt(size_t i) const { return files[i]; }

		private:
			std::vector<FstFile> files;
	};
}

#endif
