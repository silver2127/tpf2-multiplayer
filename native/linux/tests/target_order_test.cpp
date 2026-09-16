// Execute all five real patch stubs in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/target_order_linux.cpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <vector>
#include <immintrin.h>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <thread>
#include "data/windows_entity_set_oracle.h"

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
// the interrupted RSP, including the path_walk outgoing-argument alignment.
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

static Fixture MakeFixture(uintptr_t base, const TargetSite& site, unsigned alignment,
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
    out.JumpTo(base + site.rva);

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
    const auto jump = Jump(continuation);
    Write(base + (site.rva + site.size), jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}

struct FakeEntry {
    uintptr_t next = 0;
    uint32_t key = 0, pad = 0;
    std::unordered_set<uint32_t> members;
    FakeEntry() { assert(reinterpret_cast<uintptr_t>(&members) - reinterpret_cast<uintptr_t>(this) == 16); }
};
static_assert(sizeof(std::unordered_set<uint32_t>) == 56);
struct FakeOuter {
    uintptr_t native[7]{};
    std::unordered_map<uint32_t, std::unique_ptr<FakeEntry>> entries;
    uintptr_t Address() { return reinterpret_cast<uintptr_t>(this); }
    void Bind() { BindTargetOwner(Address()); }
    void Refresh() { native[2] = entries.empty() ? 0 : reinterpret_cast<uintptr_t>(entries.begin()->second.get()); native[3] = entries.size(); }
    void* Set(uint32_t target) { return &entries.at(target)->members; }
};
static void* FakeLookup(void* opaque, const uint32_t* key)
{
    auto& map = *static_cast<FakeOuter*>(opaque);
    auto at = map.entries.find(*key); return at == map.entries.end() ? nullptr : at->second.get();
}
static void FakeInsert(const uint32_t* id, const int32_t* target, void* opaque)
{
    if (*target < 0) return;
    auto& map = *static_cast<FakeOuter*>(opaque);
    auto& entry = map.entries[uint32_t(*target)];
    if (!entry) { entry = std::make_unique<FakeEntry>(); entry->key = uint32_t(*target); }
    assert(entry->members.insert(*id).second); map.Refresh();
}
static void FakeErase(const uint32_t* id, const int32_t* target, void* opaque)
{
    if (*target < 0) return;
    auto& map = *static_cast<FakeOuter*>(opaque);
    auto at = map.entries.find(uint32_t(*target)); assert(at != map.entries.end());
    assert(at->second->members.erase(*id) == 1);
    if (at->second->members.empty()) map.entries.erase(at);
    map.Refresh();
}
static void FakeClear(void* opaque)
{
    auto& map = *static_cast<FakeOuter*>(opaque); map.entries.clear(); map.Refresh();
}
static void Add(FakeOuter& map, uint32_t target, uint32_t id) { const int32_t key = int32_t(target); TargetInsert(&id, &key, &map); }
static void Erase(FakeOuter& map, uint32_t target, uint32_t id) { const int32_t key = int32_t(target); TargetErase(&id, &key, &map); }
static std::vector<uint32_t> Walk(FakeOuter& map, uint32_t target)
{
    const auto set = reinterpret_cast<uintptr_t>(map.Set(target));
    std::vector<uint32_t> result;
    for (auto node = FirstTargetNode(set); node; node = NextTargetNode(node)) {
        assert(result.size() < TargetRead(set + 0x18)); result.push_back(TargetKey(node));
    }
    return result;
}
static unsigned logCalls;
static void TestLog(const char*, ...) { ++logCalls; }
static void CheckHistory()
{
    g_targetLookup = FakeLookup;
    g_targetOriginal[1] = reinterpret_cast<void*>(FakeClear);
    g_targetOriginal[2] = reinterpret_cast<void*>(FakeInsert);
    g_targetOriginal[3] = reinterpret_cast<void*>(FakeErase);
    g_targetReady = true;
    FakeOuter map; map.Bind();
    Add(map,20835,20840); Add(map,20835,20839);
    assert(Walk(map,20835) == std::vector<uint32_t>({20840,20839}));
    Erase(map,20835,20840); Add(map,20835,20840);
    assert(Walk(map,20835) == std::vector<uint32_t>({20839,20840}));
    // The outer entry and native nodes are destroyed before erase observation.
    Erase(map,20835,20839); Erase(map,20835,20840);
    assert(!map.native[3]); Add(map,20835,20840); Add(map,20835,20839);
    assert(Walk(map,20835) == std::vector<uint32_t>({20840,20839}));
    const int32_t unused = -1; const uint32_t id = 99;
    TargetInsert(&id,&unused,&map); TargetErase(&id,&unused,&map);
    assert(Walk(map,20835).size() == 2);
    FakeOuter nested; nested.Bind(); Add(nested,20835,1); Add(nested,20835,2);
    assert(Walk(nested,20835) == std::vector<uint32_t>({1,2}));
    TargetClearOwner(&nested); assert(Walk(map,20835).size() == 2);
    TargetClearOwner(&map); assert(!g_targetOwners);
    // Real owner addresses are reused after cleanup, never inferred by age.
    for (unsigned i=0;i<40;++i) { map.Bind(); Add(map,1,i); assert(Walk(map,1) == std::vector<uint32_t>{i}); TargetClearOwner(&map); }
    FakeOuter transferred; transferred.Bind(); Add(transferred,1,8); Add(transferred,1,9);
    std::thread receiver([&]{assert(Walk(transferred,1) == std::vector<uint32_t>({8,9})); TargetClearOwner(&transferred);}); receiver.join();
    auto threaded=[](uint32_t first) { for(unsigned i=0;i<100;++i) { FakeOuter local; local.Bind(); Add(local,1,first+i); Add(local,1,first+i+1000); assert(Walk(local,1).size()==2); TargetClearOwner(&local); } };
    std::thread a(threaded,100),b(threaded,20000),c(threaded,40000);a.join();b.join();c.join();
    assert(!g_targetOwners);
    // A missed mutation is diagnosed once and retains native traversal.
    Tpf2mpTargetOrderSetLog(TestLog); logCalls = 0;
    FakeOuter missing; missing.Bind(); Add(missing,1,1);
    const int32_t one = 1; const uint32_t extra = 2; FakeInsert(&extra,&one,&missing);
    const auto native = TargetRead(reinterpret_cast<uintptr_t>(missing.Set(1)) + 0x10);
    assert(FirstTargetNode(reinterpret_cast<uintptr_t>(missing.Set(1))) == native);
    assert(FirstTargetNode(reinterpret_cast<uintptr_t>(missing.Set(1))) == native);
    assert(logCalls == 1 && std::strstr(Tpf2mpTargetOrderStatus(), "incomplete"));
    TargetClearOwner(&missing); Tpf2mpTargetOrderSetLog(nullptr);
    assert(!g_targetOwners);
    g_targetReady = false;
}
static void CheckWindowsOracle()
{
    unsigned operations=0,states=0;
    for(const auto& fixture:kWindowsSetOracleCases) {
        Tpf2mpWindowsEntitySet set{};size_t nextState=0;
        for(size_t i=0;i<fixture.count;++i) {
            const auto& op=fixture.operations[i];
            if(op.kind) assert(Tpf2mpWindowsEntitySetErase(set,op.id));
            else assert(Tpf2mpWindowsEntitySetInsert(set,op.id,uintptr_t(op.id)+1));
            ++operations;
            while(nextState<fixture.stateCount&&fixture.states[nextState].after==i+1) {
                const auto& state=fixture.states[nextState++];std::string order;
                size_t count=0;
                for(auto*node=set.first;node;node=node->next) {
                    if(!order.empty()) order+=',';
                    order+=std::to_string(node->id); ++count;
                    assert(node->nativeNode==uintptr_t(node->id)+1);
                }
                assert(count==set.count && order==state.order && (set.bucketCount?set.bucketCount:8)==state.buckets);++states;
            }
        }
        assert(nextState==fixture.stateCount);Tpf2mpWindowsEntitySetClear(set);
    }
    assert(operations==2459&&states==30);
}
static unsigned attempted,failAt;
static bool FailingHook(uintptr_t at,void*detour,int size,void**original)
{
    if(++attempted==failAt){const uint8_t partial=0xcc;Write(at,&partial,1);return false;}
    return InstallHook(at,detour,size,original);
}
static void CheckInstallFailure(uintptr_t base,unsigned failure)
{
    const auto child=fork();assert(child>=0);
    if(!child){attempted=0;failAt=failure;assert(!InstallTargetOrder(base,kTargetBuildId,FailingHook,Tpf2mpCodeWriteSelf));
        assert(!g_targetActiveMask&&!g_targetReady);
        for(const auto&c:kTargetContexts) assert(!std::memcmp(reinterpret_cast<void*>(base+c.rva),c.bytes,c.size));
        _exit(0);
    }
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void CheckInline(Fixture run,unsigned site,unsigned alignment,unsigned variant)
{
    State input{},output{};auto*words=&input.r15;
    for(unsigned i=0;i<15;++i)words[i]=UINT64_C(0x8123456700000000)+UINT64_C(0x010101011234567)*(i+variant);
    for(unsigned i=0;i<16;++i)for(unsigned j=0;j<16;++j)input.xmm[i][j]=i*17+j+variant;
    input.flags=0x202|(variant&1?0x8d5:0);
    alignas(16) unsigned char frame[512]{};alignas(16) unsigned char system[768]{};
    input.rbp=reinterpret_cast<uintptr_t>(frame)+256;
    FakeOuter map;
    if(site==0)input.rdi=reinterpret_cast<uintptr_t>(system);
    else {
        map.Bind();Add(map,20835,20840);Add(map,20835,20839);
        if(site==4)input.rax=reinterpret_cast<uintptr_t>(map.Set(20835));
        else {
            const uintptr_t first=FirstTargetNode(reinterpret_cast<uintptr_t>(map.Set(20835)));
            input.rbx=variant&1?NextTargetNode(first):first;
        }
    }
    State expected=input;
    if(site==0)expected.rdi=input.r12;
    else if(site==4){expected.rbx=FirstTargetNode(input.rax);expected.rax=input.rbp-0xb8;}
    else expected.rbx=NextTargetNode(input.rbx);
    const unsigned old=_mm_getcsr(),mxcsr=(old&~0x6000u)|((variant%4)<<13)|1u;_mm_setcsr(mxcsr);
    run(&input,&output);assert(_mm_getcsr()==mxcsr);_mm_setcsr(old);
    assert(!std::memcmp(&expected,&output,15*sizeof(uint64_t)));
    assert(!std::memcmp(expected.xmm,output.xmm,sizeof(expected.xmm)));
    assert(output.rspBefore==output.rspAfter&&output.rspAfter%16==alignment);
    if(site==5){assert(!(output.flags&0x801));assert(bool(output.flags&0x40)==!expected.rbx);}
    else assert((output.flags&0x8d5)==(input.flags&0x8d5));
    if(site==0){assert(TargetRead(input.rbp-0x60)==input.rdi);ForgetTargetOwner(input.rdi+0x1d8);}
    else TargetClearOwner(&map);
}
int main()
{
    CheckWindowsOracle();CheckHistory();
    assert(!Tpf2mpInstallTargetOrder(1,"bad"));assert(!Tpf2mpInstallTargetOrder(0,kTargetBuildId));
    const size_t imageSize=0x2e70000;void*image=mmap(nullptr,imageSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    const auto base=reinterpret_cast<uintptr_t>(image);
    for(const auto&c:kTargetContexts)std::memcpy(reinterpret_cast<void*>(base+c.rva),c.bytes,c.size);
    assert(!mprotect(image,imageSize,PROT_READ|PROT_EXEC));
    for(const auto&c:kTargetContexts){const uint8_t bad=c.bytes[c.size/2]^1;Write(base+c.rva+c.size/2,&bad,1);assert(!Tpf2mpInstallTargetOrder(base,kTargetBuildId));assert(!g_targetActiveMask);Write(base+c.rva+c.size/2,c.bytes+c.size/2,1);}
    for(unsigned failure=1;failure<=6;++failure)CheckInstallFailure(base,failure);
    assert(Tpf2mpInstallTargetOrder(base,kTargetBuildId)&&g_targetActiveMask==63);
    // Exercise real installed writer/clear entry detours around the fake
    // original containers, including actual native node lifetimes.
    void* originals[3]={g_targetOriginal[1],g_targetOriginal[2],g_targetOriginal[3]};
    g_targetOriginal[1]=reinterpret_cast<void*>(FakeClear);g_targetOriginal[2]=reinterpret_cast<void*>(FakeInsert);g_targetOriginal[3]=reinterpret_cast<void*>(FakeErase);g_targetLookup=FakeLookup;
    {FakeOuter map;map.Bind();uint32_t id=20840;int32_t target=20835;reinterpret_cast<TargetWrite>(base+0x170d520)(&id,&target,&map);id=20839;reinterpret_cast<TargetWrite>(base+0x170d520)(&id,&target,&map);assert(Walk(map,20835)==std::vector<uint32_t>({20840,20839}));reinterpret_cast<TargetWrite>(base+0x170cf70)(&id,&target,&map);assert(Walk(map,20835)==std::vector<uint32_t>{20840});reinterpret_cast<TargetClear>(base+0x16ef930)(&map);assert(!g_targetOwners);}
    auto*page=static_cast<unsigned char*>(mmap(nullptr,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(page!=MAP_FAILED);
    unsigned cases=0;
    for(unsigned site:{0u,4u,5u})for(unsigned alignment:{0u,8u}){
        auto run=MakeFixture(base,kTargetSites[site],alignment,page);
        for(unsigned variant=0;variant<8;++variant){CheckInline(run,site,alignment,variant);++cases;}}
    for(unsigned i=0;i<3;++i)g_targetOriginal[i+1]=originals[i];
    assert(!g_targetOwners);assert(!Tpf2mpInstallTargetOrder(base,kTargetBuildId));
    munmap(page,4096);munmap(image,imageSize);
    std::printf("target order: %u full-register cases;2459 Windows oracle operations;history/erase/empty/reuse/threads/runtime-fallback and6 partial rollback points passed\n",cases);
}
