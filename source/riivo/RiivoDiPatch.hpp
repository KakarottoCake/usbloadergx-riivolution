/* Runtime-only d2x LOW_READ redirection; no installed IOS is modified. */
#ifndef RIIVO_DI_PATCH_HPP_
#define RIIVO_DI_PATCH_HPP_
#include <gctypes.h>
namespace Riivo {
// Synthetic partition word addresses, not encrypted whole-disc byte offsets.
// Keep the entire window above the DVD5 layer probe, below signed-word wrap,
// and within d2x's fragment seek index. Only the LOW_READ hook may use it.
static const u32 RIIVO_REGION_WORDS = 0x60000000;
static const u64 RIIVO_REGION_BYTES = 0x180000000ULL;
static const u64 RIIVO_REGION_LIMIT = 0x200000000ULL;
// Discovery only. BuildDiHook verifies the surrounding functions and ABI too.
//
// The pattern is split: a 3-halfword head, an uncompared branch, and a
// 3-halfword tail at +8. The branch is position-dependent (it differs between
// d2x builds for identical logic) and must never be compared - the old
// contiguous 14-byte pattern missed this console on every halfword for the
// same reason its halves match Thumb noise in isolation.
//
// Single definition on purpose: this array used to be `static const` here,
// so every TU that included this header carried its own .rodata copy and the
// MEM2 scan matched our own image. It is defined once in RiivoDiHook.cpp;
// ProbeIosPlugin additionally skips matches near that address at runtime.
extern const u8 DI_READ_HEAD[];
extern const u32 DI_READ_HEAD_LEN;   // 6
extern const u32 DI_READ_TAIL_OFF;   // 8
extern const u8 DI_READ_TAIL[];
extern const u32 DI_READ_TAIL_LEN;   // 6
extern const u32 DI_READ_SPAN;       // 14: head + gap + tail, for scan bounds
}
#endif
