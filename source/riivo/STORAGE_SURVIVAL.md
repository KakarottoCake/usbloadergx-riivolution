/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Runtime storage survival: where the module and its data can live.
 *
 * Verdict first: game-RAM ownership cannot be established. A 2 MB span
 * below the MEM2 arena top is bulk-zeroed within 15 s of game entry
 * while the arena words stay fixed (emulator observation below, method
 * included). Arena lowering is this design's reservation attempt - not
 * an established necessary or sufficient condition for survival. The
 * replacement is storage-backed paging (riivo_page.h/.c, host-tested);
 * resident placement still needs hardware measurement (specified below).
 *
 * 1. Address / owner map (PPC view; Starlet subtracts 0x80000000)
 *
 *   [0x90000800 .. ]           MEM2 bottom / IPC page. Explicitly excluded:
 *                              nothing like 168 KB spare (RiivoMem2Reserve).
 *   [0x90200000, 0x93300000)   GX pool (game allocator; Dolphin
 *                              mem2alloc.cpp). Game-owned, always.
 *   [0x93000000, ...]          Loader-phase DMA target (disc.c). Pre-
 *                              shutdown only; irrelevant post-boot.
 *   [oldArenaHi - N, oldArenaHi)  The reservation (module|table|store),
 *                              e.g. below 0x933A6EE0 on the measured
 *                              hardware boot (arena lo 0x93200000). Writers:
 *                              loader pre-shutdown (flushed), ARM counters
 *                              (own line), game (UNOWNED - see 2), IOS
 *                              (never allocates game MEM2).
 *   [0x13802000, 0x13842000) Starlet view = PPC 0x93802000+: DIP plugin
 *                              ram region (256 KB link). fraglist_data
 *                              alone is 20000x12 = 240012 B + frag_idx
 *                              1280 B + misc: ~242 KB used, ~13-14 KB
 *                              slack ESTIMATED from sources (d2x-cios
 *                              master, dip-plugin/frag.{h,c}). Our
 *                              resident needs ~12 KB (RIV1-only, shared
 *                              scratch) to ~18 KB (both readers): too
 *                              close to an estimated slack to place.
 *   IOS heap ("tens of KB free", shared with DIP/EHCI transient allocs
 *   like sector_buf): 12-18 KB persistent risks starving game I/O.
 *   Unmeasured on hardware - see 5.
 *   0x80003130/34  MEM2 arena words. Lowered pre-jump, flushed. The game
 *                              reads them at OSInit - and rewrites arena
 *                              state itself elsewhere (0x80004148, prior
 *                              entry-page disassembly).
 *
 * 2. Emulator observation (game-side memory behavior only)
 *
 *   Setup: DolphinNoGUI + SB4E01 authorized backup, GDB stub, Null
 *   video. One session: arena words read at DOL entry, 2 MB sentinel
 *   span planted over [arenaHi-2MB, arenaHi), t+0 verified fully
 *   intact, single continue, timed samples on the live target.
 *   (Scripts: local temp, not shipped; method matches the prior
 *   GDB-at-birth work. No ARM/IOS emulation involved - Dolphin HLEs
 *   IOS; the PPC instruction stream is what is observed.)
 *
 *   Entry arena (Dolphin direct boot): lo=0x935E0000 hi=0x93600000.
 *   NOTE: differs from hardware-loader boot (lo 0x93200000 hi
 *   0x933A6EE0 per the T0 card log). Absolute addresses do NOT
 *   transfer; the mechanism does: in both, the reservation sits just
 *   below arena top, whose words the loader (not the game) set.
 *
 *   Result: span [0x93400000,0x93600000): t+0 100% intact; by t+15 s
 *   6.08% intact, first dead word at the span start, replacement bytes
 *   all ZERO; stable and identical at +6/+10 min; arena words fixed at
 *   every sample; live-read check PASS; ~111 CPU-s burned (game ran).
 *   Edge pinned at 1 KB: first live word 0x935E0400 (top ~124 KB
 *   survives with a ~2.5 KB ragged edge). What this establishes is
 *   narrow: overwrite happened in THAT emulator configuration, writer
 *   unidentified (game PPC code vs IOS-on-its-behalf is not
 *   distinguished by this test), and identical behavior under GX's
 *   different arena setup is NOT proven. What it rules out is the
 *   claim that a fixed arena word protects memory: words fixed,
 *   memory wiped. The surviving sliver is unexplained (mechanism
 *   unknown) and explicitly NOT selected - no quiet-looking addresses.
 *
 *   Fidelity bounds: HLE IOS does not bulk-write game MEM2, and the PPC
 *   instruction stream observed is the retail game's own; heap geometry
 *   differs (see arena note), so only the overwrite-with-fixed-words
 *   observation transfers, not addresses or timing.
 *
 * 3. Sync-hook trace (pinned revision: wiidev/d2x-cios tag d2x-v11-beta3)
 *
 *   os_sync_after_write is syscall 0x40 in that tag's own sources
 *   (cios-lib/direct_syscalls.s: `direct_syscall 0x40,
 *   direct_os_sync_after_write`), and our probe finds it per running
 *   plugin by call-pattern (device routines calling it), not by table:
 *   robust across bases 56/57/58 IF their readers call it. Confirmed
 *   on the measured module only (singular data point). Absence ->
 *   MODACK withhold by design (ack unobservable AND served bytes
 *   possibly stale - the whole serving path is suspect, not just the
 *   ack). No VALIDATED alternative exists (not "none exists": PPC
 *   cannot reach into the transaction, and no other Starlet
 *   maintenance hook is proven - absence of evidence, stated as
 *   such). Gap: per-base/slot presence survey needs hardware (see 5).
 *
 * 4. Replacement: storage-backed paging (implemented, host-tested)
 *
 *   Table + gen slices live as FILES on the mod volume (already FAT-
 *   reachable pre-shutdown for staging, and at runtime through the
 *   module's rfat - same mechanism as NAND emu). Resident per pager:
 *   5456 B index + 4096 B page + 512 B path scratch + 600 B context
 *   (measured nm; full budget in RESIDENT_BUDGET.md). Worst case per
 *   lookup one page fetch; covered requests serve sequential through
 *   the 4 KiB bounce after a read-only covers pre-scan; open-time
 *   CRC/identity/epoch over the file refuses corruption and staleness
 *   before service; unlisted bytes stop with GAP and the dispatcher
 *   delegates the request whole (MISS) - zeros come only from
 *   plan-defined ZERO runs and sector-tail padding, never from gaps.
 *   Failures are EIO, never partial. test_page: 8202 checks over
 *   production-built tables (paged lookups == resident scans, fetch
 *   bounds, cross-page abutting span, ZERO-vs-gap, corruptions
 *   refused) plus test_segread: 86 (resident serve/GAP/covers, rr
 *   parity, real dispatch gate). ARM links at 12064 code + 15552
 *   bss = 27616 resident (test_moduleinstall byte-matches the fresh
 *   link). GENERATED slices are plain staged bytes the reader
 *   addresses by offset: no MEM2 store reservation, no fill/poison
 *   path for the paged backend (fill/verify/arm still stage the gen
 *   FILE pre-boot; see CONNECTED_PATH.md). The retired framing that
 *   sized an "IOS allocation" is corrected there too: the module
 *   lives in a MEM2 reservation and consumes <1 KB of DI-thread
 *   stack (measured); what remains hardware-only is MEM2 survival,
 *   per-slot sync presence, and routine behavior under the hook.
 *
 * 5. One combined hardware check (genuinely hardware-only remainder)
 *
 *   A successful iosAlloc(12K) would establish allocation AT THAT
 *   MOMENT only - not lifetime (freed? retained across game I/O?),
 *   not 32-byte alignment, not executability of heap memory, and not
 *   the total runtime budget under game load. The check below is
 *   designed so each of those gets its own observable; anything
 *   unobserved stays open, never assumed.
 *
 *   One boot, T0 mod (in-place control, already boots), plus a probe
 *   module variant that, pre-shutdown through the existing MEM2
 *   mailbox: (a) attempts iosAlloc(4096/12288/18432) and reports each
 *   result + returned alignment; (b) reports DIP ram usage bounds for
 *   the running build + sync presence; (c) writes a sentinel band and
 *   re-reads it after a fixed delay (lifetime smoke, minutes not
 *   seconds). No new tester workflow, no gameplay needed:
 *   - Control first: stock T0 boot log (OUTCOME: FST_STAGED) - if the
 *     control fails, the run says nothing about storage.
 *   - Observations: alloc results + alignments; DIP bounds; sync
 *     present/absent (slot/base recorded); sentinel band intact or
 *     not; ack attempt lines.
 *   - Distinguishes: (i) 12K+ allocs succeed, aligned, sentinel holds
 *     -> place resident in IOS heap, proceed to a serving test;
 *     (ii) allocs fail but DIP slack measures sufficient on this
 *     exact build -> pinned-version slack placement (fragile,
 *     version-locked, stated as such); (iii) neither -> backend
 *     needs a new runtime home (stated, not designed here).
 *   - Executability is NOT covered by allocation success: if (i) wins,
 *     the serving test itself (post-boot reads through the hook) is
 *     what proves heap execution, and it follows only then.
 *   - Sync presence per tested slot/base is recorded in the same run.
 *   - Unreadable flashes inconclusive, never a rerun trigger alone.
 *   No separate allocation-only round: this one check covers
 *   allocation, cache (ack lines), and serving readiness together.
 *
 * 6. What stays refused / open
 *
 *   DVD9 refusal (plan-time). Post-shutdown survival of anything (needs
 *   the experiment above). Sync-less consoles stay MODACK-dark until a
 *   valid alternative exists - a permanent refusal is the honest state,
 *   not a bug to work around by weakening the gate.
 ***************************************************************************/
