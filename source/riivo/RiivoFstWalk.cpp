/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <ctype.h>
#include <sstream>
#include "RiivoFstWalk.hpp"

namespace Riivo
{
	static u32 be32(const u8 *p)
	{
		return ((u32) p[0] << 24) | ((u32) p[1] << 16) |
			   ((u32) p[2] << 8) | (u32) p[3];
	}

	static u32 be24(const u8 *p)
	{
		return ((u32) p[0] << 16) | ((u32) p[1] << 8) | (u32) p[2];
	}

	static std::string FoldName(const char *name, size_t length)
	{
		std::string folded;
		folded.reserve(length);
		for (size_t i = 0; i < length; ++i)
			folded += (char) tolower((unsigned char) name[i]);
		return folded;
	}

	static void SetError(std::string *outError, const char *what, u32 entry)
	{
		if (!outError)
			return;
		std::ostringstream text;
		text << "FST entry " << entry << ": " << what;
		*outError = text.str();
	}

	bool FstWalk::Open(const u8 *fstData, u32 fstSize, bool shifted,
					   std::string *outError)
	{
		entries.clear();
		if (outError)
			outError->clear();
		if (!fstData || fstSize < 12)
		{
			SetError(outError, "image is too small", 0);
			return false;
		}

		const u32 count = be32(fstData + 8);
		const u64 entryBytes = (u64) count * 12;
		if (count == 0 || entryBytes > fstSize || entryBytes == fstSize)
		{
			SetError(outError, "invalid entry count or missing string table", 0);
			return false;
		}

		const u8 *root = fstData;
		if (root[0] != 1 || be24(root + 1) != 0 ||
			be32(root + 4) != 0 || be32(root + 8) != count)
		{
			SetError(outError, "root is not a self-parented directory", 0);
			return false;
		}

		entries.resize(count);
		const u32 stringBase = (u32) entryBytes;
		std::vector<u32> stack;
		stack.push_back(0);

		for (u32 i = 0; i < count; ++i)
		{
			const u8 *raw = fstData + (u64) i * 12;
			const u8 type = raw[0];
			if (type != 0 && type != 1)
			{
				SetError(outError, "unknown entry type", i);
				entries.clear();
				return false;
			}

			const u32 nameOffset = be24(raw + 1);
			const u64 nameAt = (u64) stringBase + nameOffset;
			if (nameAt >= fstSize)
			{
				SetError(outError, "name offset is outside the string table", i);
				entries.clear();
				return false;
			}
			const char *name = (const char *) fstData + nameAt;
			size_t nameLength = 0;
			while (nameAt + nameLength < fstSize && name[nameLength] != '\0')
				++nameLength;
			if (nameAt + nameLength == fstSize)
			{
				SetError(outError, "name is not NUL terminated", i);
				entries.clear();
				return false;
			}
			if ((i == 0 && nameLength != 0) || (i != 0 && nameLength == 0))
			{
				SetError(outError, "root/name is empty in the wrong place", i);
				entries.clear();
				return false;
			}
			if (i != 0 && ((nameLength == 1 && name[0] == '.') ||
						   (nameLength == 2 && name[0] == '.' && name[1] == '.')))
			{
				SetError(outError, "name is reserved for path traversal", i);
				entries.clear();
				return false;
			}
			for (size_t n = 0; n < nameLength; ++n)
				if (name[n] == '/')
				{
					SetError(outError, "name contains a path separator", i);
					entries.clear();
					return false;
				}

			Entry &entry = entries[i];
			entry.isDir = type == 1;
			entry.foldedName = FoldName(name, nameLength);
			entry.parent = 0;
			entry.next = i + 1;
			entry.offset = 0;
			entry.length = 0;

			if (i == 0)
			{
				entry.parent = 0;
				entry.next = count;
				continue;
			}

			while (stack.size() > 1 && entries[stack.back()].next <= i)
				stack.pop_back();
			if (stack.empty() || i >= entries[stack.back()].next)
			{
				SetError(outError, "entry is outside its parent subtree", i);
				entries.clear();
				return false;
			}

			const u32 parent = stack.back();
			std::map<std::string, u32> &siblings = entries[parent].children;
			if (siblings.find(entry.foldedName) != siblings.end())
			{
				SetError(outError, "case-insensitive duplicate sibling", i);
				entries.clear();
				return false;
			}
			siblings[entry.foldedName] = i;
			entry.parent = parent;

			if (entry.isDir)
			{
				const u32 declaredParent = be32(raw + 4);
				const u32 next = be32(raw + 8);
				if (declaredParent != parent)
				{
					SetError(outError, "directory parent does not match tree", i);
					entries.clear();
					return false;
				}
				if (next <= i || next > entries[parent].next)
				{
					SetError(outError, "directory subtree end is invalid", i);
					entries.clear();
					return false;
				}
				entry.next = next;
				stack.push_back(i);
			}
			else
			{
				const u32 storedOffset = be32(raw + 4);
				entry.offset = shifted ? ((u64) storedOffset << 2) : (u64) storedOffset;
				entry.length = be32(raw + 8);
			}
		}
		return true;
	}

	bool FstWalk::Lookup(const std::string &path, FstWalkFile *out) const
	{
		if (entries.empty())
			return false;
		u32 directory = 0;
		size_t at = 0;
		while (true)
		{
			if (at == path.size())
				return false; // a path resolving to a directory is not a file result
			if (path[at] == '/')
			{
				directory = 0;
				++at;
				continue;
			}
			if (path[at] == '.')
			{
				if (at + 1 == path.size())
					return false;
				if (at + 1 < path.size() && path[at + 1] == '/')
				{
					at += 2;
					continue;
				}
				if (at + 2 == path.size() && path[at + 1] == '.')
					return false;
				if (at + 2 < path.size() && path[at + 1] == '.' && path[at + 2] == '/')
				{
					directory = entries[directory].parent;
					at += 3;
					continue;
				}
			}

			size_t end = at;
			while (end < path.size() && path[end] != '/')
				++end;
			const bool wantDirectory = end < path.size();
			const std::string component = FoldName(path.data() + at, end - at);
			const Entry &parent = entries[directory];
			u32 found = parent.next;
			// This is the SDK's directory walk: only direct children are compared;
			// a child directory advances straight to the entry after its subtree.
			for (u32 child = directory + 1; child < parent.next; )
			{
				if (entries[child].foldedName == component)
				{
					found = child;
					break;
				}
				child = entries[child].isDir ? entries[child].next : child + 1;
			}
			if (found == parent.next)
				return false;

			const Entry &entry = entries[found];
			if (wantDirectory)
			{
				if (!entry.isDir)
					return false;
				directory = found;
				at = end + 1;
				continue;
			}
			if (entry.isDir)
				return false;
			if (out)
			{
				out->offset = entry.offset;
				out->length = entry.length;
				out->entry = found;
			}
			return true;
		}
	}

	bool FstWalk::Check(const std::vector<FstWalkExpectation> &expected,
						std::string *outError) const
	{
		if (outError)
			outError->clear();
		if (entries.empty())
		{
			if (outError)
				*outError = "FST walker has not validated an image";
			return false;
		}
		for (size_t i = 0; i < expected.size(); ++i)
		{
			FstWalkFile actual;
			if (!Lookup(expected[i].path, &actual))
			{
				if (outError)
					*outError = "FST path did not resolve: " + expected[i].path;
				return false;
			}
			if (actual.offset != expected[i].offset || actual.length != expected[i].length)
			{
				if (outError)
					*outError = "FST path disagrees with placement: " + expected[i].path;
				return false;
			}
		}
		return true;
	}
}
