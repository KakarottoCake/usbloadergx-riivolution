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

## Changed in v3.45-launch-pipeline

- Launch lifecycle reliability, same two-run round as v3.44. One per-boot
  owner for staging, booking, and install verdicts with a single reset
  (including mod-less boots): an aborted or repeated launch can no longer
  inherit a previous boot's table, and staging buffers are freed instead of
  leaked. Illegal transitions (re-booking, staging twice, reinstalling,
  installing another boot's table) refuse by construction.
- Spectral cutoff instrumentation for the log that ends at `table
  serialised`: validation entry/return checkpoints name walk/compact/oom
  outcomes, and every log write is verified (failures go to Gecko, never
  silently lost). Send the entry/return lines plus the drive-light behavior.
- Same round: T0 with no marker (in-place control, must boot), then Spectral
  USA full options with the marker. Keep `relocorig.txt`, `mem2fst.txt`,
  `nofstinstall.txt`, `nomempatch.txt` and `ondemand.txt` absent for both.
- 147,138 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
