/****************************************************************************
 * Two-phase reconciliation: early registration against late placement.
 *
 * The Newer SMBW failure was an interaction, not a unit fault: files
 * registered from the card before the partition opened did not all survive
 * into the rebuilt table, and a tail-recovered offset among the dropped
 * ones withheld the whole mod anonymously. These fixtures drive the pure
 * seam (records -> skips -> matches) the way the two boot phases do,
 * including the MP9-shaped case that must keep working.
 *
 * Byte verification itself runs on the target through VerifyModFragment;
 * what is proven here is routing: every recovered offset resolves to an
 * ACTIVE placed entry, an INACTIVE record with a named reason, or UNKNOWN
 * (refuse), and no placed offset lacks a record.
 ***************************************************************************/
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "riivo/RiivoReconcile.hpp"
#include "riivo/RiivoFstBuild.hpp"
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

static Riivo::RegRecord Rec(const char *disc, const char *ext, u64 off, u32 len)
{
	Riivo::RegRecord r;
	r.disc = disc;
	r.external = ext;
	r.offset = off;
	r.length = len;
	return r;
}

static std::vector<u64> Offsets(const std::vector<Riivo::RegRecord> &records)
{
	std::vector<u64> out;
	for (size_t i = 0; i < records.size(); ++i)
		out.push_back(records[i].offset);
	return out;
}

//! MP9 shape: everything registered lands in the table; recovered files
//! resolve ACTIVE to the right placed entries, nothing is skipped, and no
//! placed offset lacks a record.
static void TestAllMatched()
{
	std::vector<Riivo::RegRecord> records;
	records.push_back(Rec("/a.arc", "usb1:/mod/a.arc", 0x180000000ULL, 100));
	records.push_back(Rec("/b.arc", "usb1:/mod/b.arc", 0x180008000ULL, 200));
	records.push_back(Rec("/c.arc", "usb1:/mod/c.arc", 0x180010000ULL, 300));

	std::vector<Riivo::SkipRecord> skips;
	std::map<std::string, char> hasRedirect;
	hasRedirect["/a.arc"] = 1;
	hasRedirect["/b.arc"] = 1;
	hasRedirect["/c.arc"] = 1;
	std::map<std::string, Riivo::SkipReason> addFails;
	Riivo::FindSkips(records, Offsets(records), 0x180000000ULL,
					 hasRedirect, addFails, skips);
	check(skips.empty(), "all-matched mod has no skips");

	std::vector<u64> recovered;
	recovered.push_back(0x180008000ULL);
	recovered.push_back(0x180010000ULL);
	std::vector<Riivo::RecMatch> matches;
	std::vector<u64> unregistered;
	Riivo::ReconcileRecovered(Offsets(records), records, skips, recovered,
							  matches, unregistered);
	check(unregistered.empty(), "all-matched mod has no unregistered offsets");
	check(matches.size() == 2, "one match per recovered offset");
	check(matches[0].state == Riivo::REC_ACTIVE && matches[0].index == 1,
		  "recovered b resolves ACTIVE to its placed entry");
	check(matches[1].state == Riivo::REC_ACTIVE && matches[1].index == 2,
		  "recovered c resolves ACTIVE to its placed entry");
}

//! Newer shape: a recovered candidate dropped late resolves INACTIVE with a
//! named reason instead of withholding anonymously; an unlisted offset is
//! UNKNOWN; unregistered placed offsets are reported.
static void TestNewerShape()
{
	std::vector<Riivo::RegRecord> records;
	records.push_back(Rec("/a.arc", "usb1:/mod/a.arc", 0x180000000ULL, 100));
	records.push_back(Rec("/b.arc", "usb1:/mod/b.arc", 0x180008000ULL, 200));
	records.push_back(Rec("/gone.arc", "usb1:/mod/gone.arc", 0x180010000ULL, 300));
	records.push_back(Rec("/conflict.arc", "usb1:/mod/conflict.arc", 0x180018000ULL, 400));

	// Late list holds a, b and conflict; gone.arc never matched the disc and
	// conflict.arc's table entry was refused.
	std::vector<u64> placed;
	placed.push_back(0x180000000ULL);
	placed.push_back(0x180008000ULL);
	placed.push_back(0x180018000ULL);

	std::map<std::string, char> hasRedirect;
	hasRedirect["/a.arc"] = 1;
	hasRedirect["/b.arc"] = 1;
	hasRedirect["/conflict.arc"] = 1;
	std::map<std::string, Riivo::SkipReason> addFails;
	addFails["/conflict.arc"] = Riivo::SKIP_ADD_FAILED;

	std::vector<Riivo::SkipRecord> skips;
	Riivo::FindSkips(records, placed, 0x180000000ULL,
					 hasRedirect, addFails, skips);
	check(skips.size() == 1, "one skip for the unmatched file");
	check(skips[0].disc == "/gone.arc"
		  && skips[0].reason == Riivo::SKIP_NO_REDIRECT,
		  "unmatched file skipped as NO_REDIRECT");

	std::vector<u64> recovered;
	recovered.push_back(0x180008000ULL); // active
	recovered.push_back(0x180010000ULL); // dropped late
	recovered.push_back(0x190000000ULL); // never registered
	std::vector<Riivo::RecMatch> matches;
	std::vector<u64> unregistered;
	Riivo::ReconcileRecovered(placed, records, skips, recovered,
							  matches, unregistered);
	check(unregistered.empty(), "no unregistered placed offsets here");
	check(matches.size() == 3, "one match per recovered offset");
	check(matches[0].state == Riivo::REC_ACTIVE && matches[0].index == 1,
		  "active recovered file routes to its placed entry for read-back");
	check(matches[1].state == Riivo::REC_INACTIVE
		  && matches[1].reason == Riivo::SKIP_NO_REDIRECT,
		  "dropped recovered file routes INACTIVE with its reason");
	check(records[matches[1].index].external == std::string("usb1:/mod/gone.arc"),
		  "INACTIVE match retains the source record for verification");
	check(matches[2].state == Riivo::REC_UNKNOWN,
		  "never-registered offset is UNKNOWN and must refuse");
}

//! A placed offset with no registration record must fail activation: the
//! table would reference bytes nothing registered.
static void TestUnregisteredPlaced()
{
	std::vector<Riivo::RegRecord> records;
	records.push_back(Rec("/a.arc", "usb1:/mod/a.arc", 0x180000000ULL, 100));
	std::vector<u64> placed;
	placed.push_back(0x180000000ULL);
	placed.push_back(0x180008000ULL); // no record claims this
	std::vector<Riivo::SkipRecord> skips;
	std::vector<u64> recovered;
	std::vector<Riivo::RecMatch> matches;
	std::vector<u64> unregistered;
	Riivo::ReconcileRecovered(placed, records, skips, recovered,
							  matches, unregistered);
	check(unregistered.size() == 1
		  && unregistered[0] == 0x180008000ULL,
		  "unregistered placed offset is reported");
}

//! Zero-length files share the cursor without advancing it: they are
//! reported as skips, never mistaken for the active file at the same
//! offset, and an empty recovered list reconciles cleanly.
static void TestZeroLength()
{
	std::vector<Riivo::RegRecord> records;
	records.push_back(Rec("/empty.arc", "usb1:/mod/empty.arc", 0x180000000ULL, 0));
	records.push_back(Rec("/full.arc", "usb1:/mod/full.arc", 0x180000000ULL, 512));
	std::vector<u64> placed;
	placed.push_back(0x180000000ULL);
	std::map<std::string, char> hasRedirect;
	hasRedirect["/empty.arc"] = 1;
	hasRedirect["/full.arc"] = 1;
	std::map<std::string, Riivo::SkipReason> addFails;
	std::vector<Riivo::SkipRecord> skips;
	Riivo::FindSkips(records, placed, 0x180000000ULL,
					 hasRedirect, addFails, skips);
	check(skips.size() == 1 && skips[0].reason == Riivo::SKIP_ZERO_LENGTH,
		  "zero-length file skipped before offset membership is tested");

	std::vector<u64> recovered;
	recovered.push_back(0x180000000ULL);
	std::vector<Riivo::RecMatch> matches;
	std::vector<u64> unregistered;
	Riivo::ReconcileRecovered(placed, records, skips, recovered,
							  matches, unregistered);
	check(matches.size() == 1 && matches[0].state == Riivo::REC_ACTIVE,
		  "shared offset resolves ACTIVE, not to the empty file");
}

//! Every remaining reason, driven directly.
static void TestReasons()
{
	std::vector<Riivo::RegRecord> records;
	records.push_back(Rec("/stat.arc", "usb1:/mod/stat.arc", 0x180000000ULL, 100));
	records.push_back(Rec("/low.arc", "usb1:/mod/low.arc", 0x1000ULL, 100));
	records.push_back(Rec("/odd.arc", "usb1:/mod/odd.arc", 0x180008000ULL, 100));
	std::vector<u64> placed; // nothing landed
	std::map<std::string, char> hasRedirect;
	hasRedirect["/stat.arc"] = 1;
	hasRedirect["/low.arc"] = 1;
	hasRedirect["/odd.arc"] = 1;
	std::map<std::string, Riivo::SkipReason> addFails;
	addFails["/stat.arc"] = Riivo::SKIP_STAT_FAILED;
	std::vector<Riivo::SkipRecord> skips;
	Riivo::FindSkips(records, placed, 0x180000000ULL,
					 hasRedirect, addFails, skips);
	check(skips.size() == 3, "all three inactive records reported");
	bool stat = false, low = false, odd = false;
	for (size_t i = 0; i < skips.size(); ++i)
	{
		if (skips[i].disc == "/stat.arc")
			stat = (skips[i].reason == Riivo::SKIP_STAT_FAILED);
		if (skips[i].disc == "/low.arc")
			low = (skips[i].reason == Riivo::SKIP_BELOW_REGION);
		if (skips[i].disc == "/odd.arc")
			odd = (skips[i].reason == Riivo::SKIP_UNASSIGNED);
	}
	check(stat, "late stat failure named");
	check(low, "below-region assignment named");
	check(odd, "redirect without assignment named as inconsistency");
	check(Riivo::SkipReasonText(Riivo::SKIP_NO_REDIRECT) != 0
		  && Riivo::SkipReasonText(Riivo::SKIP_ADD_FAILED) != 0
		  && Riivo::SkipReasonText(Riivo::SKIP_STAT_FAILED) != 0
		  && Riivo::SkipReasonText(Riivo::SKIP_ZERO_LENGTH) != 0
		  && Riivo::SkipReasonText(Riivo::SKIP_BELOW_REGION) != 0
		  && Riivo::SkipReasonText(Riivo::SKIP_UNASSIGNED) != 0,
		  "every reason has log text");
}

//! Overlapping folder rules claiming one disc path: the table keeps the
//! last claim, and the reconcile side agrees through the same key.
static void Wr32(std::vector<u8> &v, size_t at, u32 x)
{
	v[at] = (u8) (x >> 24);
	v[at + 1] = (u8) (x >> 16);
	v[at + 2] = (u8) (x >> 8);
	v[at + 3] = (u8) x;
}

static void TestLastWins()
{
	// Root (2 entries total) + one file, shifted Wii offsets.
	std::vector<u8> fst(2 * 12, 0);
	std::string strings;
	strings += '\0'; // root name
	strings += "a.arc"; strings += '\0';
	fst[0] = 1; // root dir
	fst[1] = 0; fst[2] = 0; fst[3] = 0; // name offset 0
	Wr32(fst, 4, 0); // parent 0
	Wr32(fst, 8, 2); // end 2
	fst[12] = 0; // file
	fst[13] = 0; fst[14] = 0; fst[15] = 1; // name offset 1
	Wr32(fst, 16, 0x1000 >> 2);
	Wr32(fst, 20, 0x800);
	fst.insert(fst.end(), strings.begin(), strings.end());

	Riivo::FstBuilder builder;
	check(builder.Parse(&fst[0], (u32) fst.size(), true), "sample parses");
	bool isNew = false;
	check(builder.AddOrReplace("/dup.arc", 100, &isNew) && isNew,
		  "first claim adds");
	check(builder.AddOrReplace("/DUP.ARC", 200, &isNew) && !isNew,
		  "second claim replaces case-insensitively");
	u64 off = 0;
	u32 len = 0;
	check(builder.FindAssigned("/dup.arc", &off, &len) && len == 200,
		  "winning claim is the last one");
}

//! A table whose string table has no leading root NUL (root name offset 0
//! points at the first real name) parses, and serialising it adds exactly
//! one byte: the root's empty name. Entry count, paths, offsets and lengths
//! are otherwise identical. This is the benign mechanism behind a rebuilt
//! table measuring one byte larger than the original with no added entries.
static void TestNoLeadingNul()
{
	// Root (2 entries) + one file; strings hold only "a.arc\0".
	std::vector<u8> fst(2 * 12, 0);
	std::string strings;
	strings += "a.arc"; strings += '\0';
	fst[0] = 1; // root dir
	fst[1] = 0; fst[2] = 0; fst[3] = 0; // root name offset 0
	Wr32(fst, 4, 0); // parent 0
	Wr32(fst, 8, 2); // end 2
	fst[12] = 0; // file
	fst[13] = 0; fst[14] = 0; fst[15] = 0; // name offset 0
	Wr32(fst, 16, 0x1000 >> 2);
	Wr32(fst, 20, 0x800);
	fst.insert(fst.end(), strings.begin(), strings.end());

	Riivo::FstBuilder parsed;
	check(parsed.Parse(&fst[0], (u32) fst.size(), true), "nul-less table parses");
	std::vector<u8> rebuilt;
	parsed.Serialize(rebuilt, true);
	check(rebuilt.size() == fst.size() + 1,
		  "serialise adds exactly the root NUL");
	Riivo::FstBuilder reparsed;
	check(reparsed.Parse(&rebuilt[0], (u32) rebuilt.size(), true),
		  "rebuilt table re-parses");
	u64 off = 0;
	u32 len = 0;
	check(reparsed.FindAssigned("/a.arc", &off, &len)
		  && off == 0x1000 && len == 0x800,
		  "file entry survives the round trip");
}

//! Previous-boot outcome parsing for the settings UI.
static void TestOutcome()
{
	bool live = false;
	std::string code;
	check(!Riivo::ParseBootOutcome("no outcome here\n", live, code),
		  "text without OUTCOME is not an outcome");
	check(Riivo::ParseBootOutcome("OUTCOME: FST_STAGED\n", live, code)
		  && live && code == "FST_STAGED", "FST_STAGED parses live");
	check(Riivo::ParseBootOutcome("OUTCOME: FILES_LIVE\n", live, code)
		  && live && code == "FILES_LIVE",
		  "old FILES_LIVE logs still parse live");
	check(Riivo::ParseBootOutcome("OUTCOME: WITHHELD READBACK\nOUTCOME: FST_STAGED\n",
								  live, code) && live,
		  "last OUTCOME line wins");
	check(Riivo::ParseBootOutcome("noise\nOUTCOME: WITHHELD READBACK\n", live, code)
		  && !live && code == "WITHHELD READBACK", "WITHHELD parses with stage");
	check(Riivo::ParseBootOutcome("OUTCOME: NO_FILE_WORK\r\n", live, code)
		  && !live && code == "NO_FILE_WORK",
		  "CRLF tolerated, NO_FILE_WORK is not live");
	check(Riivo::ParseBootOutcome("OUTCOME:\n", live, code) == false,
		  "empty OUTCOME is not an outcome");
}

int main()
{
	TestAllMatched();
	TestNewerShape();
	TestUnregisteredPlaced();
	TestZeroLength();
	TestReasons();
	TestLastWins();
	TestNoLeadingNul();
	TestOutcome();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
