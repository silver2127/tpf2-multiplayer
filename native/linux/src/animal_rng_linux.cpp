#include "animal_rng_linux.h"
#include "windows_person_seed_linux.h"
#include "windows_uniform_int_linux.h"
#include "codewrite_linux.h"
#include "hook.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <type_traits>
#include <dlfcn.h>

extern "C" {
__attribute__((visibility("hidden"))) void* Tpf2mpAnimalPersonality = nullptr;
__attribute__((visibility("hidden"))) void* Tpf2mpAnimalUnwindResume = nullptr;
__attribute__((visibility("hidden"))) void Tpf2mpAnimalInvoke(
    void (*invoke)(void*), void* context, void (*cleanup)(void*));
}

// Cleanup-only frame using the game's dynamic exception runtime. No exception
// is caught or translated: restore the TLS scope, then resume the same unwind.
// This also handles forced unwinds. C++ wrappers around it have trivial locals
// and no LSDA, avoiding a static libgcc personality on a foreign context.
__asm__(R"ASM(
    .text
    .p2align 4
    .type Tpf2mpAnimalInvoke,@function
Tpf2mpAnimalInvoke:
.Lanimal_fb:
    .cfi_startproc
    .cfi_personality 0x9b,Tpf2mpAnimalPersonality
    .cfi_lsda 0x1b,.Lanimal_lsda
    endbr64
    pushq %rbx
    .cfi_def_cfa_offset 16
    .cfi_offset 3,-16
    pushq %r12
    .cfi_def_cfa_offset 24
    .cfi_offset 12,-24
    pushq %r13
    .cfi_def_cfa_offset 32
    .cfi_offset 13,-32
    movq %rdi,%rax
    movq %rsi,%rbx
    movq %rdx,%r12
    movq %rbx,%rdi
.Lanimal_call_b:
    call *%rax
.Lanimal_call_e:
    movq %rbx,%rdi
    call *%r12
    .cfi_remember_state
    popq %r13
    .cfi_restore 13
    .cfi_def_cfa_offset 24
    popq %r12
    .cfi_restore 12
    .cfi_def_cfa_offset 16
    popq %rbx
    .cfi_restore 3
    .cfi_def_cfa_offset 8
    ret
.Lanimal_pad:
    .cfi_restore_state
    endbr64
    movq %rax,%r13
    movq %rbx,%rdi
    call *%r12
    movq %r13,%rdi
    call *Tpf2mpAnimalUnwindResume(%rip)
    ud2
.Lanimal_pad_e:
    .cfi_endproc
    .size Tpf2mpAnimalInvoke,.-Tpf2mpAnimalInvoke
    .section .gcc_except_table,"a",@progbits
    .p2align 2
.Lanimal_lsda:
    .byte 0xff
    .byte 0xff
    .byte 0x1
    .uleb128 .Lanimal_cse-.Lanimal_csb
.Lanimal_csb:
    .uleb128 .Lanimal_call_b-.Lanimal_fb
    .uleb128 .Lanimal_call_e-.Lanimal_call_b
    .uleb128 .Lanimal_pad-.Lanimal_fb
    .uleb128 0
    .uleb128 .Lanimal_call_e-.Lanimal_fb
    .uleb128 .Lanimal_pad_e-.Lanimal_call_e
    .uleb128 0
    .uleb128 0
.Lanimal_cse:
    .text
)ASM");

namespace {
constexpr char kAnimalBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
#include "animal_rng_sites_linux.h"
enum class AnimalKind { Worker, Spawn };
struct AnimalScope;
thread_local AnimalScope* g_animalScope = nullptr;
struct AnimalScope {
    AnimalScope* previous;
    AnimalKind kind;
    uint32_t seed;
    void* nativeEngine = nullptr;
    std::mt19937 engine;
    AnimalScope(AnimalKind kind_, uint32_t seed_)
        : previous(g_animalScope), kind(kind_), seed(seed_), engine(seed_) { g_animalScope = this; }
    AnimalScope(const AnimalScope&) = delete;
    AnimalScope& operator=(const AnimalScope&) = delete;
};
static_assert(std::is_trivially_destructible<AnimalScope>::value,
              "game frames must not acquire static-runtime C++ cleanups");
void FinishAnimalScope(AnimalScope* scope) { g_animalScope = scope->previous; }

bool ResolveAnimalRuntime()
{
    Dl_info self{}, personalityInfo{}, unwindInfo{};
    void* personality = dlsym(RTLD_DEFAULT, "__gxx_personality_v0");
    void* resume = dlsym(RTLD_DEFAULT, "_Unwind_Resume");
    if (!personality || !resume || !dladdr(reinterpret_cast<void*>(ResolveAnimalRuntime), &self) ||
        !dladdr(personality, &personalityInfo) || !dladdr(resume, &unwindInfo) ||
        self.dli_fbase == personalityInfo.dli_fbase || self.dli_fbase == unwindInfo.dli_fbase) return false;
    Tpf2mpAnimalPersonality = personality;
    Tpf2mpAnimalUnwindResume = resume;
    return true;
}

// The Windows std-MT helper draws one uint32 and converts it to a unit float.
// Keep the zero subtraction and addition explicit, including signed zero in
// directed rounding modes. Volatile intermediates also prevent reassociation.
float WindowsAnimalUnit(std::mt19937& engine)
{
    const uint32_t raw = uint32_t(engine());
    volatile float converted = float(uint64_t(raw));
    volatile float zero = 0.0f;
    volatile float distance = converted - zero;
    volatile float accumulated = zero + distance;
    return accumulated / 0x1p32f;
}
int32_t WindowsAnimalInt(std::mt19937& engine, int32_t minimum, int32_t maximum)
{
    return Tpf2mpWindowsUniformIntInclusive(minimum, maximum, &engine, [](void* opaque) {
        return uint32_t((*static_cast<std::mt19937*>(opaque))());
    });
}

AnimalScope* FindAnimalScope(void* engine)
{
    if (!engine) return nullptr;
    for (AnimalScope* scope = g_animalScope; scope; scope = scope->previous)
        if (scope->nativeEngine == engine) return scope;
    return nullptr;
}

using WorkerFn = void* (*)(void*, const void*, int32_t, int32_t);
using SpawnFn = void (*)(void*, void*, void*, void*, void*, void*, void*, int32_t);
using UnitFn = float (*)(void*);
using IntegerFn = int32_t (*)(void*, void*, const int32_t*);
void* g_originalWorker = nullptr;
void* g_originalSpawn = nullptr;
void* g_originalUnit = nullptr;
void* g_originalInteger = nullptr;
void* g_originalWorkerBind = nullptr;
void* g_originalSpawnBind = nullptr;
void* g_originalWorkerDraw = nullptr;
bool g_animalReady = false;
unsigned g_animalActiveMask = 0;
uintptr_t g_animalImageBase = 0;
char g_animalStatus[160] = "off (not initialized)";

void* AnimalWorker(void* result, const void* capture, int32_t begin, int32_t end)
{
    const auto original = reinterpret_cast<WorkerFn>(g_originalWorker);
    if (!g_animalReady) return original(result, capture, begin, end);
    uint32_t time;
    std::memcpy(&time, static_cast<const unsigned char*>(capture) + 0x28, 4);
    AnimalScope scope(AnimalKind::Worker, Tpf2mpWindowsTimeSeed(uint32_t(begin), time));
    struct Call { AnimalScope* scope; WorkerFn function; void* result; const void* capture; int32_t begin, end; void* returned; };
    Call call{&scope, original, result, capture, begin, end, nullptr};
    static_assert(std::is_trivially_destructible<Call>::value, "no foreign-frame cleanup");
    Tpf2mpAnimalInvoke([](void* opaque) {
        auto* call = static_cast<Call*>(opaque);
        call->returned = call->function(call->result, call->capture, call->begin, call->end);
    }, &call, [](void* opaque) { FinishAnimalScope(static_cast<Call*>(opaque)->scope); });
    return call.returned;
}

void AnimalSpawn(void* a, void* b, void* c, void* d, void* e, void* f,
                 void* callback, int32_t seed)
{
    const auto original = reinterpret_cast<SpawnFn>(g_originalSpawn);
    if (!g_animalReady) { original(a, b, c, d, e, f, callback, seed); return; }
    AnimalScope scope(AnimalKind::Spawn, uint32_t(seed));
    struct Call { AnimalScope* scope; SpawnFn function; void *a, *b, *c, *d, *e, *f, *callback; int32_t seed; };
    Call call{&scope, original, a, b, c, d, e, f, callback, seed};
    static_assert(std::is_trivially_destructible<Call>::value, "no foreign-frame cleanup");
    Tpf2mpAnimalInvoke([](void* opaque) {
        auto* call = static_cast<Call*>(opaque);
        call->function(call->a, call->b, call->c, call->d, call->e, call->f, call->callback, call->seed);
    }, &call, [](void* opaque) { FinishAnimalScope(static_cast<Call*>(opaque)->scope); });
}

float AnimalUnit(void* engine)
{
    if (g_animalReady)
        if (AnimalScope* scope = FindAnimalScope(engine)) return WindowsAnimalUnit(scope->engine);
    return reinterpret_cast<UnitFn>(g_originalUnit)(engine);
}
__attribute__((noinline))
int32_t AnimalInteger(void* distribution, void* engine, const int32_t* parameters)
{
    // The original distribution recurses for wide ranges. Once a request has
    // fallen back to native behavior, keep its recursive subrequests native
    // too, including an invalid range delegated to the original function.
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (g_animalReady && caller != g_animalImageBase + 0xd6f9c8)
        if (AnimalScope* scope = FindAnimalScope(engine))
            if (parameters[0] <= parameters[1])
                return WindowsAnimalInt(scope->engine, parameters[0], parameters[1]);
    return reinterpret_cast<IntegerFn>(g_originalInteger)(distribution, engine, parameters);
}

struct AnimalRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(AnimalRegisters) == 136);
static_assert(offsetof(AnimalRegisters, rbp) == 32);
static_assert(offsetof(AnimalRegisters, resume) == 128);
}

// The binding patches replay the original native seed stores. A sidecar lives
// only inside the corresponding wrapper invocation, and is bound to the exact
// local engine address after every native initialization. Nested callbacks and
// worker threads have independent scopes; later reuse of a stack address cannot
// select a completed invocation's MT state.
extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpAnimalDispatch(AnimalRegisters* registers, unsigned char* xmm)
{
    const unsigned site = unsigned(registers->resume);
    if (site < 2) {
        const bool worker = site == 0;
        registers->resume = reinterpret_cast<uintptr_t>(worker ? g_originalWorkerBind : g_originalSpawnBind);
        if (g_animalReady && g_animalScope &&
            g_animalScope->kind == (worker ? AnimalKind::Worker : AnimalKind::Spawn)) {
            g_animalScope->nativeEngine = reinterpret_cast<void*>(registers->rbp - (worker ? 0xf0 : 0x168));
            g_animalScope->engine.seed(g_animalScope->seed);
        }
        return;
    }
    registers->resume = reinterpret_cast<uintptr_t>(g_originalWorkerDraw);
    if (g_animalReady) {
        void* engine = reinterpret_cast<void*>(registers->rbp - 0xf0);
        AnimalScope* scope = FindAnimalScope(engine);
        if (scope && scope->kind == AnimalKind::Worker) {
            const float value = WindowsAnimalUnit(scope->engine);
            std::memset(xmm, 0, 16);
            std::memcpy(xmm, &value, 4);
            registers->resume = g_animalImageBase + 0x167501c;
        }
    }
}

// Mid-function entry: preserve every GP register, arithmetic flags and all XMM
// registers. The draw changes XMM0 only; the immediate resumed comparison
// overwrites the old arithmetic flags before consuming them. Native temporary
// LCG quotient/remainder registers have no live uses at that continuation.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpAnimalEntry()
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
        "mov %rbx, %rdi\n\tmov %rsp, %rsi\n\tcall Tpf2mpAnimalDispatch\n\t"
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
#define ANIMAL_STUB(index) \
    __attribute__((naked, noinline)) void AnimalStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpAnimalEntry\n\t"); \
    }
ANIMAL_STUB(0)
ANIMAL_STUB(1)
ANIMAL_STUB(2)
#undef ANIMAL_STUB

struct AnimalGuard { uintptr_t rva; const unsigned char* bytes; size_t size; };
const AnimalGuard kAnimalGuards[] = {
    {kWorkerPrefixRva, kWorkerPrefixBytes, sizeof(kWorkerPrefixBytes)},
    {kWorkerDrawRva, kWorkerDrawBytes, sizeof(kWorkerDrawBytes)},
    {kSpawnPrefixRva, kSpawnPrefixBytes, sizeof(kSpawnPrefixBytes)},
    {kUnitCoreRva, kUnitCoreBytes, sizeof(kUnitCoreBytes)},
    {kRandomTargetRva, kRandomTargetBytes, sizeof(kRandomTargetBytes)},
    {kGetPositionRva, kGetPositionBytes, sizeof(kGetPositionBytes)},
    {kIntegerCoreRva, kIntegerCoreBytes, sizeof(kIntegerCoreBytes)},
};
constexpr uintptr_t kAnimalLcgScaleRva = 0x3e8beac;
constexpr uintptr_t kAnimalOneRva = 0x3e8bdc8;
constexpr uint32_t kAnimalLcgScale = 0x30000000u;
constexpr uint32_t kAnimalOne = 0x3f800000u;
struct AnimalHook { uintptr_t rva; void* detour; int stolen; void** original; };
const AnimalHook kAnimalHooks[] = {
    {0x1674c60, reinterpret_cast<void*>(AnimalWorker), 15, &g_originalWorker},
    {0x178c620, reinterpret_cast<void*>(AnimalSpawn), 15, &g_originalSpawn},
    {0x1786a10, reinterpret_cast<void*>(AnimalUnit), 17, &g_originalUnit},
    {0x1674d5d, reinterpret_cast<void*>(AnimalStub0), 7, &g_originalWorkerBind},
    {0x178c79e, reinterpret_cast<void*>(AnimalStub1), 7, &g_originalSpawnBind},
    {0x1674faa, reinterpret_cast<void*>(AnimalStub2), 21, &g_originalWorkerDraw},
    {0xd6f8a0, reinterpret_cast<void*>(AnimalInteger), 15, &g_originalInteger},
};
constexpr unsigned kAnimalHookCount = sizeof(kAnimalHooks) / sizeof(kAnimalHooks[0]);
static_assert(kAnimalHookCount == 7);
using AnimalHookInstaller = bool (*)(uintptr_t, void*, int, void**);
using AnimalWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);

const unsigned char* AnimalOriginalBytes(const AnimalHook& hook)
{
    for (const auto& guard : kAnimalGuards)
        if (hook.rva >= guard.rva && hook.rva + hook.stolen <= guard.rva + guard.size)
            return guard.bytes + hook.rva - guard.rva;
    return nullptr;
}

bool InstallAnimalRng(uintptr_t base, const char* buildId,
                      AnimalHookInstaller install, AnimalWriter write)
{
    if (g_animalActiveMask) {
        std::snprintf(g_animalStatus, sizeof(g_animalStatus), "unchanged (animal hooks active: mask 0x%x)", g_animalActiveMask);
        return false;
    }
    if (!base || !buildId || std::strcmp(buildId, kAnimalBuildId) != 0) {
        std::strcpy(g_animalStatus, "off (unverified image)"); return false;
    }
    // Check all contexts before any overlapping prefix is modified. The two
    // utility guards establish the four shared float draws and position integer
    // draw in addition to the worker's inline float and two integer draws.
    // Hooks never reinterpret or replace an unbound LCG engine.
    for (const auto& guard : kAnimalGuards)
        if (std::memcmp(reinterpret_cast<void*>(base + guard.rva), guard.bytes, guard.size) != 0) {
            std::strcpy(g_animalStatus, "off (unverified animal RNG context)"); return false;
        }
    if (std::memcmp(reinterpret_cast<void*>(base + kAnimalLcgScaleRva), &kAnimalLcgScale, 4) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + kAnimalOneRva), &kAnimalOne, 4) != 0) {
        std::strcpy(g_animalStatus, "off (unverified animal float constants)"); return false;
    }
    for (const auto& hook : kAnimalHooks)
        if (!AnimalOriginalBytes(hook)) {
            std::strcpy(g_animalStatus, "off (invalid animal hook coverage)"); return false;
        }
    if (!ResolveAnimalRuntime()) {
        std::strcpy(g_animalStatus, "off (game exception runtime unavailable)"); return false;
    }
    g_animalImageBase = base;
    g_animalReady = false;
    for (unsigned i = 0; i < kAnimalHookCount; ++i) {
        const auto& hook = kAnimalHooks[i];
        if (install(base + hook.rva, hook.detour, hook.stolen, hook.original)) {
            g_animalActiveMask |= 1u << i; continue;
        }
        g_animalActiveMask = 0;
        for (unsigned j = 0; j <= i; ++j) {
            const auto& attempted = kAnimalHooks[j];
            int error = 0;
            write(base + attempted.rva, AnimalOriginalBytes(attempted), attempted.stolen, &error);
            if (std::memcmp(reinterpret_cast<void*>(base + attempted.rva), AnimalOriginalBytes(attempted), attempted.stolen) != 0)
                g_animalActiveMask |= 1u << j;
        }
        if (g_animalActiveMask)
            std::snprintf(g_animalStatus, sizeof(g_animalStatus), "ERROR: animal hook rollback incomplete (mask 0x%x); overrides disabled", g_animalActiveMask);
        else std::strcpy(g_animalStatus, "off (installation failed; all attempted windows restored)");
        return false;
    }
    g_animalReady = true;
    std::strcpy(g_animalStatus, "enabled: scoped spawn/movement MT seeds, float and integer draws");
    return true;
}
}

bool Tpf2mpInstallAnimalRng(uintptr_t imageBase, const char* buildId)
{
    return InstallAnimalRng(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}

const char* Tpf2mpAnimalRngStatus() { return g_animalStatus; }
