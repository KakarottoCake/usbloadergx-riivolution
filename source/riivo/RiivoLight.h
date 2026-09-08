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
/* Record a failed apploader chunk read and persist it now: destination,
   length, disc offset and the read's return code. The card log is alive
   at this point; after the return below, BootPartition is over and no
   placement report can follow, so this line is the record. */
void RiivoLogChunkFailure(unsigned int dst, unsigned int len,
						   unsigned int discOffset, int code);
#ifdef __cplusplus
}
#endif
#endif
