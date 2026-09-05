#ifndef RIIVO_PATCH_GUARD_HPP
#define RIIVO_PATCH_GUARD_HPP
#include "RiivoPatchGuard.h"
#include "RiivoTypes.hpp"
namespace Riivo {
// Call once per boot, while the card is still mounted: a <memory valuefile=>
// patch is only measurable by reading the file, and its bytes are not loaded
// until the patches are applied. Inactive/bisection resets it.
void ConfigurePatchProtection(const ResolvedPatchSet &set, const std::string &device, bool enabled);
// Search and ocarina patches add their actual write targets when resolved.
void ProtectAppliedPatch(u32 address, u32 length);
}
#endif
