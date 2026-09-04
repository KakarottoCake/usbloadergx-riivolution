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

## Why the seam test exists

The first two suites each cover one module. What they cannot catch is the two disagreeing
about what a disc path looks like — `BuildRedirects` produces lower-cased paths from the
FST, and mod folders on a card rarely match the disc's capitalisation. If those ever
diverged, nothing would crash; the plan would just quietly report that nothing matched.
`test_pipeline` pins the contract between them.

## What is deliberately not covered

The IOS-side disc-read hook is ARM code that runs inside a running cIOS. It cannot be
compiled or executed here, and it is specific to the d2x revision it patches, so it is a
hardware milestone rather than something to cover with a test. See `RIIVOLUTION_PLAN.md`.
