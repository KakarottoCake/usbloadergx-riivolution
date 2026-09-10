# fstadapter — Dolphin-side known-answer harness

Runs the REAL `Riivo::PlaceFst` / `Riivo::InstallFst`
(`../source/riivo/RiivoFstInstall.cpp`, compiled unmodified) under Dolphin,
fed with constants captured from SB4E01 T0 card logs. Expected answer:
grown placement at `0x817B2DE0`, 153934 bytes, arena high follows the
table down. See `BYPASSES.md` for what this bypasses and what it proves.

This directory is developer tooling. It is not referenced by the loader
Makefile, not linked into `boot.dol`, and ships in no release zip.

## Build

Needs devkitPPC + libogc (`DEVKITPPC` set), then `make` here. Produces
`fstadapter.dol`.

GNU make cannot handle spaces in the checkout path (the recursive
`make -C build -f .../Makefile` splits on them). On a checkout whose path
contains spaces, stage to a space-free directory instead: copy `source/`,
`shim/` and this Makefile, drop byte-identical copies of
`../source/riivo/RiivoFstInstall.cpp/.hpp` next to `main.cpp` (verify with
hashes), and change the `RIIVOSRC`/`ADAPTSHIM` lines to point at the
staged copies.

## Run (v4: production install + probe jump, three consumptions)

Boot `fstadapter.dol` in Dolphin (`-b` batch works; Null video is fine).
The harness runs UNCHANGED (modeled base table + probe jump) on its own,
then waits per poked mode. It publishes `adPhase` (1=unchanged done,
2=mode wait, 4=finished) and per-mode `adModeDone`; GDB polls with
`waitmem`, pokes, releases with `adGo=1`, and acks completion with
`adGo=2` - no sleeps-as-sync anywhere (a sleep-synced round once ran an
install in the wrong labeled slot). Static addresses come from
`powerpc-eabi-nm` (re-check after any rebuild; BSS moves). The mailbox
at `0x80000100` carries the poke values plus the place-blob length at
`+144` (the struct size is toolchain-dependent - assuming it once
poisoned neighboring staging words).

The probe returns the entry count in r3 and fills a result block at
`0x80000200` (magic, count, CRC, arena, nonce) for GDB. The block sits
clear of the mailbox (ends `0x80000194`); the per-jump nonce tells a
fresh block from a stale re-read.

Read back every poked word from RAM before trusting a result.
`placeOk` reads stale-1 after success (unflushed store vs
cache-bypassing GDB reads) - the flushed words, result blocks, and
dumped bytes are the verdict.

## Run (v6: memwindow under the production allocator + prior flows)

`make` links `memory/mem2.cpp` + `mem2alloc.cpp` with the loader's own
`-wrap` allocator flags and calls `MEM2_init(48)`, so every malloc in
the binary (including libstdc++'s) routes through the production
two-heap chain. `RunMemWindow()` (source/memwindow.cpp) executes first,
automatically: T0-scale regression (slot 0, must be in-place +
consumed), Spectral-scale mirror-window (slots 1-2, clean + history),
and consumption-ladder runs (slots 3+, hog-to-level with retained
blocks). Each slot reports plain/compacted sizes, path count, first
throwing op (0 = none), install/consume flags and MEM1/MEM2 figures to
the mailbox at `0x80000300` (8 words/slot: plain, compact, paths,
failOp|crc, inPlace|consumed, mem1delta, mem2pre, hogKept; word 0 is
the phase, 99 = done). GDB polls phase 99, then dumps slots. The v4/v5
poked production-installer flows follow unchanged and re-verify under
the new routing.

v6 results (18995, Null video): all slots failOp=0 down to ~229 KB
MEM2 free - no throwing op found; T0 regression green. The 64 KB hog
loop retained only ~3 MB/slot although MEM2 showed room (adapter MEM2
sizing vs Wii loader layout unestablished - harness-fidelity limit,
not a production signal). Mirror gaps: synthetic tree is flat, not
12-deep; modOffsets/LayoutFrom path not mirrored; tail
(CollectPlaced/FindSkips/PlanFragRegion/DescribeProbe) unmirrored.

## GDB-stub quirks found (Dolphin 5.0-18995, single-session stub)

- One TCP session per boot; `detach` is unsupported — close and reconnect
  needs a reboot. The CPU stays paused at entry until the first continue.
- Send no ACKs: a stray `+` has wedged the following Z-packet.
- `OK` is not an `O` console packet; the stub sends `$#00` after a break
  reply; a data payload may itself start with byte `0xE5`, so reply
  length — not the `E` prefix — distinguishes data from errors.
- Prefer `go` + `regs`/`mem` polling over halt/cont cycles: halting a
  running CPU is unreliable, while reads answer fine on a live target.
  Watchpoint/breakpoint stops arrive as async packets and are reliable.
- After the run the CPU parks in `KThreadIdleMain` with SP reading 0;
  that is the halted idle thread, not a crash.
