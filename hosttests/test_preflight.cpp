/****************************************************************************
 * The pre-launch check for files a mod names but the card does not have.
 *
 * Why this exists: a diagnostic pack shipped with placeholder paths and
 * empty test folders produced a boot log that said "mod files listed: 0
 * found" and named nothing at all. That reads exactly like a loader that
 * cannot see the card, and telling the two apart cost a full round of
 * hardware tests on a remote console. FindMissingExternals is the check
 * that names them while the GUI is still up and the card still mounted.
 *
 * The existence test is injected, so every case here runs with no
 * filesystem at all - which is also what lets the awkward ones (a path
 * that differs only in its root, a patch with no disc path) be provoked
 * deliberately rather than waited for.
 ***************************************************************************/
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "riivo/RiivoFile.hpp"

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

//! Stands in for the card: only the paths handed to it exist.
struct FakeCard : public Riivo::FileProbe
{
	std::set<std::string> present;
	int queries;

	FakeCard() : queries(0) {}
	void Add(const std::string &p) { present.insert(p); }
	virtual bool IsRegularFile(const std::string &path)
	{
		++queries;
		return present.find(path) != present.end();
	}
};

static Riivo::ResolvedFile MakeFile(const char *disc, const char *root,
									const char *external)
{
	Riivo::ResolvedFile f;
	f.disc = disc;
	f.root = root;
	f.external = external;
	return f;
}

//! The exact shape that produced the useless log: an unconfigured pack.
//! The XML still holds its placeholder disc path and the folder holds only
//! a "drop your file here" note, so the external is genuinely absent.
static void TestUnconfiguredPack()
{
	Riivo::ResolvedPatchSet set;
	set.files.push_back(MakeFile("/GXPROBE/FILE.ARC", "/riivolution/gxdiag",
								 "T1/probe.arc"));
	FakeCard card;

	std::vector<Riivo::MissingExternal> missing;
	Riivo::FindMissingExternals(set, "usb1:", card, missing);

	check(missing.size() == 1, "the missing probe file is reported");
	if (missing.size() == 1)
	{
		check(missing[0].external == "usb1:/riivolution/gxdiag/T1/probe.arc",
			  "and named by the full card path that was tried");
		check(missing[0].disc == "/gxprobe/file.arc",
			  "with the disc path it was meant to replace");
	}
	check(card.queries == 1, "one probe per <file> patch, no repeats");
}

//! A file that is present must not be reported. The point of naming paths
//! is lost if working mods collect warnings too.
static void TestPresentFilesAreQuiet()
{
	Riivo::ResolvedPatchSet set;
	set.files.push_back(MakeFile("/a.bin", "/mod", "T0/a.bin"));
	set.files.push_back(MakeFile("/b.bin", "/mod", "T0/b.bin"));
	FakeCard card;
	card.Add("usb1:/mod/T0/a.bin");
	card.Add("usb1:/mod/T0/b.bin");

	std::vector<Riivo::MissingExternal> missing;
	Riivo::FindMissingExternals(set, "usb1:", card, missing);
	check(missing.empty(), "a fully unpacked mod reports nothing");
}

//! The case that actually happened: some patches resolve, some do not.
//! Reporting only "0 found" for the whole boot hid exactly this.
static void TestPartialPack()
{
	Riivo::ResolvedPatchSet set;
	set.files.push_back(MakeFile("/probe.arc", "/mod", "T1/probe.arc"));
	set.files.push_back(MakeFile("/dummy.bin", "/mod", "T3/dummy.bin"));
	FakeCard card;
	card.Add("usb1:/mod/T3/dummy.bin");

	std::vector<Riivo::MissingExternal> missing;
	Riivo::FindMissingExternals(set, "usb1:", card, missing);
	check(missing.size() == 1, "only the absent one is reported");
	if (missing.size() == 1)
		check(missing[0].external == "usb1:/mod/T1/probe.arc",
			  "and it is the right one");
}

//! Paths are joined the same way enumeration joins them, or the check names
//! a path the boot never tried - worse than saying nothing.
static void TestPathJoining()
{
	{
		//! Device with no trailing slash, root with a leading one.
		Riivo::ResolvedPatchSet set;
		set.files.push_back(MakeFile("/x", "/mod", "a.bin"));
		FakeCard card;
		std::vector<Riivo::MissingExternal> missing;
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.size() == 1 && missing[0].external == "sd:/mod/a.bin",
			  "device + root + relative join once, with one slash");
	}
	{
		//! Empty root: the external is relative to the device itself.
		Riivo::ResolvedPatchSet set;
		set.files.push_back(MakeFile("/x", "", "/mod/a.bin"));
		FakeCard card;
		std::vector<Riivo::MissingExternal> missing;
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.size() == 1 && missing[0].external == "sd:/mod/a.bin",
			  "an empty root does not insert a stray slash");
	}
	{
		//! The same file under two roots is two different paths, and only
		//! the one that is absent may be reported.
		Riivo::ResolvedPatchSet set;
		set.files.push_back(MakeFile("/x", "/modA", "a.bin"));
		set.files.push_back(MakeFile("/y", "/modB", "a.bin"));
		FakeCard card;
		card.Add("sd:/modA/a.bin");
		std::vector<Riivo::MissingExternal> missing;
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.size() == 1 && missing[0].external == "sd:/modB/a.bin",
			  "roots are not conflated");
	}
}

//! Two patches naming the same absent file are two things to fix, so they
//! are two lines. Collapsing them would under-report the work.
static void TestDuplicatesKept()
{
	Riivo::ResolvedPatchSet set;
	set.files.push_back(MakeFile("/x", "/mod", "probe.arc"));
	set.files.push_back(MakeFile("/y", "/mod", "probe.arc"));
	FakeCard card;
	std::vector<Riivo::MissingExternal> missing;
	Riivo::FindMissingExternals(set, "sd:", card, missing);
	check(missing.size() == 2, "both claims on one absent file are reported");
}

//! <folder> rules are deliberately not checked here: their contents come
//! from listing the directory, which needs the card. Reporting the folder
//! as a missing file would name a path that is not a file at all.
static void TestFoldersIgnored()
{
	Riivo::ResolvedPatchSet set;
	Riivo::ResolvedFolder d;
	d.disc = "/stage";
	d.root = "/mod";
	d.external = "stage";
	d.recursive = true;
	set.folders.push_back(d);
	FakeCard card;

	std::vector<Riivo::MissingExternal> missing;
	Riivo::FindMissingExternals(set, "sd:", card, missing);
	check(missing.empty(), "a <folder> rule is not reported as a missing file");
	check(card.queries == 0, "and the card is not probed for it");
}

//! Refusals and edges: nothing to check, and a patch with no disc path.
static void TestEdges()
{
	{
		Riivo::ResolvedPatchSet set;
		FakeCard card;
		std::vector<Riivo::MissingExternal> missing;
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.empty(), "an empty set reports nothing");
	}
	{
		//! No disc path is a malformed patch, not a missing file. Naming a
		//! card path for it would send the user to the wrong place.
		Riivo::ResolvedPatchSet set;
		set.files.push_back(MakeFile("", "/mod", "a.bin"));
		FakeCard card;
		std::vector<Riivo::MissingExternal> missing;
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.empty(), "a patch with no disc path is not reported");
		check(card.queries == 0, "and the card is not probed for it");
	}
	{
		//! The output vector is cleared, so a caller reusing it cannot
		//! inherit another mod's names.
		Riivo::ResolvedPatchSet set;
		set.files.push_back(MakeFile("/x", "/mod", "a.bin"));
		FakeCard card;
		card.Add("sd:/mod/a.bin");
		std::vector<Riivo::MissingExternal> missing;
		Riivo::MissingExternal stale;
		stale.external = "sd:/stale";
		missing.push_back(stale);
		Riivo::FindMissingExternals(set, "sd:", card, missing);
		check(missing.empty(), "a reused output vector is cleared first");
	}
}

int main()
{
	TestUnconfiguredPack();
	TestPresentFilesAreQuiet();
	TestPartialPack();
	TestPathJoining();
	TestDuplicatesKept();
	TestFoldersIgnored();
	TestEdges();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
