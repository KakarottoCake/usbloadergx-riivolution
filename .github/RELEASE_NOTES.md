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
`<device>:/riivolution/usbloadergx_riivo_<GAMEID>.log`. Please attach it to any report.

## On-demand mode (opt-in, new, untested on hardware)

Create an empty file `riivolution/ondemand.txt` on the same drive as the mod to turn it
on. Without that file nothing changes and the build behaves exactly like v3.10.

It has never run on a console. Expect it to fail; the log and a USB Gecko trace are the
point of it. Leave the file off for normal use.

## Changed in v3.19

- Drive-light blink codes name each launch refusal.
- Checkpoints isolate the apploader-return window.
- Install-failure code split by bytes versus pointers.
- 146,796 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
