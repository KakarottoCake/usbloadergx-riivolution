// Probe self-exclusion: the MEM2 scan once matched our own .rodata copy of
// the dispatch pattern and the hook went into our own image. Classification
// now keys module windows off ceiling pairs plus a nearby hook marker, with
// our own copy excluded by address. The scan itself needs hardware; everything
// asserted here does not.
#include <stdio.h>
#include <map>
#include <string>
#include <vector>
#include "riivo/RiivoProbeClassify.hpp"
using namespace Riivo;

static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}

static std::map<u32, u32> g_img;
static u32 ReadImg(u32 addr, void *ctx) {
    (void) ctx;
    std::map<u32, u32>::const_iterator it = g_img.find(addr);
    return it == g_img.end() ? 0 : it->second;
}

static const u32 kRangeLo = 0x90C00000;
static const u32 kRangeHi = 0x94000000;
// Mirrors the real tester log: our copy high in MEM2, the module above it.
static const u32 kSelf     = 0x93328198;
static const u32 kOurPair  = 0x9336DC20;
static const u32 kOurThunk = 0x9336DB84;
static const u32 kModPair  = 0x93800A1C;
static const u32 kModThunk = 0x93800930;

static void BuildRealLog(void) {
    g_img.clear();
    g_img[kOurPair] = PROBE_DVD5;
    g_img[kOurPair + 4] = PROBE_DVD9;
    g_img[kOurThunk] = PROBE_THUNK;
    g_img[kModPair] = PROBE_DVD5;
    g_img[kModPair + 4] = PROBE_DVD9;
    g_img[kModThunk] = PROBE_THUNK;
}

static int Candidates(const std::vector<DiModule> &mods) {
    int n = 0;
    for (size_t i = 0; i < mods.size(); ++i)
        if (!mods[i].ours && mods[i].thunkAddr != 0)
            ++n;
    return n;
}

int main() {
    printf("1. the loader window excludes our own pair\n");
    {
        BuildRealLog();
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(kOurPair);
        dvd5.push_back(kModPair);
        thunk.push_back(kOurThunk);
        thunk.push_back(kModThunk);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 2, "both pairs found");
        ck(mods[0].pairAddr == kOurPair && mods[0].ours, "own pair classified ours");
        ck(mods[0].thunkAddr == kOurThunk, "own thunk attached, not decisive");
        ck(mods[1].pairAddr == kModPair && !mods[1].ours, "module pair kept");
        ck(mods[1].thunkAddr == kModThunk, "module thunk attached");
        ck(Candidates(mods) == 1, "exactly one survivor");
        ck(BestModuleIndex(mods) == 1, "highest survivor chosen");
    }

    printf("2. two module-shaped regions both survive for reporting\n");
    {
        BuildRealLog();
        const u32 pair2 = 0x93900000, thunk2 = 0x938FFE00;
        g_img[pair2] = PROBE_DVD5;
        g_img[pair2 + 4] = PROBE_DVD9;
        g_img[thunk2] = PROBE_THUNK;
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(kOurPair);
        dvd5.push_back(kModPair);
        dvd5.push_back(pair2);
        thunk.push_back(kOurThunk);
        thunk.push_back(kModThunk);
        thunk.push_back(thunk2);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 3, "three pairs found");
        ck(Candidates(mods) == 2, "two survivors, never a silent pick");
        ck(BestModuleIndex(mods) == 2, "highest of the two identified");
    }

    printf("3. pairs without a nearby thunk are not modules\n");
    {
        g_img.clear();
        const u32 lone = 0x93810000;
        g_img[lone] = PROBE_DVD5;
        g_img[lone + 4] = PROBE_DVD9;
        g_img[lone - 0x1000] = PROBE_THUNK; // too far: 4096 > 512
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(lone);
        thunk.push_back(lone - 0x1000);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 1 && mods[0].thunkAddr == 0, "far thunk not attached");
        ck(Candidates(mods) == 0 && BestModuleIndex(mods) == -1, "no survivor");
    }

    printf("4. thunk window edges pin the 512-byte bound\n");
    {
        g_img.clear();
        const u32 pair = 0x93820000;
        g_img[pair] = PROBE_DVD5;
        g_img[pair + 4] = PROBE_DVD9;
        g_img[pair - 512] = PROBE_THUNK;
        g_img[pair - 513] = PROBE_THUNK;
        g_img[pair + 0x100] = PROBE_THUNK; // after the pair: never counts
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(pair);
        thunk.push_back(pair - 513);
        thunk.push_back(pair - 512);
        thunk.push_back(pair + 0x100);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 1 && mods[0].thunkAddr == pair - 512, "nearest in-window thunk wins");
    }

    printf("5. reversed pair order still clusters\n");
    {
        g_img.clear();
        const u32 pair = 0x93830000;
        g_img[pair] = PROBE_DVD9;
        g_img[pair + 4] = PROBE_DVD5;
        g_img[pair - 0x80] = PROBE_THUNK;
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(pair + 4);
        thunk.push_back(pair - 0x80);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 1 && mods[0].pairAddr == pair, "pair found at lower word");
        ck(Candidates(mods) == 1, "reversed pair survives");
    }

    printf("6. neighbour reads stay inside the scan range\n");
    {
        g_img.clear();
        g_img[kRangeHi - 4] = PROBE_DVD5; // +4 neighbour is past the end
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(kRangeHi - 4);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.empty(), "no pair across the range end");
    }

    printf("7. loader window edges and the unknown-self case\n");
    {
        u32 lo, hi;
        LoaderWindow(kSelf, &lo, &hi);
        ck(lo == kSelf - 0x100000 && hi == kSelf + 0x100000, "window is self +-1MB");
        LoaderWindow(0, &lo, &hi);
        ck(lo == 0 && hi == 0, "unknown self excludes nothing");
        g_img.clear();
        const u32 edgeIn = 0x93428198; // kSelf + 1MB exactly
        g_img[edgeIn] = PROBE_DVD5;
        g_img[edgeIn + 4] = PROBE_DVD9;
        const u32 edgeOut = edgeIn + 8; // kept clear of edgeIn's words
        g_img[edgeOut] = PROBE_DVD5;
        g_img[edgeOut + 4] = PROBE_DVD9;
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(edgeIn);
        dvd5.push_back(edgeOut);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 2 && mods[0].ours, "window edge is inclusive");
        ck(!mods[1].ours, "one word past the window is outside");
    }

    printf("8. dump anchors on the module, clamped to the range\n");
    {
        u32 base, size;
        ModuleDumpWindow(kModPair, kRangeLo, kRangeHi, &base, &size);
        ck(base == kModPair - 0x8000 && size == 0x20000, "32K before, 96K after");
        ModuleDumpWindow(kRangeLo + 0x1000, kRangeLo, kRangeHi, &base, &size);
        ck(base == kRangeLo && size == 0x1000 + 0x18000, "clamped at range start");
        ModuleDumpWindow(kRangeHi - 0x1000, kRangeLo, kRangeHi, &base, &size);
        ck(base == kRangeHi - 0x1000 - 0x8000 && size == 0x8000 + 0x1000, "clamped at range end");
    }

    printf("9. the fallback anchor keeps a diagnostic round from coming home empty\n");
    {
        //! The real log: a candidate survives, so no fallback is wanted. The
        //! caller only asks for one when nothing survived, but the anchor must
        //! still prefer the module over our own pair if it is ever asked.
        BuildRealLog();
        std::vector<u32> dvd5, thunk, ceilings;
        dvd5.push_back(kOurPair);
        dvd5.push_back(kModPair);
        thunk.push_back(kOurThunk);
        thunk.push_back(kModThunk);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ceilings.push_back(kOurPair);
        ceilings.push_back(kOurPair + 4);
        ceilings.push_back(kModPair);
        ceilings.push_back(kModPair + 4);
        ck(FallbackAnchor(mods, ceilings, kSelf) == kModPair,
           "prefers the pair outside our image");

        //! A module-shaped region whose hook marker is too far away: it fails
        //! classification, but it is still the best thing to photograph.
        g_img.clear();
        const u32 lone = 0x93900000;
        g_img[lone] = PROBE_DVD5;
        g_img[lone + 4] = PROBE_DVD9;
        g_img[kOurPair] = PROBE_DVD5;
        g_img[kOurPair + 4] = PROBE_DVD9;
        g_img[kOurThunk] = PROBE_THUNK;
        std::vector<u32> d2, t2, c2;
        d2.push_back(kOurPair);
        d2.push_back(lone);
        t2.push_back(kOurThunk);
        std::vector<DiModule> m2;
        ClassifyModules(d2, t2, kSelf, kRangeLo, kRangeHi, ReadImg, 0, m2);
        int best = BestModuleIndex(m2);
        ck(best < 0, "nothing survives without a hook marker");
        c2.push_back(kOurPair);
        c2.push_back(lone);
        ck(FallbackAnchor(m2, c2, kSelf) == lone, "falls back to the far pair");

        //! Only our own image has ceiling hits: there is nothing to dump, and
        //! saying so beats dumping our own rodata a second time.
        std::vector<DiModule> m3;
        std::vector<u32> d3, t3, c3;
        d3.push_back(kOurPair);
        t3.push_back(kOurThunk);
        ClassifyModules(d3, t3, kSelf, kRangeLo, kRangeHi, ReadImg, 0, m3);
        c3.push_back(kOurPair);
        c3.push_back(kOurPair + 4);
        ck(m3.size() == 1 && m3[0].ours, "our pair is still ours");
        ck(FallbackAnchor(m3, c3, kSelf) == 0, "no anchor when everything is ours");

        //! With our own address unknown nothing can be excluded, so the
        //! highest hit is taken rather than refusing to dump at all.
        ck(FallbackAnchor(std::vector<DiModule>(), c3, 0) == kOurPair + 4,
           "unknown self takes the highest hit");
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
