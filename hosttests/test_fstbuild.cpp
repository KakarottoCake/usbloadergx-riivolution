// Host tests for Riivo::FstBuilder.
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFst.hpp"

using namespace Riivo;

static int failures = 0;
static int checks = 0;

static void ck(bool cond, const char *what)
{
	++checks;
	if (!cond) { printf("  FAIL: %s\n", what); ++failures; }
}

// ---- synthetic FST writer (mirrors the on-disc format) -------------------
struct SEnt { u8 type; std::string name; u32 a, b; };

static void wr32(std::vector<u8> &v, size_t at, u32 x)
{
	v[at] = x >> 24; v[at+1] = x >> 16; v[at+2] = x >> 8; v[at+3] = x;
}

// Build: root(4 entries total)
//  1 dir  "AudioRes"  parent=0 end=4
//  2 file "a.ast"     off=0x1000>>2 len=0x800
//  3 file "top.bin"   off=0x4000>>2 len=0x40
static void BuildSample(std::vector<u8> &out)
{
	std::vector<SEnt> e;
	e.push_back((SEnt){1, "",          0, 4});      // root
	e.push_back((SEnt){1, "AudioRes",  0, 3});      // dir, children [2,3)
	e.push_back((SEnt){0, "a.ast",     0x1000 >> 2, 0x800});
	e.push_back((SEnt){0, "top.bin",   0x4000 >> 2, 0x40});

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

int main()
{
	std::vector<u8> fst;
	BuildSample(fst);

	printf("1. round-trip an untouched table\n");
	{
		FstBuilder b;
		ck(b.Parse(&fst[0], (u32) fst.size(), true), "parse");
		std::vector<u8> out;
		b.Serialize(out, true);

		Fst a, c;
		ck(a.Parse(&fst[0], (u32) fst.size(), true), "orig parses");
		ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
		ck(a.FileCount() == c.FileCount(), "same file count");
		ck(c.FileCount() == 2, "two files");

		const FstFile *f = c.FindFile("/audiores/a.ast");
		ck(f != 0, "a.ast present");
		if (f) {
			ck(f->offset == 0x1000, "a.ast offset preserved");
			ck(f->length == 0x800, "a.ast length preserved");
		}
		const FstFile *t = c.FindFile("/top.bin");
		ck(t != 0 && t->offset == 0x4000, "top.bin preserved");
	}

	printf("2. replace an existing file with a bigger one\n");
	{
		FstBuilder b;
		b.Parse(&fst[0], (u32) fst.size(), true);
		bool isNew = true;
		ck(b.AddOrReplace("/audiores/a.ast", 0x9000, &isNew), "replace ok");
		ck(!isNew, "reported as existing");
		ck(b.Stats().replaced == 1, "one replacement");

		const u64 extent = b.OriginalExtent();
		ck(extent == 0x4040, "extent excludes the modded file");

		b.Layout(0x10000, 0x800);
		u64 off = 0; u32 len = 0;
		ck(b.FindAssigned("/audiores/a.ast", &off, &len), "assigned");
		ck(off == 0x10000, "placed at region start");
		ck(len == 0x9000, "new length recorded");
		ck((off & 0x7ff) == 0, "sector aligned");

		std::vector<u8> out;
		b.Serialize(out, true);
		Fst c;
		ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
		const FstFile *f = c.FindFile("/audiores/a.ast");
		ck(f && f->offset == 0x10000 && f->length == 0x9000, "survives serialise");
		const FstFile *t = c.FindFile("/top.bin");
		ck(t && t->offset == 0x4000 && t->length == 0x40, "unmodded file untouched");
	}

	printf("3. add files that are not on the disc, in new directories\n");
	{
		FstBuilder b;
		b.Parse(&fst[0], (u32) fst.size(), true);
		bool isNew = false;
		ck(b.AddOrReplace("/StageData/NewGalaxy.arc", 0x100, &isNew), "add nested");
		ck(isNew, "reported as new");
		ck(b.AddOrReplace("/StageData/Second.arc", 0x200, &isNew), "add sibling");
		ck(b.AddOrReplace("/LocalizeData/German/Msg.arc", 0x300, &isNew), "add deep");
		ck(b.Stats().added == 3, "three added");
		ck(b.Stats().addedDirs == 3, "three dirs created");

		b.Layout(0x20000, 0x800);
		std::vector<u8> out;
		b.Serialize(out, true);

		Fst c;
		ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
		ck(c.FileCount() == 5, "2 original + 3 added");

		const FstFile *g = c.FindFile("/stagedata/newgalaxy.arc");
		ck(g != 0, "added file found by path");
		if (g) ck(g->length == 0x100, "added length");
		const FstFile *d = c.FindFile("/localizedata/german/msg.arc");
		ck(d != 0, "deep added file found");

		// the originals must still be intact and where they were
		const FstFile *a = c.FindFile("/audiores/a.ast");
		ck(a && a->offset == 0x1000 && a->length == 0x800, "original untouched");

		// every assigned range must be aligned, ordered and non-overlapping
		std::vector<const FstFile *> all;
		u64 prevEnd = 0;
		bool ok = true;
		for (size_t i = 0; i < c.FileCount(); ++i) {
			const FstFile &f = c.FileAt(i);
			if (f.offset < 0x20000) continue; // an original
			if ((f.offset & 0x7ff) != 0) ok = false;
			if (f.offset < prevEnd) ok = false;
			prevEnd = f.offset + f.length;
		}
		ck(ok, "assigned ranges aligned and non-overlapping");
	}

	printf("4. directory subtree ends stay consistent after insertion\n");
	{
		FstBuilder b;
		b.Parse(&fst[0], (u32) fst.size(), true);
		bool isNew;
		// insert INTO an existing directory, which shifts every later index
		b.AddOrReplace("/AudioRes/zz_new.ast", 0x10, &isNew);
		b.AddOrReplace("/AudioRes/Sub/deep.ast", 0x10, &isNew);
		b.Layout(0x30000, 0x800);
		std::vector<u8> out;
		b.Serialize(out, true);

		Fst c;
		ck(c.Parse(&out[0], (u32) out.size(), true), "rebuilt parses");
		// If the end markers were wrong, these would land in the wrong folder
		// or vanish entirely.
		ck(c.FindFile("/audiores/zz_new.ast") != 0, "sibling in right dir");
		ck(c.FindFile("/audiores/sub/deep.ast") != 0, "nested dir correct");
		ck(c.FindFile("/top.bin") != 0, "later top-level entry survived");
		std::vector<const FstFile *> kids;
		c.ListFolder("/audiores", false, kids);
		ck(kids.size() == 2, "AudioRes has exactly its 2 direct files");
	}

	printf("5. hostile input is rejected, not crashed on\n");
	{
		FstBuilder b;
		ck(!b.Parse(0, 0, true), "null");
		u8 tiny[4] = {0};
		ck(!b.Parse(tiny, 4, true), "too small");
		std::vector<u8> bad = fst;
		wr32(bad, 8, 0x7fffffff); // absurd entry count
		ck(!b.Parse(&bad[0], (u32) bad.size(), true), "count past buffer");

		FstBuilder d;
		d.Parse(&fst[0], (u32) fst.size(), true);
		bool n;
		ck(!d.AddOrReplace("", 1, &n), "empty path");
		ck(!d.AddOrReplace("/", 1, &n), "root only");
		ck(!d.AddOrReplace("/AudioRes", 1, &n), "refuses to clobber a directory");
		ck(!d.AddOrReplace("/top.bin/x.arc", 1, &n), "refuses file as a directory");
	}

	printf("6. 64-bit offsets survive past the 4 GiB line\n");
	{
		FstBuilder b;
		b.Parse(&fst[0], (u32) fst.size(), true);
		bool n;
		b.AddOrReplace("/big.bin", 0x1000, &n);
		const u64 high = 0x120000000ULL; // 4.5 GB, past what a u32 holds
		b.Layout(high, 0x800);
		u64 off = 0;
		ck(b.FindAssigned("/big.bin", &off, 0), "found");
		ck(off == high, "offset kept full width in the tree");

		std::vector<u8> out;
		b.Serialize(out, true);
		Fst c;
		c.Parse(&out[0], (u32) out.size(), true);
		const FstFile *f = c.FindFile("/big.bin");
		ck(f != 0, "present after serialise");
		// stored as offset>>2, so 0x120000000 -> 0x48000000, which fits
		if (f) ck(f->offset == high, "round-trips through the shifted field");
	}

	printf("LayoutFrom: offsets decided before the table could be read\n");
	{
		//! d2x refuses IOCTL_DI_FRAG_SET once a title is running, and opening
		//! the game partition starts one - so the fragments must be registered
		//! before the file table can be read. The placement is therefore made
		//! from the files on the card and applied to the table afterwards.
		//! These two have to agree exactly or the game reads the wrong bytes.
		FstBuilder b;
		bool n = false;
		b.AddOrReplace("/ObjectData/One.arc", 0x2000, &n);
		b.AddOrReplace("/StageData/Deep/Two.arc", 0x3000, &n);
		b.AddOrReplace("/Three.arc", 0x1000, &n);

		//! Keys are lower-cased with a leading slash, which is what
		//! NormaliseDiscPath produces on the other side.
		std::map<std::string, u64> byPath;
		byPath["/objectdata/one.arc"]     = 0x100000000ULL;
		byPath["/stagedata/deep/two.arc"] = 0x100002000ULL;
		byPath["/three.arc"]              = 0x100005000ULL;

		ck(b.LayoutFrom(byPath) == 0, "every modded entry was placed");

		u64 off = 0;
		ck(b.FindAssigned("/ObjectData/One.arc", &off, 0) && off == 0x100000000ULL,
		   "nested file took its assigned offset");
		ck(b.FindAssigned("/StageData/Deep/Two.arc", &off, 0) && off == 0x100002000ULL,
		   "deeper file too");
		ck(b.FindAssigned("/Three.arc", &off, 0) && off == 0x100005000ULL,
		   "and one at the root");
		ck(b.Stats().highestOffset == 0x100005000ULL + 0x1000,
		   "the high-water mark covers the last file");

		//! A modded entry with no offset would be pointed at whatever happens
		//! to be there, so it must be reported rather than silently placed.
		FstBuilder c;
		c.AddOrReplace("/ObjectData/One.arc", 0x2000, &n);
		c.AddOrReplace("/Missing.arc", 0x1000, &n);
		std::map<std::string, u64> partial;
		partial["/objectdata/one.arc"] = 0x100000000ULL;
		ck(c.LayoutFrom(partial) == 1, "an unplaced entry is counted, not ignored");

		//! Case in the table must not matter - the mod's folder rules and the
		//! disc's own spelling routinely differ.
		FstBuilder d;
		d.AddOrReplace("/MiXeD/CaSe.arc", 0x800, &n);
		std::map<std::string, u64> lower;
		lower["/mixed/case.arc"] = 0x100008000ULL;
		ck(d.LayoutFrom(lower) == 0, "a lower-cased key matches a mixed-case entry");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
