/****************************************************************************
 * Byte-diff our rebuilt FST against Dolphin's reference implementation.
 *
 * Reference: github.com/dolphin-emu/dolphin, Source/Core/DiscIO, master as
 * retrieved 2026-09-07. The "DolphinRef" namespace below is extracted from:
 *   - DirectoryBlob.h  (FSTBuilderNode, BuilderContentSource, ContentFile,
 *     ContentMemory, ContentPartition, ContentVolume, ContentFixedByte,
 *     DiscContent/DiscContentContainer shape for content adds)
 *   - DirectoryBlob.cpp (SplitAt-adjacent logic excluded; WriteEntryData,
 *     WriteEntryName, WriteDirectory, ComputeNameSize,
 *     RecalculateFolderSizes, and the fst_data production order of BuildFST)
 *   - RiivolutionPatcher.cpp (SplitAt, ApplyPatchToFile x2,
 *     FindFileNodeInFST, FindFilenameNodeInFST, ApplyFilePatchToFST)
 *   - RiivolutionParser.h (File struct, copied verbatim)
 * Copyright for the extracted parts belongs to the Dolphin Emulator Project
 * (SPDX-License-Identifier: GPL-2.0-or-later, as in the originals).
 *
 * Mechanical adaptations for this C++17 host harness, logic untouched:
 *   - Common::CaseInsensitiveEquals -> local ASCII equivalent.
 *   - std::ranges::find_if/sort -> std::find_if/std::sort.
 *   - std::span params -> const vector& (memory-patch half not extracted).
 *   - fmt::format path combiner (folder half) not extracted; folders stay
 *     covered by test_pipeline. Only the <file> path is compared here.
 *   - Patch -> minimal holder of m_file_patches + loader pointer; extracted
 *     bodies still read patch.m_file_data_loader exactly as upstream.
 *   - BuildFST's disc-image layout (sequential data offsets, m_contents
 *     adds, disc_header writes, SHIFT-JIS conversion) is out of scope:
 *     per-file data offsets are our own layout decision, proven by
 *     read-back, so the comparison requires exact offsets only for
 *     untouched files. Test names are ASCII, for which the SHIFT-JIS
 *     conversion is the identity (noted, not tested).
 *
 * What this establishes (host only - it cannot bless the IOS runtime):
 *   entry encoding, name table layout, root entry, directory end indices,
 *   trailing padding policy, ordering policy, and file-patch size/content
 *   semantics, case by case. Divergences are pinned as passing checks that
 *   record BOTH values with an explanation, never as silent drift.
 ***************************************************************************/
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoManifest.hpp"

static int g_checks = 0;
static int g_fail = 0;

static void check(bool cond, const char *what)
{
	++g_checks;
	if (!cond)
	{
		++g_fail;
		std::printf("FAIL: %s\n", what);
	}
}

// ------------------------------------------------------------------
// DolphinRef: extracted reference logic (see header).
// ------------------------------------------------------------------
namespace DolphinRef
{
	static bool CaseInsensitiveEquals(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i]))
				return false;
		return true;
	}

	static std::string ToUpperStr(std::string s)
	{
		for (size_t i = 0; i < s.size(); ++i)
			s[i] = (char) toupper((unsigned char) s[i]);
		return s;
	}

	static u64 AlignUp(u64 v, u64 align)
	{
		return (v + align - 1) & ~(align - 1);
	}

	struct ContentFile
	{
		std::string m_filename;
		u64 m_offset = 0;
	};

	typedef std::shared_ptr<std::vector<u8>> ContentMemory;

	struct ContentPartition
	{
		u64 m_offset = 0;
		u64 m_partition_data_offset = 0;
	};

	struct ContentVolume
	{
		u64 m_offset = 0;
		int m_partition = 0;
	};

	struct ContentFixedByte
	{
		u8 m_byte = 0;
	};

	using ContentSource = std::variant<ContentFile, ContentMemory, ContentPartition,
									   ContentVolume, ContentFixedByte>;

	struct BuilderContentSource
	{
		u64 m_offset = 0;
		u64 m_size = 0;
		ContentSource m_source;
	};

	struct FSTBuilderNode
	{
		std::string m_filename;
		u64 m_size = 0;
		std::variant<std::vector<BuilderContentSource>, std::vector<FSTBuilderNode>> m_content;

		bool IsFile() const
		{
			return std::holds_alternative<std::vector<BuilderContentSource>>(m_content);
		}
		bool IsFolder() const
		{
			return std::holds_alternative<std::vector<FSTBuilderNode>>(m_content);
		}
		std::vector<BuilderContentSource> &GetFileContent()
		{
			return std::get<std::vector<BuilderContentSource>>(m_content);
		}
		const std::vector<BuilderContentSource> &GetFileContent() const
		{
			return std::get<std::vector<BuilderContentSource>>(m_content);
		}
		std::vector<FSTBuilderNode> &GetFolderContent()
		{
			return std::get<std::vector<FSTBuilderNode>>(m_content);
		}
		const std::vector<FSTBuilderNode> &GetFolderContent() const
		{
			return std::get<std::vector<FSTBuilderNode>>(m_content);
		}
	};

	class FileDataLoader
	{
		public:
			virtual ~FileDataLoader() {}
			virtual std::optional<u64> GetExternalFileSize(const std::string &p) = 0;
			virtual BuilderContentSource MakeContentSource(const std::string &p, u64 extOff,
														   u64 extSize, u64 discOff) = 0;
	};

	//! RiivolutionParser.h File, verbatim.
	struct File
	{
		std::string m_disc;
		std::string m_external;
		bool m_resize = true;
		bool m_create = false;
		u32 m_offset = 0;
		u32 m_fileoffset = 0;
		u32 m_length = 0;
	};

	struct Patch
	{
		FileDataLoader *m_file_data_loader = nullptr;
		std::vector<File> m_file_patches;
	};

	// 'before' and 'after' should be two copies of the same source
	// 'split_at' needs to be between the start and end of the source, may not match either boundary
	static void SplitAt(BuilderContentSource *before, BuilderContentSource *after, u64 split_at)
	{
		const u64 start = before->m_offset;
		const u64 size = before->m_size;
		const u64 end = start + size;

		// The source before the split point just needs its length reduced.
		before->m_size = split_at - start;

		// The source after the split needs its length reduced and its start point adjusted.
		after->m_offset += before->m_size;
		after->m_size = end - split_at;
		if (std::holds_alternative<ContentFile>(after->m_source))
		{
			std::get<ContentFile>(after->m_source).m_offset += before->m_size;
		}
		else if (std::holds_alternative<ContentMemory>(after->m_source))
		{
			after->m_source = std::make_shared<std::vector<u8>>(
				std::get<ContentMemory>(after->m_source)->begin() + before->m_size,
				std::get<ContentMemory>(after->m_source)->end());
		}
		else if (std::holds_alternative<ContentPartition>(after->m_source))
		{
			std::get<ContentPartition>(after->m_source).m_offset += before->m_size;
		}
		else if (std::holds_alternative<ContentVolume>(after->m_source))
		{
			std::get<ContentVolume>(after->m_source).m_offset += before->m_size;
		}
	}

	static void ApplyPatchToFile(const Patch &patch, FSTBuilderNode *file_node,
								 const std::string &external_filename, u64 file_patch_offset,
								 u64 raw_external_file_offset, u64 file_patch_length, bool resize)
	{
		const auto f = patch.m_file_data_loader->GetExternalFileSize(external_filename);
		if (!f)
			return;

		auto &content = std::get<std::vector<BuilderContentSource>>(file_node->m_content);

		const u64 raw_external_filesize = *f;
		const u64 external_file_offset = std::min(raw_external_file_offset, raw_external_filesize);
		const u64 external_filesize = raw_external_filesize - external_file_offset;

		const u64 patch_start = file_patch_offset;
		const u64 patch_size = file_patch_length == 0 ? external_filesize : file_patch_length;
		const u64 patch_end = patch_start + patch_size;

		const u64 target_filesize = resize ? patch_end : std::max(file_node->m_size, patch_end);

		size_t insert_where = 0;
		if (patch_start >= file_node->m_size)
		{
			// If the patch is at or past the end of the existing file no existing content needs to be
			// touched, just extend the file.
			if (patch_start > file_node->m_size)
			{
				// Insert an padding area between the old file and the patch data.
				content.emplace_back(BuilderContentSource{file_node->m_size,
														  patch_start - file_node->m_size,
														  ContentFixedByte{}});
			}

			insert_where = content.size();
		}
		else
		{
			// Patch is at the start or somewhere in the middle of the existing file. At least one source
			// needs to be modified or removed, and a new source with the patch data inserted instead.
			// To make this easier, we first split up existing sources at the patch start and patch end
			// offsets, then discard all overlapping sources and insert the patch sources there.
			for (size_t i = 0; i < content.size(); ++i)
			{
				const u64 source_start = content[i].m_offset;
				const u64 source_end = source_start + content[i].m_size;
				if (patch_start > source_start && patch_start < source_end)
				{
					content.insert(content.begin() + i + 1, content[i]);
					SplitAt(&content[i], &content[i + 1], patch_start);
					continue;
				}
				if (patch_end > source_start && patch_end < source_end)
				{
					content.insert(content.begin() + i + 1, content[i]);
					SplitAt(&content[i], &content[i + 1], patch_end);
				}
			}

			// Now discard the overlapping areas and remember where they were so we can insert there.
			for (size_t i = 0; i < content.size(); ++i)
			{
				if (patch_start == content[i].m_offset)
				{
					insert_where = i;
					while (i < content.size() && patch_end >= content[i].m_offset + content[i].m_size)
						++i;
					content.erase(content.begin() + insert_where, content.begin() + i);
					break;
				}
			}
		}

		// Insert the actual patch data.
		if (patch_size > 0 && external_filesize > 0)
		{
			BuilderContentSource source = patch.m_file_data_loader->MakeContentSource(
				external_filename, external_file_offset, std::min(patch_size, external_filesize),
				patch_start);
			content.emplace(content.begin() + insert_where, std::move(source));
			++insert_where;
		}

		// Pad with zeroes if the patch file is smaller than the patch size.
		if (external_filesize < patch_size)
		{
			BuilderContentSource padding{patch_start + external_filesize,
										 patch_size - external_filesize, ContentFixedByte{}};
			content.emplace(content.begin() + insert_where, std::move(padding));
		}

		// Update the filesize of the file.
		file_node->m_size = target_filesize;

		// Drop any source past the new end of the file -- this can happen on file truncation.
		while (!content.empty() && content.back().m_offset >= target_filesize)
			content.pop_back();
	}

	static void ApplyPatchToFile(const Patch &patch, const File &file_patch,
								 FSTBuilderNode *file_node)
	{
		// The last two bits of the offset seem to be ignored by actual Riivolution.
		ApplyPatchToFile(patch, file_node, file_patch.m_external, file_patch.m_offset & ~u64(3),
						 file_patch.m_fileoffset, file_patch.m_length, file_patch.m_resize);
	}

	static FSTBuilderNode *FindFileNodeInFST(std::string_view path,
											 std::vector<FSTBuilderNode> *fst,
											 bool create_if_not_exists)
	{
		const size_t path_separator = path.find('/');
		const bool is_file = path_separator == std::string_view::npos;
		const std::string_view name = is_file ? path : path.substr(0, path_separator);
		const auto it = std::find_if(fst->begin(), fst->end(),
									 [&](const FSTBuilderNode &node) {
										 return CaseInsensitiveEquals(node.m_filename, name);
									 });

		if (it == fst->end())
		{
			if (!create_if_not_exists)
				return nullptr;

			if (is_file)
			{
				fst->emplace_back(FSTBuilderNode{std::string(name), 0,
												 std::vector<BuilderContentSource>()});
				return &fst->back();
			}

			FSTBuilderNode folder{std::string(name), 0, std::vector<FSTBuilderNode>()};
			fst->emplace_back(std::move(folder));
			return FindFileNodeInFST(path.substr(path_separator + 1),
									 &fst->back().GetFolderContent(), true);
		}

		const bool is_existing_node_file = it->IsFile();
		if (is_file != is_existing_node_file)
			return nullptr;
		if (is_file)
			return &(*it);

		return FindFileNodeInFST(path.substr(path_separator + 1),
								 &it->GetFolderContent(), create_if_not_exists);
	}

	static FSTBuilderNode *FindFilenameNodeInFST(std::string_view filename,
												 std::vector<FSTBuilderNode> &fst)
	{
		for (FSTBuilderNode &node : fst)
		{
			if (node.IsFolder())
			{
				FSTBuilderNode *result = FindFilenameNodeInFST(filename, node.GetFolderContent());
				if (result)
					return result;
			}
			else if (CaseInsensitiveEquals(node.m_filename, filename))
			{
				return &node;
			}
		}

		return nullptr;
	}

	static void ApplyFilePatchToFST(const Patch &patch, const File &file,
									std::vector<FSTBuilderNode> *fst, FSTBuilderNode *dol_node)
	{
		if (!file.m_disc.empty() && file.m_disc[0] == '/')
		{
			// If the disc path starts with a / then we should patch that specific disc path.
			FSTBuilderNode *node =
				FindFileNodeInFST(std::string_view(file.m_disc).substr(1), fst, file.m_create);
			if (node)
				ApplyPatchToFile(patch, file, node);
		}
		else if (dol_node && CaseInsensitiveEquals(file.m_disc, "main.dol"))
		{
			// Special case: If the filename is "main.dol", we want to patch the main executable.
			ApplyPatchToFile(patch, file, dol_node);
		}
		else
		{
			// Otherwise we want to patch the first file in the FST that matches that filename.
			FSTBuilderNode *node = FindFilenameNodeInFST(file.m_disc, *fst);
			if (node)
				ApplyPatchToFile(patch, file, node);
		}
	}

	// --- FST serialization core (DirectoryBlob.cpp BuildFST/Write* order) ---
	static const u32 ENTRY_SIZE = 0x0c;
	static const u8 FILE_ENTRY = 0;
	static const u8 DIRECTORY_ENTRY = 1;

	//! Minimal content sink standing in for DiscContentContainer: the byte
	//! comparison only needs fst_data, not the disc image layout.
	struct ContentSink
	{
		struct Item
		{
			u64 off;
			u64 size;
		};
		std::vector<Item> items;
		void Add(u64 off, u64 size) { items.push_back(Item{off, size}); }
	};

	static void Write32Be(std::vector<u8> &buf, u32 at, u32 v)
	{
		buf[at] = (u8) (v >> 24);
		buf[at + 1] = (u8) (v >> 16);
		buf[at + 2] = (u8) (v >> 8);
		buf[at + 3] = (u8) v;
	}

	static void WriteEntryData(std::vector<u8> *fst_data, u32 *entry_offset, u8 type,
							   u32 name_offset, u64 data_offset, u64 length, u32 address_shift)
	{
		(*fst_data)[(*entry_offset)++] = type;

		(*fst_data)[(*entry_offset)++] = (name_offset >> 16) & 0xff;
		(*fst_data)[(*entry_offset)++] = (name_offset >> 8) & 0xff;
		(*fst_data)[(*entry_offset)++] = (name_offset) & 0xff;

		Write32Be(*fst_data, *entry_offset, (u32) (data_offset >> address_shift));
		*entry_offset += 4;

		Write32Be(*fst_data, *entry_offset, (u32) length);
		*entry_offset += 4;
	}

	static void WriteEntryName(std::vector<u8> *fst_data, u32 *name_offset,
							   const std::string &name, u64 name_table_offset)
	{
		strncpy((char *) &(*fst_data)[*name_offset + name_table_offset], name.c_str(),
				name.length() + 1);

		*name_offset += (u32) (name.length() + 1);
	}

	static u32 ComputeNameSize(const std::vector<FSTBuilderNode> &files)
	{
		u32 name_size = 0;
		for (const FSTBuilderNode &entry : files)
		{
			if (entry.IsFolder())
				name_size += ComputeNameSize(entry.GetFolderContent());
			name_size += (u32) (entry.m_filename.length() + 1);
		}
		return name_size;
	}

	static size_t RecalculateFolderSizes(std::vector<FSTBuilderNode> *fst);
	static void WriteDirectory(std::vector<u8> *fst_data,
							   std::vector<FSTBuilderNode> *parent_entries, u32 *fst_offset,
							   u32 *name_offset, u64 *data_offset, u32 parent_entry_index,
							   u64 name_table_offset, u32 address_shift);

	static size_t RecalculateFolderSizes(std::vector<FSTBuilderNode> *fst)
	{
		size_t size = 0;
		for (FSTBuilderNode &entry : *fst)
		{
			++size;
			if (entry.IsFile())
				continue;

			entry.m_size = RecalculateFolderSizes(&entry.GetFolderContent());
			size += entry.m_size;
		}
		return size;
	}

	//! Byte-production core of BuildFST for a Wii image (address_shift 2),
	//! minus image layout (sequential data offsets, m_contents adds,
	//! disc_header writes) and minus SHIFT-JIS conversion (ASCII scope).
	//! Produces exactly the fst_data bytes upstream would emit for the
	//! same node tree, including case-insensitive sort and 4-byte name
	//! table padding.
	static std::vector<u8> BuildFstBytes(std::vector<FSTBuilderNode> root_nodes,
										 u32 address_shift)
	{
		u32 name_table_size = (u32) AlignUp(ComputeNameSize(root_nodes), 1ull << address_shift);

		// 1 extra for the root entry
		u64 total_entries = RecalculateFolderSizes(&root_nodes) + 1;

		const u64 name_table_offset = total_entries * ENTRY_SIZE;
		std::vector<u8> fst_data((size_t) (name_table_offset + name_table_size), 0);

		u32 fst_offset = 0;
		u32 name_offset = 0;
		u32 root_offset = 0;

		// write root entry
		WriteEntryData(&fst_data, &fst_offset, DIRECTORY_ENTRY, 0, 0, total_entries,
					   address_shift);

		// write the tree (sort + entries + names), data offsets assigned
		// sequentially here exactly as upstream does; callers comparing
		// against our window layout must mask stored data offsets.
		u64 data_offset = 0;
		WriteDirectory(&fst_data, &root_nodes, &fst_offset, &name_offset, &data_offset,
					   root_offset, name_table_offset, address_shift);

		assert(AlignUp(name_offset, 1ull << address_shift) == name_table_size);
		return fst_data;
	}

	static void WriteDirectory(std::vector<u8> *fst_data,
							   std::vector<FSTBuilderNode> *parent_entries, u32 *fst_offset,
							   u32 *name_offset, u64 *data_offset, u32 parent_entry_index,
							   u64 name_table_offset, u32 address_shift)
	{
		std::vector<FSTBuilderNode> &sorted_entries = *parent_entries;

		// Sort for determinism
		std::sort(sorted_entries.begin(), sorted_entries.end(),
				  [](const FSTBuilderNode &one, const FSTBuilderNode &two) {
					  std::string one_upper = ToUpperStr(one.m_filename);
					  std::string two_upper = ToUpperStr(two.m_filename);
					  return one_upper == two_upper ? one.m_filename < two.m_filename :
													  one_upper < two_upper;
				  });

		for (FSTBuilderNode &entry : sorted_entries)
		{
			if (entry.IsFolder())
			{
				u32 entry_index = *fst_offset / ENTRY_SIZE;
				WriteEntryData(fst_data, fst_offset, DIRECTORY_ENTRY, *name_offset,
							   parent_entry_index, entry_index + (u32) entry.m_size + 1, 0);
				WriteEntryName(fst_data, name_offset, entry.m_filename, name_table_offset);

				auto &child_nodes = entry.GetFolderContent();
				WriteDirectory(fst_data, &child_nodes, fst_offset, name_offset, data_offset,
							   entry_index, name_table_offset, address_shift);
			}
			else
			{
				// put entry in FST
				WriteEntryData(fst_data, fst_offset, FILE_ENTRY, *name_offset, *data_offset,
							   entry.m_size, address_shift);
				WriteEntryName(fst_data, name_offset, entry.m_filename, name_table_offset);

				const u64 data_alignment = 0x8000ull;
				*data_offset = AlignUp(*data_offset + entry.m_size, data_alignment);
			}
		}
	}
}

// ------------------------------------------------------------------
// Test-side helpers (ours).
// ------------------------------------------------------------------

//! Raw 12-byte FST entry decode, independent of both serializers (the same
//! pattern as test_redirtable's independent decoder).
struct RawEntry
{
	u8 type;
	u32 nameOff;
	u32 arg1;
	u32 arg2;
	std::string name;
};

static u32 RawBe32(const std::vector<u8> &t, size_t at)
{
	return ((u32) t[at] << 24) | ((u32) t[at + 1] << 16) | ((u32) t[at + 2] << 8) | t[at + 3];
}

static bool DecodeFst(const std::vector<u8> &t, std::vector<RawEntry> &entries,
					  std::string &why)
{
	entries.clear();
	if (t.size() < 12)
	{
		why = "too short";
		return false;
	}
	const u32 count = RawBe32(t, 8);
	if ((u64) count * 12 > t.size())
	{
		why = "count runs past the buffer";
		return false;
	}
	const size_t strTab = (size_t) count * 12;
	for (u32 i = 0; i < count; ++i)
	{
		RawEntry e;
		e.type = t[i * 12];
		e.nameOff = ((u32) t[i * 12 + 1] << 16) | ((u32) t[i * 12 + 2] << 8) | t[i * 12 + 3];
		e.arg1 = RawBe32(t, i * 12 + 4);
		e.arg2 = RawBe32(t, i * 12 + 8);
		const size_t at = strTab + e.nameOff;
		if (at >= t.size())
		{
			why = "name runs past the buffer";
			return false;
		}
		while (at + e.name.size() < t.size() && t[at + e.name.size()])
			e.name.push_back((char) t[at + e.name.size()]);
		if (at + e.name.size() >= t.size())
		{
			why = "unterminated name";
			return false;
		}
		entries.push_back(e);
	}
	return true;
}

static std::string FullPath(const std::string &dir, const std::string &leaf)
{
	if (dir.empty())
		return "/" + leaf;
	if (leaf.empty())
		return dir.empty() ? "/" : dir;
	return dir + "/" + leaf;
}

//! path -> (arg1, arg2), split by kind, for semantic comparison.
static void EntryMaps(const std::vector<RawEntry> &entries,
					  std::map<std::string, std::pair<u32, u32>> &files,
					  std::map<std::string, std::pair<u32, u32>> &dirs)
{
	files.clear();
	dirs.clear();
	std::vector<std::string> stack;
	stack.push_back("");
	std::vector<u32> dirEnds;
	for (size_t i = 1; i < entries.size(); ++i)
	{
		while (!dirEnds.empty() && i >= dirEnds.back())
		{
			stack.pop_back();
			dirEnds.pop_back();
		}
		const RawEntry &e = entries[i];
		const std::string p = FullPath(stack.back(), e.name);
		if (e.type == 1)
		{
			dirs[p] = std::make_pair(e.arg1, e.arg2);
			stack.push_back(p);
			dirEnds.push_back(e.arg2);
		}
		else
			files[p] = std::make_pair(e.arg1, e.arg2);
	}
}

//! Synthetic input FST image (shifted Wii offsets) from a logical tree.
//! Entry order = insertion order, so our serializer's order policy stays
//! visible; Dolphin's side sorts on serialize by design.
struct LogicFile
{
	std::string dir; // "" for root, else "StageData" etc. (single level is enough)
	std::string name;
	u64 offset;      // byte offset on the disc
	u32 length;
};

static void WrBe32(std::vector<u8> &v, size_t at, u32 x)
{
	v[at] = (u8) (x >> 24);
	v[at + 1] = (u8) (x >> 16);
	v[at + 2] = (u8) (x >> 8);
	v[at + 3] = (u8) x;
}

static std::vector<u8> MakeInputImage(const std::vector<std::string> &dirs,
									  const std::vector<LogicFile> &files)
{
	struct SEnt
	{
		u8 type;
		std::string name;
		u32 a, b;
	};
	std::vector<SEnt> e;
	// entry 0: root. Filled below once the total is known.
	e.push_back(SEnt{1, "", 0, 0});
	// directories first (each spans to the next sibling at this level only
	// in this minimal builder: single-level trees keep every dir's range
	// exact by construction below).
	std::map<std::string, std::vector<SEnt>> perDir;
	for (size_t i = 0; i < files.size(); ++i)
	{
		if (files[i].dir.empty())
			e.push_back(SEnt{0, files[i].name, (u32) (files[i].offset >> 2), files[i].length});
		else
			perDir[files[i].dir].push_back(
				SEnt{0, files[i].name, (u32) (files[i].offset >> 2), files[i].length});
	}
	for (size_t i = 0; i < dirs.size(); ++i)
	{
		const u32 self = (u32) e.size();
		e.push_back(SEnt{1, dirs[i], 0, 0}); // parent 0, end patched below
		u32 end = self + 1 + (u32) perDir[dirs[i]].size();
		e[self].b = end;
		for (size_t k = 0; k < perDir[dirs[i]].size(); ++k)
			e.push_back(perDir[dirs[i]][k]);
	}
	e[0].b = (u32) e.size();

	const u32 n = (u32) e.size();
	std::vector<u8> out(n * 12, 0);
	std::string strings;
	strings.push_back(0); // root name: leading NUL, like real discs
	for (u32 i = 0; i < n; ++i)
	{
		u32 nameOff = (u32) strings.size();
		strings += e[i].name;
		strings.push_back(0);
		out[i * 12] = e[i].type;
		out[i * 12 + 1] = (u8) (nameOff >> 16);
		out[i * 12 + 2] = (u8) (nameOff >> 8);
		out[i * 12 + 3] = (u8) nameOff;
		WrBe32(out, i * 12 + 4, e[i].a);
		WrBe32(out, i * 12 + 8, e[i].b);
	}
	out.insert(out.end(), strings.begin(), strings.end());
	return out;
}

//! Mirror logical tree as DolphinRef nodes (unsorted insertion; their
//! serializer sorts). Indices, not pointers: pushing children can
//! reallocate the vectors.
static std::vector<DolphinRef::FSTBuilderNode>
MakeDolphinTree(const std::vector<std::string> &dirs, const std::vector<LogicFile> &files)
{
	using namespace DolphinRef;
	std::vector<FSTBuilderNode> root;
	root.reserve(dirs.size() + files.size());
	std::map<std::string, size_t> dirIndex;
	for (size_t i = 0; i < dirs.size(); ++i)
	{
		FSTBuilderNode d;
		d.m_filename = dirs[i];
		d.m_size = 0;
		d.m_content = std::vector<FSTBuilderNode>();
		root.push_back(std::move(d));
		dirIndex[dirs[i]] = root.size() - 1;
	}
	for (size_t i = 0; i < files.size(); ++i)
	{
		FSTBuilderNode f;
		f.m_filename = files[i].name;
		f.m_size = files[i].length;
		std::vector<BuilderContentSource> content;
		content.push_back(BuilderContentSource{0, files[i].length, ContentFixedByte{}});
		f.m_content = std::move(content);
		if (files[i].dir.empty())
			root.push_back(std::move(f));
		else
			root[dirIndex[files[i].dir]].GetFolderContent().push_back(std::move(f));
	}
	return root;
}

//! Stub loader: synthetic external sizes, content never read (only the
//! resulting sizes, offsets and segment maps are compared).
struct StubLoader : public DolphinRef::FileDataLoader
{
	std::map<std::string, u64> sizes;
	std::optional<u64> GetExternalFileSize(const std::string &p) override
	{
		std::map<std::string, u64>::const_iterator it = sizes.find(p);
		if (it == sizes.end())
			return std::nullopt;
		return it->second;
	}
	DolphinRef::BuilderContentSource MakeContentSource(const std::string &p, u64 extOff,
													   u64 extSize, u64 discOff) override
	{
		return DolphinRef::BuilderContentSource{
			discOff, extSize, DolphinRef::ContentFile{p, extOff}};
	}
};

//! Collect (offset, size, isFile, fileOffset-or-0) segments of a patched node.
struct Seg
{
	u64 off;
	u64 size;
	bool isFile;
	u64 fileOff;
};

static void CollectSegs(const DolphinRef::FSTBuilderNode &node, std::vector<Seg> &out)
{
	const std::vector<DolphinRef::BuilderContentSource> &c = std::get<
		std::vector<DolphinRef::BuilderContentSource>>(node.m_content);
	for (size_t i = 0; i < c.size(); ++i)
	{
		Seg s;
		s.off = c[i].m_offset;
		s.size = c[i].m_size;
		s.isFile = std::holds_alternative<DolphinRef::ContentFile>(c[i].m_source);
		s.fileOff = s.isFile ? std::get<DolphinRef::ContentFile>(c[i].m_source).m_offset : 0;
		out.push_back(s);
	}
}

static DolphinRef::FSTBuilderNode *FindDolphinNode(std::vector<DolphinRef::FSTBuilderNode> &root,
												   const std::string &dir,
												   const std::string &name)
{
	for (size_t i = 0; i < root.size(); ++i)
	{
		if (!root[i].IsFolder() || root[i].m_filename != dir)
			continue;
		std::vector<DolphinRef::FSTBuilderNode> &kids = root[i].GetFolderContent();
		for (size_t k = 0; k < kids.size(); ++k)
			if (kids[k].IsFile() && kids[k].m_filename == name)
				return &kids[k];
	}
	return nullptr;
}

// ------------------------------------------------------------------
// Shared sample tree. Mixed case on purpose: Dolphin's serializer sorts
// case-insensitively, ours preserves insertion order. Both policies are
// pinned below; only linear lookup is required of games, so both boot.
// ------------------------------------------------------------------
static void SampleTree(std::vector<std::string> &dirs, std::vector<LogicFile> &files)
{
	dirs.clear();
	dirs.push_back("StageData");
	files.clear();
	files.push_back(LogicFile{"", "top.bin", 0x4000, 0x40});
	files.push_back(LogicFile{"", "A.bin", 0x5000, 0x100});
	files.push_back(LogicFile{"StageData", "b.arc", 0x1000, 0x800});
	files.push_back(LogicFile{"StageData", "C.arc", 0x2000, 0x400});
}

static bool OurParse(Riivo::FstBuilder &b, const std::vector<std::string> &dirs,
					 const std::vector<LogicFile> &files)
{
	const std::vector<u8> img = MakeInputImage(dirs, files);
	return b.Parse(&img[0], (u32) img.size(), true);
}

static u32 OurPatchedLen(const std::vector<std::string> &dirs,
						 const std::vector<LogicFile> &files, const std::string &discPath,
						 u32 extSize)
{
	Riivo::FstBuilder b;
	if (!OurParse(b, dirs, files))
		return 0xFFFFFFFFu;
	bool wasNew = false;
	b.AddOrReplace(discPath, extSize, &wasNew);
	u64 off = 0;
	u32 len = 0;
	if (!b.FindAssigned(discPath, &off, &len))
		return 0xFFFFFFFEu;
	return len;
}

//! Children names per directory, in encounter order (order policy check).
static void ChildOrders(const std::vector<RawEntry> &entries,
						std::map<std::string, std::vector<std::string>> &out)
{
	out.clear();
	out["/"] = std::vector<std::string>();
	std::vector<std::string> stack;
	stack.push_back("");
	std::vector<u32> dirEnds;
	for (size_t i = 1; i < entries.size(); ++i)
	{
		while (!dirEnds.empty() && i >= dirEnds.back())
		{
			stack.pop_back();
			dirEnds.pop_back();
		}
		const RawEntry &e = entries[i];
		const std::string here = stack.back().empty() ? "/" : stack.back();
		out[here].push_back(e.name);
		if (e.type == 1)
		{
			const std::string p = FullPath(stack.back(), e.name);
			stack.push_back(p);
			dirEnds.push_back(e.arg2);
			out[p] = std::vector<std::string>();
		}
	}
}

static std::string JoinNames(const std::vector<std::string> &v)
{
	std::string s;
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (i)
			s += ",";
		s += v[i];
	}
	return s;
}

//! Sorted name multiset of every non-root entry (order-independent).
static std::vector<std::string> NameMultiset(const std::vector<RawEntry> &entries)
{
	std::vector<std::string> v;
	for (size_t i = 1; i < entries.size(); ++i)
		v.push_back(entries[i].name);
	std::sort(v.begin(), v.end());
	return v;
}

struct SizeLoader : public DolphinRef::FileDataLoader
{
	std::map<std::string, u64> sizes;
	std::optional<u64> GetExternalFileSize(const std::string &p) override
	{
		std::map<std::string, u64>::const_iterator it = sizes.find(p);
		if (it == sizes.end())
			return std::nullopt;
		return it->second;
	}
	DolphinRef::BuilderContentSource MakeContentSource(const std::string &p, u64 extOff,
													   u64 extSize, u64 discOff) override
	{
		return DolphinRef::BuilderContentSource{
			discOff, extSize, DolphinRef::ContentFile{p, extOff}};
	}
};

//! Apply one Dolphin <file> patch to a tree; returns the patched node.
static DolphinRef::FSTBuilderNode *DPatch(std::vector<DolphinRef::FSTBuilderNode> &root,
										  SizeLoader &loader, const char *disc, const char *ext,
										  u32 off, u32 foff, u32 len, bool resize, bool create,
										  u64 extSize)
{
	loader.sizes[ext] = extSize;
	DolphinRef::Patch p;
	p.m_file_data_loader = &loader;
	DolphinRef::File f;
	f.m_disc = disc;
	f.m_external = ext;
	f.m_offset = off;
	f.m_fileoffset = foff;
	f.m_length = len;
	f.m_resize = resize;
	f.m_create = create;
	p.m_file_patches.push_back(f);
	DolphinRef::ApplyFilePatchToFST(p, p.m_file_patches[0], &root, nullptr);
	return DolphinRef::FindFileNodeInFST(disc[0] == '/' ? std::string(disc).substr(1) : disc,
										 &root, false);
}

//! Production RedirectSpec for one <file> rule, through the real resolver.
static bool OurSpec(const std::vector<std::string> &dirs, const std::vector<LogicFile> &files,
					const std::string &disc, const std::string &extRoot,
					const std::string &ext, u32 off, u32 foff, u32 len, bool resize, bool create,
					Riivo::RedirectSpec &spec)
{
	const std::vector<u8> img = MakeInputImage(dirs, files);
	Riivo::Fst fst;
	if (!fst.Parse(&img[0], (u32) img.size(), true))
		return false;
	Riivo::ResolvedPatchSet set;
	Riivo::ResolvedFile rf;
	rf.root = extRoot;
	rf.disc = disc;
	rf.external = ext;
	rf.resize = resize;
	rf.create = create;
	rf.offset = off;
	rf.fileoffset = foff;
	rf.length = len;
	set.files.push_back(rf);
	std::vector<Riivo::RedirectSpec> out;
	std::vector<Riivo::CreatedFile> created;
	Riivo::BuildRedirects(fst, set, "sd:", nullptr, out, &created);
	if (out.empty())
		return false;
	spec = out[0];
	return true;
}

static bool OurManifestLen(const Riivo::RedirectSpec &spec, const std::string &extFull,
						   u32 extSize, bool resize, u32 &lenOut, std::string &why)
{
	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(spec);
	std::map<std::string, u32> sizes;
	sizes[extFull] = extSize;
	std::vector<bool> flags(1, resize);
	std::vector<Riivo::ManifestExtent> ext;
	if (!Riivo::BuildManifestExtents(specs, sizes, flags, ext, why))
		return false;
	lenOut = ext[0].length;
	return true;
}

static void TestSerializerBaseline()
{
	std::printf("1. serializer baseline, no patches\n");
	std::vector<std::string> dirs;
	std::vector<LogicFile> files;
	SampleTree(dirs, files);

	std::vector<DolphinRef::FSTBuilderNode> droot = MakeDolphinTree(dirs, files);
	const std::vector<u8> dbytes = DolphinRef::BuildFstBytes(std::move(droot), 2);

	Riivo::FstBuilder b;
	check(OurParse(b, dirs, files), "our sample parses");
	std::vector<u8> gbytes;
	b.Serialize(gbytes, true);

	std::vector<RawEntry> de, ge;
	std::string why;
	check(DecodeFst(dbytes, de, why), "dolphin bytes decode");
	check(DecodeFst(gbytes, ge, why), "our bytes decode");
	if (de.empty() || ge.empty())
		return;
	std::printf("  dolphin entries:\n");
	for (size_t i = 0; i < de.size(); ++i)
		std::printf("    [%u] t=%u name='%s' a1=%08x a2=%08x\n", (unsigned) i, de[i].type,
					 de[i].name.c_str(), de[i].arg1, de[i].arg2);
	std::printf("  ours entries:\n");
	for (size_t i = 0; i < ge.size(); ++i)
		std::printf("    [%u] t=%u name='%s' a1=%08x a2=%08x\n", (unsigned) i, ge[i].type,
					 ge[i].name.c_str(), ge[i].arg1, ge[i].arg2);
	check(de.size() == ge.size(), "same entry count");
	check(de[0].type == 1 && ge[0].type == 1, "both roots are directories");
	check(de[0].arg2 == ge[0].arg2 && de[0].arg2 == de.size(),
		  "root length is the entry count on both");

	std::map<std::string, std::pair<u32, u32>> dfiles, ddirs, gfiles, gdirs;
	EntryMaps(de, dfiles, ddirs);
	EntryMaps(ge, gfiles, gdirs);
	check(dfiles.size() == gfiles.size() && ddirs.size() == gdirs.size(),
		  "same file and directory sets");
	// Lengths must agree; stored data offsets are each side's own layout
	// (Dolphin assigns sequential 0x8000-aligned image offsets here, we
	// keep the disc's), so only lengths are compared. Untouched-offset
	// preservation on our side is covered by test_fstbuild round-trips.
	bool lensOk = dfiles.size() == gfiles.size();
	if (lensOk)
	{
		for (std::map<std::string, std::pair<u32, u32>>::const_iterator it = dfiles.begin();
			 it != dfiles.end(); ++it)
		{
			std::map<std::string, std::pair<u32, u32>>::const_iterator g = gfiles.find(it->first);
			if (g == gfiles.end() || g->second.second != it->second.second)
			{
				lensOk = false;
				break;
			}
		}
	}
	check(lensOk, "same paths and lengths");
	check(NameMultiset(de) == NameMultiset(ge), "same name multiset");

	// Ordering policy, pinned as a deliberate difference: Dolphin sorts
	// case-insensitively at serialize time; we preserve insertion order.
	// Linear lookup (all the SDK requires) is unaffected either way.
	std::map<std::string, std::vector<std::string>> dord, gord;
	ChildOrders(de, dord);
	ChildOrders(ge, gord);
	check(JoinNames(dord["/"]) == "A.bin,StageData,top.bin", "dolphin root order is sorted");
	check(JoinNames(gord["/"]) == "top.bin,A.bin,StageData", "our root order is insertion");
	check(JoinNames(dord["/StageData"]) == "b.arc,C.arc", "dolphin dir order is sorted");

	// Trailing padding: Dolphin pads the string table to a multiple of 4
	// (Wii shift); we emit it exact, like the discs our tables replace.
	// Both are inert past the last NUL; the sizes legitimately differ.
	const u32 n = (u32) de.size();
	const u64 dstr = (u64) dbytes.size() - (u64) n * 12;
	const u64 gstr = (u64) gbytes.size() - (u64) n * 12;
	check(dbytes.size() % 4 == 0, "dolphin total is 4-aligned");
	check(dstr == ((gstr - 1 + 3) & ~3ull), "dolphin strings = ours minus leading NUL, padded");
	std::printf("  dolphin %u bytes, ours %u bytes\n", (unsigned) dbytes.size(),
				(unsigned) gbytes.size());
}

static void DumpSegs(const char *tag, const std::vector<Seg> &segs)
{
	std::printf("  %s segs=%u:", tag, (unsigned) segs.size());
	for (size_t i = 0; i < segs.size(); ++i)
		std::printf(" [%llx+%llx %s%s%llx]", (unsigned long long) segs[i].off,
					 (unsigned long long) segs[i].size, segs[i].isFile ? "file@" : "orig",
					 segs[i].isFile ? "" : "", (unsigned long long) segs[i].fileOff);
	std::printf("\n");
}

static void TestPatchSizes()
{
	std::printf("2. file-patch sizes, both engines\n");
	std::vector<std::string> dirs;
	std::vector<LogicFile> files;
	SampleTree(dirs, files);

	// T1: full replace, same size.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0, 0, true, false, 0x800);
		check(node && node->m_size == 0x800, "T1 dolphin keeps size");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x800) == 0x800,
			  "T1 ours keeps size");
	}
	// T2: bigger + resize.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0, 0, true, false, 0x1000);
		check(node && node->m_size == 0x1000, "T2 dolphin grows");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x1000) == 0x1000, "T2 ours grows");
	}
	// T3: smaller + resize truncates on both.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0, 0, true, false, 0x200);
		check(node && node->m_size == 0x200, "T3 dolphin truncates");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x200) == 0x200,
			  "T3 ours truncates");
	}
	// T4: smaller + resize=false. Dolphin keeps the original size and the
	// original tail; our rebuilt table truncates to the external size.
	// Pinned divergence, both values recorded; no test mod uses it.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0, 0, false, false, 0x200);
		check(node && node->m_size == 0x800, "T4 dolphin keeps original size");
		std::vector<Seg> segs;
		if (node)
			CollectSegs(*node, segs);
		check(segs.size() == 2 && !segs[0].isFile == false && segs[0].size == 0x200 &&
				  !segs[1].isFile && segs[1].off == 0x200 && segs[1].size == 0x600,
			  "T4 dolphin serves patch prefix plus original tail");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x200) == 0x200,
			  "T4 ours truncates (diverges; see comment)");
	}
	// T5: bigger + resize=false. Both grow (max rule); our manifest
	// extent clamps to the original size and under-serves - pinned.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0, 0, false, false, 0x1000);
		check(node && node->m_size == 0x1000, "T5 dolphin grows under max rule");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x1000) == 0x1000,
			  "T5 ours grows too");
		Riivo::RedirectSpec spec;
		check(OurSpec(dirs, files, "/StageData/b.arc", "/mod", "b.bin", 0, 0, 0, false, false,
					  spec),
			  "T5 production spec exists");
		u32 mlen = 0;
		std::string why;
		check(OurManifestLen(spec, "sd:/mod/b.bin", 0x1000, false, mlen, why) && mlen == 0x800,
			  "T5 manifest clamps (diverges; see comment)");
	}
}

static void TestPartialAndCreate()
{
	std::printf("3. partial ranges, unaligned offsets, creation\n");
	std::vector<std::string> dirs;
	std::vector<LogicFile> files;
	SampleTree(dirs, files);

	// T6: partial (offset 0x40, fileoffset 0x10, length 0x20). Dolphin
	// clips to the request with the source offset advanced - and, note,
	// sizes a resize=true partial patch at patch_end (0x60 here), dropping
	// the original tail. Whether hardware Riivolution truncates window
	// patches is not established by either side; our table keeps the full
	// external size and serves the window from it. Pinned, not adjudicated.
	// Our spec preserves all three fields for the manifest.
	{
		SizeLoader loader;
		loader.sizes["ext"] = 0x800;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::Patch p;
		p.m_file_data_loader = &loader;
		DolphinRef::File f;
		f.m_disc = "/StageData/b.arc";
		f.m_external = "ext";
		f.m_offset = 0x40;
		f.m_fileoffset = 0x10;
		f.m_length = 0x20;
		p.m_file_patches.push_back(f);
		DolphinRef::ApplyFilePatchToFST(p, p.m_file_patches[0], &root, nullptr);
		DolphinRef::FSTBuilderNode *node = FindDolphinNode(root, "StageData", "b.arc");
		std::vector<Seg> segs;
		if (node)
			CollectSegs(*node, segs);
		else
			std::printf("  T6 node NOT FOUND\n");
		DumpSegs("T6", segs);
		check(node && node->m_size == 0x60,
			  "T6 dolphin sizes partial+resize at patch end");
		check(segs.size() == 2 && !segs[0].isFile && segs[0].off == 0 && segs[0].size == 0x40 &&
				  segs[1].isFile && segs[1].off == 0x40 && segs[1].size == 0x20 &&
				  segs[1].fileOff == 0x10,
			  "T6 dolphin clips with source offset advanced, tail dropped");
		check(OurPatchedLen(dirs, files, "/stagedata/b.arc", 0x800) == 0x800,
			  "T6 ours keeps full size (diverges; see comment)");
		Riivo::RedirectSpec spec;
		check(OurSpec(dirs, files, "/StageData/b.arc", "/mod", "b.bin", 0x40, 0x10, 0x20, true,
					  false, spec) &&
				  spec.discOffset == 0x1040 && spec.length == 0x20 && spec.fileOffset == 0x10,
			  "T6 ours preserves sub-range triple");
		u32 mlen = 0;
		std::string why;
		check(OurManifestLen(spec, "sd:/mod/b.bin", 0x800, true, mlen, why) && mlen == 0x20,
			  "T6 manifest carries the sub-range");
	}
	// T7: unaligned offset 5. Real Riivolution (per Dolphin) masks the low
	// two bits; our resolver keeps the raw value. Pinned divergence.
	{
		SizeLoader loader;
		loader.sizes["ext"] = 0x800;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::Patch p;
		p.m_file_data_loader = &loader;
		DolphinRef::File f;
		f.m_disc = "/StageData/b.arc";
		f.m_external = "ext";
		f.m_offset = 5;
		f.m_length = 0x20;
		p.m_file_patches.push_back(f);
		DolphinRef::ApplyFilePatchToFST(p, p.m_file_patches[0], &root, nullptr);
		DolphinRef::FSTBuilderNode *node = FindDolphinNode(root, "StageData", "b.arc");
		std::vector<Seg> segs;
		if (node)
			CollectSegs(*node, segs);
		check(!segs.empty() && !segs[0].isFile && segs[0].size == 4 &&
				  segs.size() > 1 && segs[1].isFile && segs[1].off == 4,
			  "T7 dolphin masks offset to 4");
		Riivo::RedirectSpec spec;
		check(OurSpec(dirs, files, "/StageData/b.arc", "/mod", "b.bin", 5, 0, 0x20, true,
					  false, spec) &&
				  spec.discOffset == 0x1005,
			  "T7 ours keeps raw offset (diverges; see comment)");
	}
	// T8: create new files, including a nested path whose parents are new.
	// Sizes are captured immediately: later tree growth can reallocate.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		bool n1ok = false, n2ok = false;
		u64 n1size = 0, n2size = 0;
		DolphinRef::FSTBuilderNode *n1 =
			DPatch(root, loader, "/StageData/new.arc", "ext", 0, 0, 0, true, true, 0x300);
		if (n1)
		{
			n1ok = true;
			n1size = n1->m_size;
		}
		DolphinRef::FSTBuilderNode *n2 =
			DPatch(root, loader, "/NewDir/deep.arc", "ext2", 0, 0, 0, true, true, 0x100);
		if (n2)
		{
			n2ok = true;
			n2size = n2->m_size;
		}
		check(n1ok && n1size == 0x300, "T8 dolphin creates with size");
		check(n2ok && n2size == 0x100, "T8 dolphin creates parents too");
		check(OurPatchedLen(dirs, files, "/stagedata/new.arc", 0x300) == 0x300,
			  "T8 ours creates with size");
		check(OurPatchedLen(dirs, files, "/newdir/deep.arc", 0x100) == 0x100,
			  "T8 ours creates parents too");
	}
	// T9: fileoffset past EOF. With resize=false Dolphin clamps to a
	// silent no-op; with resize=true the same code sizes the file at
	// patch_end (here 0) and drops all content - an upstream edge worth
	// knowing, not matching. Our manifest builder refuses by file name
	// either way. All three behaviors are safe (nothing serves garbage);
	// the mechanisms differ and are pinned.
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node =
			DPatch(root, loader, "/StageData/b.arc", "ext", 0, 0x2000, 0, false, false, 0x800);
		check(node && node->m_size == 0x800, "T9 dolphin no-resize leaves size alone");
		std::vector<DolphinRef::FSTBuilderNode> root2 = MakeDolphinTree(dirs, files);
		DolphinRef::FSTBuilderNode *node2 =
			DPatch(root2, loader, "/StageData/b.arc", "ext", 0, 0x2000, 0, true, false, 0x800);
		check(node2 && node2->m_size == 0, "T9 dolphin resize sizes past-EOF at patch end");
		Riivo::RedirectSpec spec;
		check(OurSpec(dirs, files, "/StageData/b.arc", "/mod", "b.bin", 0, 0x2000, 0, true,
					  false, spec),
			  "T9 production spec exists");
		u32 mlen = 0;
		std::string why;
		check(!OurManifestLen(spec, "sd:/mod/b.bin", 0x800, true, mlen, why) &&
				  why.find("b.bin") != std::string::npos,
			  "T9 ours refuses by file name");
	}
	// T10: bare filename matches the first file of that name, both sides.
	// Production resolves bare names through Fst::FindFile (full path out)
	// before the builder ever sees them; the test mirrors that path rather
	// than handing the builder a bare name it never receives. (DPatch's
	// path-based return lookup cannot see bare names either, so the node is
	// located with the filename search upstream itself uses.)
	{
		SizeLoader loader;
		std::vector<DolphinRef::FSTBuilderNode> root = MakeDolphinTree(dirs, files);
		DolphinRef::Patch p;
		p.m_file_data_loader = &loader;
		loader.sizes["ext"] = 0x800;
		DolphinRef::File f;
		f.m_disc = "b.arc";
		f.m_external = "ext";
		p.m_file_patches.push_back(f);
		DolphinRef::ApplyFilePatchToFST(p, p.m_file_patches[0], &root, nullptr);
		DolphinRef::FSTBuilderNode *bnode =
			DolphinRef::FindFilenameNodeInFST("b.arc", root);
		check(bnode && bnode->m_filename == "b.arc" && bnode->m_size == 0x800,
			  "T10 dolphin bare-name match");
		const std::vector<u8> img = MakeInputImage(dirs, files);
		Riivo::Fst fst;
		check(fst.Parse(&img[0], (u32) img.size(), true), "T10 our image parses");
		const Riivo::FstFile *found = fst.FindFile("b.arc");
		check(found && found->path == "/stagedata/b.arc", "T10 ours bare-name resolves");
		check(found && OurPatchedLen(dirs, files, found->path, 0x800) == 0x800,
			  "T10 ours patches through the resolved path");
	}
}

int main()
{
	TestSerializerBaseline();
	TestPatchSizes();
	TestPartialAndCreate();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
