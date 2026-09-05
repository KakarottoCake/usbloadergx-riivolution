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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
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
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ceilings.push_back(kOurPair);
        ceilings.push_back(kOurPair + 4);
        ceilings.push_back(kModPair);
        ceilings.push_back(kModPair + 4);
        ck(FallbackAnchor(mods, ceilings, 0, kSelf, kRangeLo) == kModPair,
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
        ClassifyModules(d2, t2, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, m2);
        int best = BestModuleIndex(m2);
        ck(best < 0, "nothing survives without a hook marker");
        c2.push_back(kOurPair);
        c2.push_back(lone);
        ck(FallbackAnchor(m2, c2, 0, kSelf, kRangeLo) == lone, "falls back to the far pair");

        //! Only our own image has ceiling hits: there is nothing to dump, and
        //! saying so beats dumping our own rodata a second time.
        std::vector<DiModule> m3;
        std::vector<u32> d3, t3, c3;
        d3.push_back(kOurPair);
        t3.push_back(kOurThunk);
        ClassifyModules(d3, t3, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, m3);
        c3.push_back(kOurPair);
        c3.push_back(kOurPair + 4);
        ck(m3.size() == 1 && m3[0].ours, "our pair is still ours");
        ck(FallbackAnchor(m3, c3, 0, kSelf, kRangeLo) == 0, "no anchor when everything is ours");

        //! With our own address unknown nothing can be excluded, so the
        //! highest hit is taken rather than refusing to dump at all.
        ck(FallbackAnchor(std::vector<DiModule>(), c3, 0, 0, kRangeLo) == kOurPair + 4,
           "unknown self takes the highest hit");
    }

    printf("10. the arena bound decides what is ours\n");
    {
        //! The three real candidates from the v2.4 log against hi = 0x933b6f00.
        //! No thunk hits passed: arena placement alone must get all three right.
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(0x9240C060);
        dvd5.push_back(kOurPair);
        dvd5.push_back(kModPair);
        BuildRealLog();
        g_img[0x9240C060] = PROBE_DVD5;
        g_img[0x9240C060 + 4] = PROBE_DVD9;
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, 0x933B6F00, kSelf, kRangeLo, kRangeHi,
                        ReadImg, 0, mods);
        ck(mods.size() == 3, "three pairs found");
        ck(mods[0].ours && mods[1].ours && !mods[2].ours, "arena gets all three right");
        ck(Candidates(mods) == 0, "arena alone promotes nothing");
    }

    printf("11. dead arena falls back to the pattern window\n");
    {
        BuildRealLog();
        std::vector<u32> dvd5, thunk;
        dvd5.push_back(kOurPair);
        dvd5.push_back(kModPair);
        thunk.push_back(kOurThunk);
        thunk.push_back(kModThunk);
        std::vector<DiModule> mods;
        ClassifyModules(dvd5, thunk, 0, kSelf, kRangeLo, kRangeHi, ReadImg, 0, mods);
        ck(mods.size() == 2 && mods[0].ours && !mods[1].ours, "arena 0 uses the window");
        std::vector<DiModule> mods2;
        ClassifyModules(dvd5, thunk, 0x90000000, kSelf, kRangeLo, kRangeHi,
                        ReadImg, 0, mods2);
        ck(mods2.size() == 2 && mods2[0].ours && !mods2[1].ours,
           "arena below the scan uses the window");
    }

    printf("12. fragment-code anchors pair MAX_FRAG hits within 4 KB\n");
    {
        std::vector<u32> hits;
        hits.push_back(0x938B0348);
        hits.push_back(0x938B06DC);
        std::vector<DiAnchor> out;
        ClassifyFragAnchors(hits, 0x933B6F00, kSelf, kRangeLo, out);
        ck(out.size() == 1 && out[0].addr == 0x938B0348, "real pair anchors at lower word");
        ck(out[0].kind == ANCHOR_FRAG && !out[0].ours, "fragment kind, kept");

        std::vector<u32> one;
        one.push_back(0x938B0348);
        std::vector<DiAnchor> lone;
        ClassifyFragAnchors(one, 0x933B6F00, kSelf, kRangeLo, lone);
        ck(lone.empty(), "lone hit anchors nothing");

        std::vector<u32> wide;
        wide.push_back(0x938B0000);
        wide.push_back(0x938B1001); // 0x1001 apart: straddles 4 KB
        std::vector<DiAnchor> straddle;
        ClassifyFragAnchors(wide, 0x933B6F00, kSelf, kRangeLo, straddle);
        ck(straddle.empty(), "pair straddling 4 KB rejected");

        std::vector<u32> low;
        low.push_back(0x92000000);
        low.push_back(0x92000300);
        std::vector<DiAnchor> ours;
        ClassifyFragAnchors(low, 0x933B6F00, kSelf, kRangeLo, ours);
        ck(ours.size() == 1 && ours[0].ours, "pair below the arena is ours");
    }

    printf("13. stock-DI anchors pair the read-limit table within 64 bytes\n");
    {
        std::vector<u32> n9, n5;
        n9.push_back(0x939B66E4);
        n9.push_back(0x939B66FC);
        n5.push_back(0x939B66EC);
        std::vector<DiAnchor> out;
        ClassifyStockAnchors(n9, n5, 0x933B6F00, kSelf, kRangeLo, out);
        ck(out.size() == 1 && out[0].addr == 0x939B66E4, "real triple anchors at lower word");
        ck(out[0].kind == ANCHOR_STOCK && !out[0].ours, "stock kind, kept");

        std::vector<u32> far9, far5;
        far9.push_back(0x939C0000);
        far5.push_back(0x939C0041); // 65 apart
        std::vector<DiAnchor> far;
        ClassifyStockAnchors(far9, far5, 0x933B6F00, kSelf, kRangeLo, far);
        ck(far.empty(), "pair more than 64 bytes apart rejected");

        std::vector<u32> r9, r5;
        r9.push_back(0x939D0008);
        r5.push_back(0x939D0000); // stock5 first
        std::vector<DiAnchor> rev;
        ClassifyStockAnchors(r9, r5, 0x933B6F00, kSelf, kRangeLo, rev);
        ck(rev.size() == 1 && rev[0].addr == 0x939D0000, "reversed order anchors at lower word");
    }

    printf("14. overlapping windows merge, the cap holds\n");
    {
        std::vector<DumpWindow> in, merged;
        u32 skipped = 99;
        DumpWindow a, b;
        a.base = 0x938A8348; a.size = 0x20000; a.anchor = 0x938B0348; a.kind = ANCHOR_FRAG;
        b.base = 0x938B0000; b.size = 0x20000; b.anchor = 0x938B06DC; b.kind = ANCHOR_FRAG;
        in.push_back(b); // deliberately unordered
        in.push_back(a);
        MergeDumpWindows(in, merged, &skipped);
        ck(merged.size() == 1, "overlapping windows merge");
        ck(merged[0].base == 0x938A8348
           && merged[0].size == 0x938B0000u + 0x20000u - 0x938A8348u, "merged span covers both");
        ck(merged[0].anchor == 0x938B0348 && merged[0].kind == ANCHOR_FRAG,
           "label kept from lowest contributor");
        ck(skipped == 0, "nothing skipped below the cap");

        std::vector<DumpWindow> touch, mTouch;
        DumpWindow t1, t2;
        t1.base = 0x93000000; t1.size = 0x20000; t1.anchor = 0x93008000; t1.kind = ANCHOR_D2X;
        t2.base = 0x93020000; t2.size = 0x20000; t2.anchor = 0x93028000; t2.kind = ANCHOR_D2X;
        touch.push_back(t1);
        touch.push_back(t2);
        MergeDumpWindows(touch, mTouch, &skipped);
        ck(mTouch.size() == 1 && mTouch[0].size == 0x40000, "touching windows merge");

        std::vector<DumpWindow> apart, mApart;
        DumpWindow s1, s2;
        s1.base = 0x93000000; s1.size = 0x20000; s1.anchor = 0x93008000; s1.kind = ANCHOR_D2X;
        s2.base = 0x93400000; s2.size = 0x20000; s2.anchor = 0x93408000; s2.kind = ANCHOR_STOCK;
        apart.push_back(s1);
        apart.push_back(s2);
        MergeDumpWindows(apart, mApart, &skipped);
        ck(mApart.size() == 2, "disjoint windows are kept");

        std::vector<DumpWindow> many, mMany;
        for (u32 i = 0; i < 8; ++i) {
            DumpWindow w;
            w.base = 0x93000000 + i * 0x40000;
            w.size = 0x20000;
            w.anchor = w.base + 0x8000;
            w.kind = ANCHOR_D2X;
            many.push_back(w);
        }
        MergeDumpWindows(many, mMany, &skipped);
        ck(mMany.size() == 6 && skipped == 2, "cap of six with two skipped");
        ck(mMany[5].base == 0x93000000 + 5 * 0x40000, "lowest six kept in order");

        std::vector<DumpWindow> none, mNone;
        MergeDumpWindows(none, mNone, &skipped);
        ck(mNone.empty() && skipped == 0, "empty merges to empty");
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
