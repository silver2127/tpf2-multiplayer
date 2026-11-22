// engine_parity_linux.cpp -- Windows (MSVC STL) random-distribution and
// default_random_engine parity for the native build-35924 game.
//
// Evidence (scratchpad/emu_engines.py runs the real code of both binaries):
//   uniform_int<size_t>(boost mt)  Windows 0x140b6cf80 vs 0x14ed9e0: 44/192 agree
//   std::shuffle<uint32>(boost mt) Windows 0x140914e80 vs 0x14ede20:  1/15 agree
//   Random(std::mt19937,int,int)   Windows 0x142374e10 vs 0x31b74e0: 19/120 agree
//   TownBuildingTransformator: Windows std::mt19937(id*31+7) (0x140b533b6),
//     native minstd_rand0(id*31+7) (0x182ac5c/0x182cd69): unrelated streams
//   AirConnectParts minstd_rand float: 3886/4003 bit-identical (rounding of
//     float(g)-1 vs float(g-1)), plus the native nextafter clamp
//   MSVC models of all of the above agree 100% with the Windows code.
#include "engine_parity_linux.h"
#include "engine_parity_sites_linux.h"
#include "windows_msvc_random_linux.h"
#include "windows_uniform_int_linux.h"
#include "codewrite_linux.h"
#include "hook.h"
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char kParityBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";

// Constants referenced by the AirConnectParts patch (verified).
constexpr uintptr_t kOneRva = 0x3e8bdc8;           // 1.0f
constexpr uintptr_t kTwoPowMinus31Rva = 0x3e8beac; // 2^-31
constexpr uint32_t kOneBits = 0x3f800000u;
constexpr uint32_t kTwoPowMinus31Bits = 0x30000000u;

// Clamp branches (jae to the nextafter tail), 2 bytes each.
constexpr uintptr_t kUnitStdClampRva = 0x31b5f7e;     // 73 28
constexpr uintptr_t kDoubleBoostClampRva = 0x31b7759; // 73 5d

// AirConnectParts: replace [0x2eccc54, 0x2eccc87) after the minstd step.
constexpr uintptr_t kAirPatchRva = 0x2eccc54;
constexpr uintptr_t kAirResumeRva = 0x2eccc87;

// EmitModelInstances seed stores (mov [rbp-0x248],rax, 7 bytes each).
constexpr uintptr_t kEmitStoreARva = 0x182ac9d;
constexpr uintptr_t kEmitStoreBRva = 0x182cdc6;
// NameRep selection entry (seed in ECX) and its only shuffle call return.
constexpr uintptr_t kNameEntryRva = 0xc45010;
constexpr uintptr_t kNameShuffleReturnRva = 0xc450f8;

struct Guard { uintptr_t rva; const unsigned char* bytes; size_t size; };
const Guard kGuards[] = {
    {kUniformU64BoostRva, kUniformU64BoostBytes, sizeof(kUniformU64BoostBytes)},
    {kShuffleHelperBoostRva, kShuffleHelperBoostBytes, sizeof(kShuffleHelperBoostBytes)},
    {kShuffle8BoostRva, kShuffle8BoostBytes, sizeof(kShuffle8BoostBytes)},
    {kShuffle4BoostRva, kShuffle4BoostBytes, sizeof(kShuffle4BoostBytes)},
    {kUniformI32StdRva, kUniformI32StdBytes, sizeof(kUniformI32StdBytes)},
    {kUniformU32StdRva, kUniformU32StdBytes, sizeof(kUniformU32StdBytes)},
    {kUniformU64StdRva, kUniformU64StdBytes, sizeof(kUniformU64StdBytes)},
    {kRawStdRva, kRawStdBytes, sizeof(kRawStdBytes)},
    {kInclusiveBoostRva, kInclusiveBoostBytes, sizeof(kInclusiveBoostBytes)},
    {kUnitStdRva, kUnitStdBytes, sizeof(kUnitStdBytes)},
    {kCanonicalDoubleBoostRva, kCanonicalDoubleBoostBytes, sizeof(kCanonicalDoubleBoostBytes)},
    {kEmitUnitRva, kEmitUnitBytes, sizeof(kEmitUnitBytes)},
    {kEmitSeedARva, kEmitSeedABytes, sizeof(kEmitSeedABytes)},
    {kEmitSeedBRva, kEmitSeedBBytes, sizeof(kEmitSeedBBytes)},
    {kEmitDrawARva, kEmitDrawABytes, sizeof(kEmitDrawABytes)},
    {kEmitDrawBRva, kEmitDrawBBytes, sizeof(kEmitDrawBBytes)},
    {kAirDrawRva, kAirDrawBytes, sizeof(kAirDrawBytes)},
    {kAirTailRva, kAirTailBytes, sizeof(kAirTailBytes)},
    {kNamePrefixRva, kNamePrefixBytes, sizeof(kNamePrefixBytes)},
    {kNameShuffleRva, kNameShuffleBytes, sizeof(kNameShuffleBytes)},
};

uintptr_t g_base = 0;
bool g_ready = false;
unsigned g_activeMask = 0;
char g_status[192] = "off (not initialized)";

using InclusiveFn = int (*)(void*, const int*);
using RawStdFn = uint64_t (*)(void*);
InclusiveFn g_boostInclusive = nullptr;
RawStdFn g_stdRaw = nullptr;

uint32_t BoostRaw(void* engine)
{
    // Native full-range path of the boost-MT int distribution: one draw,
    // returned as raw + INT_MIN; the game's MT storage and twist are kept.
    const int bounds[] = {INT_MIN, INT_MAX};
    return uint32_t(g_boostInclusive(engine, bounds)) - uint32_t(INT_MIN);
}
uint32_t StdRaw(void* engine) { return uint32_t(g_stdRaw(engine)); }

// ---- global distribution replacements (all callers are the same template
// instances as on Windows, where they compile to the MSVC algorithms) -------
void* g_origU64Boost = nullptr;
void* g_origShuffle8 = nullptr;
void* g_origShuffle4 = nullptr;
void* g_origI32Std = nullptr;
void* g_origU32Std = nullptr;
void* g_origU64Std = nullptr;

using U64Dist = uint64_t (*)(void*, void*, const uint64_t*);
using U32Dist = uint32_t (*)(void*, void*, const uint32_t*);
using I32Dist = int32_t (*)(void*, void*, const int32_t*);

uint64_t UniformU64Boost(void* distribution, void* engine, const uint64_t* p)
{
    if (!g_ready || p[0] > p[1]) return reinterpret_cast<U64Dist>(g_origU64Boost)(distribution, engine, p);
    return Tpf2mpMsvcUniformU64(p[0], p[1], engine, BoostRaw);
}
uint64_t UniformU64Std(void* distribution, void* engine, const uint64_t* p)
{
    if (!g_ready || p[0] > p[1]) return reinterpret_cast<U64Dist>(g_origU64Std)(distribution, engine, p);
    return Tpf2mpMsvcUniformU64(p[0], p[1], engine, StdRaw);
}
uint32_t UniformU32Std(void* distribution, void* engine, const uint32_t* p)
{
    if (!g_ready || p[0] > p[1]) return reinterpret_cast<U32Dist>(g_origU32Std)(distribution, engine, p);
    return Tpf2mpMsvcUniformU32(p[0], p[1], engine, StdRaw);
}
int32_t UniformI32Std(void* distribution, void* engine, const int32_t* p)
{
    if (!g_ready || p[0] > p[1]) return reinterpret_cast<I32Dist>(g_origI32Std)(distribution, engine, p);
    return Tpf2mpWindowsUniformIntInclusive(p[0], p[1], engine, StdRaw);
}

using ShuffleFn = void (*)(void*, void*, void*);
void Shuffle8Boost(void* first, void* last, void* engine)
{
    if (!g_ready) { reinterpret_cast<ShuffleFn>(g_origShuffle8)(first, last, engine); return; }
    const size_t count = size_t(static_cast<unsigned char*>(last) - static_cast<unsigned char*>(first)) / 8;
    Tpf2mpMsvcShuffle(first, count, 8, engine, BoostRaw);
}
void Shuffle4Boost(void* first, void* last, void* engine)
{
    if (!g_ready) { reinterpret_cast<ShuffleFn>(g_origShuffle4)(first, last, engine); return; }
    const size_t count = size_t(static_cast<unsigned char*>(last) - static_cast<unsigned char*>(first)) / 4;
    Tpf2mpMsvcShuffle(first, count, 4, engine, BoostRaw);
}

// ---- TownBuildingTransformator: Windows std::mt19937 sidecar ----------------
// Bound at each native seed store to the exact local engine address; every
// native draw from that address (0x18296a0 is only called by this function,
// always after one of the two stores in the same invocation) is served from
// the sidecar with the MSVC unit-float conversion.
struct EmitSlot { void* engine; Tpf2mpMt19937 mt; };
constexpr unsigned kEmitSlots = 4;
struct EmitSlots { EmitSlot slot[kEmitSlots]; unsigned next; };
thread_local EmitSlots* t_emit = nullptr;
void* g_origEmitUnit = nullptr;

EmitSlot* FindEmit(void* engine)
{
    if (!t_emit) return nullptr;
    for (auto& s : t_emit->slot) if (s.engine == engine) return &s;
    return nullptr;
}
void BindEmit(void* engine, uint32_t seed)
{
    if (!t_emit) {
        t_emit = static_cast<EmitSlots*>(std::calloc(1, sizeof(EmitSlots)));
        if (!t_emit) return;
    }
    EmitSlot* s = FindEmit(engine);
    if (!s) { s = &t_emit->slot[t_emit->next]; t_emit->next = (t_emit->next + 1) % kEmitSlots; }
    s->engine = engine;
    Tpf2mpMtSeed(&s->mt, seed);
}
using UnitFn = float (*)(void*);
float EmitUnit(void* engine)
{
    if (g_ready)
        if (EmitSlot* s = FindEmit(engine)) return Tpf2mpMsvcUnitFloat(Tpf2mpMtNext(&s->mt));
    return reinterpret_cast<UnitFn>(g_origEmitUnit)(engine);
}

// ---- NameRep town names: Windows std::mt19937(seed) + MSVC shuffle ----------
thread_local uint32_t t_nameSeed = 0;
thread_local bool t_nameArmed = false;
void* g_origNameShuffle = nullptr;
using IntShuffleFn = void (*)(int32_t*, int32_t*, void*);
__attribute__((noinline)) void NameShuffle(int32_t* first, int32_t* last, void* engine)
{
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (g_ready && t_nameArmed && caller == g_base + kNameShuffleReturnRva) {
        t_nameArmed = false;
        Tpf2mpMt19937 mt;
        Tpf2mpMtSeed(&mt, t_nameSeed);
        Tpf2mpMsvcShuffle(first, size_t(last - first), 4, &mt, Tpf2mpMtNext);
        return;
    }
    reinterpret_cast<IntShuffleFn>(g_origNameShuffle)(first, last, engine);
}

// ---- register-capture entries (seed stores, name entry) -------------------
struct Registers {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(Registers) == 136);
static_assert(offsetof(Registers, resume) == 128);
void* g_origEmitStoreA = nullptr;
void* g_origEmitStoreB = nullptr;
void* g_origNameEntry = nullptr;
} // namespace

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpEngineParityDispatch(Registers* r)
{
    const unsigned site = unsigned(r->resume);
    if (site == 2) {
        r->resume = reinterpret_cast<uintptr_t>(g_origNameEntry);
        if (g_ready) { t_nameSeed = uint32_t(r->rcx); t_nameArmed = true; }
        return;
    }
    r->resume = reinterpret_cast<uintptr_t>(site == 0 ? g_origEmitStoreA : g_origEmitStoreB);
    if (!g_ready) return;
    // Both stores: id at [r12] (loaded by the verified context), engine at rbp-0x248.
    int32_t id;
    std::memcpy(&id, reinterpret_cast<const void*>(r->r12), sizeof(id));
    BindEmit(reinterpret_cast<void*>(r->rbp - 0x248), uint32_t(id) * 31u + 7u);
}

// Preserve every GP register, the flags and all XMM registers across the
// C++ dispatch, then resume at the trampoline chosen by the dispatch.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpEngineParityEntry()
{
    __asm__(
        "pushfq\n\t"
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %rsi\n\tpush %rdi\n\t"
        "push %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\tpush %rbx\n\t"
        "push %rbp\n\tpush %r12\n\tpush %r13\n\tpush %r14\n\tpush %r15\n\t"
        "mov %rsp, %rbx\n\tand $-16, %rsp\n\tsub $256, %rsp\n\t"
        "movdqu %xmm0, 0(%rsp)\n\tmovdqu %xmm1, 16(%rsp)\n\t"
        "movdqu %xmm2, 32(%rsp)\n\tmovdqu %xmm3, 48(%rsp)\n\t"
        "movdqu %xmm4, 64(%rsp)\n\tmovdqu %xmm5, 80(%rsp)\n\t"
        "movdqu %xmm6, 96(%rsp)\n\tmovdqu %xmm7, 112(%rsp)\n\t"
        "movdqu %xmm8, 128(%rsp)\n\tmovdqu %xmm9, 144(%rsp)\n\t"
        "movdqu %xmm10, 160(%rsp)\n\tmovdqu %xmm11, 176(%rsp)\n\t"
        "movdqu %xmm12, 192(%rsp)\n\tmovdqu %xmm13, 208(%rsp)\n\t"
        "movdqu %xmm14, 224(%rsp)\n\tmovdqu %xmm15, 240(%rsp)\n\t"
        "mov %rbx, %rdi\n\tcall Tpf2mpEngineParityDispatch\n\t"
        "movdqu 0(%rsp), %xmm0\n\tmovdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\tmovdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\tmovdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\tmovdqu 112(%rsp), %xmm7\n\t"
        "movdqu 128(%rsp), %xmm8\n\tmovdqu 144(%rsp), %xmm9\n\t"
        "movdqu 160(%rsp), %xmm10\n\tmovdqu 176(%rsp), %xmm11\n\t"
        "movdqu 192(%rsp), %xmm12\n\tmovdqu 208(%rsp), %xmm13\n\t"
        "movdqu 224(%rsp), %xmm14\n\tmovdqu 240(%rsp), %xmm15\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\tpop %r14\n\tpop %r13\n\tpop %r12\n\tpop %rbp\n\t"
        "pop %rbx\n\tpop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\t"
        "pop %rdi\n\tpop %rsi\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\tpopfq\n\t"
        "lea 8(%rsp), %rsp\n\tjmp *-8(%rsp)\n\t");
}

namespace {
#define PARITY_STUB(index) \
    __attribute__((naked, noinline)) void ParityStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpEngineParityEntry\n\t"); \
    }
PARITY_STUB(0)
PARITY_STUB(1)
PARITY_STUB(2)
#undef PARITY_STUB

struct Hook { uintptr_t rva; void* detour; int stolen; void** original; };
const Hook kHooks[] = {
    {kUniformU64BoostRva, reinterpret_cast<void*>(UniformU64Boost), 17, &g_origU64Boost},
    {kShuffle8BoostRva, reinterpret_cast<void*>(Shuffle8Boost), 16, &g_origShuffle8},
    {kShuffle4BoostRva, reinterpret_cast<void*>(Shuffle4Boost), 16, &g_origShuffle4},
    {kUniformI32StdRva, reinterpret_cast<void*>(UniformI32Std), 17, &g_origI32Std},
    {kUniformU32StdRva, reinterpret_cast<void*>(UniformU32Std), 17, &g_origU32Std},
    {kUniformU64StdRva, reinterpret_cast<void*>(UniformU64Std), 17, &g_origU64Std},
    {kEmitUnitRva, reinterpret_cast<void*>(EmitUnit), 17, &g_origEmitUnit},
    {kEmitStoreARva, reinterpret_cast<void*>(ParityStub0), 7, &g_origEmitStoreA},
    {kEmitStoreBRva, reinterpret_cast<void*>(ParityStub1), 7, &g_origEmitStoreB},
    {kNameEntryRva, reinterpret_cast<void*>(ParityStub2), 15, &g_origNameEntry},
    {kNameShuffleRva, reinterpret_cast<void*>(NameShuffle), 16, &g_origNameShuffle},
};
constexpr unsigned kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

// Plain byte patches: the two clamp branches and the AirConnectParts window.
constexpr unsigned char kNop2[] = {0x66, 0x90};
constexpr unsigned char kAirPatch[] = {
    0x66,0x0f,0xef,0xc0,                         // pxor   xmm0,xmm0
    0xf3,0x48,0x0f,0x2a,0xc1,                    // cvtsi2ss xmm0,rcx      (g, 1..2^31-2)
    0xf3,0x0f,0x5c,0x05,0x63,0xf1,0xfb,0x00,     // subss  xmm0,[rip->0x3e8bdc8] (1.0f)
    0xf3,0x0f,0x59,0x05,0x3f,0xf2,0xfb,0x00,     // mulss  xmm0,[rip->0x3e8beac] (2^-31)
    0x48,0x8d,0x51,0xff,                         // lea    rdx,[rcx-1]    (native leaves g-1)
    0x66,0x0f,0xef,0xd2,                         // pxor   xmm2,xmm2
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,     // nop (8)
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,     // nop (8)
    0x66,0x90,                                   // nop (2) -> falls into *50 at 0x2eccc87
};
static_assert(sizeof(kAirPatch) == kAirResumeRva - kAirPatchRva);
struct BytePatch { uintptr_t rva; const unsigned char* bytes; size_t size; };
const BytePatch kPatches[] = {
    {kUnitStdClampRva, kNop2, sizeof(kNop2)},
    {kDoubleBoostClampRva, kNop2, sizeof(kNop2)},
    {kAirPatchRva, kAirPatch, sizeof(kAirPatch)},
};

const unsigned char* OriginalBytes(uintptr_t rva, size_t size)
{
    for (const auto& g : kGuards)
        if (rva >= g.rva && rva + size <= g.rva + g.size) return g.bytes + (rva - g.rva);
    return nullptr;
}

using Installer = bool (*)(uintptr_t, void*, int, void**);
using Writer = int (*)(uintptr_t, const uint8_t*, size_t, int*);

void Rollback(uintptr_t base, unsigned hooks, unsigned patches, Writer write)
{
    g_activeMask = 0;
    for (unsigned i = 0; i < kHookCount; ++i) {
        if (!(hooks & (1u << i))) continue;
        int error = 0;
        const unsigned char* orig = OriginalBytes(kHooks[i].rva, size_t(kHooks[i].stolen));
        write(base + kHooks[i].rva, orig, size_t(kHooks[i].stolen), &error);
        if (std::memcmp(reinterpret_cast<void*>(base + kHooks[i].rva), orig, size_t(kHooks[i].stolen)) != 0)
            g_activeMask |= 1u << i;
    }
    for (unsigned i = 0; i < sizeof(kPatches) / sizeof(kPatches[0]); ++i) {
        if (!(patches & (1u << i))) continue;
        int error = 0;
        const unsigned char* orig = OriginalBytes(kPatches[i].rva, kPatches[i].size);
        write(base + kPatches[i].rva, orig, kPatches[i].size, &error);
        if (std::memcmp(reinterpret_cast<void*>(base + kPatches[i].rva), orig, kPatches[i].size) != 0)
            g_activeMask |= 1u << (16 + i);
    }
}

bool InstallEngineParityWith(uintptr_t base, const char* buildId, Installer install, Writer write)
{
    if (g_activeMask) {
        std::snprintf(g_status, sizeof(g_status), "unchanged (engine parity active: mask 0x%x)", g_activeMask);
        return false;
    }
    if (const char* kill = std::getenv("TPF2MP_ENGINE_PARITY"))
        if (std::strcmp(kill, "0") == 0) { std::strcpy(g_status, "off (TPF2MP_ENGINE_PARITY=0)"); return false; }
    if (!base || !buildId || std::strcmp(buildId, kParityBuildId) != 0) {
        std::strcpy(g_status, "off (unverified image)"); return false;
    }
    for (const auto& g : kGuards)
        if (std::memcmp(reinterpret_cast<void*>(base + g.rva), g.bytes, g.size) != 0) {
            std::snprintf(g_status, sizeof(g_status), "off (unverified context at 0x%lx)", (unsigned long)g.rva);
            return false;
        }
    if (std::memcmp(reinterpret_cast<void*>(base + kOneRva), &kOneBits, 4) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + kTwoPowMinus31Rva), &kTwoPowMinus31Bits, 4) != 0) {
        std::strcpy(g_status, "off (unverified float constants)"); return false;
    }
    for (const auto& h : kHooks)
        if (!OriginalBytes(h.rva, size_t(h.stolen))) { std::strcpy(g_status, "off (hook outside guards)"); return false; }
    for (const auto& p : kPatches)
        if (!OriginalBytes(p.rva, p.size)) { std::strcpy(g_status, "off (patch outside guards)"); return false; }
    g_base = base;
    g_boostInclusive = reinterpret_cast<InclusiveFn>(base + kInclusiveBoostRva);
    g_stdRaw = reinterpret_cast<RawStdFn>(base + kRawStdRva);
    g_ready = false;
    unsigned hooks = 0, patches = 0;
    for (unsigned i = 0; i < kHookCount; ++i) {
        if (!install(base + kHooks[i].rva, kHooks[i].detour, kHooks[i].stolen, kHooks[i].original)) {
            // A failed writer may already have changed this target.
            Rollback(base, hooks | (1u << i), patches, write);
            std::snprintf(g_status, sizeof(g_status), g_activeMask
                ? "ERROR: engine parity rollback incomplete (mask 0x%x)" : "off (hook %u failed; restored)",
                g_activeMask ? g_activeMask : i);
            return false;
        }
        hooks |= 1u << i;
    }
    for (unsigned i = 0; i < sizeof(kPatches) / sizeof(kPatches[0]); ++i) {
        int error = 0;
        if (write(base + kPatches[i].rva, kPatches[i].bytes, kPatches[i].size, &error) != TPF2MP_CW_OK) {
            Rollback(base, hooks, patches | (1u << i), write);
            std::snprintf(g_status, sizeof(g_status), g_activeMask
                ? "ERROR: engine parity rollback incomplete (mask 0x%x)" : "off (patch %u failed; restored)",
                g_activeMask ? g_activeMask : i);
            return false;
        }
        patches |= 1u << i;
    }
    g_activeMask = hooks | (patches << 16);
    g_ready = true;
    std::strcpy(g_status, "enabled: MSVC uniform_int/shuffle (boost+std MT), unit-float endpoints, "
                          "transformator + town-name std::mt19937, AirConnectParts float");
    return true;
}
} // namespace

bool Tpf2mpInstallEngineParity(uintptr_t imageBase, const char* buildId)
{
    return InstallEngineParityWith(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}

const char* Tpf2mpEngineParityStatus() { return g_status; }
