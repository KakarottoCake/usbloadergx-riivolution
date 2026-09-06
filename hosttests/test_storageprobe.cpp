/****************************************************************************
 * Finding the cIOS routines that read sectors off the card.
 *
 * The module this searches is 128 KB of someone else's console, so the fixture
 * here is synthetic: a small module built to the same shape, which lets every
 * refusal be provoked deliberately. The addresses the probe finds on the real
 * dump were checked separately against the disassembly by hand, and it found
 * exactly the two routines that dump calls - but that dump is not in the
 * repository, so what CI can check is that the search and every one of its
 * refusals behave.
 *
 * The refusals are the point. A wrong address here is a hard freeze with a
 * black screen, which is the single most expensive failure mode this project
 * has: it costs a round trip to a remote tester to learn anything at all.
 ***************************************************************************/
#include <cstdio>
#include <string>
#include <vector>

#include "riivo/RiivoStorageProbe.hpp"
#include "riivo/RiivoDiHook.hpp"

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

static const u32 BASE = 0x93800000;
static const u32 CONFIG = 0x93802840;

//! Thumb halfwords are big-endian in the image, as they are on the console.
static void Put16(std::vector<u8> &m, u32 at, u16 v)
{
	m[at] = (u8) (v >> 8);
	m[at + 1] = (u8) v;
}

static void Put32(std::vector<u8> &m, u32 at, u32 v)
{
	Put16(m, at, (u16) (v >> 16));
	Put16(m, at + 2, (u16) v);
}

//! bl <target>, encoded as the pair of halfwords Thumb-1 uses.
static void PutCall(std::vector<u8> &m, u32 at, u32 target)
{
	u8 enc[4];
	if (!Riivo::EncodeThumbCall(BASE + at, target, enc))
	{
		std::printf("FAIL: fixture could not encode a call\n");
		++g_fail;
		return;
	}
	m[at] = enc[0];
	m[at + 1] = enc[1];
	m[at + 2] = enc[2];
	m[at + 3] = enc[3];
}

//! A module shaped like the real one: a dispatch on config word +8, a
//! four-argument call on the equal branch and a three-argument call on the
//! other. Offsets are fixed so tests can corrupt one field at a time.
struct Fixture
{
	std::vector<u8> mem;

	// Layout, all offsets from BASE.
	static const u32 kDispatchLdr = 0x100; // ldr r3,[pc,#N]
	static const u32 kDispatch    = 0x102; // ldr r3,[r3,#8]
	static const u32 kCmp         = 0x104; // cmp r3,#1
	static const u32 kBne         = 0x106; // bne .other
	static const u32 kCallB       = 0x10C; // bl readB
	static const u32 kOther       = 0x114; // .other
	static const u32 kCallA       = 0x118; // bl readA
	static const u32 kPool        = 0x120; // the config literal
	static const u32 kReadB       = 0x200;
	static const u32 kReadA       = 0x300;
	static const u32 kSync        = 0x500; // os_sync_after_write stub

	Fixture() : mem(0x1000, 0)
	{
		//! ldr r3,[pc,#imm] - the immediate is whatever reaches the pool.
		u32 pc = ((BASE + kDispatchLdr + 4) & ~3u);
		u32 imm = (BASE + kPool - pc) / 4;
		Put16(mem, kDispatchLdr, (u16) (0x4B00 | imm));

		Put16(mem, kDispatch, 0x689B);   // ldr r3,[r3,#8]
		Put16(mem, kCmp,      0x2B01);   // cmp r3,#1

		//! bne .other, in halfword units from pc+4.
		s32 d = (s32) ((BASE + kOther) - (BASE + kBne + 4)) / 2;
		Put16(mem, kBne, (u16) (0xD100 | (d & 0xFF)));

		//! Filler that is not a call, so the scan has to step over it.
		Put16(mem, 0x108, 0x1C2A);
		Put16(mem, 0x10A, 0x1C3B);
		PutCall(mem, kCallB, BASE + kReadB);
		Put16(mem, 0x110, 0xE004);       // b .done
		Put16(mem, 0x112, 0x46C0);       // nop

		Put16(mem, kOther,     0x9802);
		Put16(mem, kOther + 2, 0x1C29);
		PutCall(mem, kCallA, BASE + kReadA);

		Put32(mem, kPool, CONFIG);

		Put16(mem, kReadB, 0xB5F0);      // push {r4-r7,lr}
		Put16(mem, kReadA, 0xB570);      // push {r4-r6,lr}

		//! The os_sync_after_write stub: the syscall word, then `bx lr`.
		Put32(mem, kSync, 0xE6000010u
						  | (Riivo::RIIVO_SYSCALL_SYNC_AFTER_WRITE << 5));
		Put32(mem, kSync + 4, 0xE12FFF1Eu);
	}

	bool Run(Riivo::StoragePlan &plan, std::string &why)
	{
		return Riivo::BuildStoragePlan(&mem[0], (u32) mem.size(), BASE, plan, why);
	}
};

static void TestFinds()
{
	Fixture f;
	Riivo::StoragePlan p;
	std::string why;
	check(f.Run(p, why), "finds the dispatch in a well-formed module");
	check(why.empty(), "no reason given on success");
	check(p.dispatch == BASE + Fixture::kDispatch, "dispatch address");
	check(p.config == CONFIG, "config pointer read from the literal pool");
	check(p.readB == BASE + Fixture::kReadB, "four-argument routine");
	check(p.readA == BASE + Fixture::kReadA, "three-argument routine");
	check(p.readA != p.readB, "the two routines are distinct");
	check(p.ok, "plan is marked usable");
}

//! os_sync_after_write. Absence must not fail the probe - the module skips the
//! call - but ambiguity must resolve to zero rather than to a coin toss,
//! because calling the wrong stub runs an arbitrary syscall on every read.
static void TestSyncStub()
{
	{
		Fixture f;
		Riivo::StoragePlan p;
		std::string why;
		check(f.Run(p, why), "finds the plan");
		check(p.sync == BASE + Fixture::kSync, "finds the sync stub");
		check((p.sync & 1) == 0, "and it is even - the module reaches it by BLX");
	}
	{
		//! No stub at all: still a usable plan, just without the call.
		Fixture f;
		Put32(f.mem, Fixture::kSync, 0);
		Riivo::StoragePlan p;
		std::string why;
		check(f.Run(p, why), "a module without the stub still yields a plan");
		check(p.sync == 0, "and reports no sync routine");
	}
	{
		//! The syscall word without `bx lr` after it is not a stub.
		Fixture f;
		Put32(f.mem, Fixture::kSync + 4, 0xE1A00000u);   // mov r0,r0
		Riivo::StoragePlan p;
		std::string why;
		check(f.Run(p, why), "still a plan");
		check(p.sync == 0, "a syscall word without bx lr is not taken as a stub");
	}
	{
		//! Two candidates: refuse to choose.
		Fixture f;
		Put32(f.mem, 0x600, 0xE6000010u
						   | (Riivo::RIIVO_SYSCALL_SYNC_AFTER_WRITE << 5));
		Put32(f.mem, 0x604, 0xE12FFF1Eu);
		Riivo::StoragePlan p;
		std::string why;
		check(f.Run(p, why), "still a plan");
		check(p.sync == 0, "an ambiguous sync stub resolves to none, not a guess");
	}
	{
		//! A different syscall's stub must not be mistaken for this one.
		Fixture f;
		Put32(f.mem, Fixture::kSync, 0xE6000010u | (0x3Fu << 5));
		Riivo::StoragePlan p;
		std::string why;
		check(f.Run(p, why), "still a plan");
		check(p.sync == 0, "a neighbouring syscall stub is not mistaken for it");
	}
}

//! The module's own literals hold physical MEM2 addresses, because the plugin
//! is linked at a fixed physical address. Rejecting those would refuse a
//! module that is entirely fine.
static void TestPhysicalConfig()
{
	Fixture f;
	Put32(f.mem, Fixture::kPool, 0x13802840);
	Riivo::StoragePlan p;
	std::string why;
	check(f.Run(p, why), "accepts a physical MEM2 config pointer");
	check(p.config == 0x13802840, "and reports it unchanged");
}

static void TestRefusals()
{
	Riivo::StoragePlan p;
	std::string why;

	{
		//! Two dispatches: the pattern no longer identifies one thing, and
		//! picking either would be a coin toss.
		Fixture f;
		Put16(f.mem, 0x400, 0x689B);
		Put16(f.mem, 0x402, 0x2B01);
		check(!f.Run(p, why), "refuses an ambiguous dispatch");
		check(why.find("ambiguous") != std::string::npos, "and says so");
	}
	{
		Fixture f;
		Put16(f.mem, Fixture::kDispatch, 0x6899);   // ldr r1,[r3,#8]
		check(!f.Run(p, why), "refuses when the dispatch is absent");
	}
	{
		//! The instruction before the dispatch is not a pc-relative load, so
		//! there is no config to read.
		Fixture f;
		Put16(f.mem, Fixture::kDispatchLdr, 0x1C0A);
		check(!f.Run(p, why), "refuses without a config load");
	}
	{
		Fixture f;
		Put32(f.mem, Fixture::kPool, 0x80003134);   // a MEM1 address
		check(!f.Run(p, why), "refuses a config pointer outside MEM2");
	}
	{
		Fixture f;
		Put32(f.mem, Fixture::kPool, 0);
		check(!f.Run(p, why), "refuses a null config pointer");
	}
	{
		Fixture f;
		Put16(f.mem, Fixture::kBne, 0xE001);        // an unconditional branch
		check(!f.Run(p, why), "refuses when the device branch is missing");
	}
	{
		//! Neither branch contains a call within the bounded scan.
		Fixture f;
		Put16(f.mem, Fixture::kCallB, 0x46C0);
		Put16(f.mem, Fixture::kCallB + 2, 0x46C0);
		check(!f.Run(p, why), "refuses when a device read call is missing");
	}
	{
		//! Both branches call the same routine - the dispatch would be
		//! pointless, so this is a decode that went wrong.
		Fixture f;
		PutCall(f.mem, Fixture::kCallA, BASE + Fixture::kReadB);
		check(!f.Run(p, why), "refuses when both calls resolve the same");
	}
	{
		//! A call that lands somewhere that is not a function entry.
		Fixture f;
		Put16(f.mem, Fixture::kReadA, 0x1C2A);
		check(!f.Run(p, why), "refuses a call that misses a function entry");
	}
	{
		//! A branch that leaves the captured image entirely.
		Fixture f;
		Put16(f.mem, Fixture::kBne, (u16) (0xD100 | 0x7F));
		Fixture g;
		(void) g;
		//! 0x7F halfwords forward stays inside, so shorten the image instead.
		std::string w2;
		Riivo::StoragePlan p2;
		check(!Riivo::BuildStoragePlan(&f.mem[0], 0x108, BASE, p2, w2),
			  "refuses when the dispatch runs past the end of the image");
	}
	{
		check(!Riivo::BuildStoragePlan(NULL, 0x1000, BASE, p, why),
			  "refuses a null image");
		std::vector<u8> tiny(4, 0);
		check(!Riivo::BuildStoragePlan(&tiny[0], 4, BASE, p, why),
			  "refuses an image too small to hold anything");
	}

	Fixture clean;
	Riivo::StoragePlan ok;
	std::string none;
	check(clean.Run(ok, none), "the fixture is still good after all that");
}

//! Every refusal must leave the plan unusable, not half-filled: a caller that
//! checks only `ok` must never find a stale address in `readA`.
static void TestRefusalLeavesNothing()
{
	Fixture f;
	Put16(f.mem, Fixture::kDispatch, 0x6899);
	Riivo::StoragePlan p;
	p.readA = 0xDEADBEEF;
	p.ok = true;
	std::string why;
	check(!f.Run(p, why), "refuses");
	check(!p.ok, "and clears ok");
	check(p.readA == 0, "and clears the addresses it did not find");
	check(!why.empty(), "and always says why");
}

int main()
{
	TestFinds();
	TestSyncStub();
	TestPhysicalConfig();
	TestRefusals();
	TestRefusalLeavesNothing();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
