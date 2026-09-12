**Unofficial test build of USB Loader GX with native Riivolution support.**
Not from the upstream wiidev team — do not report problems with it to them.

## Install

Unzip at the root of your SD card. It lands in `/apps/usbloader_gx/` and appears in the
Homebrew Channel as *USB Loader GX Riivolution*. It reads the same config files as stock
USB Loader GX, so your settings, game list and covers are untouched.

## Using it

Put the game and the mod on a **USB drive**, then: game cover →
**Settings** → **Riivolution**, pick the XML, set the options, **Save**, launch.
Choices are stored per game.

The mod's files must be on the same drive as the game, because the cIOS reads every
fragment from one drive. Mixing them is refused.

**Mods on the SD card do not work.** The game will not load - the screen stays black
and you have to reset the console. This happens every time. The loader now warns you
when you pick a mod on SD, and again before launch. Use a USB drive.

Needs AHBPROT — launch from the Homebrew Channel directly, not from a forwarder.

`<memory>`, `<savegame>` and the option menus work. `<file>`/`<folder>` replacement is
attempted from v1.0: if any check fails, nothing is applied and the game boots
untouched.

Every launch writes a report next to the XML you picked —
`<device>:/riivolution/usbloadergx_riivo_<GAMEID>.log`. Please attach it to any report.

## On-demand mode (opt-in, untested on hardware)

Create an empty file `riivolution/ondemand.txt` on the same drive as the mod to turn it
on. Without that file nothing changes. It has never run on a console; do not combine
it with the reservation marker below.

## Changed in v3.44-smg2reserve

- A controlled Wii experiment candidate for SB4E01, not a proven fix. Behind an
  `riivolution/smg2reserve.txt` marker on revision-0 discs only: a grown table goes
  to a fixed MEM2 window (`0x90000800`, 256 KiB) and the game's own BASE getter is
  patched in its 16-byte slot to skip that window. Any failed check withholds the
  table with no fallback; the full `<memory>` set must apply with zero skips or the
  jump is refused first.
- Two runs, no more: T0 with no marker (in-place control, must boot), then Spectral
  USA full options with the marker. Keep `relocorig.txt`, `mem2fst.txt`,
  `nofstinstall.txt` and `nomempatch.txt` absent for both. Send each run's card log
  plus the light pattern (blink groups, or one solid second then dark) and where the
  screen stopped - or gameplay, if it gets there.
- 147,054 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
