/* The seam between the redirect layer and d2x's own storage routines.
 *
 * The FAT reader wants read(lba, count, buf). d2x has exactly that, twice,
 * with two different calling conventions chosen at run time by a word in its
 * device config - see RiivoStorageProbe.hpp for how the two are found. This
 * holds the addresses the loader patched in and calls whichever the config
 * selects.
 *
 * Nothing here allocates and nothing here recurses. Buffers handed to the
 * device routines must be 32-byte aligned, because Starlet's SD and USB
 * engines DMA straight into them.
 */
#ifndef RIIVO_GLUE_H
#define RIIVO_GLUE_H

#include "riivo_redirect.h"

/* d2x's three-argument reader: (lba, count, buf). Used when config +8 != 1. */
typedef int (*rg_read_a)(unsigned int lba, unsigned int count, void *buf);

/* d2x's four-argument reader: (0, lba, count, buf). Used when config +8 == 1. */
typedef int (*rg_read_b)(unsigned int zero, unsigned int lba,
						 unsigned int count, void *buf);

typedef struct
{
	rg_read_a read_a;
	rg_read_b read_b;
	const unsigned int *config;  /* word +8 selects the convention */
	unsigned int calls;          /* device reads issued */
	unsigned int sectors;        /* sectors those reads covered */
	unsigned int failures;
} rg_ctx;

/* Values the loader patches in before the hook is ever reached. */
extern rg_ctx g_rg;

/* The rfat_read_fn the FAT reader is mounted with. `ctx` is an rg_ctx. */
int rg_read(void *ctx, unsigned int lba, unsigned int count, void *buf);

/* Whether the glue has been given everything it needs. Checked before the
   first read rather than trusted, because a zero here is a branch to 0. */
int rg_ready(const rg_ctx *c);

#endif
