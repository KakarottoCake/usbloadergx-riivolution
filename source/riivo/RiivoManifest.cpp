/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoManifest.hpp for the contract. LE-by-hand like
 * RiivoRedirectTable.cpp; no console dependency so host-testable.
 ***************************************************************************/
#include "RiivoManifest.hpp"

#include <map>

namespace Riivo
{
	static const u32 RIIVO_MANIFEST_MAX_ENTRIES = 65535;

	static void PutLE16(std::vector<u8> &out, u16 v)
	{
		out.push_back((u8) (v & 0xFF));
		out.push_back((u8) ((v >> 8) & 0xFF));
	}

	static void PutLE32(std::vector<u8> &out, u32 v)
	{
		out.push_back((u8) (v & 0xFF));
		out.push_back((u8) ((v >> 8) & 0xFF));
		out.push_back((u8) ((v >> 16) & 0xFF));
		out.push_back((u8) ((v >> 24) & 0xFF));
	}

	static u16 GetLE16(const u8 *p)
	{
		return (u16) p[0] | ((u16) p[1] << 8);
	}

	static u32 GetLE32(const u8 *p)
	{
		return (u32) p[0] | ((u32) p[1] << 8) |
			((u32) p[2] << 16) | ((u32) p[3] << 24);
	}

	static u32 Crc32(const u8 *data, u32 len)
	{
		u32 crc = 0xFFFFFFFFu;
		for (u32 i = 0; i < len; ++i)
		{
			crc ^= data[i];
			for (int k = 0; k < 8; ++k)
				crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
		}
		return crc ^ 0xFFFFFFFFu;
	}

	static u64 BlobLayout(const std::vector<ManifestExtent> &extents,
						  std::vector<u32> *indices)
	{
		std::map<std::string, u32> seen;
		u64 blob = 0;
		if (indices)
			indices->reserve(extents.size());
		for (size_t i = 0; i < extents.size(); ++i)
		{
			if (extents[i].kind != RIIVO_EXT_EXTERNAL)
			{
				if (indices)
					indices->push_back(0);
				continue;
			}
			std::map<std::string, u32>::const_iterator it =
				seen.find(extents[i].path);
			if (it != seen.end())
			{
				if (indices)
					indices->push_back(it->second);
				continue;
			}
			u32 at = (u32) blob;
			seen.insert(std::make_pair(extents[i].path, at));
			if (indices)
				indices->push_back(at);
			blob += extents[i].path.size() + 1;
		}
		return blob;
	}

	u64 ManifestTableSize(const std::vector<ManifestExtent> &extents)
	{
		u64 fixed = (u64) RIIVO_MANIFEST_HEADER +
			(u64) extents.size() * RIIVO_MANIFEST_ENTRY;
		return fixed + BlobLayout(extents, NULL);
	}

	bool BuildManifestV1(const std::vector<ManifestExtent> &extents,
						 u32 partLba, u32 discId, u32 partIdx,
						 u32 caps, u32 provCaps,
						 std::vector<u8> &out, std::string &why)
	{
		why.clear();
		if (extents.empty())
		{
			why = "manifest: no extents to describe";
			return false;
		}
		if (extents.size() > RIIVO_MANIFEST_MAX_ENTRIES)
		{
			why = "manifest: too many extents for one table";
			return false;
		}

		u64 prevEnd = 0;
		for (size_t i = 0; i < extents.size(); ++i)
		{
			const ManifestExtent &e = extents[i];
			if (e.kind != RIIVO_EXT_EXTERNAL &&
				e.kind != RIIVO_EXT_GENERATED &&
				e.kind != RIIVO_EXT_ZERO)
			{
				why = "manifest: unknown extent kind";
				return false;
			}
			if (e.discOffset + e.length < e.discOffset)
			{
				why = "manifest: extent wraps past the end of the partition";
				return false;
			}
			if (i > 0 && e.discOffset < prevEnd)
			{
				why = "manifest: extents out of order or overlapping";
				return false;
			}
			u64 e2 = e.discOffset + e.length;
			if (e2 > prevEnd)
				prevEnd = e2;

			if (e.kind == RIIVO_EXT_EXTERNAL)
			{
				if (e.source != RIIVO_SRC_SD && e.source != RIIVO_SRC_USB &&
					e.source != RIIVO_SRC_RIIFS)
				{
					why = "manifest: external extent has no source";
					return false;
				}
				if (e.path.empty() || e.path[0] != '/')
				{
					why = "manifest: path is not absolute: " + e.path;
					return false;
				}
				if (e.path.find('\0') != std::string::npos)
				{
					why = "manifest: path contains a null byte";
					return false;
				}
			}
			else if (e.kind == RIIVO_EXT_ZERO)
			{
				if (!e.path.empty())
				{
					why = "manifest: zero extent must not name a path";
					return false;
				}
			}
			else // Generated
			{
				if (!e.path.empty())
				{
					why = "manifest: generated extent must not name a path";
					return false;
				}
			}
		}

		u64 total = ManifestTableSize(extents);
		if (total > 0xFFFFFFFFULL)
		{
			why = "manifest: table too large to address";
			return false;
		}

		std::vector<u32> pathIndex;
		BlobLayout(extents, &pathIndex);
		u32 strOff = RIIVO_MANIFEST_HEADER +
			(u32) extents.size() * RIIVO_MANIFEST_ENTRY;

		std::vector<u8> buf;
		buf.reserve((size_t) total);

		PutLE32(buf, RIIVO_MANIFEST_MAGIC);
		PutLE16(buf, RIIVO_MANIFEST_VERSION);
		PutLE16(buf, (u16) RIIVO_MANIFEST_HEADER);
		PutLE32(buf, (u32) total);
		PutLE32(buf, caps);
		PutLE32(buf, 0); // crc patched below
		PutLE32(buf, (u32) extents.size());
		PutLE32(buf, strOff);
		PutLE32(buf, partLba);
		PutLE32(buf, discId);
		PutLE32(buf, partIdx);
		PutLE32(buf, provCaps);
		PutLE32(buf, 0); // reserved

		for (size_t i = 0; i < extents.size(); ++i)
		{
			const ManifestExtent &e = extents[i];
			PutLE32(buf, (u32) (e.discOffset & 0xFFFFFFFFULL));
			PutLE32(buf, (u32) (e.discOffset >> 32));
			PutLE32(buf, e.length);
			PutLE16(buf, e.kind);
			PutLE16(buf, e.source);
			PutLE32(buf, (u32) (e.srcOffset & 0xFFFFFFFFULL));
			PutLE32(buf, (u32) (e.srcOffset >> 32));
			PutLE32(buf, e.kind == RIIVO_EXT_EXTERNAL ? pathIndex[i] : 0);
			PutLE32(buf, e.kind == RIIVO_EXT_GENERATED ? e.genOff : 0);
		}

		std::map<std::string, bool> written;
		for (size_t i = 0; i < extents.size(); ++i)
		{
			if (extents[i].kind != RIIVO_EXT_EXTERNAL)
				continue;
			if (written.find(extents[i].path) != written.end())
				continue;
			written.insert(std::make_pair(extents[i].path, true));
			if (buf.size() - strOff != pathIndex[i])
			{
				why = "manifest: internal string layout mismatch";
				return false;
			}
			for (size_t k = 0; k < extents[i].path.size(); ++k)
				buf.push_back((u8) extents[i].path[k]);
			buf.push_back(0);
		}

		if (buf.size() != total)
		{
			why = "manifest: internal size mismatch";
			return false;
		}

		u32 crc = Crc32(&buf[0], (u32) buf.size());
		buf[16] = (u8) (crc & 0xFF);
		buf[17] = (u8) ((crc >> 8) & 0xFF);
		buf[18] = (u8) ((crc >> 16) & 0xFF);
		buf[19] = (u8) ((crc >> 24) & 0xFF);

		out.swap(buf);
		return true;
	}

	bool ValidateManifestV1(const u8 *base, u32 len, std::string &why)
	{
		why.clear();
		if (!base || len < RIIVO_MANIFEST_HEADER)
		{
			why = "manifest: too short for a header";
			return false;
		}
		if (GetLE32(base + 0) != RIIVO_MANIFEST_MAGIC)
		{
			why = "manifest: bad magic";
			return false;
		}
		if (GetLE16(base + 4) != RIIVO_MANIFEST_VERSION)
		{
			why = "manifest: unsupported version";
			return false;
		}
		if (GetLE16(base + 6) != RIIVO_MANIFEST_HEADER)
		{
			why = "manifest: bad header length";
			return false;
		}
		u32 total = GetLE32(base + 8);
		if (total != len || total < RIIVO_MANIFEST_HEADER)
		{
			why = "manifest: bad total size";
			return false;
		}
		u32 count = GetLE32(base + 20);
		u32 strOff = GetLE32(base + 24);
		if (strOff < RIIVO_MANIFEST_HEADER + count * RIIVO_MANIFEST_ENTRY ||
			strOff > len)
		{
			why = "manifest: bad string blob offset";
			return false;
		}
		if (GetLE32(base + 44) != 0)
		{
			why = "manifest: reserved field is not zero";
			return false;
		}

		// CRC over the table with the crc field zeroed.
		u32 want = GetLE32(base + 16);
		u32 got = want;
		{
			// Copy-free: CRC the two spans around the field.
			u32 crc = 0xFFFFFFFFu;
			for (u32 i = 0; i < len; ++i)
			{
				u8 b = (i >= 16 && i < 20) ? 0 : base[i];
				crc ^= b;
				for (int k = 0; k < 8; ++k)
					crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
			}
			got = crc ^ 0xFFFFFFFFu;
		}
		if (got != want)
		{
			why = "manifest: crc mismatch";
			return false;
		}

		u64 prevEnd = 0;
		for (u32 i = 0; i < count; ++i)
		{
			const u8 *e = base + RIIVO_MANIFEST_HEADER + i * RIIVO_MANIFEST_ENTRY;
			u64 off = (u64) GetLE32(e + 0) | ((u64) GetLE32(e + 4) << 32);
			u32 elen = GetLE32(e + 8);
			u16 kind = GetLE16(e + 12);
			u32 pathOff = GetLE32(e + 24);
			if (kind != RIIVO_EXT_EXTERNAL && kind != RIIVO_EXT_GENERATED &&
				kind != RIIVO_EXT_ZERO)
			{
				why = "manifest: unknown extent kind";
				return false;
			}
			if (elen > 0 && off + elen < off)
			{
				why = "manifest: extent wraps";
				return false;
			}
			if (i > 0 && off < prevEnd)
			{
				why = "manifest: extents out of order or overlapping";
				return false;
			}
			u64 e2 = off + elen;
			if (e2 > prevEnd)
				prevEnd = e2;
			if (kind == RIIVO_EXT_EXTERNAL)
			{
				u32 at = strOff + pathOff;
				if (at >= len)
				{
					why = "manifest: path offset out of range";
					return false;
				}
				bool term = false;
				for (u32 j = at; j < len; ++j)
				{
					if (base[j] == 0)
					{
						term = true;
						break;
					}
				}
				if (!term)
				{
					why = "manifest: path is not terminated";
					return false;
				}
				if (base[at] != '/')
				{
					why = "manifest: path is not absolute";
					return false;
				}
			}
		}
		return true;
	}
}
