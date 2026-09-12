// Patch-plan builder through the production code path.
// Exercises Riivo::BuildPatchPlan (source/riivo/RiivoPatchPlan.cpp) with a
// synthetic FST + resolved sets: absolute/relative externals, patch roots,
// param expansion is covered by resolver tests; here: file replacement and
// creation, folder recursion + dataless basename, duplicate precedence,
// offset/fileoffset/length/resize, multiple mods to one file, zero-length,
// missing files/paths, main.dol refusal. FST + Config + File + Validate link
// set mirrors test_pipeline (production linkage, not a helper copy).
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#include "riivo/RiivoTypes.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoConfig.hpp"
#include "riivo/RiivoPatchPlan.hpp"

using namespace Riivo;

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// In-memory DirLister: fullDir -> relative files.
struct MemLister : public DirLister
{
	std::map<std::string, std::vector<std::string> > dirs;
	void Add(const std::string &dir, const std::string &rel)
	{
		dirs[dir].push_back(rel);
	}
	virtual void List(const std::string &fullDir, bool recursive,
					  std::vector<std::string> &out)
	{
		(void)recursive; // fixtures list exactly (no subdirs in these cases)
		std::map<std::string, std::vector<std::string> >::iterator it = dirs.find(fullDir);
		if (it == dirs.end())
			return;
		out.insert(out.end(), it->second.begin(), it->second.end());
	}
};

// In-memory sizes: external path -> size. Absent => missing.
struct MemSizes : public FileSizeProvider
{
	std::map<std::string, u32> m;
	void Put(const std::string &p, u32 s) { m[p] = s; }
	virtual bool GetSize(const std::string &external, u32 *outSize)
	{
		std::map<std::string, u32>::iterator it = m.find(external);
		if (it == m.end())
			return false;
		if (outSize)
			*outSize = it->second;
		return true;
	}
};

// Build a tiny FST image via FstBuilder: /a.bin (100), /dir/b.arc (200).
static bool MakeFst(Fst &fst, std::vector<u8> &img)
{
	FstBuilder b;
	// Parse empty? Easier: construct via AddOrReplace on empty parsed?
	// FstBuilder::Parse needs an image; instead build image by hand:
	// use FstBuilder on a minimal valid image from test helpers? Simplest:
	// serialize a builder tree starting from a 1-entry root.
	//
	// Minimal valid FST: root + 2 files + 1 dir. Hand-encode via FstBuilder
	// by parsing a known-good image? Instead use FstBuilder directly:
	// Parse() an empty root image, then AddOrReplace + Layout + Serialize,
	// then re-parse as Fst for plan input. The plan consumes Fst (parsed),
	// not the builder, so sizes/offsets come from the serialized image.
	u8 rootImg[12] = {1, 0,0,0, 0,0,0,0, 0,0,0,1};
	// root entry: type dir, nameoff 0, parent 0, next 1; string table "\0"
	u8 full[13];
	memcpy(full, rootImg, 12);
	full[12] = 0;
	if (!b.Parse(full, 13, false))
		return false;
	bool isNew = false;
	if (!b.AddOrReplace("/a.bin", 100, &isNew))
		return false;
	if (!b.AddOrReplace("/dir/b.arc", 200, &isNew))
		return false;
	b.Layout(0x1000, 32);
	b.Serialize(img, false);
	return fst.Parse(img.empty() ? 0 : &img[0], (u32)img.size(), false);
}

static ResolvedFile RF(const char *disc, const char *ext, const char *root = "/riivolution",
					   u32 off = 0, u32 foff = 0, u32 len = 0,
					   bool resize = true, bool create = false)
{
	ResolvedFile r;
	r.root = root;
	r.disc = disc;
	r.external = ext;
	r.resize = resize;
	r.create = create;
	r.offset = off;
	r.fileoffset = foff;
	r.length = len;
	return r;
}

int main()
{
	// 1. Whole-file replacement: finalSize == external, single segment.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/a.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/a.bin", 150);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.errors.empty());
		CHECK(!plan.hasBootFile && !plan.hasPartial);
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			CHECK(plan.files[0].finalSize == 150);
			CHECK(plan.files[0].wholeFile);
		}
	}
	// 2. Partial: offset/length => multi-segment, hasPartial, final honors resize.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/p.bin", "/riivolution", 40, 10, 20, true));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/p.bin", 100);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.errors.empty());
		CHECK(plan.hasPartial);
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			// orig 100, patch [40,60) from ext[10,30): segs orig[0,40),
			// ext 20, orig[60,100); final 100 (resize true, end 60 < 100? no:
			// patch_end 60, resize true => target 60? Wait orig 100, patch
			// [40,60): target = patch_end 60 when resize => truncate to 60.
			// That matches Dolphin (resize true truncates).
			CHECK(plan.files[0].finalSize == 60);
			CHECK(!plan.files[0].wholeFile);
			CHECK(plan.files[0].segs.size() == 2); // orig head + ext (tail truncated)
		}
	}
	// 3. resize=false preserves trailing original.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/p.bin", "/riivolution", 40, 10, 20, false));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/p.bin", 100);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			CHECK(plan.files[0].finalSize == 100);
			CHECK(plan.files[0].segs.size() == 3);
		}
	}
	// 4. Multiple mods to one file compose in order (second overwrites middle).
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/one.bin"));
		set.files.push_back(RF("/a.bin", "/r/two.bin", "/riivolution", 10, 0, 10, false));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/one.bin", 100);
		sizes.Put("sd:/r/two.bin", 50);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			// First: whole 100 from one.bin. Second: [10,20) from two.bin[0,10).
			// Final 100, segs: ext one[0,10), ext two[0,10), ext one[20,100).
			CHECK(plan.files[0].finalSize == 100);
			CHECK(plan.files[0].segs.size() == 3);
		}
	}
	// 5. Duplicate whole-file precedence: last wins.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/one.bin"));
		set.files.push_back(RF("/a.bin", "/r/two.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/one.bin", 100);
		sizes.Put("sd:/r/two.bin", 120);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			CHECK(plan.files[0].finalSize == 120);
			CHECK(plan.files[0].wholeFile);
			CHECK(plan.files[0].external == "sd:/r/two.bin");
		}
	}
	// 6. Creation: no disc entry + create => new file, whole.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/new/c.bin", "/r/c.bin", "/riivolution", 0, 0, 0, true, true));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/c.bin", 64);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			CHECK(plan.files[0].isNew);
			CHECK(plan.files[0].finalSize == 64);
		}
	}
	// 7. Missing external => skipped, original kept, missing recorded.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/gone.bin"));
		MemLister lister;
		MemSizes sizes; // empty: missing
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.empty());
		CHECK(plan.missingExternals.size() == 1);
	}
	// 8. Zero-length external => zero-length file, valid, no fragments.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/empty.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/empty.bin", 0);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
			CHECK(plan.files[0].finalSize == 0);
	}
	// 9. main.dol without an executable sink => boot-file error (legacy
	// callers keep prior behavior; production passes a DOL sink below).
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("main.dol", "/r/main.dol"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/main.dol", 1000);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.hasBootFile);
		CHECK(!plan.errors.empty());
		CHECK(plan.files.empty());
	}
	// 10. Dataless folder: empty disc matches by basename.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		ResolvedFolder fl;
		fl.root = "/riivolution";
		fl.disc = "";
		fl.external = "newer";
		fl.resize = true;
		fl.create = false;
		fl.recursive = true;
		fl.length = 0;
		set.folders.push_back(fl);
		MemLister lister;
		lister.Add("sd:/riivolution/newer", "b.arc");
		lister.Add("sd:/riivolution/newer", "nomatch.bin");
		MemSizes sizes;
		sizes.Put("sd:/riivolution/newer/b.arc", 200);
		sizes.Put("sd:/riivolution/newer/nomatch.bin", 10);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		// b.arc matches /dir/b.arc by basename; nomatch skipped (no create).
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
			CHECK(plan.files[0].disc == "/dir/b.arc");
	}
	// 11. Absolute external ignores patch root (device-root-relative).
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/abs/a.bin", "/mods/ssmg"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/abs/a.bin", 100);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
			CHECK(plan.files[0].wholeFile);
		CHECK(plan.missingExternals.empty());
	}
	// 12. Offset low bits ignored (offset & ~3).
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/a.bin", "/r/p.bin", "/riivolution", 41, 0, 4, true));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/p.bin", 100);
		PatchPlan plan;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why));
		CHECK(plan.files.size() == 1);
		if (plan.files.size() == 1)
		{
			// 41 & ~3 == 40: segs orig[0,40), ext 4, then truncate to 44.
			CHECK(plan.files[0].finalSize == 44);
		}
	}

	// 13. DolImageSize: max section end, 0x100 floor.
	{
		u32 offs[3] = {0x100, 0x500, 0};
		u32 sizes[3] = {0x200, 0x100, 0};
		CHECK(DolImageSize(offs, sizes, 3) == 0x600);
		CHECK(DolImageSize(0, 0, 0) == 0x100);
		CHECK(DolImageSize(offs, sizes, 0) == 0x100);
	}
	// 14. Partial DOL patch composes against the image base, preserves size,
	// does not set file-partial, carries no FST effect.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("main.dol", "/r/dolpatch.bin", "/riivolution", 0x100, 4, 8, true));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/dolpatch.bin", 100);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(plan.errors.empty());
		CHECK(plan.hasBootFile);
		CHECK(!plan.hasPartial);
		CHECK(plan.files.empty()); // DOL never enters the FST plan
		CHECK(dol.bootFile);
		CHECK(dol.finalSize == 0x1000);
		CHECK(dol.discLengthOrig == 0x1000);
		CHECK(dol.segs.size() == 3); // orig head, ext, orig tail
		if (dol.segs.size() == 3)
		{
			CHECK(dol.segs[0].kind == PlanSegment::SEG_ORIGINAL && dol.segs[0].length == 0x100);
			CHECK(dol.segs[1].kind == PlanSegment::SEG_EXTERNAL && dol.segs[1].length == 8
				  && dol.segs[1].srcOffset == 4);
			CHECK(dol.segs[2].kind == PlanSegment::SEG_ORIGINAL
				  && dol.segs[2].fileOffset == 0x108);
		}
	}
	// 15. DOL patch growing past the image end is refused (size rule).
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("main.dol", "/r/dolpatch.bin", "/riivolution", 0xFF0, 0, 0x20, true));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/dolpatch.bin", 0x100);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(!plan.errors.empty()); // 0xFF0+0x20 = 0x1010 != 0x1000
	}
	// 16. Whole-file DOL replacement, same size: allowed, wholeFile.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("MAIN.DOL", "/r/newdol.bin")); // case-insensitive
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/newdol.bin", 0x1000);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(plan.errors.empty());
		CHECK(dol.wholeFile);
		CHECK(dol.external == "sd:/r/newdol.bin");
	}
	// 17. Whole-file DOL replacement, different size: refused.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("main.dol", "/r/newdol.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/newdol.bin", 0x2000);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(!plan.errors.empty());
	}
	// 18. Missing DOL external: skipped like file-missing, no error.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("main.dol", "/r/gone.bin"));
		set.files.push_back(RF("/a.bin", "/r/a.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/a.bin", 100);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(plan.errors.empty());
		CHECK(plan.missingExternals.size() == 1);
		CHECK(plan.files.size() == 1); // /a.bin still planned
		CHECK(!dol.bootFile); // untouched: no DOL serving
	}
	// 19. Dataless folder child naming main.dol routes to the DOL plan.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		ResolvedFolder fl;
		fl.root = "/riivolution";
		fl.disc = "";
		fl.external = "newer";
		fl.resize = true;
		fl.create = false;
		fl.recursive = true;
		fl.length = 0;
		set.folders.push_back(fl);
		MemLister lister;
		lister.Add("sd:/riivolution/newer", "main.dol");
		MemSizes sizes;
		sizes.Put("sd:/riivolution/newer/main.dol", 0x1000);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(plan.errors.empty());
		CHECK(dol.bootFile && dol.wholeFile);
		CHECK(plan.files.empty());
	}

	// 20. Absolute "/main.dol" is an FST path, never the executable
	// (Dolphin-exact): no such file on disc, no create => skipped silently.
	{
		Fst fst;
		std::vector<u8> img;
		CHECK(MakeFst(fst, img));
		ResolvedPatchSet set;
		set.files.push_back(RF("/main.dol", "/r/rootmain.bin"));
		MemLister lister;
		MemSizes sizes;
		sizes.Put("sd:/r/rootmain.bin", 64);
		PatchPlan plan;
		PlannedFile dol;
		std::string why;
		CHECK(BuildPatchPlan(fst, set, "sd:", &lister, &sizes, plan, why, 0x1000, &dol));
		CHECK(plan.errors.empty());
		CHECK(!plan.hasBootFile);
		CHECK(plan.files.empty());
		CHECK(!dol.bootFile);
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
