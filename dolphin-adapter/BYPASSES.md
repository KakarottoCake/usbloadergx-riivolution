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

## Inputs ledger

- CAPTURED (SB4E01 T0 card logs, v3.36+): arena `{0, 0x817DA740,
  0x817DA740, 153792}`, wants `153934` / `153792`, block `[0x817D8740,
  0x817DA740)`, BSS `[0x80728680, 0x807E3188)`, expected dests
  `0x817B2DE0` / `0x817DA740`.
- SYNTHETIC (labeled in code): one stale-table sample `[0x817DA800,
  0x817DA900)` inside the captured reservation, exercising the skip
  counter; the staged tables are synthetic valid FST heads with marker
  pads, not real FSTs; churn sizes are stand-ins.
