**Unofficial test build of USB Loader GX with native Riivolution support.**
Not from the upstream wiidev team — do not report problems with it to them.

## Install

Unzip at the root of your SD card. It lands in `/apps/usbloader_gx/` and appears in the
Homebrew Channel as *USB Loader GX Riivolution*. It reads the same config files as stock
USB Loader GX, so your settings, game list and covers are untouched.

## Using it

Put the mod on your SD card the way its author describes, then: game cover →
**Settings** → **Riivolution**, pick the XML, set the options, **Save**, launch.
Choices are stored per game.

The mod's files must be on the same drive as the game. USB game, USB mod; SD game,
SD mod. Mixing them is refused, because the cIOS reads every fragment from one drive.

Needs AHBPROT — launch from the Homebrew Channel directly, not from a forwarder.

`<memory>`, `<savegame>` and the option menus work. `<file>`/`<folder>` replacement is
attempted from v1.0: if any check fails, nothing is applied and the game boots
untouched.

Every launch writes a report next to the XML you picked —
`<device>:/riivolution/usbloadergx_riivo.log`. Please attach it to any report.

## Changed in v3.5

- The Riivolution page only lists mods for the game you are on. Mods for other games no longer appear.
- The mod's name and its full path each get their own full-width line instead of being cut off in the narrow right-hand column.
- A new line says whether the mod is on SD or USB.
- The progress bar and the per-`<folder>` log lines are off unless you create `riivolution/loadingbar.txt`.
- If the cIOS will not accept the enlarged fragment list, the game boots unmodified instead of exiting to the Homebrew Channel.
- 146,237 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
