/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoStorageProbe.hpp for what is being found and why refusing beats
 * guessing.
 ***************************************************************************/
#include "RiivoStorageProbe.hpp"
#include "RiivoDiHook.hpp"

namespace Riivo
{
	//! Thumb halfwords are big-endian in the module image, as everywhere else
	//! in this file's world: Starlet is a big-endian ARM.
	static u16 Rd16(const u8 *p) { return (u16) ((u16(p[0]) << 8) | p[1]); }

	//! `ldr rt, [pc, #imm8]`: 0x4800 | (rt << 8) | imm8. The literal sits at
	//! (pc + 4 + imm8*4) & ~3. The immediate is decoded, never matched - pool
	//! placement moves between builds.
	static bool PcLoadTarget(u32 addr, u16 insn, u32 &at)
	{
		if ((insn & 0xF800) != 0x4800)
			return false;
		at = ((addr + 4) & ~3u) + ((insn & 0xFF) * 4);
		return true;
	}

	//! `bne.n`: 0xD100 | imm8, signed, halfword units.
	static bool BneTarget(u32 addr, u16 insn, u32 &to)
	{
		if ((insn & 0xFF00) != 0xD100)
			return false;
		s32 d = (s32) (insn & 0xFF);
		if (d & 0x80)
			d -= 0x100;
		to = addr + 4 + (u32) (d * 2);
		return true;
	}

	bool BuildStoragePlan(const u8 *image, u32 size, u32 base, StoragePlan &out,
						  std::string &why)
	{
		out = StoragePlan();

		if (!image || size < 16)
		{
			why = "no plugin image to search";
			return false;
		}

		//! ldr r3,[r3,#8] ; cmp r3,#1 - the device dispatch. Required to be
		//! unique: two matches means this pattern does not identify the thing
		//! we think it does, and picking either would be a coin toss.
		u32 found = 0;
		u32 hits = 0;
		for (u32 i = 0; i + 4 <= size; i += 2)
		{
			if (Rd16(image + i) == 0x689B && Rd16(image + i + 2) == 0x2B01)
			{
				found = i;
				if (++hits > 1)
					break;
			}
		}
		if (hits == 0)
		{
			why = "device dispatch not found; no IOS code changed";
			return false;
		}
		if (hits > 1)
		{
			why = "device dispatch is ambiguous; no IOS code changed";
			return false;
		}

		//! The config pointer is loaded immediately before the dispatch.
		if (found < 2)
		{
			why = "device dispatch has no room for a config load";
			return false;
		}
		u32 litAt = 0;
		if (!PcLoadTarget(base + found - 2, Rd16(image + found - 2), litAt))
		{
			why = "no config load before the device dispatch";
			return false;
		}
		if (litAt < base || litAt - base + 4 > size)
		{
			why = "config literal is outside the plugin image";
			return false;
		}
		{
			const u8 *p = image + (litAt - base);
			out.config = ((u32) Rd16(p) << 16) | Rd16(p + 2);
		}
		//! A config pointer that is not a MEM2 address means the literal was
		//! not what we thought, and everything after it would be nonsense.
		//!
		//! The module's own literals hold PHYSICAL MEM2 addresses (0x10000000
		//! upwards) because the plugin is linked at a fixed physical address
		//! and needs no MMU translation; the PPC sees the same memory at
		//! 0x90000000. Both forms are accepted, since which one appears
		//! depends on how the snapshot was captured, and mistaking one for a
		//! bad pointer would refuse a module that is perfectly fine.
		{
			const u32 c = out.config;
			const bool phys = c >= 0x10000000u && c < 0x14000000u;
			const bool ppc  = c >= 0x90000000u && c < 0x94000000u;
			if (!phys && !ppc)
			{
				why = "config pointer is not a MEM2 address";
				return false;
			}
		}

		//! The branch that separates the two conventions.
		u32 elseAddr = 0;
		if (!BneTarget(base + found + 2, Rd16(image + found + 4), elseAddr))
		{
			why = "no branch after the device dispatch";
			return false;
		}
		if (elseAddr < base || elseAddr - base >= size)
		{
			why = "device branch leaves the plugin image";
			return false;
		}

		//! First call after the compare is the four-argument routine; first
		//! call at the branch target is the three-argument one. Both are
		//! bounded so a missing call cannot run off into unrelated code.
		const u32 kScan = 0x20;

		u32 a = 0, b = 0;
		for (u32 i = found + 6; i + 4 <= size && i < found + 6 + kScan; i += 2)
		{
			u32 t = 0;
			if (DecodeThumbCall(base + i, image + i, t))
			{
				b = t;
				break;
			}
		}
		u32 eo = elseAddr - base;
		for (u32 i = eo; i + 4 <= size && i < eo + kScan; i += 2)
		{
			u32 t = 0;
			if (DecodeThumbCall(base + i, image + i, t))
			{
				a = t;
				break;
			}
		}

		if (!a || !b)
		{
			why = "device read calls not found; no IOS code changed";
			return false;
		}
		if (a == b)
		{
			why = "device read calls resolve to the same routine";
			return false;
		}
		if (a < base || a - base >= size || b < base || b - base >= size)
		{
			why = "a device read call leaves the plugin image";
			return false;
		}
		//! Both must look like function entries. A target that is not a push
		//! is a decode that went wrong, not a routine.
		if ((Rd16(image + (a - base)) & 0xFE00) != 0xB400
			|| (Rd16(image + (b - base)) & 0xFE00) != 0xB400)
		{
			why = "a device read call does not land on a function entry";
			return false;
		}

		//! os_sync_after_write. Its stub is one ARM word holding the syscall
		//! and one holding `bx lr`, so it is found by that pair rather than by
		//! position in the table - the table's contents differ between IOS
		//! versions and counting entries would drift.
		//!
		//! Absence is tolerated (the module skips the call); ambiguity is not,
		//! and resolves to zero rather than to a coin toss, because calling
		//! the wrong stub runs an arbitrary syscall on every read.
		{
			const u32 want = 0xE6000010u
						   | (RIIVO_SYSCALL_SYNC_AFTER_WRITE << 5);
			u32 at = 0;
			u32 n = 0;
			//! ARM words are 4-byte aligned, and `base` is aligned down to a
			//! cache line, so stepping by 4 from the start stays in phase.
			for (u32 i = 0; i + 8 <= size; i += 4)
			{
				const u32 w = ((u32) Rd16(image + i) << 16) | Rd16(image + i + 2);
				if (w != want)
					continue;
				const u32 nx = ((u32) Rd16(image + i + 4) << 16)
							 | Rd16(image + i + 6);
				if (nx != 0xE12FFF1Eu)   // bx lr
					continue;
				at = base + i;
				++n;
			}
			out.sync = (n == 1) ? at : 0;
		}

		out.dispatch = base + found;
		out.readA = a;
		out.readB = b;
		out.ok = true;
		return true;
	}
}
