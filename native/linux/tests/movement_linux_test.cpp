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
extern "C" void RoadRelayFixture(uintptr_t,uint64_t,uint32_t,int32_t,int32_t,uintptr_t,RoadBounds);
// Engine calling AddToEdgeUseManager's Add: the engine sits in r12, r9 is junk.
asm(".text\n.type RoadRelayFixture,@function\nRoadRelayFixture:\n"
    "push %r12\nmov %r9,%r12\nmov $0x5a5a5a5a,%r9\ncall SliceRoadEntriesRelay\npop %r12\nret\n");
struct AddSeen { uintptr_t mgr; uint64_t edge; uint32_t forward; int32_t entity, comp; RoadBounds bounds; int calls; } addSeen;
static void FakeAdd(uintptr_t mgr,uint64_t edge,uint32_t forward,int32_t entity,int32_t comp,RoadBounds bounds)
{ addSeen={mgr,edge,forward,entity,comp,bounds,addSeen.calls+1}; }
static uintptr_t fakeTypeNode[3];
static uintptr_t FakeTypeFind(uintptr_t,const uintptr_t*) { return uintptr_t(fakeTypeNode); }
struct RoadEntry { int32_t entity, comp; float back, front; uint8_t forward, pad[3]; };
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
    for (const auto& c:kIconChecks) memcpy(image+c.rva,c.bytes,c.size);
    for (const auto& c:kForeignWindowChecks) memcpy(image+c.rva,c.bytes,c.size);
    image[kIconChecks[0].rva]^=1;
    assert(!Check(uintptr_t(image),kIconChecks));
    for (const auto& p:iconPatches) assert(!memcmp(image+p.rva,p.before,p.size));
    image[kIconChecks[0].rva]^=1;
    assert(Check(uintptr_t(image),kIconChecks));
    image[iconPatches[3].rva]^=1;
    assert(!ApplyUiPatches(uintptr_t(image),iconPatches));
    assert(!memcmp(image+iconPatches[0].rva,iconPatches[0].before,2));
    image[iconPatches[3].rva]^=1;
    assert(ApplyUiPatches(uintptr_t(image),iconPatches));
    for (const auto& p:iconPatches) assert(!memcmp(image+p.rva,p.after,p.size));
    assert(Check(uintptr_t(image),kForeignWindowChecks));
    assert(ApplyUiPatches(uintptr_t(image),foreignWindowPatches));
    assert(!memcmp(image+0x1446d54,foreignWindowPatches[0].after,6));
    munmap(image,size);
    char dir[]="/tmp/tpf2mp-companies.XXXXXX"; assert(mkdtemp(dir));
    const std::string cfg=std::string(dir)+"/mp_company_cfg.txt";
    const std::string status=std::string(dir)+"/lockstep_status_a.txt";
    auto write=[](const std::string& path,const char* text) { FILE* f=fopen(path.c_str(),"w"); assert(f); fputs(text,f); fclose(f); };
    write(cfg,"coop\n"); assert(!ReadCompanies(dir,"a"));
    write(status,"t=1 cm=companies\n"); assert(ReadCompanies(dir,"a")); assert(!ReadCompanies(dir,"b"));
    write(status,"t=2 cm=coop\n"); assert(!ReadCompanies(dir,"a"));
    write(cfg,"companies\n"); assert(ReadCompanies(dir,"a")); assert(ReadCompanies(dir,""));
    const std::string perms=std::string(dir)+"/mp_company_perms.txt";
    StationPermissions rules; rules.Read(dir); assert(rules.Allows(101,102));
    write(perms,"pid 101 1\npid 102 2\npid 103 3\nopen 1 -\nopen 2 *\nopen 3 2,11\n");
    rules.Read(dir);
    assert(!rules.Allows(101,102) && rules.Allows(102,101));
    assert(rules.Allows(103,102) && !rules.Allows(103,101));
    assert(rules.Allows(101,101) && rules.Allows(999,101) && rules.Allows(101,999));
    write(perms,"pid 101 1\npid 102 2\nopen 1 12,21\nopen 999 -\n");
    rules.Read(dir); assert(!rules.Allows(101,102)); // IDs are whole tokens
    write(perms,"pid 101 1\npid 102 2\n");
    rules.Read(dir); assert(rules.Allows(101,102)); // absent rule defaults open
    unlink(perms.c_str()); rules.Read(dir); assert(rules.Allows(101,102));
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
    // ROAD ENTRY ORDER: synthetic manager in Add's layout, names in the Linux Name layout.
    {
        assert(sizeof(RoadEntry)==ROADENTRY_SIZE);
        std::vector<RoadEntry> entries{{30,1,0,4,1,{}},{10,2,0,4,0,{}},{20,3,0,4,1,{}}};
        uint8_t edgeData[32]{}; float len=50; memcpy(edgeData,&len,4);
        auto vec=[](uint8_t* at,uintptr_t b,uintptr_t e){ memcpy(at,&b,8); memcpy(at+8,&e,8); memcpy(at+16,&e,8); };
        vec(edgeData+8,uintptr_t(entries.data()),uintptr_t(entries.data()+3));
        std::vector<uint8_t> groups(2*72);
        vec(groups.data()+72,uintptr_t(edgeData)-32,uintptr_t(edgeData)+32);   // group 1: datas[1] is edgeData
        std::vector<int32_t> indices{-1,1};
        std::vector<uint8_t> mgr(0x80);
        vec(mgr.data()+0x30,uintptr_t(indices.data()),uintptr_t(indices.data()+2));
        vec(mgr.data()+0x48,uintptr_t(groups.data()),uintptr_t(groups.data()+groups.size()));
        const uint64_t edge=(uint64_t(1)<<32)|1;
        assert(RoadEdgeData(uintptr_t(mgr.data()),edge)==uintptr_t(edgeData));
        assert(!RoadEdgeData(uintptr_t(mgr.data()),(uint64_t(1)<<32)|0));   // index -1
        assert(!RoadEdgeData(uintptr_t(mgr.data()),(uint64_t(2)<<32)|1));   // past datas
        assert(!RoadEdgeData(uintptr_t(mgr.data()),2));                     // past indices
        assert(!RoadEdgeData(uintptr_t(mgr.data()),uint64_t(0xffffffffu)));
        // world: entity slots at +98 (24-byte vectors of {type,slot}), pools at +80, flat data at pool+b8
        const int type=2;
        std::string names[2]={"bus a","Bus B"};
        std::vector<uint8_t> pool(0xc0); uintptr_t data=uintptr_t(names); memcpy(pool.data()+0xb8,&data,8);
        uintptr_t pools[3]={0,0,uintptr_t(pool.data())};
        std::vector<uint8_t> slots(31*24);
        int32_t slot30[2]={type,0}, slot10[2]={type,1}, other20[2]={type+1,0};
        vec(slots.data()+30*24,uintptr_t(slot30),uintptr_t(slot30)+8);
        vec(slots.data()+10*24,uintptr_t(slot10),uintptr_t(slot10)+8);
        vec(slots.data()+20*24,uintptr_t(other20),uintptr_t(other20)+8);
        std::vector<uint8_t> world(0xa0);
        uintptr_t sp=uintptr_t(slots.data()), pp=uintptr_t(pools);
        memcpy(world.data()+0x98,&sp,8); memcpy(world.data()+0x80,&pp,8);
        const auto original=entries;
        // no names: ids decide
        assert(RoadEntriesSortAt(0,uintptr_t(mgr.data()),edge,-1)==RoadSorted);
        assert(entries[0].entity==10 && entries[1].entity==20 && entries[2].entity==30);
        assert(entries[0].comp==2 && entries[0].forward==0 && entries[2].comp==1 && entries[2].forward==1);
        assert(RoadEntriesSortAt(0,uintptr_t(mgr.data()),edge,-1)==RoadUnchanged);
        entries=original;
        vec(edgeData+8,uintptr_t(entries.data()),uintptr_t(entries.data()+3));
        // names, case-insensitive; the unnamed entity sorts first
        fakeTypeNode[2]=type+1;
        movementBase=uintptr_t(&FakeTypeFind)-0x9e3d50;
        roadEntriesOriginal=&FakeAdd;
        const RoadBounds b{1.5f,7.25f};
        RoadRelayFixture(uintptr_t(mgr.data()),edge,1,30,77,uintptr_t(world.data()),b);
        assert(addSeen.calls==1 && addSeen.mgr==uintptr_t(mgr.data()) && addSeen.edge==edge && addSeen.forward==1 &&
               addSeen.entity==30 && addSeen.comp==77 && addSeen.bounds.back==1.5f && addSeen.bounds.front==7.25f);
        assert(entries[0].entity==20 && entries[1].entity==30 && entries[2].entity==10);
        assert(reSorted.load()==1 && reRefused.load()==0);
        // a span that is not whole 20-byte entries is refused and left alone
        const auto sorted=entries;
        uintptr_t oddEnd=uintptr_t(entries.data())+50; memcpy(edgeData+0x10,&oddEnd,8);
        assert(RoadEntriesSortAt(uintptr_t(world.data()),uintptr_t(mgr.data()),edge,type)==RoadRefused);
        assert(!memcmp(entries.data(),sorted.data(),3*ROADENTRY_SIZE));
        // more than ROADENTRIES_MAX vehicles on one edge: refused, untouched
        std::vector<RoadEntry> many(ROADENTRIES_MAX+1);
        for (size_t i=0;i<many.size();++i) many[i].entity=int32_t(many.size()-i);
        const auto manyBefore=many;
        vec(edgeData+8,uintptr_t(many.data()),uintptr_t(many.data()+many.size()));
        assert(RoadEntriesSortAt(0,uintptr_t(mgr.data()),edge,-1)==RoadRefused);
        assert(!memcmp(many.data(),manyBefore.data(),many.size()*ROADENTRY_SIZE));
        movementBase=0;
        // byte guard: mutated evidence refuses
        const size_t isize=0x2e5a000;
        auto* img=static_cast<uint8_t*>(mmap(nullptr,isize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(img!=MAP_FAILED);
        for (const auto& c:kRoadEntriesChecks) memcpy(img+c.rva,c.bytes,c.size);
        assert(Check(uintptr_t(img),kRoadEntriesChecks));
        assert(img[0x16c484a]==0xe8);
        int32_t rel=0; memcpy(&rel,img+0x16c484b,4); assert(0x16c484f+rel==0x2e58f70);
        img[0x2e59441+20]^=1; assert(!Check(uintptr_t(img),kRoadEntriesChecks));
        munmap(img,isize);
    }
    assert(argc==2);
    void* library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(library);
    foreignThrow=reinterpret_cast<void(*)()>(dlsym(library,"SliceTestForeignThrow"));
    auto foreignCatch=reinterpret_cast<bool(*)(void(*)())>(dlsym(library,"SliceTestForeignCatch"));
    assert(foreignThrow && foreignCatch);
    filteredOriginal=reinterpret_cast<void*>(&ForeignFiltered);
    assert(foreignCatch(&ForeignCallback)); assert(!FilterContext::active);
    dlclose(library);
    puts("Linux movement: original term sums, immutable diagnostics, byte refusal, SysV station relay, road entry order PASS");
}
