# fstadapter: what it bypasses, what it proves

`fstadapter` runs the REAL `Riivo::PlaceFst` / `Riivo::InstallFst`
(`usbloadergx/source/riivo/RiivoFstInstall.cpp`, compiled unmodified) under
Dolphin, fed with constants captured from real SB4E01 T0 card logs.

## Bypassed cIOS / hardware behavior (NOT exercised here)

- All disc I/O: `WDVD_Read`, `DIOpen`, partition handling. There is no disc;
  the boot words, DOL ranges, BSS range and rebuilt size are constants.
- The apploader: no DOL chunks are loaded, so `0x81200000` holds no image
  and the chunk-yield notes do not exist. The 8 KB block
  `[0x817D8740, 0x817DA740)` is taken as a captured range, not re-derived.
- IOS heap and loader thread stacks: the adapter runs on a plain libogc
  thread. Stack-position evidence (the model-A/model-B question) CANNOT
  come from this harness.
- Drive timing, USB retries, and everything the game does after the jump:
  no game boots here.

## What it validates (real PPC code, real emulated MEM1)

- `PlaceFst` arithmetic against the captured layout reaches the known
  answer: grown placement at `0x817B2DE0`, arena high follows, 162912
  bytes reserved, one stale-table range skipped, zero malformed.
- `InstallFst` performs the real `memcpy` + `DCFlushRange` + boot-word
  repoint (`0x80000038/3C/34`) without a data-storage exception, and the
  bytes read back identical.
- Under GDB: an exec breakpoint on `InstallFst` plus a write watchpoint
  on `[0x817B2DE0, 0x817B2DE0+153934)` must fire exactly on the install
  span and nowhere else.

## Still reserved for Wii hardware

- Whether the real apploader layout matches the captured constants.
- Whether the live loader stack overlaps the destination (model A vs B).
- Whether the game reads the relocated table back correctly.
- Whether cIOS reads deliver the bytes the plan assumed.

## Inputs ledger

- CAPTURED (SB4E01 T0 card logs, v3.36+): arena `{0, 0x817DA740,
  0x817DA740, 153792}`, want `153934`, block `[0x817D8740, 0x817DA740)`,
  BSS `[0x80728680, 0x807E3188)`, expected dest `0x817B2DE0`.
- SYNTHETIC (labeled in code): one stale-table sample `[0x817DA800,
  0x817DA900)` inside the captured reservation, exercising the skip
  counter; the staged table is a marker pattern, not a real FST.
