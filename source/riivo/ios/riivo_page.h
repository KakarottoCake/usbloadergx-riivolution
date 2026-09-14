/* Paged RIV1 table access for the IOS side. See riivo_page.c for the
 * contract.
 *
 * A resident table costs ~140 KB @2802 extents, and no game-RAM region
 * provably survives startup (see ../STORAGE_SURVIVAL.md). This keeps
 * ~4.6 KB resident - index, one page, one path scratch - and fetches the
 * rest from the table file on the mod volume through the FAT reader the
 * module already carries. Pages are 4 KiB slices of the manifest's own
 * entry array (same 32-byte encoding, same order), so no second format
 * exists to drift: the PPC emitter slices what BuildManifestV1 built.
 *
 * File layout (all integers little-endian):
 *   [0]    u32 magic 'PG1P', u16 version 1, u16 pageShift 12
 *   [8]    u32 nPages, u32 nEntries
 *   [16]   u32 pagesOff (512-aligned), u32 blobOff
 *   [24]   u32 crc32 over [512, fileSize), u32 reserved (0)
 *   [32]   u32 discId, u32 partIdx (identity: stale file under a new
 *          boot refuses), u32 epoch (must equal the installed epoch)
 *   [44]   u64 rangeLo, u64 rangeHi (covered span, for fast reject)
 *   [60..511] reserved zeros (header block is 512 bytes)
 *   [512]  index: nPages x {u64 firstKey, u16 pageId, u16 pad} (12 B)
 *   [pagesOff]  pages: nPages x 4096 B entry slices
 *   [blobOff]   string blob (manifest verbatim)
 * Total file capped at 4 MB; index rows capped so the resident index
 * stays <= 4 KB. The open-time CRC covers index, pages, and blob: a
 * corrupt table refuses before anything is served from it.
 */
#ifndef RIIVO_PAGE_H_
#define RIIVO_PAGE_H_

#include "riivo_fat.h"

#define PG_MAGIC      0x50314750u   /* 'PG1P' little-endian */
#define PG_VERSION    1
#define PG_SHIFT      12            /* 4096-byte pages */
#define PG_SIZE       4096
#define PG_PER_PAGE   128           /* 32-byte entries per page */
#define PG_INDEX_MAX  341           /* pages: 341*128 = 43648 extents max */
#define PG_PATH_MAX   512

#define PG_OK         0
#define PG_MISS       1   /* no extent contains the offset; not an error */
#define PG_EIO       -1
#define PG_EINVAL    -2
#define PG_EBADTABLE -3

/* One decoded extent. Field layout matches the manifest entry encoding
   (RiivoManifest); the pager never reinterprets it. */
typedef struct
{
	unsigned long long discOffset;
	unsigned int length;
	unsigned short kind;
	unsigned short source;
	unsigned long long srcOffset;
	unsigned int pathOff;   /* string-blob offset, resolved via pg_path */
	unsigned int genOff;
} pg_entry;

/* One resident index row: the first disc offset on a page. */
typedef struct
{
	unsigned long long first;
	unsigned short page;
	unsigned short pad;
} pg_idx;

typedef struct
{
	/* Storage. The file stays open for the boot; the volume outlives us. */
	rfat_file file;
	unsigned int nPages;
	unsigned int nEntries;
	unsigned int pagesOff;
	unsigned int blobOff;
	unsigned int fileSize;
	unsigned long long rangeLo;
	unsigned long long rangeHi;
	/* Resident caller buffers: index (nPages rows), one page, path tmp. */
	pg_idx *index;
	unsigned int indexCap;
	unsigned char *page;
	unsigned int cachedPage;
	int cachedValid;
	char path[512];
	/* Counters, so tests (and the boot log) can prove fetch behavior. */
	unsigned int fetches;
	unsigned int pathReads;
} pg_ctx;

/* Open and validate: magic, version, shift, counts vs cap, identity
   (disc/part/epoch against the install's values - a stale file from an
   earlier boot refuses here, not at serve time), offsets inside the
   file, index order and page ids, then the CRC over [512, fileSize).
   Reads the index once into the caller buffer. Refuses without touching
   anything else on any failure. */
int pg_open(pg_ctx *c, rfat_vol *vol, const char *path,
			pg_idx *idxBuf, unsigned int idxCap,
			unsigned char *pageBuf,
			unsigned int expDiscId, unsigned int expPartIdx,
			unsigned int expEpoch);

/* Locate the extent containing `off`. PG_MISS when none does (a gap the
   caller delegates, exactly like an unlisted manifest range). At most one
   page fetch; a cached page costs none. Never partial: fields or nothing. */
int pg_locate(pg_ctx *c, unsigned long long off, pg_entry *out);

/* Covered span for fast reject (from the validated header, no fetch). */
void pg_range(const pg_ctx *c, unsigned long long *lo, unsigned long long *hi);

/* First extent start strictly above `off`, or ~(u64)0 if none: the gap
   bound the router needs after a MISS inside the span. Index-only unless
   the answer hides mid-page (at most one fetch). */
unsigned long long pg_next(pg_ctx *c, unsigned long long off);

/* Resolve a blob path into buf (always NUL-terminated on success).
   Bounded by the file size and PG_PATH_MAX; refuses otherwise. */
int pg_path(pg_ctx *c, unsigned int blobOff, char *buf, unsigned int bufLen);

#endif
