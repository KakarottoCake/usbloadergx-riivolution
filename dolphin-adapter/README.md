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

## Run

Boot `fstadapter.dol` in Dolphin (`-b` batch works; Null video is fine —
the verdict is also checkable over GDB). With the GDB stub on port 9090:

```
halt                                   # entry 0x80003F00
watch 0x817B2DE0 153934 write          # destination span
cont 60                                # stops once, inside memcpy
unwatch 0x817B2DE0 153934 write
cont 30                                # RUNNING = main() reached its wait loop
halt
mem 0x80000038 16                      # expect 817B2DE0 / 817B2DE0 / 0002594E
dump 0x817B2DE0 153934 dest.bin        # all bytes must match the marker pattern
```

The marker pattern is `0xA5 ^ (i&FF) ^ ((i>>8)&FF)` with sentinels
`F574AB1E size` at offset 0 and `dest E17FAB1E` at the tail.

## GDB-stub quirks found (Dolphin 5.0-18995, single-session stub)

- One TCP session per boot; `detach` is unsupported — close and reconnect
  needs a reboot.
- Send no ACKs: a stray `+` has wedged the following Z-packet.
- `OK` is not an `O` console packet; the stub sends `$#00` after a break
  reply; `E`-led short replies are errors but a data payload may itself
  start with byte `0xE5` — length, not prefix, distinguishes them.
- After the run the CPU parks in `KThreadIdleMain` with SP reading 0;
  that is the halted idle thread, not a crash.
