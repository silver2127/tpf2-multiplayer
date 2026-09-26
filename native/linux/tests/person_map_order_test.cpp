// Execute the real BuildingTypeRep registration patch in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/person_map_order_linux.cpp"
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
    out.JumpTo(base + kMapSites[site].rva);

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
    const uintptr_t resume = site == 11 ? base + 0x2e6cf92 : base + kMapSites[site].rva + kMapSites[site].length;
    if (site == 1) {
        // Deleter trampoline replays PUSH RBP/MOV RBP,RSP/PUSH R13;
        // undo those original prologue instructions before the generic capture.
        unsigned char epilogue[17] = {0x41,0x5d,0x5d};
        const auto jump = Jump(continuation); std::memcpy(epilogue + 3, jump.data(), 14);
        Write(resume, epilogue, sizeof(epilogue));
    } else {
        const auto jump = Jump(continuation); Write(resume, jump.data(), jump.size());
    }
    return reinterpret_cast<Fixture>(page);
}

struct FakeData {
    std::array<unsigned char, 0x460> data{};
    std::array<unsigned char, 0x130> owner{};
    std::vector<std::array<unsigned char, 56>> nodes;
    uintptr_t address() { return reinterpret_cast<uintptr_t>(data.data()); }
    uintptr_t ownerAddress() { return reinterpret_cast<uintptr_t>(owner.data()); }
    explicit FakeData(size_t capacity=128) { nodes.reserve(capacity); }
    void Begin() { BindMaps(address(), ownerAddress()); }
    uintptr_t Add(size_t group, uint32_t id, bool capture=true) {
        const auto map=address()+group*56;
        nodes.push_back({}); auto node=reinterpret_cast<uintptr_t>(nodes.back().data());
        const auto first=ReadPointer(map+16);std::memcpy(reinterpret_cast<void*>(node),&first,8);
        std::memcpy(reinterpret_cast<void*>(node+8),&id,4);std::memcpy(reinterpret_cast<void*>(map+16),&node,8);
        auto count=ReadPointer(map+24)+1;std::memcpy(reinterpret_cast<void*>(map+24),&count,8);
        if(capture) CaptureMapInsertion(map,id,node);
        return node;
    }
    ~FakeData() { ForgetMaps(address()); }
};
std::vector<uint32_t> Walk(FakeData& data,size_t group) {
    std::vector<uint32_t> ids;
    for(auto n=FirstMapNode(data.address(),group);n;n=NextMapNode(n,group)) {assert(ids.size()<data.nodes.size()+1);ids.push_back(ReadKey(n));}
    return ids;
}
std::atomic<unsigned> g_loggedErrors{0};
void CountLog(const char*, ...) { ++g_loggedErrors; }
void LifetimeTests() {
    Tpf2mpPersonMapOrderSetLog(CountLog);
    g_mapReportedErrors = 0; g_loggedErrors = 0;
    for(size_t group=0;group<5;++group){
        FakeData d;d.Begin();const auto a=d.Add(group,20851),b=d.Add(group,20852);
        CaptureMapInsertion(d.address()+group*56,20851,a); // duplicate lookup
        const auto before=d.nodes;
        assert((Walk(d,group)==std::vector<uint32_t>{20851,20852}));assert(d.nodes==before);
        assert(ReadPointer(b)==a);assert(!ReadPointer(a));
        FakeData nested;nested.Begin();nested.Add(group,44);nested.Add(group,45);
        assert((Walk(nested,group)==std::vector<uint32_t>{44,45}));assert(Walk(d,group).front()==20851);
    }
    FakeData reuse;for(int i=0;i<50;++i){
        reuse.data={};reuse.nodes.clear();reuse.Begin();reuse.Add(4,100+i);reuse.Add(4,200+i);
        assert(Walk(reuse,4).front()==uint32_t(100+i));ForgetMaps(reuse.address());
    }
    assert(g_loggedErrors == 0);
    FakeData missing;missing.Begin();missing.Add(0,1);missing.Add(0,2,false);
    assert((Walk(missing,0)==std::vector<uint32_t>{2,1})); // incomplete capture is all-native
    assert(g_loggedErrors == 1);
    { FakeData alsoMissing; alsoMissing.Begin(); alsoMissing.Add(0,3,false); (void)Walk(alsoMissing,0); }
    assert(g_loggedErrors == 1); // repeated failure type is logged only once
    FakeData transferred;transferred.Begin();transferred.Add(3,80);transferred.Add(3,81);
    std::thread t([&]{assert((Walk(transferred,3)==std::vector<uint32_t>{80,81}));ForgetMaps(transferred.address());});t.join();
    auto threaded=[](int base){for(int i=0;i<100;++i){FakeData d;d.Begin();d.Add(1,base+i);d.Add(1,base+i+100);assert(Walk(d,1).size()==2);}};
    std::thread a(threaded,1000),b(threaded,3000),c(threaded,5000);a.join();b.join();c.join();
    uintptr_t unbound[7]{};uint32_t id=2;CaptureMapInsertion(reinterpret_cast<uintptr_t>(unbound),id,1); // ignored without dereferencing unrelated node
}
#include "data/person_map_order_fixture.h"
void ModelTests() {
    for (const auto& golden : kMapGolden) {
        std::vector<uint32_t> output(golden.inputCount); size_t count = 0;
        assert(Tpf2mpWindowsPersonMapOrder(golden.input, golden.inputCount, output.data(), &count));
        assert(count == golden.outputCount);
        assert(!std::memcmp(output.data(), golden.expected, count * 4));
    }
    std::mt19937 random(12345);
    for(size_t count: {0u,1u,2u,8u,9u,64u,65u,512u,513u,1024u,1025u,10000u}) {
        std::vector<uint32_t> ids(count),output(count);for(auto& id:ids)id=random();
        size_t result=0;assert(Tpf2mpWindowsPersonMapOrder(ids.data(),count,output.data(),&result));
        output.resize(result);std::sort(output.begin(),output.end());std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());assert(output==ids);
    }
    const uint32_t ids[]={0,1,2,3,4,5,6,7,8,0,1};uint32_t out[11];size_t count;
    assert(Tpf2mpWindowsPersonMapOrder(ids,11,out,&count));assert(count==9);
    const uint32_t expected[]={8,0,1,2,3,4,5,6,7};assert(!std::memcmp(out,expected,sizeof(expected)));
}
void RestoreImage(uintptr_t base) {
    for(const auto& c:kMapContexts)Write(base+c.rva,c.bytes,c.size);
    g_mapActiveMask=0;std::memset(g_mapOriginal,0,sizeof(g_mapOriginal));
}
unsigned g_hookNumber=0,g_failHook=0;
bool FailingHook(uintptr_t target,void*,int length,void**) {
    ++g_hookNumber;unsigned char bytes[32];std::memset(bytes,0xcc,length);Write(target,bytes,length);return g_hookNumber!=g_failHook;
}
void InstallerTests(uintptr_t base) {
    assert(!Tpf2mpInstallPersonMapOrder(base,"wrong"));
    for(const auto& c:kMapContexts){RestoreImage(base);unsigned char bad=c.bytes[c.size-1]^1;Write(base+c.rva+c.size-1,&bad,1);assert(!Tpf2mpInstallPersonMapOrder(base,kMapBuildId));}
    for(unsigned failure=1;failure<=kMapHookCount;++failure){
        RestoreImage(base);g_hookNumber=0;g_failHook=failure;
        assert(!InstallPersonMaps(base,kMapBuildId,FailingHook,Tpf2mpCodeWriteSelf));assert(!g_mapActiveMask);
        for(const auto& s:kMapSites)assert(!std::memcmp(reinterpret_cast<void*>(base+s.rva),s.bytes,s.length));
    }
    RestoreImage(base);assert(Tpf2mpInstallPersonMapOrder(base,kMapBuildId));assert(!Tpf2mpInstallPersonMapOrder(base,kMapBuildId));
}
uint64_t TestFlags(uint64_t value,uint64_t initial) {
    uint64_t result;asm volatile("pushq %2;popfq;testq %1,%1;pushfq;popq %0":"=r"(result):"r"(value),"r"(initial):"cc");return result;
}
void AbiTests(uintptr_t base,unsigned char* code) {
    std::mt19937_64 rng(0x2818);
    size_t checked=0;
    for(size_t site=0;site<kMapHookCount;++site)for(unsigned alignment=0;alignment<2;++alignment)for(unsigned trial=0;trial<8;++trial){
        if(site==12)continue; // ordinary insertion wrapper has no inline ABI fixture
        RestoreImage(base);assert(Tpf2mpInstallPersonMapOrder(base,kMapBuildId));
        FakeData d;d.Begin();uintptr_t first[5],second[5];
        for(size_t group=0;group<5;++group){first[group]=d.Add(group,100+group*4);second[group]=d.Add(group,101+group*4);}
        (void)FirstMapNode(d.address(),0);
        const auto before=d.nodes;
        State in{},out{};auto* gp=reinterpret_cast<uint64_t*>(&in);
        for(size_t i=0;i<15;++i) gp[i]=rng();
        in.flags=0x202|(trial&1?0x891:0);
        for(auto& x:in.xmm)for(auto& byte:x)byte=static_cast<unsigned char>(rng());
        std::array<unsigned char,0x800> stack{};in.rbp=reinterpret_cast<uintptr_t>(stack.data()+0x600);
        const auto ptr=d.address();std::memcpy(reinterpret_cast<void*>(in.rbp-0x470),&ptr,8);
        std::memcpy(reinterpret_cast<void*>(in.rbp-0x1d0),&ptr,8);
        State expected=in;
        if(site==0){ForgetMaps(d.address());d.data={};d.nodes.clear();in.rdx=d.address();in.r15=d.ownerAddress();expected=in;}
        else if(site==1){in.rdi=d.address();expected=in;}
        else if(site>=2&&site<=6){
            if(site==2){in.r14=d.address();expected=in;expected.r13=first[0];}
            else {expected.rax=d.address();if(site==3)expected.rbx=first[1];if(site==4)expected.r12=first[2];if(site==5)expected.r14=first[3];if(site==6){expected.rbx=first[4];expected.r13=in.rbp-0x39c;}}
        } else if(site>=13&&site<=17) {
            const auto group=site-13;
            if(group==0){in.r14=d.address();expected=in;}
            else expected.rax=d.address();
            if(group==3)expected.r14=first[group];else expected.rbx=first[group];
        } else if(site>=18) {
            const auto group=(site-18)/2;const auto node=trial&1?second[group]:first[group];const auto next=trial&1?0:second[group];
            if(group==3)in.r14=node;else in.rbx=node;expected=in;
            if(group==3)expected.r14=next;else expected.rbx=next;
            expected.flags=TestFlags(next,in.flags);
        } else {
            const auto group=site-7;const auto node=trial&1?second[group]:first[group];const auto next=trial&1?0:second[group];
            uint64_t* i=site==7?&in.r13:site==9?&in.r12:site==10?&in.r14:&in.rbx;*i=node;expected=in;
            uint64_t* e=site==7?&expected.r13:site==9?&expected.r12:site==10?&expected.r14:&expected.rbx;*e=next;
            if(site!=11)expected.flags=TestFlags(next,in.flags);
        }
        auto fixture=MakeFixture(base,site,alignment,code);const unsigned oldMxcsr=_mm_getcsr();const unsigned mxcsr=(oldMxcsr&~0x6000u)|((trial%4)<<13);_mm_setcsr(mxcsr);
        fixture(&in,&out);assert(_mm_getcsr()==mxcsr);_mm_setcsr(oldMxcsr);
        assert(!std::memcmp(&expected,&out,128));assert(!std::memcmp(in.xmm,out.xmm,sizeof(in.xmm)));assert(out.rspBefore==out.rspAfter);
        if(site>=2)assert(d.nodes==before);
        if(site==0)assert(ReadPointer(d.ownerAddress()+0x120)==d.address());
        if(site==2){uint32_t stored;std::memcpy(&stored,reinterpret_cast<void*>(in.rbp-0x494),4);assert(stored==uint32_t(in.rax));}
        if(site==13){uint32_t stored;std::memcpy(&stored,reinterpret_cast<void*>(in.rbp-0x198),4);assert(stored==uint32_t(in.rax));}
        ++checked;
    }
    std::printf("person map: %zu real-shim GP/XMM/flags/MXCSR/RSP checks passed\n",checked);
}
int main(){
    // Each child evaluates the cached startup environment independently.
    for (const char* value : {static_cast<const char*>(nullptr), "", "1", "0", "00", "false"}) {
        const pid_t child=fork(); assert(child>=0);
        if (!child) {
            if(value) setenv("TPF2MP_ORDER_CANON",value,1); else unsetenv("TPF2MP_ORDER_CANON");
            tpf2mp_order_detail::active=false; assert(!CanonicalMaps());
            tpf2mp_order_detail::active=true;
            assert(CanonicalMaps()==(!value || std::strcmp(value,"0")));
            if (CanonicalMaps()) {
                FakeData d; d.Begin();
                for(auto id:{9u,2u,8u,1u,5u}) d.Add(0,id);
                assert((Walk(d,0)==std::vector<uint32_t>{1,2,5,8,9}));
            }
            tpf2mp_order_detail::active=false; assert(!CanonicalMaps());
            _exit(0);
        }
        int status=0; assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    setenv("TPF2MP_ORDER_CANON","1",1);
    tpf2mp_order_detail::active=true;
    ModelTests();LifetimeTests();assert(!g_mapOwners);
    for(size_t group=0;group<5;++group) {
        FakeData d;d.Begin();
        for(auto id:{9u,2u,8u,1u,5u})d.Add(group,id);
        const auto original=d.nodes;
        assert((Walk(d,group)==std::vector<uint32_t>{1,2,5,8,9}));
        assert(d.nodes==original); // sorted sidecar does not mutate bucket/list structure
    }
    const size_t size=0x2e80000;void* image=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    const auto base=reinterpret_cast<uintptr_t>(image);RestoreImage(base);assert(!mprotect(image,size,PROT_READ|PROT_EXEC));
    InstallerTests(base);void* code=mmap(nullptr,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
    AbiTests(base,static_cast<unsigned char*>(code));assert(!g_mapOwners);munmap(code,4096);munmap(image,size);
    std::puts("person map: model/lifetime/thread/28-stage rollback checks passed");
}
