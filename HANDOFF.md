# Handoff — 2026-09-09 (updated: Spectral 20-min + yom/SOME01 triage; phase-timed locally; questions only, no runs)

State of the SB4E01 (Super Mario Galaxy 2) debugging effort. Read the
"Latest evidence" section first — it supersedes the drive-blocker framing
below, which is kept for the steps it still requires.

## Spectral 20-min + yom/SOME01 triage (light frozen ON = hung, not playing)

- Spectral PAL (2378 files, 15 folders): log ends at "table serialised:
  239688", light solid ON 20 min. Solid-ON-persisting rules out handover
  (1 s then dark), refusal (blink groups + menu) and play (dark+flicker):
  hung pre-handover with the light frozen. Card log ends before the next
  persist, so the hang is in [expectations, FstWalk, compaction, report,
  gate] - all pure CPU. Timed LOCALLY at full scale (real base + real
  1912-file tree: 1675 add/237 repl, same shape as the log's 2141/267):
  expectations 0.00 s, walk-open 0.00 s, walk-check 0.02 s, compacted
  209016 in 0.06 s, staging memcpy+CRC 0.00 s; plain 222674 vs log's
  230076 (version drift, same order). NO algorithmic hang at this scale.
  Verdict: not a CPU loop - crash/exception in that window, or card-side
  stall; needs the file tail (does it end at serialised?) to localize.
  Late installation stays unproven by design (card gone) - stated.
- Yoshi (7 replacements, table 153790 IN PLACE): relocation exonerated.
  cIOS 249 = beta1, and the "cannot be served" verdict is LOGGED but
  consumed NOWHERE - fragments register and hook reads proceed against
  an incapable cIOS. Log ends inside hook verification = hung-WDVD_Read
  shape. SOME01's empty-mod beta1 run is the control (clean WITHHELD,
  nothing read). Suspected missing gate (withhold file work on negative
  verdict): proposal only. Needs: log tail + screen + (config, minimal)
  a beta3-slot run.
- SOME01 resolved: folder path resolves but EMPTY (populate or fix XML)
  + all slots beta1 (use beta3). As-run = correct no-mod boot, nothing
  broken if that was the intent.
- Newer 24 mismatches: USA-vs-PAL revision drift, correctly skipped,
  do-not-force stands. SMNP01 §12 regression green (incl. a self-caught
  hand-arithmetic slip the test now guards).
- Generality ledger: compaction live on SB4E01 x2 + RMGE01; grown path
  preserved on SMNP01/Spectral-shape; refusals intact; preflight caught
  USA/PAL drift. MEM2 marker dormant throughout.

Open, no new runs (tails of existing files + outcomes):
1. Spectral file: does it end at "table serialised"? Screen/light
   during the 20 min (already answered: solid ON - hung, recorded).
2. yom file tail (from "checking...") + screen? 3. Yoshi on beta3 slot?

## Hardware round on v3.43 (c7d6d27a): 4 logs, compaction live on 3 titles

All builds current (c7d6d27a == v3.43 code). Compaction engaged exactly
as the host driver predicted wherever it was tried:

- T0 P1 (SB4E01, gxdiag): plain 153934 -> compacted 144323 (walk passed),
  STAGED instead of plain, in place with 9469 spare, OUTCOME FST_STAGED,
  staged checksum intact at shutdown. Byte-exact match with the local
  serializer run (153934/144323). CONFIRMED 2026-09-09: boots in game.
- Gravity Demo (SB4E01, 151 files, 33 repl + 166 add): plain 158968 ->
  compacted 147769 STAGED in place, shutdown checksum intact.
  CONFIRMED 2026-09-09: boots in game.
- RMGE01 Daredevil (17 repl, 0 add): plain 81309 -> compacted 79993
  STAGED in place. (Prior batch; same mechanism, third title.)
- Newer SMNP01 (PAL, 1100 files, 104 repl + 996 add): plain 63274,
  compacted 60574 - BOTH overflow the 36712 reservation, so the grown
  path is correctly preserved (planned 0x817E5940, verified against a
  new host test with the log's exact arena/ranges). Compaction did not
  eliminate relocation here; it was never expected to at +26 KB.
- SMNP01 24 preflight mismatches: USA-built patch bytes vs PAL disc
  bytes across 0x802f/0x8032/0x801b/0x800e/0x800b/0x8015 regions
  (version tags, lis constants, branch-vs-load codegen, string data
  where code was expected). Revision drift, correctly soft-skipped;
  10 exact patches applied. DO NOT FORCE - writing USA opcodes over PAL
  code is a crash. Fix belongs in a PAL-specific XML, not the loader.
- Spectral (SB4E01, 2148 files, 503 memory patches): log ends at
  "table serialised: 230076 bytes" (tail needed). Local reproduction
  with the real base + real 1912-file local tree: 1912 applied (1675
  add / 237 repl, ratio matches), plain 222674, compacted 209016
  (0.06 s, parses, CRC logged) - both overflow, grown path stands.
  Ops review Serialize->checkpoint: FstWalk.Check, report, FragPlan,
  Activate (MEM2_alloc + memcpy + CRC), hook verification reads,
  probe, ReportFstPlacement, shutdown, InstallPendingFst - nothing at
  230 KB scale exceeds tested shapes (host scale suite covers 3920-file
  tables; install mechanics proven to 153934 in Dolphin). Neither this
  log nor Gravity's establishes late installation - structural: the
  card is gone by then, so only blink codes or Gecko could, and neither
  is in these logs. Stated, not overclaimed.
- Yoshi (SB4E01, 7 replacements, table 153790 <= 153792: IN PLACE):
  relocation exonerated by the numbers. Two findings: (1) cIOS slot 249
  is beta1, not beta3 - verdict logged ("cannot be served") but consumed
  NOWHERE downstream, so fragment registration + hook-verification reads
  proceed against a cIOS that cannot serve them; the log ends inside
  "checking the mod's files through the hook", the shape of a hung
  WDVD_Read. SOME01's empty-mod run on non-beta3 is the control: with
  nothing to read, the same verdict boots clean (WITHHELD NOT_SWITCHED).
  Suspected missing gate (withhold file work on negative verdict) -
  proposal only, no code changed. (2) Tail + screen needed to confirm.
- SOME01 (grookeytambourine): folder path resolves but is EMPTY (0 files)
  + all slots beta1 at the time -> correct WITHHELD NOT_SWITCHED no-mod
  boot, dry run, game untouched. Resolution: populate the folder (or fix
  the XML path) AND use a beta3 slot; if no-mod was intended, nothing is
  broken - the log proves clean pass-through.

Regressions encoded (host, CI): §12 SMNP01 exact-numbers placement
(0x817E5940 + compacted-60574 variant), §13 T0-compacted-in-place
consequence (144323 -> original address, arena untouched), t0serializer
delta/compact/153934-gate, §9 T0 exact addresses (existing). Hardware
pins recorded here: T0 153934/144323, Gravity 158968/147769,
RMGE01 81309/79993, SMNP01 63274/60574->0x817E5940.

Generality (not game-by-game): compaction live on SB4E01 x2 + RMGE01,
grown path preserved on SMNP01/Spectral-shape, refusal paths intact
(SOME01 empty, malformed rules), preflight caught USA/PAL drift on
SMNP01. MEM2 marker path untouched by all of this (still dormant).

Open, no new runs (questions on already-run tests + tails):
1. T0 P1 screen outcome? 2. Gravity screen outcome?
3. Spectral log tail (from "checking the mod's files") + screen?
4. yom log tail (from "checking...") + screen? 5. Yoshi on a beta3
slot (251/252 per the SB4E01 surveys) - config change, minimal.

## v3.43 pre-release (published, gated) + GXDiag v9 + hw7 pack

- `v3.43-riivo-compact`: pre-release (not draft, no Latest badge), 4
  assets. boot.dol SHA-256
  `218B5F178C6E673A5D2F5137CCF15C68C80259ACF849EE37A39777EA306E3F0C`,
  binary-gated (commit `c7d6d27a` in strings + compaction/MEM2 log
  strings present; ancestry of fix commits verified pre-tag, avoiding a
  repeat of the v3.36 mistag).
- `GXDiag-SB4E01-v9.zip` (12 entries, probes byte-identical to v8):
  CHECKLIST/README rewritten for compaction (T0 must show "compacted
  table ... STAGED" + "fits in the room", never "extended downwards";
  three-chapter history retold). Workspace working copy synced.
- `pkg-t0-6099af11/`: exact CI DOL + manifest + T0-once instructions
  (markers removed).
- `pkg-hw7/`: one CI build (`fd7bbc84`, verified strings) + gx7many
  (N6 option + 500-file overflow workload) + CHECKLIST-7/RESULTS-7/
  MARKERS.md. N1-N5+N7 runnable; N6 HELD (no passing MEM2 site).
  Developer-side validation complete; hardware time NOT requested.
  MEM2 Dolphin verdict (Test 6 HELD, gate unmet): game MEM2 grows
  bottom-up from `0x90000000` (sparse first MB at 90 s); reads above
  ~`0x93700000` fault post-boot (outside game mapping); `0x92000000`
  chosen on use+margins, NOT zeros. Consumer checks negative: top site
  faults the game+stub dark; mid site installs land but the game never
  reads the table (no TitleLogo hit in 200 s+) and parks. Mechanics
  (real PlaceFstMem2/InstallFst/probe in emulated MEM2) pass in the
  adapter. No MEM2 site passes game-consumption - nothing validated is
  substituted; the marker ships dormant and Test 6 documents its
  re-entry criteria instead of a run.

## Repair candidate 1, VERIFIED in real startup: in-place compaction

`FstBuilder::SerializeCompacted` (new, pure, host-tested): same entries,
paths, offsets and sizes as `Serialize`; shared string tails stored once
(offsets are arbitrary in U8, so overlap is format-legal). T0 workload:
153934 -> 144323 bytes (9611 saved in 0.03s), every one of 4496
paths/offsets/sizes byte-identical between variants. Production
(RiivoBoot.cpp) stages the compacted bytes when plain overflows the
boot.bin reservation but compacted fits - single staged table, zero
pending-state plumbing changes, refusal-preserving (unknown reservation,
failed build/walk, or still-overflowing compaction all keep today's
bytes exactly).

Real-game result (birth-installed, GDB): compacted table at the original
address, words `0x817DA740/144323/arena kept` - game reaches the
baseline idle signature, table fully intact post-run, and the TitleLogo
entry fires the IDENTICAL consumer (same function, same thread stack) as
the unmodified baseline. Milestone equivalence, not just idle PC:
same consumer, same trajectory, surviving table. Full suite green
(t0serializer gates both the 153934 plain and the fit).

## Real SMG2 startup: T0 failure reproduced, mechanism identified

Correction first: the missing-key claim was wrong. Dolphin carries the
retail common key built in (`IOSC::LoadDefaultEntries`, verified in
source), unwraps the ticket's title key, and decrypts internally - no
keys.bin needed. What actually blocked the ISO was a spaced-path
argument split (dialog: file "D:/Games/Wii/Super" not found), caught by
screenshotting the hidden Warning dialog. With quoting fixed, the
authorized backup (header SB4E01, partition map + tickets + both TMDs
parsed sane, data title 00010000-53423445) boots: apploader entry
`0x80004050`, then game code, no keys anywhere on the PC.

What runs, with GDB installed at game birth (12s halt, heap empty):
- UNCHANGED: baseline idle (`0x805BCCB0`, 3 rotating stacks, words
  untouched). ARENALIE (arenaHi lowered only): identical baseline -
  the game ignores arenaHi for behavior. IN-PLACE rewrite (150KB via
  GDB, words untouched): baseline, table intact - the write path is
  harmless, methodology exonerated.
- RELOCATED grown T0 table (real serializer bytes, 153934, verified
  pre-go) and VERBATIM-relocated (stock bytes moved): identical failure
  - table zeroed across its full span within seconds of entry, game
  parked at `0x805B2B14` (EE off, frozen SP, scheduler spin) while boot
  words stay intact. Content is innocent (verbatim dies too); position
  is everything.
- Apploader-time layout (halted snapshot): `[0x817B0000, 0x817D8740)`
  is 165,696 zero bytes; the 8KB block holds 4 stray bytes; the cascade
  target is a zero desert. Post-wipe diff: installed bytes -> zeros
  across the whole span, plus the 4 strays cleared.
- Marker control (clean words, 32B planted post-boot): cleared within
  ~25s while the game behaves baseline. The wipe is ROUTINE game
  behavior on that span, not a reaction to our words - with original
  arenaHi the span is inside the game's own heap/clear zone.
- Wiper regression evidence (all GDB/RAM-grade, no other channel):
  full-span diff (installed 153934 real bytes -> zeros, plus 4 stray
  block bytes cleared); timing under 4s after game entry (first poll
  already dead+frozen); trigger matrix (verbatim-relocated dies,
  lying-arena+relocated dies, arenalie-only lives, in-place lives);
  frozen spin disassembled (scheduler spin + queue-walk caller at
  `0x805B6390`, retry-bounded); entry-page disassembly shows the game's
  own BSS/relocation loop, an `arenaHi` store at `0x80004148`, and a
  `DCFlushRange` utility at `0x800041C0`. NOT captured: the exact
  dynamic clearing instruction (CPU-store watchpoints never fire -
  dcbz/DMA-shaped suspect list open) and the wipe's lower bound below
  `0x81600000` (sentinels dead from there up). No placement address is
  substituted on this evidence; MEM2 relocation stays a separate,
  unimplemented track with its own open question (game MEM2 usage).
- SCOPE CORRECTION (was overstated): what the experiments establish is
  narrow - (i) lowering arenaHi with no table changes nothing observable
  in this window (arenalie control); (ii) the wipe proceeds identically
  whether arenaHi is lowered or left original (lying-table run). That is
  all. arenaHi's broader contract role (heap bounds elsewhere, other
  games, later phases) is UNTESTED, and the entry-page code shows the
  game WRITES arenaHi itself at `0x80004148` - it actively manages the
  word. No claim beyond (i)-(ii).

Consequences: below-reservation placement is dead for SMG2 no matter
what the arena words say - the cascade-down fix steered into a clear
zone. In-place stays the only proven-safe MEM1 region. Two fix tracks:
(1) suffix-compacted string tables to fit T0-sized growth in place
(needs ~142B; whole-table suffix overlap should yield KBs); (2)
MEM2-resident grown tables (outside MEM1 clearing entirely; needs a
game-MEM2-avoidance survey, queued in Dolphin). Open reference: where
official Riivolution / riivolution-to-iso puts grown FSTs. Unlisted-block
experiment stays deferred (nothing left unaccounted that needs it).

## Latest evidence: install split confirmed on v3.34, same build throughout

| Run | Table staged | Result |
|---|---|---|
| T0 (`v3.33`) | Rebuilt, 153,934 bytes at `0x817da6a0` | Black screen |
| T0 + `relocorig.txt` (`v3.33`) | Verbatim original + 160 zero bytes, 153,952 bytes, same address | Black screen |
| T0 + `nofstinstall.txt` (`v3.34`, behavior-identical to v3.33) | Install skipped | Game boots |
| T1 Identity (`v3.34`) | Rebuilt same-size table, IN PLACE at `0x817da740`, hook + fragments live, real redirect for TitleLogo.arc (identical bytes) | Game boots |

Both installs target `0x817da6a0`, 160 bytes below the original table.
Both pass file read checks and report an intact staging checksum before
shutdown. Neither persistent log proves the late install completed.

Reading: install (either table) black-screens, skipped install boots, on
behavior-identical builds. The hook/fragment half is exonerated with the
hook and fragments live — but note the scope precisely: the game boots
with the ORIGINAL table installed, so this proves nothing about the game
consuming mod files. Serializer-only explanations are insufficient (the
verbatim original died too). The suspect is the shared path both failing
runs take and the passing run skips: late staging checks, the FST copy,
install verification, and the pointer/size/arena updates.

T1 (`v3.34`) installs its rebuilt table IN PLACE at `0x817da740` — no
relocation, no heap change — with hook and fragments live and a real
redirect for TitleLogo.arc, and boots. So the install write itself,
same-address pointer/size updates, and a live redirected read path do
not break the boot. What T1 does NOT
prove: that the game consumed our bytes — the copy is identical, so a
boot that ignored the redirect looks the same. That is T2's job.

## Dolphin reproduction: PRODUCTION InstallPendingFst verifies both modes (2026-09-08+UTC)

Retraction first: "the divergence is not in this path" overstated what a
mirror + synthetic state can claim. v3 answers it properly: the REAL
RiivoBoot.cpp TU links under emulation (gc-sections keeps only
InstallPendingFst's closure) and the REAL `InstallPendingFst` runs for
both modes with GDB-poked staging (no production seam, no logic copy).
Result: code 0 both modes, relocated + in-place spans dump byte-exact
(153934 + 153792 bytes, 0 mismatches). Omitted-ops prioritization
delivered: the biggest omitted piece - the actual caller - is now in.
NO production defect found in InstallPendingFst/InstallFst on these
inputs.

v4 adds the probe jump: absolute-address consumer entered by branch
after cache maintenance, reading the table through the repointed words
exactly like game startup would. Three-way result, all GDB-visible:
UNCHANGED consumes the modeled base (8093 entries, base CRC, arena
kept), RELOCATED consumes the production-installed grown table (8101
entries, staged CRC, arena lowered to `0x817B2DE0`), IN-PLACE consumes
the production-installed same-size table (8093 entries, staged CRC,
arena kept). Both installed spans re-dumped byte-exact afterwards. The
three consumptions are identical in shape - no differing read,
exception, or overwrite between them in emulation.

Gated re-run closes it watertight: two-way rendezvous (adPhase/adModeDone
+ acks, transition-gated, no sleeps-as-sync), per-probe nonces 1/2/3 all
verified fresh in one run, both spans byte-exact again. Every earlier
"stale read" re-explained as script-early reads - no stub read-caching;
the nonce + memdiff-change rule stands as the trust criterion. The
sleep-synced misalignment and the "lost ack" sagas were both script
protocol bugs (pokes/resets landing in unseen waits); the harness and
the production code were never at fault in those rounds.

Missing input, named once: the Wii common key (16 bytes, console-OTP
derived). The authorized SB4E01 backup is local
(`D:/Games/Wii/Super Mario Galaxy 2 (USA) (En,Fr,Es).iso`, header
verified SB4E01; partition map parsed), but its content partition is
AES-CBC locked and no keys.bin exists anywhere on this PC (Dolphin
dirs, bios, RetroBat user, Documents all swept; NAND backups do not
contain OTP). The key unblocks BOTH halves at once: real base-FST bytes
for byte-exact T0 serialization (host driver `test_t0serializer`
already asserts +144 delta and gates 153934 exact when T0_BASE_FST is
set), and real DOL/apploader/game startup under Dolphin's normal disc
backend. A Wii-side 153792-byte FST dump would unblock the serializer
half only. Wii testing stays paused - this is queued, not asked.

Harness postmortem (do not repeat): a mid-round "freeze" in the pre-copy
CRC was MY 52-vs-60 struct-size overrun poisoning size+crc through the
GDB poke - GPR forensics (marching cursor, clobbered bound) + byte-exact
RAM reads proved it, mailbox bloblen fixed it, small-size success is
explained (later overrides repaired the two words). Classified: harness
defect, illustrating a failure mechanism, NOT evidence of production
corruption. A sleep-synced round later executed an install in the wrong
labeled slot; rendezvous is now two-way (adPhase/adModeDone + acks, no
sleeps-as-sync). Stub protocol that survived 12+ runs: p/m/M/Z anytime,
run-control ONLY from stopped, doprod+pokes verified by RAM re-reads,
GDB reads bypass dcache (unflushed `placeOk` reads stale). Full ledger
in `dolphin-adapter/BYPASSES.md`. No tester run asked or needed.

v1 (isolated copy) and v2 (caller mirror) are superseded by v3 above
and kept for the record in `dolphin-adapter/BYPASSES.md`. The v2-era
sentence claiming the divergence "is NOT in this path" is retracted -
a mirror cannot clear the production path.

## Menu claim qualified: forward progress yes, functioning menu no

"GX reached a running menu" overstated. What Dolphin establishes (v3.42
boot.dol, 18995, no disc): five `regs`-polls over ~50 s of execution show
PC `0x80D43FDC` -> `memcmp` (via `wd_fix_partition_table`) -> inside
`wd_fix_partition_table` -> `0x80D2FD4C`, SP migrating `0x8108E8B8` ->
`0x8108EA40` -> `0x8118C238` (second stack = thread activity), MSR EE on.
That is sustained execution through loader code, not a wedge at entry or
an exception spin. It is NOT a functioning menu (no input/render check
under Null video). Correction: without any GDB session the CPU sits at
entry (stub force-pause) - earlier "booted to menu" runs all had a
`cont`. Stub method that works: `go` + `regs`/`mem` polling; halting a
running CPU is flaky on this stub.

## Dolphin reproduction: adapter PASS (2026-09-08+UTC, developer-side)

GX `v3.42` `boot.dol` boots under Dolphin 5.0-18995 (`-b`, Null video, no
disc) to a running menu: after 25-30 s PC advances `0x80B00000` ->
`0x80DD5D58` (`memcmp`, caller `wd_fix_partition_table` +0x80 per the
`v3.42` map) / `0x80D43FDC`, MSR `0x9032`, SP `0x8108E8B8`/`0x8108EA40`.
Ceiling confirmed: no cIOS, so no disc mount, no apploader, no FST path
on hardware-equivalent inputs. Old RetroBat 5.0-14344 stub is unreliable
(accepts then silent); 18995 answers `p/g/m/M/Z` correctly.

`dolphin-adapter/` (new, sibling of `source/`, NOT in the loader build)
compiles the REAL `RiivoFstInstall.cpp` unmodified with captured SB4E01
T0 inputs (arena `{0, 0x817DA740, 0x817DA740, 153792}`, want 153934,
block `[0x817D8740, 0x817DA740)`, BSS `[0x80728680, 0x807E3188)`, one
synthetic stale sample). Under GDB: write watchpoint on
`[0x817B2DE0, +153934)` fires exactly once inside `memcpy`
(LR=`InstallFst`+0x80); after completion `0x80000034/38/3C` read
`817B2DE0/817B2DE0/0002594E`; full-span RSP dump (153934 B) verifies
byte-identical, 0 mismatches, sentinels intact. Proven: real PPC
`PlaceFst` reaches the known answer and real `InstallFst`
memcpy+flush+repoint lands every byte in emulated MEM1 with no DSI.
NOT proven (reserved for Wii): real apploader layout, live loader-stack
position (model A/B), game read-back, cIOS reads. BYPASSES.md holds the
ledger. Post-run CPU parks in `KThreadIdleMain` (SP reads 0) - idle
thread, not a crash. Harness: Temp `rsp.py` (ack-less, single session,
`OK` vs `O`, length-checked `m` replies); build needs a space-free path
(GNU make limitation, README documents staging).

## Collision demoted: fixed defect, not the cause

T0 with obstacle-aware placement (table below the 8 KB block, verified
in the log) STILL black-screens. The overwrite was real and is still
guarded against, but it is not the established cause — stopped being
treated as one. What remains is audited in the next section, not
hypothesized per round.
Ruled out as consumers of the changed words: the IOS hook (reads no
boot words), the jump sequence (reads none, wipes only loader BSS),
everything between placement and install (no writer to `0x80000030-3C`
outside apploader/InstallFst). Remaining consumer: game startup itself.
The 8 KB block reports "no section match" (table WAS read, so this is
absence, not a failed read) — purpose still unknown, and avoidance
cannot exclude references it may hold to the original table.

## Relocated-FST handoff audit (model A vs model B, then code)

References, strongest first: R1 in-place installs (boot on this game -
same engine, same game, only the relocation deltas differ); R2 stock
boot (apploader contract: table at top, arenaHi below it, Lo zeroed
for the game - our `Disc_SetLowMem` matches it); R3 the official
Riivolution mechanism as publicly documented (grown tables with updated
address+size+arena on real hardware - the WORD updates are
reference-blessed; what differs here is address choice and path, which
is exactly what this audits); R4 Dolphin FST semantics at serializer
level (in-tree tests, no placement content).

Item by item, relocated vs R1/R2:
- Table pointer (`0x80000038` new value): apploader-contract word, read
  at game startup - the only consumer (no loader, IOS, or jump reader
  anywhere in tree). R3-blessed pattern. RESOLVED, no change.
- Size (`0x8000003C` = actual serialized size): table content is
  self-delimiting U8; exact max is safe under every reading of the
  field, and R3 writes grown sizes too. RESOLVED, no change.
- Arena high lowered by reserved: game heap-init bound; loader heap far
  below (BSS end `0x8106C260` + small-object demand, break logged per
  boot); game not running so nothing allocates in the taken range.
  RESOLVED, no change.
- Arena low untouched (0): matches stock (loader zeroes pre-apploader,
  game fills at startup). RESOLVED, no change.
- Staged-buffer integrity across phases: MEM2 staging, checksum at
  shutdown, pre-copy CRC re-check (codes 2/4/5 name rot vs write vs
  pointer faults separately). Mechanism present. RESOLVED, no change.
- Cache visibility (`InstallFst`: copy, flush table, write words, flush
  `0x80000030` block): data-only table needs no IC invalidate; no
  concurrent readers exist (game dead, IOS parses no FST); single
  thread, stores-then-flush before handoff - standard shape.
  RESOLVED, no change.
- Everything executed afterward: patch engines write game-code regions
  and the fixed `0x80001000` handler slot (Hooktype-gated; T0 runs
  Hooktype=0 so no handler loads); loader heap far below the
  destination; jump is stock code that boots unmodified games.
  Reviewed by region, not line-exhaustive - stated scope. No writer to
  the destination span identified. RESOLVED within that scope.
- Jump + game startup: stock / external. Decided by hardware
  (acceptance run below), not by further reading.
- Destination span below the reservation: the ONE open item. Heap is
  empty (game dead) except possibly live loader stack frames. Two
  models, one discriminator (SP/stack-top from existing T0 logs -
  zero hardware needed, just send those lines):
  model A (stack above: top near MEM1 top, SP ~`0x817FDxxx`) makes the
  cascade destination (`0x817B2DE0..0x817D772E`) clear and the OLD
  destination fatal, and convicts the EVIDENCE-PATH stall seen in the
  v3.41 log as the actual blocker;
  model B (stack below the reservation) makes ANY downward growth hit
  live frames, cascade included, while in-place stays structurally
  safe. Fixture case 2 pins the detector; the target enforces nothing
  yet - THAT fork (refuse-grown vs on-demand vs measured-clear) is
  decided by the SP datum, not in advance.

Resolved in code this round: nothing above needed changing except the
diagnostic overweight (removed below) - that IS the audit's main
result alongside the fixture. The remaining unknowns name their exact
missing inputs: SP/stack-top lines (in hand already, unsent) and game
consumption (acceptance run).

## Truncated-log return (distinct result - NOT a late-install black screen)

v3.40 T0 log ends after the file-work report ("How this works"): no
placement prose, no evidence/struct blocks, no OUTCOME, no policy
block, no launch report. Locally inspected (no tester round-trip
needed): 9857 bytes, ends cleanly at a `\n` (section boundary, not a
torn write), OUTCOME count 0. An older SB4E01 log on disk DOES contain
the placement section and OUTCOME - so truncation is new with this
run's conditions, not the format. Separately, an observed return (to
menu/HBC) with "two flashes". Filed here, not under install failure,
because the combination fits several branches and the log cannot
separate them yet:
- The placement report assembles into few appends, so its absence does
  not prove execution never entered it. Dead window for a silent stop:
  PrepareFileRedirects return → Disc_SetLowMem (word writes) →
  Disc_SelectVMode (stock video calls) → Apploader_Run (header/image/
  chunk reads, Nintendo code, per-chunk RegisterDOL + note call + cache
  ops, final) → placement head (arena read, small occ vector, pure
  PlaceFst).
- v3.40-new elements in that window, audited without overclaim: the
  note call is a bounded static append (no alloc, no IO - low risk, not
  no risk); struct/section reads are guarded with messages and run
  after the apploader anyway. Allocation: occ vector (~11 entries) plus
  strings - small, and far bigger vectors succeeded minutes earlier in
  the same boot, which lowers but does not eliminate the risk (later
  failure and corruption stay possible).
- Read-error handling, verified then FIXED (branch, CI below):
  `apploader.c:83` discarded the per-chunk `WDVD_Read` return while
  header/image reads were checked. Correction to the earlier audit: a
  failed read does NOT guarantee the loop finishes - subsequent
  apploader calls consume the stale/partial bytes and may themselves
  fail (→ apploader-fail branch, truncated log, blink 6, back) or hang,
  and reads into the apploader's own region corrupt live code, not just
  the later game. So chunk failure CAN fit this truncation, and the old
  "cannot explain truncation" line is withdrawn. Fix, ordered: check
  each chunk result before registering or proceeding; on failure record
  destination, length, disc offset and return code persistently, then
  `return ret` through the EXISTING error path (BootPartition 0 →
  blink 6 → back). Convention verified: `WDVD_Read` returns 0 on
  success, negative codes otherwise (`wdvd.c`, tree-wide `ret < 0`
  use). No new paths, no new codes; a silent corrupt boot becomes a
  logged refusal.
- Return-branch map (all hypotheses, none established): apploader-fail
  → blink 6 → back, with NO placement text by design (placement never
  runs) - compatible with this log, needs 6x3 blink groups to promote.
  Install/handler refusals (1-5,7) need placement to have persisted
  (card alive then), so they need a second fault (append death) to fit -
  possible: the FAT layer just went through unmount/remount gymnastics
  in SetupDisc. Manual reset is not a branch and carries no signal. Two
  flashes alone match none of these shapes and identify nothing.
- Landed for the next run (branch, CI below): an apploader-returned
  LogStep+gprintf (log + light prove the apploader finished, return
  value separates fail from hang), the placement assembly persisting
  in three chunks (prose / evidence+struct / booking+OUTCOME) so the
  next truncation bounds itself, and the chunk-failure record above.
  Logging plus one existing-path refusal; decisions otherwise
  unchanged, timing (fopen pre-shutdown) and layout effects stated.
- Needed from the tester for THIS run (log questions answered
  locally): (1) auto-return or manual reset, with timing - the
  user-reported return to HBC is recorded as observed, branch open;
  (2) exact blink groups on video if any - 6x3 vs 2x3 vs formless
  flicker decides branches, plus light motion during the apploader
  window (frozen vs moving); (3) stock no-mod boot on this hardware,
  if not already known (control validity, no hardware-change claim).

## Relocated-FST handoff audit (model A vs model B, then code)

References, strongest first: R1 in-place installs (boot on this game -
same engine, same game, only the relocation deltas differ); R2 stock
boot (apploader contract: table at top, arenaHi below it, Lo zeroed
for the game - our `Disc_SetLowMem` matches it); R3 the official
Riivolution mechanism as publicly documented (grown tables with updated
address+size+arena on real hardware - the WORD updates are
reference-blessed; what differs here is address choice and path, which
is exactly what this audits); R4 Dolphin FST semantics at serializer
level (in-tree tests, no placement content).

Item by item, relocated vs R1/R2:
- Table pointer (`0x80000038` new value): apploader-contract word, read
  at game startup - the only consumer (no loader, IOS, or jump reader
  anywhere in tree). R3-blessed pattern. RESOLVED, no change.
- Size (`0x8000003C` = actual serialized size): table content is
  self-delimiting U8; exact max is safe under every reading of the
  field, and R3 writes grown sizes too. RESOLVED, no change.
- Arena high lowered by reserved: game heap-init bound; loader heap far
  below (BSS end `0x8106C260` + small-object demand, break logged per
  boot); game not running so nothing allocates in the taken range.
  RESOLVED, no change.
- Arena low untouched (0): matches stock (loader zeroes pre-apploader,
  game fills at startup). RESOLVED, no change.
- Staged-buffer integrity across phases: MEM2 staging, checksum at
  shutdown, pre-copy CRC re-check (codes 2/4/5 name rot vs write vs
  pointer faults separately). Mechanism present. RESOLVED, no change.
- Cache visibility (`InstallFst`: copy, flush table, write words, flush
  `0x80000030` block): data-only table needs no IC invalidate; no
  concurrent readers exist (game dead, IOS parses no FST); single
  thread, stores-then-flush before handoff - standard shape.
  RESOLVED, no change.
- Everything executed afterward: patch engines write game-code regions
  and the fixed `0x80001000` handler slot (Hooktype-gated; T0 runs
  Hooktype=0 so no handler loads); loader heap far below the
  destination; jump is stock code that boots unmodified games.
  Reviewed by region, not line-exhaustive - stated scope. No writer to
  the destination span identified. RESOLVED within that scope.
- Jump + game startup: stock / external. Decided by hardware
  (acceptance run below), not by further reading.
- Destination span below the reservation: the ONE open item. Heap is
  empty (game dead) except possibly live loader stack frames. Two
  models, one discriminator (SP/stack-top from existing T0 logs -
  zero hardware needed, just send those lines):
  model A (stack above: top near MEM1 top, SP ~`0x817FDxxx`) makes the
  cascade destination (`0x817B2DE0..0x817D772E`) clear and the OLD
  destination fatal, and convicts the EVIDENCE-PATH stall seen in the
  v3.41 log as the actual blocker;
  model B (stack below the reservation) makes ANY downward growth hit
  live frames, cascade included, while in-place stays structurally
  safe. Fixture case 2 pins the detector; the target enforces nothing
  yet - THAT fork (refuse-grown vs on-demand vs measured-clear) is
  decided by the SP datum, not in advance.

Resolved in code this round: nothing above needed changing except the
diagnostic overweight (removed below) - that IS the audit's main
result alongside the fixture. The remaining unknowns name their exact
missing inputs: SP/stack-top lines (in hand already, unsent) and game
consumption (acceptance run).

## Host integration fixture (`test_installsim`, 36 checks)

Planning through installation into simulated 24 MB MEM1, driven by the
captured SB4E01 T0 numbers (arena/fst/max/want, 8 KB block, BSS) plus
labeled synthetic extras: real `PlaceFst`, real `FstBuilder` seam case,
real `Crc32` both sides of verification, mirrored stage/bounds/copy/
words/verify/refuse sequence, canaried protected regions (reservation,
block, BSS, low image, stack zone), exact destination/bytes/words
assertions, refusal quietness, and a stack-overlap detector with the
open fork documented at its definition. Validates the algorithm
end-to-end on captured numbers; REQUIRES Wii hardware for: real
addresses and cache behavior, the real apploader layout (simulated from
log values), real thread/stack reality (SP arrives as logged input),
timing and interrupts, IOS/cIOS behavior, and game consumption. A green
fixture means the handoff is bit-exact, never that the game boots.

## 0x81201b90 audit: candidate secondary FST pointer (meaning open)

The reference scan found the original FST address as the word at
`0x81201b90` (+0x10 into a structure at `0x81201b80`). Address identity:
`0x81200000` is where this loader puts the apploader image
(`apploader.c:23`) — nothing else in our tree stores into `0x8120xxxx`
(DML/Neek defines are other boot paths). So the word is either a
disc-static constant of the apploader image or a global the apploader
wrote while running; coincidence is excluded by exact equality, and our
loader is excluded as a writer (its only store there is the image load).

Producer: {disc-static, apploader-runtime} as the leading pair, plus
loader-heap overlap while the break is unreached (our only deliberate
store there is the image load; a break past the struct puts the address
inside the managed heap range - handed to a live object or free space,
unknown which; possible, not established). Equality makes the field a candidate, nothing more:
structured coincidence (same-neighborhood constants, code words) is not
excluded by it. Consumers: none in our tree (no `0x81201b` reads
anywhere); the apploader itself is dead post-run; game startup is
unobservable from here, which is exactly why the equality alone proves
nothing. Relocation updating it is UNDECIDED: necessary iff a live
consumer reads it (most plausibly game startup following a stale
pointer, which fits in-place-booting vs relocation-dying without any
overwrite). Explicitly refused: global word replacement. The field's
meaning comes first; a single-word update would remain a controlled
experiment, never an established fix, even if the dump favors it.

Indexing verified line-by-line against the code (RiivoBoot.cpp ~947-979).
Read starts at the bases, not past them; every word is base + offset:

| What | RAM | Disc buffer (`disc`, 0x100 B) | Absolute |
|---|---|---|---|
| Read start | `ramBase` = `0x81201b40` | `discBase` = image+`0x1b40` | — |
| Struct word `i` (0-7) | `ramBase+0x40+4i` | `+0x40+4i` | image+`0x1b80+4i` |
| Target +0x10 (`i=4`) | `0x81201b90` | `+0x50` | image+`0x1b90` |
| Last byte touched | `0x81201b9f` | `+0x5f` (< `0x100`) | — |

(A matching source-comment softening, "heap over it" → "may overlap
it", rides the next code change: comment-only, and committing it now
would orphan the cleared bundle.)

Ordered run (tester): ONE T0 with artifact bundle `a733ec0d`
(`diag-bundle-a733ec0d…`, run 34273600459) - install `boot.dol` from it
(verify digest against the in-artifact manifest first; back up the SD's
current dol), DELETE `nofstinstall.txt` and `relocorig.txt` if present
(normal full install at the new placement), v7 pack, watch and ideally
VIDEO the final light sequence (groups? solid second?), send the log.
(Code-identical to v3.39; only docs differ after `a733ec0d`.)
No pointer patch on changed-bytes alone, whatever the dump says.
Live on branch: the placement report now dumps the 8 words at
`0x81201b80` (neighbors name the shape: boot words beside +0x10 read as
the apploader's working copy; code bytes read as embedded constant),
re-checks +0x10 against low memory live (no T0 hardcode), and compares
each word separately against the same apploader bytes fresh off disc
(`0x2440` header + `0x20` + file offset `0x1b80`, length re-read from the
header and checked to cover the struct). Verdicts kept apart: +0x10
vs neighbors; changed-after-load vs live-reference (a difference proves
the former only); heap overlay per the break. Read-only: no pointer
updated, no placement changed. Decision tree for the next log:
- Dump IDENTICAL to disc → static image bytes. Still readable by live
  code, so not exonerated - but updating it patches dead bytes unless
  the game reads them (consumer test = single-word update run, only if
  pursued, and still an experiment).
- Dump DIFFERENT → written after loading (apploader global or heap
  overlay, per the break line). Still not a live reference, still not
  justification to update.
Disassembly brief (owner-side, needs the backup, not the console): the
apploader image starts at disc `0x2460`, the field is at file offset
`0x1b90`. In that binary look for stores to its RAM address
(`stw` with the address materialized nearby - producer proof) and loads
from it (consumer proof, noting whether the load sits on the
pre-jump path or only reachable post-jump i.e. game code). A `lis/ori`
pair building `0x817da740` nearby is address materialization, not a
stored word - do not count it as the field.
Kept separate, as ordered: none of this proves late installation or
game entry — that is what the 3×/solid-second signals are for.

## Forwarded hypothesis: a reread landed at 0x81201b80 (tests pending)

A forwarded reading of the T0 log notes `0x81201b80` is itself a
recorded apploader yield destination (`WDVD_Read(dst…)` before
`RegisterDOL`), so "different from the image" may be a subsequent disc
read, not a runtime store. Fair as logic; premises to check against the
log (not yet in hand here):
- The yield list must actually contain a chunk at/over `0x81201b80`.
  If none does, the hypothesis has no object.
- Boot-parameter shape of the words, read here, not there: +0x00..+0x0c
  are `{doloff>>2, fstoff>>2, fstsize>>2, fstmaxsize>>2}` — exactly the
  boot.bin `0x420`-block layout our own `ReadDiscFst` parses. That is an
  apploader-parsed boot-parameter block with the computed address
  appended, OR a boot.bin-tail reread plus a separate store. A reread
  explains words 0-3 only with source ≈ disc `0x420` landing exactly
  there; it cannot explain +0x10 (no disc source holds the runtime
  address - structured-coincidence caveat standing).
Built in response (branch, below): per-yield disc offsets, so the next
log shows every chunk's source — the reread claim becomes directly
checkable, including for `0x81201b80`. Still needed from the tester:
the full T0 log (chunk lines, struct block, OUTCOME, light report).

Struct block v2 (same branch): the chunk's OWN source is now primary
(latest yield covering the struct; aligned-covering disc re-read;
per-word RAM-vs-source with +0x10 separated), image compare demoted to
context (a later read supersedes the image as the explanation).
Coincidence language downgraded everywhere: equality nominates,
nothing more. Heap line kept as possibility-only.
Batching: this rides the next combined build with whatever
network-logging work is in flight - one tester round, not one per
addition. Open question back: what/where is that network-logging work
(no implementation of it is visible from here)? And
those signals are themselves still unobserved: no run yet has reported
an unambiguous refusal or handover, so late-install completion stays
unresolved regardless of this reference work.

The evidence block names an apploader-loaded block
`[0x817d8740, 0x817da740)` — 8 KB ending exactly where the FST begins.
T0's destination `0x817da6a0` overwrites its final 160 bytes. The
"free space below the FST" assumption is invalid for this game: that
space is game image. This establishes a PLANNED overwrite, not proof
the late copy completed — the install verification still only reaches
Gecko. No blink/checksum speculation follows from it; it is a concrete
placement defect, now fixed:

- `PlaceFst` takes the loaded ranges as obstacles and cascades a grown
  table below every one it would overwrite (T0 now plans
  `0x817b2de0`, heap cost 162,144 bytes (`0x27960` - an old 162,912 figure
  in earlier notes was bad hand-hex; code always computed from addresses).
  Ranges inside the stale-table reservation are the expected overlap and
  are skipped + counted
  (`ignoredRanges`); straddlers and malformed entries are obstacles and
  skips respectively, never silent. No room anywhere → refused.
- Tracing: the DOL section table + BSS come off the disc in the same
  window as the FST, and each logged chunk now names its section and
  disc offset (or BSS / no match). Purpose of the 8 KB block resolves
  from the next log: BSS verdict vs section index, plus the dirt-scan
  bytes already logged. Offline cross-check for the backup holder: DOL
  header is 18 `(fileOffset, memAddr, size)` triples (7 text + 11 data),
  big-endian; find the section containing the chunk's disc offset.
- Regression `test_fstinstall` §9 pins the exact T0 addresses
  (defective `0x817da6a0`, fixed `0x817b2de0`, reservation/straddle/
  abut/multi/no-room/in-place/malformed cases). Full host suite green
  (77 checks in fstinstall, 0 failures everywhere); edited TU
  syntax-checked under the Makefile's PPC flags.
- Next hardware run: T0 with v3.37 (NOT v3.36 - see below), pack
  GXDiag-SB4E01-v6.zip (v5 laid the T folders flat next to gxdiag.xml so
  extracting it stranded them outside gxdiag/ - exactly the reported
  missing a.bin; v6 mirrors the drive as riivolution/gxdiag.xml +
  riivolution/gxdiag/T0/..., same 8 probes, docs point at v3.37 + the
  expected `817b2de0` address). Tester for this round is the repo owner.
  T2 stays separate.

## Light signals: attempted repeatedly, inconclusive (not untested)

v3.38's 3x/solid-second protocol has run on hardware repeatedly; reports
stay "two flashes, then black". That neither confirms nor refutes any
branch: an irregular flicker tail reads the same with or without the new
rules when the tester cannot resolve groups. No unchanged rerun will
sharpen this - the protocol needs the video record (pattern over time,
not a count from memory) plus the log, or it stays undecided. The rules
themselves are unchanged and stay in the build.

## Pending run vehicle (verified, not a rerun)

Commit `0e6c9c3d`, CI run 34279739038 green (compile, files, symbols
present+resolved, manifest, upload all success), artifact
`diag-bundle-0e6c9c3d…` (17,879,895 bytes, fresh): per-yield disc
offsets + struct source-compare + all prior evidence. New information
vs every prior round, so this run is changed by construction. Video of
the ending supports the light reading; the log carries the evidence.

## Collector terms (requirements, not a design yet)

Blocking TCP is accepted as an implementation defect, not a verdict on
the channel. IF revisited, all three are required: bounded
initialization (no open-ended DHCP on the boot path), nonblocking or
timeout-limited sends, and logging failure never blocking boot.
Feasibility checked, not implemented: CI-era public `network.h`
(v2.11.0, validated) already carries `SO_SNDTIMEO`/`SO_RCVTIMEO`,
`net_setsockopt`, `O_NONBLOCK`/`FIONBIO` via `net_fcntl`/`net_ioctl`,
`net_select`, `net_poll`, `MSG_DONTWAIT` - timeout-limited sends need
no private API. Still also required and still open: deterministic
resurrection after `AppCleanUp` (the inert-on-boot-path finding
stands), lifetime extension with the wedge analysis, and a confirmed
tester end (listener? WiFi?). Sequence: pending run first - its results
may obsolete branches before any of this is built. One combined build
per round when building resumes.

Transport: blocking TCP (`RiivoNetSock.c`, libogc net_*), no timeouts,
fail-latched dead. Findings, in dependency order:
- `AppCleanUp` (BootGame, unconditional, before `SetBootContext`) calls
  `DeinitNetwork` → lwIP torn down, flag false. Nothing on the boot
  path re-inits it (`WII_Initialize` is video/pads, not net).
- The network thread either self-suspended at menu init (stays down,
  deterministic) or still loops (DHCP-seconds race vs the boot window,
  nondeterministic). Either way `OpenCollector` faces down-or-racing
  network and latches dead. The collector is inert on the boot path
  TODAY - not "dying at shutdown" as the code comments claim (those
  comments describe intent; `RiivoBoot.cpp` overstates its lifetime).
- Nothing in `ShutDownDevices` touches net either way; no IOS reload on
  the same-slot path; `__IOS_ShutdownSubsystems` only at the jump.
  USB-medium testers are an open variable (`USB_Deinitialize` vs
  USB-Ethernet unexamined - and unreachable, see above).
Last usable point, unchanged code: NONE reliably - there is no point in
the window where the channel is deterministically alive, pre- or
post-shutdown. The 5 checkpoints (entered / CRC / copied / verified /
pre-jump) therefore cannot ride it without FIRST resurrecting it
(deterministic re-init after `AppCleanUp`: seconds of DHCP on every mod
boot) AND extending socket lifetime past shutdown (blocking-write wedge
risk moves into the install window; first stall wedges indistinguishably
from install failure). Both perturb the failing path for a channel whose
tester end (listener? WiFi?) is unconfirmed.
Recommendation (superseded 2026-09-08 - see Collector terms above):
the assessment below stands as mechanism, but "do not pursue" no
longer stands as policy. Blocking TCP is an implementation defect to
fix under the three requirements, not a reason the channel cannot work.
SD log stays the persistent baseline throughout (untouched by all
options).
Not proposed: moving install earlier (reintroduces the heap-overwrite
hazard the late install exists to avoid), keeping SD/USB mounted (game
boot requires the teardown).

## v3.36 post-mortem: tagged before the code was committed

v3.36's binary (sha256 `191670a5…`) does NOT contain the placement fix:
downloaded and string-checked - v3.37-era strings absent, `RIIVO_COMMIT`
reads `0d987d2` (the notes commit, which predates the uncommitted fix).
Same sizes + identical map as v3.35 are then expected (layout unchanged;
only the commit string and per-build bytes differ). The pipeline built
exactly what the tag pointed at; the sequencing error was tagging
before committing. Institutional fix: every release's boot.dol is now
downloaded and grepped for the new code strings + commit BEFORE
announcing. v3.36 is void for the fix (v3.35 + docs); superseded, not
deleted. Verify ancestry (`merge-base --is-ancestor`) and binary strings
before every future tag.

## v3.38 (released + binary-gated): handover signals, reference scan

Run 34270314568 green. Assets: zip + boot.dol (`ad0e7179…`, 5,133,952)
+ boot.elf (`e34c834c…`) + boot.elf.map (`bfcf94e0…`, 5,151,873 - all
shifted vs v3.37, as new code requires). Binary gate passed on the
published bits (digest matches; all new strings present; commit
`47e30da` present ×2). Pack: GXDiag-SB4E01-v7.zip (signaling docs).
T0 round: v3.38 + v7, report groups-vs-solid + screen + log.

## v3.42 (released + binary-gated): trimmed diagnostics, install fixture

Run 34289717082 green. Assets: zip + boot.dol (`4a64943a…`, 5,134,080)
+ boot.elf (`dcdecc8f…`) + boot.elf.map (`e226e89a…`, 5,152,819 - all
shifted vs v3.41, as new code requires). Binary gate passed both ways
on the published bits (digest matches; evidence header, obstacles
line, apploader step and commit `e99ca5f` present; removed disc-verdict
/ ref-scan / source-verdict strings absent). Pack v8 (v3.42 refs).
T0 ACCEPTANCE: v3.42 + v8, no bypass markers - game boot is the test.
Video the ending if possible, send the log. Real-mod run only after.

## v3.41 (released + binary-gated): chunk-failure refusal + truncation bounds

Run 34285686779 green. Assets: zip + boot.dol (`de0c9b14…`, 5,137,440)
+ boot.elf (`b95a4dc6…`) + boot.elf.map (`7e40b55d…`, 5,153,659 - all
shifted vs v3.40, as new code requires). Binary gate passed on the
published bits (digest matches; chunk-failure + apploader-returned
strings + commit `92abb8f` present). T0 round: v3.41 + v7 pack, no
bypass markers, video the ending if possible, send the log.

## v3.40 (released + binary-gated): source-compare evidence

Run 34280807103 green. Assets: zip + boot.dol (5,136,928)
+ boot.elf + boot.elf.map (5,153,216 - all
shifted vs v3.39, as new code requires). Binary gate passed on the
published bits (digest matches; source-compare strings present).
Superseded by v3.41 for the T0 round.

## v3.39 (released + binary-gated): struct evidence + handover signals

Run 34275688432 green. Assets: zip + boot.dol (`110e3af6…`, 5,135,360)
+ boot.elf (`1861d40f…`) + boot.elf.map (`f06e1aff…`, 5,152,563 - all
shifted vs v3.38, as new code requires). Binary gate passed on the
published bits (digest matches; struct-block strings + commit `7ce364c`
present). Superseded by v3.40 for the T0 round.

## Reference review: Project+ FilePatchCode.asm (analysis only, NO code taken)

Source: `SDCard/Project+/Source/Project+/FilePatchCode.asm` (REDUX v0.95,
Brawl RSBE v1.31). Read as a semantics reference for runtime replacement;
nothing in it transfers to GX — every address, struct layout and helper
below is Brawl-specific. It explains, however, why Brawl can serve SD mod
files from a USB backup with no FST relocation, and why GX cannot do the
same.

How it works, per mechanism:
- Lookup is per-request at the GAME's file API (HOOK @ $8001BF38, only
  when request type is DVD): strip `dvd:`, build `<mod>/pf/<path>`, probe
  existence with the game's own checkSD. Found → rewrite request path +
  type 3 (SD). Not found → request untouched (DVD proceeds). No tables,
  no sizes, no FST involvement. Fallback is automatic and total.
- Sizes live at the size query (HOOK @ $8001FFF8): FAFStat on the SD
  path wins (rounded to 0x20), zero falls back to the original size.
  Bigger files get real buffers via the allocation fix ($8001CD0C:
  heap from the request, else target address).
- Partial reads are first-class request attributes (Length Fix
  $8001CCB8: request length or full file; SDStreamRead takes explicit
  offset+length; custom SDLoad builds {offset, length, loadAddress,
  heap} requests). Streaming audio/video (BRSTM/THP) get their own
  wrappers, each trying SD first and calling the DVD original on
  failure — different game subsystems need different hooks.
- Environmental prerequisite: "Never unmount SD" (`blr @ $8001eb94`
  neuters the game's SD unmount). Brawl keeps SD mounted for the whole
  session, so game code can read SD at any time.

What this means for GX (review, not changes):
- Opposite environments force opposite layers. Brawl runs WITH SD
  mounted (game-managed); GX shuts everything down before the jump, so
  SD is unusable and the mod must ride the same USB drive through the
  cIOS. Path substitution at a game API is therefore not available to a
  generic loader: no per-game hooks, no per-game request layouts, no
  mounted filesystem at read time.
- Given those constraints the current architecture is the right layer:
  FST rebuild (lookup) + cIOS fragments (transport). Its necessary
  consequence is that size changes MUST be written into the table —
  hence relocation when growing, hence the placement fix above. FPC
  bypasses the FST for SD files, so it never relocates anything.
- Parallels already present: our unmapped-reads-hit-original-bytes is
  the disc-layer version of FPC's untouched-request fallback; our table
  lengths + padded reads are the size handling; sector packing +
  boundary sampling is the partial-read story.
- Labeled future-design question, NO action: FPC falls back PER FILE
  (missing → DVD, rest redirect); GX currently withholds the whole
  table on any failure. Per-file fallback (failed entries point back at
  original offsets) would mirror FPC more closely but needs planner
  support. Parked until relocation boots.

Dropped as leading assumption: refusal code 2. The same two-flash
signature appears on runs that then enter the game successfully, so the
flashes are consistent with ordinary progress flicker. A code-2 reading
would additionally require the pause-then-two-slow-flashes pattern,
which was never established. The install-verification sub-cases (bytes,
pointers, staging metadata) stay unranked.

Next run, one only: v3.34 T2 Visible. One picture byte changed in
TitleLogo.arc, same in-place geometry as T1. Title glitched: the game
reads our files — the feature proven end to end on SB4E01. Title
normal: the redirect is live but not consumed (the game never reads
through it, or reads elsewhere). Either outcome retires a branch with
zero new code. Do not order T3/T4 until T2 reports. T2 tests
consumption ONLY; it says nothing about the relocation failure.

## Read-only audit: span ownership + stale-arena check (no code changed)

Scope stays: relocated install fails, in-place boots. Destination
overwrite, pointer changes, arena change, relocation-only checks are
NOT distinguished. The withdrawn items: SP-vs-origAddr as proof (needs
real stack bounds), blink code 8 (helper clamps above 7), "heap far
below" and "stack probably above" without addresses. What follows has
addresses or names the exact gap.

Loader layout, EXACT, from the tested binary itself (`boot.dol`,
v3.34 release asset, sha256 `83edd306...07ab` verified on download):
- text `[0x80B00000, 0x80DE6BC0)`, data `[0x80DE6BC0, 0x80FE3560)`,
  BSS `[0x80FE3560, 0x8106C260)`, entry `0x80B00000`. (Base `0x80B00000`
  comes from the tag Makefile `LDFLAGS --section-start`; the rvl.ld
  default `0x80003F00` does NOT apply — an earlier audit line saying so
  was wrong.)
- Heap floor (`__Arena1Lo`) = `0x8106C260`. The span bottom sits
  7,791,680 bytes above it. That is the floor ONLY — it says nothing
  about extent; occupancy needs the break (`sbrk(0)`), which is
  runtime-only. An earlier "heap far below" claim built on the floor
  alone is withdrawn.
- Build env, for the record: CI image `devkitpro/devkitppc:20250527`,
  `make release -j2` (tag workflow). A local rebuild was attempted and
  ABANDONED: local devkitPPC 16.1.0 + 2026 libogc no longer compile this
  tree (bundled portlibs `sys/socket.h` vs new libogcakh `sockaddr_storage`,
  `socket`/`connect` now real functions colliding with the tree's own
  declarations, ~10 TUs). Any local binary would NOT match the tested
  one; no local binary was kept. (Resolved since: CI attaches
  `boot.elf` already, and `boot.elf.map` rides along from the diag
  branch work.)
- Stack bounds are in NO file here: not the repo (no DOL linker
  script), not the DOL (no stack info), not the toolchain scripts. HBC
  owns SP init. Runtime-only (see capture spec).

Game-side status: the apploader HAS populated game memory before the
install (DOL sections at link addresses + FST reservation
`[0x817da740, 0x81800000)` + boot info), so "nothing else can be there
because the game isn't running" is WITHDRAWN. SB4E01 DOL section ranges
are disc data, in no file here — excludable only from actual ranges
(see capture spec), never "by construction". What static work DOES
exclude: everything loader-owned except heap-extent and stack
(code/data/BSS end `0x8106C260`, ~7.4 MB below the span); exit-path
markers (`sys.cpp:214`, `StartUpProcess.cpp:300`,
`PromptWindows.cpp:1018` — menu exits only, and inside the FST
reservation anyway); `AlternateDolParameter` (`WDMMenu.cpp:37-38`,
default 0, passed by value); XFBs (dead post-shutdown by standing
decision).

Stale-arena check — NO stale write on the Wii-disc path:
- Planning captures size only (`RiivoBoot.cpp:1640` plannedFstSize);
  no arena word is read pre-apploader.
- Sole `PlaceFst` call is `ReportFstPlacement:2536`, post-apploader, on
  fresh raw words; sole `pendingPlace` producer is `:2601`.
- Sole low-memory writers: `InstallFst:156-158`, the apploader itself,
  `channels.cpp` (channel-boot path, not this one), `Disc_SetLowMem`
  (`Arena_L = 0`, pre-apploader — that is why blindLo is expected).
  Nothing between placement and install touches `0x80000034`.
- In-place writes back identical ptr/arena and updates max only.
- The `0x817da740` vs `0x817feff0` log pair is two sources, both
  correct: raw game word (placement report) vs libogc startup cache
  `SYS_GetArenaHi` (`ReportLaunch:2677`, never touched by the
  apploader). Expected pair, not a bug; the install never consumes
  `SYS_GetArena*` (only MEM2 routing and that log line do).

## Capture: relocation evidence (on branch `diag/reloc-evidence`, NOT published)

Code: `AppendRelocationEvidence` in `ReportFstPlacement` (card log,
persistent) + pre-copy / post-verify SP+break gprintf pair in
`InstallPendingFst` (Gecko only — post-shutdown). Pure helper
`RangesOverlap` in `RiivoFstInstall.hpp`, host-tested
(`test_fstinstall` §8, 11 cases). `.github` change in the same diff:
`boot.elf.map` joins `boot.elf` in the debug artifact (both workflows) and
in release assets.

Corrections applied after review, all in code+comments, not just here:
- Every SP/break value is a SNAPSHOT of its instant. Calls between two
  samples can use deeper frames and return unseen; nothing bounds them.
  Placement SP is context on a deeper chain (expected below
  install-time, not guaranteed). Past the last sample only the return
  path, light-out and jump sequence run — shallow, unsampled.
- An empty DOL list reads UNKNOWN, never "no overlap". Null/empty,
  wrapped, or out-of-MEM1 entries are logged INVALID individually and
  force UNKNOWN — no exclusion off bad data.
- An invalid break (`(void*)-1`, outside MEM1) reads UNKNOWN. The break
  alone never establishes the full heap interval (floor cited
  `0x8106c260`, not verified).
- Wording everywhere is "installation decisions unchanged", never
  "byte-identical": the block allocates, writes the log, and shifts
  timing, stack use and binary layout.

Validated: full host suite green (test_fstinstall 57 checks, 0 fail);
edited TU syntax-checked with the Makefile's own PPC flags (local gcc
16.1 — syntax/types only, NOT the CI toolchain). CI build/link
validation: branch `diag/reloc-evidence` commit `16c02b34`, run
34211117708 — `make release -j2` on `devkitppc:20250527` COMPLETED
SUCCESS. Weak stack symbols + all APIs link under the exact toolchain.
No tag, no release from this.
Preservation: run 34213381735 (commit `3d19e641`) COMPLETED SUCCESS with
all diagnostic steps green — files present, both stack symbols present
AND resolved in the ELF, manifest written, bundle uploaded as artifact
`diag-bundle-3d19e641…` (17,849,856 bytes, not expired). Contents per
the run: boot.dol + boot.elf + boot.elf.map + diag-MANIFEST.txt
(commit + SHA-256) + diag-symbols.txt. Owner path: download the artifact
from the run page, `sha256sum -c` against the manifest (skip its
`commit` first line). Byte-level download verification is not possible
from here (artifact downloads need an owner token); everything
observable without one is verified. No tag, no release. This exact
bundle is the cleared build for the next T0 hardware run.

Pre-release v3.35-riivo-evidence (run 34214692174, tag moved once after
a Zip-step failure on the artifact's nested map path — fixed):
published, pre-release flag set. Assets: zip + boot.dol
(sha256 `191670a5…cf0f9a042`) + boot.elf (`52f54b40…07045d`) +
boot.elf.map (`bf1432ca…415cb9d058d`, 5,150,680 bytes — first release
with the map). Next hardware run: T0 with v3.35, send the log — the
Relocation evidence section is the deliverable. Multi-path artifacts
keep directory structure (`build/boot.elf.map` nested, not flat) —
accounted for in both workflows.

Round-2 corrections (branch `diag/reloc-evidence`, commit `1207ce91`,
CI run 34218344255 ALL GREEN including files/symbols/manifest/upload):
- No silent discards: obstacle list is exact-size (no cap), every
  dolList entry handed over raw; malformed entries refuse GROWN
  placement (`malformedRanges`, existing WITHHELD path) while in-place
  ignores the list. Regression: refusal, in-place+malformed ok,
  151-entry capacity, all exact-address.
- Section tracing fixed: disc offsets are absolute (partition base +
  fileOff + displacement, `%010llx`); slots keep DOL section numbers
  (empty kept, `index` field) so text/data numbering can't compact.
- BSS is a placement obstacle when the header names a valid one
  (invalid non-empty BSS counts malformed and refuses grown); status
  printed in the evidence block.
- Map saga, honestly: `boot.elf.map` passed the probe on one run and
  failed it on the next with identical paths — directory not stable
  across runs (or transient). Pipeline now searches (`find`, maxdepth 2)
  and the manifest records whichever path was hashed. Do not re-assert
  a fixed directory without new evidence.
- Preserved: artifact `diag-bundle-1207ce91…` (17,849,856 bytes, fresh):
  boot.dol + boot.elf + map + diag-MANIFEST.txt + diag-symbols.txt.
  That exact boot.dol is CLEARED for the next T0 run (verify against
  the in-artifact manifest after download; byte-level check from here
  still needs an owner token). No tag, no release from this branch.

Stack bounds, validated against libogc v2.11.0 (May 25 2025 — the CI
image `devkitppc:20250527` vintage): main-thread stack is
`[__stack_end, __stack_addr)`, the exact symbols lwp.c passes to
`__lwp_thread_init` for `_thr_main`. The TCB struct itself is private
(no public header carries it — checked installed `ogc/*.h`), so the
code reads the two symbols directly (weak: a future toolchain without
them logs "unknown"). Runtime cross-check in the block: both in MEM1,
low < high, SP inside — else bounds unused.

sbrk(0), documented from sbrk.c (identical v2.11.0/master): with the
loader's `MALLOC_MEM2 = 0` it is the newlib/MEM1 break,
`[startup-Lo, current-Lo)`. Excludes MEM2 pool, apploader reservations,
main stack. Non-main LWP stacks are workspace-backed (lwp_stack.c) and
the workspace is carved from the sbrk region at init (lwp_wkspace.c) —
break bounds them from above. Startup-Lo source not re-verified: floor
cited (`0x8106c260` on the tested binary), not relied on.

The capture decides nothing by itself and gates nothing: a pre-install
snapshot cannot exclude later corruption or relocation-only failures.
It retires (or confirms) the ownership suspects only. T2 stays a
separate consumption question and gates nothing here.

## The one thing that blocks everything

**The tester's USB setup is the prime suspect, but its guilt is not established.
Do not clear the loader and do not blame the drive solely on the evidence below.**
Two independent signs, both from v3.30:

1. A boot log came back as **26 exact copies of one 512-byte sector** (13466 bytes
   total, high entropy, not our text). The file *length* was right, so the directory
   entry updated correctly and the data did not. This confirms the *file* is
   corrupt, not where the corruption happened: failing flash, a bad enclosure or
   cable, the FAT driver misreading, or the cIOS miswriting all fit. No local
   copy of the damaged file exists to re-examine; preserve it off the drive
   before any repair is attempted.
2. A later boot of the **same test on the same build** produced no "Boot progress"
   section at all. That is consistent with dying in `get_frag_list()` — stock USB
   Loader GX code that reads the *game backup's* cluster chain out of the
   FAT, before any Riivolution code runs — but the early return was never observed
   with its return value, so a hang or crash in that window fits too. An
   unobserved return identifies nothing on its own.

Same build, same test, two completely different failure points points at the
filesystem/drive layer rather than a single code bug — but the origin is not
established by the light or the logs currently in hand.

**Do first:** `chkdsk X: /f` on that drive. Better: a different drive with a fresh copy
of the game and the test pack. Then re-baseline from scratch — no mod selected at all,
then `Test = Disabled`, then T0 — before running anything else.

A free check: with the drive in this state the game should fail **with no mod selected
at all**, because `get_frag_list` is on that path too. If plain USB booting of SB4E01
is broken right now, the drive is confirmed.

## Confirmed on hardware

- **File replacement works.** Mario Party 9, one character model replaced, no code
  patches. The game read and used the replaced file. This is the only end-to-end
  success, and it is real.
- **`ShowPreJumpSummary` was killing boots.** `Test = Disabled` (which applies
  *nothing*) black-screened on v3.22 and boots on v3.27. The only difference is that
  the summary no longer runs.
- **The loading bar was killing boots.** T0 died inside fragment mapping on v3.27 and
  runs to completion on v3.29.
- **The full pipeline can run on SB4E01.** v3.30 Boot 1: hook at `93800bd0`, LOW_READ
  read-back 2/2, table built, installed *and verified*, drive light out, jumped to the
  game. Then black screen. Whether that black screen is real or drive-induced is
  **unknown**.

## Not confirmed — do not repeat these as fact

- **Whether SB4E01 can boot with a mod live.** The one clean run of the whole pipeline
  ended in a black screen on a setup whose drive layer is suspect (see top).
- **`nofstinstall.txt` has never actually run.** Boot 2 produced no "Boot progress"
  section at all, which is consistent with `get_frag_list` failing before Riivolution
  code runs — but the return value was never observed, so that identification is
  provisional. That comparison — table/install fault vs hook/fragment fault — is still
  the right next test and is still unrun.
- **Part 2 (real file replacement on SB4E01) has never been run.** `matched on disc: 0`
  in every log to date. It is still the only thing that proves the feature on this game.
- **The T5 `<memory>` patch.** Blamed for a black screen, then exonerated when
  `Disabled` black-screened too. Its `original=` guard only proves the byte at that
  address is the byte expected — it never proved nopping `stb r0,0x68(r4)` is safe, and
  `98040068` is an ordinary encoding, so a 4-byte match is weak revision evidence.
  Status: untested.
- **The in-place vs relocation fault line — confirmed, mechanism open.**
  relocorig (verbatim bytes, moved 160B) died; T1 (rebuilt bytes, in
  place) boots. The move itself kills T0, independent of table content.
  Open: WHAT the span `[0x817da6a0, 0x817da740)` holds. Evidence
  capture is IMPLEMENTED (unpublished — see Capture section): per-chunk
  DOL ranges, validated stack bounds, SP at three points, sbrk break,
  all observation-only, no gate, no blink change.

## Standing decision: nothing draws on the boot path

Settled in v3.31 at the user's instruction ("too many variables to go wrong").
**Deleted, not gated:** `ProgressGuard`, `FragProgress`, `ShowPreJumpSummary`,
`verboseListing`, `showSummary`/`showExtended`, the `loadingbar.txt` and `showlog.txt`
markers, and `ProgressSkipDebounce`. Do not reintroduce any of them, and do not
"restore" a loading bar as a convenience — two of these each cost a week of tester
rounds.

`LogListProgress` survives because it is a log line, not a draw; `LogStep` flips the
drive light, so it is a progress signal in its own right. It runs unconditionally.

Note the trap that hid this for weeks: `ProgressWindow` opens with a 500 ms "is this
worth drawing?" wait, and every phase on this path is shorter than that — so the
window returned *without drawing*. That is why testers kept reporting the loading
screen never appeared, and it was the only reason the boot survived. v3.23's
`ProgressSkipDebounce` removed that accidental protection and made the draw real.

## Reading the drive light — the only channel that has never lied

- **Fast, irregular blinking** = alive and working. `LogStep` flips it, plus a pulse
  through the 13.6M-word IOS scan and one per apploader section.
- **Light out** = `EndLightPulse()` ran, which is the last statement before the jump
  call. It proves the loader reached the jump — not that the game executed a single
  instruction. A broken jump sequence, or a game crashing before its own video init,
  follows the same dark light.
- **Light on and static** = hung before the jump.
- **Slow deliberate blinks** = a refusal code. `RiivoBlinkCode`: 700 ms pause, then
  *n* blinks at 350 ms on / 350 ms off, n ∈ 1..7. Countable by design.
  - 1 nothing staged · 2 staged checksum · 3 install bounds · 4 installed bytes ·
    5 low-memory pointers · 6 BootPartition returned a null entry point ·
    7 code-handler collision (Hooktype nonzero AND a protected mod range overlapping
    0x80001000..0x80003000; the unguarded skip-flag query blinks nothing)
- **Record the light on every run, whatever the screen does.** A refusal can
  flash its code and then hang inside the return attempt, so blinks followed
  by a black screen do not rule a refusal out. Ask for a short video covering
  the final flashes and the ensuing black screen, not just a count.
- **Back at the loader with no blinks** = `SetupDisc()` returned < 0
  (`GameBooter.cpp:823`). No blink, no log line. `get_frag_list` failing lands here.

`Sys_BackToLoader()` calls `exit(0)` — it lands at the **Homebrew Channel**, not the
GX menu. Worth asking testers which they see; they mean different things.

## After device shutdown, only two channels exist

The card is gone, so the log cannot be written. Only the drive light (`wiilight_diag`)
and gprintf to USB Gecko survive. This is why the blink codes exist and why losing a
blink count loses the reason for a refusal permanently.

`v3.26`/`v3.28` added a checkpoint just *before* shutdown: the log now says whether
the staged table's checksum still matched, or that no table was staged. "Intact at
shutdown" plus a code-2 blink means the corruption happened after the card went away.

## Diagnostics available

| marker (next to the XML, on the mod's drive) | effect |
|---|---|
| `riivolution/nofstinstall.txt` | stage everything, then **don't** install the table. Splits table/install faults from hook/fragment faults. |
| `riivolution/relocorig.txt` | stage the **verbatim original** table (+160 zero bytes) instead of the rebuilt one, at the same relocated address. Splits relocation faults from new-table-content faults. Never combine with `nofstinstall.txt`. |
| `riivolution/verify.txt` | full large-read verification of the mod through the cIOS |
| `riivolution/dumpios.txt` | IOS dumps |
| `riivolution/ondemand.txt` | on-demand path (never run on hardware) |

## Releases this session

`v3.24` prompt text overflow · `v3.25` light pulses through the apploader ·
`v3.26` staged-table checkpoint · `v3.27` summary opt-in · `v3.28` checkpoint 3-state ·
`v3.29` loading bar off · `v3.30` `nofstinstall.txt` · `v3.31` all GUI removed ·
`v3.32` white flash removed from the jump · `v3.33` `relocorig.txt`
(`d1712fb5`, current).

Test pack (GXDiag-SB4E01): https://files.catbox.moe/8ohcmi.zip — Part 1 order is
`P1 T0 Creates`, `P1 T7 Folder`, `P1 T5 Memory`, `P1 T6 Combined`. P1/P2 prefixes lead
because the settings menu draws a choice name in a **100 px column at font size 20 with
DOTTED truncation** — about ten characters, so a suffix never reaches the screen.

## Build notes

GUI files **do** compile locally, contrary to an older note. `gui_imagedata.h` includes
`<gd.h>`, absent from portlibs, but nothing in that header uses a gd type — a 3-line
empty `gd.h` stub plus `-I/c/devkitPro/portlibs/switch/include/freetype2` (for
`ft2build.h`) compiles every GUI translation unit:

```
powerpc-eabi-g++ -fsyntax-only -Wall -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float \
  -Isource -I<stub-dir> -I/c/devkitPro/portlibs/switch/include/freetype2 \
  -I/c/devkitPro/libogc/include <file>
```

libogc is `/c/devkitPro/libogc/include`, **not** `libogc2`. Host tests:
`/c/devkitPro/msys2/usr/bin/bash -lc 'sh hosttests/run.sh'`.

Two editing traps that bit repeatedly: Bash-tool heredocs mangle `\n` inside string
literals (use the Edit tool, or verify with `cat -A` first), and
`source/prompts/ProgressWindow.cpp` is **CRLF**, so multi-line pattern matches against
`\n`-only strings silently fail.

## Method note

The single most expensive mistake this session was treating a clean log as a pass.
Four Part 1 tests were called passes on log evidence alone; at least one was a black
screen. **A log only says the loader did not record an error. Always ask what the
screen did, and what the drive light did.** The control boot — the do-nothing
configuration — should come before any theory; it would have found the
`ShowPreJumpSummary` bug several rounds earlier.

## Session protocol

Two agents share this worktree and have collided silently before. Whoever
starts work writes `usbloadergx/.agent-lock` with agent, UTC time and task,
and deletes it when done. A lock older than a day is stale — its owner is
gone, remove it and carry on. Never commit the lock file.
