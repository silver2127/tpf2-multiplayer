// Execute the seven hot-join patches in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/order_canon_linux.cpp"
#include "../src/codewrite_linux.h"
#include <random>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <vector>
#include <immintrin.h>
#include <thread>
#include <algorithm>
#include <cstdio>

struct State {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags;
    unsigned char xmm[16][16];
    uint64_t rspBefore, rspAfter;
};
static_assert(offsetof(State, flags) == 120);
static_assert(offsetof(State, xmm) == 128);
static_assert(offsetof(State, rspBefore) == 384);
static_assert(offsetof(State, rspAfter) == 392);

static void Write(uintptr_t address, const void* bytes, size_t size)
{
    int error = 0;
    assert(Tpf2mpCodeWriteSelf(address, static_cast<const uint8_t*>(bytes), size, &error) == TPF2MP_CW_OK);
}

static std::array<unsigned char, 14> Jump(uintptr_t target)
{
    std::array<unsigned char, 14> bytes{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(bytes.data() + 6, &target, sizeof(target));
    return bytes;
}

// Minimal machine-code fixture: load all supplied registers, jump through the
// installed patch, then capture registers before the fixture itself uses them.
// The fixture keeps its own output pointer and saved caller registers above
// the interrupted RSP, including both possible interrupted stack alignments.
struct Emitter {
    std::vector<unsigned char> code;
    void Bytes(std::initializer_list<unsigned char> bytes) { code.insert(code.end(), bytes); }
    void U32(uint32_t x) { for (unsigned i = 0; i < 4; ++i) code.push_back(x >> (i * 8)); }
    void JumpTo(uintptr_t target) { const auto bytes = Jump(target); code.insert(code.end(), bytes.begin(), bytes.end()); }
    void LoadGP(unsigned reg, uint32_t offset) {
        Bytes({static_cast<unsigned char>(0x49 | (reg >= 8 ? 4 : 0)), 0x8b,
               static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // mov reg,[r15+disp32]
    }
    void LoadXMM(unsigned reg, uint32_t offset) {
        Bytes({0xf3, static_cast<unsigned char>(reg >= 8 ? 0x45 : 0x41), 0x0f, 0x6f,
               static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // movdqu xmm,[r15+disp32]
    }
    void StoreXMM(unsigned reg, uint32_t offset) {
        Bytes({0xf3}); if (reg >= 8) Bytes({0x44});
        Bytes({0x0f, 0x7f, static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // movdqu [rdi+disp32],xmm
    }
};

using Fixture = void (*)(const State*, State*);

static Fixture MakeFixture(uintptr_t base, size_t site, unsigned alignment,
                           unsigned char* page)
{
    Emitter out;
    // Entry RSP%16=8, six saved registers preserve that residue.
    const unsigned char frame = alignment == 0 ? 24 : 16;
    out.Bytes({0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    out.Bytes({0x48, 0x83, 0xec, frame}); // sub rsp,frame
    out.Bytes({0x48, 0x89, 0x34, 0x24}); // mov [rsp],rsi (output)
    out.Bytes({0x48, 0x89, 0xa6}); out.U32(384); // mov [rsi+384],rsp
    out.Bytes({0x49, 0x89, 0xff}); // mov r15,rdi (input)
    for (unsigned i = 0; i < 16; ++i) out.LoadXMM(i, 128 + i * 16);
    // Hardware register numbers in the snapshot's memory order.
    const unsigned regs[] = {15, 14, 13, 12, 5, 3, 11, 10, 9, 8, 7, 6, 2, 1, 0};
    for (unsigned i = 1; i < 15; ++i) out.LoadGP(regs[i], i * 8);
    out.Bytes({0x41, 0xff, 0x77, 0x78, 0x9d}); // push [r15+120]; popfq
    out.LoadGP(15, 0); // release the final fixture pointer
    out.JumpTo(base + kCanonSites[site].rva);

    const uintptr_t continuation = reinterpret_cast<uintptr_t>(page) + out.code.size();
    out.Bytes({0x9c, 0x50, 0x51, 0x52, 0x56, 0x57,
               0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53, 0x55,
               0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    out.Bytes({0x48, 0x8b, 0xbc, 0x24}); out.U32(128); // output at original RSP
    for (unsigned i = 0; i < 16; ++i) out.StoreXMM(i, 128 + i * 16);
    out.Bytes({0x48, 0x8d, 0x84, 0x24}); out.U32(128); // interrupted RSP
    out.Bytes({0x48, 0x89, 0x87}); out.U32(392); // output.rspAfter
    out.Bytes({0x48, 0x89, 0xe6, 0xb9, 0x10, 0, 0, 0, 0xfc, 0xf3, 0x48, 0xa5}); // copy GP/flags
    out.Bytes({0x48, 0x8d, 0xa4, 0x24}); out.U32(128 + frame);
    out.Bytes({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3});
    assert(out.code.size() < 4096);
    Write(reinterpret_cast<uintptr_t>(page), out.code.data(), out.code.size());
    const auto jump=Jump(continuation);
    Write(base+kCanonSites[site].rva+kCanonSites[site].steal,jump.data(),jump.size());
    return reinterpret_cast<Fixture>(page);
}


static unsigned attempt=0,failAt=0;
static bool FailHook(uintptr_t at,void*,int n,void**) {
    unsigned char bad[32];memset(bad,0xcc,n);Write(at,bad,n);return ++attempt!=failAt;
}
static void Restore(uintptr_t base) {
    g_canonReady=false;
    const unsigned char get[]={0xf3,0x0f,0x1e,0xfa,0x48,0x8d,0x47,0x08,0xc3};
    const unsigned char none[]={0xf3,0x0f,0x1e,0xfa,0x31,0xc0,0xc3};
    Write(base+0xa914c0,get,sizeof(get)); Write(base+0xa914e0,none,sizeof(none));
    for(const auto& s:kCanonSites)Write(base+s.rva,s.bytes,sizeof(s.bytes));
}
struct Node { uintptr_t next; int32_t key; int32_t payload; };
static void MapTests() {
    uintptr_t map[7]{};
    Node nodes[]={{0,8,123},{0,1,456},{0,4,789}};
    nodes[0].next=reinterpret_cast<uintptr_t>(&nodes[1]);nodes[1].next=reinterpret_cast<uintptr_t>(&nodes[2]);
    map[2]=reinterpret_cast<uintptr_t>(nodes);map[3]=4;
    const auto first=map[2];const auto next=nodes[0].next;
    assert(!RelinkMap(reinterpret_cast<uintptr_t>(map)) && map[2]==first && nodes[0].next==next);
    map[3]=3;assert(RelinkMap(reinterpret_cast<uintptr_t>(map)));
    assert(map[2]==reinterpret_cast<uintptr_t>(&nodes[1]));
    assert(nodes[1].next==reinterpret_cast<uintptr_t>(&nodes[2]) && nodes[2].next==first && !nodes[0].next);
    assert(nodes[0].payload==123 && nodes[1].payload==456 && nodes[2].payload==789);
    nodes[0].next=first;assert(!RelinkMap(reinterpret_cast<uintptr_t>(map))); // cycle
    map[3]=kMapMaxNodes+1;assert(!RelinkMap(reinterpret_cast<uintptr_t>(map)));
}
static void FreedIdTests() {
    std::array<uintptr_t,80> engine{};
    const auto owner=reinterpret_cast<uintptr_t>(engine.data());
    const auto refused=g_canonCounters[5].refused.load();
    CanonSort(5,0,owner); // absent modification payload
    assert(g_canonCounters[5].refused==refused+1);
    uintptr_t vec[3]{};
    engine[0x208/8]=reinterpret_cast<uintptr_t>(vec);
    CanonSort(5,0,owner); // empty batch
    assert(g_canonCounters[5].refused==refused+1);
    int32_t ids[]={9,2,9,1};
    vec[0]=reinterpret_cast<uintptr_t>(ids);
    vec[1]=vec[0]+sizeof(ids);vec[2]=vec[1]-4;
    CanonSort(5,0,owner); // end exceeds capacity; do not mutate
    assert(g_canonCounters[5].refused==refused+2 && ids[0]==9);
    vec[2]=vec[1];CanonSort(5,0,owner);
    assert(ids[0]==1 && ids[1]==2 && ids[2]==9 && ids[3]==9);
    const auto reordered=g_canonCounters[5].reordered.load();
    CanonSort(5,0,owner);
    assert(g_canonCounters[5].reordered==reordered); // sorted batch stays put
}
static void FamilyTests(uintptr_t base) {
    g_familyBase=base;
    std::array<uintptr_t,80> engine{};
    uintptr_t vtable[]={0,0,base+0xa914c0};
    for(unsigned n=1;n<=5;++n) {
        const size_t stride=n+1;
        std::vector<int32_t> nodes(3*stride);
        const int32_t keys[]={9,2,5};
        for(size_t i=0;i<3;++i) {
            nodes[i*stride]=keys[i];
            for(size_t j=1;j<stride;++j) nodes[i*stride+j]=keys[i]*100+j;
        }
        int8_t ctrl[]={0,1,2,-128,-128,-128,-128};
        int32_t slots[]={9,0,2,1,5,2,0,0,0,0,0,0,0,0};
        uintptr_t family[]={reinterpret_cast<uintptr_t>(vtable),base+0x59ac260+0x20*(n-1),
            reinterpret_cast<uintptr_t>(nodes.data()),reinterpret_cast<uintptr_t>(nodes.data()+nodes.size()),
            reinterpret_cast<uintptr_t>(nodes.data()+nodes.size()),reinterpret_cast<uintptr_t>(ctrl),
            reinterpret_cast<uintptr_t>(slots),3,7};
        uintptr_t node[]={0,0,reinterpret_cast<uintptr_t>(family)};
        engine[0x170/8]=reinterpret_cast<uintptr_t>(node); engine[0x178/8]=1;
        const auto addr=reinterpret_cast<uintptr_t>(engine.data());
        const auto original=nodes;
        // Chain validation finishes before a node list can be changed.
        node[0]=reinterpret_cast<uintptr_t>(node); CanonFamilies(addr); assert(nodes==original); node[0]=0;
        slots[1]=2; CanonFamilies(addr); assert(nodes==original); slots[1]=0;
        CanonFamilies(addr);
        assert(nodes[0]==2 && nodes[stride]==5 && nodes[2*stride]==9);
        for(size_t i=0;i<3;++i) for(size_t j=1;j<stride;++j)
            assert(nodes[i*stride+j]==nodes[i*stride]*100+int(j));
        assert(slots[1]==2 && slots[3]==0 && slots[5]==1);
        auto reordered=g_canonCounters[6].reordered.load();
        CanonFamilies(addr); assert(g_canonCounters[6].reordered==reordered);
        family[2]=1; CanonFamilies(addr); // inaccessible vector: refusal, no crash
        family[0]=1; CanonFamilies(addr); // inaccessible vtable
    }
    assert(FamilyMappings());
    auto* page=mmap(nullptr,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); assert(page!=MAP_FAILED);
    assert(FamilyMappings());
    assert(Readable(page,4096) && !FamilyRange(reinterpret_cast<uintptr_t>(page),4096,true));
    assert(!FamilyRange(UINTPTR_MAX-3,8));
    munmap(page,4096);
}
int main() {
    MapTests();
    FreedIdTests();
    const size_t size=0x3258000;
    auto* image=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    const auto base=reinterpret_cast<uintptr_t>(image);
    Restore(base);assert(!mprotect(image,size,PROT_READ|PROT_EXEC));
    for (const char* value : {static_cast<const char*>(nullptr), "", "1", "0", "00", "false"}) {
        Restore(base);
        if (value) setenv("TPF2MP_ORDER_CANON",value,1); else unsetenv("TPF2MP_ORDER_CANON");
        const bool enabled = !value || std::strcmp(value,"0");
        assert(Tpf2mpInstallOrderCanon(base,kCanonBuildId) == enabled);
        if (!enabled) {
            assert(!std::strcmp(g_canonStatus.load(),"off (TPF2MP_ORDER_CANON=0)"));
            for (const auto& site:kCanonSites)
                assert(!memcmp(reinterpret_cast<void*>(base+site.rva),site.bytes,16));
        }
    }
    Restore(base);
    setenv("TPF2MP_ORDER_CANON","1",1);assert(!Tpf2mpInstallOrderCanon(base,"wrong"));
    for(const auto& s:kCanonSites) {
        Restore(base);const unsigned char bad=s.bytes[15]^1;Write(base+s.rva+15,&bad,1);
        assert(!Tpf2mpInstallOrderCanon(base,kCanonBuildId));
    }
    for(unsigned failure=1;failure<=kCanonSiteCount;++failure) {
        Restore(base);attempt=0;failAt=failure;
        assert(!InstallOrderCanon(base,kCanonBuildId,FailHook));
        for(const auto& s:kCanonSites)assert(!memcmp(reinterpret_cast<void*>(base+s.rva),s.bytes,16));
    }
    for(auto accessor : {0xa914c0,0xa914e0}) {
        Restore(base); const unsigned char bad=0xcc; Write(base+accessor,&bad,1);
        assert(!Tpf2mpInstallOrderCanon(base,kCanonBuildId));
    }
    FamilyTests(base);
    auto* code=static_cast<unsigned char*>(mmap(nullptr,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(code!=MAP_FAILED);
    std::mt19937_64 rng(35924);
    for(unsigned site=0;site<kCanonSiteCount;++site)for(unsigned alignment=0;alignment<2;++alignment) {
        Restore(base);assert(Tpf2mpInstallOrderCanon(base,kCanonBuildId));
        std::array<unsigned char,0x1800> stack{};
        std::array<uintptr_t,80> system{};std::array<uintptr_t,64> maps{};
        std::vector<int32_t> ids={8,2,4,1};
        uintptr_t vec[]={reinterpret_cast<uintptr_t>(ids.data()),reinterpret_cast<uintptr_t>(ids.data()+ids.size()),reinterpret_cast<uintptr_t>(ids.data()+ids.size())};
        State in{},out{};auto* gp=reinterpret_cast<uint64_t*>(&in);
        for(unsigned i=0;i<15;++i)gp[i]=rng();
        for(auto& x:in.xmm)for(auto& b:x)b=static_cast<unsigned char>(rng());
        in.flags=0x202;in.rbp=reinterpret_cast<uintptr_t>(stack.data()+0x1400);
        const auto slot=in.rbp+kCanonSites[site].beginOff;
        if(site<3)memcpy(reinterpret_cast<void*>(slot),vec,sizeof(vec));
        else if(site==6) {
            in.rdi=reinterpret_cast<uintptr_t>(system.data());
        } else if(site==5) {
            in.r13=reinterpret_cast<uintptr_t>(system.data());
            system[0x208/8]=reinterpret_cast<uintptr_t>(vec);
        } else {
            const auto sys=reinterpret_cast<uintptr_t>(system.data());memcpy(reinterpret_cast<void*>(slot),&sys,8);
            if(site==3)memcpy(system.data()+3,vec,sizeof(vec));
            else system[0x120/8]=reinterpret_cast<uintptr_t>(maps.data());
        }
        uintptr_t load[]={0,0x12345678};uintptr_t output=1;
        if(site==0)in.rbx=reinterpret_cast<uintptr_t>(&output);
        if(site==1)in.rdx=reinterpret_cast<uintptr_t>(load);
        if(site==2)in.rax=reinterpret_cast<uintptr_t>(load);
        State expected=in;
        if(site==6) {expected.r12=system[6];expected.r14=system[7];}
        if(site==1 || site==2) {
            expected.rdi=load[1];
            asm volatile("testq %1,%1; pushfq; popq %0":"=r"(expected.flags):"r"(load[1]):"cc");
        }
        if(site==3)expected.rcx=reinterpret_cast<uintptr_t>(system.data());
        if(site==5)expected.rax=reinterpret_cast<uintptr_t>(vec);
        if(site==4)expected.rax=reinterpret_cast<uintptr_t>(system.data());
        auto fixture=MakeFixture(base,site,alignment,code);
        const auto oldMxcsr=_mm_getcsr();const auto mxcsr=(oldMxcsr&~0x6000u)|(alignment<<13);_mm_setcsr(mxcsr);
        fixture(&in,&out);assert(_mm_getcsr()==mxcsr);_mm_setcsr(oldMxcsr);
        assert(!memcmp(&out,&expected,128));assert(!memcmp(out.xmm,in.xmm,sizeof(in.xmm)));assert(out.rspBefore==out.rspAfter);
        if(site<4 || site==5)assert((ids==std::vector<int32_t>{1,2,4,8}));
        if(site==0)assert(output==0);
    }
    munmap(code,4096);munmap(image,size);
    puts("canonical order: seven real shims, GP/XMM/flags/MXCSR/RSP, both alignments, all guards and rollback, map integrity passed");
}
