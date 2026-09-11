# fstadapter: what it bypasses, what it proves

`fstadapter` runs the REAL `Riivo::PlaceFst` / `Riivo::InstallFst`
(`../source/riivo/RiivoFstInstall.cpp`, compiled unmodified) plus the
REAL `Riivo::Crc32` (`../source/riivo/RiivoReconcile.hpp`) under Dolphin,
fed with constants captured from real SB4E01 T0 card logs. v2 executes
the actual late-install caller sequence as a line-referenced mirror of
`Riivo::InstallPendingFst` (same order, same fail codes 0-5) and the
launch-tail shape (inter-phase heap churn, consumer reads through the
repointed words, pre-jump re-read), in-place and relocated back-to-back.

## Bypassed cIOS / hardware behavior (NOT exercised here)

- All disc I/O: `WDVD_Read`, `DIOpen`, partition handling. There is no disc;
  the boot words, DOL ranges, BSS range and rebuilt sizes are constants.
- The apploader: no DOL chunks are loaded, so `0x81200000` holds no image
  and the chunk-yield notes do not exist. The 8 KB block
  `[0x817D8740, 0x817DA740)` is taken as a captured range, not re-derived.
  The boot-info block is set to captured words to model the
  apploader-filled state.
- IOS heap and loader thread stacks: the adapter runs on a plain libogc
  thread. Stack-position evidence (the model-A/model-B question) CANNOT
  come from this harness. The churn phase models loader allocation
  *activity* (sizes are stand-ins, not measurements).
- MEM2 staging uses an Arena2 bump allocator, not GX's `mem2.cpp`. What
  is preserved is the lifetime (MEM2, surviving MEM1 fills and the churn
  phase, CRC-checked before copying), not the allocator.
- The consumer parses a synthetic VALID FST head (real U8 entries +
  string table, marker pad), not a real rebuilt table. The feasible slice
  is "reads via the new pointer stay in bounds", not game execution.
- Drive timing, USB retries, and everything the game does after the jump:
  no game boots here.

## What it validates (real PPC code, real emulated MEM1)

- v1: `PlaceFst` reaches the known answer (`0x817B2DE0`, 162144 bytes
  reserved); `InstallFst` memcpy+flush+repoint lands all 153934 bytes
  (0 mismatches) with no DSI; write watchpoint fires exactly on the span.
- v2: the mirrored late-install caller verifies code 0 in BOTH modes;
  consumer reads parse 8101/8093 entries; pre-jump re-read matches.
  Mirror only - superseded by v3, kept for the record.
- v3: the PRODUCTION `InstallPendingFst` (real TU, GDB-poked staging,
  no mirror) verifies code 0 in BOTH modes: relocated and in-place
  spans dump byte-exact (153934 + 153792 bytes, 0 mismatches).
  GPR forensics confirmed the call path (cursor marching, place
  registers intact). No production defect found on these inputs.
- A passing simulated install narrows the investigation to the Wii-only
  list below; it does not clear the full Wii installation path.

## Harness postmortem (read before trusting any adapter result)

- An 8-byte struct-size assumption (`FstPlacement` 60 vs actual 52
  under this libstdc++) once overran the GDB-poked place blob into the
  neighboring staging words (`size` <- `0x817DA740`, `crc` <- junk),
  producing a deterministic "freeze" in the pre-copy CRC that looked
  exactly like a production defect. **Classified: harness defect. It
  illustrates a failure mechanism (wrong loop bound from corrupted
  staging metadata) but is NOT evidence that production staging
  metadata is corrupted** - production builds its own place struct and
  CRC in one binary; nothing crosses a debugger there. Lesson: the blob
  length travels in the mailbox (`+144`), and every GDB-poked word is
  read back from RAM before the call. Never trust a freeze without
  register-level proof.
- GDB reads bypass the data cache: `pendingPlaceOk=false` (plain
  unflushed store) still reads 1 after a verified install, while the
  flushed words and table bytes read correctly. Flushed state is truth;
  unflushed BSS is not.
- Stub run-control rule (12+ runs): p/m/M/Z anytime; `c`/`\x03` ONLY
  from stopped - sent while running they wedge the stub's command loop
  (total silence after). Drive multi-phase flows with writes + target
  waits + watch-stops, never with cont-then-halt.
- Rendezvous discipline: sleeps never synchronize. The harness publishes
  `adPhase` (1=unchanged done, 2=mode wait, 4=finished) and per-mode
  `adModeDone`, and waits for GDB acks before advancing - a sleep-synced
  round once executed an install in the wrong labeled slot, and several
  rounds "lost" acks that were actually consumed by waits the script had
  not seen. GDB polls with waitmem (equality) and memdiff (change from a
  prior value); pokes happen only in the matching wait. A read is trusted
  iff it differs from all previous reads of that address.
- Resolved observability scare: per-probe nonces (1/2/3) in the result
  block proved every "stale read" was a script-early read - the harness
  simply had not advanced yet. With transition-gated scripting, all three
  consumptions verify in one run. No stub read-caching exists; the earlier
  suspicion is withdrawn.

## Still reserved for Wii hardware

- Whether the real apploader layout matches the captured constants.
- Whether the live loader stack overlaps the destination (model A vs B).
- Whether the game reads the relocated table back correctly.
- Whether cIOS reads deliver the bytes the plan assumed.

## Real-game counterpart (Dolphin disc backend, SB4E01 backup)

The adapter's modeled cases are now cross-checked against actual SMG2
startup in Dolphin (retail ISO boots keyless - the common key is built
into Dolphin's IOSC; an early "missing key" claim was wrong, and the
real blocker was a spaced-path split caught via dialog screenshot).
Method: halt at game birth (`0x80004050`, heap empty), GDB-install bytes
+ words (mirrors loader timing - installing minutes later smashes live
heap and proves nothing), `go`, trace PC/SP/words/bytes:
- Unchanged / arena-lie / in-place-rewrite: baseline idle, untouched.
- Relocated (grown T0 bytes AND verbatim stock bytes): full-span wipe
  to zeros within seconds of entry + park at `0x805B2B14` (EE off).
  Position kills, content is innocent, arenaHi is irrelevant.
- Apploader-time snapshot: below-reservation is a zero desert; the wipe
  is routine game clearing of that zone (marker control), not a reaction.
Conclusion carried to production: below-reservation placement is dead
for SMG2; in-place is the only proven-safe MEM1 region; fix tracks are
suffix-compacted in-place tables and MEM2-resident grown tables.

## MEM2 verdict (real Spectral table, birth timing, Sep 2026)

Prior Test 6 ("mid site installs land but the game never reads the
table and parks") is now explained - the cause is overwrite, not
consumption. Method: halt at birth, GDB-install the REAL 230076-byte
Spectral plain table (`e17e71a3`) at `0x92000000` plus canary markers
at `0x91000000`/`0x92040000`, repoint `0x80000038/3C` as InstallFst
would, verify twice halted, `go`, sample, full post-mortem dump:
- Install verified (head + markers read back twice, identically).
- Within 30 s of `go`: table head, both markers, and below/above
  canaries ALL read zeros. Post-mortem dump of the full 230076 bytes:
  zero nonzero bytes (`f051507b` is the CRC of 230076 zeros).
- Pointer word never rewritten by the game (still `0x92000000`).
- Park at `0x805B2B14` EE off - the SAME park as the relocated-MEM1
  wipe. The park is the dead-table signature, not position-specific.
First divergence from the in-place baseline is overwrite, before any
read: game startup clears the MEM2 span covering at least
`0x91000000-0x92040010`. Survival fails first, so consumption is moot
until a surviving site is found. NOT established: the wipe's agent
(game MEM2/arena init presumed, not fingerprinted) or its exact
extent - the follow-up is a marker map across MEM2, not another
consumption attempt. A prior late-install run's non-zero post-mortem
pattern is consistent with post-clear heap reuse over minutes, not
contradictory. MEM2 stays a candidate with a known blocker (startup
clearing), not a track proven by elimination.

## Clearing follow-up (disassembly + trap control, Sep 2026)

- WHEN narrowed: wipe AND park both land within 10 s of birth (10 s
  sampling round already fully zero + parked). Agent still open.
- NOT the C-runtime BSS init: entry `bl` chain (`__init_registers`,
  BSS/ROM-copy table walks at `0x800042a0`) was disassembled from a
  live dump; every BSS range in the examined tables is MEM1
  (`[0x80006EA0,+0x63CE78)` is the big one). (Scope: the examined
  initialization paths only - not a claim about the whole runtime.)
  No `dcbz`, no `lis 0x9000` in `[0x80004000,0x80044000)`.
  Entry prologue confirmed to write `0x80000034` (arenaHi) itself at
  `0x80004144` and to apply relocations; it never reads `0x80000038`.
- The park is a DELIBERATE hang, not a crash: `0x805B2B10` is
  `sync/nop/li r3,0/nop/b .`, called from a bounded (16-iteration)
  queue-walk that bails to it on empty/`-1`/overflow. Its caller chain
  (`bl 0x805B6210` OSReport-style logging + `bl 0x805B5C50` queue-head)
  reads like a fatal-error queue dump. MSR EE off at the park vs EE on
  at normal idle.
- Discriminator refined: `0x805BCCB0` (self-loop, EE on) IS the normal
  stock idle - seen on an unmodified boot. `0x805B2B14` (EE off) is the
  dead-table/fatal park. Do not confuse the two.
- METHOD constraint: GDB memcheck traps are DEAD on this stub. A
  write-watch on `0x80000034` that the entry sequence demonstrably
  stores never fired across 10 s of subsequent execution. Trap
  silence (read or write) therefore means nothing - neither
  dcbz-blindness nor no-access can be inferred from it. Proven working:
  halt/regs/mem/write/dump/go-polling; halt reliably lands
  on birth (`0x80004050`) at +20-30 s; explicit-quote CLI needed for
  spaced ISO paths (bare array elements split at the first space and
  strand Dolphin on a Warning dialog with no stub).
- Ordering vs first FST lookup: still open (traps can't time it;
  first READ unobserved by any means; wipe time-bounded <3 s only).

## Tooling verdict (debugger implementation, Sep 2026)
- Scope: Dolphin 5.0-18995, GDB stub, JIT default config. "Traps
  dead" verdicts below cover exactly this configuration - not all
  trapping everywhere.
- Source (upstream master, inspected): the stub implements Z0-Z4
  into ONE shared backend (`PowerPC::BreakPoints` /
  `PowerPC::MemChecks`, same structures the native debugger feeds),
  with JIT invalidation on add. So a native-UI attempt would most
  likely fail identically - but that is a prediction from shared
  code, not a test (no automation path exists for the Qt UI).
- Exec breakpoints (Z0): silent in JIT dual-core, JIT single-core
  (`CPUThread=False` retest), and interpreter (`JitOff=True`),
  including a clean test (halted pre-execution, bp ahead at
  certain-execution `0x80004058`, demonstrably executed past).
- Write watch (Z2): silent in JIT both thread modes. ONE exception:
  under the interpreter, a Z2 write-watch on adapter-mailbox RAM
  FIRED (SIGTRAP stop precisely at the faulting `stw` inside
  memset, verified against the .elf) - so trapping CAN work outside
  the JIT. Yet game MEM2 Z2/Z3 stay silent in the same interpreter
  while the wipe provably happens. Best current hypothesis: MEM2 /
  EXRAM accesses (and/or dcbz, which owns a separate opcode path)
  bypass the hooked access paths. Unconfirmed - needs dev-build
  instrumentation or the static agent ID.
- One ambiguous data point: an interpreter Z2 on entry-store
  `0x80000044` stopped the core seconds later at `0x805B99D0` -
  genuine second store or spurious stop, UNDECIDED (decidable by
  disassembling that function; region undumped). The earlier
  `0x80000034` control is VOID (conditional store, likely skipped),
  not negative.
- `step` is not single-step on this stub (jumps, then stalls);
  file logging writes nothing in batch mode. `JitOff=True`
  ([Debug]) is the working interpreter switch; birth still lands
  at +20-30 s (IOS boot is engine-independent).
- REMAINING ROUTE: developer build with targeted EXRAM/dcbz
  logging (unstarted - toolchain + hours, stated so nobody
  re-attempts polling first). NO further address polling until a
  working trap or log identifies the operation.
- Hardware gating (unchanged): the MEM2 path stays behind the
  `riivolution/mem2fst.txt` marker (default off); no hardware run
  uses it. Compaction remains the working path; the Spectral
  preparation stop is a separate investigation.

## Dev build: dcbz trace + controls (Sep 2026, master 66d8a89220)
- Built `DolphinNoGUI.exe` (VS2022 17.14, NoGUI-only, patch in
  `MMU::ClearDCacheLine` + span-filtered `MMU::Read`). Provenance:
  shallow master @ `a2efdf1`, checked out to pre-VS2026-gate parent
  `66d8a89220` (gate `ed25d5649b`, 2026-08-05). Build notes that bit:
  msys cmake/gcc must NOT drive it (SFML Unix errors); native
  portable CMake + native ninja + vcvars MSVC required.
- VALIDATED, both controls, before any game result was trusted:
  deliberate dcbz fill logs 32/32 contiguous lines with exact
  PC/EA/sp (no false positives on the stw/memset buffers);
  deliberate stw fill trips GDB Z2 and deliberate lwz trips GDB Z3
  in the same build/profile (validates the GDB channels too).
- Trace sink: `C:\dolphin-trace\trace.log` (host-side file, flushed
  per line, line cap). NoGUI starts paused: every run needs the GDB
  halt/go handshake or nothing executes. Spaced ISO paths need the
  single-string CLI form (array form strands NoGUI pre-emulation
  with no stub - same bug class as the GUI Warning dialog).
- FIRST OVERLAPPING WRITE (verbatim stock table, JIT): dcbz,
  pc=`0x805B551C` (DCZeroRange leaf), lr=`0x805B32E8` (arena-clear
  caller via lo/hi getters), span `[0x90000800, 0x935E0000)` =
  1,765,312 lines x 32 B (~54 MB, ONE call), plus two small
  DVD-work-buffer events nearby (`lr=0x804DBC0C/70`). Validated Z2
  silence on the span means NO CPU store touched it: the wipe is
  exclusively dcbz.
- FIRST FST READ: none observed. Validated Z3 silence + span-hooked
  MMU reads at zero across full runs (fastmem off to funnel JIT
  loads). CPU-load scope only: DVD DMA never passes that path.
- POINTER REWRITE (RETIRED - profile artifact, see Causal-order
  section): the "unset at birth / async arrival" sequence appears
  only with AccurateCPUCache=True. Converged profile shows valid
  words at birth; wait-for-words protocol withdrawn.
- 0x805B99D0 interpreter stop: singular spurious sample at an IRQ
  helper (no watch there); unresolved, claimed for nothing.

## Reinstall + fatal-caller recovery (Sep 2026)

- Wipe is ONE-SHOT, not continuous: a table reinstalled at T+8s
  (after wipe+hang) survives 30 s+ verified-intact. CONDITION on that
  claim: the game was already parked; a parked game may simply never
  run the clearer again. One-shot DURING NORMAL STARTUP is not
  established by this - only that nothing re-wipes a parked game.
- ROUND-1 (T+3 s) already shows wipe+park complete: both events live
  inside the first 3 s post-birth. Their causal order is NOT
  determined (erasure-before-first-access vs failure-during-handling
  both fit; polling cannot separate them).
- Fatal-caller stack (32 KB park-stack dump, back-chain walked):
  hang `0x805B2B14` <- queue-walk `0x805B636C` <- `0x806FA220/30`,
  `0x80727A68`, `0x804B2000`, `0x804B1DF4/0x804B1DB8`. Message
  fragments in-stack: the ASCII address `'805bd060'` (matches a
  pointer word also present: hex-dump-style reporting, not prose)
  and the path `'/AudioRes/SMR.szs'` - an open-by-name for a
  Spectral-replaced file was in flight around the failure. Args at
  the park are park-loop regs (useless); call args need the frames
  above, partially recovered. Direct caller of the dump (naming the
  failing subsystem) is NOT yet disassembled.
- Dolphin-hang caveat: with mod-region offsets no local backend can
  serve the resulting DVD reads (no cIOS), so the hang itself is
  expected here and says nothing about hardware viability. The
  hardware-relevant facts are the wipe (pre-read, game code) and the
  pointer left untouched.
- Tooling status: NO trap of any kind has fired on this stub. Z0
  (exec breakpoint) is UNPROVEN - both attempts were confounded
  (resume-PC suppression pattern untested either way) - and Z2 is
  proven dead by control. All timing/state claims above rest on
  halt/poll/dump primitives only.
- Later-install design space (undecided): cIOS-hook on-demand AND
  game-side hook are both architectural options; neither is chosen.
  Both require, first: wipe-T bounds (have: one-shot, <8 s),
  first-read-T (unknown), and a truly owned execution point between
  them - plus, for any MEM2 site, surviving the wipe. No timed
  delays.

## Validity + control + Z0 verdict (Sep 2026)

- TEST-VALIDITY CORRECTION: the Spectral-probe hang cannot speak to
  MEM2 viability. Its table carries mod-region offsets no local
  backend serves (installing the table never reproduced GX's cIOS
  fragment mapping), so the fatal path there is an expected
  missing-data failure. The SMR.szs pathname is a lead, not proof
  of the failing request.
- Z0 PROVEN DEAD (clean control): halted at birth, exec breakpoint
  set one call ahead at certain-execution `0x80004058`, continue -
  game demonstrably ran past it to idle with no stop. All trap-based
  plans are void; the earlier "unproven" verdicts collapse to dead.
- Dump-caller identified (static, from park-stack frames): `0x804B1D70`
  is an async request dispatcher - busy-waits on a request state
  word, streaming-I/O (GQR) setup prologue, per-state handlers, with
  the queue-dump/hang as its failure leg. SUGGESTIVE ONLY: the shape
  fits a media-stream (DVD/audio) request path and SMR.szs was in
  flight, but the exact outstanding request and the failure reason
  remain unidentified - do not cite a subsystem.
- VERBATIM-STOCK CONTROL (all reads servable): stock SB4E01 table at
  `0x92000000`, birth install, repoint - IDENTICAL wipe (post-mortem
  all zeros) + dead park `0x805B2B14`/EE-off, pointer untouched. What
  this excludes: missing replacement payload as a NECESSARY cause of
  that run (servable content hangs the same). What it does NOT
  exclude: every other content-related explanation - and MEM2-read
  viability stays open, since a wiped table preempts both. The wipe
  may precede failure or result from error handling; it is NOT
  established as the primary blocker.
- Standing consequence: a birth-installed MEM2 table is dead before
  use either way. The wipe is THE blocker; viability questions
  beyond it stay moot until a table survives it.

## Bounds + viability verdict (Sep 2026)

- WIPE TOP: high markers (own 16 B writes to game-untouched regions
  only) at `0x924/28/2C/3000000` all read zeros after 60 s of stock
  boot; `0x93400000` reads E00 FAULT post-boot (readable+writable at
  birth). So the clear covers at least through `0x93000000`, and the
  game withdraws high-MEM2 readability after boot (matches the old
  survey: top faults post-boot). Lower bound unknown (live heap
  below `0x91000000`, deliberately untouched).
- SITE LEDGER (current knowledge): low = live heap (collision);
  mid (`0x91000000-0x93000000`) = startup-wiped; high (`0x93400000+`)
  = unreadable post-boot. NO TESTED MEM2 ADDRESS WORKS. That does
  not establish post-wipe installation as the only remaining design
  - it only closes the tested placements; reservation-timing and
  startup-behavior changes are untested.
- CONSUMPTION probe with hang neutered (verbatim stock table): game
  reaches normal idle with pointer aimed at MEM2 and stock MEM1
  zeroed - no pointer-range validation park exists. But idle proves
  no post-boot reads happened to need serving, so MEM2 reads remain
  neither proven nor opposed. The neuter also voids that run for
  failure analysis (any failure masked by design).
- REMAINING MEM2 SHAPE (only one left): install AFTER the one-shot
  wipe into mid-MEM2 (above live-heap reach), which needs a truly
  owned post-startup execution point (cIOS hook or game-side hook -
  both undecided, both need first-read timing + ownership). No
  timed-delay installs; no production changes from polling alone.

Scope note (narrowed per review): established ONLY (i) lowered arenaHi
with no table changes nothing observable here, and (ii) the wipe is
identical with arenaHi lowered or original. arenaHi's wider role is
untested - and the game's entry code writes the word itself
(`0x80004148`), so it is actively managed, not ignored. MEM2 relocation
is a separate unimplemented track (game MEM2-avoidance question open);
no unverified address is substituted anywhere on this evidence.

## Backing state + install boundary (Sep 2026)

- Arena words at birth: TOP-slot `0x807D6684` = 0, BASE-slot
  `0x807D0964` = `0xFFFFFFFF` (unset). Post-boot: TOP =
  `0x935E0000`, BASE = `0x90000800`. The clear consumes them as
  start=BASE, size=TOP-BASE (`subf` order verified in disassembly -
  an earlier read had it backwards); the observed clear span
  `[0x90000800, 0x935E0000)` matches the final values exactly, so
  the setter precedes the clear (value-consistency ordering, not
  trap PCs - see below).
- RESERVATION ASSESSMENT: the slots are game-owned and overwritten
  during init, so a loader-planted value does not survive; the same
  words feed the allocator's heap (bottom-up reuse kills any
  in-arena survivor - v1's pattern); above-TOP is unmapped. No
  clean reservation exists. The only sketched shape - a hook
  between setter and clear narrowing the bounds the allocator also
  reads - is prerequisites, not a proposal (setter/clear order and
  allocator-word identity both still open).
- INSTALL BOUNDARY (RETIRED - profile artifact): birth-unset words
  and wait-for-words belonged to the AccurateCPUCache=True split
  view. Converged profile: words valid at birth, install races
  nothing. Hardware timing model was never affected.
- TRAP-PC RELIABILITY WARNING: GDB trap stops do NOT reliably land
  on the faulting instruction (backing-state traps stopped at an
  `li` and an `mfmsr`-cluster instruction, neither a store).
  Trap OCCURRENCE stands (values confirm the stores happened);
  trap PCs do not identify instructions. dcbz-trace PCs are exact
  (synchronous hook, disassembly-matched). The 0x805B99D0-cluster
  stops stay unexplained-singular.
- Exclusive-`dcbz` scoped to instrumented paths as required: the
  trace sees CPU dcbz; GDB covers CPU stores/loads (validated);
  DMA, host-side HLE writes, and IOS-side writes are outside both
  and not excluded.

## Install boundary + execution order (dev build, Sep 2026)

- HLE boundary works: `install.fst` loads to `0x92000000` with words
  set, verified, and logged synchronously pre-entry
  (`install dest=92000000 size=153792 verified=1`); no GDB race
  possible. GDB relegated to post-hoc observation.
- ORDERED RECORD (file order = execution order, verbatim-stock run):
  install -> apploader/entry zeroes TOP slot (`pc=800046C0`) ->
  init sets BASE=`0x90000800` (`lr=805B399C`) -> init sets
  TOP=`0x935E0000` (`lr=805B39B0`) -> stack clear -> arena clear
  `[0x90000800, 0x935E0000)` 1.77M dcbz, one call (`lr=805B32E8`)
  -> ... -> TOP NARROWED to `0x90DB4800` (`lr=804BCDE4`,
  streaming module, near end of the clear storm) -> DVD
  work-buffer clears. Setter-before-clear holds by
  value-consistency AND now by order.
- RESERVATION, corrected wording: NO TESTED reservation survives
  (overwritten slots + the wipes above close the tested placements;
  reservation-timing/startup-behavior changes are untested).
  Located candidate (NOT a proposal): `[0x90DB4800, 0x935E0000)`
  (~4.6 MB) is cleared once, then heap-excluded by the narrowing -
  mapped post-boot, fits the table easily. Prerequisites, all open:
  heap-cap reliability, an owned post-clear install point, and
  first-read timing.
- First FST READ: observed extensively since (parser + heap +
  streaming reads logged with values throughout; DMA excluded
  by design). The boot-info words arrive synchronously from the
  apploader (profile artifact retired above).

## Causal order established (dev build, Sep 2026)

- PUBLISHER IDENTIFIED (direct instrumentation, not silence):
  `bootwrite cpu pc=812011D0 lr=812007E4 ea=80000038 val=817DA740`
  (+`3C` sibling) - the apploader's own PPC stores, also seen at
  the HW tap with `dr=1`. No host-side writer ever touches the
  words in any run. The "host-side publisher" theory is withdrawn.
- PROFILE ARTIFACT CORRECTED: the "unset at birth / async
  publication ~1 s post-entry" sequence appears ONLY with
  `AccurateCPUCache=True` (dcache emulation: CPU view valid,
  host view stale-zero until flush). With `AccurateCPUCache=False`
  (fastmem still off, dcbz hook intact) host==cpu==valid at every
  phase (`phase after-close`, `entry host38=cpu38=817DA740`).
  Birth install races nothing; wait-for-words was a workaround
  for the tracing profile. HLE publication is synchronous
  apploader CPU stores - keep the CUSTOM pre-entry install
  (host CopyToEmu+words) marked as emulator-specific until
  compared with GX's real apploader handoff on hardware.
- ROOT-COUNT VALUE (verbatim-stock at MEM2, converged profile):
  exactly one table-span read,
  `read pc=805D153C sp=807F2CD0 ea=92000008 len=4 val=00000000`,
  vs stock control's `val=0000118D` (4493) at the same PC/SP.
  Zero is proven, not inferred.
- ABORT BRANCH (disassembled `/tmp/parser.bin`): `0x805D153C`
  `lwz r4,8(r3)` loads the count; the entry loop
  (`0x805D1550`-`0x805D1638`, 12 B stride, bound `cmplw r3,r4`)
  fails on the first compare with count 0, returns -1
  (`0x805D163C`), and the caller takes its failure leg
  (`0x805D1680 cmpwi / 0x805D1684 blt-`). One read, then
  nothing - matches the trace exactly.
- ORDER (corrected): apploader publishes (CPU) -> HLE install
  overwrites words (deterministic, pre-entry) -> entry, words
  valid -> WIPE dcbz `[0x90000800, 0x935E0000)` -> first read
  (zeros) -> parse abort (-1) -> fatal DVD-path hang. No CPU
  read precedes the wipe; DMA-copy excluded by the single
  direct span read plus absent boot progress.
- Candidate region `[0x90DB4800, 0x935E0000)` stays OWNED and
  CLOSED (streaming reads+writes, free-list traffic) - and no
  late pointer update is proposed as a fix. Any future
  reservation must be respected by BOTH the startup clear and
  the streaming allocator, with ownership proven; none is
  identified. The occupied region is not overwritten.
- Consequence, bounded: the parser reads through a repointed
  pointer and the wipe precedes first use - but the wipe is one
  established fact in the chain, not a certified primary
  blocker. No GX/Wii changes.

## Reference reproduction (converged, preserved Sep 2026)

- Profile: `gxtrace3` (`AccurateCPUCache=False`, `Fastmem=False`,
  `FastmemArena=False`; dcbz slow-path hook intact, no dcache
  split). Preserved as
  `C:\dolphin-trace\reference\Dolphin.gxtrace3.ini`.
- Stock control: unmodified boot, words valid at every phase,
  stock parse reads (`pc=805D153C val=0000118D`) flowing, idle.
  Preserved as `reference\trace-stock-converged.log`.
- Install run: verbatim-stock table at MEM2 pre-entry, single
  zero root-count read, abort, hang. Preserved as
  `reference\trace-install-converged.log`.
- Retired (profile artifacts, do not cite as behavior):
  birth-unset words, async publication/clobber, wait-for-words
  and repoint-after-waitmem, install-races-publication. All ran
  under AccurateCPUCache=True host-stale reads.

## Grown-parse matrix (dev build, Sep 2026)

All runs: base ISO + GX table at `0x90000800` reservation + BASE
redirect (or noted poke), converged profile. Verdicts by 75 s
(wedge = halt-timeout + dead signature; idle = `0x805BCCB0` EE-on):

| variant | bytes | entries | outcome |
|---|---|---|---|
| M0 stock verbatim | 153792 | 4493 | IDLE, 494625 reads |
| M1 stock+76 KB trailing zeros | 230076 | 4493 | bdnz compute loop at 75 s, IDLE by 325 s (SLOW, not stuck) |
| M2 +10 files | 154122 | 4503 | IDLE |
| M4 +500 files | 169312 | 4993 | IDLE |
| M6 +100 files, 600 B names | 215112 | 4593 | IDLE |
| M7 +60 zero-length files | 155672 | 4553 | IDLE (zero-length innocent) |
| M8 +2000 files | 215812 | 6493 | IDLE (count to 6493 innocent) |
| M9 267 files lengths+4096, stock offsets | 153790 | 4493 | IDLE (lengths innocent) |
| M10 267 files stock lengths, 6 GB offsets | 153790 | 4493 | WEDGE ~1237 reads, no DI/DVDThread activity |
| M11 267 files shifted +1 MB in-disc | 153790 | 4493 | past M10 point (1561 reads), thread reads flow, later content wedge (expected: wrong bytes) |
| Spectral GX 230076 | 230076 | 6618 | WEDGE ~1240 reads, no DI/DVDThread activity |
| Dolphin 230076 table | 230076 | 6618 | WEDGE ~1764 reads, same signature |

- REJECT CONDITION: mod-range OFFSETS, pre-submit. M9 idles /
  M10 wedges with entry count, names, and lengths controlled;
  count (M8), strings (M6), zero-length (M7), lengths (M9),
  padding-slowness (M1) all exonerated as wedge causes. With
  6 GB offsets the game never issues any DI/DVDThread request
  (hook proven live: stock/M11 runs log thread reads); with
  in-disc offsets it proceeds to served reads. First divergence
  is layout (own string bases), not behavior; the abort leaves
  no table reads and identical heap-setup tails.
- SERVING STATUS: backend moved to the true choke point
  (`DVDThread::ProcessReadRequest` serves file + DTK streaming
  with correct async completion; a `PerformDecryptingRead` hook
  was tried and removed as redundant). Path bug found and
  fixed: msys `/j/...` paths are invalid Win32 (fixed by
  normalization at load). Serving PROVEN live: 11 correct reads
  of mod audio (`SMR.szs`, 32 B probe + streaming chunks)
  with disc fallback intact (Error#001 probe still errors).
- BARRIER, precisely localized (instrument-before-change):
  DI entry logs every request; the bound check logs offset/len/
  limit/decision without changing behavior. M10 run: game
  SUBMITS the 6 GB read twice (retry), IOS pre-checks pass
  (partition open, buffer exact), `diret=submitted`, then
  `oobcheck ... WOULD-ERROR` -> BlockOOB + DEINT -> fatal.
  "PPC-side rejection" WITHDRAWN: the game submits past any
  game-side check; the barrier is the emulator's
  `m_disc_end_offset` comparison. Emulator-backend barrier, not
  a Wii defect finding.
- BOUND BYPASS (diagnostic, mapped-only): skip BlockOOB solely
  for fully-mapped extents (same map the thread serves from);
  unmapped OOB still errors; nothing carried to GX. With it,
  Spectral runs serve mod bytes and proceed past the old stop
  (  streaming flows); the remaining stop is a LATER per-open
  offset gate (M11 in-disc proceeds past M10's stop with thread
  reads flowing; M10/6 GB stalls with zero thread requests while
  the hook is proven live - both pre-bypass observations, now
  explained: unmapped OOB errored before the thread).
  Next: operand identification (partition-size source the open
  path reads) + size-report extension test. No GX/Wii changes.
- Wedge-point forensics: stop lands mid-string-walk over VALID
  NUL-terminated names (`MessageData`); no malformed structure
  at `0x900171B8`. Post-read tail = heap/DVD-request setup
  (426 writes) then silence; process ~2% CPU (blocked, not
  spinning); only dialog is a benign host-font warning.
- Hardware reading: T0 (unopened created files) and Yoshi
  (in-place) never opened a mod-offset file, so no hardware run
  contradicts a DOL-side range check - but none confirms it
  either. If the DOL checks against IOS-reported size, cIOS may
  satisfy it on hardware (unverified). Next probes (no GX
  changes): extend `m_disc_end_offset` + log DI entries, rerun
  M10/Spectral; find the DOL check's operand on a miss.
- Method caveats: NoGUI-master second-halt-in-session times out
  (single-halt sessions work; trace file is authoritative);
  `reservewrite`/`dcbz` reservation silence re-verified each
  matrix run (0 in-reservation writes).

- CONTRACT (exact, file-order from the converged trace): entry
  zeroes TOP (`pc=800046C0`) -> setter writes BASE=`0x90000800`
  (`pc=805B4ED0 lr=805B399C`) and TOP=`0x935E0000`
  (`pc=805B4EA0 lr=805B39B0`) -> clearer reads BASE+TOP via
  getters (`pc=805B4E70/805B4E40 lr=805B32BC/32C4/32B4`) then ONE
  `DCZeroRange` call (`pc=805B551C lr=805B32E8`) -> streaming
  narrower reads both (`lr=804BCDC4/CC`) and writes TOP only
  (`lr=804BCDE4`, preserves BASE-relative size `0xDB4000`) ->
  later consumer reads narrowed TOP (`lr=8059D08C`). All
  consumers read the same two slots the setter writes.
- MECHANISM (dev harness, SMG2-specific values): redirect the
  BASE setter `0x90000800->0x90040800` (256 KB), install the
  table at `0x90000800`. Clearer, heap, and narrower all consume
  the patched value (traced); the streaming TOP write is
  untouched, its ownership preserved. No address-skipping, no
  occupied-region overwrite.
- STOCK VALIDATION: byte-identical post-75 s dump (CRC
  `36f2a321`), 494k valid parser reads
  (`ea=90000808 val=0000118D`), normal idle (not the dead park),
  pointer intact, narrowing + streaming traffic intact.
  Survival, lookup, and allocator activity all proven locally.
- PRODUCTION SHAPE (identified, unexecuted): pokes proved the
  setter clamp keeps raw incoming when the floor is raised
  (BASE stayed `0x90000800`, wipe proceeded) - so the floor
  immediates are NOT the patch site. Identified instead: patch
  the BASE getter (`0x805B4E70`, 8-byte `lwz/blr`) with a branch
  to a loader-planted trampoline (`lwz/addis/blr`, +`0x40000`)
  in a code cave. ONE getter feeds clearer, heap, and narrower
  alike (traced), so a single 4-byte branch + 12-byte trampoline
  narrows all three coherently; the   streaming TOP write is
  untouched. Loader-applicable post-apploader pre-entry (same
  verified point and DOL-patch machinery as table install).
  SMG2-specific addresses; needs cave survey + patch code + test.
- TRAMPOLINE AUDIT (static DOL + dynamic traces): BASE getter
  `0x805B4E70` (`lwz r3,-27068(r13); blr`) has 12 call sites;
  surveyed uses are subf-differences, single adds, and compares
  - no site sums two getter results, so a uniform +C preserves
  every relation (clearer start/end, heap base, narrower math).
  NO direct slot readers exist outside the getters (static scan
  of text for `lwz *,-27068(r13)`/`-3228(r13)` finds only the
  getters; dynamic traces across multi-minute runs show only
  known getter/setter PCs). Setters fire once (init) plus the
  TOP narrowing - no later BASE rewrite, so no double-adjust
  path exists in the traced behavior. ABI: trampoline
  (`lwz/addis/blr`) touches only r3, preserves LR/CR/stack/r13
  by construction. Cave ownership through gameplay NOT yet
  proven (12 B paddings abundant in text; need birth/idle/+min
  reads of the chosen cave) - the patch stays unexecuted.
- SPECTRAL TABLE: 1238 valid reads (root `0x19DA`, entry/name
  bytes), ZERO dcbz lines inside the reservation, then wedge -
  SUPERSEDED by the matrix below: the wedge is pre-submit offset
  rejection (M9/M10 split), not missing-backend content failure.
  Survival + lookup stand; content serving awaits a run that
  reaches submit.
- WRITE-SILENCE (boot-to-extended-idle, ~4.5 min, 4.4M-line
  trace): ZERO `reservewrite` + ZERO dcbz inside the reservation;
  game alive across code regions, narrowing intact. Level-loading
  coverage rides the backend run below.
- BACKEND (Dolphin Riivolution DirectoryBlob, native): v11a
  descriptor boots to idle with modded FST (`w38=0x817C7D40`
  `w3c=230076` - same 230076 bytes as GX's plain build, cross
  implementation agreement); `replacements.log` maps 2088 files
  (CustomCode 15 incl. 5 LoaderSB4, 267+1881 shape) with
  disc offsets/lengths and disc fallback; 299k stock-site parse
  reads flow. STATUS, corrected: serving is IMPLEMENTED but
  UNEXERCISED in the integrated run - the empty `gxserved.log`
  (no GX-offset read arrived before the parse wedge) establishes
  nothing about content delivery. Save selection + playable
  level need input driving (movie/input poke - planned, not
  done); Wii stays paused until that integrated run passes.
- INTEGRATED RUN (base ISO + GX Spectral table at reservation +
  redirect + GX-offset backend): configuration PROVEN in-run
  (words `0x90000800/0x382BC`, GX table head `0x19DA`, redirect
  logged, backend map loaded) - but the game wedges in table
  parsing (1240 reads, last `ea=900171B8`, stub unresponsive)
  BEFORE any content read, so the backend never fires
  (`gxserved.log` empty). Integrated-to-title NOT achieved.
- CONTROL kills the serializer theory: Dolphin's OWN 230076-byte
  table at the reservation wedges IDENTICALLY (1764 reads, 112
  distinct, same string-compare PCs, same dead park) - while the
  stock 153792-byte table idles. Byte-diff of the two 230076
  tables: same 6618 paths, but 191KB differ (GX appends new
  content at end + leading-NUL strings; Dolphin alpha-inserts).
  The wedge tracks grown size/count (6618 entries), not
  serializer, offsets, or content. Mechanism open (parse-abort
  vs fault on a specific entry; exact trigger unidentified) -
  and it re-scopes everything below: reservation + survival +
  backend are proven, but a grown table does not parse in MEM2
  regardless of who built it.
- HLE PUBLICATION STATUS: our pre-entry host install (CopyToEmu
  + words overwriting apploader-published stock words) stays
  marked emulator-specific until compared with GX's actual
  apploader handoff (loader `InstallPendingFst` post-apploader
  on hardware). Timing and context differ; the reservation
  contract itself is game-side and would carry over.

## Inputs ledger

- CAPTURED (SB4E01 T0 card logs, v3.36+): arena `{0, 0x817DA740,
  0x817DA740, 153792}`, wants `153934` / `153792`, block `[0x817D8740,
  0x817DA740)`, BSS `[0x80728680, 0x807E3188)`, expected dests
  `0x817B2DE0` / `0x817DA740`.
- SYNTHETIC (labeled in code): one stale-table sample `[0x817DA800,
  0x817DA900)` inside the captured reservation, exercising the skip
  counter; the staged tables are synthetic valid FST heads with marker
  pads, not real FSTs; churn sizes are stand-ins.
