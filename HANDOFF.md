# Handoff — 2026-09-08 (updated: capture corrected + CI validation running, pre-T2)

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
not break the boot. Relocation stands convicted by the relocorig run
(verbatim bytes died on the move); table content stands unindicted
(a rebuilt serializer table boots when it stays put). What T1 does NOT
prove: that the game consumed our bytes — the copy is identical, so a
boot that ignored the redirect looks the same. That is T2's job.

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
  one; no local binary was kept. Original-env `boot.map` was never
  published — worth asking the release pipeline to attach it.
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
`boot.map` joins `boot.elf` in the debug artifact (both workflows) and
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
Preservation gap: main.yml upload steps SKIPPED (`ALLOW_UPLOADS` not
true for branch builds), so this run kept no DOL/ELF/map. The boot.map
plumbing in the diff is workflow-valid (run parsed and executed) but
unproven end-to-end until a build with uploads enabled. Exact rebuild
stays available on demand: commit `16c02b34` + image
`devkitpro/devkitppc:20250527`. Next tag build (release.yml, ungated)
attaches DOL+ELF+MAP automatically.

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
