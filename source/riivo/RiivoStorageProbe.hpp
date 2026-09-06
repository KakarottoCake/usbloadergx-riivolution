/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Find the cIOS routines that actually read sectors off the card.
 *
 * The on-demand design needs to read arbitrary sectors from inside the DI
 * thread: a FAT directory here, a cluster chain there, then the file's data.
 * The existing hook cannot do that. It calls d2x's fragment reader, which
 * takes a WORD offset on the virtual disc - a 32-bit word offset is only 16
 * GiB of address space, and the mod region already sits at 6 GiB, so a card
 * larger than a couple of gigabytes cannot be addressed that way at all.
 *
 * Underneath the fragment reader, though, d2x resolves a fragment to a plain
 * LBA and calls one of two device routines. Those take an absolute 32-bit
 * sector number, which is two terabytes of range, and they are exactly the
 * primitive a FAT reader wants. This finds them.
 *
 * From the read worker on the tester's module (dipp-93800a1c.asm):
 *
 *   ldr  r3, [pc, #88]      ; the device config
 *   ldr  r3, [r3, #8]       ; which device the game was loaded from
 *   cmp  r3, #1
 *   bne  .other
 *   ...                     ; r0 = 0, r1 = lba, r2 = count, r3 = buf
 *   bl   <read_b>
 *   b    .done
 *  .other:
 *   ...                     ; r0 = lba, r1 = count, r2 = buf
 *   bl   <read_a>
 *
 * So there are two calling conventions, not one, and which applies is decided
 * at run time by a word in the config. Both are recorded here and the choice
 * is left to the caller, because guessing wrong writes sectors into the wrong
 * register and reads from a garbage LBA.
 *
 * Nothing is assumed about where any of this sits. The dispatch is located by
 * its instruction pattern and required to be UNIQUE in the module: if a build
 * lays it out differently, or has two of them, this refuses and no IOS code is
 * changed. A wrong address here is a hard freeze with nothing on screen, which
 * is the most expensive failure this project has, so refusing is always the
 * better answer.
 ***************************************************************************/
#ifndef RIIVO_STORAGE_PROBE_HPP_
#define RIIVO_STORAGE_PROBE_HPP_

#include <gctypes.h>
#include <string>

namespace Riivo
{
	//! The value of config word +8 that selects the four-argument routine.
	static const u32 RIIVO_STORAGE_DEV_B = 1;

	//! IOS syscalls are the undefined ARM instruction 0xE6000010 | (num << 5),
	//! wrapped one per stub in a table, each followed by `bx lr`.
	//! os_sync_after_write is syscall 0x40 - confirmed on the measured module
	//! by both device read routines calling it around their own transfers,
	//! rather than taken from a remembered syscall table.
	static const u32 RIIVO_SYSCALL_SYNC_AFTER_WRITE = 0x40;

	struct StoragePlan
	{
		bool ok;
		u32 dispatch;   //!< the `ldr r3,[r3,#8]` that picks a device
		u32 config;     //!< the config struct; word +8 is the device
		u32 readA;      //!< device != 1: (r0 = lba, r1 = count, r2 = buf)
		u32 readB;      //!< device == 1: (r0 = 0, r1 = lba, r2 = count, r3 = buf)

		//! os_sync_after_write, or 0 if it could not be identified.
		//!
		//! Not fatal when absent: the module skips the call. d2x's own read
		//! routines already sync what they DMA in, so what goes unsynced is
		//! only the module's own copy out to the game's buffer - which the PPC
		//! may then read stale. That fails later and looks like corruption, so
		//! it is worth having, but it is not worth refusing a boot over.
		//!
		//! ARM, not Thumb: the address is even and the module reaches it with
		//! a BLX, which takes the target state from bit 0.
		u32 sync;

		std::string why;

		StoragePlan()
			: ok(false), dispatch(0), config(0), readA(0), readB(0), sync(0) {}
	};

	//! Locate both device read routines in a snapshot of the plugin.
	//! `base` is the address `image` was captured from. Returns false with
	//! `why` set rather than a guess.
	bool BuildStoragePlan(const u8 *image, u32 size, u32 base, StoragePlan &out,
						  std::string &why);
}

#endif
