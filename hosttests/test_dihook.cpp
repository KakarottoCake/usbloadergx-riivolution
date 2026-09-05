// Hook verification against the tester's console (dipp-93800a1c.asm), plus
// relocation/rejection tests. The fixture mirrors the real relative layout -
// dispatcher, case, handler, epilogue, worker, frag reader, storage - at a
// synthetic base, so every offset BuildDiHook derives is exercised. The
// redirect.S round-trip at the end checks the assembled routine byte for
// byte against the hex embedded in RiivoDiHook.cpp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iterator>
#include <vector>
#include "riivo/RiivoDiHook.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok,const char *what) {
    ++checks; if(!ok){++failures;printf("FAIL: %s\n",what);}
}
static void hex(std::vector<u8>&v,u32 at,const char *s) {
    for(;*s;s+=2,++at) { char b[]={s[0],s[1],0}; v[at]=(u8)strtoul(b,0,16); }
}
// Thumb-1 unconditional branch encoder (test-side only).
static u16 encB(u32 from,u32 to) {
    return (u16)(0xE000 | (((to - (from + 4)) >> 1) & 0x7FF));
}
// ldr rt,[pc,#imm] encoder (test-side only).
static u16 encLDR(u8 reg,u32 from,u32 target) {
    u32 off = target - ((from + 4) & ~3u);
    return (u16)(0x4800 | (reg << 8) | ((off / 4) & 0xFF));
}
static const u32 kBase = 0x93300000;
static const u32 kSite = kBase + 0xC00;
static const u32 kStore = kSite - 0x984;   // bit0 reader, dead in frag mode
static const u32 kEntry = kSite - 0x1A8;   // dispatcher
static const u32 kCase  = kSite - 0x170;   // LOW_READ switch case
static const u32 kEpi   = kSite + 0x27E;   // dispatcher return sequence,
                                           // the real module's offset
static const u32 kWork  = kBase + 0x400;   // read worker (decoded, not fixed)
static const u32 kFrag  = kBase + 0x600;   // frag-mode chunked reader
static const u32 kCfg   = 0x1383D204;      // config struct, both literals agree
static const u32 kEndW  = 0x68000000;
static std::vector<u8> fixture() {
    std::vector<u8> v(0x2000);
    char b[16];
    // Storage head: push, sub sp (any frame), str, sector shift.
    hex(v,kStore - kBase,"B5F0B08992050A57");
    // Dispatcher prologue: twelve immediate-free halfwords, then the config
    // load whose literal both sides must agree on.
    hex(v,kEntry - kBase,"B5F0B08378031C061C0F9200");
    snprintf(b,sizeof(b),"%04X",encLDR(4,kEntry + 12,kEntry + 0x40));
    hex(v,kEntry + 12 - kBase,b);
    // LOW_READ case: cmp #0x71, beq +0, branch to the handler.
    hex(v,kCase - 6 - kBase,"2B71D000");
    snprintf(b,sizeof(b),"%04X",encB(kCase,kSite));
    hex(v,kCase - kBase,b);
    // Handler head, position-dependent branch skipped, tail at +8.
    hex(v,kSite - kBase,"6823079AD400E133684168821C38");
    // Worker call + epilogue branch, both decoded, never matched.
    u8 call[4];
    EncodeThumbCall(kSite + 0xE,kWork,call);
    for(int i=0;i<4;++i) { snprintf(b,sizeof(b),"%02X",call[i]); hex(v,kSite + 0xE + i - kBase,b); }
    snprintf(b,sizeof(b),"%04X",encB(kSite + 0x14,kEpi));
    hex(v,kSite + 0x14 - kBase,b);
    // Worker head, its config load, and its two calls: frag reader first.
    hex(v,kWork - kBase,"B538");
    snprintf(b,sizeof(b),"%04X",encLDR(3,kWork + 2,kWork + 0x80));
    hex(v,kWork + 2 - kBase,b);
    hex(v,kWork + 4 - kBase,"685B2B01");
    EncodeThumbCall(kWork + 0x36,kFrag,call);
    for(int i=0;i<4;++i) { snprintf(b,sizeof(b),"%02X",call[i]); hex(v,kWork + 0x36 + i - kBase,b); }
    EncodeThumbCall(kWork + 0x4A,kStore,call); // the single caller of storage
    for(int i=0;i<4;++i) { snprintf(b,sizeof(b),"%02X",call[i]); hex(v,kWork + 0x4A + i - kBase,b); }
    // Frag reader head, fully immediate-free.
    hex(v,kFrag - kBase,"B5F0B0851C069100");
    // Epilogue: add sp, mov r0, pop-and-return.
    hex(v,kEpi - kBase,"B0031C28BDF0");
    return v;
}
// Big-endian word writer for literal pools (hex() writes halfword-swapped).
static void wordbe(std::vector<u8>&v,u32 at,u32 w) {
    v[at]=u8(w>>24); v[at+1]=u8(w>>16); v[at+2]=u8(w>>8); v[at+3]=u8(w);
}
int main(int argc,char **argv) {
    std::vector<u8> v=fixture();
    wordbe(v,kEntry + 0x40 - kBase,kCfg);
    wordbe(v,kWork + 0x80 - kBase,kCfg);
    DiHookPlan p; std::string why;
    ck(BuildDiHook(&v[0],v.size(),kBase,kSite,kEndW,p,why),"supported image");
    if(!p.code.size()) {printf("%s\n",why.c_str());return 1;}
    ck(p.storage==kStore && p.dispatch==kSite,"resolved RX storage");
    ck(p.code.size()==112 && p.branch.size()==4,"bounded patch size");
    u32 target=0;
    ck(DecodeThumbCall(kSite,&p.branch[0],target)&&target==kStore+8,"entry calls routine");
    ck(DecodeThumbCall(kStore+0x36,&p.code[0x36],target)&&target==kFrag,"frag call relocated");
    ck(p.code[0x60]==0x68 && p.code[0x61]==0 && p.code[0x62]==0 && p.code[0x63]==0,
       "exclusive word limit");
    ck(p.code[0x64]==u8(kEpi>>24) && p.code[0x65]==u8(kEpi>>16)
       && p.code[0x66]==u8(kEpi>>8) && p.code[0x67]==u8(kEpi|1),
       "epilogue word takes the decoded address");
    // bx takes its target state from bit 0. Written even, the core switches
    // to ARM and runs the Thumb epilogue as ARM instructions.
    ck((p.code[0x67]&1)==1,"epilogue word carries the Thumb bit");
    // The branch at +6 is never compared: retargeting it must still pass.
    {
        std::vector<u8> b2=v; b2[kSite-kBase+6]=0xE1; b2[kSite-kBase+7]=0x34;
        ck(BuildDiHook(&b2[0],b2.size(),kBase,kSite,kEndW,p,why),"skipped branch ignored");
    }
    // The epilogue must be DERIVED from the handler's branch, not assumed at
    // a fixed offset. Move it, retarget the branch, and the build must follow:
    // a hardcoded offset would agree with this fixture and disagree with the
    // console, which is exactly how it shipped wrong once.
    {
        std::vector<u8> b3=v;
        const u32 moved = kSite + 0x1FE;
        for(int i=0;i<6;++i) b3[kEpi-kBase+i]=0;
        hex(b3,moved-kBase,"B0031C28BDF0");
        char bb[8]; snprintf(bb,sizeof(bb),"%04X",encB(kSite+0x14,moved));
        hex(b3,kSite+0x14-kBase,bb);
        DiHookPlan p3; std::string w3;
        ck(BuildDiHook(&b3[0],b3.size(),kBase,kSite,kEndW,p3,w3),"epilogue found where the branch points");
        ck(p3.code[0x64]==u8(moved>>24) && p3.code[0x65]==u8(moved>>16)
           && p3.code[0x66]==u8(moved>>8) && p3.code[0x67]==u8(moved|1),
           "moved epilogue address is the one emitted");
    }
    // A branch that lands on something that is not the epilogue is refused.
    {
        std::vector<u8> b4=v;
        char bb[8]; snprintf(bb,sizeof(bb),"%04X",encB(kSite+0x14,kSite+0x1FE));
        hex(b4,kSite+0x14-kBase,bb);
        ck(!BuildDiHook(&b4[0],b4.size(),kBase,kSite,kEndW,p,why),"branch to non-epilogue refused");
    }
    // The old build's handler is logically identical but register-allocated
    // differently. It must be refused, never matched loosely.
    {
        std::vector<u8> old=v;
        hex(old,kSite-kBase,"682B079BD400E740688268410038");
        ck(!BuildDiHook(&old[0],old.size(),kBase,kSite,kEndW,p,why),"old allocation refused");
    }
    const u32 mutate[]={kSite-kBase,kSite-kBase+2,kSite-kBase+8,kSite-kBase+10,
        kCase-kBase,kCase-kBase+1,kEntry-kBase,kEntry-kBase+2,
        kEntry-kBase+12,kEpi-kBase,kEpi-kBase+2,kEpi-kBase+4,
        kWork-kBase,kWork-kBase+2,kWork-kBase+4,kWork-kBase+6,
        kFrag-kBase,kFrag-kBase+2,kStore-kBase,kStore-kBase+2,kStore-kBase+4,
        kStore-kBase+6,kEntry+0x40-kBase,kWork+0x80-kBase};
    for(u32 i=0;i<sizeof(mutate)/sizeof(mutate[0]);++i) {
        std::vector<u8> bad=v; bad[mutate[i]]^=0x80;
        ck(!BuildDiHook(&bad[0],bad.size(),kBase,kSite,kEndW,p,why),"changed byte refused");
    }
    // A second caller of the storage destroys its dead-code proof.
    {
        std::vector<u8> two=v;
        u8 c2[4];
        EncodeThumbCall(kEpi,kStore,c2);
        for(int i=0;i<4;++i) two[kEpi - kBase + i]=c2[i];
        ck(!BuildDiHook(&two[0],two.size(),kBase,kSite,kEndW,p,why),"second storage caller refused");
    }
    ck(!BuildDiHook(&v[0],0x900,kBase,kSite,kEndW,p,why),"truncated window");
    ck(!BuildDiHook(&v[0],v.size(),kBase,kSite,0x60000000,p,why),"empty mod range");
    ck(!BuildDiHook(&v[0],v.size(),kBase,kSite,0x80000001,p,why),"signed-word overflow");
    ck(BuildDiHook(&v[0],v.size(),kBase,kSite,0x80000000,p,why),"8 GiB exclusive end");
    u32 bt=0;
    ck(encB(kCase,kSite)==0xE0B6,"branch encoder matches console");
    ck(DecodeThumbBranch(kCase,0xE0B6,bt)&&bt==kSite,"branch decode pins dispatch");
    ck(!DecodeThumbBranch(kSite-0x170,0xD400,bt),"conditional is not a branch");
    ck(!DecodeThumbBranch(kSite-0x170,0xF7FF,bt),"call is not a branch");
    u8 b[4];
    for(s64 d=-4194304;d<=4194302;d+=62) {
        const u32 from=0x10000000,to=u32(s64(from)+4+d);
        ck(EncodeThumbCall(from,to,b)&&DecodeThumbCall(from,b,target)&&target==to,"BL roundtrip");
    }
    ck(!EncodeThumbCall(0x10000000,0x10400004,b),"too far forward");
    ck(!EncodeThumbCall(0x10000000,0x0fc00002,b),"too far backward");
    ck(!EncodeThumbCall(0x10000000,0x10000001,b),"unaligned target");
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);
        std::vector<u8> blob((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        ck(blob.size()==p.code.size(),"assembled routine matches embedded size");
        if(blob.size()==p.code.size()) {
            // Patch sites take build-time values; everything else must be identical.
            for(u32 i=0;i<blob.size();++i) {
                if((i>=0x36&&i<0x3A)||(i>=0x60&&i<0x68)) continue;
                if(blob[i]!=p.code[i]) {
                    char m[64];
                    snprintf(m,sizeof(m),"round-trip mismatch at 0x%x",i);
                    ck(false,m);
                    break;
                }
            }
            ck(true,"round-trip bytes match");
            ck(blob[0x60]==0&&blob[0x61]==0&&blob[0x62]==0&&blob[0x63]==0,"limit word unpatched");
            ck(blob[0x64]==0&&blob[0x65]==0&&blob[0x66]==0&&blob[0x67]==0,"epilogue word unpatched");
            u32 dt=0;
            ck(DecodeThumbCall(0x36,&blob[0x36],dt),"assembled frag call decodes");
            ck(blob[0x68]==0x00&&blob[0x69]==0x05&&blob[0x6A]==0x21&&blob[0x6B]==0x00,
               "block-range word assembled");
        }
    }
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
