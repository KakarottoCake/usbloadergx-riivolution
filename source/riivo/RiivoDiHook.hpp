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
bool EncodeThumbCall(u32 from, u32 to, u8 *out);
bool DecodeThumbCall(u32 from, const u8 *in, u32 &to);
bool DecodeThumbBranch(u32 from, u16 insn, u32 &to);
}
#endif
