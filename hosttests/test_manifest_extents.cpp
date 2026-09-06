/****************************************************************************
 * WP2 fixtures: redirect specs -> v1 manifest External extents.
 *
 * The bridge preserves what the legacy 'RIIV' table cannot: fileOffset
 * sub-ranges, whole-file (length=0) resolution against stat sizes, and the
 * resize=false clamp. Refusals name the file; the manifest builder
 * re-checks order/overlap, so this suite also chains into BuildManifestV1
 * + ValidateManifestV1 to prove the handoff.
 ***************************************************************************/
#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

static Riivo::RedirectSpec Spec(u64 off, u32 len, u32 fileOff, u32 discLen,
								const char *external)
{
	Riivo::RedirectSpec s;
	s.discOffset = off;
	s.length = len;
	s.fileOffset = fileOff;
	s.discLength = discLen;
	s.disc = "/x";
	s.external = external;
	return s;
}

static void TestSubRange()
{
	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(Spec(0x1000, 0x200, 0x100, 0x1000, "sd:/mod/a.bin"));
	std::map<std::string, u32> sizes;
	sizes["sd:/mod/a.bin"] = 0x1000;
	std::vector<bool> rz(1, true);
	std::vector<Riivo::ManifestExtent> out;
	std::string why;

	check(Riivo::BuildManifestExtents(specs, sizes, rz, out, why),
		  "builds a partial replacement");
	check(out.size() == 1, "one spec in, one extent out");
	if (out.empty())
		return;
	check(out[0].discOffset == 0x1000, "disc offset kept");
	check(out[0].length == 0x200, "replacement length kept");
	check(out[0].srcOffset == 0x100, "file offset preserved as source offset");
	check(out[0].source == Riivo::RIIVO_SRC_SD, "sd: classifies as SD");
	check(out[0].path == "/mod/a.bin", "device prefix stripped, path absolute");
	check(out[0].kind == Riivo::RIIVO_EXT_EXTERNAL, "kind is External");
}

static void TestWholeFile()
{
	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(Spec(0x2000, 0, 0x80, 0x1000, "usb1:/mod/b.bin"));
	std::map<std::string, u32> sizes;
	sizes["usb1:/mod/b.bin"] = 0x1000;
	std::vector<Riivo::ManifestExtent> out;
	std::string why;

	check(Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									  out, why),
		  "length=0 resolves against the stat size");
	check(!out.empty() && out[0].length == 0xF80,
		  "length is size minus fileOffset");
	check(!out.empty() && out[0].source == Riivo::RIIVO_SRC_USB,
		  "usb1: classifies as USB");
}

static void TestResizeClamp()
{
	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(Spec(0x3000, 0x2000, 0, 0x400, "sd:/mod/c.bin"));
	std::map<std::string, u32> sizes;
	sizes["sd:/mod/c.bin"] = 0x3000;
	std::vector<Riivo::ManifestExtent> out;
	std::string why;

	std::vector<bool> grow(1, true);
	check(Riivo::BuildManifestExtents(specs, sizes, grow, out, why),
		  "resize=true keeps the full length");
	check(!out.empty() && out[0].length == 0x2000, "no clamp when growing");

	std::vector<bool> keep(1, false);
	check(Riivo::BuildManifestExtents(specs, sizes, keep, out, why),
		  "resize=false still builds");
	check(!out.empty() && out[0].length == 0x400,
		  "resize=false clamps to the disc length");
}

static void TestRefusals()
{
	std::vector<Riivo::ManifestExtent> out;
	std::string why;
	std::map<std::string, u32> sizes;
	sizes["sd:/mod/a.bin"] = 0x1000;

	check(!Riivo::BuildManifestExtents(std::vector<Riivo::RedirectSpec>(),
									   sizes, std::vector<bool>(), out, why),
		  "refuses an empty spec list");

	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(Spec(0x1000, 0x200, 0, 0x1000, "sd:/mod/a.bin"));
	std::map<std::string, u32> empty;
	check(!Riivo::BuildManifestExtents(specs, empty, std::vector<bool>(),
									   out, why)
		  && why.find("sd:/mod/a.bin") != std::string::npos,
		  "missing size refused by file name");

	specs[0].fileOffset = 0x2000;
	check(!Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									   out, why),
		  "fileoffset past EOF refused");

	specs[0].fileOffset = 0x100;
	specs[0].length = 0xF01; // 0x100 + 0xF01 > 0x1000
	check(!Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									   out, why),
		  "range past EOF refused");

	specs[0].length = 0x100;
	specs[0].external = "net:/mod/a.bin";
	sizes.clear();
	sizes["net:/mod/a.bin"] = 0x1000;
	check(!Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									   out, why),
		  "unknown device refused (RiiFS arrives in WP8)");

	std::vector<bool> wrong(2, true);
	specs[0].external = "sd:/mod/a.bin";
	sizes.clear();
	sizes["sd:/mod/a.bin"] = 0x1000;
	check(!Riivo::BuildManifestExtents(specs, sizes, wrong, out, why),
		  "mismatched resize-flag count refused");

	// Zero-length after resolution prunes to an empty table, refused.
	specs[0].fileOffset = 0x1000;
	specs[0].length = 0;
	check(!Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									   out, why),
		  "fully pruned table refused");
}

static void TestOrderingAndChain()
{
	std::vector<Riivo::RedirectSpec> specs;
	specs.push_back(Spec(0x9000, 0x100, 0, 0x100, "sd:/mod/z.bin"));
	specs.push_back(Spec(0x1000, 0x100, 0, 0x100, "sd:/mod/a.bin"));
	std::map<std::string, u32> sizes;
	sizes["sd:/mod/z.bin"] = 0x100;
	sizes["sd:/mod/a.bin"] = 0x100;
	std::vector<Riivo::ManifestExtent> out;
	std::string why;

	check(Riivo::BuildManifestExtents(specs, sizes, std::vector<bool>(),
									  out, why),
		  "builds unsorted specs");
	check(out.size() == 2 && out[0].discOffset == 0x1000
		  && out[1].discOffset == 0x9000, "output sorted ascending");

	std::vector<u8> tab;
	check(Riivo::BuildManifestV1(out, Riivo::RIIVO_MANIFEST_DISCOVER, 0, 0,
								 Riivo::RIIVO_CAP_SPLIT_READ,
								 Riivo::RIIVO_PROV_BOUNDED, tab, why),
		  "bridge output builds a v1 manifest");
	check(!tab.empty()
		  && Riivo::ValidateManifestV1(&tab[0], (u32) tab.size(), why),
		  "bridge output validates as v1");
}

int main()
{
	TestSubRange();
	TestWholeFile();
	TestResizeClamp();
	TestRefusals();
	TestOrderingAndChain();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
