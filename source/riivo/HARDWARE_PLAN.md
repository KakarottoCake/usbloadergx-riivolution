/* What the first console run must prove, in the smallest order that
 * proves it. No results are claimed here - this is the procedure the
 * reviewed commit (segreader-integration, CI-pinned) is ready for.
 * The frozen tester package stays untouched throughout.
 */

Session definition (what "same state" actually means)
-----------------------------------------------------
Two HBC launches do NOT test cross-boot cache persistence: launching an
app from HBC keeps IOS only if no reload happens, and GX reloads IOS
on start (58, then the game slot) - an IOS reload reboots Starlet and
freshens its caches, which destroys exactly the residency under test.
The state that persists is narrower and precise: consecutive GAME
BOOTS inside one GX session on one cIOS slot, without a power cycle
and without an IOS reload between them (game A with mod -> return to
GX -> game B with mod). There d2x keeps running, ARM caches persist,
and a reused reservation address replays the boot-reuse paths
(writable-span discard, once-flag publication) for real. To VERIFY the
session preserved IOS: compare the install log lines across the two
boots (same slot/base reported by the storage probe, hook site found
at the same address) and confirm no IOS reload intervened (GX logs
its IOS transitions; any reload between the boots invalidates the
reuse half of the run, though each boot still validates serving).

Workload (must exercise the new backend, not just any mod)
----------------------------------------------------------
A two-file mod on one game: one partial file (ORIGINAL prefix/suffix
-> GENERATED slices, one EXTERNAL slice with a nonzero srcOffset) plus
one whole file placed with an alignment gap after the first, so the
emitted table contains EXTERNAL + GENERATED/filed + explicit ZERO runs
across a page boundary. Gameplay must READ all three: pick replacements
on a late, large, streaming asset (level geometry/audio-specific,
never the title screen) so reads demonstrably cross the tail -> gap
-> head span after startup. One same-size full-file replacement is NOT
sufficient for this run (it never touches the composition path).

Counters retrieval (how post-startup numbers actually leave the box)
--------------------------------------------------------------------
Primary: USB Gecko live log. The module counts reads/misses/errors in
ARM-owned words and echoes state/acked; the install log already prints
module address, arena movement, and bytes left to the game, and the
ack probe prints its three attempts. During play, sample the card/USB
log (gprintf) for: reads > 0 with errors == 0, misses only outside the
mod span, acked == installed epoch, state == 1 throughout.
Fallback without Gecko: return-to-loader, then dump the reservation.
Words, all as the PPC sees them (module base M from the install log):
  params      = M + RIIVO_MODULE_PARAMS_OFF (116 bytes)
  counters    = params + 96: state, reads, misses, errors, acked
Read through the UNCACHED alias and never flush that line (a PPC
writeback of a dirty counters line would clobber ARM's counts - the
layout comment states this; the dump routine must use uncached reads
only). Validity is circular in exactly one stated way: an intact dump
already evidences survival (a game that overwrote the reservation
would show garbage counters, which fails the run rather than passing
it). MODACK pre-launch proves init; reads > 0 with correct in-game
behavior proves post-startup serving; identical reservation addresses
across the two boots with correct second-boot serving proves the
reuse paths.

Pass/fail for this run
-----------------------
PASS: both boots ack (MODACK gate green pre-launch), gameplay shows
the replacements (partial slices + whole file), counters read
reads > 0 / errors == 0 / acked == epoch on both boots, second boot
correct with a reused reservation address, no freeze attributable to
the hook (any freeze with the module armed fails the run - the probe
containment argument says maintenance faults land pre-launch, so a
mid-game freeze is a different bug until proven otherwise).
FAIL (any): ack withhold without a named planner refusal, errors > 0,
stale/wrong bytes in game, reservation overwritten (garbage dump),
freeze. Each fail names the layer (plan/stage/install/serve/survive)
from the log lines in CONNECTED_PATH.md before any new test is added.
Out of scope for this run: DVD9 (refused at staging by design),
peri-slot sync survey beyond the boot slot, performance numbers.
