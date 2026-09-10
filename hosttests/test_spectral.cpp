// Exact-input Spectral reproduction: the REAL local USA XML + the REAL
// local mod tree + the REAL SB4E01 base FST, driven through the REAL
// production chain (ParseFile -> Resolve -> BuildRedirects ->
// FstBuilder -> ValidateTable with opTrace). No synthetic files, no
// padding, no mirrors.
//
// Needs SPECTRAL_FST (153792-byte SB4E01 FST), SPECTRAL_XML (the USA
// xml) and SPECTRAL_MOD (the mod root holding e.g. AudioRes/,
// LocalizeData/, ...). Without all three it SKIPS (CI-safe).
//
// Reference: the tester's hardware log at
//   Downloads/Spectral_usbloadergx_riivo_SB4E01.log
// (build c7d6d27a, SB4E01, "Spectral USA.xml", Core=Enabled):
//   mod files listed: 2148 found; matched: 267 replacements, 1881
//   additions; planned 2148 (0 rejected); table serialised 230076
//   bytes - then the log ends: the stop is inside ValidateTable.
// Deltas between that log and this run name the version gap between
// the tester's newer pack and this local v11 tree.
//
// VERDICT DISCIPLINE (record, not conclusion): this run covers the
// 2143-path LOCAL input; hardware runs 2148 paths. The 5-path gap is
// localised to CustomCode/ (tester 15 files, local 10) and is UNTESTED:
// nothing here says the inputs do or do not explain the stop until
// those five files' relative paths and sizes are reproduced and the
// 230076-byte plain table is confirmed. Their contents are never read
// by the exercised path (table bytes come from names/offsets/sizes
// only); paths+sizes reproduce, hashes identify.
// A passing exact-input run still would not isolate fragmentation.
// Open differences vs hardware, in decreasing order of suspicion:
// production allocation history (13 s of placement/fragment mapping
// precede the window), MEM2 fragmentation state at window entry (log
// shows 36920 KB free - free, not contiguous), device I/O during
// matching, card logging, interrupts, and prior heap corruption.
// Related: the log's "492235142 bytes" is MAPPED DISC PAYLOAD (offset
// accounting), not RAM - the window itself holds single-digit MB
// (printed below). Never compare the two as if both were memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include "riivo/RiivoParser.hpp"
#include "riivo/RiivoConfig.hpp"
#include "riivo/RiivoFile.hpp"
#include "riivo/RiivoFst.hpp"
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFstWalk.hpp"
#include "riivo/RiivoReconcile.hpp"
#include "riivo/RiivoValidate.hpp"

using namespace Riivo;

static int failures = 0, checks = 0;
static void ck(bool c, const char *w)
{
	++checks;
	if (!c) { printf("  FAIL: %s\n", w); ++failures; }
}

//! DirLister over a host directory tree. Production hands fullDir as
//! JoinPath(device, root, external); with device="" that is "/Spectral"
//! + external, and CARD_PREFIX ("/Spectral") is re-rooted onto MOD_ROOT.
//! Everything else about the listing - recursion, relative paths, skip
//! rules - is the production FsDirLister, not a copy.
struct HostLister : public FsDirLister
{
	std::string cardPrefix, modRoot;
	u32 listedSoFar;
	HostLister() : listedSoFar(0) {}
	void List(const std::string &fullDir, bool recursive,
			  std::vector<std::string> &out)
	{
		if (!cardPrefix.empty() &&
			fullDir.compare(0, cardPrefix.size(), cardPrefix) == 0)
		{
			FsDirLister::List(modRoot + fullDir.substr(cardPrefix.size()),
							  recursive, out);
		}
		else
			FsDirLister::List(fullDir, recursive, out);
		// Per-rule progress in the hardware log's own format: each
		// rule prints once with the files gathered so far, so every
		// line diffs directly against the matching log line.
		printf("    listing %s (%u so far)\n", fullDir.c_str(),
			   listedSoFar);
		listedSoFar += (u32) out.size();
	}
};

static bool ReadFile(const char *path, std::vector<u8> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) { fclose(f); return false; }
	out.resize((size_t) len);
	bool ok = fread(&out[0], 1, (size_t) len, f) == (size_t) len;
	fclose(f);
	return ok;
}

static u32 HostSize(const std::string &cardPrefix, const std::string &modRoot,
					const std::string &external, bool &ok)
{
	ok = false;
	if (cardPrefix.empty() ||
		external.compare(0, cardPrefix.size(), cardPrefix) != 0)
		return 0;
	struct stat st;
	if (stat((modRoot + external.substr(cardPrefix.size())).c_str(),
			 &st) != 0)
		return 0;
	ok = true;
	return (u32) st.st_size;
}

int main()
{
	const char *fstPath = getenv("SPECTRAL_FST");
	const char *xmlPath = getenv("SPECTRAL_XML");
	const char *modRoot = getenv("SPECTRAL_MOD");
	if (!fstPath || !*fstPath || !xmlPath || !*xmlPath ||
		!modRoot || !*modRoot)
	{
		printf("spectral: SKIPPED (set SPECTRAL_FST/SPECTRAL_XML/SPECTRAL_MOD)\n");
		return 0;
	}
	printf("spectral: exact-input reproduction\n");
	printf("  fst: %s\n  xml: %s\n  mod: %s\n", fstPath, xmlPath, modRoot);

	std::vector<u8> base;
	ck(ReadFile(fstPath, base), "base FST reads");
	Fst disc;
	ck(disc.Parse(&base[0], (u32) base.size(), true), "base parses");

	Disc parsed;
	std::string perr;
	ck(ParseFile(xmlPath, parsed, &perr), "USA xml parses");
	if (!perr.empty()) printf("  parse note: %s\n", perr.c_str());
	// Replay the tester's selection (the xml carries no default):
	// Super Mario Spectral / Core = Enabled.
	bool sel = false;
	for (size_t s = 0; s < parsed.sections.size() && !sel; ++s)
		for (size_t o = 0; o < parsed.sections[s].options.size(); ++o)
		{
			parsed.sections[s].options[o].selectedChoice = 1;
			sel = true;
		}
	ck(sel, "Core=Enabled selected");
	ResolvedPatchSet set;
	Resolve(parsed, "SB4E01", set);
	printf("  resolved: %u file, %u folder rules\n",
		   (unsigned) set.files.size(), (unsigned) set.folders.size());

	HostLister lister;
	lister.cardPrefix = "/Spectral";
	lister.modRoot = modRoot;
	// Per-rule listing progress prints from inside the lister (see
	// above), in the hardware log's own format.
	printf("  per-rule listing:\n");
	std::vector<RedirectSpec> redirects;
	std::vector<CreatedFile> created;
	BuildRedirects(disc, set, "", &lister, redirects, &created);
	printf("  matched: %u replacement(s), %u addition(s)\n",
		   (unsigned) redirects.size(), (unsigned) created.size());
	printf("  (hardware log: 267 replacements, 1881 additions)\n");
	ck(redirects.size() + created.size() > 0, "non-empty applied set");

	// External sizes (production stats the card here): stat the host
	// tree through the same prefix mapping. A miss is a phantom
	// addition, exactly the confusion missingCreated exists to avoid.
	FstBuilder b;
	ck(b.Parse(&base[0], (u32) base.size(), true), "builder parses base");
	std::map<std::string, u32> modSizes;
	u32 planned = 0, rejected = 0, missing = 0;
	for (size_t i = 0; i < redirects.size(); ++i)
	{
		bool ok = false;
		u32 sz = HostSize(lister.cardPrefix, lister.modRoot,
						  redirects[i].external, ok);
		std::string key = NormaliseDiscPath(redirects[i].disc);
		if (!ok) { ++missing; continue; }
		bool isNew = false;
		if (b.AddOrReplace(redirects[i].disc, sz, &isNew))
		{
			++planned;
			modSizes[key] = sz;
		}
		else
			++rejected;
	}
	for (size_t i = 0; i < created.size(); ++i)
	{
		bool ok = false;
		u32 sz = HostSize(lister.cardPrefix, lister.modRoot,
						  created[i].external, ok);
		std::string key = NormaliseDiscPath(created[i].disc);
		if (!ok) { ++missing; continue; }
		bool isNew = false;
		if (b.AddOrReplace(created[i].disc, sz, &isNew))
		{
			++planned;
			modSizes[key] = sz;
		}
		else
			++rejected;
	}
	printf("  table entries planned: %u (%u rejected, %u missing)\n",
		   planned, rejected, missing);
	printf("  (hardware log: planned 2148, 0 rejected)\n");

	// Production-shaped early placement: 32 KB-aligned cursor from the
	// mod region start (harness value; the exact gameEnd-derived start
	// only shifts offsets, not the window's allocation shape).
	const u64 regionStart = 0x0180000000ULL;
	const u64 mask = (u64) 32768 - 1;
	u64 cursor = regionStart;
	std::map<std::string, u64> modOffsets;
	std::vector<RegRecord> records;
	for (std::map<std::string, u32>::const_iterator it = modSizes.begin();
		 it != modSizes.end(); ++it)
	{
		cursor = (cursor + mask) & ~mask;
		modOffsets[it->first] = cursor;
		cursor += it->second;
	}
	// Record discs must match the redirect set (FindSkips reads them).
	for (size_t i = 0; i < redirects.size(); ++i)
	{
		std::string key = NormaliseDiscPath(redirects[i].disc);
		std::map<std::string, u64>::const_iterator it =
			modOffsets.find(key);
		if (it == modOffsets.end()) continue;
		RegRecord r;
		r.disc = redirects[i].disc;
		r.external = redirects[i].external;
		r.offset = it->second;
		r.length = modSizes[key];
		records.push_back(r);
	}
	for (size_t i = 0; i < created.size(); ++i)
	{
		std::string key = NormaliseDiscPath(created[i].disc);
		std::map<std::string, u64>::const_iterator it =
			modOffsets.find(key);
		if (it == modOffsets.end()) continue;
		RegRecord r;
		r.disc = created[i].disc;
		r.external = created[i].external;
		r.offset = it->second;
		r.length = modSizes[key];
		records.push_back(r);
	}
	ck(b.LayoutFrom(modOffsets) == 0, "early placement clean");
	std::vector<u8> plain;
	b.Serialize(plain, true);
	printf("  plain table: %u bytes (hardware log: 230076)\n",
		   (unsigned) plain.size());

	std::map<std::string, SkipReason> addFails;
	ValidateRequest vreq;
	vreq.builder = &b;
	vreq.fst = &disc;
	vreq.plainFst = &plain;
	vreq.modOffsets = &modOffsets;
	vreq.expectedModSizes = &modSizes;
	vreq.fstReserve = 153792;
	vreq.region = regionStart;
	vreq.modRegionStart = regionStart;
	vreq.redirects = &redirects;
	vreq.created = &created;
	vreq.modRecords = &records;
	vreq.modAddFails = &addFails;
	vreq.imageBytes = 4685037568ULL;
	vreq.sectorSize = 512;
	vreq.usedFrags = 3;
	ValidateResult vres;
	int trace = -1;
	ValidateTable(vreq, vres, &trace);
	// RAM the window actually holds (vs the mapped disc payload):
	// plain + staged + expectations + placements. Single-digit MB;
	// the log's 492 MB figure is offset accounting, not memory.
	u64 payload = 0;
	for (std::map<std::string, u32>::const_iterator it = modSizes.begin();
		 it != modSizes.end(); ++it)
		payload += it->second;
	printf("  mapped payload: %llu bytes (hardware log: 492235142)\n",
		   (unsigned long long) payload);
	printf("  window: oom=%d walk=%d paths=%u compact=%d staged=%u "
		   "plan=%d placed=%u skips=%u trace=%d\n",
		   (int) vres.oom, (int) vres.fstWalkOK, vres.expectedPaths,
		   (int) vres.compactOK,
		   (unsigned) vres.staged.size(), (int) vres.plan.ok,
		   (unsigned) vres.placed.size(),
		   (unsigned) vres.modSkips.size(), trace);
	printf("  window RAM: plain %u + staged %u + expectations %u x %uB + "
		   "placed %u x %uB\n",
		   (unsigned) plain.size(), (unsigned) vres.staged.size(),
		   vres.expectedPaths, (unsigned) sizeof(FstWalkExpectation),
		   (unsigned) vres.placed.size(), (unsigned) sizeof(PlacedFile));
	if (!vres.fstWalkOK) printf("  walkError: %s\n", vres.walkError.c_str());
	if (!vres.plan.ok) printf("  planWhy: %s\n", vres.plan.why.c_str());
	if (!vres.modSkips.empty())
	{
		std::map<int, u32> byReason;
		for (size_t i = 0; i < vres.modSkips.size(); ++i)
			++byReason[(int) vres.modSkips[i].reason];
		printf("  skip reasons:");
		for (std::map<int, u32>::const_iterator it = byReason.begin();
			 it != byReason.end(); ++it)
			printf(" %d:%u", it->first, it->second);
		printf(" (first: %s)\n", vres.modSkips[0].disc.c_str());
	}
	const u8 *stagePtr = vres.staged.empty()
		? (plain.empty() ? (const u8 *) 0 : &plain[0]) : &vres.staged[0];
	u32 stageLen = vres.staged.empty()
		? (u32) plain.size() : (u32) vres.staged.size();
	if (stagePtr && stageLen)
		printf("  staged CRC32: %08x\n", Crc32(stagePtr, stageLen));
	ck(trace == VOP_NONE, "trace runs to completion");

	printf("spectral: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
