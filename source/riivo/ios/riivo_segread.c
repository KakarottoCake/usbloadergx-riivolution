/* RIV1 segment reader: the runtime half of the manifest contract.
 *
 * What this is: the loader composes every enabled file patch into segments
 * (original / external + source offset / zero) and stages the diverted runs
 * as an RIV1 table. This side resolves a game read against that table and
 * pulls bytes from the card by path (external), from the reserved store
 * (generated), or as zeros - splitting across runs the way the contract
 * requires. Nothing is mapped to sectors up front; a file is opened when the
 * game first asks inside it, which is the whole point of the on-demand path.
 *
 * What this is not: a second layout that happens to agree. Extent order,
 * overlap, encoding, and validation mirror the loader's builder exactly
 * (RiivoManifest), and the host suite feeds this code production-built
 * tables, including corrupted ones.
 *
 * Failure posture, stated once: init validates everything the loader claims
 * (a table pointing past its own end would read arbitrary IOS memory as a
 * filename). Reads that touch nothing return MISS with the buffer untouched
 * so the caller runs the stock path. Anything failing mid-read returns EIO
 * with nothing further attempted - the caller must discard the buffer, never
 * boot or serve it partial. A table reaching below the declared size is
 * refused at init: it would shadow game data.
 *
 * Constraints, from the IOS side rather than taste (same as riivo_fat.c):
 * no libc, no allocation, no recursion. Field assembly is by hand, never by
 * struct cast: PPC wrote these bytes little-endian and Starlet reads them
 * big-endian.
 */
#include "riivo_segread.h"

static unsigned int rd16le(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

static unsigned int rd32le(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8)
		 | ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

static unsigned long long rd64le(const unsigned char *p)
{
	return (unsigned long long) rd32le(p)
		 | ((unsigned long long) rd32le(p + 4) << 32);
}

static unsigned int sr_crc32(const unsigned char *d, unsigned int n)
{
	unsigned int crc = 0xFFFFFFFFu;
	unsigned int i;
	int k;
	for (i = 0; i < n; ++i)
	{
		unsigned char b = (i >= 16 && i < 20) ? 0 : d[i];
		crc ^= b;
		for (k = 0; k < 8; ++k)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return crc ^ 0xFFFFFFFFu;
}

static const unsigned char *entry_at(const sr_ctx *c, unsigned int i)
{
	return c->table + SR_HEADER + (unsigned int) i * SR_ENTRY_SIZE;
}

static unsigned long long entry_off(const sr_ctx *c, unsigned int i)
{
	const unsigned char *e = entry_at(c, i);
	return rd64le(e);
}

static unsigned int entry_len(const sr_ctx *c, unsigned int i)
{
	return rd32le(entry_at(c, i) + 8);
}

static unsigned int entry_kind(const sr_ctx *c, unsigned int i)
{
	return rd16le(entry_at(c, i) + 12);
}

/* Index of the entry containing `off`, or the first entry starting after it,
   or count. Binary search: a table can hold thousands of extents and this
   runs on the DI thread with the game waiting. Resident backend only; the
   pager answers through its own index. */
static unsigned int find_index(const sr_ctx *c, unsigned long long off)
{
	unsigned int lo = 0, hi = c->count;
	while (lo < hi)
	{
		unsigned int mid = lo + (hi - lo) / 2;
		if (entry_off(c, mid) + entry_len(c, mid) <= off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int sr_init(sr_ctx *c, const void *table, unsigned int table_len,
			rfat_vol *vol, const void *genBase, unsigned int genSize,
			unsigned long long declSize,
			unsigned int expDiscId, unsigned int expPartIdx)
{
	const unsigned char *t = (const unsigned char *) table;
	unsigned int i, str_off, count;
	unsigned long long prev_end = 0;
	int have_generated = 0;

	if (!c || !t || !vol || table_len < SR_HEADER)
		return SR_EINVAL;

	if (rd32le(t) != SR_MAGIC)
		return SR_EBADTABLE;
	if (rd16le(t + 4) != SR_VERSION)
		return SR_EBADTABLE;
	if (rd16le(t + 6) != SR_HEADER)
		return SR_EBADTABLE;
	if (rd32le(t + 8) != table_len || table_len < SR_HEADER)
		return SR_EBADTABLE;
	if (rd32le(t + 44) != 0)
		return SR_EBADTABLE;

	count = rd32le(t + 20);
	str_off = rd32le(t + 24);
	if (count == 0 || count > 65535)
		return SR_EBADTABLE;
	if (str_off < SR_HEADER + count * SR_ENTRY_SIZE || str_off > table_len)
		return SR_EBADTABLE;
	if (sr_crc32(t, table_len) != rd32le(t + 16))
		return SR_EBADTABLE;

	/* Identity: this table was staged for one game and one partition.
	   Anything else is a stale table under a new boot, refused whole. */
	if (rd32le(t + 32) != expDiscId || rd32le(t + 36) != expPartIdx)
		return SR_EBADTABLE;

	c->table = t;
	c->vol = vol;
	c->paged = 0;
	c->pg = 0;
	c->cached_valid = 0;
	c->cached_key = 0;
	c->opens = 0;
	c->lookups = 0;
	c->strings = (const char *) (t + str_off);
	c->genBase = (const unsigned char *) genBase;
	c->genSize = genSize;
	c->declSize = declSize;
	c->count = count;

	for (i = 0; i < count; ++i)
	{
		unsigned long long off = entry_off(c, i);
		unsigned int len = entry_len(c, i);
		unsigned int kind = entry_kind(c, i);
		unsigned int poff = rd32le(entry_at(c, i) + 24);
		unsigned int k;
		int terminated = 0;

		if (kind != SR_EXT_EXTERNAL && kind != SR_EXT_GENERATED
			&& kind != SR_EXT_ZERO)
			return SR_EBADTABLE;
		/* Sorted and non-overlapping, checked rather than trusted. */
		if (i > 0 && off < prev_end)
			return SR_EBADTABLE;
		if (len > 0 && off + len < off)
			return SR_EBADTABLE;
		if (off + len > prev_end)
			prev_end = off + len;

		/* Anti-shadow: diverted bytes must live at or above the end of the
		   backup's own virtual disc. Below that they would replace game
		   data the table never meant to claim. */
		if (off < declSize)
			return SR_EBADTABLE;

		if (kind == SR_EXT_EXTERNAL)
		{
			unsigned int at = str_off + poff;
			if (at >= table_len)
				return SR_EBADTABLE;
			for (k = at; k < table_len; ++k)
			{
				if (t[k] == 0)
				{
					terminated = 1;
					break;
				}
			}
			if (!terminated)
				return SR_EBADTABLE;
			if (t[at] != '/')
				return SR_EBADTABLE;
		}
		else if (kind == SR_EXT_GENERATED)
		{
			have_generated = 1;
		}
	}

	/* Generated runs read staged slices; without the store they cannot be
	   served, so a table naming them is refused whole rather than served
	   partial. */
	if (have_generated && (!genBase || !genSize))
		return SR_EBADTABLE;

	return SR_OK;
}

int sr_init_paged(sr_ctx *c, pg_ctx *pg, rfat_vol *vol,
				  unsigned long long declSize)
{
	unsigned long long lo = 0, hi = 0;
	if (!c || !pg || !vol)
		return SR_EINVAL;
	if (!pg->index || pg->nPages == 0 || pg->nEntries == 0)
		return SR_EBADTABLE;
	/* Anti-shadow, same rule as the resident path: diverted bytes must
	   live at or above the end of the backup's own virtual disc. */
	pg_range(pg, &lo, &hi);
	if (lo < declSize)
		return SR_EBADTABLE;
	c->table = 0;
	c->count = 0;
	c->strings = 0;
	c->vol = vol;
	c->paged = 1;
	c->pg = pg;
	c->genBase = 0;
	c->genSize = 0;
	c->declSize = declSize;
	c->cached_valid = 0;
	c->cached_key = 0;
	c->opens = 0;
	c->lookups = 0;
	return SR_OK;
}

void sr_range(const sr_ctx *c, unsigned long long *lo, unsigned long long *hi)
{
	if (!c)
	{
		if (lo)
			*lo = 0;
		if (hi)
			*hi = 0;
		return;
	}
	if (c->paged)
	{
		if (!c->pg)
		{
			if (lo)
				*lo = 0;
			if (hi)
				*hi = 0;
			return;
		}
		pg_range(c->pg, lo, hi);
		return;
	}
	if (!c->count)
	{
		if (lo)
			*lo = 0;
		if (hi)
			*hi = 0;
		return;
	}
	if (lo)
		*lo = entry_off(c, 0);
	if (hi)
		*hi = entry_off(c, c->count - 1) + entry_len(c, c->count - 1);
}

/* Entry source: resident table or pager. One lookup shape for both, so
   the router below cannot drift between backends. FOUND fills every
   field; GAP means no extent contains pos (delegate); FAIL is corruption
   or storage failure (discard, never serve partial). */
typedef struct
{
	unsigned long long off;
	unsigned int length;
	unsigned int kind;
	unsigned long long src;
	unsigned int pathOff;
	unsigned int genOff;
	unsigned int source;
	unsigned int idx;       /* resident entry index (open-cache key) */
	const char *path;
} sentry;

enum { SF_FOUND = 0, SF_GAP = 1, SF_FAIL = 2 };

static unsigned int s_count(const sr_ctx *c)
{
	if (c->paged)
		return c->pg ? c->pg->nEntries : 0;
	return c->count;
}

static int s_locate(sr_ctx *c, unsigned long long pos, sentry *out,
					unsigned long long *gapNext)
{
	if (c->paged)
	{
		pg_entry pe;
		int rc;
		if (!c->pg)
			return SF_FAIL;
		rc = pg_locate(c->pg, pos, &pe);
		if (rc == PG_OK)
		{
			out->off = pe.discOffset;
			out->length = pe.length;
			out->kind = pe.kind;
			out->src = pe.srcOffset;
			out->pathOff = pe.pathOff;
			out->genOff = pe.genOff;
			out->source = pe.source;
			out->idx = 0;
			if (pe.kind == SR_EXT_EXTERNAL)
			{
				if (pg_path(c->pg, pe.pathOff, c->pathname,
							sizeof(c->pathname)) != PG_OK)
					return SF_FAIL;
				out->path = c->pathname;
			}
			else
				out->path = "";
			return SF_FOUND;
		}
		if (rc == PG_MISS)
		{
			*gapNext = pg_next(c->pg, pos);
			return SF_GAP;
		}
		return SF_FAIL;
	}
	{
		unsigned int idx = find_index(c, pos);
		unsigned long long e_off;
		const unsigned char *e;
		if (idx >= c->count)
		{
			*gapNext = ~(unsigned long long) 0;
			return SF_GAP;
		}
		e = entry_at(c, idx);
		e_off = rd64le(e);
		if (pos < e_off)
		{
			*gapNext = e_off;
			return SF_GAP;
		}
		out->off = e_off;
		out->length = rd32le(e + 8);
		out->kind = rd16le(e + 12);
		out->src = rd64le(e + 16);
		out->pathOff = rd32le(e + 24);
		out->genOff = rd32le(e + 28);
		out->source = rd16le(e + 14);
		out->path = c->strings + out->pathOff;
		out->idx = idx;
		return SF_FOUND;
	}
}

static int open_entry(sr_ctx *c, unsigned long long key, const char *path)
{
	int rc;
	if (c->cached_valid && c->cached_key == key)
		return SR_OK;
	rc = rfat_open(c->vol, path, &c->cached_file);
	if (rc != RFAT_OK)
	{
		c->cached_valid = 0;
		return SR_EIO;
	}
	c->cached_key = key;
	c->cached_valid = 1;
	++c->opens;
	return SR_OK;
}

int sr_read(sr_ctx *c, unsigned long long offset, unsigned int len, void *buf)
{
	unsigned char *out = (unsigned char *) buf;
	unsigned long long lo, hi, pos;
	unsigned int done = 0;

	if (!c || !buf || s_count(c) == 0)
		return SR_EINVAL;
	if (len == 0)
		return SR_OK;

	sr_range(c, &lo, &hi);
	if (offset + len <= lo || offset >= hi)
		return SR_MISS;

	++c->lookups;
	pos = offset;

	while (done < len)
	{
		sentry s;
		unsigned long long gapNext = 0;
		unsigned int take, got = 0;
		unsigned long long e_off, e_end, src;
		unsigned long long key;
		int fr, rc;

		fr = s_locate(c, pos, &s, &gapNext);
		if (fr == SF_FAIL)
			return SR_EIO;
		if (fr == SF_GAP)
		{
			/* Past the last extent, or a gap between placed extents:
			   the rest (or run) reads as zero padding. */
			unsigned long long fillEnd = gapNext;
			unsigned long long reqEnd = offset + len;
			if (fillEnd > reqEnd || gapNext == ~(unsigned long long) 0)
				fillEnd = reqEnd;
			take = (unsigned int) (fillEnd - pos);
			while (take--)
				out[done++] = 0;
			pos = offset + done;
			continue;
		}

		e_off = s.off;
		e_end = e_off + s.length;
		key = c->paged ? s.off : (unsigned long long) s.idx;
		take = (e_end - pos > (unsigned long long) (len - done))
			   ? (len - done) : (unsigned int) (e_end - pos);

		if (s.kind == SR_EXT_ZERO)
		{
			while (got < take)
				out[done + got++] = 0;
			done += take;
			pos = offset + done;
			continue;
		}

		if (s.kind == SR_EXT_GENERATED)
		{
			unsigned long long go = (unsigned long long) s.genOff
								  + (pos - e_off);
			unsigned int k;
			/* The store was bounds-checked at init for presence, not per
			   extent; check each run, because a corrupt length reaching
			   here would otherwise read past reserved memory. */
			if (!c->genBase || go + take > c->genSize)
				return SR_EIO;
			for (k = 0; k < take; ++k)
				out[done + k] = c->genBase[(unsigned int) go + k];
			done += take;
			pos = offset + done;
			continue;
		}

		rc = open_entry(c, key, s.path);
		if (rc != SR_OK)
			return rc;

		/* Source offset advances with the intra-run position: a partial
		   segment starts mid-file, and a read starting mid-extent starts
		   further in still. */
		src = s.src + (pos - e_off);
		if (src + take < src || src + take > 0xFFFFFFFFu)
			return SR_EIO;
		rc = rfat_read(&c->cached_file, (unsigned int) src,
					   out + done, take, &got);
		if (rc != RFAT_OK)
		{
			c->cached_valid = 0;
			return SR_EIO;
		}
		/* The placed extent is rounded up to a sector, so the tail past the
		   file's real end is padding and reads as zero. rfat_read reports a
		   short count there rather than failing. */
		while (got < take)
			out[done + got++] = 0;

		done += take;
		pos = offset + done;
	}

	return SR_OK;
}
