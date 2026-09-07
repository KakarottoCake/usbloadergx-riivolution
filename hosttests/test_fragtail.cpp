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

//! d2x __Frag_Get, relevant shape only: the first fragment covering the
//! sector wins; an unmapped read below the declared size is zeros, past it
//! an error. Both halves of this rule are already relied upon (sparse reads
//! below size, range errors past it) and were measured on hardware.
static int frag_lookup(u64 posBytes, u32 sectorSize, u32 declaredSectors, u64 *sectorOut)
{
    const u64 s = posBytes / sectorSize;
    for (u32 i = 0; i < g_list.num; ++i) {
        const u64 start = g_list.frag[i].offset;
        if (s >= start && s < start + g_list.frag[i].count) {
            if (sectorOut)
                *sectorOut = (u64) g_list.frag[i].sector + (s - start);
            return 1;
        }
    }
    if (posBytes < (u64) declaredSectors * sectorSize) return 0;
    return -1;
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

    printf("11. boundary requests through d2x's lookup rule\n");
    {
        //! The pre-fix planner aligned each file up to 2 KiB while
        //! RebaseAppend clips every file's fragments to its exact length,
        //! so the padding sliver between two files was unmapped. d2x serves
        //! the first fragment covering a sector, zeros below the declared
        //! size, and errors past it - so an over-read into padding errors
        //! where the reference design serves original bytes. The planner
        //! now packs at sector granularity to stop emitting this shape;
        //! these requests prove what the old tables did, and pin why the
        //! packing fix is required. Whether games issue such reads is a
        //! hardware trace, not established here.
        g_mode = M_NORMAL;
        reset(MAX_FRAG);
        const u32 s1 = 1000u, s2 = 3000u;
        const u64 o1 = (u64) vbase * 512;
        const u64 o2 = (o1 + s1 + 2047u) & ~2047ull;
        const u32 sizes[] = { s1, s2 };
        std::vector<PlacedFile> v;
        PlacedFile f1;
        f1.offset = o1; f1.length = s1; f1.external = "mem:/0";
        PlacedFile f2;
        f2.offset = o2; f2.length = s2; f2.external = "mem:/1";
        v.push_back(f1);
        v.push_back(f2);
        g_multi = sizes;
        g_drvBase = 0x10000;
        FragBuildStats st;
        ck(AppendModFragments(v, 512, kFsFat, 0, st), "boundary pair maps");
        ck(st.failed == 0 && g_list.num == 2, "one fragment per contiguous file");
        g_multi = 0;
        //! Mirror the boot's restore step: frag_append leaves size at the
        //! last mod fragment, and the boot puts the backup's own size back.
        g_list.size = 0x0117400000ULL / 512;
        u64 sector = 0;
        ck(frag_lookup(o1, 512, g_list.size, &sector) == 1
           && sector == 0x10000u, "exact file bytes hit");
        ck(frag_lookup(o1 + s1 - 1, 512, g_list.size, &sector) == 1,
           "last file byte hits");
        ck(frag_lookup(o1 + 1024, 512, g_list.size, &sector) == -1,
           "over-read into alignment padding misses past declared size");
        ck(frag_lookup(o2 - 512, 512, g_list.size, &sector) == -1,
           "padding just before the next file misses");
        ck(frag_lookup(o2, 512, g_list.size, &sector) == 1,
           "next file start hits");
        ck(frag_lookup(0x1000, 512, g_list.size, &sector) == 0,
           "unmapped below declared size is zeros");
    }

    printf("12. sector-packed layout has no gap; final extent errors past it\n");
    {
        //! The v3.18 planner packs each file at the drive sector size, so
        //! one file's fragment coverage ends exactly where the next begins.
        //! Same two files as section 11, packed: every sector from the
        //! first file's start through the last file's end must hit, and
        //! only reads past the final extent may miss.
        g_mode = M_NORMAL;
        reset(MAX_FRAG);
        const u32 s1 = 1000u, s2 = 3000u;
        const u64 o1 = (u64) vbase * 512;
        const u64 o2 = o1 + ((s1 + 511u) & ~511ull); // sector-packed, no gap
        const u32 sizes[] = { s1, s2 };
        std::vector<PlacedFile> v;
        PlacedFile f1;
        f1.offset = o1; f1.length = s1; f1.external = "mem:/0";
        PlacedFile f2;
        f2.offset = o2; f2.length = s2; f2.external = "mem:/1";
        v.push_back(f1);
        v.push_back(f2);
        g_multi = sizes;
        g_drvBase = 0x10000;
        FragBuildStats st;
        ck(AppendModFragments(v, 512, kFsFat, 0, st), "packed pair maps");
        ck(st.failed == 0 && g_list.num == 2, "packed pair costs one fragment each");
        g_multi = 0;
        g_list.size = 0x0117400000ULL / 512;
        u64 sector = 0;
        ck(frag_lookup(o1 + 1024, 512, g_list.size, &sector) == 1,
           "old gap position now hits the next file");
        //! Cross-file boundary: every sector of a span crossing from file
        //! 1's tail into file 2's head must resolve.
        bool spanOk = true;
        for (u64 p = o1 + 512; p < o1 + 2048; p += 512)
            spanOk = spanOk && frag_lookup(p, 512, g_list.size, &sector) == 1;
        ck(spanOk, "span across the file boundary resolves every sector");
        const u64 lastEnd = o2 + ((s2 + 511u) & ~511ull);
        ck(frag_lookup(lastEnd - 1, 512, g_list.size, &sector) == 1,
           "last byte of the final extent hits");
        ck(frag_lookup(lastEnd, 512, g_list.size, &sector) == -1,
           "first byte past the final extent misses past declared size");
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
