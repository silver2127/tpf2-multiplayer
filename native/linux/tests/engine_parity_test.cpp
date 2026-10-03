// Engine/distribution parity: MSVC algorithms against golden values produced
// by executing the Windows build-35924 code (engine_parity_golden_linux.h),
// and the installed hooks/patches executing the REAL native bytes (copied into
// a fake image at their RVAs) against the same golden values.
#include "../src/engine_parity_linux.cpp"
#include "engine_parity_golden_linux.h"
#include <sys/mman.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// boost::random::mt19937 native layout: u32[624], size_t index at +0x9c0.
struct BoostMT { uint32_t words[624]; uint64_t index; };
static_assert(offsetof(BoostMT, index) == 0x9c0);
static void BoostSeed(BoostMT* mt, uint32_t seed)
{
    Tpf2mpMt19937 ref; Tpf2mpMtSeed(&ref, seed);
    std::memcpy(mt->words, ref.state, sizeof(mt->words)); mt->index = 624;
}
static void BoostTwist(BoostMT* mt)
{
    for (uint32_t k = 0; k < 624; ++k) {
        const uint32_t y = (mt->words[k] & 0x80000000u) | (mt->words[(k + 1) % 624] & 0x7fffffffu);
        mt->words[k] = mt->words[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
    }
    mt->index = 0;
}
// libstdc++ std::mt19937: u64[624], size_t index at +0x1380.
struct GnuMT { uint64_t words[624]; uint64_t index; };
static_assert(offsetof(GnuMT, index) == 0x1380);
static void GnuSeed(GnuMT* mt, uint32_t seed)
{
    Tpf2mpMt19937 ref; Tpf2mpMtSeed(&ref, seed);
    for (int i = 0; i < 624; ++i) mt->words[i] = ref.state[i];
    mt->index = 624;
}
static uint32_t Untemper(uint32_t y)
{
    uint32_t x = y;
    for (unsigned i = 0; i < 32; ++i) x = y ^ (x >> 18);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ ((x << 15) & 0xefc60000u);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ ((x << 7) & 0x9d2c5680u);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ (x >> 11);
    return x;
}
static void Jump(unsigned char* p, uintptr_t target)
{
    const unsigned char op[] = {0xff,0x25,0,0,0,0};
    std::memcpy(p, op, sizeof(op)); std::memcpy(p + 6, &target, 8);
}
static float NextAfterF(float a, float b) { return std::nextafter(a, b); }
static double NextAfterD(double a, double b) { return std::nextafter(a, b); }

// Run a code window that expects RCX and ends in a RET placed by the test.
// The stack is moved below the red zone and aligned like a real call site.
__attribute__((noinline)) static float CallWithRcx(uintptr_t code, uint64_t rcx)
{
    uint32_t out;
    __asm__ volatile(
        "mov %%rsp, %%rbx\n\t"
        "sub $128, %%rsp\n\tand $-16, %%rsp\n\t"
        "mov %[v], %%rcx\n\t"
        "call *%[c]\n\t"
        "mov %%rbx, %%rsp\n\t"
        "movd %%xmm0, %[o]\n\t"
        : [o] "=r"(out) : [c] "r"(code), [v] "r"(rcx)
        : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","memory","cc",
          "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7");
    float f; std::memcpy(&f, &out, 4); return f;
}
// Enter an interior call-site snippet (which itself CALLs) with a stack whose
// alignment matches the original function body: RSP % 16 == 0 at the snippet.
__attribute__((noinline)) static void CallSite3(uintptr_t code, void* a, void* b, void* c)
{
    __asm__ volatile(
        "mov %%rsp, %%rbx\n\t"
        "sub $128, %%rsp\n\tand $-16, %%rsp\n\tsub $8, %%rsp\n\t"
        "mov %[a], %%rdi\n\tmov %[b], %%rsi\n\tmov %[c2], %%rdx\n\t"
        "call *%[f]\n\t"
        "mov %%rbx, %%rsp\n\t"
        : : [f] "r"(code), [a] "r"(a), [b] "r"(b), [c2] "r"(c)
        : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","memory","cc",
          "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
          "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}

static void TestModels()
{
    // G1: uniform_int<size_t> (boost mt 5489).
    Tpf2mpMt19937 mt; Tpf2mpMtSeed(&mt, 5489);
    const size_t n1 = sizeof(kGoldenU64Range) / sizeof(kGoldenU64Range[0]);
    for (size_t i = 0; i < n1; ++i) {
        const uint64_t hi = kGoldenU64Range[i] ? kGoldenU64Range[i] - 1 : UINT64_MAX;
        assert(Tpf2mpMsvcUniformU64(0, hi, &mt, Tpf2mpMtNext) == kGoldenU64Value[i]);
    }
    // G2: shuffle<uint32>, n=40 then n=7 on the same engine.
    Tpf2mpMtSeed(&mt, 5489);
    uint32_t a[40]; for (uint32_t i = 0; i < 40; ++i) a[i] = i;
    Tpf2mpMsvcShuffle(a, 40, 4, &mt, Tpf2mpMtNext);
    assert(std::memcmp(a, kGoldenShuffle40, sizeof(a)) == 0);
    uint32_t b[7]; for (uint32_t i = 0; i < 7; ++i) b[i] = i;
    Tpf2mpMsvcShuffle(b, 7, 4, &mt, Tpf2mpMtNext);
    assert(std::memcmp(b, kGoldenShuffle7After, sizeof(b)) == 0);
    // G3: Random(std::mt19937, 0, n) == uniform_int<int>(0, n-1).
    Tpf2mpMtSeed(&mt, 77);
    for (size_t i = 0; i < sizeof(kGoldenStdIntHi) / sizeof(kGoldenStdIntHi[0]); ++i)
        assert(Tpf2mpWindowsUniformIntInclusive(0, kGoldenStdIntHi[i] - 1, &mt, Tpf2mpMtNext) == kGoldenStdIntValue[i]);
    // G5: minstd_rand unit float of the next state.
    for (size_t i = 0; i < sizeof(kGoldenMinstdState) / sizeof(kGoldenMinstdState[0]); ++i) {
        const uint32_t g = uint32_t((uint64_t(kGoldenMinstdState[i]) * 48271u) % 2147483647u);
        assert(Bits(Tpf2mpMsvcMinstdUnitFloat(g)) == kGoldenMinstdUnitBits[i]);
    }
    // G6: transformator draws.
    for (size_t i = 0; i < sizeof(kGoldenEmitId) / sizeof(kGoldenEmitId[0]); i += 4) {
        Tpf2mpMtSeed(&mt, uint32_t(kGoldenEmitId[i]) * 31u + 7u);
        for (size_t k = 0; k < 4; ++k)
            assert(Bits(Tpf2mpMsvcUnitFloat(Tpf2mpMtNext(&mt))) == kGoldenEmitBits[i + k]);
    }
    // G7: town-name order.
    for (size_t s = 0; s < 3; ++s) {
        Tpf2mpMtSeed(&mt, kGoldenNameSeed[s]);
        uint32_t order[20]; for (uint32_t i = 0; i < 20; ++i) order[i] = i;
        Tpf2mpMsvcShuffle(order, 20, 4, &mt, Tpf2mpMtNext);
        assert(std::memcmp(order, kGoldenNameOrder + 20 * s, sizeof(order)) == 0);
    }
}

// Simulate InstallHook reporting failure after its target became visible.
static uintptr_t g_failHook;
static bool PartialHookFailure(uintptr_t target, void* detour, int stolen, void** original)
{
    if (target != g_failHook) return InstallHook(target, detour, stolen, original);
    const unsigned char changed = 0xcc;
    int error = 0;
    assert(Tpf2mpCodeWriteSelf(target, &changed, 1, &error) == TPF2MP_CW_OK);
    return false;
}

int main()
{
    TestModels();

    assert(!InstallEngineParityWith(1, "unknown", InstallHook, Tpf2mpCodeWriteSelf));
    constexpr size_t length = (0x3e8c000 + 8191) & ~size_t(4095);
    auto* memory = static_cast<unsigned char*>(mmap(nullptr, length,
        PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    for (const auto& g : kGuards) std::memcpy(memory + g.rva, g.bytes, g.size);
    std::memcpy(memory + kOneRva, &kOneBits, 4);
    std::memcpy(memory + kTwoPowMinus31Rva, &kTwoPowMinus31Bits, 4);
    const uint32_t twoPowMinus32 = 0x2f800000u;
    std::memcpy(memory + 0x3e8be70, &twoPowMinus32, 4);          // std unit-float scale
    Jump(memory + 0x14ed8d0, reinterpret_cast<uintptr_t>(BoostTwist));
    Jump(memory + 0x6dc590, reinterpret_cast<uintptr_t>(NextAfterF));
    Jump(memory + 0x6dcd30, reinterpret_cast<uintptr_t>(NextAfterD));

    // Every failed hook, including a partially written target, is restored.
    for (const auto& h : kHooks) {
        g_failHook = base + h.rva;
        assert(!InstallEngineParityWith(base, kParityBuildId, PartialHookFailure, Tpf2mpCodeWriteSelf));
        assert(g_activeMask == 0 && !g_ready);
        for (const auto& g : kGuards)
            assert(std::memcmp(memory + g.rva, g.bytes, g.size) == 0);
    }

    // Every guard byte is load-bearing.
    for (const auto& g : kGuards) {
        memory[g.rva + g.size / 2] ^= 0x40;
        assert(!InstallEngineParityWith(base, kParityBuildId, InstallHook, Tpf2mpCodeWriteSelf));
        assert(g_activeMask == 0);
        memory[g.rva + g.size / 2] ^= 0x40;
    }
    setenv("TPF2MP_ENGINE_PARITY", "0", 1);
    assert(!InstallEngineParityWith(base, kParityBuildId, InstallHook, Tpf2mpCodeWriteSelf));
    unsetenv("TPF2MP_ENGINE_PARITY");
    assert(InstallEngineParityWith(base, kParityBuildId, InstallHook, Tpf2mpCodeWriteSelf));
    std::printf("status: %s\n", Tpf2mpEngineParityStatus());
    assert(!InstallEngineParityWith(base, kParityBuildId, InstallHook, Tpf2mpCodeWriteSelf));

    // G1 through the hooked native uniform_int<size_t>(boost mt), raw draws from
    // the native full-range int path and native twist call site.
    static BoostMT boost;
    BoostSeed(&boost, 5489);
    using U64 = uint64_t (*)(void*, void*, const uint64_t*);
    for (size_t i = 0; i < sizeof(kGoldenU64Range) / sizeof(kGoldenU64Range[0]); ++i) {
        const uint64_t p[2] = {0, kGoldenU64Range[i] ? kGoldenU64Range[i] - 1 : UINT64_MAX};
        assert(reinterpret_cast<U64>(base + kUniformU64BoostRva)(nullptr, &boost, p) == kGoldenU64Value[i]);
    }
    // G2 through the hooked native shuffles (4-byte, then 8-byte pairs).
    BoostSeed(&boost, 5489);
    using Shuf = void (*)(void*, void*, void*);
    uint32_t a[40]; for (uint32_t i = 0; i < 40; ++i) a[i] = i;
    reinterpret_cast<Shuf>(base + kShuffle4BoostRva)(a, a + 40, &boost);
    assert(std::memcmp(a, kGoldenShuffle40, sizeof(a)) == 0);
    BoostSeed(&boost, 5489);
    uint32_t pairs[80]; for (uint32_t i = 0; i < 40; ++i) { pairs[2 * i] = i; pairs[2 * i + 1] = 1000 + i; }
    reinterpret_cast<Shuf>(base + kShuffle8BoostRva)(pairs, pairs + 80, &boost);
    for (uint32_t i = 0; i < 40; ++i)
        assert(pairs[2 * i] == kGoldenShuffle40[i] && pairs[2 * i + 1] == 1000 + kGoldenShuffle40[i]);
    // G3 through the hooked native uniform_int<int>(std mt) with the native raw operator.
    static GnuMT gnu;
    GnuSeed(&gnu, 77);
    using I32 = int32_t (*)(void*, void*, const int32_t*);
    for (size_t i = 0; i < sizeof(kGoldenStdIntHi) / sizeof(kGoldenStdIntHi[0]); ++i) {
        const int32_t p[2] = {0, kGoldenStdIntHi[i] - 1};
        assert(reinterpret_cast<I32>(base + kUniformI32StdRva)(nullptr, &gnu, p) == kGoldenStdIntValue[i]);
    }
    // uint32 / uint64 std variants agree with the verified model.
    GnuSeed(&gnu, 3);
    Tpf2mpMt19937 ref; Tpf2mpMtSeed(&ref, 3);
    using U32 = uint32_t (*)(void*, void*, const uint32_t*);
    for (uint32_t n : {2u, 17u, 1000u, 70000u, 0xffffffffu}) {
        const uint32_t lo = n == 0xffffffffu ? 0 : 5;
        const uint32_t p[2] = {lo, lo + (n - 1)};
        assert(reinterpret_cast<U32>(base + kUniformU32StdRva)(nullptr, &gnu, p) ==
               Tpf2mpMsvcUniformU32(p[0], p[1], &ref, Tpf2mpMtNext));
        const uint64_t q[2] = {9, 9 + uint64_t(n) * 3};
        assert(reinterpret_cast<U64>(base + kUniformU64StdRva)(nullptr, &gnu, q) ==
               Tpf2mpMsvcUniformU64(9, 9 + uint64_t(n) * 3, &ref, Tpf2mpMtNext));
    }
    // Std unit float: a raw 0xffffffc0 now yields exactly 1.0 (Windows), not nextafter.
    GnuSeed(&gnu, 1); gnu.index = 0; gnu.words[0] = Untemper(0xffffffc0u);
    using Unit = float (*)(void*);
    assert(reinterpret_cast<Unit>(base + kUnitStdRva)(&gnu) == 1.0f);
    assert(memory[kUnitStdClampRva] == 0x66 && memory[kDoubleBoostClampRva] == 0x66);

    // G6: the two seed stores bind the sidecar to rbp-0x248 with [r12]*31+7;
    // the hooked native draw helper then yields the Windows floats.
    for (size_t i = 0; i < sizeof(kGoldenEmitId) / sizeof(kGoldenEmitId[0]); i += 4) {
        static unsigned char frame[0x400];
        int32_t id = kGoldenEmitId[i];
        Registers r{};
        r.rbp = reinterpret_cast<uintptr_t>(frame) + 0x300;
        r.r12 = reinterpret_cast<uintptr_t>(&id);
        r.resume = (i / 4) % 2;
        Tpf2mpEngineParityDispatch(&r);
        assert(r.resume == reinterpret_cast<uintptr_t>((i / 4) % 2 ? g_origEmitStoreB : g_origEmitStoreA));
        void* engine = frame + 0x300 - 0x248;
        for (size_t k = 0; k < 4; ++k)
            assert(Bits(reinterpret_cast<Unit>(base + kEmitUnitRva)(engine)) == kGoldenEmitBits[i + k]);
    }
    // An unbound minstd engine keeps the native conversion.
    uint64_t lcg = 1;
    const float native = reinterpret_cast<Unit>(base + kEmitUnitRva)(&lcg);
    assert(lcg == 16807 && native == float(16806) * 0x1p-31f);

    // G5: execute the patched AirConnectParts window with RCX = next state.
    memory[kAirResumeRva] = 0xc3;
    for (size_t i = 0; i < sizeof(kGoldenMinstdState) / sizeof(kGoldenMinstdState[0]); ++i) {
        const uint32_t g = uint32_t((uint64_t(kGoldenMinstdState[i]) * 48271u) % 2147483647u);
        assert(Bits(CallWithRcx(base + kAirPatchRva, g)) == kGoldenMinstdUnitBits[i]);
    }

    // G7: arm the name entry (ECX = seed) and run the real call site
    // 0xc450f3 (call shuffle) with a RET placed at its return address.
    memory[kNameShuffleReturnRva] = 0xc3;
    for (size_t s = 0; s < 3; ++s) {
        Registers r{};
        r.rcx = kGoldenNameSeed[s];
        r.resume = 2;
        Tpf2mpEngineParityDispatch(&r);
        assert(r.resume == reinterpret_cast<uintptr_t>(g_origNameEntry));
        uint32_t order[20]; for (uint32_t i = 0; i < 20; ++i) order[i] = i;
        uint64_t minstd = 1;
        CallSite3(base + 0xc450f3, order, order + 20, &minstd);
        assert(std::memcmp(order, kGoldenNameOrder + 20 * s, sizeof(order)) == 0);
        assert(minstd == 1); // the native engine is not consumed
    }
    std::puts("engine parity: all checks passed");
    return 0;
}
