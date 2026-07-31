/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 1: apply <memory> patches (direct / valuefile / search / ocarina) to
 * the loaded game binary, in the pre-entry window (after gamepatches, before
 * the DOL list is cleared and the game is launched).
 *
 * Semantics mirror Dolphin's RiivolutionPatcher.cpp:
 *   - effective address = (offset | 0x80000000)
 *   - direct: optional `original` byte-check, then write value/valuefile bytes
 *   - search: scan DOL sections (stride = align) for `original`, write value
 *   - ocarina: scan for `value` pattern, advance to next blr (0x4E800020),
 *              write branch ((target - blr) & 0x03FFFFFC) | 0x48000000
 ***************************************************************************/
#ifndef RIIVO_MEMORY_HPP_
#define RIIVO_MEMORY_HPP_

#include "RiivoTypes.hpp"

namespace Riivo
{
	//! Apply every <memory> patch in `set`. `device` is the SD/USB mount prefix
	//! (e.g. "sd:") used to resolve valuefile paths against each patch root.
	//! Returns the number of patches successfully applied.
	int ApplyMemoryPatches(const ResolvedPatchSet &set, const std::string &device);
}

#endif
