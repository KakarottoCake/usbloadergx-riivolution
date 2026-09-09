/* Shim that lets the REAL RiivoFstInstall.cpp compile outside GX.
 * Production GX provides source/gecko.h with the same gprintf symbol;
 * here it is the video console (defined in source/main.cpp). */
#ifndef FSTADAPTER_GECKO_H_
#define FSTADAPTER_GECKO_H_

#include <gccore.h>

#ifdef __cplusplus
extern "C"
{
#endif

void gprintf(const char *str, ...);

#ifdef __cplusplus
}
#endif

#endif
