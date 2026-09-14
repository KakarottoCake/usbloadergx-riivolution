/* The paged segment path, end to end: what is built, where it lands,
 * who checks it, and what happens when any step fails or is interrupted.
 * Line references are source/riivo/RiivoBoot.cpp unless noted.
 * The frozen tester package is untouched by all of this.
 */

1. Plan (pre-shutdown, card mounted)
   PrepareEarlyFst builds the authoritative plan once; the paged block
   (~4872) rebuilds the RIV1 manifest from it via BuildSegmentManifest
   (planner-owned routing/dedup/DOL-exclusion; preserved ORIGINAL runs
   materialize as GENERATED slices in GenLayout, never as gaps).
   Dual-layer images refuse here before anything is staged
   (declared >= RIIVO_DVD9_PROBE_BYTES, same floor as the fragment
   path's PlanFragRegion refusal): layer-1 diversion is unproven, so
   the safe direction is explicit refusal, not a half-layered boot.

2. Persist (checked, torn files removed)
   BuildPagedFile binds the manifest to the boot generation (epoch) and
   slices it into pages; WriteCardFile writes /riivolution/rxivtbl.bin
   (RIIVO_PAGED_TABLE_PATH) and size-readbacks it - a short file is
   removed and staging refuses (4417-4447). NOTHING large stays in
   game RAM: the reservation holds the module alone (PlanOnDemand,
   RiivoOnDemand.cpp:19-122; 27616 bytes per RESIDENT_BUDGET.md).

3. Install (unarmed by construction)
   InstallOnDemand places the module, writes params (table 0/len 0 by
   design for paged; kind/epoch/disc/part/declSize), and leaves
   armed = 0 (RiivoOnDemand.cpp:190-197). The hook goes live but every
   read MISSES at the per-read gate without initializing - no reader
   state can predate a complete contract. FST commit additionally
   requires the ARM ack (RequireModuleAck, ~4923).

4. Fill + verify, then arm (late, partition open, stock reads only)
   FillGenFile (~2409) streams ORIGINAL slices via WDVD_ReadStock into
   /riivolution/rxivgen.bin at layout offsets, reads them back and
   compares; ANY failure removes the torn file and withholds GENFILL,
   and DisarmModule clears + flushes the activation word (~2111), so
   an initialized reader can never serve past a cleared arm. ArmModule
   flips armed only after the verified fill and flushes exactly that
   PPC-owned line. Activate rebuilds the manifest late and drift-checks
   it against early staging (MANIFEST_DRIFT): changed inputs withhold
   instead of serving a stale table.

5. Serve (per read, on the DI thread)
   armed gate -> init once (invalidate inputs; mount; pg_open checks
   magic/CRC/order/identity/epoch; sr_init_paged re-checks the
   anti-shadow rule; acked = epoch, published via sync) -> covers
   pre-scan of the WHOLE request (sr_covers/rr_covers: read-only walk,
   no files opened, nothing written) -> serve chunked through the
   4 KiB bounce, or MISS whole with the caller buffer untouched.
   Listed kinds serve (EXTERNAL/GENERATED/ZERO + sector-tail pad);
   unlisted bytes stop with GAP and delegate whole - only the stock
   path can read original bytes. Counters (reads/misses/errors) are
   ARM-owned and never invalidated. Host proof: test_segread 86
   (resident serve/GAP/covers/rr parity/dispatch gate), test_page
   8202 (pager-backed serve, cross-page abutting span, ZERO vs gap,
   torn-chain EIO, epoch/identity refusals).

6. Failure, interruption, and staleness
   staging refusal     -> patchWhy surfaced, gprintf, ordinary launch
                          (disabled path unchanged; no partial mod).
   fill/verify failure -> torn file removed, DisarmModule, GENFILL
                          withhold. Verified paths in FillGenFile.
   interrupted prep    -> armed stays 0: reads MISS to stock. A later
   (power loss/abort)    boot's pg_open refuses the stale table file:
                          epoch is bumped per boot (Begin,
                          RiivoLaunchState.hpp:76-82) and baked into
                          the file header. Host-tested (stale epoch).
   mid-boot file swap  -> open/read fails -> EIO, caches dropped,
                          never partial. Host-tested (torn chain).
   OPEN HOLES (named, not hidden):
   a. Mod-file mutation between staging and serving is served live:
      the table carries no per-file content hash, only the table CRC.
      Candidate fix: snapshot sizes at stage, verify at first open.
   b. Truncation past the placed extent masks as zero tail-pad. Same
      fix as (a) covers it.
   c. Staged files are never deleted on success: stale rxivtbl/rxivgen
      linger, guarded by epoch but not cleaned. Hygiene item.
   d. A game read spanning mapped + unlisted bytes delegates WHOLE to
      stock (all-original for that request): correct bytes need
      split-serving at a layer that can read stock, which the module
      cannot (no frag replication). Safe direction, documented
      limitation; spanning WITHIN coverage serves exact.
