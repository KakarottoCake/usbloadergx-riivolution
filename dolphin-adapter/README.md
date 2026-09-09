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

## Run (v3: PRODUCTION InstallPendingFst, both modes)

Boot `fstadapter.dol` in Dolphin (`-b` batch works; Null video is fine).
The harness prints a GDB mailbox at `0x80000100`
(`stageR crcR stageI crcI sizeR sizeI blobR blobI blobLen@+144`) -
read it, poke the file-static staging words (addresses from
`powerpc-eabi-nm`: `pendingFst pendingPlace pendingFstCrc
pendingFstSize pendingPlaceOk fileWorkLive skipFstInstall
installFailCode`), set `adGo=1`:

```
halt                                   # entry, stopped: arming is safe here
watch 0x817B2DE0 153934 write          # relocated span (optional)
go                                     # THE run-control for this boot
sleep 10                               # harness reaches the mode wait
doprod 0 <8 static addrs>              # poke RELOC staging from mailbox
wmem adGo 00000001
sleep 15                               # production call runs; watch may stop it
mem 0x80000030 16                      # boot words
mem installFailCode 4                  # expect 0
unwatch ...                            # only while stopped or before go
go                                     # valid ONLY while stopped
...
```

Read back every poked word from RAM before trusting a result, and
re-check `nm` after any rebuild (BSS moves). `placeOk` reads stale-1
after success (unflushed store vs cache-bypassing GDB reads) - the
flushed words and dumped bytes are the verdict, confirmed byte-exact
for both modes (153934 + 153792 bytes, 0 mismatches).

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
