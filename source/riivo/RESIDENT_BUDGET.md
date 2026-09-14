/* Residence of the injected module: every byte it owns on the console,
 * measured from the production ARM link - not estimated, not inherited.
 *
 * Build this doc describes: devkitARM -O2 Thumb, freestanding, linked by
 * source/riivo/ios/module.ld (ORIGIN 0x10000000, every section ALIGN(32)),
 * embedded by source/riivo/ios/genblob.py. test_moduleinstall re-links
 * these exact sources and byte-compares against the carried blob, so the
 * numbers below cannot drift from what runs without failing the suite.
 */

Measured resident (current blob: 12096 code, 15552 bss, 33 relocs)
------------------------------------------------------------------
code (text+data)   12096  = 378 * 32, no alignment waste inside.
bss                15552  = 15546 symbols + 6 pad. Symbol by symbol
                          (arm-none-eabi-nm -t d, decimal):
  g_pgIndex   5456  pager index, 341 entries (the pager cap)
  g_pgPage    4096  one table page, 32-aligned (DMA bounce source)
  g_bounce    4096  serve staging, 32-aligned (DMA target)
  g_pg         600  pager context
  g_sr         592  segment-reader context (incl. 512 B path scratch)
  g_sec        512  FAT sector cache, 32-aligned (DMA target)
  g_vol         68  FAT volume context
  g_rr          82  whole-file reader context (open cache + file pos)
  g_rg          36  device-routine glue context
  g_useSeg       4  backend selector
  g_sec_valid    4  sector-cache valid flag
  .data (in code): g_params 116 + g_sec_lba 4 + 8 pad = 128.
TOTAL MEM2 reservation, paged backend: 27648 bytes (module alone;
PlanOnDemand adds 32-aligned table/store parts only when resident).

Relocation storage: 33 words = 132 bytes, carried in the LOADER binary
(RIIVO_MODULE_RELOCS), applied in place at install. Zero resident cost.
The install-time audit (test_moduleinstall) requires every relocated
word to land strictly inside the physical module - including the two
writable-span bounds, which is why the span names its last byte rather
than its exclusive end.

Stack: the module's own frames, measured per function (-fstack-usage),
worst call chains summed by hand (inlined helpers fold into callers):
  find_entry 376 | sr_read 216 | pg_open 152 | rr_read 128 |
  riivo_di_read 72 | sr_covers/rr_covers 104/72 | pg_locate 64 |
  rfat_open 64 | rfat_read 56 | pg_fetch/pg_pread 32 | rg_read 24 ...
  serve (open miss, deep dir): 72+216+64+376+56+16+24      = 824
  covers pre-scan (paged):     72+104+64+32+32+56+16+24    = 400
  init (paged open):           72+48+152+64+376+56+16+24   = 808
Ceiling 824 B of the IOS DI-thread stack - NOT in the reservation.
Excluded, both IOS-owned and unmeasurable on a host: the d2x device
routine's own frames below rg_read, and interrupt context.

Allocation: none. The ios/ sources are freestanding (no libc, no
malloc/new/alloca - grep it); the 512 B sector cache is static BSS;
PROV_BOUNDED holds: no per-read heap on any path. The covers pre-scan
doubles lookup walks per game read (one query walk + one serve walk);
each step is an index bisection plus at most one cached page fetch -
a per-read TIME cost, bounded, not a memory cost.

Ownership, by writer class (the mechanism, not the wish)
--------------------------------------------------------
PPC loader/installer: WriteCode/ClearMem invalidate-before-write,
write, flush, and READ BACK verify through the uncached alias
(RiivoIosProbe.cpp:407-432) - for the module image AND the BSS
clearing. The reservation boundary is lowered FIRST
(CommitMem2Reservation) so later loader allocations cannot enter it;
the layout math (sizes, alignment, fit, no-overlap) is host-tested
(test_mem2reserve, test_ondemand). Against the loader, owned.
Game allocator: the lowered arena-high word (0x80003134) is what a
game's OSInit hands its heap manager; the reservation sits above it.
Owned against every game that allocates through the arena words.
A game that hardcodes MEM2-top or ignores the arena collides - no
static check can see that, so it is a per-game compatibility datum
for the support matrix (measured on hardware), not a design hole.
ARM reuse across boots: Starlet's data cache persists across game
boots (IOS keeps running d2x), so a reused reservation address still
has the last boot's lines tagged - including a stale state word that
would skip init and serve garbage. Closed in design: init discards
the whole writable span (.data through .bss, linker-bounded) before
reading a single word of it (riivo_ios.c). Code needs no such guard:
the blob is byte-identical every boot of one loader build, so stale
instruction or literal lines can only hold the same bytes.
DMA writers (the engines behind read_a/read_b): Starlet's cache does
not snoop them, and a reused sector/page/bounce buffer would serve
its previous occupant after the first read. Closed in design:
rg_read invalidates exactly the DMA'd range after every successful
device read (riivo_glue.c) - unconditional, because double
maintenance is free and missing maintenance is corruption. All
DMA targets are 32-aligned by contract (refused otherwise).
IOS proper (IPC buffers, EHCI descriptors, its own heaps): lives
above the arena boundary and in IOS-private memory, never in the
reservation. Structurally clear; no allocation from any IOS pool
exists in this design - the "IOS allocation" framing is retired.
Apploader/game loading: DOL sections load at linked addresses and
the apploader honors the same arena words; the reservation sits at
the arena top where linked sections do not reach. Same class of
residual as the game allocator above.

What remains genuinely hardware-only: whether a given game honors
the arena words, IOS thread-stack headroom below our 824 B, device
routines' own frames under hook reentrancy per base/slot, and
per-slot sync-hook presence. Those are measurements with a console,
listed as such - everything above them is mechanism, reviewed here.

Local vs hardware, split explicitly
-----------------------------------
Local (continue here): budget tracking per blob regen (this file +
SEGREADER_NOTES memory line), covers/serve semantics and their host
proofs, planner gates (DVD9, epoch, drift, gap ZERO-fill), the
file-immutability contract and its serve-time enforcement,
reservation-arithmetic tests.
Hardware only: the four measurements above, layer-1 diversion on
DVD9, and any timing/stack observation inside IOS. No combined
experiment is proposed until the integration evidence
(CONNECTED_PATH.md) is reviewed.
