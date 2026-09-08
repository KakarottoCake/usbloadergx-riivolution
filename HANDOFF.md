# Handoff — 2026-09-08 (updated: v3.33 paired runs)

State of the SB4E01 (Super Mario Galaxy 2) debugging effort. Read the
"Latest evidence" section first — it supersedes the drive-blocker framing
below, which is kept for the steps it still requires.

## Latest evidence: paired T0 runs on v3.33, same build `07dfade1`

| Run | Table staged | Result |
|---|---|---|
| Normal T0 | Rebuilt, 153,934 bytes at `0x817da6a0` | Black screen |
| T0 + `relocorig.txt` | Verbatim original + 160 zero bytes, 153,952 bytes, same address | Black screen |
| Earlier T0 + `nofstinstall.txt` | Install skipped | Game boots |

Both installs target `0x817da6a0`, 160 bytes below the original table.
Both pass file read checks and report an intact staging checksum before
shutdown. Neither persistent log proves the late install completed.

Reading: the hook/fragment half is exonerated (again — `nofstinstall`
boots with hook and fragments live). Original entries do not rescue the
boot, so the serializer is deprioritized; the shared staging/install
path is now the suspect, not table content.

Open, provisional: the tester reported two flashes then darkness on the
`relocorig` run, possibly refusal code 2 (staged checksum mismatch at
the late copy). Ordinary progress flicker looks similar, so the count
alone confirms nothing — the pattern must be pause-then-two-slow-flashes
(~350 ms on/off). If code 2 is confirmed, the staging buffer changed
between the pre-shutdown checkpoint and the late copy, the destination
copy never happened, and relocation is unimplicated. If ruled out,
destination ownership and the installed pointer/size/arena values stay
open. A post-shutdown refusal returning visibly is itself unproven: the
return runs after device teardown and may die silently, which would also
present as black screen.

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
- **The in-place vs relocation theory.** `PlaceFst` takes the in-place path when
  `fstSize <= fstMaxSize` and relocates otherwise. Replacing a file never grows the
  table; adding one does. So MP9 (replace only) took the safe path and T0/T7 (add
  files) relocate. It fits the evidence but was built on a T7 refusal that happened on
  a build which could not boot the game at all. Plausible, unproven.

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
  *n* blinks at 350 ms on / 350 ms off, n ∈ 1..7. Countable by design. "Too fast to
  count" is never a refusal code.
  - 1 nothing staged · 2 staged checksum · 3 install bounds · 4 installed bytes ·
    5 low-memory pointers · 6 BootPartition returned a null entry point ·
    7 code-handler collision (Hooktype nonzero AND a protected mod range overlapping
    0x80001000..0x80003000; the unguarded skip-flag query blinks nothing)
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
