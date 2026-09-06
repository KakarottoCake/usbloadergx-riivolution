/* Read-only FAT16/FAT32, sized to run inside IOS on the DI thread.
 *
 * Constraints this is written to, from the IOS side rather than taste:
 *   - No libc, no allocation, no recursion. The DI thread's stack is small
 *     (2-4 KB), so every buffer is static and the directory walk is a loop.
 *   - Every chain walk is bounded by the cluster count. A corrupt FAT must
 *     end the walk, not spin: a spin here hangs the console, because the game
 *     is blocked waiting on /dev/di.
 *   - Sector reads go through a caller-supplied function, so on the console
 *     this can call d2x's own block read and here it can read an image file.
 *
 * Compiles for the host and for ARM from the same source; the host build is
 * where the exhaustive tests run, the ARM build is what actually ships.
 */
#include "riivo_fat.h"

#define ATTR_READONLY  0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_LFN       0x0F

#define FAT_EOC_MIN32  0x0FFFFFF8u
#define FAT_EOC_MIN16  0xFFF8u

/* One sector of scratch, reused for FAT and directory reads. Static on
   purpose: 512 bytes on the DI thread's stack would be most of it. */
/* 32-byte aligned because the storage layer DMAs straight into it:
   Starlet's SD and USB engines are bus masters, and an unaligned
   destination is corrupt data rather than an error. */
static unsigned char g_sec[RFAT_SECTOR] __attribute__((aligned(32)));
static unsigned int g_sec_lba = 0xFFFFFFFFu;
static int g_sec_valid;

static unsigned short rd16(const unsigned char *p)
{
	return (unsigned short) (p[0] | (p[1] << 8));
}

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int) p[0] | ((unsigned int) p[1] << 8)
		 | ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

static int cache_sector(rfat_vol *v, unsigned int lba)
{
	if (g_sec_valid && g_sec_lba == lba)
		return 1;
	if (!v->read(v->ctx, lba, 1, g_sec))
	{
		g_sec_valid = 0;
		return 0;
	}
	g_sec_lba = lba;
	g_sec_valid = 1;
	return 1;
}

void rfat_drop_cache(void)
{
	g_sec_valid = 0;
	g_sec_lba = 0xFFFFFFFFu;
}

/* FAT partition types. 0x0B/0x0C are FAT32 (CHS and LBA), 0x04/0x06/0x0E are
   FAT16. 0x05/0x0F are extended containers, followed one level down - which
   is where a second FAT partition on a shipped-formatted drive often lives. */
static int is_fat_type(unsigned char t)
{
	return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C
		   || t == 0x0E;
}

static int is_ext_type(unsigned char t)
{
	return t == 0x05 || t == 0x0F || t == 0x85;
}

int rfat_find_partition(rfat_read_fn read, void *ctx, int index,
                        unsigned int *lba_out)
{
	/* Reuse the shared scratch sector rather than putting 512 bytes on
	   the stack: this runs on the DI thread, which has 2-4 KB total. */
	unsigned char *mbr = g_sec;
	unsigned int ext_base = 0, next_ext = 0;
	int found = 0, hop = 0, i;

	if (!read || !lba_out || index < 0)
		return RFAT_EINVAL;

	/* The shared buffer no longer holds what the sector cache thinks. */
	rfat_drop_cache();
	if (!read(ctx, 0, 1, mbr))
		return RFAT_EIO;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return RFAT_ENOFS;

	/* A card formatted without a partition table has its boot sector at LBA 0
	   and no table. Both start with 0x55AA, so the table is what tells them
	   apart: a real entry has a status of 0x00 or 0x80 and a non-zero type. */
	{
		int plausible = 0;
		for (i = 0; i < 4; ++i)
		{
			const unsigned char *e = mbr + 446 + i * 16;
			if ((e[0] == 0x00 || e[0] == 0x80) && e[4] != 0)
				plausible = 1;
		}
		if (!plausible)
		{
			if (index != 0)
				return RFAT_ENOENT;
			*lba_out = 0;          /* superfloppy */
			return RFAT_OK;
		}
	}

	for (;;)
	{
		next_ext = 0;
		for (i = 0; i < 4; ++i)
		{
			const unsigned char *e = mbr + 446 + i * 16;
			unsigned char type = e[4];
			unsigned int start = rd32(e + 8);
			if (e[0] != 0x00 && e[0] != 0x80)
				continue;                  /* not a valid entry */
			if (type == 0 || start == 0)
				continue;
			if (is_ext_type(type))
			{
				if (!next_ext)
					next_ext = ext_base ? ext_base + start : start;
				continue;
			}
			if (!is_fat_type(type))
				continue;
			if (found++ == index)
			{
				*lba_out = ext_base ? ext_base + start : start;
				return RFAT_OK;
			}
		}

		/* Extended partitions form a linked list; a corrupt one can point at
		   itself, so the hop count is what stops this rather than trust. */
		if (!next_ext || ++hop > 16)
			return RFAT_ENOENT;
		if (!ext_base)
			ext_base = next_ext;
		rfat_drop_cache();
		if (!read(ctx, next_ext, 1, mbr))
			return RFAT_EIO;
		if (mbr[510] != 0x55 || mbr[511] != 0xAA)
			return RFAT_ENOENT;
	}
}

int rfat_mount(rfat_vol *v, rfat_read_fn read, void *ctx, unsigned int part_lba)
{
	unsigned int root_sectors, fat_sz, tot_sec, data_sectors;

	if (!v || !read)
		return RFAT_EINVAL;

	v->read = read;
	v->ctx = ctx;
	v->part_lba = part_lba;
	rfat_drop_cache();

	if (!cache_sector(v, part_lba))
		return RFAT_EIO;

	/* A boot sector must end in 0x55AA, and the fields below must be sane.
	   Refusing here is how a non-FAT partition stops being our problem. */
	if (g_sec[510] != 0x55 || g_sec[511] != 0xAA)
		return RFAT_ENOFS;

	v->bytes_per_sec = rd16(g_sec + 11);
	v->sec_per_clus = g_sec[13];
	v->reserved = rd16(g_sec + 14);
	v->num_fats = g_sec[16];

	if (v->bytes_per_sec != RFAT_SECTOR)
		return RFAT_ENOFS;
	if (v->sec_per_clus == 0 || (v->sec_per_clus & (v->sec_per_clus - 1)))
		return RFAT_ENOFS;
	if (v->reserved == 0 || v->num_fats == 0)
		return RFAT_ENOFS;

	v->root_entries = rd16(g_sec + 17);
	fat_sz = rd16(g_sec + 22);
	if (fat_sz == 0)
		fat_sz = rd32(g_sec + 36);
	v->fat_sectors = fat_sz;
	if (fat_sz == 0)
		return RFAT_ENOFS;

	tot_sec = rd16(g_sec + 19);
	if (tot_sec == 0)
		tot_sec = rd32(g_sec + 32);
	if (tot_sec == 0)
		return RFAT_ENOFS;

	root_sectors = ((unsigned int) v->root_entries * 32 + RFAT_SECTOR - 1) / RFAT_SECTOR;
	v->first_data = part_lba + v->reserved + (unsigned int) v->num_fats * fat_sz
					+ root_sectors;
	if (v->first_data < part_lba || v->first_data - part_lba >= tot_sec)
		return RFAT_ENOFS;

	data_sectors = tot_sec - (v->reserved + (unsigned int) v->num_fats * fat_sz
							  + root_sectors);
	v->count_clusters = data_sectors / v->sec_per_clus;

	/* The cluster count is what decides FAT16 versus FAT32 - not the string
	   at offset 82, which is a comment and is routinely wrong. */
	v->is_fat32 = (v->count_clusters >= 65525) ? 1 : 0;
	if (v->count_clusters < 1)
		return RFAT_ENOFS;

	if (v->is_fat32)
	{
		v->root_cluster = rd32(g_sec + 44);
		if (v->root_cluster < 2 || v->root_cluster >= v->count_clusters + 2)
			return RFAT_ENOFS;
		v->root_lba = 0;
	}
	else
	{
		if (v->root_entries == 0)
			return RFAT_ENOFS;
		v->root_cluster = 0;
		v->root_lba = part_lba + v->reserved + (unsigned int) v->num_fats * fat_sz;
	}
	return RFAT_OK;
}

static unsigned int clus_lba(const rfat_vol *v, unsigned int c)
{
	return v->first_data + (c - 2) * v->sec_per_clus;
}

/* Next cluster in the chain, or 0 for "stop". Bad values become 0 rather
   than an error path, because every caller's response to both is to stop. */
static unsigned int next_cluster(rfat_vol *v, unsigned int c)
{
	unsigned int off, lba, val;

	if (c < 2 || c >= v->count_clusters + 2)
		return 0;

	if (v->is_fat32)
	{
		off = c * 4;
		lba = v->part_lba + v->reserved + off / RFAT_SECTOR;
		if (!cache_sector(v, lba))
			return 0;
		val = rd32(g_sec + (off % RFAT_SECTOR)) & 0x0FFFFFFFu;
		if (val >= FAT_EOC_MIN32 || val < 2 || val >= v->count_clusters + 2)
			return 0;
	}
	else
	{
		off = c * 2;
		lba = v->part_lba + v->reserved + off / RFAT_SECTOR;
		if (!cache_sector(v, lba))
			return 0;
		val = rd16(g_sec + (off % RFAT_SECTOR));
		if (val >= FAT_EOC_MIN16 || val < 2 || val >= v->count_clusters + 2)
			return 0;
	}
	return val;
}

static char upcase(char c)
{
	return (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
}

/* Rebuild the 8.3 name as "NAME.EXT" so it can be compared like any other. */
static void short_name(const unsigned char *e, char *out)
{
	int i, n = 0;
	for (i = 0; i < 8 && e[i] != ' '; ++i)
		out[n++] = (char) e[i];
	if (e[8] != ' ')
	{
		out[n++] = '.';
		for (i = 8; i < 11 && e[i] != ' '; ++i)
			out[n++] = (char) e[i];
	}
	out[n] = 0;
}

static unsigned char lfn_checksum(const unsigned char *short11)
{
	int i;
	unsigned char s = 0;
	for (i = 0; i < 11; ++i)
		s = (unsigned char) (((s & 1) << 7) + (s >> 1) + short11[i]);
	return s;
}

/* Case-insensitive compare of one path component. */
static int name_eq(const char *a, const char *b, unsigned int blen)
{
	unsigned int i = 0;
	for (; i < blen; ++i)
	{
		if (!a[i] || upcase(a[i]) != upcase(b[i]))
			return 0;
	}
	return a[i] == 0;
}

/* Copy the 13 UTF-16 units of one LFN slot into `out` as 8-bit characters.
   Anything above U+00FF becomes '?', which can never match a real name and so
   fails the comparison rather than matching the wrong file. */
static int lfn_chunk(const unsigned char *e, char *out)
{
	static const int off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	int i, n = 0;
	for (i = 0; i < 13; ++i)
	{
		unsigned short u = rd16(e + off[i]);
		if (u == 0x0000 || u == 0xFFFF)
			break;
		out[n++] = (u < 0x100) ? (char) u : '?';
	}
	return n;
}

/* Find `name` (length nlen) inside the directory starting at `dir_clus`, or
   in the FAT16 fixed root when dir_clus is 0. */
static int find_entry(rfat_vol *v, unsigned int dir_clus, const char *name,
					  unsigned int nlen, rfat_dirent *out)
{
	char lfn[RFAT_MAX_NAME + 1];
	int lfn_len = 0;
	unsigned char want_sum = 0;
	int have_lfn = 0;
	unsigned int guard = 0;
	unsigned int clus = dir_clus;
	unsigned int lba, sec_in, sectors;

	if (dir_clus == 0 && !v->is_fat32)
	{
		sectors = ((unsigned int) v->root_entries * 32 + RFAT_SECTOR - 1) / RFAT_SECTOR;
		lba = v->root_lba;
	}
	else
	{
		sectors = v->sec_per_clus;
		lba = clus_lba(v, clus);
	}

	for (;;)
	{
		for (sec_in = 0; sec_in < sectors; ++sec_in)
		{
			int i;
			if (!cache_sector(v, lba + sec_in))
				return RFAT_EIO;
			for (i = 0; i < RFAT_SECTOR; i += 32)
			{
				const unsigned char *e = g_sec + i;
				unsigned char attr;

				if (e[0] == 0x00)
					return RFAT_ENOENT;   /* no entries past here */
				if (e[0] == 0xE5)
				{
					have_lfn = 0;
					continue;
				}
				attr = e[11];
				if ((attr & ATTR_LFN) == ATTR_LFN)
				{
					int seq = e[0] & 0x1F;
					char part[14];
					int n, k, base;
					if (seq < 1 || seq > 20)
					{
						have_lfn = 0;
						continue;
					}
					if (e[0] & 0x40)
					{
						lfn_len = 0;
						have_lfn = 1;
						want_sum = e[13];
						for (k = 0; k < RFAT_MAX_NAME; ++k)
							lfn[k] = 0;
					}
					if (!have_lfn || e[13] != want_sum)
					{
						have_lfn = 0;
						continue;
					}
					n = lfn_chunk(e, part);
					base = (seq - 1) * 13;
					if (base + n > RFAT_MAX_NAME)
					{
						have_lfn = 0;
						continue;
					}
					for (k = 0; k < n; ++k)
						lfn[base + k] = part[k];
					if (base + n > lfn_len)
						lfn_len = base + n;
					continue;
				}
				if (attr & ATTR_VOLUME)
				{
					have_lfn = 0;
					continue;
				}

				/* A short entry ends whatever LFN run preceded it. The
				   checksum is what ties the two together; without it a
				   deleted-and-reused slot can graft the wrong name on. */
				if (have_lfn && lfn_checksum(e) == want_sum && lfn_len > 0)
				{
					lfn[lfn_len] = 0;
					if (nlen == (unsigned int) lfn_len && name_eq(lfn, name, nlen))
					{
						out->cluster = ((unsigned int) rd16(e + 20) << 16)
									   | rd16(e + 26);
						out->size = rd32(e + 28);
						out->is_dir = (attr & ATTR_DIR) ? 1 : 0;
						return RFAT_OK;
					}
				}
				have_lfn = 0;
				{
					char s83[13];
					short_name(e, s83);
					if (name_eq(s83, name, nlen))
					{
						out->cluster = ((unsigned int) rd16(e + 20) << 16)
									   | rd16(e + 26);
						out->size = rd32(e + 28);
						out->is_dir = (attr & ATTR_DIR) ? 1 : 0;
						return RFAT_OK;
					}
				}
			}
		}

		if (dir_clus == 0 && !v->is_fat32)
			return RFAT_ENOENT;           /* fixed root has no chain */

		clus = next_cluster(v, clus);
		if (clus == 0)
			return RFAT_ENOENT;
		if (++guard > v->count_clusters)
			return RFAT_ECORRUPT;         /* a chain that loops */
		lba = clus_lba(v, clus);
	}
}

int rfat_open(rfat_vol *v, const char *path, rfat_file *f)
{
	unsigned int clus;
	const char *p = path;
	rfat_dirent de;
	int rc;

	if (!v || !path || !f)
		return RFAT_EINVAL;

	clus = v->is_fat32 ? v->root_cluster : 0;
	de.cluster = clus;
	de.size = 0;
	de.is_dir = 1;

	while (*p == '/' || *p == '\\')
		++p;

	while (*p)
	{
		const char *start = p;
		unsigned int len = 0;
		while (*p && *p != '/' && *p != '\\')
		{
			++p;
			++len;
		}
		if (len == 0 || len > RFAT_MAX_NAME)
			return RFAT_EINVAL;

		if (!de.is_dir)
			return RFAT_ENOENT;           /* a path component under a file */

		rc = find_entry(v, clus, start, len, &de);
		if (rc != RFAT_OK)
			return rc;

		clus = de.cluster;
		while (*p == '/' || *p == '\\')
			++p;
	}

	if (de.is_dir)
		return RFAT_EISDIR;

	f->vol = v;
	f->first_cluster = de.cluster;
	f->size = de.size;
	/* Start the walk cache at the beginning; rfat_read only ever moves it
	   forward, and rewinds when asked for an earlier offset. */
	f->cur_cluster = de.cluster;
	f->cur_index = 0;
	return RFAT_OK;
}

int rfat_read(rfat_file *f, unsigned int offset, void *buf, unsigned int len,
			  unsigned int *got)
{
	rfat_vol *v;
	unsigned char *out = (unsigned char *) buf;
	unsigned int csize, index, done = 0, guard;

	if (got)
		*got = 0;
	if (!f || !f->vol || !buf)
		return RFAT_EINVAL;
	v = f->vol;

	if (offset >= f->size || len == 0)
		return RFAT_OK;                   /* nothing to do, not an error */
	if (len > f->size - offset)
		len = f->size - offset;
	if (f->first_cluster < 2)
		return RFAT_ECORRUPT;             /* non-empty file with no chain */

	csize = (unsigned int) v->sec_per_clus * RFAT_SECTOR;
	index = offset / csize;

	/* Seeking forward reuses the cached position; seeking back restarts. A
	   sequential read therefore walks each chain once, not once per call. */
	if (f->cur_index > index || f->cur_cluster < 2)
	{
		f->cur_cluster = f->first_cluster;
		f->cur_index = 0;
	}
	guard = 0;
	while (f->cur_index < index)
	{
		unsigned int n = next_cluster(v, f->cur_cluster);
		if (n == 0)
			return RFAT_ECORRUPT;         /* chain ends inside the file */
		f->cur_cluster = n;
		++f->cur_index;
		if (++guard > v->count_clusters)
			return RFAT_ECORRUPT;
	}

	while (done < len)
	{
		unsigned int in_clus = (offset + done) % csize;
		unsigned int sec = in_clus / RFAT_SECTOR;
		unsigned int in_sec = in_clus % RFAT_SECTOR;
		unsigned int take = RFAT_SECTOR - in_sec;
		unsigned int i;

		if (take > len - done)
			take = len - done;

		//! Whole sectors go straight to the caller in one transfer. Retail
		//! games issue single reads of several megabytes; going a sector at
		//! a time through the bounce buffer would be thousands of round
		//! trips to storage for one request. Only the ragged head and tail
		//! need the buffer.
		//! The destination must also be aligned for the storage layer to
		//! DMA into it directly. A read whose head was partial leaves this
		//! offset odd, and then the sector goes through the bounce buffer
		//! instead - slower, but never a refused read mid-transfer.
		if (in_sec == 0 && len - done >= RFAT_SECTOR
			&& (((unsigned long) (out + done) & 31ul) == 0))
		{
			unsigned int whole = (len - done) / RFAT_SECTOR;
			unsigned int avail = v->sec_per_clus - sec;
			if (whole > avail)
				whole = avail;
			if (!v->read(v->ctx, clus_lba(v, f->cur_cluster) + sec, whole,
						 out + done))
				return RFAT_EIO;
			done += whole * RFAT_SECTOR;
			if (done < len && (offset + done) % csize == 0)
			{
				unsigned int n = next_cluster(v, f->cur_cluster);
				if (n == 0)
					return RFAT_ECORRUPT;
				f->cur_cluster = n;
				++f->cur_index;
			}
			continue;
		}

		if (!cache_sector(v, clus_lba(v, f->cur_cluster) + sec))
			return RFAT_EIO;
		for (i = 0; i < take; ++i)
			out[done + i] = g_sec[in_sec + i];
		done += take;

		if (done < len && (offset + done) % csize == 0)
		{
			unsigned int n = next_cluster(v, f->cur_cluster);
			if (n == 0)
				return RFAT_ECORRUPT;
			f->cur_cluster = n;
			++f->cur_index;
		}
	}

	if (got)
		*got = done;
	return RFAT_OK;
}
