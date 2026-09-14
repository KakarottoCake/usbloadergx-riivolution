/* Resonance of the injected module: every byte it owns on the console,
 * measured from the production ARM link - not estimated, not inherited.
 *
 * Build this doc describes: devkitARM -O2 Thumb, freestanding, linked by
 * source/riivo/ios/module.ld (ORIGIN 0x10000000, every section ALIGN(32)),
 * embedded by source/riivo/ios/genblob.py. test_moduleinstall re-links
 * these exact sources and byte-compares against the carried blob, so the
 * numbers below cannot drift from what runs without failing the suite.
 */

Measured resident (current blob: 12064 code, 15552 bss, 31 relocs)
------------------------------------------------------------------
code (text+data)   12064  = 377 * 32, no alignment waste inside.
bss                15552  = 15546 symbols + 6 pad. Symbol by symbol
                          (arm-none-eabi-nm -t d, decimal):
  g_pgIndex   5456  pager index, 341 entries (the pager cap)
  g_pgPage    4096  one table page, 32-aligned (DMA bounce source)
  g_bounce    4096  serve staging, 32-aligned (DMA target)
  g_pg         600  pager context
  g_sr         592  segment-reader context (incl. 512 B path scratch)
  g_sec        512  FAT sector cache (the only "cache"; static, bounded)
  g_vol         68  FAT volume context
  g_rr          82  whole-file reader context (open cache + file pos)
  g_rg          36  device-routine glue context
  g_useSeg       4  backend selector
  g_sec_valid    4  sector-cache valid flag
  .data (in code): g_params 116 + g_sec_lba 4 = 120.
TOTAL MEM2 reservation, paged backend: 27616 bytes (module alone;
PlanOnDemand adds 32-aligned table/store parts only when resident).

Relocation storage: 31 words = 124 bytes, carried in the LOADER binary
(RIIVO_MODULE_RELOCS), applied in place at install. Zero resident cost.

Stack: the module's own frames, measured per function (-fstack-usage),
worst call chains summed by hand (inlined helpers fold into callers):
  find_entry 376 | sr_read 216 | pg_open 152 | rr_read 136 |
  riivo_di_read 72 | sr_covers 104 | rr_covers 72 | pg_locate 64 |
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

Allocation contract (revised)
-----------------------------
1. Nothing is allocated from the IOS heap, the DIP slack, or any IOS
   pool. The earlier framing that sized an "IOS allocation" is retired:
   the module lives in a MEM2 reservation the loader owns, and its only
   IOS-side consumables are <1 KB of DI-thread stack and re-entrant
   calls into d2x's own device routines - the same routines every FAT
   read at init and every file open already calls from hook context,
   so paging adds table-file reads through them but no NEW requirement.
2. The reservation derives from the blob itself: ModuleFootprint() =
   RIIVO_MODULE_CODE_LEN + RIIVO_MODULE_BSS_LEN
   (RiivoModuleInstall.cpp:32), aligned to MEM2_RESERVE_ALIGN (32),
   with overflow-checked parts and fit re-verified after the split
   (RiivoOnDemand.cpp:19-122). Regenning the blob moves the budget
   automatically; the suite pins it (sizes + layout + fresh-link
   byte-match).
3. What this contract does NOT claim: MEM2 lifetime against the game
   (overwrite), PPC/ARM cache behavior beyond the documented
   flush/invalidate islands, IOS thread-stack headroom below our
   824 B, or device-routine reentrancy on untested slots. Those are
   hardware measurements, listed below - not assumptions.

Local vs hardware, split explicitly
-----------------------------------
Local (continue here): budget tracking per blob regen (this file +
SEGREADER_NOTES memory line), covers/serve semantics and their host
proofs, planner gates (DVD9, epoch, drift), source-backed sync
analysis, reservation-arithmetic tests.
Hardware only: MEM2 survival past game init, per-slot sync-hook
presence and cache visibility, d2x routine behavior under hook
reentrancy on each base/slot, layer-1 diversion on DVD9, and any
timing/stack observation inside IOS. No combined experiment is
proposed until the integration evidence (CONNECTED_PATH.md) is
reviewed.
