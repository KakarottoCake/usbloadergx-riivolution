/* RIV1 segment reader for the IOS side. See riivo_segread.c for the contract.
 *
 * Serves the manifest the loader staged: ordered, non-overlapping extents in
 * partition bytes. Gaps (Original) are NOT listed; how they read is decided
 * by sr_read, never by guessing here.
 */
#ifndef RIIVO_SEGREAD_H_
#define RIIVO_SEGREAD_H_

#include "riivo_fat.h"
#include "riivo_page.h"

#define SR_MAGIC      0x31564952u   /* 'RIV1' little-endian */
#define SR_VERSION    1
#define SR_HEADER     48
#define SR_ENTRY_SIZE 32

#define SR_EXT_EXTERNAL 1
#define SR_EXT_GENERATED 2
#define SR_EXT_ZERO     3

#define SR_OK          0
#define SR_EINVAL     -1
#define SR_EBADTABLE  -2
#define SR_MISS       -3   /* not ours; the caller should do a normal read */
#define SR_EIO        -4

typedef struct
{
	const unsigned char *table;   /* resident table (paged == 0 only) */
	unsigned int count;
	const char *strings;
	rfat_vol *vol;

	/* Paged backend (paged != 0 only): entries come from the table file
	   through pg, which was opened and validated by the caller (magic,
	   CRC, order, identity, epoch). sr never touches table/strings then. */
	int paged;
	pg_ctx *pg;

	/* GENERATED extents read here: staged slices in reserved memory,
	   addressed as genBase + genOff. Zero when the table has none. */
	const unsigned char *genBase;
	unsigned int genSize;

	/* Declared virtual-disc bytes. The table must start at or above it:
	   anything below would shadow game data. */
	unsigned long long declSize;

	/* The last file served, kept open: sequential reads inside one file
	   dominate, and reopening would redo the directory walk and lose the
	   cluster position. Keyed by entry index (resident) or extent disc
	   offset (paged) - stable per extent either way. */
	unsigned long long cached_key;
	int cached_valid;
	rfat_file cached_file;
	/* Pager path scratch: pg_path resolves here during lookup, and the
	   open below consumes it before the next lookup runs. BSS, not
	   stack: the DI thread it runs on is small. */
	char pathname[512];   /* scratch for pager path resolution on open */

	/* Counters, so tests can prove the cache is doing its job. */
	unsigned int opens;
	unsigned int lookups;
} sr_ctx;

/* Validate and adopt the table. Refuses (never trusts): bad magic/version/
   header/total/crc, order/overlap/wrap, bad string references, identity
   mismatch against expDiscId/expPartIdx, table reaching below declSize,
   GENERATED extents without a store, empty table. All refusals leave the
   context untouched for the caller to fall back. */
int sr_init(sr_ctx *c, const void *table, unsigned int table_len,
			rfat_vol *vol, const void *genBase, unsigned int genSize,
			unsigned long long declSize,
			unsigned int expDiscId, unsigned int expPartIdx);

/* Paged adoption: entries come from an opened, validated pg_ctx instead
   of resident memory. Identity/epoch/CRC/order were checked at pg_open
   against the install's values; sr re-checks nothing except liveness
   (non-null, opened, same volume). All refusals leave the context
   untouched for the caller to fall back. */
int sr_init_paged(sr_ctx *c, pg_ctx *pg, rfat_vol *vol,
				  unsigned long long declSize);

/* Lowest and highest disc offset the table covers. Two comparisons decide
   whether a read is ours at all. */
void sr_range(const sr_ctx *c, unsigned long long *lo, unsigned long long *hi);

/* Serve a read. Returns SR_MISS if the range touches nothing in the table,
   in which case nothing has been written to buf. In-region gaps and the tail
   past the last file read back as zero - the same observable behavior as the
   whole-file runtime and as unmapped declared space on a stock read. Any
   sub-read failure returns SR_EIO with nothing further attempted; the caller
   must discard the buffer, never serve it partial. */
int sr_read(sr_ctx *c, unsigned long long offset, unsigned int len, void *buf);

#endif
