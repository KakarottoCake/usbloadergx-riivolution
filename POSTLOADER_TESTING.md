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

No IOS dumps are needed for this round. They are disabled unless
`usb1:/riivolution/dumpios.txt` exists. Any opt-in dump is taken before patching and
has a 41-byte `RIIVODIP1` ASCII header.
