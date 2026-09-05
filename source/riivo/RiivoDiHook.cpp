#include "RiivoDiHook.hpp"
#include "RiivoDiPatch.hpp"
#include <string.h>

namespace Riivo {
// Discovery pattern, single definition for the whole loader. Bytes taken
// from the tester's console (dipp-93800a1c.asm); the branch at +6 is
// deliberately absent - it is position-dependent and never compared.
const u8 DI_READ_HEAD[] = { 0x68,0x23,0x07,0x9A,0xD4,0x00 };
const u32 DI_READ_HEAD_LEN = sizeof(DI_READ_HEAD);
const u32 DI_READ_TAIL_OFF = 8;
const u8 DI_READ_TAIL[] = { 0x68,0x41,0x68,0x82,0x1C,0x38 };
const u32 DI_READ_TAIL_LEN = sizeof(DI_READ_TAIL);
const u32 DI_READ_SPAN = 14; // head + gap + tail, for scan bounds
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

bool DecodeThumbBranch(u32 from, u16 insn, u32 &to) {
    if ((insn & 0xF800) != 0xE000 || (from & 1)) return false;
    s32 delta = (s32)(insn & 0x7FF);
    if (delta & 0x400) delta -= 0x800;
    delta <<= 1;
    const s64 target = s64(from) + 4 + delta;
    if (target < 0 || target > 0xffffffffLL) return false;
    to = u32(target);
    return true;
}

// Read the word a `ldr rt,[pc,#imm]` at `insn` points at. The immediate is
// decoded, never matched: pool placement moves between builds.
static bool ReadLiteralAt(const Snapshot &s, u32 insn, u32 &word) {
    const u8 *p = s.at(insn, 2);
    if (!p) return false;
    const u16 h = Read16(p);
    if ((h & 0xF800) != 0x4800) return false;
    const u32 lit = ((insn + 4) & ~3u) + ((h & 0xFF) * 4);
    const u8 *w = s.at(lit, 4);
    if (!w) return false;
    word = Read32(w);
    return true;
}

// Match `ldr rt,[pc,#imm]` for a specific register, any offset.
static bool MatchPCLoad(const Snapshot &s, u32 insn, u8 reg) {
    const u8 *p = s.at(insn, 2);
    if (!p) return false;
    const u16 h = Read16(p);
    return (h & 0xF800) == 0x4800 && ((h >> 8) & 7) == reg;
}

bool BuildDiHook(const u8 *image, u32 size, u32 base, u32 site,
                 u32 endWords, DiHookPlan &plan, std::string &why) {
    plan = DiHookPlan();
    why = "unsupported d2x LOW_READ layout; no IOS code changed";
    if (!image || endWords <= RIIVO_REGION_WORDS || endWords > 0x80000000u) return false;
    const Snapshot s = { image, size, base };
    // Split discovery pattern: 3-halfword head, position-dependent branch
    // skipped, 3-halfword tail at +8. Every halfword of the old contiguous
    // pattern differs on this build, while its halves match Thumb noise in
    // isolation - anchor on the head, skip the branch, verify the tail.
    const u8 *dispatch = s.at(site, DI_READ_HEAD_LEN);
    if (!dispatch || memcmp(dispatch, DI_READ_HEAD, DI_READ_HEAD_LEN)) {
        why = "LOW_READ handler head not found; no IOS code changed";
        return false;
    }
    const u8 *tail = s.at(site + DI_READ_TAIL_OFF, DI_READ_TAIL_LEN);
    if (!tail || memcmp(tail, DI_READ_TAIL, DI_READ_TAIL_LEN)) {
        why = "LOW_READ handler tail not found; no IOS code changed";
        return false;
    }
    // LOW_READ case in the dispatcher switch: cmp #0x71, beq +0, then the
    // branch this hook replaces. Decoding it (rather than trusting an
    // offset) proves the write site really calls this handler.
    if (!s.match(site - 0x176, "2B71D000")) {
        why = "LOW_READ switch case not found; no IOS code changed";
        return false;
    }
    {
        const u8 *b = s.at(site - 0x170, 2);
        u32 target = 0;
        if (!b || !DecodeThumbBranch(site - 0x170, Read16(b), target) || target != site) {
            why = "switch case does not branch to the handler; no IOS code changed";
            return false;
        }
    }
    // Dispatcher entry: twelve immediate-free halfwords of frame setup, then
    // the config load. The pool offset moves between builds, so the load is
    // matched by shape (any offset) and its word is compared, not copied.
    const u32 entry = site - 0x1A8;
    if (!s.match(entry, "B5F0B08378031C061C0F9200")) {
        why = "dispatcher entry not found; no IOS code changed";
        return false;
    }
    u32 cfgA = 0;
    if (!MatchPCLoad(s, entry + 12, 4) || !ReadLiteralAt(s, entry + 12, cfgA) || !cfgA) {
        why = "dispatcher config pointer not found; no IOS code changed";
        return false;
    }
    // Read worker: the handler's BL target. Head has one pc-relative load
    // (any offset); it must point at the same config struct the dispatcher
    // uses, or this is not the worker.
    u32 raw = 0;
    {
        const u8 *b = s.at(site + 0xE, 4);
        if (!b || !DecodeThumbCall(site + 0xE, b, raw) || !s.at(raw, 8)) {
            why = "read worker call not found; no IOS code changed";
            return false;
        }
    }
    if (!s.match(raw, "B538") || !MatchPCLoad(s, raw + 2, 3) ||
        !s.match(raw + 4, "685B2B01")) {
        why = "read worker head not found; no IOS code changed";
        return false;
    }
    u32 cfgB = 0;
    if (!ReadLiteralAt(s, raw + 2, cfgB) || !cfgB || cfgB != cfgA) {
        why = "worker config pointer does not match; no IOS code changed";
        return false;
    }
    // Frag reader: the first BL in the worker head. A build that lays the
    // worker out differently decodes to something whose head will not match
    // below, which is a refusal, not a wrong call target.
    u32 frag = 0;
    {
        bool found = false;
        for (u32 a = raw; a + 4 <= raw + 0x60; a += 2) {
            const u8 *p = s.at(a, 4);
            u32 t = 0;
            if (!p) break;
            if (DecodeThumbCall(a, p, t) && s.at(t, 8)) {
                frag = t;
                found = true;
                break;
            }
        }
        if (!found) {
            why = "frag reader call not found; no IOS code changed";
            return false;
        }
    }
    if (!s.match(frag, "B5F0B0851C069100")) {
        why = "frag reader head not found; no IOS code changed";
        return false;
    }
    // Epilogue: the dispatcher's return sequence. Located by decoding the
    // handler's own branch to it, then verified byte for byte - the redirect
    // jumps here with the result in r5.
    //! Derived from the branch, never assumed from a fixed offset. A constant
    //! here is one the host fixture can happily agree with while the console
    //! disagrees: on the measured module the epilogue is at site + 0x27E, and
    //! a hardcoded site + 0x17E lands on 687368B29300 - a refusal on hardware
    //! that no self-consistent test could have caught.
    u32 epi = 0;
    {
        const u8 *b = s.at(site + 0x14, 2);
        if (!b || !DecodeThumbBranch(site + 0x14, Read16(b), epi)) {
            why = "handler return branch does not decode; no IOS code changed";
            return false;
        }
    }
    if (!s.match(epi, "B0031C28BDF0")) {
        why = "dispatcher return not found; no IOS code changed";
        return false;
    }
    // Storage: the bit0 reader below the worker. Called exactly once in the
    // snapshot, from the worker - unreachable while MODE_FRAG is set, which
    // it is from fragment registration on. The entry becomes the failing
    // stub; the LOW_READ case calls entry + 8.
    const u32 store = site - 0x984;
    if (!s.match(store, "B5F0") || !s.match(store + 4, "92050A57")) {
        why = "hook storage head not found; no IOS code changed";
        return false;
    }
    {
        const u8 *h = s.at(store + 2, 2);
        if (!h || (Read16(h) & 0xFF80) != 0xB080) {
            why = "hook storage frame not found; no IOS code changed";
            return false;
        }
    }
    {
        u32 callers = 0;
        for (u32 a = base; ; a += 2) {
            const u8 *p = s.at(a, 4);
            if (!p) break;
            u32 t = 0;
            if (DecodeThumbCall(a, p, t) && t == store) ++callers;
        }
        if (callers != 1) {
            why = "hook storage has callers; no IOS code changed";
            return false;
        }
    }
    // Bytes assembled from ios/redirect.S, ARMv5TE big-endian Thumb-1.
    // Patch offsets mirror the redirect_* labels: frag call, limit word,
    // epilogue word. The host test reassembles redirect.S and checks this.
    static const u32 FRAG_OFF = 0x36;
    static const u32 LIMIT_OFF = 0x60;
    static const u32 EPI_OFF = 0x64;
    static const u32 STORAGE_SIZE = 0xC4; // 0x24c..0x316 minus the 8-byte stub
    plan.code = Hex(
        "20A00200477046C0B5D9688223C005DB429AD3164B12429AD218"
        "1A9B009B68414299D81368232510422BD00F68A368E5195B18D2"
        "0038F7FFFFFE2800D1080005E00ABCD9B0016823079A47704B06"
        "E0004B06612325A0022DBCD9B0014B0147180000000000000000"
        "0005210000031100");
    plan.storage = store;
    plan.dispatch = site;
    plan.branch.resize(4);
    if ((u32)plan.code.size() > STORAGE_SIZE ||
        !EncodeThumbCall(site, store + 8, &plan.branch[0]) ||
        !EncodeThumbCall(store + FRAG_OFF, frag, &plan.code[FRAG_OFF])) return false;
    Write32(&plan.code[LIMIT_OFF], endWords);
    //! The routine reaches the epilogue with `ldr r3, <word>` + `bx r3`, and
    //! bx takes the target state from bit 0. Written even, the core switches
    //! to ARM and runs the Thumb epilogue as ARM instructions.
    Write32(&plan.code[EPI_OFF], epi | 1u);
    why.clear();
    return true;
}
}
