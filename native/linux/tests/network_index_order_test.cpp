// Execute persistent person-route index patches in a private image.
#include "../src/network_index_order_linux.cpp"
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
    out.JumpTo(base + kIndexSites[site].rva);

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
    const uintptr_t resume = base + kIndexSites[site].rva + kIndexSites[site].length;
    if (site == 1) {
        // Deleter trampoline replays PUSH RBP/MOV RBP,RSP/PUSH R12/PUSH RBX;
        // undo those original prologue instructions before the generic capture.
        unsigned char epilogue[20] = {0x5b,0x41,0x5c,0x41,0x5d,0x5d};
        const auto jump = Jump(continuation); std::memcpy(epilogue + 6, jump.data(), 14);
        Write(resume, epilogue, sizeof(epilogue));
    } else {
        const auto jump = Jump(continuation); Write(resume, jump.data(), jump.size());
    }
    if (site >= 2 && site <= 5) {
        const auto jump = Jump(continuation);
        Write(Tpf2mpNetworkIndexLoop[site-2], jump.data(), jump.size());
    }
    return reinterpret_cast<Fixture>(page);
}


struct Data {
    std::array<uintptr_t, 100> system{}, frame{};
    std::array<uintptr_t, 12> outer{};
    std::array<uintptr_t, 2> node1{0,100}, node2{0,101};
    uint32_t id=100;
    uintptr_t map(){return reinterpret_cast<uintptr_t>(system.data())+0x90;}
    uintptr_t set(){return reinterpret_cast<uintptr_t>(outer.data())+0x10;}
    uintptr_t rbp(){return reinterpret_cast<uintptr_t>(frame.data())+0x100;}
    void put(uintptr_t address,uintptr_t value){std::memcpy(reinterpret_cast<void*>(address),&value,8);}
    Data(){
        BindNetworkIndexOwner(map());
        put(map()+0x10,reinterpret_cast<uintptr_t>(outer.data()));put(map()+0x18,1);outer[1]=123;
        put(set()+0x10,reinterpret_cast<uintptr_t>(node1.data()));put(set()+0x18,1);
        ObserveNetworkIndexInsert(map(),123,100,set());
    }
    ~Data(){ForgetNetworkIndexOwner(map());}
};
void RestoreImage(uintptr_t base){for(const auto& c:kIndexContexts)Write(base+c.rva,c.bytes,c.size);g_indexActiveMask=0;g_indexReady=false;}
unsigned calls,failAt;
bool FailHook(uintptr_t target,void*,int n,void**){unsigned char b[16];std::memset(b,0xcc,n);Write(target,b,n);return ++calls!=failAt;}
void InstallerTests(uintptr_t base){
    assert(!Tpf2mpInstallNetworkIndexOrder(base,"wrong"));
    for(const auto& c:kIndexContexts){RestoreImage(base);unsigned char b=c.bytes[c.size-1]^1;Write(base+c.rva+c.size-1,&b,1);assert(!Tpf2mpInstallNetworkIndexOrder(base,kIndexBuildId));}
    for(failAt=1;failAt<=10;++failAt){RestoreImage(base);calls=0;assert(!InstallIndexOrder(base,kIndexBuildId,FailHook,Tpf2mpCodeWriteSelf));assert(!g_indexActiveMask);for(const auto& s:kIndexSites)assert(!std::memcmp(reinterpret_cast<void*>(base+s.rva),s.bytes,s.length));}
}
void CollectionTests(){
    Data d;
    assert(FirstNetworkIndexNode(d.set())==reinterpret_cast<uintptr_t>(d.node1.data()));
    d.node2[0]=reinterpret_cast<uintptr_t>(d.node1.data());
    d.put(d.set()+0x10,reinterpret_cast<uintptr_t>(d.node2.data()));d.put(d.set()+0x18,2);
    ObserveNetworkIndexInsert(d.map(),123,101,d.set());
    assert(FirstNetworkIndexNode(d.set())==reinterpret_cast<uintptr_t>(d.node1.data()));
    assert(NextNetworkIndexNode(reinterpret_cast<uintptr_t>(d.node1.data()))==reinterpret_cast<uintptr_t>(d.node2.data()));
    ObserveNetworkIndexInsert(d.map(),123,101,d.set()); // duplicate preserves order
    d.node2[0]=0;d.put(d.set()+0x18,1);ObserveNetworkIndexErase(d.map(),123,100);
    assert(FirstNetworkIndexNode(d.set())==reinterpret_cast<uintptr_t>(d.node2.data()));
    ObserveNetworkIndexErase(d.map(),123,100); // absent erase is legitimate
    assert(!g_networkIndexOwners->records->invalid);
    d.put(d.set()+0x18,2); // missed writer must retain native traversal
    assert(FirstNetworkIndexNode(d.set())==reinterpret_cast<uintptr_t>(d.node2.data()));
    assert(g_networkIndexOwners->records->invalid);
}
void AbiTests(uintptr_t base,unsigned char* code){
    for(unsigned site=0;site<10;++site)for(unsigned alignment=0;alignment<2;++alignment)for(unsigned taken=0;taken<2;++taken){
        RestoreImage(base);assert(Tpf2mpInstallNetworkIndexOrder(base,kIndexBuildId));
        Data d;State in{},out{};in.flags=0x202;in.rbp=d.rbp();in.r14=taken;
        for(unsigned i=0;i<16;++i)for(unsigned j=0;j<16;++j)in.xmm[i][j]=i*16+j;
        in.rdi=reinterpret_cast<uintptr_t>(d.system.data());
        if(site==1)in.rdi=d.map();
        if(site>=2&&site<=5){
            in.r12=reinterpret_cast<uintptr_t>(&d.id);in.r15=d.map();
            d.put(d.rbp()-0x40,123);d.put(d.rbp()-0x58,d.map());d.put(d.rbp()-0x48,d.set());
            if(site>=4){d.put(d.set()+0x10,0);d.put(d.set()+0x18,0);}
        }
        if(site==6)in.r12=reinterpret_cast<uintptr_t>(d.outer.data());
        if(site>=7)in.r12=reinterpret_cast<uintptr_t>(d.node1.data());
        auto fixture=MakeFixture(base,site,alignment,code);fixture(&in,&out);
        assert(out.rspBefore==out.rspAfter);assert(!std::memcmp(in.xmm,out.xmm,sizeof(in.xmm)));
        const auto* input=reinterpret_cast<const uint64_t*>(&in);
        const auto* output=reinterpret_cast<const uint64_t*>(&out);
        for(unsigned reg=0;reg<15;++reg)if(reg!=3)assert(input[reg]==output[reg]);
        if(site<2||site==6)assert(out.flags==in.flags);
        if(site>=2&&site<=5)assert(bool(out.flags&0x40)==!taken);
        if(site>=7)assert(out.flags&0x40);
        if(site==6)assert(out.r12==reinterpret_cast<uintptr_t>(d.node1.data()));
        else if(site>=7)assert(!out.r12);
        else assert(out.r12==in.r12);
        if(site>=2&&site<=5)assert(!g_networkIndexOwners->records->invalid);
    }
}
int main(){
    CollectionTests();const size_t size=0x1800000;
    void* image=mmap(nullptr,size,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    auto base=reinterpret_cast<uintptr_t>(image);RestoreImage(base);InstallerTests(base);
    void* code=mmap(nullptr,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
    AbiTests(base,static_cast<unsigned char*>(code));assert(!g_networkIndexOwners);
    munmap(code,4096);munmap(image,size);std::puts("network index: lifecycle, membership, rollback and all ten executable hooks passed");
}
