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

## Changed in v3.8

- Much faster to start a modded game. Each mod folder is now read off the card once instead of twice, and the per-file read-back check samples up to 128 files instead of reading all of them.
- Every step in the log carries how long the boot had been running when it finished.
- If preparing the mod takes longer than five minutes, the loader gives up and boots the game unmodified.
- `riivolution/verify.txt` restores the full per-file check and the whole-mod read-back.
- 146,237 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
