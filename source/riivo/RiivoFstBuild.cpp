/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <ctype.h>
#include <string.h>
#include "RiivoFstBuild.hpp"

namespace Riivo
{
	// --------------------------------------------------------------------
	// Big-endian helpers. The FST is big-endian on disc; read and write it
	// explicitly so this behaves the same on the PPC target and on a
	// little-endian host running the tests.
	// --------------------------------------------------------------------

	static u32 be32(const u8 *p)
	{
		return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
	}

	static u32 be24(const u8 *p)
	{
		return ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
	}

	static void wr32(u8 *p, u32 v)
	{
		p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16);
		p[2] = (u8) (v >> 8);  p[3] = (u8) v;
	}

	static void wr24(u8 *p, u32 v)
	{
		p[0] = (u8) (v >> 16); p[1] = (u8) (v >> 8); p[2] = (u8) v;
	}

	static std::string ToLower(const std::string &s)
	{
		std::string r = s;
		for (size_t i = 0; i < r.size(); ++i)
			r[i] = (char) tolower((unsigned char) r[i]);
		return r;
	}

	//! Split "/a/b/c.arc" into {"a","b","c.arc"}. Empty components are dropped
	//! so a doubled slash or a trailing one cannot create a nameless entry.
	static void SplitPath(const std::string &path, std::vector<std::string> &out)
	{
		out.clear();
		size_t i = 0;
		while (i < path.size())
		{
			while (i < path.size() && path[i] == '/')
				++i;
			size_t j = i;
			while (j < path.size() && path[j] != '/')
				++j;
			if (j > i)
				out.push_back(path.substr(i, j - i));
			i = j;
		}
	}

	// --------------------------------------------------------------------
	// Parsing: flat table -> tree
	// --------------------------------------------------------------------

	static std::string ReadName(const u8 *fst, u32 fstSize, u32 strOff)
	{
		std::string name;
		for (u32 i = strOff; i < fstSize && fst[i]; ++i)
			name += (char) fst[i];
		return name;
	}

	//! Recursively consume entries [*idx, end) as children of `parent`.
	static void ParseInto(FstNode &parent, const u8 *fst, u32 fstSize, u32 strTable,
						  u32 count, bool shifted, u32 *idx, u32 end)
	{
		while (*idx < end && *idx < count)
		{
			const u8 *e = fst + (*idx) * 12;
			const u8 type = e[0];
			const std::string name = ReadName(fst, fstSize, strTable + be24(e + 1));
			const u32 here = *idx;
			++(*idx);

			FstNode node;
			node.name = name;

			if (type == 1)
			{
				u32 subEnd = be32(e + 8);
				//! A corrupt or hostile table could point the subtree end
				//! backwards or past the array; clamp rather than run wild.
				if (subEnd <= here || subEnd > count)
					subEnd = count;
				node.isDir = true;
				ParseInto(node, fst, fstSize, strTable, count, shifted, idx, subEnd);
			}
			else
			{
				const u32 raw = be32(e + 4);
				node.isDir = false;
				//! Widen before shifting: (raw << 2) in 32 bits wraps once the
				//! stored value passes 0x40000000, which a dual-layer disc does.
				node.offset = shifted ? ((u64) raw << 2) : (u64) raw;
				node.length = be32(e + 8);
			}
			parent.children.push_back(node);
		}
	}

	bool FstBuilder::Parse(const u8 *fstData, u32 fstSize, bool shifted)
	{
		root = FstNode();
		root.isDir = true;
		stats = FstBuildStats();

		if (!fstData || fstSize < 12)
			return false;

		const u32 count = be32(fstData + 8); // root length == total entry count
		if (count == 0 || (u64) count * 12 > fstSize)
			return false;

		const u32 strTable = count * 12;
		u32 idx = 1; // entry 0 is the root itself
		ParseInto(root, fstData, fstSize, strTable, count, shifted, &idx, count);

		stats.origFstSize = fstSize;
		return true;
	}

	// --------------------------------------------------------------------
	// Editing
	// --------------------------------------------------------------------

	static FstNode *FindChild(FstNode &dir, const std::string &name)
	{
		const std::string want = ToLower(name);
		for (size_t i = 0; i < dir.children.size(); ++i)
			if (ToLower(dir.children[i].name) == want)
				return &dir.children[i];
		return 0;
	}

	bool FstBuilder::AddOrReplace(const std::string &discPath, u32 size, bool *outWasNew)
	{
		std::vector<std::string> parts;
		SplitPath(discPath, parts);
		if (parts.empty())
			return false;

		FstNode *dir = &root;
		//! Walk (and create) the parent directories.
		for (size_t i = 0; i + 1 < parts.size(); ++i)
		{
			FstNode *child = FindChild(*dir, parts[i]);
			if (child && !child->isDir)
				return false; // a file already occupies a directory slot
			if (!child)
			{
				FstNode d;
				d.name = parts[i];
				d.isDir = true;
				d.modded = true;
				dir->children.push_back(d);
				child = &dir->children.back();
				++stats.addedDirs;
			}
			dir = child;
		}

		const std::string &leaf = parts.back();
		FstNode *file = FindChild(*dir, leaf);
		if (file)
		{
			if (file->isDir)
				return false; // refuse to turn a directory into a file
			file->length = size;
			file->modded = true;
			if (outWasNew)
				*outWasNew = false;
			++stats.replaced;
			return true;
		}

		FstNode f;
		f.name = leaf;
		f.isDir = false;
		f.length = size;
		f.modded = true;
		dir->children.push_back(f);
		if (outWasNew)
			*outWasNew = true;
		++stats.added;
		return true;
	}

	// --------------------------------------------------------------------
	// Layout
	// --------------------------------------------------------------------

	static void WalkExtent(const FstNode &n, u64 *hi)
	{
		for (size_t i = 0; i < n.children.size(); ++i)
		{
			const FstNode &c = n.children[i];
			if (c.isDir)
				WalkExtent(c, hi);
			else if (!c.modded)
			{
				const u64 end = c.offset + c.length;
				if (end > *hi)
					*hi = end;
			}
		}
	}

	u64 FstBuilder::OriginalExtent() const
	{
		u64 hi = 0;
		WalkExtent(root, &hi);
		return hi;
	}

	static void WalkLayout(FstNode &n, u64 *cursor, u32 align, FstBuildStats *st)
	{
		for (size_t i = 0; i < n.children.size(); ++i)
		{
			FstNode &c = n.children[i];
			if (c.isDir)
			{
				WalkLayout(c, cursor, align, st);
				continue;
			}
			if (!c.modded)
			{
				++st->unchanged;
				continue;
			}
			//! Round up to the alignment so every redirected range can start on
			//! a device sector; a fragment cannot begin mid-sector.
			const u64 mask = (u64) align - 1;
			*cursor = (*cursor + mask) & ~mask;
			c.offset = *cursor;
			*cursor += c.length;
			if (*cursor > st->highestOffset)
				st->highestOffset = *cursor;
		}
	}

	void FstBuilder::Layout(u64 regionStart, u32 align)
	{
		if (align == 0)
			align = 1;
		stats.unchanged = 0;
		stats.highestOffset = regionStart;
		u64 cursor = regionStart;
		WalkLayout(root, &cursor, align, &stats);
	}

	// --------------------------------------------------------------------
	// Serialisation: tree -> flat table
	// --------------------------------------------------------------------

	static u32 CountNodes(const FstNode &n)
	{
		u32 total = 0;
		for (size_t i = 0; i < n.children.size(); ++i)
		{
			++total;
			if (n.children[i].isDir)
				total += CountNodes(n.children[i]);
		}
		return total;
	}

	//! Emit `dir`'s children. `self` is the index of the entry for `dir` itself
	//! (0 for the root), needed because a directory entry stores its parent.
	static void Emit(const FstNode &dir, u32 self, std::vector<u8> &entries,
					 std::string &strings, bool shifted)
	{
		for (size_t i = 0; i < dir.children.size(); ++i)
		{
			const FstNode &c = dir.children[i];

			const u32 here = (u32) (entries.size() / 12);
			const u32 nameOff = (u32) strings.size();
			strings += c.name;
			strings += '\0';

			entries.resize(entries.size() + 12);
			u8 *e = &entries[here * 12];
			e[0] = c.isDir ? 1 : 0;
			wr24(e + 1, nameOff);

			if (c.isDir)
			{
				//! Placeholder: the subtree end is only known once its children
				//! have been emitted, so patch it afterwards.
				wr32(e + 4, self);
				wr32(e + 8, 0);
				Emit(c, here, entries, strings, shifted);
				const u32 end = (u32) (entries.size() / 12);
				wr32(&entries[here * 12] + 8, end);
			}
			else
			{
				const u64 off = shifted ? (c.offset >> 2) : c.offset;
				wr32(e + 4, (u32) off);
				wr32(e + 8, c.length);
			}
		}
	}

	void FstBuilder::Serialize(std::vector<u8> &out, bool shifted) const
	{
		std::vector<u8> entries;
		std::string strings;

		const u32 total = CountNodes(root) + 1; // +1 for the root entry itself
		entries.resize(12); // root, filled in below

		//! The root's name is the empty string at string-table offset 0.
		strings += '\0';

		Emit(root, 0, entries, strings, shifted);

		u8 *r = &entries[0];
		r[0] = 1;          // directory
		wr24(r + 1, 0);    // empty name
		wr32(r + 4, 0);    // parent of the root is itself
		wr32(r + 8, total);

		out.clear();
		out.reserve(entries.size() + strings.size());
		out.insert(out.end(), entries.begin(), entries.end());
		out.insert(out.end(), strings.begin(), strings.end());

		FstBuildStats *st = const_cast<FstBuildStats *>(&stats);
		st->entryCount = total;
		st->fstSize = (u32) out.size();
	}

	// --------------------------------------------------------------------
	// Lookup
	// --------------------------------------------------------------------

	static const FstNode *FindNode(const FstNode &dir, const std::vector<std::string> &parts, size_t i)
	{
		const std::string want = ToLower(parts[i]);
		for (size_t k = 0; k < dir.children.size(); ++k)
		{
			const FstNode &c = dir.children[k];
			if (ToLower(c.name) != want)
				continue;
			if (i + 1 == parts.size())
				return c.isDir ? 0 : &c;
			if (!c.isDir)
				return 0;
			return FindNode(c, parts, i + 1);
		}
		return 0;
	}

	bool FstBuilder::FindAssigned(const std::string &discPath, u64 *outOffset, u32 *outLength) const
	{
		std::vector<std::string> parts;
		SplitPath(discPath, parts);
		if (parts.empty())
			return false;
		const FstNode *n = FindNode(root, parts, 0);
		if (!n)
			return false;
		if (outOffset)
			*outOffset = n->offset;
		if (outLength)
			*outLength = n->length;
		return true;
	}
}
