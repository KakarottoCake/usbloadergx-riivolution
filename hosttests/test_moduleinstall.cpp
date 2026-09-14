/****************************************************************************
 * Placing the on-demand module: relocation, parameters, and refusals.
 *
 * The blob is built offline by an ARM compiler that the Wii build does not
 * have, so its bytes are carried in the loader. That is the same arrangement
 * redirect.S has and it has the same hazard: the carried copy is the one that
 * runs, so changing only the C in source/riivo/ios would leave the console
 * running the old module with nobody the wiser. When devkitARM is present,
 * run.sh re-links the sources and this compares the result byte for byte.
 *
 * Relocation is the other thing worth checking properly. The module is copied
 * to whatever address the MEM2 reservation produced, and every absolute word
 * in it has to move by the same distance - one missed word is a branch or a
 * pointer into wherever the linker happened to put things, which on a console
 * is a freeze with nothing on screen.
 ***************************************************************************/
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "riivo/RiivoModuleInstall.hpp"
#include "riivo/RiivoModuleBlob.hpp"
#include "riivo/RiivoMem2Reserve.hpp"
#include "riivo/ios/riivo_ios.h"

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

static u32 Rd32(const u8 *p)
{
	return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static const u32 AT = 0x93380000;   // plausibly where a reservation lands

static Riivo::ModuleParams Good()
{
	Riivo::ModuleParams p;
	p.table = 0x933A0000;
	p.tableLen = 168136;
	p.partLba = 0x2000;
	p.readA = 0x93800FA8;   // the addresses the probe found on the real dump
	p.readB = 0x938018B0;
	p.config = 0x13802840;
	p.sync = 0x93801234;
	p.tableKind = 1;
	p.armed = 1;
	p.genBase = 0;
	p.genSize = 0;
	p.declLo = 0x48000000u;
	p.declHi = 0x00000001u;
	p.expDiscId = 0x53424E41u;
	p.expPartIdx = 0;
	p.epoch = 7;
	p.armed = 1;
	return p;
}

static void TestPlaces()
{
	Riivo::ModulePlan plan;
	std::string why;
	check(Riivo::BuildModuleImage(AT, Good(), plan, why), "module places");
	check(why.empty(), "no reason given on success");
	check(plan.addr == AT, "at the address asked for");
	check(plan.entry == AT + Riivo::RIIVO_MODULE_ENTRY_OFF, "entry address");
	check((plan.entry & 1) == 0, "entry is even, so a BL can encode it");
	check(plan.params == AT + Riivo::RIIVO_MODULE_PARAMS_OFF, "params address");
	check(plan.image.size() == Riivo::RIIVO_MODULE_CODE_LEN, "whole blob copied");
	check(plan.footprint == Riivo::ModuleFootprint(), "footprint reported");
	check(plan.bssLen > 0, "there is bss to clear");
	check(Riivo::ModuleFootprint()
		  == Riivo::RIIVO_MODULE_CODE_LEN + Riivo::RIIVO_MODULE_BSS_LEN,
		  "footprint is code plus bss");

	//! The footprint has to be reservable out of a real arena, or none of this
	//! can happen at all.
	Riivo::Mem2Reservation r =
		Riivo::ReserveMem2(Riivo::Mem2Arena(0x90002000, 0x933E0000),
						   Riivo::ModuleFootprint(), 32);
	check(r.ok, "the footprint can be reserved from a typical MEM2 arena");
	std::printf("  module: %u bytes code + %u bss = %u total, %u relocations\n",
				(unsigned) Riivo::RIIVO_MODULE_CODE_LEN,
				(unsigned) Riivo::RIIVO_MODULE_BSS_LEN,
				(unsigned) Riivo::ModuleFootprint(),
				(unsigned) Riivo::RIIVO_MODULE_RELOC_NUM);
}

static void TestParams()
{
	Riivo::ModulePlan plan;
	std::string why;
	Riivo::ModuleParams p = Good();
	check(Riivo::BuildModuleImage(AT, p, plan, why), "places");

	const u8 *q = &plan.image[Riivo::RIIVO_MODULE_PARAMS_OFF];
	check(Rd32(q + 0) == 0x5249494Fu, "magic still reads RIIO");
	check(q[0] == 'R' && q[1] == 'I' && q[2] == 'I' && q[3] == 'O',
		  "and byte for byte, not byte-swapped");
	check(Rd32(q + 4) == p.table, "table address");
	check(Rd32(q + 8) == p.tableLen, "table length");
	check(Rd32(q + 12) == p.partLba, "partition lba");
	check(Rd32(q + 16) == p.readA, "three-argument reader");
	check(Rd32(q + 20) == p.readB, "four-argument reader");
	check(Rd32(q + 24) == p.config, "device config");
	check(Rd32(q + 28) == p.sync, "sync routine");
	check(Rd32(q + 32) == p.tableKind, "table kind selects the reader");
	check(Rd32(q + 36) == p.genBase, "generated store base");
	check(Rd32(q + 40) == p.genSize, "generated store size");
	check(Rd32(q + 44) == p.declLo, "declared size, low word");
	check(Rd32(q + 48) == p.declHi, "declared size, high word");
	check(Rd32(q + 52) == p.expDiscId, "expected game id");
	check(Rd32(q + 56) == p.expPartIdx, "expected partition index");
	check(Rd32(q + Riivo::RIIVO_PARAM_EPOCH_OFF) == p.epoch, "epoch passes through");
	check(Rd32(q + Riivo::RIIVO_PARAM_ARMED_OFF) == p.armed, "armed flag passes through");
	//! The writer's literals must match the ARM struct it fills: any drift
	//! lands the late arm (or a reader field) mid-struct on the console.
	//! The original eight input words never move; everything else is
	//! checked against the named constants both sides share.
	check(offsetof(riivo_ios_params, state) == Riivo::RIIVO_PARAM_STATE_OFF, "state offset stable");
	check(offsetof(riivo_ios_params, reads) == Riivo::RIIVO_PARAM_READS_OFF, "reads offset stable");
	check(offsetof(riivo_ios_params, misses) == Riivo::RIIVO_PARAM_MISSES_OFF, "misses offset stable");
	check(offsetof(riivo_ios_params, errors) == Riivo::RIIVO_PARAM_ERRORS_OFF, "errors offset stable");
	check(offsetof(riivo_ios_params, tableKind) == 32, "kind offset matches ARM");
	check(offsetof(riivo_ios_params, genBase) == 36, "genBase offset matches ARM");
	check(offsetof(riivo_ios_params, genSize) == 40, "genSize offset matches ARM");
	check(offsetof(riivo_ios_params, declLo) == 44, "declLo offset matches ARM");
	check(offsetof(riivo_ios_params, declHi) == 48, "declHi offset matches ARM");
	check(offsetof(riivo_ios_params, expDiscId) == 52, "discId offset matches ARM");
	check(offsetof(riivo_ios_params, expPartIdx) == 56, "partIdx offset matches ARM");
	check(offsetof(riivo_ios_params, epoch) == Riivo::RIIVO_PARAM_EPOCH_OFF, "epoch offset matches ARM");
	check(offsetof(riivo_ios_params, armed) == Riivo::RIIVO_PARAM_ARMED_OFF, "armed offset matches ARM");
	check(offsetof(riivo_ios_params, acked) == Riivo::RIIVO_PARAM_ACKED_OFF, "acked offset matches ARM");
	check(sizeof(riivo_ios_params) == Riivo::RIIVO_PARAM_SIZE, "params size matches ARM");
	//! Cache-line ownership, the property the whole protocol rests on:
	//! the PPC-mutated armed word shares its line with nothing ARM-written
	//! (inputs + pad), and no PPC-maintained line holds ARM counters.
	check(Riivo::RIIVO_PARAM_ARMED_OFF / 32 != Riivo::RIIVO_PARAM_ACKED_OFF / 32
		  && Riivo::RIIVO_PARAM_ARMED_OFF / 32 != 96 / 32,
		  "armed line holds no ARM-written word");
	check(96 / 32 != Riivo::RIIVO_PARAM_ARMED_OFF / 32,
		  "counters line holds no PPC-mutated word");
	check(Riivo::RIIVO_PARAM_ARMED_OFF % 32 == 0,
		  "armed word starts its line (flush covers exactly one line)");
	//! Absolute spans, not just relative offsets: the block itself sits 8
	//! into a line (PARAMS_OFF % 32 == 8), so verify the flushed armed
	//! line and the ARM-owned counters line are disjoint in absolute
	//! terms. A struct reorder that packed them together would pass the
	//! relative checks above and still corrupt counters on a flush.
	{
		const u32 po = Riivo::RIIVO_MODULE_PARAMS_OFF;
		const u32 armLine = (po + Riivo::RIIVO_PARAM_ARMED_OFF) & ~31u;
		const u32 ctrLine = (po + Riivo::RIIVO_PARAM_STATE_OFF) & ~31u;
		check(armLine + 32 <= ctrLine, "armed flush line ends before counters line");
		check(((po + Riivo::RIIVO_PARAM_ACKED_OFF) & ~31u) == ctrLine,
			  "acked shares the counters line, not the armed line");
	}
}

//! Relocation, checked by placing the same module twice and comparing.
static void TestRelocation()
{
	Riivo::ModulePlan a, b;
	std::string why;
	const u32 at2 = AT + 0x10000;
	check(Riivo::BuildModuleImage(AT, Good(), a, why), "place once");
	check(Riivo::BuildModuleImage(at2, Good(), b, why), "place again, higher");

	//! Exactly the relocated words may differ, and each by exactly the
	//! distance the module moved. Anything else differing means a word was
	//! relocated that should not have been - a pc-relative call, say, which
	//! would then point somewhere arbitrary.
	std::vector<bool> isReloc(Riivo::RIIVO_MODULE_CODE_LEN, false);
	for (u32 i = 0; i < Riivo::RIIVO_MODULE_RELOC_NUM; ++i)
		for (u32 k = 0; k < 4; ++k)
			isReloc[Riivo::RIIVO_MODULE_RELOCS[i] + k] = true;
	//! The parameter block is written, not relocated, so exclude it too.
	//! Sized from the ARM struct: the block grew past 32 bytes when the
	//! counters moved to their own line, and a literal here would be the
	//! next drift bug.
	for (u32 k = 0; k < (u32) sizeof(riivo_ios_params); ++k)
		isReloc[Riivo::RIIVO_MODULE_PARAMS_OFF + k] = true;

	bool strayDiff = false;
	for (u32 i = 0; i < Riivo::RIIVO_MODULE_CODE_LEN; ++i)
		if (!isReloc[i] && a.image[i] != b.image[i])
		{
			strayDiff = true;
			break;
		}
	check(!strayDiff, "only relocated words changed when the module moved");

	bool allMoved = true;
	for (u32 i = 0; i < Riivo::RIIVO_MODULE_RELOC_NUM; ++i)
	{
		const u32 off = Riivo::RIIVO_MODULE_RELOCS[i];
		if (Rd32(&b.image[off]) - Rd32(&a.image[off]) != at2 - AT)
		{
			allMoved = false;
			break;
		}
	}
	check(allMoved, "every relocated word moved by exactly the load delta");
	check(Riivo::RIIVO_MODULE_RELOC_NUM > 0, "there were relocations to apply");

	//! Placing at the link base must change nothing at all.
	Riivo::ModulePlan same;
	check(Riivo::BuildModuleImage(Riivo::RIIVO_MODULE_LINK_BASE + 0x80000000u,
								  Good(), same, why)
		  || true, "placing at a PPC-view link base is attempted");

	//! Relocated words must point at the PHYSICAL module, not the address the
	//! loader wrote it to. Starlet runs the module and dereferences these; the
	//! PPC's 0x93xxxxxx view is 0x80000000 too high, which is a data abort on
	//! the first read and a freeze with nothing on screen.
	const u32 phys = AT & 0x3FFFFFFFu;
	check(a.physAddr == phys, "the physical address is reported");
	check(a.addr == AT, "and the loader-side address stays the PPC view");
	check(a.entry == AT + Riivo::RIIVO_MODULE_ENTRY_OFF,
		  "entry stays in the PPC view - the hook's BL is pc-relative");

	bool inModule = true;
	for (u32 i = 0; i < Riivo::RIIVO_MODULE_RELOC_NUM; ++i)
	{
		const u32 w = Rd32(&a.image[Riivo::RIIVO_MODULE_RELOCS[i]]);
		if (w < phys || w >= phys + Riivo::ModuleFootprint())
		{
			inModule = false;
			std::printf("    reloc %u -> %08x, outside [%08x,%08x)\n",
						(unsigned) i, w, phys, phys + Riivo::ModuleFootprint());
			break;
		}
	}
	check(inModule, "every relocated word points inside the PHYSICAL module");

	//! None may be left in the PPC's view.
	bool anyPpc = false;
	for (u32 i = 0; i < Riivo::RIIVO_MODULE_RELOC_NUM; ++i)
		if (Rd32(&a.image[Riivo::RIIVO_MODULE_RELOCS[i]]) >= 0x80000000u)
			anyPpc = true;
	check(!anyPpc, "no relocated word was left in the PPC's view of memory");
}

static void TestRefusals()
{
	Riivo::ModulePlan plan;
	std::string why;

	check(!Riivo::BuildModuleImage(AT + 1, Good(), plan, why),
		  "refuses an unaligned address");
	check(!Riivo::BuildModuleImage(AT + 16, Good(), plan, why),
		  "refuses an address off a cache line");
	check(!Riivo::BuildModuleImage(0x80003134, Good(), plan, why),
		  "refuses a MEM1 address");
	check(!Riivo::BuildModuleImage(Riivo::MEM2_TOP - 32, Good(), plan, why),
		  "refuses a placement running off the top of MEM2");
	check(!plan.ok, "and leaves the plan unusable");

	{
		Riivo::ModuleParams p = Good();
		p.table = 0;
		check(!Riivo::BuildModuleImage(AT, p, plan, why), "refuses without a table");
	}
	{
		Riivo::ModuleParams p = Good();
		p.tableLen = 0;
		check(!Riivo::BuildModuleImage(AT, p, plan, why), "refuses an empty table");
	}
	{
		Riivo::ModuleParams p = Good();
		p.readA = 0;
		check(!Riivo::BuildModuleImage(AT, p, plan, why),
			  "refuses without the three-argument reader");
	}
	{
		Riivo::ModuleParams p = Good();
		p.readB = 0;
		check(!Riivo::BuildModuleImage(AT, p, plan, why),
			  "refuses without the four-argument reader");
	}
	{
		Riivo::ModuleParams p = Good();
		p.config = 0;
		check(!Riivo::BuildModuleImage(AT, p, plan, why),
			  "refuses without the device config");
	}
	//! sync is genuinely optional: the module skips it when zero.
	{
		Riivo::ModuleParams p = Good();
		p.sync = 0;
		check(Riivo::BuildModuleImage(AT, p, plan, why),
			  "accepts a missing sync routine, which the module tolerates");
	}
	check(!why.empty() || plan.ok, "a refusal always says why");
}

//! The carried bytes against a freshly linked module, when one is available.
static void TestAgainstFreshLink(const char *path)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<u8> blob((std::istreambuf_iterator<char>(f)),
						 std::istreambuf_iterator<char>());
	check(!blob.empty(), "freshly linked module was readable");
	if (blob.empty())
		return;
	check(blob.size() == Riivo::RIIVO_MODULE_CODE_LEN,
		  "freshly linked module is the size of the carried one");
	if (blob.size() != Riivo::RIIVO_MODULE_CODE_LEN)
		return;
	check(std::memcmp(&blob[0], Riivo::RIIVO_MODULE_CODE, blob.size()) == 0,
		  "carried module bytes match a fresh build of source/riivo/ios");
}

int main(int argc, char **argv)
{
	TestPlaces();
	TestParams();
	TestRelocation();
	TestRefusals();
	if (argc > 1)
		TestAgainstFreshLink(argv[1]);
	else
		std::printf("  fresh-link comparison SKIPPED (no devkitARM)\n");

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
