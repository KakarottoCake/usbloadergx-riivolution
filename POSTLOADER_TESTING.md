# Post-loader test round

Use the same game, mod, device and options for both boots. Large-read verification
runs before launch and can take longer than the old first/last-byte checks.

1. Boot normally. Save `usb1:/riivolution/usbloadergx_riivo_SB4E01.log` before the
   next boot overwrites it. Record whether the game shows anything or stays black.
2. Create an empty `usb1:/riivolution/nomempatch.txt` and boot again. Save the new
   log separately, then delete the marker. It disables only mod memory patches;
   the mod's files and rebuilt table remain enabled if their checks pass.

Replace `usb1:` and the game ID if using another mount or game.

The new log includes an independent walk of original and modded FST paths, full
128 KiB-chunk verification of the 16 largest files and every internally fragmented
file, explicit internal-boundary reads, and the effective Gecko handler policy.
Every large request is logged and closed to disk before calling IOS, so a hang in
that test leaves its offset, length and filename at the end of the log.

The fragment count includes the base image and transitions between separate files.
It is not the number of boundaries crossed by an individual archive read. A report
of zero internal boundaries means that case was not exercised on this device.
The largest successful single request is reported; 128 KiB chunks do not prove a
single multi-megabyte request.

Gecko is skipped, including code-list copying and entry-point hooking, when the mod
owns its low-memory region. Width and 480p branch/trampoline writes are guarded.
Other optional patchers are not covered by a universal collision detector.

The final disk log is written before device shutdown. Its entry-point line is not
proof that subsequent memory patching or the actual jump completed. A different
result with `nomempatch.txt` narrows the cause, but a black screen in both runs does
not establish a file-table fault: a mod may require its custom code to read its assets.

What the log does prove about memory patches, read these three lines:

- The preflight table names every skip with expected-versus-actual bytes. When it
  reports hard failures the whole set is held back at entry, and the consequence
  line next to it states which boot that means: files-only when the mod's files
  are installed (not unmodified), unmodified when the mod replaces no files. Do
  not assume a clean boot means no mod was active.
- The cIOS survey names the slot that actually runs the game and verdicts it for
  file replacement. On AUTO game IOS the Play-click check can only confirm a beta3
  exists somewhere; this verdict is the one that knows the resolved slot.
- The final patch policy states the launch rule: post-apply and pre-jump re-reads
  are diagnostic-only and always launch. Their mismatches reach USB Gecko alone
  and never appear in this file; the preflight table above is the persistent
  record of what should be in RAM.

No IOS dumps are needed for this round. They are disabled unless
`usb1:/riivolution/dumpios.txt` exists. Any opt-in dump is taken before patching and
has a 41-byte `RIIVODIP1` ASCII header.
