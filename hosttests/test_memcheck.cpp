// Memory-patch preflight: the same checks as the apply path, read-only,
// plus the hold-back policy and the apply summary.
//
// What runs here and what does not: search/ocarina scans run against stubbed
// DOL sections, and every pure function (policy, formatting, divergence)
// runs for real. Direct-RAM reads touch absolute console addresses and cannot
// run on host - only their no-RAM failure modes (no value, bad target) are
// covered; the memcmp paths are verified by PPC compile and on hardware,
// where every skip lands in the persistent preflight table.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "riivo/RiivoMemory.hpp"
using namespace Riivo;
static int checks, failed;
static void ck(bool ok,const char *name) { ++checks; if(!ok){++failed;printf("FAIL: %s\n",name);} }
static bool has(const std::string &hay,const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Fake DOL sections. Heap memory, so only relative scans are meaningful;
// absolute console addresses never work here (see above).
static u8 *g_dol[4];
static int g_dolLen[4];
static int g_dolN;
extern "C" int RiivoGetDOLCount(void) { return g_dolN; }
extern "C" u8 *RiivoGetDOLDst(int i) { return (i >= 0 && i < g_dolN) ? g_dol[i] : 0; }
extern "C" int RiivoGetDOLLen(int i) { return (i >= 0 && i < g_dolN) ? g_dolLen[i] : 0; }

static void bytes(std::vector<u8> &v,const char *s) {
    v.assign(s, s + strlen(s));
}

int main() {
    ck(EncodeOcarinaBranch(0x80001800,0x80001000) == 0x48000800,"branch encoding");

    // Section 0 carries a search pattern at 64 and an overrun-only one at 250
    // (length 260: the 8-byte pattern fits, a 16-byte write does not).
    // Section 1 carries an ocarina value at 32 with a blr at 64.
    static u8 sec0[260], sec1[256];
    memset(sec0,0xAA,sizeof(sec0));
    memset(sec1,0xBB,sizeof(sec1));
    memcpy(sec0 + 64,"SEARCHME!!",10);
    memcpy(sec0 + 250,"TAILEND!",8);
    memcpy(sec1 + 32,"HOOKME!!",8);
    u32 blr = 0x4E800020;
    memcpy(sec1 + 64,&blr,4);
    memcpy(sec1 + 252,"TAIL",4); // pattern with no blr after it
    g_dol[0] = sec0; g_dolLen[0] = sizeof(sec0);
    g_dol[1] = sec1; g_dolLen[1] = sizeof(sec1);
    g_dolN = 2;

    ResolvedPatchSet set;
    std::vector<MemOutcome> out;
    // 0: direct, no value at all (never touches RAM: value check is first).
    { ResolvedMemory m = {}; m.offset = 0x6fc348; bytes(m.value,""); set.memories.push_back(m); }
    // 1: direct, preload failed shape (never touches RAM either).
    { ResolvedMemory m = {}; m.offset = 0x6fc348; m.valuefile = "missing.arc"; set.memories.push_back(m); }
    // 2: direct, target outside RAM (returns before any RAM read).
    { ResolvedMemory m = {}; m.offset = 0x01FFFF00; bytes(m.value,"abcd"); set.memories.push_back(m); }
    // 3: search hit, fits.
    { ResolvedMemory m = {}; m.search = true; m.align = 1; bytes(m.original,"SEARCHME!!"); bytes(m.value,"WRITTEN!"); set.memories.push_back(m); }
    // 4: search miss.
    { ResolvedMemory m = {}; m.search = true; m.align = 1; bytes(m.original,"NOTHERE!!"); bytes(m.value,"WRITTEN!"); set.memories.push_back(m); }
    // 5: search overrun-only.
    { ResolvedMemory m = {}; m.search = true; m.align = 1; bytes(m.original,"TAILEND!"); bytes(m.value,"0123456789ABCDEF"); set.memories.push_back(m); }
    // 6: search, empty pattern.
    { ResolvedMemory m = {}; m.search = true; bytes(m.value,"x"); set.memories.push_back(m); }
    // 7: search, value missing.
    { ResolvedMemory m = {}; m.search = true; bytes(m.original,"SEARCHME!!"); set.memories.push_back(m); }
    // 8: search stride skips an off-stride pattern.
    { ResolvedMemory m = {}; m.search = true; m.align = 4; bytes(m.original,"E!!"); bytes(m.value,"x"); set.memories.push_back(m); }
    // 9: ocarina hit with blr after it.
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x600000; bytes(m.value,"HOOKME!!"); set.memories.push_back(m); }
    // 10: ocarina pattern absent.
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x600000; bytes(m.value,"ABSENT!!"); set.memories.push_back(m); }
    // 11: ocarina pattern with no blr after it.
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x600000; bytes(m.value,"TAIL"); set.memories.push_back(m); }
    // 12: ocarina, empty value.
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x600000; set.memories.push_back(m); }
    // 13: ocarina, branch target outside RAM.
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x02000000; bytes(m.value,"HOOKME!!"); set.memories.push_back(m); }

    ck(PreflightMemoryPatches(set,out) == 7,"seven hard failures counted");
    ck(out.size() == 14,"one outcome per patch");
    ck(out[0].check == MEM_CHECK_HARD_NO_VALUE,"direct empty value is hard");
    ck(out[1].check == MEM_CHECK_HARD_NO_VALUE,"direct failed valuefile is hard");
    ck(out[2].check == MEM_CHECK_HARD_BAD_TARGET && out[2].target == 0x81FFFF00,
       "direct outside RAM is hard, address kept");
    ck(out[3].check == MEM_CHECK_OK && out[3].target == out[3].writeAddr && out[3].writeAddr != 0,
       "search hit is ok with match address");
    ck(out[4].check == MEM_CHECK_SOFT_NOT_FOUND && out[4].wantLen == 9
       && memcmp(&out[4].want[0],"NOTHERE!!",9) == 0,"search miss names the pattern");
    ck(out[5].check == MEM_CHECK_SOFT_OVERRUN && out[5].target != 0 && out[5].writeAddr == 0,
       "overrun-only match refuses with match address");
    ck(out[6].check == MEM_CHECK_HARD_NO_PATTERN,"empty search pattern is hard");
    ck(out[7].check == MEM_CHECK_HARD_NO_VALUE,"search without value is hard");
    ck(out[8].check == MEM_CHECK_SOFT_NOT_FOUND,"stride skips off-stride pattern");
    ck(out[9].check == MEM_CHECK_OK && out[9].target == 0x80600000 && out[9].writeAddr != 0,
       "ocarina hit records target and blr slot");
    ck(out[9].writeAddr == (u32)(uintptr_t)(sec1 + 64),"ocarina blr slot is exact");
    ck(out[10].check == MEM_CHECK_SOFT_NOT_FOUND,"ocarina miss is soft");
    ck(out[11].check == MEM_CHECK_SOFT_OVERRUN,"ocarina without blr is soft");
    ck(out[12].check == MEM_CHECK_HARD_NO_PATTERN,"empty ocarina value is hard");
    ck(out[13].check == MEM_CHECK_HARD_BAD_TARGET,"ocarina outside RAM is hard");
    ck(MemPreflightHardFails(out) == 7,"hard-fail counter agrees");
    ck(out[3].writeLen == 8,"search hit records write length");
    ck(out[9].writeLen == 4,"ocarina hit records 4-byte hook length");
    ck(out[4].writeLen == 0 && out[5].writeLen == 0 && out[2].writeLen == 0,
       "skips and hard failures record no write length");

    std::string text = DescribeMemPreflight(out);
    ck(has(text,"preflight: 14 checked"),"counts line present");
    ck(has(text,"@0x80600000"),"ocarina target logged");
    // The mismatch formatter cannot run on host RAM, so feed it a hand-built
    // outcome: this is the exact shape the eight original= checks produce.
    {
        std::vector<MemOutcome> one;
        MemOutcome m = {};
        m.index = 0; m.kind = "direct"; m.target = 0x806fc348; m.writeAddr = 0x806fc348;
        m.check = MEM_CHECK_SOFT_ORIG_MISMATCH; m.wantLen = 8;
        u8 w[8] = {0x77,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        u8 g[8] = {0x77,0x01,0x02,0x03,0x04,0x05,0x06,0xFF};
        memcpy(m.want,w,8); memcpy(m.got,g,8); m.gotLen = 8;
        one.push_back(m);
        std::string t = DescribeMemPreflight(one);
        ck(has(t,"original-mismatch"),"mismatch reason logged");
        ck(has(t,"want 8: 77 01 02 03 04 05 06 07"),"expected bytes logged");
        ck(has(t,"got: 77 01 02 03 04 05 06 ff"),"actual bytes logged");
        ck(has(t,"preflight: 1 checked, 0 ok, 1 soft, 0 hard"),"mismatch counts as soft");
    }

    // Apply execution is target-only for OK writes (they go through the
    // truncated u32 addresses, exact on the 32-bit console but wild on a
    // 64-bit host). Every skip path executes for real here - no write
    // happens on a skip, so nothing dereferences - covering record-keeping,
    // counts, and agreement with preflight on identical bytes.
    ResolvedPatchSet qset;
    { ResolvedMemory m = {}; m.offset = 0x6fc348; qset.memories.push_back(m); }
    { ResolvedMemory m = {}; m.search = true; m.align = 1; bytes(m.original,"NOTHERE!!"); bytes(m.value,"WRITTEN!"); qset.memories.push_back(m); }
    { ResolvedMemory m = {}; m.search = true; m.align = 1; bytes(m.original,"TAILEND!"); bytes(m.value,"0123456789ABCDEF"); qset.memories.push_back(m); }
    { ResolvedMemory m = {}; m.ocarina = true; m.offset = 0x600000; bytes(m.value,"ABSENT!!"); qset.memories.push_back(m); }
    std::vector<MemOutcome> qpre, app;
    PreflightMemoryPatches(qset,qpre);
    int applied = ApplyMemoryPatches(qset,"usb1:",app);
    ck(applied == 0,"all skipped, none applied");
    ck(app.size() == 4,"apply records every patch");
    {
        bool agree = app.size() == qpre.size();
        for (size_t i = 0; agree && i < qpre.size(); ++i)
            agree = app[i].check == qpre[i].check
                 && app[i].target == qpre[i].target
                 && app[i].writeAddr == qpre[i].writeAddr
                 && app[i].writeLen == qpre[i].writeLen;
        ck(agree,"apply agrees with preflight on skips");
    }
    std::string sum = DescribeMemApplySummary(qpre,app,applied);
    ck(has(sum,"applied 0/4"),"summary counts");
    ck(!has(sum,"changed after preflight"),"agreement means no divergence");

    // Hand-built vectors exercise the summary paths no fake DOL can reach.
    {
        std::vector<MemOutcome> pre, ap;
        MemOutcome a = {}; a.index = 0; a.kind = "direct"; a.target = 0x80600000;
        a.writeAddr = 0x80600000; a.check = MEM_CHECK_OK; pre.push_back(a);
        MemOutcome b = a; b.check = MEM_CHECK_SOFT_ORIG_MISMATCH; ap.push_back(b);
        MemOutcome c = {}; c.index = 1; c.kind = "search"; c.check = MEM_CHECK_SOFT_NOT_FOUND; pre.push_back(c);
        MemOutcome d = c; d.check = MEM_CHECK_OK; d.writeAddr = 0x80700000; ap.push_back(d);
        std::string s = DescribeMemApplySummary(pre,ap,1);
        ck(has(s,"applied 1/2"),"hand summary counts");
        ck(has(s,"changed after preflight"),"bad-direction divergence flagged");
        std::vector<MemOutcome> empty;
        ck(VerifyAppliedPatches(set,empty,"test") == 0,"nothing applied verifies clean");
    }

    // Sequential dependency: a later patch changed by an earlier patch's
    // write names that write as the likely cause, not loader patches.
    // (VerifyAppliedPatches itself dereferences console addresses and can
    // only run on hardware; both directions of its shared overlap helper
    // run for real here.)
    {
        std::vector<MemOutcome> pre, ap;
        MemOutcome w0 = {}; w0.index = 0; w0.kind = "direct"; w0.target = 0x80600000;
        w0.writeAddr = 0x80600000; w0.writeLen = 8; w0.check = MEM_CHECK_OK;
        MemOutcome p1 = {}; p1.index = 1; p1.kind = "direct"; p1.target = 0x80600004;
        p1.writeAddr = 0x80600004; p1.writeLen = 4; p1.check = MEM_CHECK_OK;
        pre.push_back(w0); pre.push_back(p1);
        MemOutcome a0 = w0; ap.push_back(a0);
        MemOutcome a1 = p1; a1.check = MEM_CHECK_SOFT_ORIG_MISMATCH; ap.push_back(a1);
        std::string s = DescribeMemApplySummary(pre,ap,1);
        ck(has(s,"changed after preflight"),"overlap case still flags divergence");
        ck(has(s,"overlaps earlier write [0]"),"earlier overlapping write named as likely cause");
        ck(FindOverlappingWrite(ap,0,2,0x80600004,4) == 0,"helper finds earlier overlap");
        ck(FindOverlappingWrite(ap,1,2,0x80600004,4) == -1,"helper respects begin bound");
        ck(FindOverlappingWrite(ap,0,2,0x80700000,4) == -1,"helper clears distant range");
        ck(FindOverlappingWrite(ap,0,2,0,4) == -1,"helper ignores null address");
        ck(FindOverlappingWrite(ap,2,1,0x80600004,4) == -1,"helper handles empty range");
        // Forward (verifier) direction: the earlier write is superseded.
        ck(FindOverlappingWrite(pre,1,2,0x80600000,8) == 1,"helper finds later overlap");
        // Most recent wins when two earlier writes overlap the query.
        MemOutcome w2 = {}; w2.index = 2; w2.kind = "direct"; w2.target = 0x80600002;
        w2.writeAddr = 0x80600002; w2.writeLen = 2; w2.check = MEM_CHECK_OK;
        ap.push_back(w2);
        ck(FindOverlappingWrite(ap,0,3,0x80600001,2) == 2,"helper prefers most recent overlap");
    }
    // Same divergence with no earlier write nearby: no attribution line.
    {
        std::vector<MemOutcome> pre, ap;
        MemOutcome w0 = {}; w0.index = 0; w0.kind = "direct"; w0.target = 0x80600000;
        w0.writeAddr = 0x80600000; w0.writeLen = 8; w0.check = MEM_CHECK_OK;
        MemOutcome p1 = {}; p1.index = 1; p1.kind = "direct"; p1.target = 0x80700000;
        p1.writeAddr = 0x80700000; p1.writeLen = 4; p1.check = MEM_CHECK_OK;
        pre.push_back(w0); pre.push_back(p1);
        MemOutcome a0 = w0; ap.push_back(a0);
        MemOutcome a1 = p1; a1.check = MEM_CHECK_SOFT_ORIG_MISMATCH; ap.push_back(a1);
        std::string s = DescribeMemApplySummary(pre,ap,1);
        ck(has(s,"changed after preflight"),"distant divergence still flagged");
        ck(!has(s,"overlaps earlier write"),"no attribution without an overlapping write");
    }
    printf("%d checks, %d failures\n",checks,failed); return failed?1:0;
}
