// Tail-cluster recovery: the vendored FAT driver reports whole clusters
// against a floor bound, so files whose data ends inside the first sector
// of a cluster come back exactly one sector short. The recovery appends the
// next sector contiguously and the read-back proves the guess.
//
// The driver model below follows the disassembly: with S = floor(size/bps)
// it emits ceil(S/spc) runs of spc sectors, then a (S, 0, 0) end marker.
#include <stdio.h>
#include <string>
#include <vector>
#include "riivo/RiivoFragBuild.hpp"
#include "libs/libfat/fatfile_frag.h"
#include "libs/libntfs/ntfsfile_frag.h"
#include "libs/libext2fs/ext2_frag.h"
using namespace Riivo;

static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}

// PART_FS_FAT in usbloader/wbfs.h (that header needs gccore.h, so it cannot
// be included on host; the value is what AppendModFragments switches on).
static const u8 kFsFat = 1;

// --- minimal platform: faithful mirrors of frag.c, plus driver stubs ---

static FragList g_list;

extern "C" int frag_append(void *f, u32 offset, u32 sector, u32 count)
{
    FragList *ff = (FragList *)f;
    if (count) {
        int n = (int)ff->num - 1;
        if (ff->num > 0
            && ff->frag[n].offset + ff->frag[n].count == offset
            && ff->frag[n].sector + ff->frag[n].count == sector)
            ff->frag[n].count += count;
        else {
            if (ff->num >= ff->maxnum)
                return -500;
            n = (int)ff->num;
            ff->frag[n].offset = offset;
            ff->frag[n].sector = sector;
            ff->frag[n].count = count;
            ff->num++;
        }
    }
    ff->size = offset + count;
    return 0;
}

extern "C" FragList *frag_list_mutable(void) { return &g_list; }

enum ModelMode { M_NORMAL, M_DROP_LAST, M_TRUNC1, M_NOTHING, M_SCATTERED, M_HOLE };
static u32 g_size, g_drvBase;
static ModelMode g_mode;
//! Non-null for the multi-file case: sizes indexed by the "mem:/N" path, so
//! one run can mix files that need recovery with one that does not.
static const u32 *g_multi;
static const u32 kBps = 512, kSpc = 64;

extern "C" int _FAT_get_fragments(const char *path, _fat_frag_append_t append, void *data)
{
    if (g_multi && path && path[5]) g_size = g_multi[path[5] - '0'];
    const u32 S = g_size / kBps; // floor bound, straight from the disassembly
    if (S > 0) {
        const u32 ncl = (S + kSpc - 1) / kSpc;
        for (u32 k = 0; k < ncl; ++k) {
            if (g_mode == M_DROP_LAST && k == ncl - 1) continue;
            if (g_mode == M_NOTHING) break;
            if (g_mode == M_HOLE && ncl >= 3 && k == 1) continue;
            u32 sector = g_drvBase + k * kSpc;
            if (g_mode == M_SCATTERED) sector = g_drvBase + ((k * 3 + 1) % ncl) * kSpc;
            u32 count = kSpc;
            if (g_mode == M_TRUNC1 && k == ncl - 1) count = kSpc - 1;
            append(data, k * kSpc, sector, count);
        }
    }
    append(data, S, 0, 0); // end marker: sector 0, count 0, carries no data
    return 0;
}

extern "C" int _NTFS_get_fragments(const char *p, _ntfs_frag_append_t a, void *d)
{ (void)p; (void)a; (void)d; return -1; }
extern "C" int _EXT2_get_fragments(const char *p, _ext2_frag_append_t a, void *d)
{ (void)p; (void)a; (void)d; return -1; }
extern "C" s32 WDVD_Read(void *b, u32 l, u64 o) { (void)b; (void)l; (void)o; return -1; }

// --- harness ---

static void reset(u32 maxnum) { g_list.num = 0; g_list.size = 0; g_list.maxnum = maxnum; }

static bool runOne(u32 size, u32 vbase, FragBuildStats &st)
{
    reset(MAX_FRAG);
    g_size = size;
    g_drvBase = 0x10000;
    PlacedFile f;
    f.offset = (u64)vbase * 512;
    f.length = size;
    f.external = "mem:/case";
    std::vector<PlacedFile> v;
    v.push_back(f);
    return AppendModFragments(v, 512, kFsFat, 0, st);
}

static u32 covered(void)
{
    u32 n = 0;
    for (u32 i = 0; i < g_list.num; ++i) n += g_list.frag[i].count;
    return n;
}

struct RealCase { const char *name; u32 size; };
static const RealCase kReal[] = {
    { "JaiSeq", 164000 }, { "astro2", 2163072 }, { "Font", 360557 },
    { "AsteroidD", 131296 }, { "HeavensDoor", 32960 }, { "LavaPlatform3", 557283 },
    { "LavaStrangeRock", 33184 }, { "ShittyCandy", 65996 }, { "Stardust", 1180065 },
    { "SunshineIsles", 852224 }, { "Effect", 851978 },
};

int main()
{
    const u32 vbase = 0xC00000; // 6 GiB window, in sectors
    char msg[128];

    printf("1. the eleven real shortfalls recover into one merged entry\n");
    const u32 nreal = sizeof(kReal) / sizeof(kReal[0]);
    for (u32 c = 0; c < nreal; ++c) {
        g_mode = M_NORMAL;
        FragBuildStats st;
        const u32 limit = (kReal[c].size + 511) / 512;
        snprintf(msg, sizeof(msg), "%s maps", kReal[c].name);
        ck(runOne(kReal[c].size, vbase, st), msg);
        snprintf(msg, sizeof(msg), "%s nothing refused", kReal[c].name);
        ck(st.failed == 0 && st.firstFailure.empty() && st.failList.empty(), msg);
        snprintf(msg, sizeof(msg), "%s covers limit in one entry", kReal[c].name);
        ck(g_list.num == 1 && covered() == limit, msg);
        snprintf(msg, sizeof(msg), "%s entry is (vbase, drvBase, limit)", kReal[c].name);
        ck(g_list.frag[0].offset == vbase && g_list.frag[0].sector == 0x10000
           && g_list.frag[0].count == limit, msg);
        snprintf(msg, sizeof(msg), "%s reported for unconditional verify", kReal[c].name);
        ck(st.extended.size() == 1 && st.extended[0] == (u64) vbase * 512, msg);
    }

    printf("2. the over-report path still clips, no recovery\n");
    {
        g_mode = M_NORMAL;
        FragBuildStats st;
        ck(runOne(11242720u, vbase, st), "over-report maps");
        ck(st.failed == 0 && st.extended.empty(), "over-report needs no recovery");
        ck(g_list.num == 1 && covered() == (11242720u + 511) / 512, "over-report clipped to limit");
    }

    printf("3. exact cluster multiples never trigger recovery\n");
    {
        const u32 sizes[] = { 32768u, 1048576u };
        for (u32 i = 0; i < 2; ++i) {
            g_mode = M_NORMAL;
            FragBuildStats st;
            ck(runOne(sizes[i], vbase, st), "cluster multiple maps");
            ck(st.extended.empty() && g_list.num == 1, "cluster multiple untouched");
        }
    }

    printf("4. sector-exact but cluster-partial sizes clip without recovery\n");
    {
        const u32 sizes[] = { 1024u, 33792u };
        for (u32 i = 0; i < 2; ++i) {
            g_mode = M_NORMAL;
            FragBuildStats st;
            ck(runOne(sizes[i], vbase, st), "sector-exact maps");
            ck(st.extended.empty() && covered() == (sizes[i] + 511) / 512, "sector-exact clipped");
        }
    }

    printf("5. a shortfall of two or more is still refused\n");
    {
        g_mode = M_TRUNC1; // JaiSeq shape, last run one sector shorter: 2 short
        FragBuildStats st;
        ck(!runOne(164000u, vbase, st), "two short refused");
        ck(st.failed == 1 && st.extended.empty(), "two short not recovered");
        ck(st.failList.size() == 1 && st.failList[0].code == FRAG_FAIL_SHORT, "two short is SHORT");
    }
    {
        g_mode = M_DROP_LAST; // whole last cluster missing: 65 short
        FragBuildStats st;
        ck(!runOne(164000u, vbase, st), "dropped cluster refused");
        ck(st.failed == 1 && st.extended.empty(), "dropped cluster not recovered");
    }

    printf("6. a file the driver maps nothing for is still refused\n");
    {
        g_mode = M_NOTHING;
        FragBuildStats st;
        ck(!runOne(164000u, vbase, st), "unmapped refused");
        ck(st.failed == 1 && st.extended.empty(), "unmapped not recovered");
    }

    printf("7. recovery works on fragmented layouts, costing one entry\n");
    {
        g_mode = M_SCATTERED; // JaiSeq, 5 clusters at permuted drive sectors
        FragBuildStats st;
        ck(runOne(164000u, vbase, st), "scattered maps");
        ck(st.extended.size() == 1 && st.extended[0] == (u64) vbase * 512,
           "scattered reported extended");
        ck(g_list.num == 5 && covered() == 321u, "scattered covers limit in 5 entries");
        ck(g_list.frag[4].offset == vbase + 256 && g_list.frag[4].sector == 0x10000u + 192
           && g_list.frag[4].count == 65,
           "recovery merges into the last run");
    }

    printf("8. holes and full tables are still refused, unrecovered\n");
    {
        g_mode = M_HOLE;
        FragBuildStats st;
        ck(!runOne(164000u, vbase, st), "hole refused");
        ck(st.failList.size() == 1 && st.failList[0].code == FRAG_FAIL_GAP, "hole is GAP");
        ck(st.extended.empty(), "hole not recovered");
    }
    {
        g_mode = M_SCATTERED;
        reset(2); // room for two entries; the file needs five
        g_size = 164000u;
        g_drvBase = 0x10000;
        PlacedFile f;
        f.offset = (u64)vbase * 512;
        f.length = 164000u;
        f.external = "mem:/case";
        std::vector<PlacedFile> v;
        v.push_back(f);
        FragBuildStats st;
        ck(!AppendModFragments(v, 512, kFsFat, 0, st), "full table refused");
        ck(st.firstFailure == "the cIOS fragment table filled up", "full table named");
    }

    printf("9. the failure list formatter\n");
    {
        g_mode = M_DROP_LAST;
        FragBuildStats st;
        runOne(164000u, vbase, st);
        ck(!DescribeFragFailList(st).empty(), "failure list names the file");
        ck(!DescribeFragFailure(st).empty(), "first failure has a sentence");
        g_mode = M_NORMAL;
        FragBuildStats ok;
        runOne(164000u, vbase, ok);
        ck(DescribeFragFailList(ok).empty(), "no list when nothing failed");
    }

    printf("10. a mixed run names the rescued files by offset, not by position\n");
    {
        //! The caller matches these against a placement list it rebuilds
        //! separately, so the key has to identify the file on its own. Three
        //! files, only the middle two short: a positional key would name the
        //! wrong ones the moment the two lists differ by a single element.
        g_mode = M_NORMAL;
        reset(MAX_FRAG);
        const u32 sizes[] = { 11242720u, 164000u, 32960u };
        std::vector<PlacedFile> v;
        u64 off = (u64) vbase * 512;
        for (u32 i = 0; i < 3; ++i) {
            PlacedFile f;
            f.offset = off;
            f.length = sizes[i];
            char p[32];
            snprintf(p, sizeof(p), "mem:/%u", i);
            f.external = p;
            v.push_back(f);
            off += (sizes[i] + 2047u) & ~2047ull;
        }
        g_multi = sizes;
        FragBuildStats st;
        ck(AppendModFragments(v, 512, kFsFat, 0, st), "mixed run maps");
        ck(st.failed == 0 && st.files == 3, "mixed run places all three");
        ck(st.extended.size() == 2, "mixed run rescues exactly two");
        ck(st.extended.size() == 2 && st.extended[0] == v[1].offset
           && st.extended[1] == v[2].offset, "rescued files named by their offsets");
        g_multi = 0;
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
