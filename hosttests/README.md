# Riivolution host tests

The parts of `source/riivo/` that carry real risk — reading a game's file table,
rebuilding it with a mod applied, and matching a mod folder against a disc — are written
without any console dependency. That means they can be **run** on a development machine
rather than only compiled for the Wii, which matters here: the alternative is finding out
on hardware, one reboot at a time.

```sh
sh hosttests/run.sh
```

Any C++ compiler will do (`g++` on PATH). Two shims in `shim/` stand in for
`<gctypes.h>` and `gprintf`; the sources under test are compiled unmodified.

| suite | covers | checks |
|---|---|---|
| `test_fstbuild` | `RiivoFstBuild`: round-tripping an untouched table, replacing a file with a bigger one, adding files into directories that do not exist yet, directory subtree-end indices staying consistent after an insertion, malformed and hostile tables, 64-bit offsets past the 4 GiB line | 49 |
| `test_scale` | the same rebuilder against a realistic 3920-file table — growth, timing, and that every relocated range is aligned and non-overlapping | 2159 |
| `test_pipeline` | the seam `RiivoBoot::PrepareFileRedirects` walks on console: disc FST → `BuildRedirects` → `FstBuilder` → rebuilt table, including case-insensitive matching between a mod folder and the disc | 24 |
| `test_fragplan` | Synthetic 6–8 GiB LOW_READ window, unchanged raw-disc geometry, DVD9 refusal and fragment budget | 72 |
| `test_fstinstall` | `PlaceFst`: where the rebuilt table goes in the running game's memory, how much heap that costs, and — mostly — every case where it must refuse rather than guess | 37 |

## Why the seam test exists

Additional post-loader suites:

| suite | covers | checks |
|---|---|---|
| `test_fstwalk` | Independent flat-FST lookup, corrupted subtree bounds, zero-length files, 306 new directories and 4,148 paths | 8324 |
| `test_readverify` | Single 128 KiB calls, within-file boundary reads, EOF padding, accumulated failures and malformed geometry | 38 |
| `test_patchguard` | Gecko/Syati overlap, full trampoline spans, half-open ranges and suppressed/reset state | 12 |

The first two suites each cover one module. What they cannot catch is the two disagreeing
about what a disc path looks like — `BuildRedirects` produces lower-cased paths from the
FST, and mod folders on a card rarely match the disc's capitalisation. If those ever
diverged, nothing would crash; the plan would just quietly report that nothing matched.
`test_pipeline` pins the contract between them.

## What is deliberately not covered

The hook suite assembles the Thumb source and checks the embedded bytes and relocation
profiles. This does not execute IOS or exercise real USB/SD transfers. The large-read
suite uses callbacks on the host; the same verifier calls LOW_READ on the console.
See [POSTLOADER_TESTING.md](../POSTLOADER_TESTING.md) for the paired hardware run.
