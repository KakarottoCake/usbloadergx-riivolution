/* The module the loader injects: what the DI hook calls, and everything the
 * loader has to fill in before it does.
 *
 * The parameter block sits at a known offset in the blob so the loader can
 * find it without a symbol table - it is located by its magic, then written,
 * then the whole blob is verified by uncached read-back. Every address in it
 * is one that cannot be known until the console is running: where d2x's device
 * routines live, where the table was placed, which FAT partition to mount.
 *
 * Addresses in `dst` come from the PPC and are in the PPC's view of memory
 * (0x80xxxxxx for MEM1, 0x90xxxxxx for MEM2). Starlet sees the same memory at
 * 0x00xxxxxx and 0x10xxxxxx, so every one of them has to be masked before it
 * is written through. Getting that wrong is an ARM data abort inside an
 * unanswered IPC, which is a hard freeze with nothing on screen.
 */
#ifndef RIIVO_IOS_H
#define RIIVO_IOS_H

#include "riivo_glue.h"

#define RIIVO_IOS_MAGIC   0x5249494Fu   /* 'RIIO' */

/* PPC 0x80xxxxxx -> 0x00xxxxxx, 0x90xxxxxx -> 0x10xxxxxx. */
#define RIIVO_PHYS(a)     ((unsigned int) (a) & 0x3FFFFFFFu)

/* Return codes from riivo_di_read. */
#define RIIVO_DI_OK      0
#define RIIVO_DI_MISS    1   /* not ours; the caller must fall through */
#define RIIVO_DI_FAIL   (-1)

typedef void (*riivo_sync_fn)(void *addr, unsigned int len);

typedef struct
{
	unsigned int magic;       /* RIIVO_IOS_MAGIC, so the loader can find this */
	unsigned int table;       /* redirect table, PPC address */
	unsigned int table_len;
	unsigned int part_lba;    /* FAT partition start, or RR_PART_DISCOVER */
	unsigned int read_a;      /* d2x (lba, count, buf) */
	unsigned int read_b;      /* d2x (0, lba, count, buf) */
	unsigned int config;      /* d2x device config; word +8 picks the above */
	unsigned int sync;        /* os_sync_after_write, or 0 */

	/* Filled in by the module, read back by the loader's boot log. */
	unsigned int state;       /* 0 untried, 1 ready, else the init error */
	unsigned int reads;       /* DI reads served */
	unsigned int misses;      /* DI reads that were not ours */
	unsigned int errors;
} riivo_ios_params;

extern riivo_ios_params g_params;

/* Mount the card and validate the table. Safe to call more than once; only
   the first call does anything. Returns RIIVO_DI_OK or RIIVO_DI_FAIL. */
int riivo_ios_init(void);

/* Serve one DI read. `off_words` is the disc offset in words, exactly as it
   arrives in the command block; `dst` is the PPC-side destination.
   Returns RIIVO_DI_MISS when the offset is outside the mod region, and the
   caller must then run the stock read path unchanged. */
int riivo_di_read(unsigned int off_words, unsigned int len, void *dst);

#endif
