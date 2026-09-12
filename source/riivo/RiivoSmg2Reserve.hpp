#ifndef RIIVO_SMG2_RESERVE_HPP
#define RIIVO_SMG2_RESERVE_HPP

#include <gctypes.h>
#include <string.h>
#include "RiivoFstInstall.hpp"

namespace Riivo {
// SB4E01 revision 0 only. Getter occupies a 16-byte aligned slot in
// main.dol. Grow lwz/blr to lwz/addis/blr within that slot; no external cave.
// Static survey of main.dol (text sections): exactly one reader of
// r13-27068 (the getter itself), exactly one writer (the setter at
// 0x805B4ED0), 12 bl call sites to the slot entry, and no absolute
// references into the slot padding. Patching the getter therefore reaches
// exactly the consumers the emulator's setter interception reached;
// the Wii boot is what proves it dynamically.
static const u32 SMG2_GET_BASE = 0x805b4e70;
static const u32 SMG2_FST_BASE = 0x90000800;
static const u32 SMG2_FST_CAP = 0x00040000;
static const u32 SMG2_FST_END = SMG2_FST_BASE + SMG2_FST_CAP;

inline bool Smg2GetterOriginal(const u32 *w) {
    return w && w[0] == 0x806d9644 && w[1] == 0x4e800020
        && w[2] == 0 && w[3] == 0;
}
inline bool Smg2GetterPatched(const u32 *w) {
    return w && w[0] == 0x806d9644 && w[1] == 0x3c630004
        && w[2] == 0x4e800020 && w[3] == 0;
}
inline bool BuildSmg2GetterPatch(const u32 *original, u32 *patched) {
    if (!patched || !Smg2GetterOriginal(original)) return false;
    patched[0] = original[0]; // lwz r3,-27068(r13)
    patched[1] = 0x3c630004; // addis r3,r3,4 (no CR update)
    patched[2] = 0x4e800020; // blr; LR, r13 and stack unchanged
    patched[3] = 0;
    return true;
}
inline bool Smg2Overlaps(u32 a, u32 size, u32 lo, u32 hi) {
    return size && (u64)a < hi && (u64)a + size > lo;
}
inline const char *CheckSmg2Reservation(const u8 *id, u8 revision,
        u32 size, u32 iosBoundary, u32 source, const u32 *getter) {
    if (!id || memcmp(id, "SB4E01", 6) || revision != 0)
        return "requires SB4E01 revision 0";
    if (!size || size > SMG2_FST_CAP) return "table exceeds 256 KiB reservation";
    if (iosBoundary < SMG2_FST_END + 8*1024*1024 || iosBoundary > 0x94000000)
        return "invalid or insufficient PPC/IOS MEM2 boundary";
    if (source < 0x90200000 || (u64)source + size > 0x93300000)
        return "staging is outside the GX MEM2 allocator";
    if (!Smg2GetterOriginal(getter)) return "BASE getter revision or patch conflict";
    return 0;
}
// Placement half of the reservation: the MEM2 answer with MEM1 arena
// untouched (mirrors PlaceFstMem2's conventions: reserved 0, heapLeft
// unmeasured). Pure arithmetic, host-tested. The runtime guards above
// (game, size, boundary, staging, getter) are evaluated separately by
// the caller, which also refuses rather than falls back when they fail:
// on SB4E01 the MEM1 cascade steers into the game's clear zone.
inline FstPlacement BuildSmg2ReservationPlacement(const ArenaInfo &arena, u32 fstSize) {
    FstPlacement p;
    if (!fstSize) { p.why = "the rebuilt table is empty"; return p; }
    if (fstSize > SMG2_FST_CAP) { p.why = "the rebuilt table is larger than the SB4E01 reservation"; return p; }
    if (arena.arenaHi <= MEM1_BASE || arena.arenaHi > MEM1_END) { p.why = "arena high is outside MEM1"; return p; }
    if (arena.fstAddr < MEM1_BASE || arena.fstAddr >= MEM1_END) { p.why = "the file table is not in MEM1 - has the apploader run?"; return p; }
    p.ok = true;
    p.inPlace = false;
    p.fstAddr = SMG2_FST_BASE; // 32-byte aligned by construction
    p.newArenaHi = arena.arenaHi; // untouched: MEM1 heap gives up nothing
    p.reserved = 0;
    p.heapLeft = 0; // not measured here; MEM1 heap is unchanged
    return p;
}
} // namespace Riivo
#endif
