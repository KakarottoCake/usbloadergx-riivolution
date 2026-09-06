# Changelog

Full history for the Riivolution fork. The GitHub release body carries only the
current version's bullets; everything older lives here.

## Unreleased

The last gap in the on-demand path is closed: os_sync_after_write is now found
in the running plugin instead of being left out.

Starlet's data cache is write-back and does not snoop toward the PowerPC, so
without that call the game reads whatever was in its buffer before - it fails
later and looks like corruption rather than pointing anywhere near here. d2x's
own read routines already sync what they DMA in; what was going unsynced was
the module's own copy out to the game's buffer.

IOS syscalls are the undefined ARM instruction 0xE6000010 | (num << 5), each
wrapped in a stub ending in `bx lr`, so the routine is found by that pair
rather than by counting entries in a table whose contents differ between IOS
versions. That it is syscall 0x40 was established from the module itself -
both device read routines call it around their own transfers - rather than
from a remembered syscall table.

Absence is tolerated, since the module skips the call and the result is merely
stale rather than wrong-addressed. Ambiguity is not: two candidates resolve to
none rather than to a coin toss, because calling the wrong stub would run an
arbitrary syscall on every read.

The address needs the same physical masking as everything else the module is
handed - it is found by scanning a snapshot taken through the PowerPC's view,
and reached from Thumb by a BLX, which takes the target state from bit 0.


The on-demand path is wired into the boot sequence, behind a marker file
(riivolution/ondemand.txt). The fragment path is untouched and still the
default: this changes how every mod file is reached, so it earns its way in
rather than being switched on under people who did not ask for it.

With the marker present the boot skips AppendModFragments entirely. That call
is the reason the screen stays black - it opens every placed file and walks its
cluster chain, thousands of them, and the cost grows with the size of the mod
rather than with what the game actually reads. On-demand resolves a file by
path when the game asks for it, so none of it happens before the game starts.
The game's own fragments are left exactly as they were, so a refusal anywhere
after that still boots the game unmodified.

The module and the redirect table come out of ONE MEM2 reservation. Lowering
the arena boundary twice is a way to get the second one wrong, and getting it
wrong is silent - the game simply allocates over whichever was left outside.
The boundary is lowered before either is written, so a failure from that point
on costs the game a little MEM2 and nothing else.

One thing the wiring exposed, which would have been a freeze: the module is
written by the PowerPC at 0x93xxxxxx but Starlet runs it at 0x13xxxxxx, so its
relocations have to be applied against the physical address, and the pointers
to d2x's own read routines have to be masked the same way. The hook's call into
the module is unaffected either way - a Thumb BL is pc-relative, so the
distance is the same in both views.


The on-demand hook and the module that backs it are now buildable end to end.

BuildDiHookOnDemand emits redirect_ondemand.S instead of the fragment-list
routine and points its call at the module. It does not repeat any discovery:
it runs the shipped builder first and reuses the site, the storage slot, the
branch and the resolved epilogue, so the two paths cannot disagree about where
it is safe to patch. The module entry must be passed even - a Thumb symbol's
bit 0 is state, not address, and rounding it here would be a guess about which
way. test_dihook now reassembles redirect_ondemand.S and compares it to the
embedded bytes, so the .S and the copy that actually runs cannot drift.

The module itself (source/riivo/ios) is compiled for the Starlet offline and
carried as bytes, because the Wii build has no ARM compiler - the same
arrangement redirect.S has always had. 5248 bytes of code, 4768 of bss.

It is copied to whatever address the MEM2 reservation produced, which is not
known until the game is booting, so it is linked with --emit-relocs and every
absolute word is fixed up against where it actually landed. Only R_ARM_ABS32
is touched; the pc-relative calls are correct wherever the module sits and
adding a delta to one would send an internal call somewhere arbitrary. The
test places the same module at two addresses and checks that exactly the
relocated words differ, each by exactly the load delta, and that every one of
them still points inside the module.

The parameter block is written big-endian, byte by byte, because that is how a
big-endian ARM reads its own memory - and its magic is checked before anything
is filled in, so a blob and a set of offsets from different builds cannot be
combined into something that branches into the middle of its own code.


The injected module now runs end to end on emulated big-endian ARM: a redirect
table goes in, and the game's bytes come out of a real FAT image, through the
same C that will run on the Starlet.

The address mask is the part worth testing and it is now tested so it cannot
pass by accident. Destinations arrive in the PPC's view of memory (0x90xxxxxx)
and Starlet sees the same memory at 0x10xxxxxx, so every one has to be masked
before it is written through; getting it wrong is an ARM data abort inside an
IPC that is then never answered - a hard freeze, no screen, no log. The
emulator maps memory ONLY at the physical address, so a module that forgets to
mask faults instead of appearing to work. Removing the mask was tried: it
faults, which is what makes the passing run mean something.

Everything Starlet reads goes through a bounce buffer the module owns rather
than being DMA'd straight into the caller's buffer. That costs a copy per
chunk and buys not having to know whether d2x's device routines accept a
PPC-form address - a question nothing offline can settle, and whose wrong
answer is a DMA into the wrong physical page.

Two alignment bugs the emulator found, both of which would have meant nothing
ever read on hardware: the FAT reader's sector buffer was 4-byte aligned when
the storage engines DMA into it and need 32, and the multi-sector fast path
could hand storage a destination that a partial head had left unaligned. The
fast path now falls back to the bounce buffer in that case rather than being
refused mid-transfer.

redirect_ondemand.S is the hook that calls the module. It does no range test of
its own - the module returns MISS for anything outside the mod region, which is
the same decision made once instead of twice. Unlike the shipped hook it
reaches its pass-through path AFTER issuing a call, so lr has been clobbered
and is restored off the stack; returning with the wrong lr lands back inside
the routine and loops.


RiivoStorageProbe: finds the two cIOS routines that read raw sectors off the
card. The on-demand design has to read arbitrary sectors from inside the DI
thread, and the existing hook cannot - it calls d2x's fragment reader, which
takes a 32-bit WORD offset on the virtual disc. That is 16 GiB of address
space in total, and the mod region already sits at 6 GiB, so a card bigger
than a couple of gigabytes cannot be addressed through it at all.

Underneath the fragment reader, d2x resolves a fragment to a plain LBA and
calls one of two device routines, chosen at run time by a word in its device
config. Those take an absolute 32-bit sector number - two terabytes of range -
and they are exactly the primitive a FAT reader wants. The two have DIFFERENT
calling conventions, so which one applies is recorded rather than guessed:
calling the wrong one puts the LBA where the count belongs, reads a plausible
wrong sector, and returns success.

The dispatch is located by its instruction pattern and required to be unique
in the module. Anything else - an ambiguous match, a config pointer outside
MEM2, a branch leaving the image, both calls resolving the same, a call that
misses a function entry - refuses with no IOS code changed. Physical MEM2
literals (0x13800000 upwards, where the plugin is linked) are accepted
alongside the PPC-visible 0x93800000 view.

Enumeration no longer stats every file twice. devkitPro's dirent carries
d_stat, filled from the directory entry readdir just read; the old code threw
that away and called stat() on the full path, making libfat walk from the root
again for each of several thousand files and evicting the directory being
iterated from its small sector cache.


RiivoRedirectTable: the table the loader hands to IOS in the on-demand design.
Entries are {disc offset, length, path} rather than raw sectors, so the loader
no longer walks a FAT cluster chain per file before the game starts - it needs
only each file's size, which enumeration already knows.

The format is little-endian regardless of the console. A big-endian PowerPC
writes it and a big-endian ARM reads it, and neither may depend on a struct
layout, so every field is written a byte at a time. A memcpy of a u32 here
would emit the wrong order and the module would reject the table as a number
on a black screen.

The builder refuses rather than emits anything the module would distrust:
unsorted entries, overlapping extents, relative or empty paths, a path with an
embedded null, an extent that wraps, or more entries than the module accepts.
Each of those is checked on the module side too - it cannot assume the memory
it was handed came from us - but a refusal here comes with a sentence instead
of an error code.

test_redirtable decodes the result with a second implementation written from
the format description rather than by calling the builder's own helpers, so
builder and decoder cannot drift together, and pins the byte order against a
literal expected table rather than only round-tripping. 49 checks. At 2802
files - the size of the mod that motivated this - the table is 168 KB, which
is why the module reads it in place rather than copying it into an IOS heap
measured in tens of kilobytes.



The memory patcher writes through absolute console addresses - CommitWrite and
VerifyAppliedPatches cast 0x80600000 straight to a pointer - so on a host those
pages are unmapped and every write path bailed before writing. test_memcheck
said so in its own header, and "applied 0/4, 0 mismatch(es) in 0 checked
write(s)" was the most it could reach: the preflight was covered, the writing
and the re-reading were not.

A 64-bit host can just reserve those addresses. test_memapply takes 24 MB at
0x80000000 and runs the UNMODIFIED patcher against it, with the fake DOL
sections inside that region so a scan match is a console-shaped address. Bytes
land where they would land on the Wii, and the verifier re-reads what was
written. Nothing in source/ is shimmed for it.

Covered for the first time: a direct write actually landing; original= matching
and mismatching against real bytes; the preflight provably writing nothing; a
search match writing at the found address and leaving the bytes past it; the
ocarina branch encoding landing at the blr; the verifier reporting one, then
two, then no corrupted writes as bytes are changed under it; last-writer-wins
suppression on overlapping writes; and a hard-failed set leaving RAM untouched.

The ocarina slot is kept out of the verifier cases on purpose. The patcher
writes the branch as the native bytes of a u32 while the verifier expands the
same word big-endian by hand: identical on PowerPC, reversed on a
little-endian host. Correct on the target, untestable as-is off it, so the two
halves are pinned by an explicit equivalence check instead.

Both new behaviours were mutation-checked: removing the last-writer-wins skip
fails exactly the two overlap cases, and reordering the direct checks fails the
ordering cases. A test that cannot fail proves nothing.

- test_memapply: 46 checks over the write and re-read paths, on reserved memory.
- Skips itself with a note where the address space cannot be reserved.
- 146,330 automated checks, all passing.

### Boot log streaming

The log on the card is only readable after the fact, and the failures worth
diagnosing are the ones with no after: the console sits black and the card has
to be pulled to learn anything. A line already sent survives that, so the last
one received names the phase that stalled.

AppendLog is the single choke point every report already passes through, so the
stream is one hook there: written to the card first, sent second, never instead.
The address comes from riivolution/collector.txt on the card - a build with no
such file never opens a socket, and nothing is stored in the binary.

Plaintext TCP on purpose. The HTTPS path exists for covers and needs wolfSSL,
SNI and a 2 KB request buffer; a TLS handshake per line would cost more than the
phases being measured. Nothing secret is sent.

The socket calls live in a C file. <network.h> and the bundled portlibs headers
both define socklen_t and sockaddr_storage; C accepts the identical typedef
twice and C++ rejects the file outright, which is why every other socket user
here is C. Verified by control: untouched upstream https.c fails locally with
the same three errors, so the pinned container builds both or neither.

It can never delay a boot it only watches: no listener means one failed connect
and permanent silence, and a write failure drops the socket rather than retrying.

- Boot log streams line by line to a listener named in riivolution/collector.txt.
- Closed before device shutdown, so what is queued leaves while IOS still exists.
- test_netlog: 23 checks on the address parsing, the half that fails quietly.
- 146,353 automated checks, all passing.

## Changed in v3.11

- New on-demand mode: the mod's files are opened by path from inside IOS when the game asks for them.
- On-demand skips the pre-boot fragment mapping entirely.
- Directory enumeration no longer stats every file a second time.
- 146,642 automated checks, all passing.

## Changed in v3.10

A memory patch that half-applied was the worst failure this fork could produce:
the game crashed, the devices were already shut down, and the log said nothing,
so a run cost a hardware round and returned no information. The patches are now
checked as a set before any of them is written.

PreflightMemoryPatches runs the same checks as the apply path, in the same order,
read-only, while the loaded DOL is in RAM and the log is still writable. Every
skip lands in the persistent log with expected-versus-actual bytes. If any check
fails hard - no value bytes, a bad target - the whole set is held back, because a
partially applied set is what crashes without a log. Soft mismatches (original
bytes, an absent pattern) still skip individually.

The consequence of a hold-back is recorded while the log can still be written,
not at the point the decision takes effect: a held-back set with the mod's files
installed is a files-only boot, NOT an unmodified one, and the log now says which
of the two happened rather than leaving a clean boot to be misread as no mod.

Two re-reads verify the writes survived. The post-apply one runs immediately; the
pre-jump one runs last, after the code handler, 480p, Wiimmfi and the file-table
install have all written game RAM. Both are diagnostic only - nothing that late
can repair a byte, and refusing the jump would strand the tester with less than a
boot attempt gives - so the game always launches and mismatches go to USB Gecko.

- Preflight pass over the whole memory set before anything is written.
- Hard failures hold the set back; the log names the resulting boot.
- Post-apply and pre-jump re-reads, diagnostic only, never blocking the launch.
- Release notes now name the log file per game, as the loader actually writes it.
- 146,284 automated checks, all passing.

## Changed in v3.9

Pressing Play on a file-replacing mod while the game is set to anything other
than d2x v11 beta3 now shows a Continue/Cancel warning naming the game's IOS
slot. Only file replacement needs that build; memory-only mods and unmodded
boots are not nagged. The check reads the slot's NAND info block, so unknown
slots and non-d2x cIOS warn the same way.

- Play-click warning for non-beta3 cIOS on file-replacing mods.
- 146,270 automated checks, all passing.

## Changed in v3.8

Startup cost is not a bug in this design, it is the design. Riivolution's
dipmodule opens mod files by path on demand inside IOS, so it starts instantly
and pays per read. This fork relocates every mod file into a synthetic disc
region, which means enumerating every file, walking each one's FAT cluster
chain, building the fragment table and rebuilding the file table before the game
can start - all of it scaling with the size of the mod. On a 2802-file, 178 MB
total conversion that is minutes, and a tester never got far enough to report
anything else.

The design stays - it is what lets this work on stock d2x with no custom IOS,
and keeps the loader's cheats, aspect and 480p - but the duplicated work goes.

- `FsDirLister::List` memoises by (directory, recursive). Every `<folder>` rule
  was listed twice per boot with identical arguments, once by `ListModFiles` for
  placement and once by `BuildRedirects` for matching, each an opendir plus a
  stat per entry. Cleared in `SetBootContext`, because the card can be swapped
  between launches.
- The per-file first/last read-back samples up to 128 files (`MAX_SAMPLED`)
  instead of all of them - 5578 disc reads on Starshine. What the check catches
  is systematic: wrong drive, wrong LBA base, a dispatch that did not take. Those
  miss every file, so a sample finds them. First and last are always read and
  tail-recovered files stay unconditional.
- `riivolution/verify.txt` restores stride 1 and the whole-mod read-back for when
  one specific file is in doubt.

## Changed in v3.7

Each run has been reaching a later step than the one before it - "game partition
opened", then "matched: 687 replacement(s), 2115 addition(s)". That is the shape
of work that is progressing slowly, not work that is wedged, and the only reason
it was ever in doubt is that nothing measured time.

- `LogStep` stamps each line with milliseconds since the first step. The clock
  starts at the first step and resets in `SetBootContext`.
- `RIIVO_TIME_BUDGET_MS` (five minutes) bounds the file half. `RiivoDeadlinePassed()`
  is checked where bailing out is free - after the listing, before anything has
  been changed - and again at the activation gate. Over budget, the fragment list
  is left alone and the game boots unmodified.
- The budget is a safety net, not a performance policy. Code that is stuck cannot
  notice it is stuck, but code that checks a deadline at a phase boundary can, and
  a mod too big to finish inside it is better off booting unmodified than leaving
  someone staring at a black screen with no way to tell.

## Changed in v3.6

With the progress window off, Starshine listed all 2802 files, mapped 2789 of
them to 2791 fragments, handed the list over and opened the partition - then the
log stopped. So the loading bar was not the cause, and the next phase,
`PrepareFileRedirects`, was the remaining blind spot: it logged nothing until it
was completely finished.

That phase is not cheap. It re-walks every `<folder>` rule over the card a second
time in `BuildRedirects`, reads and rebuilds the file table, checks the first and
last bytes of every placed file through the cIOS, and then reads the entire mod
back and compares it against the card - 128,547,465 bytes on Starshine, so about
256 MB of traffic counting both sides. A tester watching a black screen has no
way to tell that from a hang, and reasonably gives up.

- `deepVerify`, set by `riivolution/verify.txt`, gates the large-read pass. It is
  diagnosis, not a gate: it proved what it was written to prove, and the per-file
  first/last check that stays on already establishes that every fragment maps
  where the table says it does.
- `LogStep` now covers reading the game's table, matching the mod against it
  (with the replacement/addition counts), the hook check, and the end of the file
  work.

## Changed in v3.5

The Riivolution page offered every XML on the card for every game - a Galaxy
disc cycled past Newer SMBW - because `ScanXmlFiles` matched on the `.xml`
extension and nothing else. `IsValidForGame` existed but was only used to LABEL
an already-chosen file as "other game".

- `ScanXmlFiles` parses each candidate and keeps only those whose `<id>` names
  this game. The check has to read the file: the game id is inside it, not in
  the name.
- Each kept XML carries its device and the mod's own name (the first `<section>`
  with one), gathered during the scan so drawing never re-reads the card.
- Layout. `GuiOptionBrowser` runs here with `staticValues=true`, which pins every
  value to `optionValLen` - 100px - while a row whose value is exactly one space
  gets its NAME drawn at `fullWidth`. So the two strings worth reading, the mod
  name and the path, are now full-width name rows, and the values that remain
  are short enough to fit: ON/OFF/None, and SD or USB 1.
- `HeaderRows()` is the single source for how many rows sit above the `<option>`
  rows, so the click handler and both builders cannot disagree.

## Changed in v3.4

v2.9 listed SuperMarioGravity_Demo and booted it end to end. Since then two
consoles have stopped partway through that same listing phase and returned to
the Homebrew Channel. Exactly two things were added that run inside that loop:
the per-`<folder>` log lines (v3.0) and the progress window (v3.1). The progress
window is not a passive indicator - it builds a GUI dialog on a second thread,
calls `HaltGui`/`ResumeGui`, sets `mainWindow` to STATE_DISABLED, and
`ProgressStop` spins on `LWP_ThreadIsSuspended` - all while the main thread is
inside libfat enumerating thousands of files.

- `verboseListing`, set by `riivolution/loadingbar.txt`, gates both. Off by
  default the listing phase is as quiet as it was in v2.9 and only its start and
  end are logged; the phase-boundary steps sit outside the loop and are
  unaffected.
- This is a bisect, not a diagnosis: one build, two launches, and the answer is
  whichever way round it fails.

## Changed in v3.3

Two testers' consoles returned to the Homebrew Channel instead of booting. Both
logs stop partway through the file listing, and one ends in 88 zero bytes - a
write whose data never reached the card. The Homebrew Channel is reachable from
here by exactly one route: `SetupDisc` returns a negative value and `BootGame`
answers it with `Sys_BackToLoader()`. Both calls that can return it -
`set_frag_list` and `Disc_Open` - run after the listing, so the listing finished
and the later log lines simply never landed.

- `RevertFragList()` puts the game's own fragment list back and stands the file
  work down. `SetupDisc` calls it when `set_frag_list` fails and retries once
  with the original list. Registering the enlarged list was the only step in
  this feature whose failure ended the boot outright; everything else already
  degrades to booting the game unmodified.
- The failing return code is printed before the retry.

## Changed in v3.2

Starshine GLE completed every check the loader can make - 2789 of 2789 files
read back through the cIOS, 128,547,465 bytes compared, 6050 FST paths walked,
the table installed - and still black-screened. The failure is therefore
somewhere the log cannot reach, and two very different failures were producing
the same black screen.

- `InstallPendingFst()` does the copy into MEM1 last, immediately before
  `Disc_JumpToEntrypoint`, instead of inside `ReportFstPlacement`. The table
  lands at the top of MEM1 (measured: `817c8100..817fffe9`) which is also inside
  the loader's own heap (`81581000..817feff0`), and `ShutDownDevices`,
  `gamepatches` and the mod's memory patches all still run after the old install
  point. The placement is still decided where it was - it needs the boot-info
  block the apploader fills in - but only the placement is kept.
- `Disc_JumpToEntrypoint` paints the framebuffer white after
  `__exception_closeall()`. `Disc_SetVMode()` at the top of that function is what
  blanks the display, so black currently means either "died before the jump" or
  "jumped into a game that never reached its own video init". White is set once
  threads are dead, so nothing repaints over it, and a game that boots overwrites
  it as soon as it configures video. The mode and framebuffer are captured before
  `__IOS_ShutdownSubsystems()`, because `VIDEO_GetPreferredMode` reads SYSCONF.

## Changed in v3.1

The step log from v3.0 says afterwards where a boot stopped; it does nothing for
the person sitting in front of a black screen wondering whether to power off. The
GUI threads are still running through all of this - `ExitGUIThreads()` fires only
on Wii U - so the loader's own progress window was available the whole time.

- `ProgressGuard` drives `StartProgress`/`ShowProgress`/`ProgressStop` across the
  four slow phases: reading the mod's folders, mapping its files, checking them
  back through the hook, and verifying large reads. RAII, because
  `PrepareFragList` has a dozen early returns and a window left standing would
  sit on top of the game.
- `ListModFiles` fills the bar per `<folder>` rule; `AppendModFragments` takes a
  progress callback and fills it per file.

## Changed in v3.0

Starshine GLE on IOS252 stopped after the "Loader patch settings" block and left a
black screen. Nothing between `SetupDisc` and the first report in
`PrepareFileRedirects` wrote anything, so a mod that spends minutes enumerating
files, one that exhausts the heap, and one that genuinely hangs all produced the
same truncated log. There was no way to tell them apart after the fact.

- `LogStep` writes one line per completed boot step, flushed immediately, with the
  free MEM2 figure beside it. If the log stops inside that section, the step after
  the last line is the one that did not finish.
- Steps cover the fragment list being retained, the partition lookup, the mod
  device, file listing, placement, fragment mapping, the handover to the cIOS,
  `Disc_Open` and the partition open.
- `ListModFiles` takes an optional progress callback and reports each `<folder>`
  rule as it starts listing it, with the running file count. A rule that resolves
  somewhere unintended and a rule that is merely slow look identical from outside;
  this names the rule and shows whether the count is still climbing.
- The loader's own steps between `PrepareFragList` and `PrepareFileRedirects` are
  logged through `Riivo::LogBootStep`, after the card is remounted so the append
  has somewhere to go.

## Changed in v2.9

A second console, running d2x v11 beta1 instead of beta3, found the dispatch
correctly at `93800bb0` and then refused: `hook storage head not found`. Its plugin
sits 0x20 lower than the one the hook was written against, and the storage was the
last thing in `BuildDiHook` still located by a fixed delta (`site - 0x984`).

- Storage is now derived, like everything else in that function: walk the read
  worker's own calls, take the one whose head matches the bit0 reader and that
  nothing else calls, refuse if there is not exactly one. No offset survives.
- Verified against the real beta3 module: the search returns `9380024c`, the same
  address the delta gave, so the change is behaviour-preserving where it worked.
- Three new host tests relocate the storage, remove it, and duplicate it. The
  relocation test crashes the old delta-based build rather than passing it, so it
  is not a test that agrees with whatever the code happens to do.
- A hook refusal now calls `WriteProbeDumps` itself. The probe keeps the windows it
  chose whether or not it wrote them, so a build this code cannot read comes home
  with its bytes in the same round instead of costing another.
- Release notes now state the same-drive requirement: the cIOS is handed one device
  number for the whole fragment list, so a mod on SD with a game on USB is refused.
- 146,237 automated checks, all passing (was 146,232).

## Changed in v2.8

v2.7 proved the loader's half works: all 2088 mod files read back correctly at 6 GiB
with the disc still single-layer, which the stock reader cannot serve, so the hook is
installed and working. The console still came up black. This build closes the three
things that were never tested.

- Large-read verification. Every previous check was 32 bytes at each end of a file.
  The game reads whole archives. Full 128 KiB-chunk verification now runs over the 16
  largest files and every internally fragmented file, plus an explicit read across each
  internal fragment boundary. Each request is logged and flushed to disk before IOS is
  called, so a hang leaves its offset, length and filename as the last line.
- Independent FST walk. The finished table is parsed by a reader that shares nothing
  with the builder, and every placed path is resolved from the root and checked against
  its expected offset and length. The install is refused if the walk disagrees.
- Loader-patch collision guard. `<memory>` patch ranges are registered before any of
  the loader's own patchers run. The Gecko code handler is skipped entirely when the
  mod owns `80001000..80003000` - the Spectral mod puts a 2308-byte loader at
  `80001800` and branches into it - and the width and 480p trampolines are range-checked
  before writing. A conflict that only appears once a search patch resolves refuses the
  launch rather than running with a half-overwritten handler.
- `<memory valuefile=>` patches are measured off the file at configure time, while the
  card is still mounted, so they are protected like inline ones.
- The log records the loader's resolved patch settings and the effective handler policy.
- IOS dumps are opt-in via `riivolution/dumpios.txt`. They are written before the patch
  and never showed it; the write is already byte-verified against an uncached read-back.
- The hand-over section no longer claims the loader is finished at that point. Device
  shutdown, loader patches, mod memory patches and the jump all follow it.
- 146,232 automated checks, all passing (was 137,854).

## Changed in v2.7

Diagnostic build. The v2.6 log showed the whole file pipeline working on hardware -
hook applied, 2091 fragments registered, layer checks preserved, patches applied -
and the console still came up black. This build narrows what is left.

- Read-back stride dropped to 1: all 2088 placed files are checked, not 67 of them.
- Read-back failures accumulate and are reported together (first 24 named, plus a
  count), instead of aborting on the first one. One hardware round now names every
  bad file rather than one.
- New final log section, `Handing over to the game`, recording the apploader's entry
  point and `SYS_GetArenaLo/Hi` immediately before control leaves the loader. This is
  the last line the log can ever carry: `ShutDownDevices()` unmounts the card the
  moment `BootPartition` returns.
- A marker file, `<device>:/riivolution/nomempatch.txt`, installs the mod's files but
  skips its `<memory>` patches. That halfway state is one the file/patch interlock
  normally forbids; it exists so a fault in the files or the rebuilt table can be
  told apart from one in the 503 memory patches. The marker is read in
  `SetBootContext`, while the card is still mounted.
- 137,854 automated checks, all passing (unchanged: these are runtime diagnostics,
  not new pure logic).

## Changed in v2.6

- Fixed: the read hook is re-derived against the tester's actual cIOS; the old pattern never matched it.
- The redirect now calls the frag-mode reader with the console's register contract.
- Fragment-code anchors are logged, not dumped; the read path lives in DIPP.
- 137,854 automated checks, all passing.

## Changed in v2.5

- The console now dumps the fragment-code and stock-DI windows the read path lives in.
- Loader-owned addresses are excluded by the arena bound instead of the pattern window.
- Overlapping dump windows are merged and capped at six, each labelled by its anchor.
- 137,827 automated checks, all passing.

## Changed in v2.4

- Fixed: the console scan matched our own copy of the search pattern and mistook it for the cIOS.
- The scan now finds the cIOS module first and only searches for the read routine inside it.
- The log prints our image address, the arena bounds, and how each candidate was classified.
- The memory dump is now 128 KB per candidate module instead of 32 KB around a match, and a dump is still taken when no candidate is found.
- 137,804 automated checks, all passing.

## Changed in v2.3

- Fixed: mod files whose size is not a whole number of sectors were refused as
  unmappable. The missing last sector is now read from the next cluster instead,
  and the log says how many files that covered.
- Every file recovered that way is read back through the console no matter the
  mod size; the sampling shortcut does not apply to them.
- 137,775 automated checks, all passing.

## Changed in v2.2

- When mod files cannot be mapped, the log now lists every failing file (up to 16 shown,
  plus a count of any beyond), each with its size and its expected vs reported sectors.
- When fewer sectors are reported than a file needs, the log now records whether the
  missing bytes still read back, distinguishing a short file on the card from driver
  under-reporting.
- The game/mod drive and filesystem lines now print when mapping is refused too.

## Changed in v2.1

- When a mod file cannot be mapped, the log now says why, and gives the file's size, the
  sectors expected of it, the sectors the drive actually reported, and the run that failed.
- The log now says how many of the mod's files were mapped and how many failed, not just
  the first one that did.
- Fixed: the refusal line printed a bare file path in the middle of a sentence.
- Fixed: an empty file in a mod made the rebuilt file table refuse itself.
- The heap readout no longer prints "0 MB" when the game leaves arena low unset; it says so
  and shows how much of the 1 MB allowance the table used.
- Mods over 256 files now read back a sample plus both ends instead of every file, which
  removes a long black screen before launch.

## Changed in v2.0

- The four-byte patch is replaced by a small routine installed in memory, which routes only
  reads inside its own address window to the mod and leaves every other read on the stock
  path.
- Mods can now be up to 2 GB. The previous limit was about 14 MB.
- A game whose own data crosses the 4 GiB line is no longer refused.
- The routine verifies the surrounding cIOS code, its calling convention and its own
  scratch space before writing anything, and rolls both writes back if either does not
  stick.
- Every mod file is now read back through the console, first and last bytes, instead of
  only the first file.
- After installation the loader confirms that the reads a game uses to tell a disc from a
  copy still fail, and refuses to continue if any of them succeeds.
- A mod file that is only partly locatable on the card is now refused instead of leaving a
  gap that reads as zeros.
- The fragment list is put back exactly as it was whenever setup is abandoned part-way.
- Dual-layer games are still refused.
- 137,686 automated checks, all passing.

## Changed in v1.9

- Fixed: the mod's files were located using the *game's* partition instead of their own,
  so on a different drive or partition the console read the right sector numbers off the
  wrong disk and got noise.
- The mod and the game now have to be on the same drive, and this is checked and refused
  before launch instead of failing silently.
- The log now names the drive the game is read from, the drive the mod is on, and both
  filesystems.
- When the read-back check fails, the log now shows which sector the offset resolved to.
- The cIOS dump is now taken around the patch site instead of 280 KB away from it.
- The room available for a mod is smaller than v1.8 claimed: it is the gap between the end
  of the backup image and the single-layer limit, about 14 MB on a full-size backup.

## Changed in v1.8

- Fixed: the mod's own fragments were being used as the floor the mod had to clear, so
  every file it placed was then refused.
- Fixed: adding the mod's fragments overwrote the disc's declared size. It is now put
  back afterwards.
- The virtual disc is never enlarged and the console is never told a single-layer game is
  dual-layer.
- A mod that does not fit below the single-layer limit is now refused before launch
  instead of on the console. That is about 385 MB of mod on a typical backup.
- A file the loader could not locate on your card now stops the whole thing rather than
  leaving a gap.
- 2334 automated checks, all passing.

## Changed in v1.7

- Fixed: "The cIOS refused the extended list (-128)". d2x blocks the fragment ioctl once a
  title is running, and opening the game partition starts one. The mod's fragments are now
  worked out and handed over in `SetupDisc`, in the same call the loader already makes.
- The rebuilt file table is now made to agree with that placement instead of choosing its
  own.
- Fixed: the mod's files were being measured in the wrong directory, so a mod that needed
  a larger disc could fail to get one.
- Logs are now per game: `usbloadergx_riivo_<GAMEID>.log`, and the cIOS dump alongside it.
- 16 more automated checks - 2330 total, all passing.

## Changed in v1.6

- Fixed: Error #001 / anti-piracy screen. The virtual disc was enlarged to 8.5 GB on every
  launch, which made reads past the end of the disc succeed instead of fail.
- The mod is now placed above where the game's data actually ends, not above the disc's
  declared size, so it usually fits without enlarging anything at all.
- When enlarging is unavoidable, the disc now grows only as far as the mod reaches.
- The log reports whether the disc was enlarged, and by how much.
- 9 more automated checks - 2323 total, all passing.

## Changed in v1.5

- Fixed: "Could not tell which partition the game is on". The lookup ran after `SetupDisc`
  had unmounted and remounted SD, which destroys the partition object it needs. It now
  happens before the unmount.
- The patch site was found and applied on the tester's console, so the remaining work is
  the fragment list itself.

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
