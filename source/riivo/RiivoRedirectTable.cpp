/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoRedirectTable.hpp for what this is and why the byte order is
 * written out by hand.
 ***************************************************************************/
#include "RiivoRedirectTable.hpp"

#include <map>

namespace Riivo
{
	//! The module refuses a count outside this, so refuse it here too and say
	//! why, rather than shipping a table that fails on the console.
	static const u32 RIIVO_TABLE_MAX_ENTRIES = 65535;

	//! Little-endian by hand. The console is big-endian, so a cast-and-store
	//! would emit the wrong order and the module would read a byte-swapped
	//! table - which passes no check cleanly and would look like corruption.
	static void PutLE32(std::vector<u8> &out, u32 v)
	{
		out.push_back((u8) (v & 0xFF));
		out.push_back((u8) ((v >> 8) & 0xFF));
		out.push_back((u8) ((v >> 16) & 0xFF));
		out.push_back((u8) ((v >> 24) & 0xFF));
	}

	//! Same, at a known position, for fields whose value is only known once
	//! the rest of the table has been sized.
	static void PatchLE32(std::vector<u8> &out, size_t at, u32 v)
	{
		out[at + 0] = (u8) (v & 0xFF);
		out[at + 1] = (u8) ((v >> 8) & 0xFF);
		out[at + 2] = (u8) ((v >> 16) & 0xFF);
		out[at + 3] = (u8) ((v >> 24) & 0xFF);
	}

	//! Shared by the sizer and the builder so the two can never disagree
	//! about how big the string blob is. Identical paths are stored once;
	//! a <folder> patch commonly maps the same external file at more than one
	//! disc path, and the blob is the part that scales worst.
	static u64 BlobLayout(const std::vector<RedirectEntry> &entries,
						  std::vector<u32> *indices)
	{
		std::map<std::string, u32> seen;
		u64 blob = 0;

		if (indices)
			indices->reserve(entries.size());

		for (size_t i = 0; i < entries.size(); ++i)
		{
			std::map<std::string, u32>::const_iterator it =
				seen.find(entries[i].path);
			if (it != seen.end())
			{
				if (indices)
					indices->push_back(it->second);
				continue;
			}
			//! Truncation is impossible here: the caller has already been
			//! refused if the table would exceed a u32 in total.
			u32 at = (u32) blob;
			seen.insert(std::make_pair(entries[i].path, at));
			if (indices)
				indices->push_back(at);
			blob += entries[i].path.size() + 1;
		}
		return blob;
	}

	u64 RedirectTableSize(const std::vector<RedirectEntry> &entries)
	{
		u64 fixed = (u64) RIIVO_TABLE_HEADER
				  + (u64) entries.size() * RIIVO_TABLE_ENTRY;
		return fixed + BlobLayout(entries, NULL);
	}

	bool BuildRedirectTable(const std::vector<RedirectEntry> &entries,
							u32 partLba, std::vector<u8> &out,
							std::string &why)
	{
		why.clear();

		if (entries.empty())
		{
			why = "redirect table: no files to place";
			return false;
		}
		if (entries.size() > RIIVO_TABLE_MAX_ENTRIES)
		{
			why = "redirect table: too many files for one table";
			return false;
		}

		//! Every rule below is one the module also enforces at init. It has to
		//! - it cannot assume the memory it was handed came from us - but a
		//! table that fails there fails as a number on a black screen, and one
		//! that fails here fails with a sentence.
		u64 prevEnd = 0;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const RedirectEntry &e = entries[i];

			if (e.path.empty() || e.path[0] != '/')
			{
				why = "redirect table: path is not absolute: " + e.path;
				return false;
			}
			if (e.path.find('\0') != std::string::npos)
			{
				why = "redirect table: path contains a null byte";
				return false;
			}
			if (e.discOffset + e.length < e.discOffset)
			{
				why = "redirect table: file wraps past the end of the disc: "
					+ e.path;
				return false;
			}
			if (i > 0 && e.discOffset < prevEnd)
			{
				why = "redirect table: files out of order or overlapping at "
					+ e.path;
				return false;
			}
			prevEnd = e.discOffset + e.length;
		}

		u64 total = RedirectTableSize(entries);
		if (total > 0xFFFFFFFFULL)
		{
			why = "redirect table: table too large to address";
			return false;
		}

		std::vector<u32> pathIndex;
		BlobLayout(entries, &pathIndex);

		u32 strOff = RIIVO_TABLE_HEADER
				   + (u32) entries.size() * RIIVO_TABLE_ENTRY;

		std::vector<u8> buf;
		buf.reserve((size_t) total);

		PutLE32(buf, RIIVO_TABLE_MAGIC);
		PutLE32(buf, (u32) entries.size());
		PutLE32(buf, strOff);
		PutLE32(buf, partLba);

		for (size_t i = 0; i < entries.size(); ++i)
		{
			PutLE32(buf, (u32) (entries[i].discOffset & 0xFFFFFFFFULL));
			PutLE32(buf, (u32) (entries[i].discOffset >> 32));
			PutLE32(buf, entries[i].length);
			PutLE32(buf, pathIndex[i]);   // patched below once the blob is laid out
		}

		//! Emit the blob in first-use order, which is the order BlobLayout
		//! assigned the indices in.
		std::map<std::string, bool> written;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (written.find(entries[i].path) != written.end())
				continue;
			written.insert(std::make_pair(entries[i].path, true));

			//! Position must match the index BlobLayout handed out, or an
			//! entry names the wrong file. Cheap to assert, and silent
			//! corruption is the alternative.
			if (buf.size() - strOff != pathIndex[i])
			{
				why = "redirect table: internal string layout mismatch";
				return false;
			}
			for (size_t k = 0; k < entries[i].path.size(); ++k)
				buf.push_back((u8) entries[i].path[k]);
			buf.push_back(0);
		}

		if (buf.size() != total)
		{
			why = "redirect table: internal size mismatch";
			return false;
		}

		//! strOff must land inside the table and past the entries, which the
		//! module checks; with a non-empty blob it always does, but the check
		//! costs nothing and documents the invariant.
		if (strOff >= buf.size())
		{
			why = "redirect table: string blob is empty";
			return false;
		}
		PatchLE32(buf, 8, strOff);

		out.swap(buf);
		return true;
	}
}
