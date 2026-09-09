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

## Run (v2: late-install caller, both modes)

Boot `fstadapter.dol` in Dolphin (`-b` batch works; Null video is fine —
the verdict is checkable over GDB). With the GDB stub on port 9090:

```
halt                                   # entry 0x80003F00
watch 0x817B2DE0 153934 write          # relocated destination span
go                                     # async continue; sleeps are client-side
sleep 12
mem 0x80000030 16                      # boot words mid-run
unwatch 0x817B2DE0 153934 write        # before the run proceeds
go
sleep 25                               # both modes complete, parked in wait loop
mem 0x80000030 16                      # expect in-place final words (ran last)
dump 0x817B2DE0 153934 destR.bin       # relocated span, byte-exact
dump 0x817DA740 153792 destI.bin       # in-place span, byte-exact
```

A watchpoint hit stops the CPU (the stop packet surfaces on the next
command); the hit PC inside `memcpy` with LR inside `InstallFst`
identifies the install write. `go`+`regs`-polling is the reliable way to
sample a running target — `\x03`-halting a running CPU is flaky on this
stub, and `OK`/`O`, `$#00`, and `E`-prefix replies need the handling
noted below. Each staged table is a synthetic valid mini-FST (real U8
entries + strings) with a marker pad
(`0xA5 ^ (i&FF) ^ ((i>>8)&FF)` from the header end).

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
