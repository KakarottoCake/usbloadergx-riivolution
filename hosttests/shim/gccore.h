#ifndef SHIM_GCCORE_H
#define SHIM_GCCORE_H
// Stands in for libogc's gccore.h, which host builds cannot use. Only what
// the headers pulled in by the tested TUs need: disc.h wants
// ATTRIBUTE_PACKED and the rmode extern's type, nothing else.
#define ATTRIBUTE_PACKED __attribute__((packed))
typedef struct { int unused; } GXRModeObj;
#endif
