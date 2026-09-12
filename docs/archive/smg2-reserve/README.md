# SMG2 reservation — retired game-specific workaround (historical evidence)

Retired 2026-09-12 under the general-loading-pipeline work order, which
requires that the Riivolution implementation must not select behavior using
specific game IDs, revisions, mod names, or hardcoded addresses discovered
in one game.

## What this was

- Target: one game (`SB4E01` revision 0, Super Mario Galaxy 2 USA) behind one
  marker file (`riivolution/smg2reserve.txt`).
- Mechanism: a grown file table placed at a fixed MEM2 window
  (`0x90000800`, 256 KiB) with the game's own BASE getter
  (`0x805B4E70`, 16-byte slot: `lwz/blr` → `lwz/addis/blr`) patched to skip
  the window. MEM1 arena untouched.
- Policy gate: `Smg2ReservationPending()` armed for install; a memory-patch
  set incomplete under the reservation refused before table install.
- Placement half: `BuildSmg2ReservationPlacement()` (pure arithmetic,
  host-tested). Guard half: `CheckSmg2Reservation()` (game, size, PPC/IOS
  MEM2 boundary, staging in GX MEM2 allocator, getter signature).
- Conflict guard: direct `<memory>` patches writing the getter slot or the
  reserved span refused the reservation; search/ocarina re-checked at apply.

## Why it was retired

Game-ID-gated production behavior obstructs the general pipeline. The fixed
MEM2 window and getter address are per-title surveyed values, not derived
safe addresses. The general architecture must handle grown tables via a
patched boot view (apploader allocates for the rebuilt table) rather than a
per-game reservation.

## What replaces it

Grown (non-in-place) placements now refuse explicitly via the general
placement policy with required/available capacity, until the patched boot
view lands. No silent fallback to the MEM1 cascade below the reservation
(which game startup clears on the affected title). In-place tables
(including suffix-compacted in-place) remain supported.

## Preserved artifacts

- `RiivoSmg2Reserve.hpp` — production header as retired (copied verbatim).
- `test_smg2reserve.cpp` — host test as retired (copied verbatim).
- Parent-tree evidence (not copied, referenced):
  - `../TESTER-smg2reserve.md` — tester plan/notes.
  - `../GXDiag-SB4E01*.zip`, `GXDiag-SB4E01/` — diagnostic builds/logs.
  - `../log-v2.7.log`, `../HANDOFF*.md` — historical handoffs/logs.
  - Commit `efe55d49` ("Mark SMG2 reservation header as experimental
    game-specific workaround") — last production state before retirement.

## Provenance

Surveyed on SB4E01 under Dolphin (game MEM2 grows bottom-up from
`0x90000000`, top faults post-boot) plus static main.dol survey (one getter
reader, one setter writer at `0x805B4ED0`, 12 call sites, no absolute refs
into slot padding). Wii boot was the required dynamic proof; hardware
validation of the reservation itself was not claimed as completed.
