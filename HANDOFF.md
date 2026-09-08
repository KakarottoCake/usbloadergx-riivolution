# Handoff — 2026-09-08 (updated: v3.39 released + binary-gated; T0 run live)

State of the SB4E01 (Super Mario Galaxy 2) debugging effort. Read the
"Latest evidence" section first — it supersedes the drive-blocker framing
below, which is kept for the steps it still requires.

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

## Collision demoted: fixed defect, not the cause

T0 with obstacle-aware placement (table below the 8 KB block, verified
in the log) STILL black-screens. The overwrite was real and is still
guarded against, but it is not the established cause — stopped being
treated as one. Relocated-vs-in-place differences that remain suspects:
pointer/arena value changes, heap given up, relocation-only checks.
Ruled out as consumers of the changed words: the IOS hook (reads no
boot words), the jump sequence (reads none, wipes only loader BSS),
everything between placement and install (no writer to `0x80000030-3C`
outside apploader/InstallFst). Remaining consumer: game startup itself.
The 8 KB block reports "no section match" (table WAS read, so this is
absence, not a failed read) — purpose still unknown, and avoidance
cannot exclude references it may hold to the original table.

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
game entry — that is what the 3×/solid-second signals are for. And
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
  `0x817b2de0`, heap cost 162,912 bytes). Ranges inside the stale-table
  reservation are the expected overlap and are skipped + counted
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
  expected `817b2de0` address). Tester for this round is the repo owner
  (new console/drive vs all prior rounds - do not mix results across
  testers without saying so). T2 stays separate.

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

## v3.39 (released + binary-gated): struct evidence + handover signals

Run 34275688432 green. Assets: zip + boot.dol (`110e3af6…`, 5,135,360)
+ boot.elf (`1861d40f…`) + boot.elf.map (`f06e1aff…`, 5,152,563 - all
shifted vs v3.38, as new code requires). Binary gate passed on the
published bits (digest matches; struct-block strings + commit `7ce364c`
present). T0 round per above uses the code-identical artifact bundle.

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
