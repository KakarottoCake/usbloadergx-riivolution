// Per-boot launch state: reset semantics, booking, and the install guard.
// Exercises the production Riivo::LaunchState type directly through
// scripted call sequences that mirror production's call order (stage,
// book, install-guard, consume/refuse/skip). This pins the state
// contract; it does not execute BootGame or InstallPendingFst, which need
// the console. A second boot (or an aborted one) must never inherit the
// previous boot's staged table, placement verdict, or file-work flags.
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
        l.fileWorkWanted = true;
        ck(l.Stage(bootABytes, sizeof(bootABytes), 0x12345678), "staging accepted");
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
        ck(l.Stage(bootABytes, sizeof(bootABytes), 1), "staging accepted");
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
        ck(l.Stage(bootBBytes, sizeof(bootBBytes), 2), "staging accepted");
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
        ck(l.Stage(bootABytes, sizeof(bootABytes), 1), "staging accepted");
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
        ck(!a.Book(p), "booking without staging refused");
        ck(!a.CanInstall(), "booking without staged bytes cannot install");
        b.Begin();
        ck(b.Stage(bootABytes, sizeof(bootABytes), 1), "staging accepted");
        ck(!b.CanInstall(), "staged bytes without booking cannot install");
    }
    // Stage() is the only way in: twice, null, empty, or after refusal
    // all refuse, and the first staging wins.
    {
        LaunchState l;
        l.Begin();
        ck(!l.Stage(0, 16, 1), "null staging refused");
        ck(!l.Stage(bootABytes, 0, 1), "empty staging refused");
        ck(l.Stage(bootABytes, sizeof(bootABytes), 0x1111), "first staging accepted");
        ck(!l.Stage(bootBBytes, sizeof(bootBBytes), 0x2222), "second staging refused");
        ck(l.stageBytes == bootABytes && l.stageCrc == 0x1111, "first staging kept");
        l.Refuse(2);
        ck(!l.Stage(bootBBytes, sizeof(bootBBytes), 0x2222), "staging after refusal refused");
    }
    // Book() refuses invalid placements and any re-booking.
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement bad;
        bad.ok = false;
        ck(!l.Book(bad), "invalid placement refused");
        ck(!l.CanInstall(), "refused booking installs nothing");
        FstPlacement p;
        p.ok = true;
        p.fstAddr = 0x817b2de0;
        ck(l.Book(p), "valid booking accepted");
        ck(!l.Book(p), "re-booking refused");
        ck(l.CanInstall(), "original booking still live");
        l.Consume();
        ck(!l.Book(p), "booking after consume refused");
    }
    // The memory hold-back predicate runs on owned fields.
    {
        LaunchState l;
        l.Begin();
        l.fileWorkWanted = true;
        ck(l.FileWorkIncomplete(), "wanted but not live holds memory back");
        FstPlacement p;
        p.ok = true;
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        l.Book(p);
        ck(!l.FileWorkIncomplete(), "live files release memory patches");
    }
    // Orchestration: aborted mod launch -> unmodified launch.
    {
        LaunchState l;
        l.Begin();
        l.fileWorkWanted = true;
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Refuse(2); // aborted (e.g. handler collision) before install
        u8 *freed = l.Begin(); // next boot: unmodified, stages nothing
        ck(freed == bootABytes, "aborted boot hands its staging back exactly once");
        ck(!l.CanInstall() && !l.FileWorkIncomplete(), "unmodified boot is clean");
    }
    // Orchestration: mod A -> mod B hands each buffer back exactly once.
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        int frees = 0;
        u8 *f = l.Begin();
        if (f == bootABytes) ++frees;
        l.Stage(bootBBytes, sizeof(bootBBytes), 2);
        l.Book(p);
        ck(l.CanInstall() && l.stageBytes == bootBBytes, "mod B installs its own table");
        f = l.Begin();
        if (f == bootBBytes) ++frees;
        f = l.Begin();
        if (f) ++frees;
        ck(frees == 2, "each staging freed exactly once, nothing dangling");
    }
    // Orchestration: repeated refusal sticks; skipped install spends the
    // booking; a second install attempt after success refuses.
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Refuse(3);
        l.Refuse(4);
        ck(!l.CanInstall() && l.installFailCode == 4, "repeated refusal sticks with latest code");
        ck(!l.Book(p), "no re-booking a refused launch");
    }
    {
        LaunchState l;
        l.Begin();
        ck(l.Stage(bootABytes, sizeof(bootABytes), 1), "staging accepted");
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Skip(); // nofstinstall bypass: booked but deliberately uninstalled
        ck(!l.CanInstall(), "second attempt after a skip refuses");
        ck(l.stage == LaunchStage::Skipped, "skip recorded, not installed");
        l.Skip();
        ck(!l.CanInstall(), "repeated skip stays spent");
    }
    // Production verify-failure route: InstallFst wrote, verification
    // failed, so the launch refuses with the verification code (4 bytes,
    // 5 pointers) instead of claiming an install that must not be trusted.
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Refuse(4); // installed bytes mismatch
        ck(!l.CanInstall(), "bytes-failed install cannot install");
        ck(l.stage == LaunchStage::Refused, "bytes failure is a refusal");
        ck(l.installFailCode == 4, "bytes failure carries code 4");
        ck(!l.Book(p), "no re-booking a verify-failed launch");
    }
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Refuse(5); // low-memory pointers mismatch
        ck(!l.CanInstall(), "pointers-failed install cannot install");
        ck(l.installFailCode == 5, "pointers failure carries code 5");
    }
    {
        LaunchState l;
        l.Begin();
        l.Stage(bootABytes, sizeof(bootABytes), 1);
        FstPlacement p;
        p.ok = true;
        l.Book(p);
        l.Consume();
        l.ReleaseStaging();
        ck(!l.CanInstall() && !l.HaveStaged(), "released staging installs nothing");
        u8 *f = l.Begin();
        ck(f == 0, "released staging is not handed back again");
    }
    // Reservation arm flag resets with everything else.
    {
        LaunchState l;
        l.Begin();
        l.smg2Armed = true;
        l.Begin();
        ck(!l.smg2Armed, "reservation arm does not survive Begin");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
