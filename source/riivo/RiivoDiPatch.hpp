/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * The disc-read patch: four bytes, derived offline from d2x's own source.
 *
 * HOW THIS WAS OBTAINED (so it can be re-derived when d2x changes)
 *
 * d2x does not ship the DIP plugin as a separate binary - it exists only as
 * DIPP.app, assembled inside the installer - but the source builds with
 * devkitARM, and the tag d2x-v11-beta3 is the current release:
 *
 *   git clone https://github.com/wiidev/d2x-cios
 *   cd source/cios-lib   && make PREFIX=<devkitARM>/bin/arm-none-eabi- \
 *                               CC='<devkitARM>/bin/arm-none-eabi-gcc -std=gnu11'
 *   cd ../dip-plugin     && make  (same overrides; it stops at ../stripios,
 *                                  which is fine - dip-plugin.elf.orig is the
 *                                  unstripped ELF and is what we want)
 *   arm-none-eabi-objdump -d dip-plugin.elf.orig
 *
 * -std=gnu11 is needed because devkitARM 16 defaults to C23, where `bool` is a
 * keyword and cios-lib's `typedef int bool` stops compiling.
 *
 * WHAT THE PATCH DOES
 *
 * DI_EmulateCmd's IOCTL_DI_LOW_READ case (at 0x13800b4a in that build) reads:
 *
 *   682b   ldr  r3, [r5, #0]     @ config.mode
 *   079b   lsls r3, r3, #30      @ test MODE_CRYPT (bit 1)
 *   d400   bmi  .raw             @ set   -> __DI_ReadUnencrypted
 *   e740   b    .decrypt         @ clear -> DI_HandleCmd (stock IOS: AES + H0-H3)
 *  .raw:
 *   6882   ldr  r2, [r0, #8]     @ offset = inbuf[2]
 *   6841   ldr  r1, [r0, #4]     @ len    = inbuf[1]
 *   0038   movs r0, r7           @ outbuf
 *
 * Replacing the first two instructions turns the mode test into an offset test:
 *
 *   6883   ldr  r3, [r0, #8]     @ the requested word offset
 *   005b   lsls r3, r3, #1       @ bit 30 -> sign bit
 *
 * so `bmi` now means "offset >= 0x40000000 words", i.e. at or above 4 GiB into
 * the partition. Mod files are relocated there, and are served raw from the
 * fragment list with no decryption and no hash check - which is exactly right,
 * because they are plaintext files sitting on the card. Every read below the
 * line is real game data and still goes down the untouched, verified path.
 *
 * Two things fall out of the encoding and are worth stating plainly:
 *
 *  - The threshold has to be a power of two, because `lsls`+`bmi` is all that
 *    fits in the two instructions available; an arbitrary 32-bit compare needs
 *    a literal load and there is no room for one before the branch target. Bit
 *    30 is the only usable bit: bit 29 (2 GiB) sits inside real game data, and
 *    bit 31 never gets set because the DVD9 ceiling is 0x7ED38000. So the mod
 *    region starts at exactly 4 GiB, and a game whose own data reaches past
 *    that cannot be patched this way. The caller must check.
 *
 *  - MODE_CRYPT is no longer consulted at all. USB Loader GX never sets it
 *    (it never sends IOCTL_DI_CRYPT_SET), so nothing is lost here, but a loader
 *    that did use it would need a different shape of patch.
 *
 * The patch is applied to the copy of the plugin living in the console's RAM.
 * It installs nothing and is gone on the next reboot.
 ***************************************************************************/
#ifndef RIIVO_DI_PATCH_HPP_
#define RIIVO_DI_PATCH_HPP_

#include <gctypes.h>

namespace Riivo
{
	//! Word offset at or above which reads are served raw. Chosen by the
	//! encoding, not by preference - see the note above.
	static const u32 RIIVO_REGION_WORDS = 0x40000000;

	//! ... and the same threshold as a byte offset into the partition: 4 GiB.
	//! The rebuilt file table must place every modded file at or above this,
	//! and the game's own data must end below it.
	static const u64 RIIVO_REGION_BYTES = 0x100000000ULL;

	//! What to look for in the running cIOS. The first four bytes are the ones
	//! that get replaced; the remaining ten are context, included so the match
	//! cannot land anywhere but the read dispatch.
	static const u8 DI_READ_PATTERN[] = {
		0x68, 0x2B,             // ldr  r3, [r5, #0]     <- replaced
		0x07, 0x9B,             // lsls r3, r3, #30      <- replaced
		0xD4, 0x00,             // bmi  .raw
		0xE7, 0x40,             // b    .decrypt
		0x68, 0x82,             // ldr  r2, [r0, #8]
		0x68, 0x41,             // ldr  r1, [r0, #4]
		0x00, 0x38              // movs r0, r7
	};

	static const u8 DI_READ_REPLACE[] = {
		0x68, 0x83,             // ldr  r3, [r0, #8]
		0x00, 0x5B              // lsls r3, r3, #1
	};

	static const u32 DI_READ_PATTERN_LEN = sizeof(DI_READ_PATTERN);
	static const u32 DI_READ_REPLACE_LEN = sizeof(DI_READ_REPLACE);
}

#endif
