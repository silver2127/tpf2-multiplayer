// Execute original affected-network traversal patches in a private image.
#include "../src/network_person_order_linux.cpp"
#include <array>
#include <vector>
#include <algorithm>
#include <random>
#include <cassert>
#include <cstdio>
#include <thread>
#include <sys/mman.h>
#include <immintrin.h>
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
    out.JumpTo(base + kNetworkSites[site].rva);

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
    const uintptr_t resume = base + kNetworkSites[site].rva + kNetworkSites[site].length;
    if (site == 2) {
        // Deleter trampoline replays PUSH RBP/MOV RBP,RSP/PUSH R12/PUSH RBX;
        // undo those original prologue instructions before the generic capture.
        unsigned char epilogue[18] = {0x5b,0x41,0x5c,0x5d};
        const auto jump = Jump(continuation); std::memcpy(epilogue + 4, jump.data(), 14);
        Write(resume, epilogue, sizeof(epilogue));
    } else {
        const auto jump = Jump(continuation); Write(resume, jump.data(), jump.size());
    }
    return reinterpret_cast<Fixture>(page);
}

struct FakeNetwork {
    bool restorePaths;
    std::array<unsigned char, 0x800> frame{};
    std::vector<std::array<unsigned char,24>> nodes;
    std::vector<std::vector<uint32_t>> batches;
    std::vector<std::array<uintptr_t,3>> headers;
    uintptr_t rbp() { return reinterpret_cast<uintptr_t>(frame.data()+0x600); }
    uintptr_t set() { return rbp()-(restorePaths?0x2b0:0x270); }
    uintptr_t source() { return rbp()-(restorePaths?0x3c0:0x380); }
    explicit FakeNetwork(std::vector<std::vector<uint32_t>> input, bool restore=false):restorePaths(restore),batches(std::move(input)) {
        std::vector<uint32_t> ids;
        for(auto& b:batches) { ids.insert(ids.end(),b.begin(),b.end());const auto p=reinterpret_cast<uintptr_t>(b.data());headers.push_back({p,p+b.size()*4,p+b.size()*4}); }
        std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());nodes.resize(ids.size());
        uintptr_t head=0;
        for(size_t i=0;i<ids.size();++i){auto p=reinterpret_cast<uintptr_t>(nodes[i].data());std::memcpy(reinterpret_cast<void*>(p),&head,8);std::memcpy(reinterpret_cast<void*>(p+8),&ids[i],4);head=p;}
        std::memcpy(reinterpret_cast<void*>(set()+0x10),&head,8);const size_t count=ids.size();std::memcpy(reinterpret_cast<void*>(set()+0x18),&count,8);
        const auto p=reinterpret_cast<uintptr_t>(headers.data());const uintptr_t h[]={p,p+headers.size()*24,p+headers.size()*24};std::memcpy(reinterpret_cast<void*>(source()),h,sizeof(h));
    }
    ~FakeNetwork(){ForgetNetworkSet(set());}
    std::vector<uint32_t> Walk(){std::vector<uint32_t> ids;for(auto n=FirstNetworkNode(set(),source());n;n=NextNetworkNode(set(),n)){assert(ids.size()<nodes.size()+1);ids.push_back(NetworkKey(n+8));}return ids;}
};
#include "data/person_map_order_fixture.h"
std::atomic<unsigned> loggedErrors{0};
void CountLog(const char*,...){++loggedErrors;}
void CollectionTests(){
    Tpf2mpNetworkPersonOrderSetLog(CountLog);
    for(const auto& golden:kMapGolden){
        std::vector<uint32_t> ids(golden.input,golden.input+golden.inputCount);
        const size_t split=ids.size()/2;
        FakeNetwork f({{ids.begin(),ids.begin()+split},{},{ids.begin()+split,ids.end()}});
        const auto before=f.nodes;const auto frame=f.frame;const auto result=f.Walk();
        assert(result.size()==golden.outputCount);assert(std::equal(result.begin(),result.end(),golden.expected));
        assert(f.nodes==before&&f.frame==frame);
    }
    for(unsigned i=0;i<50;++i){
        FakeNetwork f({{20839,20840},{20841,20839}});assert((f.Walk()==std::vector<uint32_t>{20839,20840,20841}));
        FakeNetwork nested({{7,8,9}});assert(nested.Walk().size()==3);
        std::thread worker([&]{assert(f.Walk().size()==3);ForgetNetworkSet(f.set());});worker.join();
    }
    assert(!loggedErrors);
    {FakeNetwork f({{1,2}});f.batches[0][0]=3;const auto native=NetworkPointer(f.set()+0x10);assert(FirstNetworkNode(f.set(),f.source())==native);assert(!g_networkRecords);}
    {FakeNetwork f({{1,2}});uintptr_t bad=NetworkPointer(f.source())+1;std::memcpy(reinterpret_cast<void*>(f.source()+8),&bad,8);assert(FirstNetworkNode(f.set(),f.source())==NetworkPointer(f.set()+0x10));}
    {FakeNetwork f({{1,2}});f.headers[0][1]-=4;assert(FirstNetworkNode(f.set(),f.source())==NetworkPointer(f.set()+0x10));assert(!g_networkRecords);}
    {FakeNetwork f({{1,2}});const auto node=NetworkPointer(f.set()+0x10);std::memcpy(reinterpret_cast<void*>(node),&node,8);assert(FirstNetworkNode(f.set(),f.source())==node);assert(!g_networkRecords);}
    assert(loggedErrors==1); // bounded diagnostic, complete native fallback
    assert(!g_networkRecords);
}
void RestoreImage(uintptr_t base){for(const auto& c:kNetworkContexts)Write(base+c.rva,c.bytes,c.size);g_networkActiveMask=0;g_networkReady=false;std::memset(g_networkOriginal,0,sizeof(g_networkOriginal));}
unsigned hookNumber,failHook;
bool FailingHook(uintptr_t target,void*,int length,void**){++hookNumber;unsigned char bytes[32];std::memset(bytes,0xcc,length);Write(target,bytes,length);return hookNumber!=failHook;}
void InstallerTests(uintptr_t base){
    assert(!Tpf2mpInstallNetworkPersonOrder(base,"wrong"));
    for(const auto& c:kNetworkContexts){RestoreImage(base);const unsigned char byte=c.bytes[c.size-1]^1;Write(base+c.rva+c.size-1,&byte,1);assert(!Tpf2mpInstallNetworkPersonOrder(base,kNetworkBuildId));}
    for(unsigned n=1;n<=kNetworkHookCount;++n){RestoreImage(base);hookNumber=0;failHook=n;assert(!InstallNetworkOrder(base,kNetworkBuildId,FailingHook,Tpf2mpCodeWriteSelf));assert(!g_networkActiveMask);for(const auto& s:kNetworkSites)assert(!std::memcmp(reinterpret_cast<void*>(base+s.rva),s.bytes,s.length));}
    RestoreImage(base);assert(Tpf2mpInstallNetworkPersonOrder(base,kNetworkBuildId));assert(!Tpf2mpInstallNetworkPersonOrder(base,kNetworkBuildId));
}
uint64_t TestFlags(uint64_t value,uint64_t initial){uint64_t result;asm volatile("pushq %2;popfq;testq %1,%1;pushfq;popq %0":"=r"(result):"r"(value),"r"(initial):"cc");return result;}
void AbiTests(uintptr_t base,unsigned char* code){
    std::mt19937_64 rng(0x76523);size_t checked=0;
    for(size_t site=0;site<kNetworkHookCount;++site)for(unsigned alignment=0;alignment<2;++alignment)for(unsigned trial=0;trial<12;++trial){
        RestoreImage(base);assert(Tpf2mpInstallNetworkPersonOrder(base,kNetworkBuildId));
        FakeNetwork data({{100,101}},site>=3);const auto first=FirstNetworkNode(data.set(),data.source());const auto second=NextNetworkNode(data.set(),first);assert(first&&second);
        const auto before=data.nodes;auto frame=data.frame;
        State in{},out{};auto* gp=reinterpret_cast<uint64_t*>(&in);for(size_t i=0;i<15;++i)gp[i]=rng();in.rbp=data.rbp();in.flags=0x202|(trial&1?0x891:0);
        for(auto& x:in.xmm)for(auto& b:x)b=rng();
        State expected=in;
        if(site==0)expected.rbx=first;
        else if(site==1){in.rbx=trial&1?second:first;expected=in;expected.rbx=trial&1?0:second;expected.flags=TestFlags(expected.rbx,in.flags);}
        else if(site==3)expected.rax=first;
        else if(site==4){
            in.rax=trial&1?second:first;expected=in;expected.rax=trial&1?0:second;
            std::memcpy(frame.data()+0x600-0x528,&expected.rax,8);
        }
        else{in.rdi=data.set();expected=in;}
        auto fixture=MakeFixture(base,site,alignment,code);const unsigned old=_mm_getcsr();const unsigned mxcsr=(old&~0x6000u)|((trial%4)<<13);_mm_setcsr(mxcsr);fixture(&in,&out);assert(_mm_getcsr()==mxcsr);_mm_setcsr(old);
        assert(!std::memcmp(&expected,&out,128));assert(!std::memcmp(in.xmm,out.xmm,sizeof(in.xmm)));assert(out.rspBefore==out.rspAfter);assert(data.nodes==before&&data.frame==frame);
        if(site==2)assert(!g_networkRecords);
        ++checked;
    }
    std::printf("network person: %zu actual-shim GP/XMM/flags/MXCSR/RSP cases passed\n",checked);
}

unsigned partialHookCount;
uintptr_t retainedFirst;
bool PartialHook(uintptr_t address,void* detour,int length,void** original){
    if(++partialHookCount==1)return InstallHook(address,detour,length,original);
    return false;
}
int PartialRollback(uintptr_t address,const uint8_t* bytes,size_t length,int* error){
    if(address==retainedFirst)return TPF2MP_CW_UNAVAILABLE;
    return Tpf2mpCodeWriteSelf(address,bytes,length,error);
}
void FailedRollbackTest(uintptr_t base,unsigned char* code){
    RestoreImage(base);partialHookCount=0;retainedFirst=base+kNetworkSites[0].rva;
    assert(!InstallNetworkOrder(base,kNetworkBuildId,PartialHook,PartialRollback));
    assert(g_networkActiveMask==1&&!g_networkReady.load());
    FakeNetwork data({{100,101}});State in{},out{};in.rbp=data.rbp();in.flags=0x202;
    const auto expected=NetworkPointer(data.set()+0x10);
    auto fixture=MakeFixture(base,0,0,code);fixture(&in,&out);
    assert(out.rbx==expected&&out.rbp==in.rbp&&out.flags==in.flags);
    assert(out.rspBefore==out.rspAfter&&!g_networkRecords);
    RestoreImage(base);
}
int main(){
    CollectionTests();const size_t size=0x2e80000;void* image=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    const auto base=reinterpret_cast<uintptr_t>(image);RestoreImage(base);assert(!mprotect(image,size,PROT_READ|PROT_EXEC));InstallerTests(base);
    void* code=mmap(nullptr,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);AbiTests(base,static_cast<unsigned char*>(code));FailedRollbackTest(base,static_cast<unsigned char*>(code));assert(!g_networkRecords);
    munmap(code,4096);munmap(image,size);std::puts("network person: batch/member integrity, unchanged native data, nested/thread cleanup and five-stage rollback passed");
}
