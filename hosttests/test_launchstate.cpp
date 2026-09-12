// Per-boot launch state: reset semantics, booking, and the install guard.
// Exercises the production Riivo::LaunchState type directly: a second boot
// (or an aborted one) must never inherit the previous boot's staged table,
// placement verdict, or file-work flags.
#include <stdio.h>
#include <string.h>
#include "riivo/RiivoLaunchState.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
static u8 bootABytes[16], bootBBytes[16];
int main() {
    // Fresh object installs nothing.
    {
        LaunchState l;
        ck(!l.CanInstall(), "fresh: cannot install");
        ck(!l.HaveStaged(), "fresh: nothing staged");
        ck(l.stage == LaunchStage::None, "fresh: stage None");
    }
    // One full boot: stage, book, install.
    {
        LaunchState l;
        u8 *old = l.Begin();
        ck(old == 0, "first Begin: no previous staging to free");
        ck(l.generation == 1, "first Begin: generation 1");
        l.stageBytes = bootABytes;
        l.stageSize = sizeof(bootABytes);
        l.stageCrc = 0x12345678;
        l.fileWorkWanted = true;
        l.NoteStaged();
        ck(!l.CanInstall(), "staged but not booked: cannot install");
        FstPlacement p;
        p.ok = true;
        p.fstAddr = 0x817b2de0;
        l.Book(p);
        ck(l.CanInstall(), "booked same-boot table: can install");
        ck(l.HaveStaged(), "booked table counts as staged");
        ck(l.fileWorkLive, "booking marks file work live");
        l.Consume();
        ck(!l.CanInstall(), "consumed booking cannot install again");
        ck(!l.HaveStaged(), "consumed booking counts as unstaged");
    }
    // Second boot inherits nothing: the catastrophic shape is a stale
    // table plus a live-looking booking under a new boot.
    {
        LaunchState l;
        l.Begin();
        l.stageBytes = bootABytes;
        l.stageSize = sizeof(bootABytes);
        l.NoteStaged();
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        ck(l.CanInstall(), "boot A installs");
        // Boot B begins without staging anything (memory-only mod).
        // The caller frees the returned buffer exactly once.
        u8 *freed = l.Begin();
        ck(freed == bootABytes, "Begin hands back the old staging buffer");
        ck(l.generation == 2, "generation advanced");
        ck(!l.CanInstall(), "unstaged second boot cannot install");
        ck(!l.HaveStaged(), "unstaged second boot counts as unstaged");
        ck(!l.fileWorkWanted && !l.fileWorkLive, "verdicts cleared");
        ck(l.stage == LaunchStage::None, "stage restarted");
        ck(l.installFailCode == 0, "fail code cleared");
        ck(l.plannedFstSize == 0, "planned size cleared");
    }
    // A table staged under generation 1 must not install under
    // generation 2 even if Begin were somehow skipped: the stamp
    // disagrees, so CanInstall refuses.
    {
        LaunchState l;
        l.Begin();
        l.stageBytes = bootBBytes;
        l.stageSize = sizeof(bootBBytes);
        l.NoteStaged();
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        ck(l.CanInstall(), "same-generation booking installs");
        l.generation = 99; // simulate a missed reset around a live booking
        ck(!l.CanInstall(), "cross-generation booking refused");
    }
    // Refusal sticks and carries its code.
    {
        LaunchState l;
        l.Begin();
        l.stageBytes = bootABytes;
        l.stageSize = sizeof(bootABytes);
        l.NoteStaged();
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Refuse(3);
        ck(!l.CanInstall(), "refused booking cannot install");
        ck(l.installFailCode == 3, "refusal carries the blink code");
        ck(l.stage == LaunchStage::Refused, "stage records refusal");
    }
    // Booking without bytes, or bytes without booking, installs nothing.
    {
        LaunchState a, b;
        a.Begin();
        FstPlacement p;
        p.ok = true;
        a.Book(p);
        ck(!a.CanInstall(), "booking without staged bytes cannot install");
        b.Begin();
        b.stageBytes = bootABytes;
        b.stageSize = sizeof(bootABytes);
        b.NoteStaged();
        ck(!b.CanInstall(), "staged bytes without booking cannot install");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
