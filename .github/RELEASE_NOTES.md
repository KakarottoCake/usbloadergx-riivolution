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

Needs AHBPROT — launch from the Homebrew Channel directly, not from a forwarder.

`<memory>`, `<savegame>` and the option menus work. `<file>`/`<folder>` replacement is
attempted from v1.0: if any check fails, nothing is applied and the game boots
untouched.

Every launch writes a report next to the XML you picked —
`<device>:/riivolution/usbloadergx_riivo.log`. Please attach it to any report.

## Changed in v2.8

- Mod files are now verified with full 128 KiB reads and reads across every fragment boundary, not 32 bytes per file.
- The rebuilt file table is walked independently and every path resolved before it is installed.
- The Gecko code handler and the width/480p trampolines stand down when the mod owns the memory they write.
- The log records the loader's own patch settings and the handler policy actually used.
- IOS dumps are off unless `riivolution/dumpios.txt` exists.
- 146,232 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
