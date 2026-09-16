// Execute the resident hash patches in a private image. No live game state
// or installed file is accessed or modified by this fixture.
#include "../src/resident_hash_linux.cpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <vector>
#include <immintrin.h>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <string>
#include "data/resident_windows_oracle.h"
#include "data/resident_native_helpers.h"

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

static Fixture MakeFixture(uintptr_t base, const ResidentSite& site, unsigned alignment,
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

static unsigned installerCalls, failAt;
static bool FailingInstaller(uintptr_t address, void* detour, int size, void** original)
{
    if (++installerCalls == failAt) {
        // Include a failed attempt that changed its entry: rollback must
        // restore this window as well as all earlier successful hooks.
        const uint8_t partial = 0xcc;
        Write(address, &partial, 1);
        return false;
    }
    return InstallHook(address, detour, size, original);
}
static void CheckRollback(uintptr_t base, unsigned failure)
{
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        installerCalls = 0; failAt = failure;
        assert(!InstallResidentHash(base, kResidentBuildId, FailingInstaller, Tpf2mpCodeWriteSelf));
        assert(installerCalls == failure && !g_residentActiveMask && !g_residentReady);
        assert(std::strstr(Tpf2mpResidentHashStatus(), "original resident code restored"));
        for (const auto& context : kResidentContexts)
            assert(!std::memcmp(reinterpret_cast<void*>(base + context.rva), context.code, context.size));
        _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static uint64_t NativeHash(uint32_t id)
{
    const uint64_t signExtended = uint64_t(int64_t(int32_t(id)));
    const unsigned __int128 product = static_cast<unsigned __int128>(signExtended) * UINT64_C(0xde5fb9d2630458e9);
    return uint64_t(product) + uint64_t(product >> 64);
}

static void CheckRegisters(Fixture run, unsigned site, unsigned alignment, uint32_t id,
                           unsigned variant, bool active)
{
    State input{}, output{};
    uint64_t* words = &input.r15;
    for (unsigned i = 0; i < 15; ++i) words[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + variant);
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + variant;
    input.flags = 0x202 | (variant & 1 ? 0x8d5 : 0);
    input.r9 = uint64_t(int64_t(int32_t(id)));
    const unsigned __int128 product = static_cast<unsigned __int128>(input.r9) * UINT64_C(0xde5fb9d2630458e9);
    input.rax = uint64_t(product); input.rdx = uint64_t(product >> 64);
    uint64_t fakeSet[6]{};
    uint32_t slots[3] = {0x12345678, id, 0x23456789};
    fakeSet[1] = reinterpret_cast<uintptr_t>(slots);
    fakeSet[4] = 127;
    if (site == 2) input.rcx = reinterpret_cast<uintptr_t>(fakeSet);
    if (site == 3) { input.r12 = reinterpret_cast<uintptr_t>(fakeSet); input.r14 = reinterpret_cast<uintptr_t>(&slots[1]); }
    if (site == 4) { input.r13 = reinterpret_cast<uintptr_t>(fakeSet); input.rbx = 1; }
    ResidentScope scope{g_residentScope, fakeSet};
    if (active) g_residentScope = &scope;
    State expected = input;
    const uint64_t hash = site < 3 || active ? Tpf2mpWindowsResidentHash(id) : NativeHash(id);
    switch (site) {
    case 0: expected.rax = hash; expected.rdx = input.r8 + 1; expected.rcx = hash >> 7; break;
    case 1: expected.rax = hash; expected.rcx = hash >> 7; break;
    case 2: expected.rax = hash; expected.rdx = fakeSet[4]; expected.rdi = hash; expected.r10 = fakeSet[4] + 1; break;
    case 3: expected.r8 = hash; break;
    case 4: expected.r15 = hash; expected.rsi = hash; break;
    }
    const unsigned oldMxcsr = _mm_getcsr(), mxcsr = oldMxcsr | 1u;
    _mm_setcsr(mxcsr);
    run(&input, &output);
    assert(_mm_getcsr() == mxcsr); _mm_setcsr(oldMxcsr);
    if (active) g_residentScope = scope.previous;
    if (std::memcmp(&expected.r15, &output.r15, 15 * sizeof(uint64_t))) {
        for (unsigned i = 0; i < 15; ++i)
            if ((&expected.r15)[i] != (&output.r15)[i])
                std::fprintf(stderr, "site %u reg %u expected %llx got %llx\n", site, i,
                             (unsigned long long)(&expected.r15)[i], (unsigned long long)(&output.r15)[i]);
        std::abort();
    }
    // Active adapters retain entry flags; inactive helpers replay their actual
    // arithmetic, whose flag results are part of the native passthrough.
    if (active || site < 3) assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(!std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)));
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == alignment);
    assert(slots[0] == 0x12345678 && slots[1] == id && slots[2] == 0x23456789);
}

static ResidentPrepare ScopedCall, UnrelatedCall;
static thread_local unsigned depth, observed;
static thread_local void* expectedSet;
static size_t FakePrepare(void* set, uint64_t hash)
{
    ++observed;
    assert(set == expectedSet);
    if (!depth) {
        assert(g_residentScope && g_residentScope->set == set);
        assert(IsResidentSet(reinterpret_cast<uintptr_t>(set)));
        assert(!IsResidentSet(reinterpret_cast<uintptr_t>(set) + 1));
        auto* outer = g_residentScope;
        unsigned nestedStorage;
        void* saved = expectedSet;
        expectedSet = &nestedStorage; ++depth;
        assert(ScopedCall(expectedSet, hash + 1) == size_t(hash + 1));
        assert(g_residentScope == outer);
        // Unrelated direct callers retain their exact original scope. They
        // cannot make a different Lua/private-copy table become resident.
        assert(UnrelatedCall(expectedSet, hash + 2) == size_t(hash + 2));
        assert(g_residentScope == outer);
        --depth; expectedSet = saved;
    } else {
        assert(g_residentScope);
        if (g_residentScope->set == set) assert(g_residentScope->previous);
        else assert(!IsResidentSet(reinterpret_cast<uintptr_t>(set)));
    }
    return size_t(hash);
}
static size_t PlainPrepare(void* set, uint64_t hash)
{
    assert(!g_residentScope && set == expectedSet); ++observed; return size_t(hash);
}
static void GateSequence()
{
    unsigned storage;
    expectedSet = &storage; observed = depth = 0;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ScopedCall(expectedSet, i) == i);
        assert(!g_residentScope);
    }
    assert(observed == 300);
    expectedSet = nullptr;
}
static void CheckGate(uintptr_t base)
{
    // Genuine CALL instructions at the verified return PC and at an unrelated
    // PC, each with a normal SysV stack. The guarded full function was checked
    // before replacing its surrounding caller with this isolated fixture.
    const uintptr_t entries[] = {base + kResidentInsertReturn - 9, base + 0x1730000};
    for (uintptr_t entry : entries) {
        std::vector<unsigned char> code{0x48,0x83,0xec,8,0xe8};
        const int32_t rel = int32_t(base + 0x1721480 - (entry + 9));
        const auto* p = reinterpret_cast<const unsigned char*>(&rel); code.insert(code.end(), p, p + 4);
        code.insert(code.end(), {0x48,0x83,0xc4,8,0xc3});
        Write(entry, code.data(), code.size());
    }
    ScopedCall = reinterpret_cast<ResidentPrepare>(entries[0]);
    UnrelatedCall = reinterpret_cast<ResidentPrepare>(entries[1]);
    void* original = g_residentOriginal[5];
    g_residentOriginal[5] = reinterpret_cast<void*>(PlainPrepare);
    unsigned storage; expectedSet = &storage; observed = 0;
    assert(UnrelatedCall(expectedSet, 99) == 99 && observed == 1);
    g_residentReady = false;
    assert(ScopedCall(expectedSet, 100) == 100 && observed == 2);
    g_residentReady = true;
    g_residentOriginal[5] = reinterpret_cast<void*>(FakePrepare);
    GateSequence();
    std::thread first(GateSequence), second(GateSequence); first.join(); second.join();
    assert(!g_residentScope);
    g_residentOriginal[5] = original;
}

// Run the original native probing, growth, erase and tombstone algorithms.
// Only their CRT allocation leaves are supplied by the host process.
struct RawResidentSet {
    signed char* ctrl;
    uint32_t* slots;
    uint64_t count, capacity, padding, growth;
};
static_assert(sizeof(RawResidentSet) == 48);
struct RawResidentEntry { uint32_t destination, padding; RawResidentSet residents; };
static_assert(sizeof(RawResidentEntry) == 56);
static uintptr_t containerBase;
static size_t DropResidents(void* set, uint64_t)
{
    reinterpret_cast<void (*)(void*)>(containerBase + 0x1721240)(set);
    return 0;
}
static void InitializeContainerLeaves(uintptr_t base)
{
    containerBase = base;
    const std::pair<uintptr_t, uintptr_t> leaves[] = {
        {0x6dbce0, reinterpret_cast<uintptr_t>(std::malloc)},
        {0x6dbcd0, reinterpret_cast<uintptr_t>(std::free)},
        {0x6dbe70, reinterpret_cast<uintptr_t>(std::memset)},
        {0x6db850, reinterpret_cast<uintptr_t>(std::abort)}
    };
    for (const auto& leaf : leaves) { const auto jump = Jump(leaf.second); Write(base + leaf.first, jump.data(), jump.size()); }
}
struct OriginalResidentSet {
    alignas(16) signed char empty[32];
    alignas(16) signed char outerControl[32];
    RawResidentEntry entry{};
    RawResidentSet outer{};
    OriginalResidentSet()
    {
        std::memset(empty, 0x80, sizeof(empty)); empty[0] = -1;
        entry.destination = 20835;
        entry.residents.ctrl = empty;
        std::memset(outerControl, 0x80, sizeof(outerControl));
        outerControl[0] = outerControl[2] = NativeHash(entry.destination) & 127;
        outerControl[1] = -1;
        outer = {outerControl, reinterpret_cast<uint32_t*>(&entry), 1, 1, 0, 0};
    }
    ~OriginalResidentSet() { if (outer.count && entry.residents.capacity) std::free(entry.residents.ctrl); }
    RawResidentSet& Set() { return entry.residents; }
    void Insert(uint32_t id)
    {
        assert(outer.count);
        const size_t slot = ScopedCall(&Set(), Tpf2mpWindowsResidentHash(id));
        assert(slot < Set().capacity);
        Set().slots[slot] = id;
    }
    void Erase(uint32_t id)
    {
        const uint32_t* destinations[] = {&entry.destination, &entry.destination + 1};
        reinterpret_cast<void (*)(const uint32_t*, const uint32_t* const*, RawResidentSet*)>(containerBase + 0x170acb0)
            (&id, destinations, &outer);
    }
    void Drop() { assert(Set().capacity >= 15); assert(ResidentScopedPrepare(&Set(), 0, DropResidents) == 0); }
    std::vector<uint32_t> Order()
    {
        std::vector<uint32_t> result;
        if (!outer.count) return result;
        for (size_t slot = 0; slot < Set().capacity; ++slot)
            if (Set().ctrl[slot] >= 0) result.push_back(Set().slots[slot]);
        assert(result.size() == Set().count && Set().ctrl[Set().capacity] == -1);
        return result;
    }
};
static void CheckOriginalContainers(uintptr_t base)
{
    InitializeContainerLeaves(base);
    unsigned oracleStates = 0;
    for (const auto& fixture : kResidentOracleCases) {
        OriginalResidentSet original;
        for (size_t step = 0; step < fixture.count; ++step) {
            const auto& operation = fixture.operations[step];
            if (operation.kind == 'i') original.Insert(operation.id);
            else if (operation.kind == 'e') original.Erase(operation.id);
            else { assert(operation.kind == 'd'); original.Drop(); }
            auto& set = original.Set();
            const auto& expected = fixture.states[step];
            assert(set.count == expected.count && set.capacity == expected.capacity && set.growth == expected.growth);
            std::string control, occupied;
            constexpr char hex[] = "0123456789abcdef";
            for (size_t slot = 0; slot < set.capacity + 16; ++slot) {
                const uint8_t value = uint8_t(set.ctrl[slot]);
                control += hex[value >> 4]; control += hex[value & 15];
                if (slot < set.capacity && set.ctrl[slot] >= 0) {
                    if (!occupied.empty()) occupied += ',';
                    occupied += std::to_string(slot) + ':' + std::to_string(set.slots[slot]);
                }
            }
            if (control != expected.controlHex || occupied != expected.occupiedCsv) {
                std::fprintf(stderr, "original native/Windows state mismatch: %s step %zu\n", fixture.name, step);
                std::abort();
            }
            ++oracleStates;
        }
    }
    assert(oracleStates == 300);
    {
        OriginalResidentSet set;
        for (uint32_t id : {20839u, 20840u, 20841u}) set.Insert(id);
        // Actual Windows a8cb90/963500 order and the first-terminal live trace.
        assert(set.Order() == std::vector<uint32_t>({20841, 20840, 20839}));
        set.Erase(20840);
        assert(set.Order() == std::vector<uint32_t>({20841, 20839}));
        set.Insert(20840);
        assert(set.Order() == std::vector<uint32_t>({20841, 20840, 20839}));
        set.Erase(20839); set.Erase(20840); set.Erase(20841);
        assert(!set.outer.count && set.Order().empty());
    }
    {
        OriginalResidentSet set;
        constexpr unsigned count = 4000;
        for (unsigned id = 0; id < count; ++id) set.Insert(id);
        assert(set.Set().capacity == 8191);
        const auto order = set.Order();
        const std::vector<uint32_t> prefix{1725,3576,224,933,1566,77,904,1286,1751,3071,46,1712,3589,754,2152,
                                          3511,410,839,1154,3640,288,1135,2986,1228,3825,2036,2402,3241,2623,3450};
        assert(std::equal(prefix.begin(), prefix.end(), order.begin()));
        for (unsigned id = 0; id < count; id += 3) set.Erase(id);
        const auto before = set.Order();
        set.Drop();
        auto after = set.Order(), sortedBefore = before;
        std::sort(after.begin(), after.end()); std::sort(sortedBefore.begin(), sortedBefore.end());
        assert(after == sortedBefore);
        for (unsigned id = 0; id < count; id += 3) set.Insert(id);
        auto restored = set.Order(); std::sort(restored.begin(), restored.end());
        for (unsigned id = 0; id < count; ++id) assert(restored[id] == id);
    }
    assert(!g_residentScope);
}

int main(int argc, char** argv)
{
    // SDK tests link the loader's static C++ runtime. Load the same tiny foreign
    // dynamic-runtime fixture as the unwind test so production runtime checks
    // remain unchanged and can resolve the game's dynamic personality.
    assert(argc == 1 || argc == 2);
    if (argc == 2) assert(dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL));
    assert(!Tpf2mpInstallResidentHash(1, "unknown"));
    assert(!Tpf2mpInstallResidentHash(0, kResidentBuildId));
    assert(!Tpf2mpInstallResidentHash(1, nullptr));
    const size_t imageSize = 0x3f20000;
    void* image = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    for (const auto& context : kResidentContexts)
        std::memcpy(reinterpret_cast<void*>(base + context.rva), context.code, context.size);
    for (const auto& region : kResidentFixtureRegions)
        std::memcpy(reinterpret_cast<void*>(base + region.rva), region.code, region.size);
    assert(mprotect(image, imageSize, PROT_READ | PROT_EXEC) == 0);
    for (const auto& context : kResidentContexts) {
        const uint8_t changed = context.code[context.size / 2] ^ 1;
        Write(base + context.rva + context.size / 2, &changed, 1);
        assert(!Tpf2mpInstallResidentHash(base, kResidentBuildId));
        assert(!g_residentActiveMask);
        Write(base + context.rva + context.size / 2, context.code + context.size / 2, 1);
    }
    for (unsigned fail = 1; fail <= kResidentSiteCount; ++fail) CheckRollback(base, fail);
    assert(Tpf2mpInstallResidentHash(base, kResidentBuildId));
    assert(g_residentActiveMask == 63 && g_residentReady);
    CheckGate(base);
    CheckOriginalContainers(base);
    unsigned char* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    const uint32_t ids[] = {0, 1, 2, 3, 20839, 20840, 20841, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
    unsigned cases = 0;
    for (unsigned index = 0; index < 5; ++index) for (unsigned alignment : {0u, 8u}) {
        const Fixture run = MakeFixture(base, kResidentSites[index], alignment, page);
        for (uint32_t id : ids) for (unsigned variant : {0u, 1u}) {
            CheckRegisters(run, index, alignment, id, variant, true); ++cases;
            if (index >= 3) { CheckRegisters(run, index, alignment, id, variant, false); ++cases; }
        }
    }
    assert(!Tpf2mpInstallResidentHash(base, kResidentBuildId));
    assert(g_residentActiveMask == 63);
    assert(std::strstr(Tpf2mpResidentHashStatus(), "already active"));
    assert(munmap(page, 4096) == 0 && munmap(image, imageSize) == 0);
    std::printf("resident hash: %u GP/XMM/flags/stack cases, caller/pointer/TLS gates, six rollback points, 300 original Windows container states and 4000-ID grow/erase/drop/reinsert passed\n", cases);
}
