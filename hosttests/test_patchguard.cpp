#include <stdio.h>
#include "riivo/RiivoPatchGuard.hpp"
using namespace Riivo;
static int checks, failed;
static void ck(bool ok,const char *name) { ++checks; if(!ok){++failed;printf("FAIL: %s\n",name);} }
int main() {
    ResolvedPatchSet set;
    ResolvedMemory m = {}; m.offset=0x1800; m.value.resize(2308); set.memories.push_back(m);
    ConfigurePatchProtection(set,"",true);
    ck(RiivoPatchConflict(0x80001000,0x2000),"Syati excludes Gecko");
    ck(!RiivoPatchConflict(0x80001000,0x800),"half-open lower edge");
    ck(!RiivoPatchConflict(0x80002104,32),"half-open upper edge");
    ck(RiivoPatchConflict(0x80002100,12),"480p return branch protected too");
    ck(!RiivoPatchConflict(0x80001800,0),"empty probe");
    ck(!RiivoPatchConflict(0xfffffff0,64),"no 32-bit wrap");
    ck(!RiivoPatchConflict(0x80400000,12),"unrelated patch allowed");
    ProtectAppliedPatch(0x80400000,4);
    ck(RiivoPatchConflict(0x80400000,12),"resolved search write protected");
    ConfigurePatchProtection(set,"",false);
    ck(!RiivoPatchConflict(0x80001000,0x2000),"bisection permits stock handler");
    ConfigurePatchProtection(ResolvedPatchSet(),"",true);
    ck(!RiivoPatchConflict(0x80001000,0x2000),"next boot resets ranges");
    set.memories[0].search=true;
    ConfigurePatchProtection(set,"",true);
    ck(!RiivoPatchConflict(0x80001000,0x2000),"search offset not a write target");
    // A <memory valuefile=> patch carries no bytes until it is applied, so
    // its length has to come off the file. Measured here, or not protected.
    {
        const char *blob = "test_patchguard_blob.bin";
        FILE *f = fopen(blob, "wb");
        ck(f != 0, "valuefile fixture created");
        if (f) { for (int i = 0; i < 2308; ++i) fputc(0, f); fclose(f); }
        ResolvedPatchSet vs;
        ResolvedMemory vm = {}; vm.offset = 0x1800; vm.valuefile = blob;
        vs.memories.push_back(vm);
        ConfigurePatchProtection(vs, ".", true);
        ck(RiivoPatchConflict(0x80001000,0x2000),"valuefile blob measured and protected");
        ck(!RiivoPatchConflict(0x80002104,32),"valuefile length is the file size, not a guess");
        remove(blob);
        ConfigurePatchProtection(vs, ".", true);
        ck(!RiivoPatchConflict(0x80001000,0x2000),"unmeasurable valuefile left to the late check");
    }
    ProtectAppliedPatch(0xfffffff0,64);
    ck(!RiivoPatchConflict(0xfffffff0,16),"invalid patch target excluded");
    printf("%d checks, %d failures\n",checks,failed); return failed?1:0;
}
