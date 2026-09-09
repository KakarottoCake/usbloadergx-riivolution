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

- v1: `PlaceFst` reaches the known answer (`0x817B2DE0`, 162912 bytes
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
  exactly like a production defect. Lesson: the blob length travels in
  the mailbox (`+144`), and every GDB-poked word is read back from RAM
  before the call. Never trust a freeze without register-level proof.
- GDB reads bypass the data cache: `pendingPlaceOk=false` (plain
  unflushed store) still reads 1 after a verified install, while the
  flushed words and table bytes read correctly. Flushed state is truth;
  unflushed BSS is not.
- Stub run-control rule (12+ runs): p/m/M/Z anytime; `c`/`\x03` ONLY
  from stopped - sent while running they wedge the stub's command loop
  (total silence after). Drive multi-phase flows with writes + target
  waits + watch-stops, never with cont-then-halt.

## Still reserved for Wii hardware

- Whether the real apploader layout matches the captured constants.
- Whether the live loader stack overlaps the destination (model A vs B).
- Whether the game reads the relocated table back correctly.
- Whether cIOS reads deliver the bytes the plan assumed.

## Inputs ledger

- CAPTURED (SB4E01 T0 card logs, v3.36+): arena `{0, 0x817DA740,
  0x817DA740, 153792}`, wants `153934` / `153792`, block `[0x817D8740,
  0x817DA740)`, BSS `[0x80728680, 0x807E3188)`, expected dests
  `0x817B2DE0` / `0x817DA740`.
- SYNTHETIC (labeled in code): one stale-table sample `[0x817DA800,
  0x817DA900)` inside the captured reservation, exercising the skip
  counter; the staged tables are synthetic valid FST heads with marker
  pads, not real FSTs; churn sizes are stand-ins.
