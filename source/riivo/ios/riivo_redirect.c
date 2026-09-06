/* See riivo_redirect.h for the table layout and why this exists. */
#include "riivo_redirect.h"

static unsigned int rd32le(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8)
		 | ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

static const unsigned char *entry_at(const rr_ctx *c, unsigned int i)
{
	return c->table + RR_HEADER + (unsigned int) i * RR_ENTRY_SIZE;
}

static unsigned long long entry_off(const rr_ctx *c, unsigned int i)
{
	const unsigned char *e = entry_at(c, i);
	return ((unsigned long long) rd32le(e + 4) << 32) | rd32le(e);
}

static unsigned int entry_len(const rr_ctx *c, unsigned int i)
{
	return rd32le(entry_at(c, i) + 8);
}

static const char *entry_path(const rr_ctx *c, unsigned int i)
{
	return c->strings + rd32le(entry_at(c, i) + 12);
}

int rr_init(rr_ctx *c, const void *table, unsigned int table_len, rfat_vol *vol)
{
	const unsigned char *t = (const unsigned char *) table;
	unsigned int i, str_off;
	unsigned long long prev_end = 0;

	if (!c || !t || !vol || table_len < RR_HEADER)
		return RR_EINVAL;

	c->table = t;
	c->vol = vol;
	c->cached_valid = 0;
	c->cached_index = 0;
	c->opens = 0;
	c->lookups = 0;

	if (rd32le(t) != RR_MAGIC)
		return RR_EBADTABLE;
	c->count = rd32le(t + 4);
	str_off = rd32le(t + 8);

	/* Everything the table claims has to fit inside the buffer we were
	   given. A table that points past its own end is the one failure mode
	   that would read arbitrary IOS memory as a filename. */
	if (c->count == 0 || c->count > 65535)
		return RR_EBADTABLE;
	if (RR_HEADER + c->count * RR_ENTRY_SIZE > table_len)
		return RR_EBADTABLE;
	if (str_off < RR_HEADER + c->count * RR_ENTRY_SIZE || str_off >= table_len)
		return RR_EBADTABLE;
	c->strings = (const char *) (t + str_off);

	for (i = 0; i < c->count; ++i)
	{
		unsigned long long off = entry_off(c, i);
		unsigned int len = entry_len(c, i);
		unsigned int poff = rd32le(entry_at(c, i) + 12);
		unsigned int k;
		int terminated = 0;

		/* Sorted and non-overlapping, checked rather than trusted: out of
		   order would break the search, overlap would serve the wrong file. */
		if (i > 0 && off < prev_end)
			return RR_EBADTABLE;
		if (off + len < off)
			return RR_EBADTABLE;
		prev_end = off + len;

		if (str_off + poff >= table_len)
			return RR_EBADTABLE;
		for (k = str_off + poff; k < table_len; ++k)
		{
			if (t[k] == 0)
			{
				terminated = 1;
				break;
			}
		}
		if (!terminated)
			return RR_EBADTABLE;   /* a path running off the end */
	}
	return RR_OK;
}

void rr_range(const rr_ctx *c, unsigned long long *lo, unsigned long long *hi)
{
	if (!c || !c->count)
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

/* Index of the entry containing `off`, or the first entry starting after it,
   or count. Binary search: a table can hold thousands of files and this runs
   on the DI thread with the game waiting. */
static unsigned int find_index(const rr_ctx *c, unsigned long long off)
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

static int open_entry(rr_ctx *c, unsigned int i)
{
	int rc;
	if (c->cached_valid && c->cached_index == i)
		return RR_OK;
	rc = rfat_open(c->vol, entry_path(c, i), &c->cached_file);
	if (rc != RFAT_OK)
	{
		c->cached_valid = 0;
		return RR_EIO;
	}
	c->cached_index = i;
	c->cached_valid = 1;
	++c->opens;
	return RR_OK;
}

int rr_read(rr_ctx *c, unsigned long long offset, unsigned int len, void *buf)
{
	unsigned char *out = (unsigned char *) buf;
	unsigned long long lo, hi, pos;
	unsigned int done = 0;

	if (!c || !buf || !c->count)
		return RR_EINVAL;
	if (len == 0)
		return RR_OK;

	rr_range(c, &lo, &hi);
	if (offset + len <= lo || offset >= hi)
		return RR_MISS;

	++c->lookups;
	pos = offset;

	while (done < len)
	{
		unsigned int idx = find_index(c, pos);
		unsigned int take, got = 0;
		unsigned long long e_off, e_end;
		int rc;

		if (idx >= c->count)
		{
			/* Past the last file: the rest of the request is padding. */
			while (done < len)
				out[done++] = 0;
			break;
		}

		e_off = entry_off(c, idx);
		e_end = e_off + entry_len(c, idx);

		if (pos < e_off)
		{
			/* A gap between two placed files. The disc would read as zero
			   here, so that is what it reads as. */
			unsigned long long gap = e_off - pos;
			take = (gap > (unsigned long long) (len - done))
				   ? (len - done) : (unsigned int) gap;
			while (take--)
				out[done++] = 0;
			pos = offset + done;
			continue;
		}

		take = (e_end - pos > (unsigned long long) (len - done))
			   ? (len - done) : (unsigned int) (e_end - pos);

		rc = open_entry(c, idx);
		if (rc != RR_OK)
			return rc;

		rc = rfat_read(&c->cached_file, (unsigned int) (pos - e_off),
					   out + done, take, &got);
		if (rc != RFAT_OK)
		{
			c->cached_valid = 0;
			return RR_EIO;
		}
		/* The placed extent is rounded up to a sector, so the tail past the
		   file's real end is padding and reads as zero. rfat_read reports a
		   short count there rather than failing. */
		while (got < take)
			out[done + got++] = 0;

		done += take;
		pos = offset + done;
	}
	return RR_OK;
}
