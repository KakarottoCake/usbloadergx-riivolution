#include <stdio.h>
#include <string.h>
#include "riivo/RiivoSmg2Reserve.hpp"
using namespace Riivo;
static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
int main() {
    const u8 id[]="SB4E01";
    u32 w[4]={0x806d9644,0x4e800020,0,0}, p[4]={0};
    CHECK(BuildSmg2GetterPatch(w,p)); CHECK(Smg2GetterPatched(p));
    CHECK(!BuildSmg2GetterPatch(p,p)); // cannot apply twice
    CHECK(!BuildSmg2GetterPatch(0,p)); CHECK(!BuildSmg2GetterPatch(w,0));
    CHECK(!CheckSmg2Reservation(id,0,230076,0x935e0000,0x90200020,w));
    CHECK(!CheckSmg2Reservation(id,0,SMG2_FST_CAP,0x935e0000,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,0,SMG2_FST_CAP+1,0x935e0000,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,0,0,0x935e0000,0x90200020,w));
    CHECK(CheckSmg2Reservation((const u8*)"SB4P01",0,230076,0x935e0000,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,1,230076,0x935e0000,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,0,230076,0,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,0,230076,0xffffffff,0x90200020,w));
    CHECK(CheckSmg2Reservation(id,0,230076,0x935e0000,SMG2_FST_BASE,w));
    CHECK(CheckSmg2Reservation(id,0,230076,0x935e0000,0xfffffff0,w));
    for(int i=0;i<4;++i) { w[i]^=1; CHECK(!BuildSmg2GetterPatch(w,p)); w[i]^=1; }
    CHECK(!Smg2Overlaps(SMG2_FST_END,32,SMG2_FST_BASE,SMG2_FST_END));
    CHECK(Smg2Overlaps(SMG2_FST_END-1,2,SMG2_FST_BASE,SMG2_FST_END));
    CHECK(!Smg2Overlaps(SMG2_FST_BASE,0,SMG2_FST_BASE,SMG2_FST_END));
    // Decode the instruction actually emitted, including wrap semantics.
    CHECK(BuildSmg2GetterPatch(w,p));
    CHECK((p[1]>>26)==15 && ((p[1]>>21)&31)==3 && ((p[1]>>16)&31)==3);
    CHECK(0x90000800u + ((p[1]&0xffff)<<16) == SMG2_FST_END);
    // Placement half: MEM2 answer, MEM1 arena untouched.
    {
        ArenaInfo arena;
        arena.arenaLo = 0; arena.arenaHi = 0x817E0000;
        arena.fstAddr = 0x817DA740; arena.fstMaxSize = 153792;
        FstPlacement sp = BuildSmg2ReservationPlacement(arena, 230076);
        CHECK(sp.ok); CHECK(!sp.inPlace);
        CHECK(sp.fstAddr == SMG2_FST_BASE);
        CHECK(sp.newArenaHi == arena.arenaHi);
        CHECK(sp.reserved == 0);
        CHECK(!BuildSmg2ReservationPlacement(arena, 0).ok);
        CHECK(!BuildSmg2ReservationPlacement(arena, SMG2_FST_CAP + 1).ok);
        CHECK(BuildSmg2ReservationPlacement(arena, SMG2_FST_CAP).ok);
        ArenaInfo badHi = arena; badHi.arenaHi = 0;
        CHECK(!BuildSmg2ReservationPlacement(badHi, 230076).ok);
        ArenaInfo badFst = arena; badFst.fstAddr = 0x90000800;
        CHECK(!BuildSmg2ReservationPlacement(badFst, 230076).ok);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
