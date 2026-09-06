/****************************************************************************
 * WP1 fixtures: option defaults, revision/disc filters, multi-XML merge,
 * and patch-reference accounting.
 *
 * Built programmatically (no pugixml needed): what matters here is the
 * resolver contract, not the XML text parser. Malformed-XML text cases are
 * covered by ParseFile's error strings and stay on the target side.
 ***************************************************************************/
#include <cstdio>
#include <string>
#include <vector>

#include "riivo/RiivoConfig.hpp"

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

static Riivo::Disc DiscWithVersionFilter(int version)
{
	Riivo::Disc d;
	d.filter.hasVersion = true;
	d.filter.version = version;
	return d;
}

static void TestUnknownFilters()
{
	Riivo::Disc d = DiscWithVersionFilter(2);
	check(d.IsValidForGame("SB4E01", 0, Riivo::RIIVO_REVISION_UNKNOWN),
		  "unknown revision skips the version check");
	check(!d.IsValidForGame("SB4E01", 0, 0),
		  "revision 0 does not match a version=2 filter");
	check(d.IsValidForGame("SB4E01", 0, 2), "exact revision matches");
	check(!d.IsValidForGame("SB4E01", 0, 3), "wrong revision refused");

	Riivo::Disc dd;
	dd.filter.hasDisc = true;
	dd.filter.disc = 1;
	check(dd.IsValidForGame("SB4E01", Riivo::RIIVO_DISC_UNKNOWN, 0),
		  "unknown disc skips the disc check");
	check(!dd.IsValidForGame("SB4E01", 0, 0),
		  "disc 0 does not match a disc=1 filter");
	check(dd.IsValidForGame("SB4E01", 1, 0), "exact disc matches");

	// Unrelated filters still enforced when one axis is unknown.
	Riivo::Disc g;
	g.filter.hasGame = true;
	g.filter.game = "SB4";
	g.filter.hasVersion = true;
	g.filter.version = 2;
	check(!g.IsValidForGame("SMN001", 0, Riivo::RIIVO_REVISION_UNKNOWN),
		  "game mismatch refused even with unknown revision");
	check(!g.IsValidForGame("SHORT", 0, Riivo::RIIVO_REVISION_UNKNOWN),
		  "short game id refused");

	Riivo::Disc r;
	r.filter.regions.push_back("E");
	check(r.IsValidForGame("SB4E01", Riivo::RIIVO_DISC_UNKNOWN,
						   Riivo::RIIVO_REVISION_UNKNOWN),
		  "region match passes with everything else unknown");
	check(!r.IsValidForGame("SB4J01", Riivo::RIIVO_DISC_UNKNOWN,
							Riivo::RIIVO_REVISION_UNKNOWN),
		  "region mismatch refused");
}

static void TestMerge()
{
	Riivo::Disc a, b, m;
	a.root = "/riivolution";
	a.xmlPath = "a.xml";

	Riivo::Patch pa;
	pa.id = "p";
	Riivo::File fa;
	fa.disc = "/a";
	fa.external = "ea";
	pa.files.push_back(fa);

	Riivo::Patch pb;
	pb.id = "p";
	Riivo::File fb;
	fb.disc = "/b";
	fb.external = "eb";
	pb.files.push_back(fb);

	Riivo::Patch qb;
	qb.id = "q";

	a.patches.push_back(pa);
	b.patches.push_back(pb);
	b.patches.push_back(qb);
	b.xmlPath = "b.xml";

	Riivo::Section sa;
	sa.name = "A";
	Riivo::Section sb;
	sb.name = "B";
	a.sections.push_back(sa);
	b.sections.push_back(sb);

	Riivo::MergeDiscs(a, b, m);
	check(m.patches.size() == 2, "merge keeps one entry per patch id");
	check(m.patches[0].files[0].disc == "/b", "later XML wins the patch id");
	check(m.patches[1].id == "q", "new patch id appended");
	check(m.sections.size() == 2 && m.sections[0].name == "A"
		  && m.sections[1].name == "B", "sections concatenate in order");
	check(m.root == "/riivolution", "merged root comes from the first XML");
	check(m.xmlPath == "a.xml;b.xml", "merged xmlPath names both sources");

	// Merging with an empty second disc is the identity.
	Riivo::Disc e, m2;
	Riivo::MergeDiscs(a, e, m2);
	check(m2.patches.size() == a.patches.size()
		  && m2.sections.size() == a.sections.size(),
		  "merge with an empty disc changes nothing");
}

static Riivo::Disc DiscWithBadRef()
{
	Riivo::Disc r;
	Riivo::Section rs;
	Riivo::Option ro;
	Riivo::Choice rc;
	Riivo::PatchRef bad;
	bad.id = "missing";
	rc.patchRefs.push_back(bad);
	ro.choices.push_back(rc);
	ro.selectedChoice = 1;
	rs.options.push_back(ro);
	r.sections.push_back(rs);
	return r;
}

static void TestSkippedRefs()
{
	Riivo::Disc r = DiscWithBadRef();
	Riivo::ResolvedPatchSet set;
	Riivo::ResolveStats stats;
	Riivo::ResolveWithStats(r, "SB4E01", set, stats);
	check(stats.skippedPatchRefs == 1, "missing patch id counted");
	check(set.IsEmpty(), "nothing resolved from a missing id");

	// The legacy entry point behaves identically minus the count.
	Riivo::ResolvedPatchSet set2;
	Riivo::Resolve(r, "SB4E01", set2);
	check(set2.IsEmpty(), "legacy Resolve still skips silently");
}

static void TestGoodRefResolves()
{
	Riivo::Disc d;
	Riivo::Patch p;
	p.id = "p";
	p.root = "/root";
	Riivo::File f;
	f.disc = "/disc/a.bin";
	f.external = "ext/a.bin";
	f.resize = true;
	p.files.push_back(f);
	d.patches.push_back(p);

	Riivo::Section s;
	Riivo::Option o;
	Riivo::Choice c;
	Riivo::PatchRef ref;
	ref.id = "p";
	c.patchRefs.push_back(ref);
	o.choices.push_back(c);
	o.selectedChoice = 1;
	s.options.push_back(o);
	d.sections.push_back(s);

	Riivo::ResolvedPatchSet set;
	Riivo::ResolveStats stats;
	Riivo::ResolveWithStats(d, "SB4E01", set, stats);
	check(stats.skippedPatchRefs == 0, "valid ref is not counted as skipped");
	check(set.files.size() == 1, "valid ref resolves one file");
	if (!set.files.empty())
	{
		check(set.files[0].root == "/root", "per-patch root carried through");
		check(set.files[0].disc == "/disc/a.bin", "disc path substituted");
	}

	// Disabled options (selectedChoice 0) resolve nothing.
	d.sections[0].options[0].selectedChoice = 0;
	Riivo::ResolvedPatchSet off;
	Riivo::ResolveStats offStats;
	Riivo::ResolveWithStats(d, "SB4E01", off, offStats);
	check(off.IsEmpty() && offStats.skippedPatchRefs == 0,
		  "disabled option resolves nothing");
}

static void TestSelectionRoundTrip()
{
	Riivo::Disc d;
	Riivo::Section s;
	Riivo::Option o1, o2;
	Riivo::Choice c;
	o1.choices.push_back(c);
	o1.choices.push_back(c);
	o1.selectedChoice = 2;
	o2.choices.push_back(c);
	o2.selectedChoice = 0;
	s.options.push_back(o1);
	s.options.push_back(o2);
	d.sections.push_back(s);

	std::string ser = Riivo::SerializeSelection(d);
	check(ser == "0=2,1=0", "selection serialises in flattened order");

	Riivo::Disc d2 = d;
	d2.sections[0].options[0].selectedChoice = 0;
	Riivo::ApplySelection(d2, ser);
	check(d2.sections[0].options[0].selectedChoice == 2
		  && d2.sections[0].options[1].selectedChoice == 0,
		  "selection round-trips");

	// Out-of-range and garbage tokens clamp rather than corrupt.
	Riivo::ApplySelection(d2, "0=9,7=1,zzz");
	check(d2.sections[0].options[0].selectedChoice == 0,
		  "over-range choice clamps to disabled");
}

int main()
{
	TestUnknownFilters();
	TestMerge();
	TestSkippedRefs();
	TestGoodRefResolves();
	TestSelectionRoundTrip();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
