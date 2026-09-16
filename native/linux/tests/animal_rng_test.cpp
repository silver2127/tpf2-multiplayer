// Isolated actual patch execution: no game files or live processes are used.
#include "../src/animal_rng_linux.cpp"
#include <sys/mman.h>
#include <array>
#include <cassert>
#include <vector>
#include <thread>
#include <cmath>
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

static Fixture MakeFixture(uintptr_t base, uintptr_t entryRva, uintptr_t resumeRva, unsigned alignment,
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
    out.JumpTo(base + entryRva);

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
    Write(base + resumeRva, jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}

static uintptr_t g_testBase;
static Fixture g_workerDrawFixture, g_workerBindFixture, g_spawnBindFixture;
static thread_local unsigned g_observedDraws;
static thread_local bool g_nestedWorker;
static thread_local uint32_t g_expectedWorkerSeed, g_expectedSpawnSeed;

static uint32_t FloatBits(float value) { uint32_t x; std::memcpy(&x, &value, 4); return x; }
static float ExpectedUnit(std::mt19937& engine) { return WindowsAnimalUnit(engine); }
static UnitFn PatchedUnit() { return reinterpret_cast<UnitFn>(g_testBase + kUnitCoreRva); }
static IntegerFn PatchedInteger() { return reinterpret_cast<IntegerFn>(g_testBase + kIntegerCoreRva); }
static const int32_t kIntegerRanges[][2] = {
    {0, 0}, {-17, -17}, {0, 1}, {0, 9}, {-99, 100}, {0, INT32_MAX},
    {INT32_MIN, INT32_MAX}, {INT32_MIN, INT32_MAX - 1}, {-1, INT32_MAX},
    {INT32_MIN, 0}, {0, 2147483645}, {0, 9999},
};

static void CheckIntegerDraw(void* engine, std::mt19937& reference, unsigned ordinal)
{
    const auto& range = kIntegerRanges[ordinal % (sizeof(kIntegerRanges) / sizeof(kIntegerRanges[0]))];
    // These expected mappings have an independent original-Windows machine
    // oracle. This fixture checks hook selection and the shared draw stream.
    const int32_t expected = WindowsAnimalInt(reference, range[0], range[1]);
    assert(PatchedInteger()(nullptr, engine, range) == expected);
}

static float InlineUnit(uintptr_t frame)
{
    State input{}, output{};
    input.rbp = frame; input.flags = 0x202;
    g_workerDrawFixture(&input, &output);
    float value; std::memcpy(&value, output.xmm[0], 4); return value;
}

static void CheckRegisters(Fixture run, unsigned alignment, unsigned site, uint32_t seed)
{
    State input{}, output{};
    for (unsigned i = 0; i < 15; ++i)
        reinterpret_cast<uint64_t*>(&input)[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + seed);
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + seed;
    input.flags = 0x202 | (seed & 1 ? 0x8d5 : 0);
    uint64_t slots[3] = {0xabcdef, 0x1234, 0xfeedab};
    input.rbp = reinterpret_cast<uintptr_t>(&slots[1]) + (site == 1 ? 0x168 : 0xf0);
    input.rdx = UINT64_C(0x7654321000000000) | seed;
    AnimalScope scope(site == 1 ? AnimalKind::Spawn : AnimalKind::Worker, seed);
    scope.nativeEngine = &slots[1];
    State expected = input;
    const unsigned originalMxcsr = _mm_getcsr(), before = originalMxcsr | 1u;
    _mm_setcsr(before);
    if (site == 2) {
        std::mt19937 reference(seed);
        float value = ExpectedUnit(reference);
        std::memset(expected.xmm[0], 0, 16); std::memcpy(expected.xmm[0], &value, 4);
    }
    const unsigned expectedMxcsr = _mm_getcsr(); _mm_setcsr(before);
    run(&input, &output);
    assert(_mm_getcsr() == expectedMxcsr); _mm_setcsr(originalMxcsr);
    assert(std::memcmp(&expected, &output, 15 * sizeof(uint64_t)) == 0);
    assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)) == 0);
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == alignment);
    assert(scope.nativeEngine == &slots[1]);
    assert(slots[1] == (site == 2 ? 0x1234 : input.rdx));
    assert(slots[0] == 0xabcdef && slots[2] == 0xfeedab);
    FinishAnimalScope(&scope);
}

// Called from real native entry/initializer prefixes in the private image.
// These observers replace only later game-world work, after the installed
// entry wrappers, original prologues and binding stores have all executed.
static void ObserveWorker(uintptr_t frame)
{
    assert(g_animalScope && g_animalScope->kind == AnimalKind::Worker);
    assert(g_animalScope->seed == g_expectedWorkerSeed);
    assert(g_animalScope->nativeEngine == reinterpret_cast<void*>(frame - 0xf0));
    std::mt19937 reference(g_animalScope->seed);
    for (unsigned i = 0; i < 1300; ++i) {
        if (i % 3 == 2) CheckIntegerDraw(g_animalScope->nativeEngine, reference, i / 3);
        else {
            float actual = i & 1 ? InlineUnit(frame) : PatchedUnit()(g_animalScope->nativeEngine);
            assert(FloatBits(actual) == FloatBits(ExpectedUnit(reference)));
        }
        ++g_observedDraws;
    }
    assert(g_animalScope->engine == reference);
}

static void ObserveSpawn(uintptr_t frame)
{
    assert(g_animalScope && g_animalScope->kind == AnimalKind::Spawn);
    assert(g_animalScope->seed == g_expectedSpawnSeed);
    assert(g_animalScope->nativeEngine == reinterpret_cast<void*>(frame - 0x168));
    AnimalScope* outer = g_animalScope;
    std::mt19937 reference(outer->seed);
    for (unsigned i = 0; i < 1300; ++i) {
        if (i == 321 && g_nestedWorker) {
            unsigned char capture[0x30]{}; uint32_t time = 756000;
            std::memcpy(capture + 0x28, &time, 4);
            unsigned char result[72]{};
            g_expectedWorkerSeed = Tpf2mpWindowsTimeSeed(17, time);
            void* returned = reinterpret_cast<WorkerFn>(g_testBase + kWorkerPrefixRva)(result, capture, 17, 21);
            assert(returned == result && g_animalScope == outer);
        }
        if (i % 3 == 2) CheckIntegerDraw(outer->nativeEngine, reference, i / 3);
        else assert(FloatBits(PatchedUnit()(outer->nativeEngine)) == FloatBits(ExpectedUnit(reference)));
        ++g_observedDraws;
    }
    assert(outer->engine == reference);
}

static void RelJump(uintptr_t at, uintptr_t to)
{
    const intptr_t d = intptr_t(to) - intptr_t(at + 5); assert(d >= INT32_MIN && d <= INT32_MAX);
    unsigned char bytes[5] = {0xe9}; const int32_t relative = int32_t(d);
    std::memcpy(bytes + 1, &relative, 4); Write(at, bytes, 5);
}

static void MakeObserver(uintptr_t at, void (*observer)(uintptr_t), uintptr_t after)
{
    Emitter e;
    e.Bytes({0x48,0x89,0xef,0x48,0xb8}); // RDI=original RBP; call observer
    uintptr_t target = reinterpret_cast<uintptr_t>(observer);
    for (unsigned i=0;i<8;++i) e.code.push_back(target >> (i*8));
    e.Bytes({0xff,0xd0}); e.JumpTo(after);
    Write(at,e.code.data(),e.code.size());
}

static unsigned g_hookCalls, g_failHook;
static bool FailAfterHook(uintptr_t at, void* target, int n, void** original)
{
    assert(InstallHook(at, target, n, original));
    return ++g_hookCalls != g_failHook;
}
static int RefuseRestore(uintptr_t, const uint8_t*, size_t, int*) { return TPF2MP_CW_UNAVAILABLE; }

int main()
{
    // The production game already provides this dynamic runtime. CTest itself
    // links the hidden static runtime, so supply the game-like runtime here.
    assert(dlopen("libstdc++.so.6", RTLD_NOW | RTLD_GLOBAL));
    assert(!Tpf2mpInstallAnimalRng(1, "unknown"));
    assert(!Tpf2mpInstallAnimalRng(1, nullptr));
    assert(!Tpf2mpInstallAnimalRng(0, kAnimalBuildId));
    constexpr size_t size = 0x3f1e000;
    void* image = mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image != MAP_FAILED); g_testBase = reinterpret_cast<uintptr_t>(image);
    for (const auto& guard : kAnimalGuards) std::memcpy(reinterpret_cast<void*>(g_testBase+guard.rva),guard.bytes,guard.size);
    std::memcpy(reinterpret_cast<void*>(g_testBase+kAnimalLcgScaleRva),&kAnimalLcgScale,4);
    std::memcpy(reinterpret_cast<void*>(g_testBase+kAnimalOneRva),&kAnimalOne,4);
    assert(mprotect(image,size,PROT_READ|PROT_EXEC)==0);
    for (const auto& guard : kAnimalGuards) {
        const uint8_t bad = guard.bytes[0]^1;
        Write(g_testBase+guard.rva,&bad,1);
        assert(!Tpf2mpInstallAnimalRng(g_testBase,kAnimalBuildId));
        assert(!g_animalReady && !g_animalActiveMask);
        Write(g_testBase+guard.rva,guard.bytes,1);
    }
    const uint32_t badScale = kAnimalLcgScale ^ 1;
    Write(g_testBase+kAnimalLcgScaleRva,&badScale,4);
    assert(!Tpf2mpInstallAnimalRng(g_testBase,kAnimalBuildId));
    Write(g_testBase+kAnimalLcgScaleRva,&kAnimalLcgScale,4);
    for (unsigned failure=1;failure<=kAnimalHookCount;++failure) {
        g_hookCalls=0;g_failHook=failure;
        assert(!InstallAnimalRng(g_testBase,kAnimalBuildId,FailAfterHook,Tpf2mpCodeWriteSelf));
        assert(!g_animalReady && !g_animalActiveMask);
        for(const auto& guard:kAnimalGuards)
            assert(!std::memcmp(reinterpret_cast<void*>(g_testBase+guard.rva),guard.bytes,guard.size));
    }
    g_hookCalls=0;g_failHook=3;
    assert(!InstallAnimalRng(g_testBase,kAnimalBuildId,FailAfterHook,RefuseRestore));
    assert(!g_animalReady && g_animalActiveMask == 7);
    assert(std::strstr(Tpf2mpAnimalRngStatus(),"ERROR"));
    {
        uint64_t originalEngine=1;
        AnimalScope scope(AnimalKind::Worker,9);scope.nativeEngine=&originalEngine;
        assert(FloatBits(PatchedUnit()(&originalEngine))==FloatBits(float(16806)/2147483648.0f));
        assert(originalEngine==16807); // partial installation remains inert
        FinishAnimalScope(&scope);
    }
    for(const auto& guard:kAnimalGuards)Write(g_testBase+guard.rva,guard.bytes,guard.size);
    g_animalActiveMask=0;
    assert(Tpf2mpInstallAnimalRng(g_testBase,kAnimalBuildId));
    assert(g_animalReady && g_animalActiveMask == (1u << kAnimalHookCount) - 1);

    auto* pages=static_cast<unsigned char*>(mmap(nullptr,6*4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(pages!=MAP_FAILED);
    // The complete original helper is position independent on successful
    // calls, including its relative self-recursion for wide valid ranges.
    // Execute a separate unmodified copy to verify unbound engines retain
    // both original integer values and the original LCG advancement.
    Write(reinterpret_cast<uintptr_t>(pages+3*4096),kIntegerCoreBytes,sizeof(kIntegerCoreBytes));
    const auto nativeInteger=reinterpret_cast<IntegerFn>(pages+3*4096);
    unsigned originalIntegerCases=0;
    uint64_t boundStorage=1;
    AnimalScope unrelatedScope(AnimalKind::Spawn,17); unrelatedScope.nativeEngine=&boundStorage;
    const std::mt19937 unusedReference=unrelatedScope.engine;
    for(uint64_t seed:{UINT64_C(1),UINT64_C(2),UINT64_C(5489),UINT64_C(2147483646)}) {
        uint64_t actual=seed,expected=seed;
        for(unsigned i=0;i<1200;++i) {
            const auto& range=kIntegerRanges[i%(sizeof(kIntegerRanges)/sizeof(kIntegerRanges[0]))];
            assert(PatchedInteger()(nullptr,&actual,range)==nativeInteger(nullptr,&expected,range));
            assert(actual==expected); ++originalIntegerCases;
        }
    }
    assert(unrelatedScope.engine==unusedReference);
    FinishAnimalScope(&unrelatedScope);
    const uintptr_t entries[]={0x1674d5d,0x178c79e,0x1674faa};
    const uintptr_t resumes[]={0x1674d64,0x178c7a5,0x167501c};
    const uint32_t seeds[]={0,1,5489,756000,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
    unsigned registerCases=0;
    for(unsigned site=0;site<3;++site)for(unsigned alignment:{0u,8u}) {
        Fixture run=MakeFixture(g_testBase,entries[site],resumes[site],alignment,pages+site*4096);
        for(uint32_t seed:seeds){CheckRegisters(run,alignment,site,seed);++registerCases;}
    }
    g_workerBindFixture=MakeFixture(g_testBase,entries[0],resumes[0],0,pages);
    g_spawnBindFixture=MakeFixture(g_testBase,entries[1],resumes[1],0,pages+4096);
    g_workerDrawFixture=MakeFixture(g_testBase,entries[2],resumes[2],0,pages+8192);

    // Restore original seed continuations before running real native prefixes.
    // The register fixtures wrote14-byte captures beyond the guarded prefixes;
    // exact original early-return/epilogue code is supplied below by the fixture.
    const unsigned char workerReturn[]={
        0x31,0xc0,0x31,0xd2,0x31,0xc9,0x31,0xf6,0x31,0xff,0x45,0x31,0xc0,0x45,0x31,0xc9,
        0x45,0x31,0xd2,0x45,0x31,0xdb,0x49,0x89,0x44,0x24,0x40,0x4c,0x89,0xe0,
        0x4d,0x89,0x1c,0x24,0x4d,0x89,0x54,0x24,0x08,0x4d,0x89,0x4c,0x24,0x10,
        0x4d,0x89,0x44,0x24,0x18,0x49,0x89,0x7c,0x24,0x20,0x49,0x89,0x74,0x24,0x28,
        0x49,0x89,0x4c,0x24,0x30,0x49,0x89,0x54,0x24,0x38,
        0x48,0x8d,0x65,0xd8,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    Write(g_testBase+0x1674d69,workerReturn,sizeof(workerReturn));
    MakeObserver(g_testBase+0x178e000,ObserveWorker,g_testBase+0x1674d69);
    RelJump(g_testBase+resumes[0],g_testBase+0x178e000);
    const unsigned char spawnReturn[]={0x48,0x8d,0x65,0xd8,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    Write(g_testBase+0x178cc4d,spawnReturn,sizeof(spawnReturn));
    MakeObserver(g_testBase+0x178f000,ObserveSpawn,g_testBase+0x178cc4d);
    RelJump(g_testBase+resumes[1],g_testBase+0x178f000);
    const unsigned char zeroFloat[]={0x0f,0x57,0xc0,0x0f,0x57,0xc9,0xc3};
    Write(g_testBase+0xcf5410,zeroFloat,sizeof(zeroFloat));
    Write(g_testBase+0x1786af0,zeroFloat,sizeof(zeroFloat));
    // Spawn's pre-seed area/rounding arithmetic consumes these original
    // constants; values are supplied below from the checked native prefix.
    const std::array<std::array<uint32_t,2>,5> constants{{
        {{0x3e8bdf4,0x358637bd}},{{0x3f1db40,0x7fffffff}},
        {{0x3e8bdcc,0x3f000000}},{{0x3e8bdd0,0x4b000000}},{{0x3e8bdc8,0x3f800000}}
    }};
    for(const auto& c:constants)Write(g_testBase+c[0],&c[1],4);

    uint32_t config=0; unsigned char capture[0x30]{}; unsigned char result[72]{};
    const auto worker=reinterpret_cast<WorkerFn>(g_testBase+kWorkerPrefixRva);
    const auto spawn=reinterpret_cast<SpawnFn>(g_testBase+kSpawnPrefixRva);
    for(uint32_t seed:seeds) {
        std::memcpy(capture+0x28,&seed,4);
        g_expectedWorkerSeed=Tpf2mpWindowsTimeSeed(seed,seed);
        assert(worker(result,capture,int32_t(seed),int32_t(seed))==result);
        assert(!g_animalScope);
        g_expectedSpawnSeed=seed;
        spawn(nullptr,nullptr,nullptr,nullptr,nullptr,&config,nullptr,int32_t(seed));
        assert(!g_animalScope);
    }
    g_nestedWorker=true;
    g_expectedSpawnSeed=991;
    spawn(nullptr,nullptr,nullptr,nullptr,nullptr,&config,nullptr,991);
    g_nestedWorker=false;assert(!g_animalScope);

    // Completed scopes cannot affect another LCG at the same address.
    uint64_t reused=1;
    {
        AnimalScope scope(AnimalKind::Spawn,77);scope.nativeEngine=&reused;
        std::mt19937 reference(77);
        uint64_t unbound=1;
        assert(FloatBits(PatchedUnit()(&unbound))==FloatBits(float(16806)/2147483648.0f));
        assert(unbound==16807);
        assert(!FindAnimalScope(nullptr));
        assert(FloatBits(PatchedUnit()(&reused))==FloatBits(ExpectedUnit(reference)));
        assert(reused==1);
        FinishAnimalScope(&scope);
    }
    assert(!g_animalScope);
    assert(FloatBits(PatchedUnit()(&reused))==FloatBits(float(16806)/2147483648.0f));
    assert(reused==16807);
    alignas(16) unsigned char fallbackFrame[0x200]{};
    uint64_t initialLcg=1;
    std::memcpy(fallbackFrame+0x90,&initialLcg,8);
    assert(FloatBits(InlineUnit(reinterpret_cast<uintptr_t>(fallbackFrame+0x180)))==FloatBits(float(16806)/2147483648.0f));
    std::memcpy(&initialLcg,fallbackFrame+0x90,8);assert(initialLcg==16807);
    // Foreign exceptions are exercised by animal_foreign_unwind_test with a
    // dynamic-runtime thrower, including nested cleanup and later address reuse.
    assert(!g_animalScope);
    std::array<std::thread,4> threads;
    for(unsigned n=0;n<threads.size();++n)threads[n]=std::thread([&,n]{
        uint32_t ownConfig=0;g_observedDraws=0;
        g_expectedSpawnSeed=n+1;
        spawn(nullptr,nullptr,nullptr,nullptr,nullptr,&ownConfig,nullptr,int32_t(n+1));
        assert(!g_animalScope&&g_observedDraws==1300);
    });
    for(auto& thread:threads)thread.join();

    // A delegated native request may recursively call the hooked helper.
    // Exercise the original call instruction with its exact return address:
    // even a bound pointer then stays on the native stream. The surrounding
    // synthetic prologue/epilogue supply only a standalone ABI-safe caller.
    const unsigned char enterRecursive[]={0x48,0x83,0xec,0x08};
    const unsigned char leaveRecursive[]={0x48,0x83,0xc4,0x08,0xc3};
    Write(g_testBase+0xd6f9bf,enterRecursive,sizeof(enterRecursive));
    Write(g_testBase+0xd6f9c8,leaveRecursive,sizeof(leaveRecursive));
    {
        uint64_t actual=1,expected=1;
        AnimalScope scope(AnimalKind::Spawn,5489);scope.nativeEngine=&actual;
        const std::mt19937 untouched=scope.engine;
        const int32_t range[]={0,9};
        assert(reinterpret_cast<IntegerFn>(g_testBase+0xd6f9bf)(nullptr,&actual,range)==nativeInteger(nullptr,&expected,range));
        assert(actual==expected&&scope.engine==untouched);
        FinishAnimalScope(&scope);
    }
    Write(g_testBase+0xd6f9bf,kIntegerCoreBytes+0xd6f9bf-kIntegerCoreRva,14);
    assert(!Tpf2mpInstallAnimalRng(g_testBase,kAnimalBuildId));
    std::printf("Animal RNG: %u full-register cases; %u main-thread mixed real-prefix draws; %u original integer cases; scopes, reuse, threads and seven-hook rollback passed\n",registerCases,g_observedDraws,originalIntegerCases);
    assert(munmap(pages,6*4096)==0&&munmap(image,size)==0);
}
