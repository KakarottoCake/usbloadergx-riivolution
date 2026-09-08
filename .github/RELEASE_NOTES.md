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

## Changed in v3.38

- Unambiguous handover signals. A refused boot now blinks its code THREE times with a pause between (meanings 1-7 unchanged - count one group, check the others match; irregular flicker is just progress). A verified install is followed by one solid second ON, then dark, then the jump. Groups without solid = refused; solid without groups = the game has it.
- New log section: every file-mod boot scans all loaded ranges for words already equal to the table addresses (FST address, sizes, arena values), to tell whether anything references the original table.
- If you are on the SB4E01 round: run T0 with THIS build and the v7 pack, and report the light pattern (repeating groups or solid second), the screen, and the log.
- 146,897 automated checks, all passing.

## Changed in v3.37

- CORRECTION: v3.36's notes below describe this fix, but its binary was built from a tree that predates the code - T0 on v3.36 repeats the old black screen. This build carries the fix; the `build :` line in any log names its exact commit, and the release's boot.dol was string-checked for the new code before publishing.
- Fixed the SB4E01 relocation overwrite: a grown file table is now moved down past every apploader-loaded range it would hit, instead of assuming the space below the old table is free. The stale table's own space is still reused (that overlap is expected); malformed range data refuses the grown install rather than guessing around it.
- Every file-mod boot now logs each loaded range with its DOL section and disc offset (or BSS), and the placement names the ranges it kept clear.
- If you are on the SB4E01 round: run T0 with THIS build and send the log - the table should land below the game's 8 KB block this time.
- 146,886 automated checks, all passing.

## Changed in v3.36

- Fixed the SB4E01 relocation overwrite: a grown file table is now moved down past every apploader-loaded range it would hit, instead of assuming the space below the old table is free. The stale table's own space is still reused (that overlap is expected); malformed range data refuses the grown install rather than guessing around it.
- Every file-mod boot now logs each loaded range with its DOL section and disc offset (or BSS), and the placement names the ranges it kept clear.
- If you are on the SB4E01 round: run T0 with this build and send the log - the table should land below the game's 8 KB block this time.
- 146,886 automated checks, all passing.

## Changed in v3.35

- New diagnostic for the SB4E01 relocation failure: every file-mod boot now logs a "Relocation evidence" block naming the planned table address, the loaded game ranges one by one, the thread stack bounds, and the heap extent, with an overlap reading for each. Installation itself is unchanged - same decisions, same refusals, no new blink codes.
- `boot.elf.map` (linker map) now ships with every release next to `boot.elf`, so each build's exact memory layout is on record.
- If you are on the SB4E01 round: run T0 with this build and send the log - the new section is the evidence the relocation question needs.
- 146,876 automated checks, all passing.

## Changed in v3.34

- Removed the last leftover text of the deleted result screen. Nothing is drawn on the boot path.
- Fixed comments that overstated what light-out proves, to match the blink-code table.
- 146,865 automated checks, all passing.

## Changed in v3.33

- New diagnostic: `riivolution/relocorig.txt` installs the verbatim original table, relocated, instead of the rebuilt one - to separate a relocation fault from new-table content. Do not combine it with `nofstinstall.txt`.

## Changed in v3.32

- The white flash in the jump sequence is gone. The launch path is back to stock code; a black screen after the light goes out means the game itself never came up.

## Changed in v3.31

- The loading bar and the pre-jump result screen are gone entirely, along with the markers that turned them on. Nothing is drawn during a mod boot; the disc light is the whole progress signal.

## Changed in v3.30

- New diagnostic: `riivolution/nofstinstall.txt` stages the mod as usual but does not install the rebuilt file table, to separate a table fault from a hook or fragment fault.

Older versions: [CHANGELOG.md](CHANGELOG.md)
