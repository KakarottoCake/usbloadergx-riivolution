#include "RiivoDiHook.hpp"
#include "RiivoDiPatch.hpp"
#include <string.h>

namespace Riivo {
static u16 Read16(const u8 *p) { return (u16(p[0]) << 8) | p[1]; }
static void Write16(u8 *p, u16 n) { p[0] = n >> 8; p[1] = n; }
static u32 Read32(const u8 *p) { return (u32(Read16(p)) << 16) | Read16(p + 2); }
static void Write32(u8 *p, u32 n) { Write16(p, n >> 16); Write16(p + 2, n); }
static std::vector<u8> Hex(const char *s) {
    std::vector<u8> out;
    for (; *s; s += 2) {
        unsigned a = s[0] <= '9' ? s[0] - '0' : s[0] - 'A' + 10;
        unsigned b = s[1] <= '9' ? s[1] - '0' : s[1] - 'A' + 10;
        out.push_back((a << 4) | b);
    }
    return out;
}
bool EncodeThumbCall(u32 from, u32 to, u8 *out) {
    const s64 delta = s64(to) - s64(from) - 4;
    if ((from | to) & 1 || delta < -4194304 || delta > 4194302) return false;
    const u32 bits = u32(delta);
    Write16(out, 0xf000 | ((bits >> 12) & 0x7ff));
    Write16(out + 2, 0xf800 | ((bits >> 1) & 0x7ff));
    return true;
}
bool DecodeThumbCall(u32 from, const u8 *in, u32 &to) {
    const u16 a = Read16(in), b = Read16(in + 2);
    if ((a & 0xf800) != 0xf000 || (b & 0xf800) != 0xf800 || (from & 1)) return false;
    u32 bits = ((a & 0x7ff) << 12) | ((b & 0x7ff) << 1);
    s64 delta = bits & 0x400000 ? s64(bits) - 0x800000 : bits;
    const s64 target = s64(from) + 4 + delta;
    if (target < 0 || target > 0xffffffffLL) return false;
    to = u32(target);
    return true;
}

struct Snapshot {
    const u8 *bytes; u32 size, base;
    const u8 *at(u32 addr, u32 count) const {
        if (addr < base || u64(addr) - base + count > size) return 0;
        return bytes + (addr - base);
    }
    bool match(u32 addr, const char *hex) const {
        const std::vector<u8> v = Hex(hex);
        const u8 *p = at(addr, v.size());
        return p && memcmp(p, &v[0], v.size()) == 0;
    }
    bool call(u32 addr, u32 &target) const {
        const u8 *p = at(addr, 4);
        return p && DecodeThumbCall(addr, p, target) && at(target, 4);
    }
};

bool BuildDiHook(const u8 *image, u32 size, u32 base, u32 site,
                 u32 endWords, DiHookPlan &plan, std::string &why) {
    plan = DiHookPlan();
    why = "unsupported d2x LOW_READ layout; no IOS code changed";
    if (!image || endWords <= RIIVO_REGION_WORDS || endWords > 0x80000000u) return false;
    const Snapshot s = { image, size, base };
    const u8 *dispatch = s.at(site, 20);
    if (!dispatch || memcmp(dispatch, DI_READ_PATTERN, DI_READ_PATTERN_LEN) ||
        !s.match(site + 18, "0004E7DB") || site < 0x1a2) return false;
    // Verify the stack frame and r5/r6/r7 assignments used by redirect.S.
    const u32 entry = site - 0x1a2;
    if (!s.match(entry, "6803B5F74DAB0006000F0E1B9200")) return false;
    const u32 stock = site - 0x176;
    if (!s.match(stock, "9A0000390030")) return false;
    u32 raw = 0, frag = 0, dvd = 0, handle = 0;
    if (!s.call(site + 14, raw) || !s.call(stock + 6, handle)) return false;

    // A fully checked function body, apart from relocatable BL immediates and
    // its configuration pointer. Constants and all control flow must match.
    const std::vector<u8> rawExpected = Hex(
        "B5704C1368632B01D0092B02D1094B11429AD3064B1020A002006123BD704B0FE7F6"
        "68A368E5195B189A682306DCD502F7FFFF0AE7F2075CD502F000FB19E7ED07DBD502"
        "F7FFFC4CE7E8F7FFFCABE7E51383D1E47ED380000005210046090000");
    const u8 *r = s.at(raw, rawExpected.size());
    if (!r || (raw & 3)) return false;
    for (u32 i = 0; i < rawExpected.size(); ++i) {
        if ((i >= 0x30 && i < 0x34) || (i >= 0x3a && i < 0x3e) ||
            (i >= 0x44 && i < 0x48) || (i >= 0x4a && i < 0x4e) ||
            (i >= 0x50 && i < 0x54)) continue;
        if (r[i] != rawExpected[i]) return false;
    }
    const u8 *config = s.at(((entry + 8) & ~3u) + 0xab * 4, 4);
    if (!config || Read32(config) != Read32(r + 0x50)) return false;
    u32 unused;
    if (!s.call(raw + 0x30, frag) || !s.call(raw + 0x44, dvd) ||
        !s.call(raw + 0x3a, unused) || !s.call(raw + 0x4a, unused) ||
        !s.match(frag, "B5F00017B085000A0005000CAB030001")) return false;

    why = "unsupported d2x DVD-ROM helper; no IOS code changed";
    // Reuse an actual RX function, never presumed padding or RW fragment data.
    // This entry is unreachable in MODE_FRAG; keep a failing DVD-ROM stub at
    // its original address in case the mode is subsequently changed.
    const std::vector<u8> dvdExpected = Hex(
        "B5F02400B0870A53002790059104920393009B04429FD3020020B007BDF09B049A03"
        "1BDD9B002600025B429AD9011AD3009E9B05228019DB0112002900189302F000F87E"
        "2800D0012E00D0262280197301124293D9001B954B182080681B2120010047989001"
        "2800D02421809A000109F7FFFF781E04D1059B0198021999002AF001F84A4B0E9801"
        "681B47989A0019AB0ADB18D3197F9300E7BB23FF000503DB4298D900001D9A009802"
        "0029F7FFFF5A0004E7EB24164264E7E846C01383D1D81383D1DC");
    const u8 *d = s.at(dvd, dvdExpected.size());
    if (!d || (dvd & 3)) return false;
    for (u32 i = 0; i < 0xbc; ++i) {
        if ((i >= 0x40 && i < 0x44) || (i >= 0x70 && i < 0x74) ||
            (i >= 0x80 && i < 0x84) || (i >= 0xac && i < 0xb0)) continue;
        if (d[i] != dvdExpected[i]) return false;
    }
    for (u32 i = 0; i < 4; ++i) {
        const u32 offsets[] = { 0x40, 0x70, 0x80, 0xac };
        if (!s.call(dvd + offsets[i], unused)) return false;
    }
    // Bytes assembled from ios/redirect.S, ARMv5TE big-endian Thumb-1.
    plan.code = Hex(
        "20A00200477046C0B51068B223C005DB429AD31D4B19429AD2291A9B009B68714299"
        "D8249B024299D821231F4219D11E00384218D11B682B24104223D01768AB68EC4323"
        "D113F7FFFFFE2800D111BD10682B079BD405003000399A02F7FFFFFEBD10687168B2"
        "0038F7FFFFFEBD104B04E0004B04612B20A00200BD10000000000005210000031100");
    plan.storage = dvd;
    plan.dispatch = site;
    plan.branch.resize(6);
    if (!EncodeThumbCall(dvd + 0x46, frag, &plan.code[0x46]) ||
        !EncodeThumbCall(dvd + 0x5c, handle, &plan.code[0x5c]) ||
        !EncodeThumbCall(dvd + 0x68, raw, &plan.code[0x68]) ||
        !EncodeThumbCall(site, dvd + 8, &plan.branch[0])) return false;
    Write32(&plan.code[0x7c], endWords);
    // After the call, jump to the existing mov r4,r0 / DI return sequence.
    Write16(&plan.branch[4], 0xe005); // site+4 -> site+18
    why.clear();
    return true;
}
}
