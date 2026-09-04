**Unofficial test build of USB Loader GX with native Riivolution support.**
Not from the upstream wiidev team — do not report problems with it to them.

## Install

Unzip at the **root of your SD card**. It lands in `/apps/usbloader_gx/` and appears in
the Homebrew Channel as *USB Loader GX Riivolution*. Your existing settings, game list
and covers are untouched — it reads the same config files as stock USB Loader GX, so you
can keep your normal build alongside it in a different folder.

## Using it

1. Put the mod on your SD card the way its author describes (usually a `/riivolution`
   folder with the `.xml`, plus a folder of game files).
2. Highlight the game → **Game Settings** → **Riivolution**.
3. Press the top row to cycle through the XMLs found in `<device>:/riivolution`.
4. Set each option below it to the choice you want, then press **Save**.
5. Launch the game.

Choices are stored per game, so each game remembers its own mod setup.

## What actually works in this build

| Riivolution feature | State |
|---|---|
| `<memory>` direct / `valuefile` / `search` / `ocarina` patches | Working |
| `<savegame>` redirect (mod gets its own save) | Working — requires NAND emulation on for that game |
| Option / choice menus and `${param}` substitution | Working |
| `<file>` / `<folder>` file replacement | **Not applied.** Parsed and planned only |

That last row is the important caveat: **mods that replace files on the disc — new
levels, textures, models, audio — will not do anything yet.** The loader-side logic
exists but the on-console disc-read hook that makes it take effect is unfinished. Mods
that work purely through code patches (and mod loaders that bootstrap from a `<memory>`
patch) are the ones worth testing right now.

## Checking what happened

There is no on-screen feedback — Riivolution is applied after the loader has already
shut the screen down. Instead, each launch writes a report to:

```
<device>:/riivolution/usbloadergx_riivo.log
```

next to the XML you selected. It records whether the XML parsed, whether it matches the
game you launched, which options were active, every patch that was enabled, and any
`valuefile` that could not be read. Please attach that file to any bug report.

## Fixed since the previous commit

- **`<memory valuefile=...>` patches never worked.** The blob was opened at patch time,
  which is after the loader unmounts SD/USB, so every read failed silently. Valuefiles
  are now loaded up front while the devices are still mounted.
- **`<memory search>` could write past the end of a DOL section** when the replacement
  value was longer than the pattern it matched. Such matches are now skipped.
- **No bounds checking on patch addresses.** A mismatched XML could write anywhere in
  memory, including over the loader, and crash with nothing on screen. Targets outside
  MEM1/MEM2 are now rejected and logged.
- The Riivolution menu now says **None found** when there are no XMLs on the card, and
  flags a selected XML as **other game** when its `<id>` block doesn't match the game.

## Known rough edges

- vWii (Wii U) has not been tested; the savegame-clone step runs at a point where the
  GUI thread is already gone there.
- The Riivolution menu lists XMLs from the top level of `<device>:/riivolution` only.
