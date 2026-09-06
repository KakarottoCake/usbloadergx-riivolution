#ifndef RIIVO_DI_HOOK_HPP_
#define RIIVO_DI_HOOK_HPP_
#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo {
struct DiHookPlan {
    u32 storage;
    u32 dispatch;
    std::vector<u8> code;
    std::vector<u8> branch;
    DiHookPlan() : storage(0), dispatch(0) {}
};

// All addresses are in the same snapshot coordinate system. Relative branches
// retain their meaning when IOS maps that physical segment at a virtual address.
bool BuildDiHook(const u8 *image, u32 size, u32 base, u32 site,
                 u32 endWords, DiHookPlan &plan, std::string &why);

// The same hook, for the on-demand design: instead of answering out of the
// fragment list it calls a module the loader placed in reserved MEM2, which
// opens the mod's files by path. Discovery and every verification are shared
// with BuildDiHook - only the emitted code differs - so the two can never
// disagree about where the site is or whether it is safe to patch.
//
// `moduleEntry` is riivo_di_read. Pass it EVEN: a Thumb symbol carries bit 0
// set, and the BL encoding has no room for it (the call switches state on its
// own). An odd address is refused rather than silently rounded, because
// rounding the wrong way lands mid-instruction.
//
// There is no limit word here. The module returns MISS for anything outside
// the mod region, so that decision is made once rather than duplicated in two
// places that can drift.
bool BuildDiHookOnDemand(const u8 *image, u32 size, u32 base, u32 site,
                         u32 moduleEntry, DiHookPlan &plan, std::string &why);
bool EncodeThumbCall(u32 from, u32 to, u8 *out);
bool DecodeThumbCall(u32 from, const u8 *in, u32 &to);
bool DecodeThumbBranch(u32 from, u16 insn, u32 &to);
}
#endif
