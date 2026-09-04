**Unofficial test build of USB Loader GX with native Riivolution support.**
Not from the upstream wiidev team — do not report problems with it to them.

## Install

Unzip at the **root of your SD card**. It lands in `/apps/usbloader_gx/` and appears in
the Homebrew Channel as *USB Loader GX Riivolution*. Your existing settings, game list
and covers are untouched — it reads the same config files as stock USB Loader GX, so you
can keep your normal build alongside it in a different folder.

## Using it

1. Put the mod on your SD card the way its author describes (usually a `/riivolution`
   folder with the `.xml`, plus a folder of game files).
2. Click the game's cover → **Settings** → **Riivolution** (the first button).
3. Press the top row to cycle through the XMLs found in `<device>:/riivolution`.
4. Set each option below it to the choice you want, then press **Save**.
5. Launch the game.

Choices are stored per game, so each game remembers its own mod setup.

## What actually works in this build

| Riivolution feature | State |
|---|---|
| `<memory>` direct / `valuefile` / `search` / `ocarina` patches | Working |
| `<savegame>` redirect (mod gets its own save) | Working — requires NAND emulation on for that game |
| Option / choice menus and `${param}` substitution | Working |
| `<file>` / `<folder>` file replacement | **Attempted from v1.0.** See below |

File replacement is switched on from v1.0. When every check passes, the mod's files are
added to the table the console reads the game through, the file table is rebuilt so the
game knows about them, and a four-byte change in memory makes the console serve them.

It checks itself before committing: it reads the first mod file back *through the console*
and compares it with the file on your card. If anything is off — the mod on a different
drive from the game, a filesystem whose layout can't be read, a dual-layer game, a game
whose own data crosses the 4 GiB line — it stops and the game boots exactly as it would
without Riivolution. The log says which check stopped it.

If a mod replaces files and that could not be done, its `<memory>` patches are held back
as well and the game boots completely unmodified. Those patches assume the mod's files are
present; applying them on their own is what makes a total conversion exit to the System
Menu instead of booting.

This is new and has had little time on real hardware, so treat a working boot as good news
rather than the expected outcome, and please send the log either way.

**It needs AHBPROT**, which means launching from the Homebrew Channel directly - not from a
forwarder channel or anything that reloads IOS on the way in. The log says whether it was
granted.

## Checking what happened

There is no on-screen feedback — Riivolution is applied after the loader has already
shut the screen down. Instead, each launch writes a report to:

```
<device>:/riivolution/usbloadergx_riivo.log
```

next to the XML you selected. It records whether the XML parsed, whether it matches the
game you launched, which options were active, every patch that was enabled, and any
`valuefile` that could not be read. Please attach that file to any bug report.

## Changed in v1.4

- Fixed: the cIOS probe and the four-byte patch now run in `SetupDisc`, where hardware
  access still exists. They were running later, in `BootPartition`, by which point
  AHBPROT is closed and neither could ever succeed.
- The log now reports the patch result from the early window, and says why if it failed.

## Changed in v1.3

- Fixed: the fragment list was enlarged even when file replacement could not run, so the
  game was booted differently from stock for no reason. It is now left untouched unless
  the mod can actually be applied.
- Fixed: a disc file claimed by two overlapping `<folder>` rules produced two entries at
  the same place and the whole plan was refused with "two files overlap".
- The log now says when the fragment list was deliberately left alone, and why.
- 9 more automated checks - 2314 total, all passing.

## Changed in v1.2 - it warns you before launching, not after

Two things the last test should not have had to discover from a log file.

### The warning before launch was still telling you it does not work

Since v1.0, picking a mod that replaces files put up this, every time:

> This mod replaces files on the disc, which this build cannot do yet, so the game will
> most likely hang on a black screen.

That was written for v0.3 and has been wrong since v1.0. Anyone testing a file-replacing
mod was told, on the way in, that the thing they were testing could not work. Gone.

### It now checks for hardware access while there is still a screen

File replacement has to patch the running cIOS in memory, and that needs the hardware
access (AHBPROT) that the Homebrew Channel grants at launch. Starting the loader from a
forwarder channel — or anything that reloads IOS on the way in — drops it before the
loader ever runs, and nothing can get it back afterwards.

Until now that was only discoverable *after* the fact, buried in the log as
`AHBPROT is not open`, following a boot that looked normal and quietly did nothing. It
cost a full test round.

Now, if the selected mod replaces files and that access is missing, you get a prompt
before the game launches:

> This mod replaces files, which needs hardware access this loader was not given. Launch
> USB Loader GX from the Homebrew Channel directly — not from a forwarder channel — or the
> mod's files will not be applied. The game will still boot unmodified.

Continue or Cancel, same as the other pre-flight warnings. Mods that only use `<memory>`
or `<savegame>` do not need it and are not warned about.

## Changed in v1.1 - three bugs found by the NSMBW test

A test of Newer Super Mario Bros. Wii ended at the System Menu instead of booting.
The log explained all of it, and it was three separate faults stacked on top of each
other. All three are fixed.

### 1. File replacement could never activate. On any game.

This is the serious one, and it was self-inflicted in v1.0.

To make room for the mod, the loader tells the cIOS the virtual disc is bigger than it
really is - that is what promotes the disc to dual-layer read limits and creates the
space above the backup. It does that by raising the fragment list's declared size to the
read ceiling.

The code that then works out *where* the mod can go read that same field back and took it
for the backup's own size. So every game looked like it already filled the entire disc
address space, and every game was refused with:

```
REFUSED: the backup already fills the disc address space -
         dual-layer games cannot be patched this way
```

New Super Mario Bros. Wii is single-layer. Its data ends at 4.29 GB with plenty of room
above it. The message was simply wrong, and it would have appeared on every single-layer
game since v1.0 - including Galaxy 2.

The backup's real size is now recorded *before* the reservation overwrites it. The log
prints both figures so the two can never be confused again.

### 2. A mod's memory patches are no longer applied without its files

This is what actually caused the exit to the System Menu, and it is the more important
fix of the two.

Newer SMBW is a total conversion: 1092 files, 996 of which the disc has no entry for at
all, plus 34 memory patches including a 2552-byte block. Those memory patches are written
on the assumption that the mod's files are there. Fault 1 meant the files were refused -
but the memory patches were applied anyway. The game was patched to go looking for assets
that were not on the disc, and it bailed out to the System Menu.

That broke the rule this whole thing is built on: **every failure must still boot the
game.** It now holds:

- If a mod replaces files and those files did not get installed, its memory patches are
  skipped too, and the game boots completely unmodified.
- A mod that only uses `<memory>` or `<savegame>` is unaffected - it never wanted files,
  so nothing is held back.

The log now ends with a **Memory patches** section saying `Applied` or `HELD BACK`, and why.

### 3. Games whose apploader leaves arena low at zero

The rebuilt table is placed using the four boot-info words the apploader fills in. NSMBW's
apploader sets three of them and leaves arena low at `00000000`, letting the game's own
startup fill it in later. The placement code treated that as a corrupt value and refused.

A zero there means "not known yet", not "invalid". The table only ever moves *down* from
arena high, into heap the game has not been handed, so it is placeable regardless - what
is lost is the ability to check the game keeps 4 MB of heap. So with an unknown floor the
move is capped at 1 MB instead, and anything larger is still refused rather than guessed
at. On the real NSMBW layout the table needs 26 KB.

### Also

- Corrected log wording that still said file replacement was "unfinished" and listed work
  as "still to build". Both have been done since v1.0; the text had not caught up and made
  a working build look like a dry run.
- 14 more automated checks, including the exact NSMBW numbers from the tester's log -
  **2311 total, all passing**.

### One thing that is not a bug: AHBPROT

That test also reported:

```
AHBPROT is not open, so IOS memory is hidden from the loader
```

Without it the cIOS patch cannot be made, so file replacement will not switch on no matter
what else is fixed. It is not something the loader can work around - the permission has to
be granted at launch.

**Launch it from the Homebrew Channel directly.** Not from a forwarder channel, and not
from anything that reloads IOS on the way in - those drop the hardware access before the
loader ever starts. The Homebrew Channel needs to be reasonably current (1.1.0 or newer)
for it to be granted at all. The log says whether it was.

## Changed in v1.0.1

- **Fixed: 4K-sector drives could never activate.** The mod's files were laid out on 2 KB
  boundaries (a Wii disc's own granularity) but a fragment has to start on a boundary of
  the *drive's* sectors. On a 4K-native drive every file failed the alignment check and
  the whole thing refused. It now aligns to whichever is larger.
- Clarified what does and does not work with **`.wbfs` files**: they are fine. A game
  stored as a `.wbfs` container on a FAT/NTFS/ext drive goes through a second layer of
  mapping to find its real sectors, and the fragment list that comes out of that is in
  exactly the same address space the mod's files are added to. A raw **WBFS partition** is
  a different matter - it has no filesystem, so there is nowhere to put the mod's files in
  the first place, and that is refused with a message saying so.

## Changed in v1.0 - it is switched on

This is the first build that actually tries to run the mod. Everything up to now
measured and refused; this one, when every check passes, does the four things that make
it real:

1. **Extends the fragment list** — looks up where each of the mod's files physically sits
   on your drive and adds them to the table the cIOS reads the game through.
2. **Checks its own work** — reads the first mod file back *through the cIOS*, at the
   offset the game will ask for, and compares it against the file on your card. If those
   don't match, it stops here.
3. **Patches the cIOS** — the four bytes, in memory only.
4. **Installs the rebuilt file table** into the game's memory and points the game at it.

### If something is wrong, the game still boots

The order above is chosen so that every failure leaves your console in a state that boots
the game normally:

- A half-built fragment list is never handed over.
- The extended list, once handed over, is a **superset** of the original — every read
  below the mod region is byte-for-byte what it was.
- The four-byte patch on its own changes nothing, because a game with an unmodified file
  table never reads above the 4 GiB line.
- The rebuilt table is only installed *after* the patch is confirmed in place, so the game
  is never pointed at a region nothing is serving.

Both logs are written **before** the game is launched, so even if it hangs, the log tells
us exactly how far it got and which step stopped.

### One more thing that had to change

Your backup declares how big its virtual disc is, and the cIOS uses that to decide whether
the disc is single- or dual-layer — which sets the read ceiling. A single-layer disc caps
reads at 4.699 GB, which is *exactly* where the mod region starts, so every mod read would
have been refused. The loader now declares a larger virtual disc before handing the list
over, so the cIOS's second-layer probe succeeds and the ceiling becomes 8.5 GB. Nothing is
added to your drive by this — unmapped areas simply read as zeros.

### What to send

The same log as before, whatever happens:

```
<device>:/riivolution/usbloadergx_riivo.log
```

It now ends with a **Switching it on** section saying either what was activated, or which
check stopped it and why.

## Changed in v0.9 - where the mod actually lives

The cIOS reads your game through a *fragment list* — a table mapping offsets on a virtual
disc to real sectors on your drive. Adding the mod means extending that table so the extra
offsets point at ordinary files on your card. This build works out exactly where that
region can sit and whether everything fits, and prints it in the log.

Three constraints decide it, and all three are now checked before anything is attempted:

1. It must start **at or above 4 GiB**, because that's the only threshold the four-byte
   patch can express.
2. It must start **at or above the end of your backup's own virtual disc** — otherwise the
   game's fragments would shadow the mod's, and files would silently read as the wrong
   data rather than fail.
3. It must end **below the cIOS read ceiling**, past which every read is refused.

For a normal single-layer backup that puts the region at about **4.70 GB**, leaving
roughly **2.7 GB** of space under the ceiling — comfortably more than a mod like Spectral
needs.

### A limitation that falls out of this

Constraint 2 rules out **dual-layer games**. Their backup already fills the address space
right up to the ceiling, so there is nowhere left to put anything. That is a real
consequence of the approach, not something I've overlooked. Single-layer games — including
Super Mario Galaxy 2 — are fine.

### Also

- The log now reports your backup's declared size, how many of the 20000 fragment slots it
  already uses, and how many the mod would need.
- 28 more automated checks — **2297 total, all passing**.

Still not applied. What's left is the mechanical part: reading each mod file's real sectors
off the card, appending them to the table, and writing the rebuilt file table into the
game's memory.

## Changed in v0.8 - the patch, and it is four bytes

I said I needed a photograph of your cIOS's memory before I could write the disc-read
patch. I no longer do — I got the code a better way, and the patch is written.

d2x doesn't ship its disc plugin as a file you can open, but it *does* build from source,
and the release tag `d2x-v11-beta3` is exactly the cIOS in your slot 252. Building it
locally with the ARM compiler and disassembling the result gives the real instructions:

```
ldr  r3, [r5, #0]     ; config.mode
lsls r3, r3, #30      ; is this image already decrypted?
bmi  .raw             ;   yes -> hand back raw bytes
b    .decrypt         ;   no  -> decrypt and hash-check (normal path)
```

Changing the first two instructions turns "is this image already decrypted?" into
**"is this read at or above 4 GiB?"**:

```
ldr  r3, [r0, #8]     ; the offset being read
lsls r3, r3, #1       ; test bit 30
```

Mod files are relocated above the 4 GiB line, so they come back raw from your drive —
which is right, because they're ordinary files sitting on your card. Everything below the
line is real game data and is still decrypted and hash-checked exactly as before. Four
bytes, in memory only, gone on reboot. Nothing is installed and no cIOS is modified.

**This build checks that patch site exists on your console but does not write it.** The
log will say `FOUND, exactly once` if the running cIOS matches what I built against.

### What I still need

Just the log, same as before:

```
<device>:/riivolution/usbloadergx_riivo.log
```

The `.bin` dump is still written, but it now only matters if the log says the patch site
was *not* found — in which case that file is what I'd use to re-derive it.

### One limitation worth knowing

The threshold has to be a power of two — there's only room for two instructions, and an
arbitrary comparison needs a constant loaded from memory that won't fit. That makes 4 GiB
the only usable line: 2 GiB lands inside real game data, and the ceiling above is fixed by
the disc format. So a game whose own data reaches past 4 GiB can't be done this way. Super
Mario Galaxy 2's data ends at about 4.28 GB — under the line, with roughly 9 MB to spare.
The log now prints that margin so it's checkable rather than assumed.

### Still to build

Extending the fragment list to cover the relocated files, and writing the rebuilt table
into the game's memory. Both are already measured in the log.

## Changed in v0.7 - the last thing I need from your console

**Please run your game once with the mod selected, then send me two files:**

```
<device>:/riivolution/usbloadergx_riivo.log
<device>:/riivolution/usbloadergx_riivo_dip.bin
```

The `.bin` is new, about 32 KB, and it is the one thing I cannot work out from
here. Nothing in this build changes how your game boots — it reads, measures and
writes those two files.

### Why one more round

I found the route to making this actually work, and it is much smaller than I
feared. Your cIOS reads the game off your drive through a fragment list, and it
has **two** read paths: the normal one that decrypts (and checks hashes), and one
that hands back raw bytes untouched. Mod files get relocated to their own region
high above the real game data, so the change needed is roughly *"reads above this
line take the raw path"*. That is a couple of instructions. Everything below the
line — the actual game — is untouched and still verified exactly as before.

The catch is that those instructions have to be written against your cIOS's real
compiled code, and that code cannot be obtained anywhere but a running console:
IOS modules run at addresses the Wii's ARM chip remaps privately, d2x does not
ship the plugin as a separate file, and the layout shifts between cIOS versions.
So this build scans your console's memory for the plugin's fingerprints and
photographs the surrounding code into that `.bin`. With it I can write the patch
against real instructions instead of guessing; without it I would be shipping you
blind attempts, one reboot at a time.

### Also in this build

- **Where the rebuilt file table would go.** The table has to be written into the
  game's memory and the game pointed at it. That is done by taking a slice of the
  heap the game has not claimed yet — the same trick the console's own loader
  uses. Measured on a normal layout it costs about **272 KB out of 24 MB**. The
  placement logic refuses rather than guesses on anything unexpected, because a
  wrong address there overwrites the running game and looks like a hang.
- 37 more automated checks, mostly of those refusals — **2269 total, all passing**.

## Changed in v0.6 - the answer, and the engine that follows from it

The v0.5 log answered the design question, and the answer was worse than hoped. Measured
on your console, on Super Mario Galaxy 2 with Super Mario Spectral:

| | files |
|---|---|
| real mod files (after discarding macOS `._` twins) | 2148 |
| **no entry on the disc at all** — files the mod *adds* | **1881 (87.6%)** |
| match a disc file but are **bigger** than it | 64 |
| could be served by simply reading a different file | **203 (9.5%)** |

Simply pointing disc reads at files on your card was never going to run this mod. A
level hack is mostly *new* files, and the disc's file table has no entry for them — the
game would never even ask. So the file table itself has to be rebuilt.

**That rebuild engine is now written and tested.** It reads the game's real file table,
inserts entries for every file the mod adds (creating folders as needed), corrects the
size of every file the mod replaces, and hands each one a fresh location. It is covered
by automated checks — including a full rebuild of a 3920-file table — which run on a PC
rather than on your console, so this part did not cost you any reboots. They are in the
repository under `hosttests/` if you want to run them yourself.

This build runs the whole thing against your actual disc and writes the result to the
log — real entry counts, the new table size, where the mod would live, and whether that
fits inside what your cIOS will read. Nothing is applied, so it is safe to run.

**One correction to what I told you earlier.** I previously said the IOS-side hook might
not be needed. That was wrong, and this build's log now explains why: the fragment list
the loader gives the cIOS serves your backup with the game partition still *encrypted*,
and the console decrypts whatever comes back. A plaintext file from your card, fed
through that path, decrypts into noise. The substitution has to happen inside IOS, after
decryption. That hook is the remaining work.

## Changed in v0.5 - Phase 3 recon, second pass

The v0.4 dry run worked: it read the game's real file table off the disc and resolved
the mod against it. Two things it got wrong or left unanswered are fixed here.

- **The cIOS survey reported nothing.** It read the info block straight out of NAND
  without initialising ISFS first, so every slot silently came back empty. Fixed, and it
  now also prints the d2x list the loader itself builds at startup, which is the
  authoritative answer.
- **macOS metadata files flooded the results.** A mod unpacked on a Mac carries a `._`
  twin for every real file. Thousands of them were being enumerated and counted as
  "files the mod adds". `._*`, `.DS_Store` and `Thumbs.db` are now skipped and counted
  separately. (Deleting them from your card is still worth doing.)
- **The report now answers the question that decides the design:** for every file the
  mod replaces, is the replacement *bigger* than the original? A bigger file cannot just
  be read in place, because the disc's file table still advertises the old length. The
  log now counts those, lists the biggest, and separates files the mod *replaces* from
  files it *adds*.

Run it once more and send the log. That tells me whether read redirection alone can
carry this mod, or whether the file-table rebuild is mandatory too.

## Changed in v0.4 - Phase 3 begins

File replacement still does not work. What this build adds is the **loader half** of it,
running for real on your console but in **dry-run mode** — it reads your game's actual
file table off the disc, matches the mod against it, and writes down exactly what it
would redirect. It changes nothing about how the game boots, so it is safe to run.

Launch your game with the mod selected and then read
`<device>:/riivolution/usbloadergx_riivo.log`. It will now also contain:

- **A cIOS survey** — every cIOS slot on your console, its d2x version and base IOS, and
  which one the game actually ran under. The disc-read hook has to patch that specific
  cIOS, so this decides how the next step gets written.
- **A Phase 3 dry run** — your disc's FST (offset, size, file count), every disc byte
  range the mod would redirect and to which file on your card, anything missing from the
  card, and any files the mod adds that have no disc entry.

**Please send that log.** It is the input for the IOS-side hook, which cannot be written
blind.

## Changed in v0.3

- **A warning before launch instead of a black screen.** Riivolution is applied after
  the loader has torn down the screen, so anything wrong with the setup used to show up
  only as a hang after the health and safety screen. The loader now checks before it
  boots and tells you if: the XML can't be read, the XML is for a different game, the
  mod needs file replacement (which this build can't do), or every option is still set
  to Disabled. You can Continue anyway or Cancel.

## Changed in v0.2

- **Riivolution is now the first button in Game Settings**, above Game Load, instead of
  sitting after Ocarina. It only appears for Wii games.

## Fixed in v0.1

- **`<memory valuefile=...>` patches never worked.** The blob was opened at patch time,
  which is after the loader unmounts SD/USB, so every read failed silently. Valuefiles
  are now loaded up front while the devices are still mounted.
- **`<memory search>` could write past the end of a DOL section** when the replacement
  value was longer than the pattern it matched. Such matches are now skipped.
- **No bounds checking on patch addresses.** A mismatched XML could write anywhere in
  memory, including over the loader, and crash with nothing on screen. Targets outside
  MEM1/MEM2 are now rejected and logged.
- The Riivolution menu now says **None found** when there are no XMLs on the card, and
  flags a selected XML as **other game** when its `<id>` block doesn't match the game.

## Known rough edges

- vWii (Wii U) has not been tested; the savegame-clone step runs at a point where the
  GUI thread is already gone there.
- The Riivolution menu lists XMLs from the top level of `<device>:/riivolution` only.
