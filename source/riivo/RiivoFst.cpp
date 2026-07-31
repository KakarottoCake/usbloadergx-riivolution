/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <ctype.h>
#include "RiivoFst.hpp"
#include "gecko.h"

namespace Riivo
{
	// Big-endian readers (the FST is big-endian on disc; read explicitly so the
	// parser is correct on both the PPC target and a little-endian host test).
	static u32 be32(const u8 *p) { return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3]; }
	static u32 be24(const u8 *p) { return ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2]; }

	static std::string ToLower(const std::string &s)
	{
		std::string r = s;
		for (size_t i = 0; i < r.size(); ++i)
			r[i] = (char) tolower((unsigned char) r[i]);
		return r;
	}

	//! Read a NUL-terminated name from the string table, bounded by fstSize.
	static std::string ReadName(const u8 *fst, u32 fstSize, u32 strOff)
	{
		std::string name;
		for (u32 i = strOff; i < fstSize && fst[i]; ++i)
			name += (char) fst[i];
		return name;
	}

	bool Fst::Parse(const u8 *fstData, u32 fstSize, bool shifted)
	{
		files.clear();
		if (!fstData || fstSize < 12)
			return false;

		const u32 count = be32(fstData + 8); // root entry length = total entry count
		if (count == 0 || (u64) count * 12 > fstSize)
			return false;

		const u32 strTable = count * 12;

		// Directory stack: (endIndex, lowercased path-prefix). Root spans all entries.
		struct Dir { u32 end; std::string path; };
		std::vector<Dir> stack;
		Dir root;
		root.end = count;
		root.path = "";
		stack.push_back(root);

		for (u32 i = 1; i < count; ++i)
		{
			while (stack.size() > 1 && stack.back().end <= i)
				stack.pop_back();

			const u8 *e = fstData + i * 12;
			const u8 type = e[0];
			const u32 nameOff = be24(e + 1);
			const std::string name = ToLower(ReadName(fstData, fstSize, strTable + nameOff));
			const std::string full = stack.back().path + "/" + name;

			if (type == 1) // directory
			{
				u32 endIdx = be32(e + 8);
				if (endIdx <= i || endIdx > count) // guard against corrupt trees
					endIdx = count;
				Dir d;
				d.end = endIdx;
				d.path = full;
				stack.push_back(d);
			}
			else // file
			{
				FstFile f;
				f.path = full;
				u32 off = be32(e + 4);
				f.offset = shifted ? (off << 2) : off;
				f.length = be32(e + 8);
				f.index = i;
				files.push_back(f);
			}
		}
		return true;
	}

	const FstFile *Fst::FindFile(const std::string &discPath) const
	{
		if (discPath.empty())
			return NULL;
		const std::string q = ToLower(discPath);

		if (q[0] == '/')
		{
			// Full-path match.
			for (size_t i = 0; i < files.size(); ++i)
				if (files[i].path == q)
					return &files[i];
			return NULL;
		}

		// Bare filename: match against basenames.
		for (size_t i = 0; i < files.size(); ++i)
		{
			const std::string &p = files[i].path;
			size_t slash = p.find_last_of('/');
			const std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
			if (base == q)
				return &files[i];
		}
		return NULL;
	}

	void Fst::ListFolder(const std::string &dirPath, bool recursive,
						 std::vector<const FstFile *> &out) const
	{
		std::string prefix = ToLower(dirPath);
		if (prefix.empty() || prefix[0] != '/')
			prefix = "/" + prefix;
		if (prefix.size() > 1 && prefix[prefix.size() - 1] == '/')
			prefix.erase(prefix.size() - 1);
		const std::string base = (prefix == "/") ? std::string("/") : prefix + "/";

		for (size_t i = 0; i < files.size(); ++i)
		{
			const std::string &p = files[i].path;
			if (p.size() <= base.size() || p.compare(0, base.size(), base) != 0)
				continue;
			if (!recursive)
			{
				// Direct child only: no further '/' after the prefix.
				if (p.find('/', base.size()) != std::string::npos)
					continue;
			}
			out.push_back(&files[i]);
		}
	}
}
