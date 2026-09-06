/* See riivo_ios.h. */
#include "riivo_ios.h"

riivo_ios_params g_params = { RIIVO_IOS_MAGIC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static rfat_vol g_vol;
static rr_ctx g_rr;

/* Everything Starlet reads into goes through here first.
 *
 * The alternative - handing the caller's buffer straight to d2x's device
 * routine, the way the fragment reader does - would save a copy, but it
 * depends on those routines accepting a PPC-form address, and nothing offline
 * can establish that. A wrong answer there is a DMA into the wrong physical
 * page: silent corruption, or a freeze, discovered only on a console. Reading
 * into memory this module owns and copying out to the masked address is
 * uniformly correct and costs one memcpy per chunk.
 *
 * 32-byte aligned because the device engines DMA into it. */
static unsigned char g_bounce[4096] __attribute__((aligned(32)));

static void copy_out(unsigned char *dst, const unsigned char *src,
					 unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n; ++i)
		dst[i] = src[i];
}

int riivo_ios_init(void)
{
	int rc;

	if (g_params.state == 1)
		return RIIVO_DI_OK;
	if (g_params.state != 0)
		return RIIVO_DI_FAIL;   /* already failed; do not retry per read */

	/* The loader finds these by scanning a snapshot it took through the PPC's
	   view of MEM2, so they arrive as 0x93xxxxxx. We are executing on Starlet,
	   where the same code and data are at 0x13xxxxxx. Calling the unmasked
	   address branches into unmapped space. Masking an address that is already
	   physical is a no-op, so this is safe either way. */
	g_rg.read_a = (rg_read_a) RIIVO_PHYS(g_params.read_a);
	g_rg.read_b = (rg_read_b) RIIVO_PHYS(g_params.read_b);
	g_rg.config = (const unsigned int *) RIIVO_PHYS(g_params.config);

	if (!rg_ready(&g_rg))
	{
		g_params.state = 2;
		return RIIVO_DI_FAIL;
	}
	if (!g_params.table || !g_params.table_len)
	{
		g_params.state = 3;
		return RIIVO_DI_FAIL;
	}

	rc = rfat_mount(&g_vol, rg_read, &g_rg, g_params.part_lba);
	if (rc != RFAT_OK)
	{
		g_params.state = 4;
		return RIIVO_DI_FAIL;
	}

	rc = rr_init(&g_rr, (const void *) RIIVO_PHYS(g_params.table),
				 g_params.table_len, &g_vol);
	if (rc != RR_OK)
	{
		g_params.state = 5;
		return RIIVO_DI_FAIL;
	}

	g_params.state = 1;
	return RIIVO_DI_OK;
}

int riivo_di_read(unsigned int off_words, unsigned int len, void *dst)
{
	unsigned long long off;
	unsigned char *out;
	unsigned int done = 0;

	/* A zero-length read happens and must succeed without touching storage. */
	if (len == 0)
		return RIIVO_DI_OK;
	if (!dst)
		return RIIVO_DI_FAIL;

	if (riivo_ios_init() != RIIVO_DI_OK)
		return RIIVO_DI_MISS;   /* fall through to the stock path, unmodded */

	off = (unsigned long long) off_words * 4u;
	out = (unsigned char *) RIIVO_PHYS(dst);

	while (done < len)
	{
		unsigned int take = len - done;
		int rc;

		if (take > sizeof(g_bounce))
			take = sizeof(g_bounce);

		rc = rr_read(&g_rr, off + done, take, g_bounce);
		if (rc == RR_MISS)
		{
			/* Only meaningful before anything has been written. Once part of
			   the buffer is ours, falling through would run the stock read
			   over the top of it. */
			if (done == 0)
			{
				++g_params.misses;
				return RIIVO_DI_MISS;
			}
			++g_params.errors;
			return RIIVO_DI_FAIL;
		}
		if (rc != RR_OK)
		{
			++g_params.errors;
			return RIIVO_DI_FAIL;
		}

		copy_out(out + done, g_bounce, take);
		done += take;
	}

	/* Starlet's data cache is write-back with no snooping toward the PPC, so
	   without this the game reads whatever was in that memory before. That
	   fails later and looks like corruption rather than like a bug here. */
	if (g_params.sync)
		((riivo_sync_fn) RIIVO_PHYS(g_params.sync))(out, len);

	++g_params.reads;
	return RIIVO_DI_OK;
}
