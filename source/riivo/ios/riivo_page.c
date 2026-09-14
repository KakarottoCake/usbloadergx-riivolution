/* Paged RIV1 table access: the runtime half of the storage contract.
 *
 * Why this exists: a resident table costs ~140 KB at 2802 extents and no
 * game-RAM region provably survives startup (see STORAGE_SURVIVAL:
 * bulk-zeroed below a fixed arena while the words stay put). This keeps
 * one index (~10 bytes/page, capped), one 4 KiB page, and one path
 * scratch resident, and fetches everything else from the table file the
 * PPC staged on the mod volume. Worst case per lookup is one page fetch;
 * a spanning read touches at most two, served sequentially through the
 * same buffer - never held simultaneously.
 *
 * What this is not: a second table format. Pages are verbatim 4 KiB
 * slices of the manifest entry array the PPC emitter sliced from a
 * validated RIV1 blob (same 32-byte encoding, same order), and the
 * string blob rides after the pages untouched. The host suite proves
 * paged lookups equal resident scans over production-built tables.
 *
 * Failure posture, stated once: corrupt headers refuse at open (never a
 * half-adopted table); a failed page fetch fails the lookup (the caller
 * discards its buffer, never serves partial); a gap is PG_MISS, which is
 * delegation, not an error. No libc, no allocation, no recursion; every
 * loop is bounded by validated counts.
 */
#include "riivo_page.h"

#define PG_FILE_MAX (4u << 20)   /* total table file cap */
#define PG_INDEX_OFF 512          /* header block size; index starts here */

static unsigned int pg_rd16(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

static unsigned int pg_rd32(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8)
		 | ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

static unsigned long long pg_rd64(const unsigned char *p)
{
	return (unsigned long long) pg_rd32(p)
		 | ((unsigned long long) pg_rd32(p + 4) << 32);
}

/* One file-range read through the open handle. Short counts are failure:
   the table file is written once pre-boot and never grows. */
static int pg_pread(pg_ctx *c, unsigned int off, void *buf, unsigned int len)
{
	unsigned int got = 0;
	if (!c || !buf)
		return PG_EINVAL;
	if (len == 0)
		return PG_OK;
	if (off + len < off || off + len > c->fileSize)
		return PG_EIO;
	if (rfat_read(&c->file, off, buf, len, &got) != RFAT_OK || got != len)
		return PG_EIO;
	return PG_OK;
}

int pg_open(pg_ctx *c, rfat_vol *vol, const char *path,
			pg_idx *idxBuf, unsigned int idxCap,
			unsigned char *pageBuf, char *pathBuf)
{
	unsigned char head[32];
	unsigned int i, pagesOff, blobOff, wantCrc;
	unsigned int crc, at;
	unsigned long long prevFirst;

	if (!c || !vol || !path || !idxBuf || !pageBuf || !pathBuf)
		return PG_EINVAL;
	c->cachedValid = 0;
	c->cachedPage = 0;
	c->fetches = 0;
	c->pathReads = 0;
	c->index = 0;
	c->page = 0;
	if (rfat_open(vol, path, &c->file) != RFAT_OK)
		return PG_EIO;
	c->fileSize = c->file.size;
	if (c->fileSize < 512 || c->fileSize > PG_FILE_MAX)
		return PG_EBADTABLE;
	if (pg_pread(c, 0, head, 32) != PG_OK)
		return PG_EBADTABLE;
	if (pg_rd32(head) != PG_MAGIC)
		return PG_EBADTABLE;
	if (pg_rd16(head + 4) != PG_VERSION)
		return PG_EBADTABLE;
	if (pg_rd16(head + 6) != PG_SHIFT)
		return PG_EBADTABLE;
	c->nPages = pg_rd32(head + 8);
	c->nEntries = pg_rd32(head + 12);
	pagesOff = pg_rd32(head + 16);
	blobOff = pg_rd32(head + 20);
	wantCrc = pg_rd32(head + 24);
	if (pg_rd32(head + 28) != 0)
		return PG_EBADTABLE;
	if (c->nPages == 0 || c->nPages > PG_INDEX_MAX)
		return PG_EBADTABLE;
	if (c->nPages > idxCap)
		return PG_EBADTABLE;
	if (c->nEntries == 0 || c->nEntries > (unsigned int) c->nPages * PG_PER_PAGE)
		return PG_EBADTABLE;
	if (pagesOff < PG_INDEX_OFF + (unsigned int) c->nPages * 12
		|| (pagesOff & 511) != 0)
		return PG_EBADTABLE;
	if (pagesOff + (unsigned int) c->nPages * PG_SIZE < pagesOff)
		return PG_EBADTABLE;
	if (blobOff < pagesOff + (unsigned int) c->nPages * PG_SIZE)
		return PG_EBADTABLE;
	if (blobOff > c->fileSize)
		return PG_EBADTABLE;
	/* Index rows: 12 bytes each, firstKeys strictly ordered, page ids
	   identity (transposition would route reads to the wrong page). */
	prevFirst = 0;
	for (i = 0; i < c->nPages; ++i)
	{
		unsigned char row[12];
		unsigned long long first;
		unsigned int page;
		if (pg_pread(c, PG_INDEX_OFF + i * 12, row, 12) != PG_OK)
			return PG_EBADTABLE;
		first = pg_rd64(row);
		page = pg_rd16(row + 8);
		if (page != i)
			return PG_EBADTABLE;
		if (i > 0 && first <= prevFirst)
			return PG_EBADTABLE;
		idxBuf[i].first = first;
		idxBuf[i].page = (unsigned short) page;
		idxBuf[i].pad = 0;
		prevFirst = first;
	}
	/* Whole-file CRC over everything past the header block: index, pages,
	   blob. One sequential pass at open, streamed through the caller's
	   page buffer (idle until the first fetch); corrupt tables refuse
	   before anything is served from them. */
	crc = 0xFFFFFFFFu;
	for (at = PG_INDEX_OFF; at < c->fileSize; at += PG_SIZE)
	{
		unsigned int n = c->fileSize - at;
		unsigned int k;
		if (n > PG_SIZE)
			n = PG_SIZE;
		if (pg_pread(c, at, pageBuf, n) != PG_OK)
			return PG_EBADTABLE;
		for (k = 0; k < n; ++k)
		{
			crc ^= pageBuf[k];
			{
				int b;
				for (b = 0; b < 8; ++b)
					crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
			}
		}
	}
	if ((crc ^ 0xFFFFFFFFu) != wantCrc)
		return PG_EBADTABLE;
	c->index = idxBuf;
	c->indexCap = idxCap;
	c->page = pageBuf;
	c->pathTmp = pathBuf;
	c->pagesOff = pagesOff;
	c->blobOff = blobOff;
	c->cachedValid = 0;
	return PG_OK;
}

static int pg_fetch(pg_ctx *c, unsigned int page)
{
	unsigned int got = 0;
	if (c->cachedValid && c->cachedPage == page)
		return PG_OK;
	if (page >= c->nPages)
		return PG_EIO;
	if (rfat_read(&c->file, c->pagesOff + page * PG_SIZE,
				  c->page, PG_SIZE, &got) != RFAT_OK || got != PG_SIZE)
		return PG_EIO;
	c->cachedPage = page;
	c->cachedValid = 1;
	++c->fetches;
	return PG_OK;
}

static void pg_decode(const unsigned char *e, pg_entry *out)
{
	out->discOffset = pg_rd64(e);
	out->length = pg_rd32(e + 8);
	/* kind+source share one word in this encoding, split like the reader */
	out->kind = pg_rd16(e + 12);
	out->source = pg_rd16(e + 14);
	out->srcOffset = pg_rd64(e + 16);
	out->pathOff = pg_rd32(e + 24);
	out->genOff = pg_rd32(e + 28);
}

int pg_locate(pg_ctx *c, unsigned long long off, pg_entry *out)
{
	unsigned int lo, hi, page;
	unsigned long long eoff;
	unsigned int elen;
	unsigned char *e;

	if (!c || !out || !c->index || !c->page)
		return PG_EINVAL;
	if (c->nPages == 0)
		return PG_EBADTABLE;
	/* Index bisection: last page whose first key is at or below off. */
	lo = 0;
	hi = c->nPages;
	while (lo + 1 < hi)
	{
		unsigned int mid = lo + (hi - lo) / 2;
		if (c->index[mid].first <= off)
			lo = mid;
		else
			hi = mid;
	}
	page = c->index[lo].page;
	if (pg_fetch(c, page) != PG_OK)
		return PG_EIO;
	/* Entry bisection inside the page. The last page may be partial:
	   entries past nEntries do not exist, whatever the bytes say. */
	lo = 0;
	hi = PG_PER_PAGE;
	{
		unsigned int base = page * PG_PER_PAGE;
		unsigned int have = c->nEntries > base ? c->nEntries - base : 0;
		if (have > PG_PER_PAGE)
			have = PG_PER_PAGE;
		hi = have;
	}
	while (lo < hi)
	{
		unsigned int mid = lo + (hi - lo) / 2;
		e = c->page + mid * 32;
		eoff = pg_rd64(e);
		elen = pg_rd32(e + 8);
		if (eoff + elen <= off && !(elen == 0 && eoff <= off))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= PG_PER_PAGE)
		return PG_MISS;
	{
		unsigned int base = page * PG_PER_PAGE;
		unsigned int have = c->nEntries > base ? c->nEntries - base : 0;
		if (have > PG_PER_PAGE)
			have = PG_PER_PAGE;
		if (lo >= have)
			return PG_MISS;
	}
	e = c->page + lo * 32;
	pg_decode(e, out);
	eoff = out->discOffset;
	elen = out->length;
	if (off < eoff || (elen > 0 && off >= eoff + elen))
		return PG_MISS;
	if (elen == 0 && off != eoff)
		return PG_MISS;
	return PG_OK;
}

int pg_path(pg_ctx *c, unsigned int blobOff, char *buf, unsigned int bufLen)
{
	unsigned int at, k;
	unsigned int got = 0;
	if (!c || !buf || bufLen == 0)
		return PG_EINVAL;
	at = c->blobOff + blobOff;
	/* Bounded walk: the blob ends where the file ends. */
	for (k = 0; k < bufLen && k < PG_PATH_MAX; ++k)
	{
		unsigned char ch;
		if (at + k >= c->fileSize)
			return PG_EIO;
		if (rfat_read(&c->file, at + k, &ch, 1, &got) != RFAT_OK || got != 1)
			return PG_EIO;
		++c->pathReads;
		buf[k] = (char) ch;
		if (ch == 0)
			return PG_OK;
	}
	return PG_EIO;
}
