/* Starlet data-cache maintenance for the redirect module.
 *
 * The PPC and Starlet share MEM2 through separate data caches with no
 * snooping between them, so each side must make its writes visible before
 * the other side reads them - and must never discard the other side's
 * dirty data doing it. The rules, stated once:
 *
 * - PPC writes, ARM reads (module/table/store/params inputs/armed word):
 *   PPC flushes (writeback) the exact lines, then ARM invalidates those
 *   lines before first use. Invalidate means DISCARD: it must only ever
 *   target lines ARM never stores to (params inputs, armed word, table,
 *   slice store). Invalidating a line that holds ARM-written counters
 *   would silently lose them, so the params layout keeps ARM-written
 *   words on their own line and no call site below names it.
 * - ARM writes, PPC reads (state/reads/misses/errors/acked): PPC reads
 *   through the uncached alias and never flushes that line; ARM never
 *   invalidates it.
 *
 * riivo_inv_range(addr, len) invalidates whole 32-byte lines covering
 * [addr, addr+len): no writeback, faults nothing, bounded by the caller.
 * Zero length is a no-op; callers pass sane ranges (table/store sizes
 * already validated before use).
 */
#ifndef RIIVO_CACHE_H_
#define RIIVO_CACHE_H_

#ifdef __arm__
/* MCR cache maintenance is ARM-mode-only and the module builds Thumb, so
   the loop lives on a per-function ARM island; callers stay Thumb and the
   branch exchanges across the island boundary like any other call.
   noinline is load-bearing, not cosmetic: this toolchain inlines small
   static functions into Thumb callers and drops the ISA on the floor
   (verified: MCRs emitted under .code 16 without it). */
__attribute__((target("arm"), noinline))
static void riivo_inv_lines(unsigned int a, unsigned int lines)
{
	unsigned int zero = 0;
	while (lines-- > 0)
	{
		__asm__ volatile ("mcr p15, 0, %0, c7, c6, 1" :: "r" (a) : "memory");
		a += 32;
	}
	/* Drain the write buffer so later loads observe invalidated lines. */
	__asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" :: "r" (zero) : "memory");
}
static inline void riivo_inv_range(void *addr, unsigned int len)
{
	unsigned int a = (unsigned int) addr & ~31u;
	/* Line count rounds up to cover the range; the end address is never
	   formed, so a corrupt length cannot wrap it into an unbounded loop -
	   at most (len+31)/32 lines, and callers pass validated sizes. */
	unsigned int lines = (len + 31u) >> 5;
	if (len == 0)
		return;
	riivo_inv_lines(a, lines);
}
#else
/* Host test build: the test file defines this mock (call log + bounds
   assertions). Production ARM code never sees the mock. */
void riivo_inv_range(void *addr, unsigned int len);
#endif

#endif
