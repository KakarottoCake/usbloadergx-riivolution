/* Serving synthetic disc offsets out of files on the card.
 *
 * The PPC side lays the mod's files out in a region of the virtual disc and
 * rebuilds the game's file table to point at them - code that already exists
 * and is well tested. What it hands over here is a table saying, for each
 * placed file, where it sits on that virtual disc and what its path is. This
 * side resolves a read against that table and pulls the bytes off the card by
 * path, on demand, which is the whole point: nothing walks a cluster chain
 * until the game actually asks for the file.
 *
 * Table layout (little-endian, built by the PPC side, read-only here):
 *
 *   +0   magic 'RIIV'
 *   +4   count of entries
 *   +8   byte offset from the table base to the string blob
 *   +12  FAT partition LBA, or 0xFFFFFFFF meaning "discover it"
 *   +16  entries[count], sorted ascending by disc offset, non-overlapping:
 *          +0  disc offset, low 32 bits
 *          +4  disc offset, high 32 bits
 *          +8  file length in bytes
 *          +12 byte offset into the string blob of a NUL-terminated path
 *
 * Sorted and non-overlapping is checked at init, not assumed: a bad table
 * would otherwise show up as a game reading the wrong file.
 */
#ifndef RIIVO_REDIRECT_H_
#define RIIVO_REDIRECT_H_

#include "riivo_fat.h"

#define RR_MAGIC      0x56494952u   /* 'RIIV' little-endian */
#define RR_ENTRY_SIZE 16
#define RR_HEADER     16

#define RR_OK          0
#define RR_EINVAL     -1
#define RR_EBADTABLE  -2
#define RR_MISS       -3   /* not ours; the caller should do a normal read */
#define RR_EIO        -4
#define RR_GAP        -5   /* range crosses unlisted disc; delegate whole */

typedef struct
{
	const unsigned char *table;
	unsigned int count;
	const char *strings;
	rfat_vol *vol;

	/* The last file served, kept open. Sequential reads inside one file are
	   the common case by far, and reopening would redo the directory walk
	   and lose the cluster position - which is exactly the cost this design
	   exists to avoid. */
	unsigned int cached_index;
	int cached_valid;
	rfat_file cached_file;

	/* Counters, so tests can prove the cache is doing its job. */
	unsigned int opens;
	unsigned int lookups;
} rr_ctx;

int rr_init(rr_ctx *c, const void *table, unsigned int table_len, rfat_vol *vol);

/* Lowest and highest disc offset the table covers. The DI hook uses these to
   decide in two comparisons whether a read is ours at all. */
void rr_range(const rr_ctx *c, unsigned long long *lo, unsigned long long *hi);

/* Serve a read. Returns RR_MISS if the range touches nothing in the table, in
   which case nothing has been written to buf. Entries match their backing
   files exactly, so a short backing file is truncation and fails EIO -
   never zero-padded; intentional padding is explicit ZERO extents in the
   segment runtime. Anything UNLISTED inside the range stops the read with
   RR_GAP: the caller must delegate the whole request to the stock path,
   never serve it partial. rr_covers answers the same question without
   writing anything, so the dispatcher asks first and serves only fully
   covered requests. */
int rr_read(rr_ctx *c, unsigned long long offset, unsigned int len, void *buf);

/* Coverage query: 1 if every byte of [off, off+len) sits in a listed
   file, 0 if any byte is unlisted, the range wraps, or the context is
   unusable. Opens no files, writes no buffers. */
int rr_covers(rr_ctx *c, unsigned long long off, unsigned int len);

#endif
