<p align="center"><a href="https://github.com/wiidev/usbloadergx/" title="USB Loader GX"><img src="data/web/logo.png"></a></p>

# USB Loader GX — Riivolution Edition

A fork of [USB Loader GX](https://github.com/wiidev/usbloadergx) that adds **native
Riivolution mod support**, so you can play game mods **on the fly** — no need to
pre-patch or rebuild your game backup first.

> ⚠️ **Work in progress.** Memory-based mods and mod-save redirection work today.
> Full file-replacement mods are partly built and still need testing on a real Wii.
> See [Status](#status) below before you get your hopes up.

---

## What is this, in plain English?

**USB Loader GX** is a popular app for the Wii that launches your game backups from a
USB drive or SD card.

**Riivolution** is a modding system for Wii games. Fan-made mods — new levels, texture
packs, translations, difficulty hacks — are distributed as a small folder of files plus
an XML "recipe" that tells the console what to change. Think of it like the Wii's
version of a mod loader.

Normally, using a Riivolution mod means either running a separate Riivolution app, or
using a PC tool to bake the mod permanently into a copy of the game (a big, slow,
disk-hungry step). **This fork skips all that.** You pick the mod right inside USB
Loader GX, launch the game, and the mod is applied in memory as the game boots. Your
original game backup is never modified.

## How you'll use it (once finished)

1. Copy the mod's files to your SD card (exactly as the mod author instructs — usually a
   `riivolution` folder plus a folder of game files).
2. Open USB Loader GX and highlight your game.
3. Open **Game Settings** — **Riivolution** is the first button in the list.
4. Pick the mod's XML and toggle the options it offers (e.g. "Widescreen: On",
   "Character pack: B").
5. Launch the game. That's it — the mod is live.

Settings are saved **per game**, so each game remembers its own mod choices.

## Status

| Riivolution feature | What it does | State |
|---|---|---|
| `<memory>` patches | Directly change values in the game's code/RAM (most code mods, cheats-style hacks, ASM loaders) | ✅ Working |
| `<memory search>` / `<memory ocarina>` | Find-and-patch and hook-injection variants | ✅ Working |
| `<savegame>` | Give the mod its own separate save file so it can't corrupt your vanilla save | ✅ Working (needs NAND emulation enabled) |
| Option / choice menus + `${param}` | The mod's configurable toggles | ✅ Working |
| `<file>` / `<folder>` replacement | Swap real game files for modded ones from your SD card (new levels, textures, audio) | 🛠️ Loader logic built & tested; the on-console read-hook still needs hardware testing |
| `<folder>` add-new-files | Mods that add brand-new files | ⏳ Planned |

## "It black-screens right after the health and safety screen"

That is the expected failure for a mod that replaces disc files, which this build cannot
do yet. Most large mods install their loader with a `<memory>` patch and then have that
loader read replacement files off the SD card — apply only the first half and the game
jumps into a loader whose files are missing, and hangs.

The loader now checks for this before booting and warns you, along with the other common
setup mistakes (XML for the wrong game, XML that won't parse, every option left on
Disabled). If you get the warning and Continue anyway, expect the hang.

## Did it work? (checking without a USB Gecko)

Riivolution runs at the very end of the boot sequence, after the loader has already torn
down the screen, so there is nothing to see on the TV. Every launch therefore writes a
short report to:

```
<device>:/riivolution/usbloadergx_riivo.log
```

on the same device as the XML you selected. Pull the card, open that file, and it will
tell you whether the XML parsed, whether it is actually meant for that game, which
options were active, how many patches were enabled, and whether any `valuefile` blobs
were missing. If the file isn't there at all, the loader never reached your XML — check
that **Game Settings → Riivolution** is pointing at a file and not `OFF`.

**How it works under the hood, briefly:** memory patches reuse the loader's existing
"poke RAM before the game starts" machinery (the same idea as Ocarina cheats), so they
need no custom cIOS. File replacement is the hard part — it requires intercepting the
game's disc reads at runtime. This fork does that by patching the running cIOS **in
memory at boot**, so there's **nothing extra to install and no risk of bricking from
flashing a custom cIOS**.

Full technical design and progress: [RIIVOLUTION_PLAN.md](RIIVOLUTION_PLAN.md).

## Requirements

- A Wii (or Wii U in vWii mode) already set up to run USB Loader GX.
- The [latest d2x cIOS](https://github.com/wiidev/d2x-cios/releases) (same as stock USB
  Loader GX — see [README.upstream.md](README.upstream.md) for cIOS slot setup).
- For mod-save redirection, NAND emulation enabled for the game.

## Building it yourself

Same toolchain as upstream USB Loader GX (devkitPro / devkitPPC). The repository ships a
`Dockerfile` that pins the exact toolchain:

```bash
docker build -o . .
```

The compiled `boot.dol` lands in the project directory.

## Credits & license

- Built on **[USB Loader GX](https://github.com/wiidev/usbloadergx)** by the wiidev team
  and contributors — all original credits in [README.upstream.md](README.upstream.md).
- Riivolution XML parsing and patch logic are ported from the excellent reference
  implementation in **[Dolphin](https://github.com/dolphin-emu/dolphin)**
  (`DiscIO/RiivolutionParser` / `RiivolutionPatcher`), and from the companion
  ISO-patcher project **[riivolution-to-iso](https://github.com/KakarottoCake/riivolution-to-iso)**.
- Riivolution patch format: <https://aerialx.github.io/rvlution.net/wiki/Patch_Format/>

This project inherits USB Loader GX's **GPL-3.0** license. See
[gpl-3.0.txt](gpl-3.0.txt) / the upstream repository for details.

*Not affiliated with or endorsed by the upstream USB Loader GX team, Dolphin, or
Nintendo. Use with your own legally-obtained games.*
