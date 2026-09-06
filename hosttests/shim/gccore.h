#ifndef SHIM_GCCORE_H
#define SHIM_GCCORE_H
// Stands in for libogc's gccore.h, which host builds cannot use. Only what
// the headers pulled in by the tested TUs need: disc.h wants
// ATTRIBUTE_PACKED and the rmode extern's type, nothing else.
#define ATTRIBUTE_PACKED __attribute__((packed))
typedef struct { int unused; } GXRModeObj;
// Cache maintenance after RAM writes. The Wii needs it (Broadway/Starlet do
// not snoop each other); a host test process has coherent memory, so these
// are no-ops that exist only so memory-touching TUs link. Never used to
// decide anything - flushing cannot fail.
static inline void DCFlushRange(void *addr, unsigned len) { (void)addr; (void)len; }
static inline void ICInvalidateRange(void *addr, unsigned len) { (void)addr; (void)len; }
#endif
