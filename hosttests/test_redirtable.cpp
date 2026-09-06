/****************************************************************************
 * The redirect table, as written by the loader and as read by IOS.
 *
 * The table is the only thing the two halves of the on-demand design share,
 * and they share it across a byte-order boundary they must not depend on: a
 * big-endian PowerPC writes it, a big-endian ARM reads it, and the format is
 * little-endian on purpose so that neither can get away with a struct copy.
 * A mistake here is invisible on both sides individually - the builder emits
 * something that looks fine, the module rejects it as a number on a black
 * screen - so the decoder below is written from the format description in
 * RiivoRedirectTable.hpp rather than by calling the builder's own helpers.
 * If the builder and this file agree, the format is being written twice and
 * read the same way.
 *
 * The C reader in source/riivo/ios is checked against this same format by the
 * cross-test, which feeds a table built here straight into it.
 ***************************************************************************/
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "riivo/RiivoRedirectTable.hpp"

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

//! An independent little-endian decoder, from the spec.
static u32 Get32(const std::vector<u8> &t, size_t at)
{
	return (u32) t[at] | ((u32) t[at + 1] << 8) | ((u32) t[at + 2] << 16)
		 | ((u32) t[at + 3] << 24);
}

static u64 Get64(const std::vector<u8> &t, size_t at)
{
	return (u64) Get32(t, at) | ((u64) Get32(t, at + 4) << 32);
}

//! Decode an entry's path the way the module does: index is relative to the
//! string blob, and the string runs to the first NUL.
static std::string PathOf(const std::vector<u8> &t, u32 i)
{
	u32 strOff = Get32(t, 8);
	u32 idx = Get32(t, Riivo::RIIVO_TABLE_HEADER + i * Riivo::RIIVO_TABLE_ENTRY + 12);
	size_t at = strOff + idx;
	std::string s;
	while (at < t.size() && t[at])
		s.push_back((char) t[at++]);
	return s;
}

static Riivo::RedirectEntry E(u64 off, u32 len, const char *path)
{
	return Riivo::RedirectEntry(off, len, path);
}

//! ------------------------------------------------------------------
//! Round-trip: what goes in comes back out, decoded independently.
//! ------------------------------------------------------------------
static void TestRoundTrip()
{
	std::vector<Riivo::RedirectEntry> in;
	in.push_back(E(0x180000000ULL, 4096, "/riivo/mod/files/a.arc"));
	in.push_back(E(0x180008000ULL, 32768, "/riivo/mod/files/b.arc"));
	in.push_back(E(0x180010000ULL, 1, "/riivo/mod/files/deep/c.brres"));

	std::vector<u8> t;
	std::string why;
	check(Riivo::BuildRedirectTable(in, Riivo::RIIVO_PART_DISCOVER, t, why),
		  "three entries build");
	check(why.empty(), "no reason given on success");

	check(Get32(t, 0) == Riivo::RIIVO_TABLE_MAGIC, "magic is RIIV");
	check(t[0] == 'R' && t[1] == 'I' && t[2] == 'I' && t[3] == 'V',
		  "magic reads as RIIV byte for byte, not byte-swapped");
	check(Get32(t, 4) == 3, "count");
	check(Get32(t, 12) == Riivo::RIIVO_PART_DISCOVER, "partition LBA passed through");

	u32 strOff = Get32(t, 8);
	check(strOff == Riivo::RIIVO_TABLE_HEADER + 3 * Riivo::RIIVO_TABLE_ENTRY,
		  "string blob begins right after the entries");
	check(strOff < t.size(), "string blob is inside the table");

	for (u32 i = 0; i < 3; ++i)
	{
		size_t at = Riivo::RIIVO_TABLE_HEADER + i * Riivo::RIIVO_TABLE_ENTRY;
		check(Get64(t, at) == in[i].discOffset, "entry offset round-trips");
		check(Get32(t, at + 8) == in[i].length, "entry length round-trips");
		check(PathOf(t, i) == in[i].path, "entry path round-trips");
	}

	//! The high word must actually carry the top bits: a 6 GiB offset is the
	//! whole point, and a 32-bit truncation here would place every file at a
	//! plausible-looking low offset instead of failing loudly.
	check(Get32(t, Riivo::RIIVO_TABLE_HEADER + 0) == 0x80000000u,
		  "low word of 0x180000000");
	check(Get32(t, Riivo::RIIVO_TABLE_HEADER + 4) == 1u,
		  "high word of 0x180000000");

	check(t.size() == Riivo::RedirectTableSize(in),
		  "RedirectTableSize agrees with what was built");
}

//! ------------------------------------------------------------------
//! Byte order, stated as an absolute expectation rather than a
//! round-trip - a builder and decoder that are both byte-swapped would
//! round-trip perfectly and fail on the console.
//! ------------------------------------------------------------------
static void TestGoldenBytes()
{
	std::vector<Riivo::RedirectEntry> in;
	in.push_back(E(0x0102030405060708ULL, 0x0A0B0C0D, "/x"));

	std::vector<u8> t;
	std::string why;
	check(Riivo::BuildRedirectTable(in, 0x11223344, t, why), "golden builds");

	static const u8 want[] = {
		0x52, 0x49, 0x49, 0x56,             // 'RIIV'
		0x01, 0x00, 0x00, 0x00,             // count = 1
		0x20, 0x00, 0x00, 0x00,             // strings at 32
		0x44, 0x33, 0x22, 0x11,             // partition LBA, little-endian
		0x08, 0x07, 0x06, 0x05,             // offset low word
		0x04, 0x03, 0x02, 0x01,             // offset high word
		0x0D, 0x0C, 0x0B, 0x0A,             // length
		0x00, 0x00, 0x00, 0x00,             // path index
		'/', 'x', 0x00
	};
	check(t.size() == sizeof(want), "golden table is the expected length");
	if (t.size() == sizeof(want))
		check(std::memcmp(&t[0], want, sizeof(want)) == 0,
			  "golden table matches byte for byte");
}

//! ------------------------------------------------------------------
//! Repeated paths are stored once. A <folder> patch routinely maps one
//! external file at several disc paths, and the blob is what scales
//! worst on a big mod.
//! ------------------------------------------------------------------
static void TestDedup()
{
	const char *shared = "/riivo/mod/files/shared.arc";
	std::vector<Riivo::RedirectEntry> in;
	in.push_back(E(0x00000000ULL, 16, shared));
	in.push_back(E(0x00008000ULL, 16, "/riivo/mod/files/other.arc"));
	in.push_back(E(0x00010000ULL, 16, shared));

	std::vector<u8> t;
	std::string why;
	check(Riivo::BuildRedirectTable(in, 0, t, why), "dedup case builds");

	size_t e0 = Riivo::RIIVO_TABLE_HEADER;
	size_t e2 = Riivo::RIIVO_TABLE_HEADER + 2 * Riivo::RIIVO_TABLE_ENTRY;
	check(Get32(t, e0 + 12) == Get32(t, e2 + 12),
		  "the two entries naming one file share a string index");
	check(PathOf(t, 0) == shared && PathOf(t, 2) == shared,
		  "both still decode to that file");
	check(PathOf(t, 1) == "/riivo/mod/files/other.arc",
		  "the distinct path is unaffected");

	u64 blob = t.size() - Get32(t, 8);
	check(blob == std::strlen(shared) + 1
			   + std::strlen("/riivo/mod/files/other.arc") + 1,
		  "the shared path occupies the blob once");
}

//! ------------------------------------------------------------------
//! What it refuses. Each of these would be caught by the module too,
//! but there it is a negative number on a black screen.
//! ------------------------------------------------------------------
static void TestRefusals()
{
	std::vector<u8> t;
	std::string why;

	{
		std::vector<Riivo::RedirectEntry> in;
		check(!Riivo::BuildRedirectTable(in, 0, t, why), "refuses an empty set");
		check(!why.empty(), "and says why");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0x8000, 16, "/b.arc"));
		in.push_back(E(0x0000, 16, "/a.arc"));
		check(!Riivo::BuildRedirectTable(in, 0, t, why), "refuses unsorted");
		check(why.find("/a.arc") != std::string::npos,
			  "names the entry that broke the order");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0x0000, 0x9000, "/a.arc"));   // runs into the next
		in.push_back(E(0x8000, 16, "/b.arc"));
		check(!Riivo::BuildRedirectTable(in, 0, t, why), "refuses overlap");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0, 16, "riivo/mod/a.arc"));
		check(!Riivo::BuildRedirectTable(in, 0, t, why),
			  "refuses a relative path");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0, 16, ""));
		check(!Riivo::BuildRedirectTable(in, 0, t, why), "refuses an empty path");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		std::string bad = "/a";
		bad.push_back('\0');
		bad += "b.arc";
		in.push_back(Riivo::RedirectEntry(0, 16, bad));
		check(!Riivo::BuildRedirectTable(in, 0, t, why),
			  "refuses a path with an embedded null");
	}
	{
		//! A file whose end wraps past 2^64. The module checks this because
		//! its search would then find the entry for every offset.
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0xFFFFFFFFFFFFFFF0ULL, 0x20, "/a.arc"));
		check(!Riivo::BuildRedirectTable(in, 0, t, why), "refuses a wrapping extent");
	}

	//! Touching extents are legal - Layout() pads to 32 KB so this should not
	//! arise, but a zero-length file or an exactly-aligned one can produce it
	//! and it is not an error.
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0x0000, 0x8000, "/a.arc"));
		in.push_back(E(0x8000, 0x8000, "/b.arc"));
		check(Riivo::BuildRedirectTable(in, 0, t, why), "accepts touching extents");
	}
	{
		std::vector<Riivo::RedirectEntry> in;
		in.push_back(E(0x0000, 0, "/empty.bin"));
		in.push_back(E(0x0000, 16, "/a.arc"));
		check(Riivo::BuildRedirectTable(in, 0, t, why),
			  "accepts a zero-length file sharing an offset with the next");
	}
	{
		//! On refusal the caller's buffer must be left alone, not half-written.
		std::vector<Riivo::RedirectEntry> good;
		good.push_back(E(0, 16, "/a.arc"));
		std::vector<u8> keep;
		check(Riivo::BuildRedirectTable(good, 0, keep, why), "build a good table");
		std::vector<u8> before = keep;

		std::vector<Riivo::RedirectEntry> bad;
		check(!Riivo::BuildRedirectTable(bad, 0, keep, why), "then refuse into it");
		check(keep == before, "the previous table survives a refusal intact");
	}
}

//! ------------------------------------------------------------------
//! A mod the size of the one that started all this: 2802 files.
//! ------------------------------------------------------------------
static void TestAtScale()
{
	const u32 kFiles = 2802;
	std::vector<Riivo::RedirectEntry> in;
	in.reserve(kFiles);

	u64 off = 0x180000000ULL;
	u64 payload = 0;
	for (u32 i = 0; i < kFiles; ++i)
	{
		char path[96];
		std::snprintf(path, sizeof(path), "/riivolution/starshine/files/d%03u/f%04u.arc",
					  i % 200, i);
		u32 len = 1024 + (i * 7919) % 200000;
		in.push_back(E(off, len, path));
		payload += len;
		//! What Layout() does: round up to the 32 KB boundary.
		u64 end = off + len;
		off = (end + Riivo::RIIVO_MOD_ALIGN - 1)
			  & ~(u64) (Riivo::RIIVO_MOD_ALIGN - 1);
	}

	std::vector<u8> t;
	std::string why;
	check(Riivo::BuildRedirectTable(in, Riivo::RIIVO_PART_DISCOVER, t, why),
		  "2802 files build");
	check(Get32(t, 4) == kFiles, "all of them are in the table");

	//! Spot-check the ends and a few in the middle rather than all 2802.
	bool allGood = true;
	for (u32 i = 0; i < kFiles; ++i)
	{
		size_t at = Riivo::RIIVO_TABLE_HEADER + i * Riivo::RIIVO_TABLE_ENTRY;
		if (Get64(t, at) != in[i].discOffset || Get32(t, at + 8) != in[i].length
			|| PathOf(t, i) != in[i].path)
		{
			allGood = false;
			break;
		}
	}
	check(allGood, "every one of the 2802 entries round-trips");

	//! The size that matters: this is what has to be handed to IOS, and IOS
	//! heap is measured in tens of kilobytes. Reading it in place out of MEM2
	//! rather than copying it is not an optimisation, it is the only option.
	std::printf("  2802 files: table %u bytes (%u entries + %u strings), "
				"payload %.1f MB\n",
				(unsigned) t.size(),
				(unsigned) (kFiles * Riivo::RIIVO_TABLE_ENTRY),
				(unsigned) (t.size() - Get32(t, 8)),
				(double) payload / (1024.0 * 1024.0));
	check(t.size() < 512 * 1024, "the table fits in the 512 KB MEM2 reservation");
}

//! ------------------------------------------------------------------
//! The count the module refuses at, checked from this side.
//! ------------------------------------------------------------------
static void TestCountLimit()
{
	std::vector<Riivo::RedirectEntry> in;
	in.reserve(65536);
	for (u32 i = 0; i < 65536; ++i)
		in.push_back(E((u64) i * 0x8000ULL, 16, "/a.arc"));

	std::vector<u8> t;
	std::string why;
	check(!Riivo::BuildRedirectTable(in, 0, t, why),
		  "refuses 65536 files, one past what the module accepts");

	in.pop_back();
	check(Riivo::BuildRedirectTable(in, 0, t, why), "accepts 65535");
	check(Get32(t, 4) == 65535, "and records the count");
}

int main()
{
	TestRoundTrip();
	TestGoldenBytes();
	TestDedup();
	TestRefusals();
	TestAtScale();
	TestCountLimit();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
