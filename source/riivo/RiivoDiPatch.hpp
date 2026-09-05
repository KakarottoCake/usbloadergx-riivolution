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
static const u8 DI_READ_PATTERN[] = {
    0x68,0x2B,0x07,0x9B,0xD4,0x00,0xE7,0x40,0x68,0x82,0x68,0x41,0x00,0x38
};
static const u32 DI_READ_PATTERN_LEN = sizeof(DI_READ_PATTERN);
}
#endif
