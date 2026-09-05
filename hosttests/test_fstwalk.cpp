// Independent validation and SDK-style lookup of a completed Wii FST.
#include <stdio.h>
#include <vector>
#include <string>
#include "riivo/RiivoFstBuild.hpp"
#include "riivo/RiivoFstWalk.hpp"

using namespace Riivo;

static int failures = 0;
static int checks = 0;

static void ck(bool condition, const char *what)
{
	++checks;
	if (!condition) { printf("  FAIL: %s\n", what); ++failures; }
}

static void wr32(std::vector<u8> &data, size_t at, u32 value)
{
	data[at] = (u8) (value >> 24);
	data[at + 1] = (u8) (value >> 16);
	data[at + 2] = (u8) (value >> 8);
	data[at + 3] = (u8) value;
}

static void wr24(std::vector<u8> &data, size_t at, u32 value)
{
	data[at] = (u8) (value >> 16);
	data[at + 1] = (u8) (value >> 8);
	data[at + 2] = (u8) value;
}

static u32 rd24(const std::vector<u8> &data, size_t at)
{
	return ((u32) data[at] << 16) | ((u32) data[at + 1] << 8) | data[at + 2];
}

static void AddFile(FstBuilder &builder, const std::string &path, u32 length)
{
	bool isNew = false;
	ck(builder.AddOrReplace(path, length, &isNew), "builder add");
}

static void BuildNested(std::vector<u8> &data,
						std::vector<FstWalkExpectation> &expected)
{
	FstBuilder builder;
	AddFile(builder, "/StageData/NewGalaxy/First.arc", 0x180);
	AddFile(builder, "/StageData/NewGalaxy/empty.bin", 0);
	AddFile(builder, "/ObjectData/Shared.arc", 0x200);
	AddFile(builder, "/Top.bin", 0x40);
	const u64 start = 0x180000000ULL; // 6 GiB, retained through word offsets
	builder.Layout(start, 0x800);

	const char *paths[] = {
		"/StageData/NewGalaxy/First.arc",
		"/StageData/NewGalaxy/empty.bin",
		"/ObjectData/Shared.arc",
		"/Top.bin"
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
	{
		u64 offset = 0;
		u32 length = 0;
		ck(builder.FindAssigned(paths[i], &offset, &length), "builder assignment exists");
		expected.push_back(FstWalkExpectation(paths[i], offset, length));
	}
	builder.Serialize(data, true);
}

static void BuildScale(std::vector<u8> &data,
					   std::vector<FstWalkExpectation> &expected)
{
	FstBuilder builder;
	const u32 areas = 61;
	const u32 groups = 4;
	const u32 filesPerGroup = 17;
	for (u32 area = 0; area < areas; ++area)
		for (u32 group = 0; group < groups; ++group)
			for (u32 file = 0; file < filesPerGroup; ++file)
			{
				char path[96];
				sprintf(path, "/StageData/New%03u/Branch%u/File%02u.arc",
						(unsigned) area, (unsigned) group, (unsigned) file);
				bool isNew = false;
				ck(builder.AddOrReplace(path, 0x100 + file, &isNew) && isNew,
					   "scaled file added in a new directory");
			}
	ck(builder.Stats().addedDirs >= 244, "at least 244 new nested directories");
	ck(builder.Stats().added >= 4000, "several thousand new files");
	builder.Layout(0x180000000ULL, 0x800);

	for (u32 area = 0; area < areas; ++area)
		for (u32 group = 0; group < groups; ++group)
			for (u32 file = 0; file < filesPerGroup; ++file)
			{
				char path[96];
				sprintf(path, "/StageData/New%03u/Branch%u/File%02u.arc",
						(unsigned) area, (unsigned) group, (unsigned) file);
				u64 offset = 0;
				u32 length = 0;
				ck(builder.FindAssigned(path, &offset, &length), "scaled assignment exists");
				expected.push_back(FstWalkExpectation(path, offset, length));
			}
	builder.Serialize(data, true);
}

int main()
{
	std::vector<u8> fst;
	std::vector<FstWalkExpectation> expected;
	BuildNested(fst, expected);

	printf("1. completed FST resolves every placed path\n");
	{
		FstWalk unopened;
		std::vector<FstWalkExpectation> noExpected;
		std::string error;
		ck(!unopened.Check(noExpected, &error), "unopened walker rejects an empty batch");

		FstWalk walk;
		ck(walk.Open(&fst[0], (u32) fst.size(), true, &error), "strict validation");
		ck(walk.EntryCount() == 8, "nested directories are entries too");
		ck(walk.Check(expected, &error), "all placements resolve exactly");
		FstWalkFile first;
		ck(walk.Lookup("/stagedata/newgalaxy/FIRST.ARC", &first), "case-insensitive lookup");
		ck(first.offset == 0x180000000ULL && first.length == 0x180,
		   "6 GiB word offset is widened before shifting");
		ck(first.entry == 3, "lookup reports the source FST entry number");
		FstWalkFile empty;
		ck(walk.Lookup("/StageData/NewGalaxy/empty.bin", &empty) && empty.length == 0,
		   "zero-length mod file is accepted");
		ck(walk.Lookup("/StageData/NewGalaxy/../NewGalaxy/First.arc", &first),
		   "parent-directory walk follows FST parents");
	}

	printf("2. scale: every placement resolves after one validation pass\n");
	{
		std::vector<u8> large;
		std::vector<FstWalkExpectation> largeExpected;
		BuildScale(large, largeExpected);
		FstWalk walk;
		std::string error;
		ck(walk.Open(&large[0], (u32) large.size(), true, &error),
		   "large rebuilt FST validates");
		ck(walk.Check(largeExpected, &error), "all scaled placements resolve exactly");
	}

	printf("3. malformed flat input is refused\n");
	{
		FstWalk walk;
		std::string error;
		std::vector<u8> bad = fst;
		// Entry 1 is StageData. Its next index must remain inside root's subtree.
		wr32(bad, 1 * 12 + 8, 0xffffffffU);
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error), "out-of-range next rejected");

		bad = fst;
		// The first child must name root as its parent.
		wr32(bad, 1 * 12 + 4, 1);
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error), "wrong directory parent rejected");

		bad = fst;
		bad[1 * 12] = 2;
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error), "unknown type rejected");

		bad = fst;
		size_t name = 8 * 12 + rd24(bad, 3 * 12 + 1);
		bad[name] = '.'; bad[name + 1] = 0;
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error), "dot name rejected");

		bad = fst;
		name = 8 * 12 + rd24(bad, 3 * 12 + 1);
		bad[name] = '.'; bad[name + 1] = '.'; bad[name + 2] = 0;
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error), "dot-dot name rejected");

		ck(!walk.Open(&fst[0], (u32) fst.size() - 1, true, &error),
		   "unterminated string-table name rejected");
	}

	printf("4. case-fold collisions are ambiguous to the SDK\n");
	{
		std::vector<u8> bad = fst;
		// Top.bin is entry 7. Give it an all-lowercase spelling of ObjectData,
		// creating two direct root children that the SDK cannot distinguish.
		const u32 nameOff = (u32) bad.size() - 8 * 12;
		const char duplicate[] = "objectdata";
		for (size_t i = 0; i < sizeof(duplicate); ++i)
			bad.push_back((u8) duplicate[i]);
		wr24(bad, 7 * 12 + 1, nameOff);
		FstWalk walk;
		std::string error;
		ck(!walk.Open(&bad[0], (u32) bad.size(), true, &error),
		   "duplicate case-folded siblings rejected");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
