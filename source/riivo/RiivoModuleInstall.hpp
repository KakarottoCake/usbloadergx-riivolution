/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Put the on-demand module where IOS can run it, and tell it what it needs.
 *
 * The module is built offline for the Starlet and carried as bytes (see
 * RiivoModuleBlob.hpp). It is linked at a fixed address purely as a
 * convention: the memory it actually gets is whatever RiivoMem2Reserve carved
 * out of the game's MEM2 arena, which depends on the game and the IOS and
 * cannot be known when the blob is built. So every absolute word in it is
 * fixed up here against where it really landed. The relocations came out of
 * the linker with --emit-relocs, so this is not guesswork about which words
 * are addresses.
 *
 * Only R_ARM_ABS32 words are touched. The pc-relative calls inside the module
 * are correct wherever it sits, and adding a delta to one would send an
 * internal call somewhere arbitrary.
 *
 * The parameter block is written big-endian because that is how the module
 * reads it: Starlet is a big-endian ARM. The PowerPC is big-endian too, so on
 * the console a plain store would do - but this is written out byte by byte so
 * the host tests exercise the same code, and so a future little-endian host
 * cannot quietly produce a blob that only looks right.
 *
 * Everything here is arithmetic on a buffer. Nothing is written to IOS memory
 * until the caller hands the result to the existing patch path, which does the
 * flush and the uncached read-back verify.
 ***************************************************************************/
#ifndef RIIVO_MODULE_INSTALL_HPP_
#define RIIVO_MODULE_INSTALL_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{
	//! Everything the module cannot know until the console is running.
	struct ModuleParams
	{
		u32 table;      //!< the redirect table, as the PPC addresses it
		u32 tableLen;
		u32 partLba;    //!< FAT partition start, or RIIVO_PART_DISCOVER
		u32 readA;      //!< d2x (lba, count, buf)
		u32 readB;      //!< d2x (0, lba, count, buf)
		u32 config;     //!< d2x device config; word +8 picks between them
		u32 sync;       //!< os_sync_after_write inside IOS, or 0

		ModuleParams()
			: table(0), tableLen(0), partLba(0), readA(0), readB(0), config(0),
			  sync(0) {}
	};

	struct ModulePlan
	{
		bool ok;
		u32 addr;        //!< where the blob goes, in the PPC's view
		u32 entry;       //!< riivo_di_read, PPC view, even (the BL is relative)
		u32 params;      //!< g_params, PPC view - the loader writes it
		u32 physAddr;    //!< the same blob as Starlet addresses it
		u32 codeLen;     //!< bytes in `image`
		u32 bssLen;      //!< bytes to clear immediately after them
		u32 footprint;   //!< codeLen + bssLen
		std::vector<u8> image;  //!< relocated and filled, ready to write
		std::string why;

		ModulePlan()
			: ok(false), addr(0), entry(0), params(0), physAddr(0), codeLen(0),
			  bssLen(0), footprint(0) {}
	};

	//! How much MEM2 to reserve. Known before a placement is attempted, so the
	//! reservation can be sized without building anything.
	u32 ModuleFootprint();

	//! Relocate the module to `at` and fill in its parameters. `at` must be
	//! 32-byte aligned - the module contains buffers the storage engines DMA
	//! into - and must leave room for the whole footprint inside MEM2.
	//!
	//! Refuses rather than produces something that would have to be trusted:
	//! a misaligned address, a footprint leaving MEM2, a parameter block that
	//! is missing an address the module would branch through, or a blob whose
	//! magic is not where it should be, which would mean the carried bytes and
	//! these offsets came from different builds.
	bool BuildModuleImage(u32 at, const ModuleParams &p, ModulePlan &plan,
						  std::string &why);
}

#endif
