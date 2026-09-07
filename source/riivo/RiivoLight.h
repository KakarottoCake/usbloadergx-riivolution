/* Shared C interface for the boot drive light. apploader.c is C and cannot
   include RiivoBoot.hpp, but the apploader's read loop is the longest stretch
   of the boot that logs nothing, so it is exactly where the pulse is needed. */
#ifndef RIIVO_LIGHT_H
#define RIIVO_LIGHT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Flip the light. No-op unless a Riivolution boot armed it. */
void RiivoPulseLight(void);
#ifdef __cplusplus
}
#endif
#endif
