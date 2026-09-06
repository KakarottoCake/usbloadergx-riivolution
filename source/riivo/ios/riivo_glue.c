/* See riivo_glue.h. */
#include "riivo_glue.h"

rg_ctx g_rg;

int rg_ready(const rg_ctx *c)
{
	if (!c || !c->config)
		return 0;
	/* Whichever convention the config selects must be present. Both are
	   required rather than just the selected one: the config word is read
	   from d2x's memory and could change between here and the first read,
	   and a null branch target is a hard freeze with nothing on screen. */
	return c->read_a != 0 && c->read_b != 0;
}

int rg_read(void *ctx, unsigned int lba, unsigned int count, void *buf)
{
	rg_ctx *c = (rg_ctx *) ctx;
	int rc;

	if (!rg_ready(c) || !buf || count == 0)
		return 0;

	/* Starlet DMAs into this buffer. An unaligned destination is silently
	   corrupt data rather than an error, which is the worst kind of failure
	   to debug on a console, so refuse it here. */
	if (((unsigned long) buf & 31ul) != 0)
	{
		++c->failures;
		return 0;
	}

	++c->calls;
	c->sectors += count;

	if (c->config[2] == 1)
		rc = c->read_b(0, lba, count, buf);
	else
		rc = c->read_a(lba, count, buf);

	/* d2x's readers return 0 for success on the path this hooks; anything
	   else is a failure the FAT reader must see as one rather than treat as
	   a short read. */
	if (rc != 0)
	{
		++c->failures;
		return 0;
	}
	return 1;
}
