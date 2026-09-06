/****************************************************************************
 * The v1 runtime manifest ('RIV1'), as written by the loader.
 *
 * Gate B contract: little-endian, fixed-width, versioned. The decoder below
 * is written from the format description in RiivoManifest.hpp rather than by
 * calling the builder's helpers, so builder and test cannot drift together.
 * Corruption cases prove the integrity check fires before any extent is
 * trusted; legacy 'RIIV' tables are refused as a different magic, never
 * misread as v1.
 ***************************************************************************/
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "riivo/RiivoManifest.hpp"
#include "riivo/RiivoAddr.hpp"

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

static u16 Get16(const std::vector<u8> &t, size_t at)
{
	return (u16) t[at] | ((u16) t[at + 1] << 8);
}

static u32 Get32(const std::vector<u8> &t, size_t at)
{
	return (u32) t[at] | ((u32) t[at + 1] << 8) | ((u32) t[at + 2] << 16)
		 | ((u32) t[at + 3] << 24);
}

static u64 Get64(const std::vector<u8> &t, size_t at)
{
	return (u64) Get32(t, at) | ((u64) Get32(t, at + 4) << 32);
}

static Riivo::ManifestExtent Ext(u64 off, u32 len, u16 kind, u16 src,
								 u64 srcOff, const char *path, u32 genOff)
{
	Riivo::ManifestExtent e;
	e.discOffset = off;
	e.length = len;
	e.kind = kind;
	e.source = src;
	e.srcOffset = srcOff;
	if (path)
		e.path = path;
	e.genOff = genOff;
	return e;
}

static std::vector<Riivo::ManifestExtent> SampleExtents()
{
	std::vector<Riivo::ManifestExtent> v;
	v.push_back(Ext(0x180000000ULL, 0x8000, Riivo::RIIVO_EXT_EXTERNAL,
					Riivo::RIIVO_SRC_SD, 0x100, "/mod/a.bin", 0));
	v.push_back(Ext(0x180008000ULL, 0x100, Riivo::RIIVO_EXT_ZERO,
					Riivo::RIIVO_SRC_NONE, 0, NULL, 0));
	v.push_back(Ext(0x180008100ULL, 0x200, Riivo::RIIVO_EXT_GENERATED,
					Riivo::RIIVO_SRC_NONE, 0, NULL, 0x1000));
	return v;
}

static void TestRoundTrip()
{
	std::vector<Riivo::ManifestExtent> in = SampleExtents();
	std::vector<u8> t;
	std::string why;
	u32 caps = Riivo::RIIVO_CAP_SPLIT_READ | Riivo::RIIVO_CAP_ZERO_FILL
			 | Riivo::RIIVO_CAP_GENERATED;
	check(Riivo::BuildManifestV1(in, Riivo::RIIVO_MANIFEST_DISCOVER, 0x53424E59u,
								 0, caps, Riivo::RIIVO_PROV_BOUNDED, t, why),
		  "builds the sample manifest");
	if (t.empty())
		return;

	// Independent decode from the header layout.
	check(Get32(t, 0) == 0x31564952u, "magic is 'RIV1'");
	check(Get16(t, 4) == 1, "version is 1");
	check(Get16(t, 6) == Riivo::RIIVO_MANIFEST_HEADER, "header length field");
	check(Get32(t, 8) == t.size(), "total size matches the buffer");
	check(Get32(t, 12) == caps, "caps round-trip");
	check(Get32(t, 20) == 3, "count is 3");
	check(Get32(t, 28) == Riivo::RIIVO_MANIFEST_DISCOVER, "discover LBA");
	check(Get32(t, 32) == 0x53424E59u, "disc id");
	check(Get32(t, 44) == 0, "reserved is zero");

	u32 strOff = Get32(t, 24);
	check(strOff == Riivo::RIIVO_MANIFEST_HEADER + 3 * Riivo::RIIVO_MANIFEST_ENTRY,
		  "string blob starts past the entries");

	size_t e0 = Riivo::RIIVO_MANIFEST_HEADER;
	check(Get64(t, e0 + 0) == 0x180000000ULL, "entry 0 offset");
	check(Get32(t, e0 + 8) == 0x8000, "entry 0 length");
	check(Get16(t, e0 + 12) == Riivo::RIIVO_EXT_EXTERNAL, "entry 0 kind");
	check(Get16(t, e0 + 14) == Riivo::RIIVO_SRC_SD, "entry 0 source");
	check(Get64(t, e0 + 16) == 0x100, "entry 0 source offset preserved");

	size_t e1 = e0 + Riivo::RIIVO_MANIFEST_ENTRY;
	check(Get16(t, e1 + 12) == Riivo::RIIVO_EXT_ZERO, "entry 1 kind is Zero");

	size_t e2 = e1 + Riivo::RIIVO_MANIFEST_ENTRY;
	check(Get16(t, e2 + 12) == Riivo::RIIVO_EXT_GENERATED, "entry 2 kind");
	check(Get32(t, e2 + 28) == 0x1000, "generated offset");

	// Blob path, the way the runtime reads it: blob-relative to first NUL.
	u32 p0 = Get32(t, e0 + 24);
	std::string path;
	for (size_t i = strOff + p0; i < t.size() && t[i]; ++i)
		path.push_back((char) t[i]);
	check(path == "/mod/a.bin", "blob path decodes");

	check(Riivo::ValidateManifestV1(&t[0], (u32) t.size(), why),
		  "valid manifest validates");
}

static void TestRefusals()
{
	std::vector<u8> t;
	std::string why;

	check(!Riivo::BuildManifestV1(std::vector<Riivo::ManifestExtent>(),
								  0, 0, 0, 0, 0, t, why),
		  "refuses an empty extent list");

	std::vector<Riivo::ManifestExtent> overlap = SampleExtents();
	overlap[1].discOffset = overlap[0].discOffset + 1;
	check(!Riivo::BuildManifestV1(overlap, 0, 0, 0, 0, 0, t, why),
		  "refuses overlapping extents");

	std::vector<Riivo::ManifestExtent> badKind = SampleExtents();
	badKind[0].kind = 99;
	check(!Riivo::BuildManifestV1(badKind, 0, 0, 0, 0, 0, t, why),
		  "refuses an unknown extent kind");

	std::vector<Riivo::ManifestExtent> noSrc = SampleExtents();
	noSrc[0].source = Riivo::RIIVO_SRC_NONE;
	check(!Riivo::BuildManifestV1(noSrc, 0, 0, 0, 0, 0, t, why),
		  "refuses an external extent with no source");

	std::vector<Riivo::ManifestExtent> zeroPath = SampleExtents();
	zeroPath[1].path = "/should/not/be/here";
	check(!Riivo::BuildManifestV1(zeroPath, 0, 0, 0, 0, 0, t, why),
		  "refuses a zero extent naming a path");

	std::vector<Riivo::ManifestExtent> relPath = SampleExtents();
	relPath[0].path = "relative/a.bin";
	check(!Riivo::BuildManifestV1(relPath, 0, 0, 0, 0, 0, t, why),
		  "refuses a relative path");
}

static void TestIntegrity()
{
	std::vector<Riivo::ManifestExtent> in = SampleExtents();
	std::vector<u8> good;
	std::string why;
	check(Riivo::BuildManifestV1(in, 0, 0, 0, 0, 0, good, why), "builds");
	if (good.empty())
		return;

	std::vector<u8> bad = good;
	bad[0] ^= 0xFF;
	check(!Riivo::ValidateManifestV1(&bad[0], (u32) bad.size(), why),
		  "bad magic refused before anything else");

	bad = good;
	bad[4] ^= 0xFF;
	check(!Riivo::ValidateManifestV1(&bad[0], (u32) bad.size(), why),
		  "wrong version refused");

	bad = good;
	bad[30] ^= 0xFF;
	check(!Riivo::ValidateManifestV1(&bad[0], (u32) bad.size(), why),
		  "single-byte corruption fails the crc");

	check(!Riivo::ValidateManifestV1(&good[0], (u32) good.size() - 1, why),
		  "truncated buffer refused");

	// Legacy tables are a different magic, never a misread v1.
	std::vector<u8> legacy(16, 0);
	legacy[0] = 0x52;
	legacy[1] = 0x49;
	legacy[2] = 0x49;
	legacy[3] = 0x56; // 'RIIV'
	check(!Riivo::ValidateManifestV1(&legacy[0], 16, why),
		  "legacy 'RIIV' table refused as v1");
}

static void TestRangeContract()
{
	using namespace Riivo;
	const u64 probe = 0x47000000ULL * 4;
	const u64 base = 0x180000000ULL;
	const u64 limit = 0x200000000ULL;

	check(ValidatePartitionRange(base, 0x8000, probe, base, limit) == RANGE_OK,
		  "in-window range is placeable");
	check(ValidatePartitionRange(base - 1, 0x8000, probe, base, limit)
		  == RANGE_BELOW_REGION, "below-region start refused");
	check(ValidatePartitionRange(limit - 0x100, 0x200, probe, base, limit)
		  == RANGE_ABOVE_REGION, "past-limit end refused");
	check(ValidatePartitionRange(0xFFFFFFFFFFFFFF00ULL, 0x200, probe, base, limit)
		  == RANGE_WRAPS, "wrapping range refused");
	check(ValidatePartitionRange(probe - 0x100, 0x200, probe, 0, limit)
		  == RANGE_HITS_DVD9_PROBE, "probe-straddling range refused");
	check(ValidatePartitionRange(0x1FFFFFFFFULL, 0x200, probe, 0, 0x300000000ULL)
		  == RANGE_HITS_SIGNED_WRAP, "signed-wrap range refused");

	// Address spaces do not convert silently.
	PartitionBytes pb;
	check(DiWordsToPartitionBytes(DiWord(0x60000000ULL), pb)
		  && pb.v == 0x180000000ULL, "DI words scale to partition bytes");
	DiWord dw;
	check(!PartitionBytesToDiWords(PartitionBytes(0x180000001ULL), dw),
		  "unaligned bytes have no DI word");
	PhysSector ps;
	check(!BytesToPhysSector(1000, 512, ps), "mid-sector byte has no sector");
	u64 sb = 0;
	check(PhysSectorToBytes(PhysSector(8), 512, sb) && sb == 4096,
		  "sector scales by the drive size");
	check(!SectorSizeValid(1000), "non-power-of-two sector size rejected");
}

int main()
{
	TestRoundTrip();
	TestRefusals();
	TestIntegrity();
	TestRangeContract();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
