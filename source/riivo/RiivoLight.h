/* Shared C interface into RiivoBoot for C translation units. apploader.c
   is C and cannot include RiivoBoot.hpp, but the apploader's read loop is
   the longest stretch of the boot that logs nothing (hence the pulse),
   and its per-yield disc offsets are dropped by RegisterDOL (hence the
   range notes, which trace a loaded range back to its source offset). */
#ifndef RIIVO_LIGHT_H
#define RIIVO_LIGHT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Flip the light. No-op unless a Riivolution boot armed it. */
void RiivoPulseLight(void);
/* Record one apploader yield: bytes [dst, dst+len) came from discOffset.
   Evidence only; the boot below runs exactly as without it. */
void RiivoNoteDOLRange(unsigned int dst, unsigned int len, unsigned int discOffset);
#ifdef __cplusplus
}
#endif
#endif
