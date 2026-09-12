# USB Loader GX Riivolution: agent guidance

Shared instructions for Codex, OpenCode/Muse Spark, Claude, and Gemini.
Keep AGENTS.md, CLAUDE.md, and GEMINI.md identical; edit all three together.
Bryan's current request controls scope. This file guides work; it does not authorize releases, hardware changes, or unrelated refactors.

## Work style and coordination

- Complete the requested work. Be direct: result, evidence, remaining limitation.
- Skip mandatory triage blocks, self-scores, variant tournaments, automatic skill creation, and repeated approval questions. Use relevant installed skills when helpful; do not invent tools or require Claude-specific APIs, session variables, or model routing.
- Work solo unless delegation is requested or agreed. When delegating, assign bounded responsibilities and explicit file ownership.
- Before editing, check the repository path, branch, relevant diff, and existing work. Other agents may be active. Never reset, stash, switch branches, normalize the whole tree, or overwrite another agent's changes as setup.
- Use a separate worktree when concurrent edits require isolation. Start from the agreed revision; do not silently drop ongoing branch work by starting from a remote default.
- Commit, push, merge, tag, and publish only within Bryan's authorized scope. No automatic release after diagnostics. Do not bundle unrelated dirty files.
- Use PowerShell-compatible commands on Windows and quote paths with spaces. Run shell scripts in an actual compatible shell. Inspect line-ending noise with a scoped diff and `git diff -w --stat`; avoid repository-wide churn.

## Find the relevant code and evidence

- `source/riivo/`: XML resolution, FST construction/validation/installation, memory patches, fragment planning, and diagnostics.
- `source/usbloader/apploader.c`, `GameBooter.cpp`, `disc.c`: loading, shutdown, final patching, and handoff.
- `source/memory/mem2.cpp`: allocator behavior.
- `hosttests/`: portable production-code tests.
- `dolphin-adapter/README.md` and `BYPASSES.md`: emulator setup, experiments, and fidelity limits.
- Read the relevant latest handoff entries, then verify against code and artifacts. Historical handoffs contain superseded claims; do not promote them into current facts.
- Keep durable rules here. Put changing addresses, experiment results, build IDs, and next discriminators in the handoff.

## Wii implementation rules

- Keep loader-side preparation separate from runtime DI/IOS serving. Trace which processor owns each hook and who completes each request. Preserve the game's asynchronous completion, error, ordering, and buffer-lifetime contracts.
- Preserve stock read/decrypt/hash behavior outside explicitly registered mod ranges. Do not copy emulator-only OOB bypasses or padding into cIOS without a verified runtime contract.
- Handle reads beginning within a file, crossing extents, crossing gaps, and ending beyond a file. Specify source-offset advancement and fallback/padding semantics; test actual boundaries.
- Use checked arithmetic with explicit units: bytes, sectors, partition-relative offsets, disc offsets, and FST word offsets are different. Preserve large offsets until the documented interface conversion. Cover single- and dual-layer images; never impose a DVD5 bound on legitimate DVD9 data.
- Treat XML memory patches and file redirects as one effective mod configuration. Do not silently apply dependent memory patches after withholding files. Files-only experiments must be labeled and cannot establish full-mod compatibility.
- Check I/O results before consuming or registering loaded bytes. Refuse through an established path on failure; do not proceed with stale data.
- Executable PPC patches need data-cache writeback and instruction-cache invalidation over the modified range, with the platform's required ordering. Plain data/FST pointers need appropriate data visibility, not automatic instruction-cache invalidation. ARM/Starlet hooks require their own cache protocol.
- Respect PPC ABI, alignment, endian encoding, branch reach, and register/stack preservation. Validate original bytes and game revision before any game-specific patch.
- Prove memory ownership and lifetime from staging through actual consumption. Zeros, spare physical RAM, a passing memcpy, or a lowered arena word do not establish a reservation.
- Account for game startup clearing, heaps, BSS, stack, loader lifetime, and IOS reservations. Low memory and code caves are not universally free; mods and handlers may already occupy them.
- General pipeline only: do not introduce game-specific reservation hooks, markers, addresses, or policy gates in production. Per-title surveyed values belong in archived research, not shared machinery. Unsupported placements refuse explicitly until the general implementation handles them.
- Bound RAM, fragment counts, and runtime work. Stream payloads rather than loading entire mods into RAM. Reuse metadata where valid; measure phase times and allocation peaks before optimizing.
- Audit shutdown ordering, device lifetime, and logging separately. Diagnostic code can allocate, block, alter timing, or overwrite memory; it is not behavior-neutral by default.

## Verification and debugging

- Prefer tests calling production code over copies of its logic. Label mirrors, mocks, synthetic data, and bypassed layers.
- Run relevant host tests for changes to pure logic. Full host suite: `sh hosttests/run.sh` from the repository root in a compatible environment. Docs-only edits need consistency checks, not a Wii build.
- Build affected PPC code and validate linking for boot-path changes. Consult the current workflows for the pinned toolchain; release CI currently uses `devkitpro/devkitppc:20250527` and `make release`. A newer local syntax check is not equivalent to that build.
- Tie results to commit, binary hash, game ID/revision, XML/options, device/cIOS configuration, and marker state. Preserve manifests and maps with diagnostic artifacts when available.
- Separate evidence levels: host validation, loader read-back, installation, game consumption, title, save selection, playable level, and hardware. Passing one does not prove the next.
- A light signal proves only its code location was reached. A truncated file does not identify a crash, hang, failed append, or shutdown. A pre-jump marker does not prove game entry.
- Validate debugger controls in the exact emulator/profile before interpreting silence. Track cache visibility, JIT/interpreter behavior, DMA/HLE paths, and uninstrumented writes.
- Use one owned emulator process per experiment, unique output paths and run IDs, bounded tracing, and explicit trace-cap records. Clean up only processes you started. Check hidden dialogs before declaring a CPU dead.
- Change one discriminating variable where possible. Keep controls equivalent in effective memory patches, payload bytes, placement, and serving behavior. Missing mod initialization can invalidate a files-only reproduction.
- Record observation, interpretation, and uncertainty separately. A passing approximation cannot exclude exact-input faults; free-memory totals cannot exclude every allocator failure.

## Protect tester time

- Exhaust useful local reproduction and code checks before requesting hardware.
- Each hardware round needs a verified build, exact configuration, expected discriminator, and minimal numbered steps. Do not repeat an unchanged inconclusive run.
- Preserve known-working controls and original logs. Never ask testers to repair or wipe storage before preserving evidence.
- Keep experimental markers default-off unless explicitly requested. Avoid diagnostics that add long waits, blocking network sends, or repeated card work.
- Report what changed, what was actually tested, what remains unproven, and the next useful action. Do not claim a mod works until the stated gameplay milestone is observed.

