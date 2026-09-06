#ifndef RIIVO_PATCH_GUARD_HPP
#define RIIVO_PATCH_GUARD_HPP
#include "RiivoPatchGuard.h"
#include "RiivoTypes.hpp"
namespace Riivo {
// Call once per boot, after PreloadValueFiles has inlined every <memory
// valuefile=> and while the card is still mounted. A patch that still has a
// valuefile at this point is one whose preload failed; it is measured off the
// card as a fallback rather than assumed absent. Inactive/bisection resets it.
void ConfigurePatchProtection(const ResolvedPatchSet &set, const std::string &device, bool enabled);
// Search and ocarina patches add their actual write targets when resolved.
void ProtectAppliedPatch(u32 address, u32 length);
}
#endif
