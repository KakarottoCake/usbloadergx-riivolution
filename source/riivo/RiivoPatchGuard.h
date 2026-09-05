/* Shared C interface for loader patchers. Addresses are PPC byte addresses. */
#ifndef RIIVO_PATCH_GUARD_H
#define RIIVO_PATCH_GUARD_H
#include <gctypes.h>
#ifdef __cplusplus
extern "C" {
#endif
int RiivoPatchConflict(u32 address, u32 length);
#ifdef __cplusplus
}
#endif
#endif
