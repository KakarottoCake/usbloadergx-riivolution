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

## Changed in v3.0

- The log now records each boot step as it completes, with free memory, so a boot that stops before the file report says where it stopped.
- Each `<folder>` rule is logged as it is listed, with a running file count.
- 146,237 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
