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
2. Click the game's cover → **Settings** → **Riivolution** (the first button).
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

That last row is the important caveat, and it is the one most likely to bite you:
**mods that replace files on the disc — new levels, textures, models, audio — will not
work yet.** The loader-side logic exists but the on-console disc-read hook that makes it
take effect is unfinished.

This matters more than it sounds, because most big mods are *both*: a `<memory>` patch
installs the mod's loader, and that loader then reads replacement files. Apply only the
first half and the game boots into a loader whose files aren't there — which shows up as
a **black screen right after the health and safety screen**. That is expected in this
build, not a bug in your setup. From v0.3 the loader warns you before launching instead
of leaving you at a black screen.

Mods that work purely through code patches are the ones worth testing right now.

## Checking what happened

There is no on-screen feedback — Riivolution is applied after the loader has already
shut the screen down. Instead, each launch writes a report to:

```
<device>:/riivolution/usbloadergx_riivo.log
```

next to the XML you selected. It records whether the XML parsed, whether it matches the
game you launched, which options were active, every patch that was enabled, and any
`valuefile` that could not be read. Please attach that file to any bug report.

## Changed in v0.5 - Phase 3 recon, second pass

The v0.4 dry run worked: it read the game's real file table off the disc and resolved
the mod against it. Two things it got wrong or left unanswered are fixed here.

- **The cIOS survey reported nothing.** It read the info block straight out of NAND
  without initialising ISFS first, so every slot silently came back empty. Fixed, and it
  now also prints the d2x list the loader itself builds at startup, which is the
  authoritative answer.
- **macOS metadata files flooded the results.** A mod unpacked on a Mac carries a `._`
  twin for every real file. Thousands of them were being enumerated and counted as
  "files the mod adds". `._*`, `.DS_Store` and `Thumbs.db` are now skipped and counted
  separately. (Deleting them from your card is still worth doing.)
- **The report now answers the question that decides the design:** for every file the
  mod replaces, is the replacement *bigger* than the original? A bigger file cannot just
  be read in place, because the disc's file table still advertises the old length. The
  log now counts those, lists the biggest, and separates files the mod *replaces* from
  files it *adds*.

Run it once more and send the log. That tells me whether read redirection alone can
carry this mod, or whether the file-table rebuild is mandatory too.

## Changed in v0.4 - Phase 3 begins

File replacement still does not work. What this build adds is the **loader half** of it,
running for real on your console but in **dry-run mode** — it reads your game's actual
file table off the disc, matches the mod against it, and writes down exactly what it
would redirect. It changes nothing about how the game boots, so it is safe to run.

Launch your game with the mod selected and then read
`<device>:/riivolution/usbloadergx_riivo.log`. It will now also contain:

- **A cIOS survey** — every cIOS slot on your console, its d2x version and base IOS, and
  which one the game actually ran under. The disc-read hook has to patch that specific
  cIOS, so this decides how the next step gets written.
- **A Phase 3 dry run** — your disc's FST (offset, size, file count), every disc byte
  range the mod would redirect and to which file on your card, anything missing from the
  card, and any files the mod adds that have no disc entry.

**Please send that log.** It is the input for the IOS-side hook, which cannot be written
blind.

## Changed in v0.3

- **A warning before launch instead of a black screen.** Riivolution is applied after
  the loader has torn down the screen, so anything wrong with the setup used to show up
  only as a hang after the health and safety screen. The loader now checks before it
  boots and tells you if: the XML can't be read, the XML is for a different game, the
  mod needs file replacement (which this build can't do), or every option is still set
  to Disabled. You can Continue anyway or Cancel.

## Changed in v0.2

- **Riivolution is now the first button in Game Settings**, above Game Load, instead of
  sitting after Ocarina. It only appears for Wii games.

## Fixed in v0.1

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
