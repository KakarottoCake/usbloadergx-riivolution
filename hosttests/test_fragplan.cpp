// Synthetic LOW_READ placement: raw-disc capacity is deliberately unchanged.
#include <stdio.h>
#include "riivo/RiivoFragPlan.hpp"
#include "riivo/RiivoDiPatch.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
static std::vector<ModExtent> pack(u64 start, u32 length, u32 count, u32 align) {
    std::vector<ModExtent> out;
    for (u32 i=0;i<count;++i) {
        ModExtent m = {start, length}; out.push_back(m);
        start += ((u64)length + align - 1) & ~(u64)(align - 1);
    }
    return out;
}
int main() {
    const u64 image = 4685037568ULL; // RAW mapped end, not FST payload end
    const u64 start = RIIVO_REGION_BYTES;
    for (u32 sector=512;sector<=4096;sector*=2) {
        ck(PlanRegionStart(image,sector)==start,"fixed 6 GiB start");
        ck(PlanFragRegion(image,sector,3,pack(start,52863,151,sector)).ok,"8 MB mod");
        ck(PlanFragRegion(image,sector,3,pack(start,602753,1067,sector)).ok,"645 MB mod");
        ck(PlanFragRegion(image,sector,3,pack(start,494018560,1,sector)).ok,"Spectral aligned span");
        ck(PlanFragRegion(4699979776ULL,sector,3,pack(start,4096,1,sector)).ok,"full DVD5 ISO");
        ck(!PlanFragRegion(RIIVO_DVD9_PROBE_BYTES,sector,3,pack(start,4096,1,sector)).ok,"DVD9 refused");
        ck(!PlanFragRegion(image,sector,3,pack(start-sector,4096,1,sector)).ok,"below window");
        ck(!PlanFragRegion(image,sector,3,pack(start+1,4096,1,sector)).ok,"unaligned");
        ck(!PlanFragRegion(image,sector,3,pack(RIIVO_REGION_LIMIT,1,1,sector)).ok,"above window");
        ck(!PlanFragRegion(image,sector,3,pack(~(u64)0-sector+1,4096,1,sector)).ok,"overflow");
        ck(!PlanFragRegion(image,sector,3,pack(start,4096,0,sector)).ok,"empty");
        std::vector<ModExtent> v=pack(start,4096,3,sector);
        v[2].offset=v[1].offset;
        ck(!PlanFragRegion(image,sector,3,v).ok,"duplicate/overlap");
        v[2].offset=start;
        ck(!PlanFragRegion(image,sector,3,v).ok,"descending");
        ck(!PlanFragRegion(image,sector,19936,pack(start,4096,1,sector)).ok,"reserved slots");
        ck(!PlanFragRegion(image,sector,0xffffffff,pack(start,4096,1,sector)).ok,"slot overflow");
        FragPlan exact=PlanFragRegion(image,sector,3,pack(start,0x80000000u,1,sector));
        ck(exact.ok && exact.regionEnd==RIIVO_REGION_LIMIT && !exact.ceilingSpare,"exact 2 GiB window");
        ck(!PlanFragRegion(image,sector,3,pack(start,0x80000001u,1,sector)).ok,"one byte too large");
    }
    ck(!PlanFragRegion(image,500,3,pack(start,4096,1,512)).ok,"non-power-of-two sector");
    ck(!PlanFragRegion(image,8192,3,pack(start,4096,1,512)).ok,"unsupported sector");
    ck(start > RIIVO_DVD9_PROBE_BYTES + 0x800,"no fragment in layer probe");
    ck(start > RIIVO_DVD5_CEILING,"mods deliberately above raw ceiling");
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
