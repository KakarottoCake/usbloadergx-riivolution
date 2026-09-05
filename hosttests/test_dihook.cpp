// d2x-v11-beta3 devkitARM disassembly fixture, plus relocation/rejection tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iterator>
#include "riivo/RiivoDiHook.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok,const char *what) {
    ++checks; if(!ok){++failures;printf("FAIL: %s\n",what);}
}
static void hex(std::vector<u8>&v,u32 at,const char *s) {
    for(;*s;s+=2,++at) { char b[]={s[0],s[1],0}; v[at]=strtoul(b,0,16); }
}
static std::vector<u8> fixture() {
    std::vector<u8> v(0x2000);
    hex(v,0x9a8,"6803B5F74DAB0006000F0E1B9200");
    hex(v,0x9d4,"9A0000390030");
    EncodeThumbCall(0x9da,0x332,&v[0x9da]);
    hex(v,0xb4a,"682B079BD400E740688268410038F7FFFEF60004E7DB");
    hex(v,0xc5c,"1383D1E4");
    hex(v,0x948,"B5704C1368632B01D0092B02D1094B11429AD3064B1020A002006123BD704B0FE7F668A368E5195B189A682306DCD502F7FFFF0AE7F2075CD502F000FB19E7ED07DBD502F7FFFC4CE7E8F7FFFCABE7E51383D1E47ED380000005210046090000");
    hex(v,0x790,"B5F00017B085000A0005000CAB030001");
    hex(v,0x228,"B5F02400B0870A53002790059104920393009B04429FD3020020B007BDF09B049A031BDD9B002600025B429AD9011AD3009E9B05228019DB0112002900189302F000F87E2800D0012E00D0262280197301124293D9001B954B182080681B21200100479890012800D02421809A000109F7FFFF781E04D1059B0198021999002AF001F84A4B0E9801681B47989A0019AB0ADB18D3197F9300E7BB23FF000503DB4298D900001D9A0098020029F7FFFF5A0004E7EB24164264E7E846C01383D1D81383D1DC");
    return v;
}
int main(int argc,char **argv) {
    std::vector<u8> v=fixture();
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);
        std::vector<u8> elf((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        if(elf.size()<0x394c || memcmp(&elf[0],"\x7f" "ELF",4)) return 2;
        v.assign(elf.begin()+0x2000,elf.begin()+0x394c);
    }
    DiHookPlan p; std::string why;
    const u32 base=0x93300000,site=base+0xb4a;
    ck(BuildDiHook(&v[0],v.size(),base,site,0x68000000,p,why),"supported image");
    if(!p.code.size()) {printf("%s\n",why.c_str());return 1;}
    ck(p.storage==base+0x228 && p.dispatch==site,"resolved RX storage");
    ck(p.code.size()==136 && p.branch.size()==6,"bounded patch size");
    u32 target=0;
    ck(DecodeThumbCall(site,&p.branch[0],target)&&target==base+0x230,"dispatch target");
    ck(DecodeThumbCall(p.storage+0x46,&p.code[0x46],target)&&target==base+0x790,"fragment target");
    ck(DecodeThumbCall(p.storage+0x5c,&p.code[0x5c],target)&&target==base+0x332,"stock target");
    ck(DecodeThumbCall(p.storage+0x68,&p.code[0x68],target)&&target==base+0x948,"raw target");
    ck(p.code[0x7c]==0x68 && p.code[0x7d]==0,"exclusive word limit");
    if(argc>2) {
        std::ofstream out(argv[2],std::ios::binary);
        out.write((const char*)&p.code[0],p.code.size());
        if(!out) return 2;
    }
    const u32 mutate[]={0xb4a,0xb4e,0xb5c,0x9a8,0x9ac,0x9d4,0xc5c,0x948,0x998,0x99c,0x790,0x228,0x268};
    for(u32 i=0;i<sizeof(mutate)/sizeof(mutate[0]);++i) {
        std::vector<u8> bad=v; bad[mutate[i]]^=0x80;
        ck(!BuildDiHook(&bad[0],bad.size(),base,site,0x68000000,p,why),"changed ABI/opcode/literal rejected");
    }
    ck(!BuildDiHook(&v[0],0x900,base,site,0x68000000,p,why),"truncated window");
    ck(!BuildDiHook(&v[0],v.size(),base,site,0x60000000,p,why),"empty mod range");
    ck(!BuildDiHook(&v[0],v.size(),base,site,0x80000001,p,why),"signed-word overflow");
    ck(BuildDiHook(&v[0],v.size(),base,site,0x80000000,p,why),"8 GiB exclusive end");
    u8 b[4];
    for(s64 d=-4194304;d<=4194302;d+=62) {
        const u32 from=0x10000000,to=u32(s64(from)+4+d);
        ck(EncodeThumbCall(from,to,b)&&DecodeThumbCall(from,b,target)&&target==to,"BL roundtrip");
    }
    ck(!EncodeThumbCall(0x10000000,0x10400004,b),"too far forward");
    ck(!EncodeThumbCall(0x10000000,0x0fc00002,b),"too far backward");
    ck(!EncodeThumbCall(0x10000000,0x10000001,b),"unaligned target");
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
