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

## Changed in v3.6

- The slowest check is now off by default. It read the whole mod back and compared it against the card - about 256 MB of traffic on a large mod, which is minutes of black screen. Create `riivolution/verify.txt` if you want it.
- Every step of the second half of the boot is now logged as it completes, so a boot that stops there says where.
- The Riivolution page only lists mods for the game you are on, shows the mod name and path on their own lines, and says whether the mod is on SD or USB.
- 146,237 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
