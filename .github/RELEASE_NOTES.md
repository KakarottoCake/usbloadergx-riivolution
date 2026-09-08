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

## On-demand mode (opt-in, new, untested on hardware)

Create an empty file `riivolution/ondemand.txt` on the same drive as the mod to turn it
on. Without that file nothing changes and the build behaves exactly like v3.10.

It has never run on a console. Expect it to fail; the log and a USB Gecko trace are the
point of it. Leave the file off for normal use.

## Changed in v3.32

- The white flash in the jump sequence is gone. The launch path is back to stock code; a black screen after the light goes out means the game itself never came up.

## Changed in v3.31

- The loading bar and the pre-jump result screen are gone entirely, along with the markers that turned them on. Nothing is drawn during a mod boot; the disc light is the whole progress signal.

## Changed in v3.30

- New diagnostic: `riivolution/nofstinstall.txt` stages the mod as usual but does not install the rebuilt file table, to separate a table fault from a hook or fragment fault.

## Changed in v3.29

- The loading bar is off again by default, and no longer forced to draw immediately. Turn it on with `riivolution/loadingbar.txt` (it replaces `noloadingbar.txt`).

## Changed in v3.28

- The staged-table checkpoint now says when no table was staged instead of reporting a checksum it never took.

## Changed in v3.27

- The pre-jump result screen is now off unless `riivolution/showlog.txt` exists. Drawing it after the drives were released stopped games booting - confirmed on hardware: a boot with no patches at all black-screened with it on and starts with it off.

## Changed in v3.26

- The log now records whether the staged file table is still intact at device shutdown, so a refused install says when it broke.

## Changed in v3.25

- The disc light keeps pulsing through the apploader instead of freezing for the last three seconds of the boot.

## Changed in v3.24

- Long prompt messages no longer draw on top of the prompt's own title.
- The missing-files warning is shorter.

## Changed in v3.23

- The loading screen no longer waits half a second before appearing, so short phases actually show it. *(Reverted in v3.29 - the wait was what kept it from drawing, and drawing stopped boots.)*

## Changed in v3.22

- The disc light pulses while the mod is being prepared and goes out when the game takes over. If it is dark and the screen is still black after 20 seconds, it is stuck.
- The loading bar is now on by default (`riivolution/noloadingbar.txt` turns it off). *(Reverted in v3.29 - drawing it stopped boots.)*
- The disc light now works for warnings and blink codes even if you have the disc light turned off in settings.

## Changed in v3.21

- A mod on the SD card now warns you when you pick it, and again before launch.

## Changed in v3.20

- Files the mod names but the card does not have are now listed by path, in the log and on screen before launch.
- Missing created files named instead of phantom additions.
- Dolphin reference byte-diff for FST rebuilds.
- Deliberate returns to the loader blink a distinct drive-light code.
- Every mod boot briefly shows its result before launching.
- The last boot's outcome is shown when you launch that game again.
- Mods pack at the drive's sector size, leaving no unmapped gaps between files.
- The rebuilt file table is verified after install; a bad install refuses instead of black-screening.
- Tail-recovered files are matched against their registration records instead of withholding the whole mod.
- 146,865 automated checks, all passing.

Older versions: [CHANGELOG.md](CHANGELOG.md)
