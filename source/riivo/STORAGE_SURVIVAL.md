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
 *   survives with a ~2.5 KB ragged edge). Conclusion: startup
 *   bulk-zeroes everything below ~124 KB under arena top while the
 *   words stay fixed. A reservation there is wiped no matter what the
 *   words say; the surviving sliver is unexplained (mechanism unknown)
 *   and explicitly NOT selected - no quiet-looking addresses.
 *
 *   Fidelity bounds: the writer is game PPC code or IOS-on-its-behalf
 *   post-entry (apploader done pre-entry; HLE IOS does not bulk-write
 *   game MEM2). The instruction stream is identical to hardware; heap
 *   geometry differs (see arena note), so only the mechanism transfers.
 *
 * 3. Sync-hook trace (source, wiidev/d2x-cios master)
 *
 *   os_sync_after_write is syscall 0x40, found per running plugin by
 *   call-pattern (device routines calling it), not by table: robust
 *   across bases 56/57/58 IF their readers call it. Confirmed on the
 *   measured module only (singular data point). Absence -> MODACK
 *   withhold by design (ack unobservable AND served bytes possibly
 *   stale - the whole serving path is suspect, not just the ack).
 *   No alternative primitive identified: PPC cannot reach into the
 *   transaction, and no other Starlet maintenance hook is known.
 *   Gap: per-base/slot presence survey needs hardware (see 5).
 *
 * 4. Replacement: storage-backed paging (implemented, host-tested)
 *
 *   Table + gen slices live as FILES on the mod volume (already FAT-
 *   reachable pre-shutdown for staging, and at runtime through the
 *   module's rfat - same mechanism as NAND emu). Resident per pager:
 *   index (10 B/page, 220 B @2802, capped 4 KB), one 4 KB page, 256 B
 *   path scratch, ~64 B context: ~4.6 KB. Worst case per lookup one
 *   page fetch; spanning reads sequential through one buffer; open-time
 *   CRC over the file refuses corruption before service; gaps are MISS
 *   (delegate), never zeros-from-nowhere; failures are EIO, never
 *   partial. test_page: 8175 checks over production-built tables
 *   (paged lookups == resident scans, fetch bounds, path exactness,
 *   corruptions refused). ARM compiles clean (1976 B text).
 *   GENERATED slices become plain file ranges (the emitter already maps
 *   genOff to file offsets): no store reservation, no fill/poison path.
 *   Integration (Boot staging the table FILE, init params change,
 *   sr via pager) is the next slice after review - NOT in this commit.
 *   Resident placement (~12 KB minimum: 6.9 KB code + state + pager +
 *   shared scratch) still needs (5): it fits neither game RAM (wiped)
 *   nor provably the DIP slack or IOS heap from here.
 *
 * 5. Smallest hardware experiment (genuinely hardware-only remainder)
 *
 *   One boot, T0 mod (in-place control, already boots), plus a probe
 *   module variant that (a) attempts iosAlloc(12K) and (b) reports DIP
 *   BSS bounds + sync presence, all through the existing MEM2 mailbox
 *   pre-shutdown - no new tester workflow, no gameplay needed:
 *   - Control first: stock T0 boot log (OUTCOME: FST_STAGED) - if the
 *     control fails, the run says nothing about storage.
 *   - Observations: iosAlloc(12K) success/failure; DIP ram usage bounds;
 *     sync hook present/absent (slot/base recorded); ack attempt lines.
 *   - Distinguishes: (i) IOS heap fits resident -> place there, proceed
 *     to serving test; (ii) heap refuses but DIP slack measures
 *     sufficient on this exact build -> pinned-version slack placement
 *     (fragile, version-locked, stated as such); (iii) neither ->
 *     backend needs a new runtime home (stated, not designed here).
 *   - Sync presence per tested slot/base is recorded in the same run.
 *   - Unreadable flashes inconclusive, never a rerun trigger alone.
 *
 * 6. What stays refused / open
 *
 *   DVD9 refusal (plan-time). Post-shutdown survival of anything (needs
 *   the experiment above). Sync-less consoles stay MODACK-dark until a
 *   valid alternative exists - a permanent refusal is the honest state,
 *   not a bug to work around by weakening the gate.
 ***************************************************************************/
