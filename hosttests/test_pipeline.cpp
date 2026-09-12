// Host test for the seam RiivoBoot::PrepareFileRedirects actually walks:
//   disc FST -> BuildRedirects -> FstBuilder -> rebuilt FST
// The two halves are each covered by their own suite; what is untested is that
// the disc paths one produces are the ones the other can consume. A mismatch
// there is silent - the plan would just report zeros - so it is worth pinning.
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoFstBuild.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

// ---- synthetic disc ------------------------------------------------------
struct SEnt { u8 type; std::string name; u32 a, b; };

static void wr32(std::vector<u8> &v, size_t at, u32 x)
{
	v[at] = x >> 24; v[at+1] = x >> 16; v[at+2] = x >> 8; v[at+3] = x;
}

static void Flatten(const std::vector<SEnt> &e, std::vector<u8> &out)
{
	const u32 n = (u32) e.size();
	out.assign(n * 12, 0);
	std::string strings;
	for (u32 i = 0; i < n; ++i) {
		u32 nameOff = (u32) strings.size();
		strings += e[i].name; strings += '\0';
		out[i*12] = e[i].type;
		out[i*12+1] = nameOff >> 16; out[i*12+2] = nameOff >> 8; out[i*12+3] = nameOff;
		wr32(out, i*12+4, e[i].a);
		wr32(out, i*12+8, e[i].b);
	}
	out.insert(out.end(), strings.begin(), strings.end());
}

// /StageData/Galaxy1.arc, /StageData/Galaxy2.arc, /AudioRes/bgm.ast, /boot.bin
// Deliberately mixed case, because a mod's folder on the card rarely matches.
static void BuildDisc(std::vector<u8> &out)
{
	std::vector<SEnt> e;
	e.push_back((SEnt){1, "",          0, 6});            // root, 6 entries
	e.push_back((SEnt){1, "StageData", 0, 4});            // children [2,4)
	e.push_back((SEnt){0, "Galaxy1.arc", 0x20000 >> 2, 0x1000});
	e.push_back((SEnt){0, "Galaxy2.arc", 0x30000 >> 2, 0x2000});
	e.push_back((SEnt){1, "AudioRes",  0, 6});            // children [5,6)
	e.push_back((SEnt){0, "bgm.ast",   0x40000 >> 2, 0x8000});
	Flatten(e, out);
}

// ---- a DirLister standing in for the mod folder on the card --------------
struct FakeLister : public DirLister
{
	std::vector<std::string> files;
	void List(const std::string &, bool, std::vector<std::string> &out) { out = files; }
};

int main()
{
	std::vector<u8> discFst;
	BuildDisc(discFst);

	Fst fst;
	ck(fst.Parse(&discFst[0], (u32) discFst.size(), true), "disc FST parses");
	ck(fst.FileCount() == 3, "3 files on the disc");

	//! One <folder> patch, create=true, pointing at a mod folder that holds:
	//!  - a replacement for a disc file, at the SAME size
	//!  - a replacement that is BIGGER than the disc file
	//!  - two files the disc has never heard of, one of them nested
	//!  - a replacement whose case differs from the disc's
	FakeLister lister;
	lister.files.push_back("Galaxy1.arc");            // same size
	lister.files.push_back("galaxy2.arc");            // bigger, and lower-cased
	lister.files.push_back("Galaxy99.arc");           // added
	lister.files.push_back("Sub/Deep/Extra.arc");     // added, two new dirs

	ResolvedPatchSet set;
	ResolvedFolder f;
	f.root = "";
	f.disc = "/StageData";
	f.external = "/Spectral/StageData";
	f.resize = true;
	f.create = true;
	f.recursive = true;
	f.length = 0;
	set.folders.push_back(f);

	std::vector<RedirectSpec> redirects;
	std::vector<CreatedFile> created;
	BuildRedirects(fst, set, "usb1:", &lister, redirects, &created);

	printf("1. BuildRedirects splits matched from added\n");
	ck(redirects.size() == 2, "2 files matched a disc entry");
	ck(created.size() == 2, "2 files have no disc entry");

	//! The case-differing name must match the disc entry, not be treated as new.
	bool sawG2 = false;
	for (size_t i = 0; i < redirects.size(); ++i)
		if (redirects[i].disc == "/stagedata/galaxy2.arc") sawG2 = true;
	ck(sawG2, "case-insensitive match against the disc");

	printf("2. every disc path handed over is one FstBuilder accepts\n");
	FstBuilder b;
	ck(b.Parse(&discFst[0], (u32) discFst.size(), true), "builder parses the same FST");

	//! Exactly what PrepareFileRedirects does, with stat() replaced by a table.
	const u32 sizes[4] = { 0x1000, 0x9000, 0x400, 0x400 };
	u32 planned = 0, rejected = 0;
	bool isNew = false;
	for (size_t i = 0; i < redirects.size(); ++i) {
		const u32 sz = redirects[i].disc == "/stagedata/galaxy2.arc" ? sizes[1] : sizes[0];
		if (b.AddOrReplace(redirects[i].disc, sz, &isNew)) ++planned; else ++rejected;
	}
	for (size_t i = 0; i < created.size(); ++i)
		if (b.AddOrReplace(created[i].disc, sizes[2], &isNew)) ++planned; else ++rejected;

	ck(rejected == 0, "no path was rejected");
	ck(planned == 4, "all 4 files planned");
	ck(b.Stats().replaced == 2, "2 replaced");
	ck(b.Stats().added == 2, "2 added");
	ck(b.Stats().addedDirs == 2, "Sub and Deep created");

	printf("3. the rebuilt table says what the plan claimed\n");
	const u64 extent = b.OriginalExtent();
	//! bgm.ast is the highest unmodded file: 0x40000 + 0x8000.
	ck(extent == 0x48000, "extent covers only unmodded files");
	const u64 region = (extent + 0x1fffffULL) & ~0x1fffffULL;
	b.Layout(region, 0x800);

	std::vector<u8> rebuilt;
	b.Serialize(rebuilt, true);
	ck(b.Stats().fstSize > (u32) discFst.size(), "table grew");

	Fst out;
	ck(out.Parse(&rebuilt[0], (u32) rebuilt.size(), true), "rebuilt table parses");
	ck(out.FileCount() == 5, "3 original + 2 added");

	const FstFile *g1 = out.FindFile("/stagedata/galaxy1.arc");
	ck(g1 && g1->length == 0x1000, "same-size replacement keeps its length");
	ck(g1 && g1->offset >= region, "and is still relocated");

	const FstFile *g2 = out.FindFile("/stagedata/galaxy2.arc");
	ck(g2 && g2->length == 0x9000, "grown replacement gets its REAL length");
	ck(g2 && g2->offset >= region, "grown replacement moved above the disc data");

	const FstFile *n1 = out.FindFile("/stagedata/galaxy99.arc");
	ck(n1 && n1->length == 0x400, "added file is in the table");
	const FstFile *n2 = out.FindFile("/stagedata/sub/deep/extra.arc");
	ck(n2 != 0, "added file in newly created directories");

	const FstFile *bgm = out.FindFile("/audiores/bgm.ast");
	ck(bgm && bgm->offset == 0x40000 && bgm->length == 0x8000,
	   "an untouched file is bit-identical");

	printf("4. no relocated range overlaps another, or the disc\n");
	bool ok = true;
	std::vector<std::pair<u64, u64> > used;
	for (size_t i = 0; i < out.FileCount(); ++i) {
		const FstFile &e = out.FileAt(i);
		if (e.offset < region) continue;
		if ((e.offset & 0x7ff) != 0) ok = false;
		for (size_t k = 0; k < used.size(); ++k)
			if (e.offset < used[k].second && e.offset + e.length > used[k].first) ok = false;
		used.push_back(std::make_pair(e.offset, (u64) e.offset + e.length));
	}
	ck(used.size() == 4, "all 4 modded files were relocated");
	ck(ok, "aligned and non-overlapping, clear of the disc data");

	printf("5. a folder without a disc path matches by basename (Newer Others)\n");
	//! Synthetic FST, clearly labeled: models the Newer shape (nested disc
	//! files under /Layout, one duplicated basename), not real disc data.
	//! /Layout/characterChange/characterChange.arc @0x10000 len 0x1000
	//! /Layout/dateFile/dateFile.arc @0x20000 len 0x2000
	//! /Layout/dupA/same.arc @0x30000 len 0x400 } duplicate basename:
	//! /Layout/dupB/same.arc @0x40000 len 0x800 } first FST-order hit wins
	std::vector<u8> newerDisc;
	{
		std::vector<SEnt> e;
		e.push_back((SEnt){1, "",                0, 10});
		e.push_back((SEnt){1, "Layout",          0, 10});
		e.push_back((SEnt){1, "characterChange", 0, 4});
		e.push_back((SEnt){0, "characterChange.arc", 0x10000 >> 2, 0x1000});
		e.push_back((SEnt){1, "dateFile",        0, 6});
		e.push_back((SEnt){0, "dateFile.arc",    0x20000 >> 2, 0x2000});
		e.push_back((SEnt){1, "dupA",            0, 8});
		e.push_back((SEnt){0, "same.arc",        0x30000 >> 2, 0x400});
		e.push_back((SEnt){1, "dupB",            0, 10});
		e.push_back((SEnt){0, "same.arc",        0x40000 >> 2, 0x800});
		Flatten(e, newerDisc);
	}
	Fst newerFst;
	ck(newerFst.Parse(&newerDisc[0], (u32) newerDisc.size(), true), "newer-shape base parses");
	ck(newerFst.FileCount() == 4, "4 files on the newer-shape disc");

	//! The struct production Resolve produces for NewerSMBW.xml's Others
	//! rule (root /NewerSMBW, no disc path, no create): verified against a
	//! resolve run over the real XML. Filenames below are real Others/
	//! names; missing.arc and the nested probe are synthetic.
	ResolvedPatchSet newerSet;
	ResolvedFolder others;
	others.root = "/NewerSMBW";
	others.external = "Others";
	others.disc = "";
	others.resize = true;
	others.create = false;
	others.recursive = true;
	others.length = 0;
	newerSet.folders.push_back(others);

	FakeLister newerLister;
	newerLister.files.push_back("characterChange.arc"); // nested disc match
	newerLister.files.push_back("dateFile.arc");        // nested disc match
	newerLister.files.push_back("missing.arc");         // unmatched: ignored
	newerLister.files.push_back("Sub/characterChange.arc"); // nested rel: never matches

	std::vector<RedirectSpec> newerRedirects;
	std::vector<CreatedFile> newerCreated;
	BuildRedirects(newerFst, newerSet, "usb1:", &newerLister, newerRedirects, &newerCreated);
	ck(newerRedirects.size() == 2, "dataless folder: 2 top-level names matched");
	ck(newerCreated.size() == 0, "dataless folder: nothing created");
	bool sawCC = false, sawDF = false;
	for (size_t i = 0; i < newerRedirects.size(); ++i)
	{
		const RedirectSpec &r = newerRedirects[i];
		if (r.disc == "/layout/characterchange/characterchange.arc")
		{
			sawCC = true;
			ck(r.discOffset == 0x10000, "matched file keeps its disc offset");
			ck(r.discLength == 0x1000, "matched file keeps its disc size");
			ck(r.external == "usb1:/NewerSMBW/Others/characterChange.arc",
			   "matched file keeps its external path");
		}
		if (r.disc == "/layout/datefile/datefile.arc")
		{
			sawDF = true;
			ck(r.discOffset == 0x20000, "second match keeps its disc offset");
			ck(r.discLength == 0x2000, "second match keeps its disc size");
		}
	}
	ck(sawCC, "nested disc file matched by top-level basename");
	ck(sawDF, "second nested disc file matched");

	printf("6. explicit paths are untouched; duplicates take the first hit\n");
	//! Same file through an explicit disc path: identical target, proving
	//! the exact lookup did not change.
	ResolvedPatchSet pathedSet;
	ResolvedFolder pathed;
	pathed.root = "/NewerSMBW";
	pathed.external = "Others";
	pathed.disc = "/Layout/characterChange";
	pathed.resize = true;
	pathed.create = false;
	pathed.recursive = true;
	pathed.length = 0;
	pathedSet.folders.push_back(pathed);
	FakeLister pathedLister;
	pathedLister.files.push_back("characterChange.arc");
	std::vector<RedirectSpec> pathedRedirects;
	std::vector<CreatedFile> pathedCreated;
	BuildRedirects(newerFst, pathedSet, "usb1:", &pathedLister, pathedRedirects, &pathedCreated);
	ck(pathedRedirects.size() == 1, "explicit path: control matches");
	if (pathedRedirects.size() == 1)
	{
		ck(pathedRedirects[0].disc == "/layout/characterchange/characterchange.arc",
		   "explicit path: same disc target as the basename match");
		ck(pathedRedirects[0].discOffset == 0x10000, "explicit path: same offset");
	}

	//! Duplicate basenames: first FST-order hit wins, with its own
	//! offset and size (not the later twin's).
	FakeLister dupLister;
	dupLister.files.push_back("same.arc");
	std::vector<RedirectSpec> dupRedirects;
	std::vector<CreatedFile> dupCreated;
	BuildRedirects(newerFst, newerSet, "usb1:", &dupLister, dupRedirects, &dupCreated);
	ck(dupRedirects.size() == 1, "duplicate basenames: exactly one match");
	if (dupRedirects.size() == 1)
	{
		ck(dupRedirects[0].disc == "/layout/dupa/same.arc",
		   "duplicate basenames: first FST-order hit wins");
		ck(dupRedirects[0].discOffset == 0x30000, "duplicate: first hit's offset");
		ck(dupRedirects[0].discLength == 0x400, "duplicate: first hit's size");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
