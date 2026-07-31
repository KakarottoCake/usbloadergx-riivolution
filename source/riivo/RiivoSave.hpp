/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 2: <savegame> redirection. USB Loader GX redirects saves via NAND
 * emulation, so a Riivolution <savegame external=> maps to pointing the
 * NAND-emu base path at the mod's own folder, isolating the mod save from the
 * vanilla save. `clone=true` copies the existing save in on first run.
 ***************************************************************************/
#ifndef RIIVO_SAVE_HPP_
#define RIIVO_SAVE_HPP_

#include "RiivoTypes.hpp"

namespace Riivo
{
	//! If `set` contains a <savegame> patch and NAND emulation is enabled, compute
	//! the mod-specific NAND-emu base path (device+root+external) into `outNandPath`
	//! and, when clone is requested and it's the first run, copy the existing save
	//! from `defaultNandPath`. Returns true if the NAND-emu path should be redirected.
	//!
	//! `gameId` is the 6-char disc id. Handles the common title/00010000 save class.
	bool SetupSavegame(const ResolvedPatchSet &set, const std::string &device,
					   const char *gameId, bool nandEmuEnabled,
					   const char *defaultNandPath, std::string &outNandPath);
}

#endif
