// Large LOW_READ verification: selection, ioctl sizes, fragment-boundary
// crossings, EOF rounding, pending-read ordering, and bounded failure reports.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "riivo/RiivoReadVerify.hpp"
using namespace Riivo;

static int checks, failures;
static void ck(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}

struct Call
{
    std::string path;
    u64 fileOffset;
    u64 discOffset;
    u32 length;
    ReadVerifyKind kind;
};

struct Model
{
    std::vector<PlacedFile> files;
    std::vector<std::vector<u8> > bytes;
    std::vector<bool> failRead;
    std::vector<bool> noWrite;
    std::vector<bool> mismatch;
    std::vector<Call> calls;
    Call pending;
    bool hasPending;
    int orderingErrors;
    int compares;
    u32 sectorSize;

    Model() : hasPending(false), orderingErrors(0), compares(0), sectorSize(512) {}
};

static u8 scratch[READ_VERIFY_CHUNK] ATTRIBUTE_ALIGN(32);
static FragList fragList;

static u64 roundUp(u64 n, u32 a) { return (n + a - 1) / a * a; }

static int fileIndex(const Model &m, const char *path)
{
    for (u32 i = 0; i < m.files.size(); ++i)
        if (m.files[i].external == path) return (int)i;
    return -1;
}

static int discIndex(const Model &m, u64 offset, u32 length)
{
    for (u32 i = 0; i < m.files.size(); ++i) {
        const u64 end = m.files[i].offset
                      + roundUp(m.files[i].length, m.sectorSize);
        if (offset >= m.files[i].offset && offset + length <= end) return (int)i;
    }
    return -1;
}

static void pendingRead(void *opaque, const char *path, u64 fileOffset,
                        u64 discOffset, u32 length, ReadVerifyKind kind)
{
    Model &m = *(Model *)opaque;
    Call c;
    c.path = path; c.fileOffset = fileOffset; c.discOffset = discOffset;
    c.length = length; c.kind = kind;
    m.pending = c;
    m.hasPending = true;
    m.calls.push_back(c);
}

static s32 readDisc(void *opaque, void *buffer, u32 length, u64 discOffset)
{
    Model &m = *(Model *)opaque;
    if (!m.hasPending || m.pending.discOffset != discOffset
        || m.pending.length != length)
        ++m.orderingErrors;
    m.hasPending = false;

    const int i = discIndex(m, discOffset, length);
    if (i < 0) return -90;
    if (m.failRead[i]) return -91;
    if (m.noWrite[i]) return 0;

    const u64 at = discOffset - m.files[i].offset;
    memset(buffer, 0, length);
    if (at < m.bytes[i].size()) {
        const u32 n = (u32)((m.bytes[i].size() - at < length)
                         ? m.bytes[i].size() - at : length);
        memcpy(buffer, &m.bytes[i][(size_t)at], n);
    }
    return 0;
}

static int compareFile(void *opaque, const char *path, u64 fileOffset,
                       const void *data, u32 length)
{
    Model &m = *(Model *)opaque;
    ++m.compares;
    const int i = fileIndex(m, path);
    if (i < 0 || fileOffset + length > m.bytes[i].size()) return -1;
    if (m.mismatch[i]) return 1;
    return memcmp(data, &m.bytes[i][(size_t)fileOffset], length) ? 1 : 0;
}

static ReadVerifyCallbacks callbacks(Model &m)
{
    ReadVerifyCallbacks cb;
    cb.context = &m;
    cb.readDisc = readDisc;
    cb.compareFile = compareFile;
    cb.pendingRead = pendingRead;
    return cb;
}

static void resetFrags()
{
    memset(&fragList, 0, sizeof(fragList));
    fragList.maxnum = MAX_FRAG;
}

static void addFrag(u32 offset, u32 count)
{
    Fragment &f = fragList.frag[fragList.num++];
    f.offset = offset;
    f.sector = 0x10000 + offset * 3;
    f.count = count;
}

static void addFile(Model &m, u32 sector, u32 length, const std::string &name)
{
    PlacedFile f;
    f.offset = (u64)sector * m.sectorSize;
    f.length = length;
    f.external = name;
    m.files.push_back(f);
    std::vector<u8> data(length);
    for (u32 i = 0; i < length; ++i) data[i] = (u8)(i * 29 + sector);
    m.bytes.push_back(data);
    m.failRead.push_back(false);
    m.noWrite.push_back(false);
    m.mismatch.push_back(false);
}

static void testChunkSizes()
{
    Model m;
    addFile(m, 100, 300003, "large.arc");
    resetFrags();
    addFrag(100, (300003 + 511) / 512);
    ReadVerifyStats st;
    const bool ok = VerifyLargeReads(m.files, fragList, 512, scratch,
                                     sizeof(scratch), callbacks(m), st);
    ck(ok, "large file verifies");
    ck(m.calls.size() == 3, "large file uses three LOW_READ calls");
    ck(m.calls[0].length == 131072 && m.calls[1].length == 131072,
       "full chunks are single 128 KiB ioctls");
    ck(m.calls[2].length == 37888, "EOF request rounds up to 32 bytes");
    ck(st.totalBytes == 300003, "total counts compared file bytes");
    ck(st.largestRead == 131072, "largest successful ioctl reported");
    ck(m.orderingErrors == 0, "pending record precedes every ioctl");
}

static void testBoundaryOwnership()
{
    Model separate;
    addFile(separate, 200, 2048, "a.bin");
    addFile(separate, 204, 2048, "b.bin");
    resetFrags();
    addFrag(200, 4);
    addFrag(204, 4); // exactly the next file's start
    ReadVerifyStats st;
    ck(VerifyLargeReads(separate.files, fragList, 512, scratch,
                        sizeof(scratch), callbacks(separate), st),
       "separate files verify");
    ck(st.multiFragmentFiles == 0 && st.boundaryReads == 0,
       "inter-file transition is not an internal boundary");

    Model split;
    addFile(split, 300, 100000, "split.arc");
    resetFrags();
    addFrag(300, 100);
    addFrag(400, (100000 + 511) / 512 - 100);
    ck(VerifyLargeReads(split.files, fragList, 512, scratch,
                        sizeof(scratch), callbacks(split), st),
       "split file verifies");
    ck(st.multiFragmentFiles == 1 && st.boundaryReads == 1,
       "internal transition selects file and gets crossing read");
    const Call &cross = split.calls.back();
    ck(cross.kind == READ_VERIFY_FRAGMENT_BOUNDARY && cross.length == 65536,
       "boundary call is one 64 KiB ioctl");
    const u64 boundaryDisc = split.files[0].offset + 100 * 512;
    ck(cross.discOffset < boundaryDisc
       && cross.discOffset + cross.length > boundaryDisc,
       "64 KiB ioctl straddles fragment transition");
}

static void testLargestSelection()
{
    Model m;
    resetFrags();
    for (u32 i = 0; i < 17; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "rank-%02u.bin", i);
        const u32 length = i == 16 ? 1024 : 4096 + i;
        const u32 base = 600 + i * 20;
        addFile(m, base, length, name);
        addFrag(base, (length + 511) / 512);
    }
    ReadVerifyStats st;
    ck(VerifyLargeReads(m.files, fragList, 512, scratch,
                        sizeof(scratch), callbacks(m), st),
       "largest-file selection verifies");
    ck(st.fullFiles == 16 && st.multiFragmentFiles == 0,
       "exactly the largest 16 are selected without internal boundaries");
    bool sawSmallest = false;
    for (u32 i = 0; i < m.calls.size(); ++i)
        if (m.calls[i].path == "rank-16.bin") sawSmallest = true;
    ck(!sawSmallest, "seventeenth-largest file is not selected by rank");

    m.calls.clear();
    resetFrags();
    for (u32 i = 0; i < 16; ++i)
        addFrag((u32)(m.files[i].offset / 512),
                (m.files[i].length + 511) / 512);
    const u32 smallBase = (u32)(m.files[16].offset / 512);
    addFrag(smallBase, 1);
    addFrag(smallBase + 1, 1);
    ck(VerifyLargeReads(m.files, fragList, 512, scratch,
                        sizeof(scratch), callbacks(m), st),
       "internally fragmented file outside top 16 verifies");
    ck(st.fullFiles == 17 && st.multiFragmentFiles == 1
       && st.boundaryReads == 1,
       "internal fragmentation adds the seventeenth file to the full pass");
}

static void testSmallEofBoundary()
{
    Model m;
    addFile(m, 500, 513, "tiny.bin");
    resetFrags();
    addFrag(500, 1);
    addFrag(501, 1);
    ReadVerifyStats st;
    ck(VerifyLargeReads(m.files, fragList, 512, scratch,
                        sizeof(scratch), callbacks(m), st),
       "tiny split file verifies");
    ck(m.calls.size() == 2 && m.calls[0].length == 544
       && m.calls[1].length == 544,
       "small EOF and crossing reads round to 32 bytes");
    ck(m.calls[1].discOffset >= m.files[0].offset
       && m.calls[1].discOffset + m.calls[1].length
          <= m.files[0].offset + 1024,
       "small crossing stays in its own allocation");
    ck(st.totalBytes == 513 + (513 - m.calls[1].fileOffset),
       "padding is not counted as file content");
}

static void testFailuresAreAccumulated()
{
    Model m;
    resetFrags();
    for (u32 i = 0; i < 25; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "bad-%02u.bin", i);
        const u32 base = 1000 + i * 2;
        addFile(m, base, 1024, name);
        addFrag(base, 1);
        addFrag(base + 1, 1);
        m.mismatch[i] = true;
    }
    ReadVerifyStats st;
    ck(!VerifyLargeReads(m.files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(m), st),
       "mismatches fail the pass");
    ck(st.failedFiles == 25 && st.failedReads == 50,
       "all full and boundary failures are counted");
    ck(st.failureFiles.size() == 24,
       "failure name list is capped at 24");
    ck(st.failureFiles.front() == "bad-00.bin"
       && st.failureFiles.back() == "bad-23.bin",
       "first 24 failure names retain file order");
    ck(st.failureDetails.size() == 24,
       "first 24 unique failures retain detailed evidence");
    ck(st.failureDetails[0].path == "bad-00.bin"
       && st.failureDetails[0].discOffset == m.files[0].offset
       && st.failureDetails[0].requestLength == 1024
       && st.failureDetails[0].readResult == 0
       && st.failureDetails[0].compareResult == 1,
       "compare failure records path, offset, length, and callback returns");

    Model io;
    addFile(io, 2000, 4096, "io-error.bin");
    io.failRead[0] = true;
    resetFrags();
    addFrag(2000, 8);
    ck(!VerifyLargeReads(io.files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(io), st),
       "disc error fails the pass");
    ck(st.failedFiles == 1 && io.compares == 0,
       "failed ioctl is recorded without comparing stale data");
    ck(st.failureDetails.size() == 1
       && st.failureDetails[0].discOffset == io.files[0].offset
       && st.failureDetails[0].requestLength == 4096
       && st.failureDetails[0].readResult == -91
       && st.failureDetails[0].compareResult == READ_VERIFY_COMPARE_NOT_RUN,
       "read failure records return and marks comparison not run");

    Model silent;
    addFile(silent, 2100, 4096, "no-write.bin");
    silent.noWrite[0] = true;
    resetFrags();
    addFrag(2100, 8);
    ck(!VerifyLargeReads(silent.files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(silent), st),
       "success without writing cannot reuse the preceding buffer");
    ck(st.failureDetails.size() == 1
       && st.failureDetails[0].readResult == 0
       && st.failureDetails[0].compareResult == 1,
       "poisoned no-write success is diagnosed as a compare failure");
}

static void testInvalidScratch()
{
    Model m;
    addFile(m, 3000, 512, "one.bin");
    resetFrags();
    addFrag(3000, 1);
    ReadVerifyStats st;
    ck(!VerifyLargeReads(m.files, fragList, 512, scratch + 1,
                         sizeof(scratch) - 1, callbacks(m), st)
       && !st.fatal.empty(), "unaligned scratch is refused");
}

static void testMalformedRanges()
{
    Model m;
    addFile(m, 3100, 512, "geometry.bin");
    resetFrags();
    addFrag(3100, 1);
    ReadVerifyStats st;
    ck(!VerifyLargeReads(m.files, fragList, 1000, scratch,
                         sizeof(scratch), callbacks(m), st),
       "non-power-of-two sector geometry is refused");

    std::vector<PlacedFile> files(1);
    files[0].offset = (~0ULL / 512) * 512;
    files[0].length = 1024;
    files[0].external = "wrap.bin";
    ck(!VerifyLargeReads(files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(m), st)
       && !st.fatal.empty(), "wrapping file allocation is refused");

    files[0].offset = 0x100000000ULL * 512;
    files[0].length = 512;
    ck(!VerifyLargeReads(files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(m), st),
       "file sector outside fragment representation is refused");

    files = m.files;
    resetFrags();
    fragList.num = 1;
    fragList.frag[0].offset = 0xffffffffU;
    fragList.frag[0].sector = 1;
    fragList.frag[0].count = 2;
    ck(!VerifyLargeReads(files, fragList, 512, scratch,
                         sizeof(scratch), callbacks(m), st),
       "overflowing retained fragment is refused");
}

int main()
{
    testChunkSizes();
    testBoundaryOwnership();
    testLargestSelection();
    testSmallEofBoundary();
    testFailuresAreAccumulated();
    testInvalidScratch();
    testMalformedRanges();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
