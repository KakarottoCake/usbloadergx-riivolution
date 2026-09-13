// Early-FST probe helpers: digest stability and early/late agreement.
// Exercises the production RiivoEarlyFst.hpp directly (pure header, no
// console, no Wii dependency): FNV-1a digest properties over synthetic
// buffers and the agreement gate the boot log reports.
// What this does NOT prove: that the Wii software reader returns the same
// bytes as the cIOS path on hardware. That needs a console with the probe
// log lines; this pins the comparison those lines are computed with.
#include <stdio.h>
#include <string.h>
#include "riivo/RiivoEarlyFst.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
int main() {
    // Empty and NULL inputs digest as the FNV offset basis, never refused.
    ck(FstDigest(0, 0) == 2166136261u, "NULL digests as offset basis");
    ck(FstDigest((const u8 *)"", 0) == 2166136261u, "empty digests as offset basis");
    // Deterministic over the same bytes.
    {
        const u8 b[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
        ck(FstDigest(b, sizeof(b)) == FstDigest(b, sizeof(b)), "digest deterministic");
    }
    // Any byte difference changes the digest (single-bit flip).
    {
        const u8 a[] = { 0x10, 0x20, 0x30 };
        const u8 b[] = { 0x10, 0x20, 0x31 };
        ck(FstDigest(a, sizeof(a)) != FstDigest(b, sizeof(b)), "one byte flips digest");
    }
    // Order-sensitive: same bytes, swapped, differ.
    {
        const u8 a[] = { 0x01, 0x02 };
        const u8 b[] = { 0x02, 0x01 };
        ck(FstDigest(a, sizeof(a)) != FstDigest(b, sizeof(b)), "digest order-sensitive");
    }
    // Length-sensitive: trailing zero still changes the hash.
    {
        const u8 a[] = { 0x07 };
        const u8 b[] = { 0x07, 0x00 };
        ck(FstDigest(a, sizeof(a)) != FstDigest(b, sizeof(b)), "digest length-sensitive");
    }
    // A fresh identity is invalid and agrees with nothing.
    {
        EarlyFstIdentity e;
        ck(!e.valid, "fresh identity invalid");
        ck(!EarlyFstAgrees(e, 0, 0, e.digest), "invalid agrees with nothing");
    }
    // Exact match on all three agrees.
    {
        EarlyFstIdentity e;
        e.size = 1234; e.files = 56; e.digest = 0x89abcdef; e.valid = true;
        ck(EarlyFstAgrees(e, 1234, 56, 0x89abcdef), "exact match agrees");
    }
    // Each single-field mismatch refuses.
    {
        EarlyFstIdentity e;
        e.size = 1234; e.files = 56; e.digest = 0x89abcdef; e.valid = true;
        ck(!EarlyFstAgrees(e, 1235, 56, 0x89abcdef), "size mismatch refuses");
        ck(!EarlyFstAgrees(e, 1234, 57, 0x89abcdef), "file-count mismatch refuses");
        ck(!EarlyFstAgrees(e, 1234, 56, 0x89abcdee), "digest mismatch refuses");
    }
    // Usability gate: a retained plan plus agreement proceeds; every
    // other combination aborts coherently instead of switching sources.
    {
        EarlyFstIdentity e;
        e.size = 1234; e.files = 56; e.digest = 0x89abcdef; e.valid = true;
        ck(EarlyPlanUsable(true, e, 1234, 56, 0x89abcdef),
           "plan plus agreement is usable");
        ck(!EarlyPlanUsable(true, e, 1234, 56, 0x00000000),
           "plan plus disagreement is unusable (abort, no switch)");
        ck(!EarlyPlanUsable(true, e, 9999, 56, 0x89abcdef),
           "plan plus size drift is unusable");
        EarlyFstIdentity bad;
        ck(!EarlyPlanUsable(true, bad, 1234, 56, 0x89abcdef),
           "plan without a valid read is unusable");
        ck(!EarlyPlanUsable(false, e, 1234, 56, 0x89abcdef),
           "agreement without a plan is unusable");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
