#include <cassert>
#include <sys/mman.h>
#include <dlfcn.h>
#include <array>
#include <algorithm>
#include "slice/slice_core_internal.h"
#include "../src/slice/movement_linux.cpp"
template<class T> void Put(std::vector<uint8_t>& mem,size_t at,T v) { memcpy(mem.data()+at,&v,sizeof(v)); }
extern "C" int StationFixture(int,int);
extern "C" void StationReturn();
asm(".text\n.type StationFixture,@function\nStationFixture:\n"
    "push %rbx\nsub $56,%rsp\nmov %rsp,%rbx\nmov %esi,40(%rbx)\nmov %edi,%eax\n"
    "call SliceStationRelay\nadd $56,%rsp\npop %rbx\nret\n"
    ".type StationReturn,@function\nStationReturn:\nret\n");
extern "C" float TermFixture(void*,float);
asm(".text\n.type TermFixture,@function\nTermFixture:\n"
    "push %rbp\nmov %rsp,%rbp\nsub $88,%rsp\nmovl $0,-0x44(%rbp)\n"
    "call *%rdi\nadd $88,%rsp\npop %rbp\nret\n");
extern "C" float PlainTermFixture(void*,float);
asm(".text\n.type PlainTermFixture,@function\nPlainTermFixture:\n"
    "sub $8,%rsp\nmovaps %xmm0,%xmm2\npxor %xmm0,%xmm0\n"
    "sub $8,%rsp\ncall *%rdi\nadd $16,%rsp\nret\n");
static float FakePlain(uintptr_t,uintptr_t)
{
    PlainTermFixture(reinterpret_cast<void*>(&SliceRoadRelayA),16777216.0f);
    PlainTermFixture(reinterpret_cast<void*>(&SliceRoadRelayB),1.0f);
    PlainTermFixture(reinterpret_cast<void*>(&SliceRoadRelayA),1.0f);
    return 16777216.0f;
}
static float FakeFiltered(uintptr_t mode,uintptr_t,uintptr_t)
{
    if (mode==1) {
        SliceFilteredTerm(1.0f);
        assert(FilteredHook(0,0,0)==16777218.0f); // nested context
        SliceFilteredTerm(2.0f); return 3.0f;
    }
    if (mode==2) { SliceFilteredTerm(1); throw 42; }
    if (mode==3) { for(int i=0;i<513;++i) SliceFilteredTerm(1); return 123.0f; }
    TermFixture(reinterpret_cast<void*>(&SliceFilteredRelayA),16777216.0f);
    TermFixture(reinterpret_cast<void*>(&SliceFilteredRelayB),1.0f);
    TermFixture(reinterpret_cast<void*>(&SliceFilteredRelayA),1.0f);
    return 16777216.0f;
}
static void (*foreignThrow)();
static float ForeignFiltered(uintptr_t,uintptr_t,uintptr_t) { foreignThrow(); return 0; }
static void ForeignCallback() { FilteredHook(0,0,0); }
int main(int argc,char** argv)
{
    assert(SliceReadInit());
    std::vector<uint8_t> self(0x60);
    // Observers copy their inputs; no writes to the family, even on refusal.
    std::vector<std::array<int32_t,5>> nodes{{9,0,0,0,0},{3,0,0,0,0}};
    const auto saved=nodes; Put(self,8,uintptr_t(&nodes)); MoveSample sample{};
    assert(Measure(uintptr_t(self.data()),0,2,-1,&sample)); assert(nodes==saved);
    assert(!Measure(uintptr_t(self.data()),0,3,-1,&sample)); assert(nodes==saved);
    assert(!Measure(uintptr_t(self.data()),0,-1,-1,&sample));
    // Unmapped and one-byte-mutated evidence must never install a hook.
    assert(!Check(0,kRoadChecks));
    const size_t size=0x5a04000;
    auto* image=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(image!=MAP_FAILED);
    for (const auto& c:kRoadChecks) memcpy(image+c.rva,c.bytes,c.size);
    assert(Check(uintptr_t(image),kRoadChecks)); image[kRoadChecks[0].rva+50]^=1;
    assert(!Check(uintptr_t(image),kRoadChecks));
    for (const auto& c:kPausedChecks) memcpy(image+c.rva,c.bytes,c.size);
    image[kPausedChecks[4].rva]^=1;
    assert(!InstallPausedTick(uintptr_t(image),"",""));
    assert(!memcmp(image+0xa61860,kPausedChecks[1].bytes+16,5));
    image[kPausedChecks[4].rva]^=1;
    assert(InstallPausedTick(uintptr_t(image),"",""));
    assert(!memcmp(image+0xa61860,"\x0f\x1f\x44\x00\x00",5));
    assert(!memcmp(image+0xa617c3,kPausedChecks[2].bytes,kPausedChecks[2].size));
    munmap(image,size);
    char dir[]="/tmp/tpf2mp-companies.XXXXXX"; assert(mkdtemp(dir));
    const std::string cfg=std::string(dir)+"/mp_company_cfg.txt";
    const std::string status=std::string(dir)+"/lockstep_status_a.txt";
    auto write=[](const std::string& path,const char* text) { FILE* f=fopen(path.c_str(),"w"); assert(f); fputs(text,f); fclose(f); };
    write(cfg,"coop\n"); assert(!ReadCompanies(dir,"a"));
    write(status,"t=1 cm=companies\n"); assert(ReadCompanies(dir,"a")); assert(!ReadCompanies(dir,"b"));
    write(status,"t=2 cm=coop\n"); assert(!ReadCompanies(dir,"a"));
    write(cfg,"companies\n"); assert(ReadCompanies(dir,"a")); assert(ReadCompanies(dir,""));
    unlink(cfg.c_str()); unlink(status.c_str()); rmdir(dir);
    // Execute only the handwritten relay and synthetic frame, no ELF code.
    SliceStationResume=uintptr_t(&StationReturn);
    assert(StationFixture(4,4)==1); assert(StationFixture(4,5)==0);
    SliceRoadResumeA=SliceRoadResumeB=uintptr_t(&StationReturn);
    roadOriginal=reinterpret_cast<void*>(&FakePlain);
    assert(RoadHook(0,0)==16777218.0f && !FilterContext::active);
    SliceFilteredResumeA=SliceFilteredResumeB=uintptr_t(&StationReturn);
    filteredOriginal=reinterpret_cast<void*>(&FakeFiltered);
    assert(FilteredHook(0,0,0)==16777218.0f && !FilterContext::active);
    assert(FilteredHook(1,0,0)==3.0f && !FilterContext::active);
    assert(FilteredHook(3,0,0)==123.0f && !FilterContext::active); // cap: original answer
    try { FilteredHook(2,0,0); assert(false); } catch (int n) { assert(n==42); }
    assert(!FilterContext::active);
    assert(argc==2);
    void* library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(library);
    foreignThrow=reinterpret_cast<void(*)()>(dlsym(library,"SliceTestForeignThrow"));
    auto foreignCatch=reinterpret_cast<bool(*)(void(*)())>(dlsym(library,"SliceTestForeignCatch"));
    assert(foreignThrow && foreignCatch);
    filteredOriginal=reinterpret_cast<void*>(&ForeignFiltered);
    assert(foreignCatch(&ForeignCallback)); assert(!FilterContext::active);
    dlclose(library);
    puts("Linux movement: original term sums, immutable diagnostics, byte refusal, SysV station relay PASS");
}
