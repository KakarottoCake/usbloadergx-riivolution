// Checked log persistence: the production append helper reports success,
// open failure, and short writes distinctly, so a record is never claimed
// to survive a storage failure.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "riivo/RiivoPersist.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
static bool ReadBack(const char *path, std::string &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[256];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
int main() {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    std::string tmp = std::string(tmpdir) + "/riivo-persist-test.log";
    remove(tmp.c_str());
    // Empty inputs succeed trivially (nothing asked, nothing failed).
    ck(AppendFileBytes(tmp.c_str(), "x", 0), "zero length succeeds");
    ck(AppendFileBytes("", "x", 1), "empty path succeeds");
    ck(AppendFileBytes(0, "x", 1), "null path succeeds");
    // Real appends land byte-exact (no newline-bearing payload here, so
    // text/binary mode cannot skew the comparison on any host).
    ck(AppendFileBytes(tmp.c_str(), "ABCDEF", 6), "first append reports success");
    ck(AppendFileBytes(tmp.c_str(), "1234", 4), "second append reports success");
    std::string back;
    ck(ReadBack(tmp.c_str(), back) && back == "ABCDEF1234", "bytes round-trip exact");
    // An unwritable path reports failure instead of silence.
    ck(!AppendFileBytes("/nonexistent-dir-xyz/riivo.log", "ABC", 3),
       "open failure reports failure");
    // A short write reports failure: /dev/full accepts the open, then
    // refuses every byte (ENOSPC). Absent on some hosts - then skip loudly.
    FILE *probe = fopen("/dev/full", "a");
    if (probe) {
        fclose(probe);
        ck(!AppendFileBytes("/dev/full", "ABCDEF", 6),
           "short write reports failure");
    } else {
        printf("SKIP: /dev/full absent, short-write path untested here\n");
    }
    remove(tmp.c_str());
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
