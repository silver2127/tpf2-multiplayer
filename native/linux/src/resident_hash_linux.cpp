#include "resident_hash_linux.h"
#include "codewrite_linux.h"
#include "hook.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <type_traits>

extern "C" {
__attribute__((visibility("hidden"))) void* Tpf2mpResidentPersonality = nullptr;
__attribute__((visibility("hidden"))) void* Tpf2mpResidentUnwindResume = nullptr;
__attribute__((visibility("hidden"))) void Tpf2mpResidentInvoke(
    void (*invoke)(void*), void* context, void (*cleanup)(void*));
}

// Cleanup-only frame using the game's dynamic exception runtime. No exception
// is caught or translated: restore the TLS scope, then resume the same unwind.
// This also handles forced unwinds. C++ wrappers around it have trivial locals
// and no LSDA, avoiding a static libgcc personality on a foreign context.
__asm__(R"ASM(
    .text
    .p2align 4
    .type Tpf2mpResidentInvoke,@function
Tpf2mpResidentInvoke:
.Lresident_fb:
    .cfi_startproc
    .cfi_personality 0x9b,Tpf2mpResidentPersonality
    .cfi_lsda 0x1b,.Lresident_lsda
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
.Lresident_call_b:
    call *%rax
.Lresident_call_e:
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
.Lresident_pad:
    .cfi_restore_state
    endbr64
    movq %rax,%r13
    movq %rbx,%rdi
    call *%r12
    movq %r13,%rdi
    call *Tpf2mpResidentUnwindResume(%rip)
    ud2
.Lresident_pad_e:
    .cfi_endproc
    .size Tpf2mpResidentInvoke,.-Tpf2mpResidentInvoke
    .section .gcc_except_table,"a",@progbits
    .p2align 2
.Lresident_lsda:
    .byte 0xff
    .byte 0xff
    .byte 0x1
    .uleb128 .Lresident_cse-.Lresident_csb
.Lresident_csb:
    .uleb128 .Lresident_call_b-.Lresident_fb
    .uleb128 .Lresident_call_e-.Lresident_call_b
    .uleb128 .Lresident_pad-.Lresident_fb
    .uleb128 0
    .uleb128 .Lresident_call_e-.Lresident_fb
    .uleb128 .Lresident_pad_e-.Lresident_call_e
    .uleb128 0
    .uleb128 0
.Lresident_cse:
    .text
)ASM");


uint64_t Tpf2mpWindowsResidentHash(uint32_t id) noexcept
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned byte = 0; byte != 4; ++byte) {
        hash = (hash ^ uint8_t(id)) * UINT64_C(0x100000001b3);
        id >>= 8;
    }
    const unsigned __int128 product = static_cast<unsigned __int128>(hash)
        * UINT64_C(0xde5fb9d2630458e9);
    return uint64_t(product) + uint64_t(product >> 64);
}

namespace {
constexpr char kResidentBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
constexpr uintptr_t kResidentInsertReturn = 0x17171d9;
struct ResidentContext { uintptr_t rva; const unsigned char* code; size_t size; };
struct ResidentSite { unsigned context; uintptr_t rva; int size; };
#include "resident_hash_sites_linux.h"
constexpr size_t kResidentSiteCount = sizeof(kResidentSites) / sizeof(kResidentSites[0]);
static_assert(kResidentSiteCount == 6);
struct ResidentRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(ResidentRegisters) == 136);
static_assert(offsetof(ResidentRegisters, resume) == 128);
void* g_residentOriginal[kResidentSiteCount]{};
uintptr_t g_residentImageBase = 0;
unsigned g_residentActiveMask = 0;
bool g_residentReady = false;
char g_residentStatus[160] = "off (not initialized)";
struct ResidentScope { ResidentScope* previous; void* set; };
thread_local ResidentScope* g_residentScope = nullptr;
static_assert(std::is_trivially_destructible<ResidentScope>::value,
              "game frames must not acquire static-runtime C++ cleanups");
bool IsResidentSet(uint64_t set)
{
    for (auto* scope = g_residentScope; scope; scope = scope->previous)
        if (reinterpret_cast<uintptr_t>(scope->set) == set) return true;
    return false;
}
bool ResolveResidentRuntime()
{
    Dl_info self{}, personalityInfo{}, unwindInfo{};
    void* personality = dlsym(RTLD_DEFAULT, "__gxx_personality_v0");
    void* resume = dlsym(RTLD_DEFAULT, "_Unwind_Resume");
    if (!personality || !resume || !dladdr(reinterpret_cast<void*>(ResolveResidentRuntime), &self) ||
        !dladdr(personality, &personalityInfo) || !dladdr(resume, &unwindInfo) ||
        self.dli_fbase == personalityInfo.dli_fbase || self.dli_fbase == unwindInfo.dli_fbase) return false;
    Tpf2mpResidentPersonality = personality;
    Tpf2mpResidentUnwindResume = resume;
    return true;
}
using ResidentPrepare = size_t (*)(void*, uint64_t);
size_t ResidentScopedPrepare(void* set, uint64_t hash, ResidentPrepare original)
{
    ResidentScope scope{g_residentScope, set};
    g_residentScope = &scope;
    struct Call { ResidentScope* scope; ResidentPrepare function; void* set; uint64_t hash; size_t result; };
    Call call{&scope, original, set, hash, 0};
    static_assert(std::is_trivially_destructible<Call>::value, "no foreign-frame cleanup");
    Tpf2mpResidentInvoke([](void* opaque) {
        auto* call = static_cast<Call*>(opaque);
        call->result = call->function(call->set, call->hash);
    }, &call, [](void* opaque) {
        g_residentScope = static_cast<Call*>(opaque)->scope->previous;
    });
    return call.result;
}
__attribute__((noinline)) size_t ResidentPrepareInsert(void* set, uint64_t hash)
{
    const auto original = reinterpret_cast<ResidentPrepare>(g_residentOriginal[5]);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!g_residentReady || caller != g_residentImageBase + kResidentInsertReturn)
        return original(set, hash);
    return ResidentScopedPrepare(set, hash, original);
}
}

// Only shared rehash helpers need a pointer gate. Native engine insertion and
// erase contain the other three regions, whose surrounding full functions are
// pinned below. The outer destination map retains its native hash everywhere.
extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpResidentHashDispatch(ResidentRegisters* r) noexcept
{
    const size_t index = r->resume;
    r->resume = reinterpret_cast<uintptr_t>(g_residentOriginal[index]);
    if (!g_residentReady) return;
    if (index == 3 && !IsResidentSet(r->r12)) return;
    if (index == 4 && !IsResidentSet(r->r13)) return;
    uint32_t id = uint32_t(r->r9);
    if (index == 3) std::memcpy(&id, reinterpret_cast<void*>(r->r14), sizeof(id));
    else if (index == 4) {
        uintptr_t slots;
        std::memcpy(&slots, reinterpret_cast<void*>(r->r13 + 8), sizeof(slots));
        std::memcpy(&id, reinterpret_cast<void*>(slots + r->rbx * 4), sizeof(id));
    }
    const uint64_t hash = Tpf2mpWindowsResidentHash(id);
    switch (index) {
    case 0: r->rax = hash; r->rdx = r->r8 + 1; r->rcx = hash >> 7; break;
    case 1: r->rax = hash; r->rcx = hash >> 7; break;
    case 2:
        r->rax = hash;
        std::memcpy(&r->rdx, reinterpret_cast<void*>(r->rcx + 0x20), sizeof(r->rdx));
        r->rdi = hash; r->r10 = r->rdx + 1; break;
    case 3: r->r8 = hash; break;
    case 4: r->r15 = hash; r->rsi = hash; break;
    }
    r->resume = g_residentImageBase + kResidentSites[index].rva + kResidentSites[index].size;
}

// Integer-only dispatch cannot change MXCSR. Preserve all GP/flags/XMM state;
// the original continuation overwrites arithmetic flags before reading them.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpResidentHashEntry()
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
        "mov %rbx, %rdi\n\tcall Tpf2mpResidentHashDispatch\n\t"
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
        // Restore the interrupted RSP without changing flags or a register.
        // Jump rather than synthesizing a return/shadow-stack entry.
        "lea 8(%rsp), %rsp\n\tjmp *-8(%rsp)\n\t");
}

namespace {
#define RESIDENT_STUB(index) \
    __attribute__((naked, noinline)) void ResidentStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpResidentHashEntry\n\t"); \
    }
RESIDENT_STUB(0)
RESIDENT_STUB(1)
RESIDENT_STUB(2)
RESIDENT_STUB(3)
RESIDENT_STUB(4)
#undef RESIDENT_STUB
void* const kResidentDetours[] = {
    reinterpret_cast<void*>(ResidentStub0), reinterpret_cast<void*>(ResidentStub1),
    reinterpret_cast<void*>(ResidentStub2), reinterpret_cast<void*>(ResidentStub3),
    reinterpret_cast<void*>(ResidentStub4), reinterpret_cast<void*>(ResidentPrepareInsert)
};
using ResidentInstaller = bool (*)(uintptr_t, void*, int, void**);
using ResidentWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);
const unsigned char* ResidentOriginalBytes(const ResidentSite& site)
{
    const auto& context = kResidentContexts[site.context];
    return context.code + site.rva - context.rva;
}
bool InstallResidentHash(uintptr_t base, const char* buildId, ResidentInstaller install,
                         ResidentWriter write)
{
    if (g_residentActiveMask) {
        std::snprintf(g_residentStatus, sizeof(g_residentStatus),
                      "unchanged (resident windows already active: mask 0x%x)", g_residentActiveMask);
        return false;
    }
    if (!base || !buildId || std::strcmp(buildId, kResidentBuildId)) {
        std::strcpy(g_residentStatus, "off (unverified image)"); return false;
    }
    // Both insertion sites and the gated call share one full context. Verify
    // every function before changing any byte, including all resize/drop code.
    for (const auto& context : kResidentContexts)
        if (std::memcmp(reinterpret_cast<void*>(base + context.rva), context.code, context.size)) {
            std::strcpy(g_residentStatus, "off (unverified resident hash/context)"); return false;
        }
    if (!ResolveResidentRuntime()) {
        std::strcpy(g_residentStatus, "off (game exception runtime unavailable)"); return false;
    }
    g_residentImageBase = base;
    for (size_t i = 0; i != kResidentSiteCount; ++i) {
        const auto& site = kResidentSites[i];
        if (install(base + site.rva, kResidentDetours[i], site.size, &g_residentOriginal[i])) {
            g_residentActiveMask |= 1u << i; continue;
        }
        g_residentReady = false;
        g_residentActiveMask = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& attempted = kResidentSites[j];
            int error = 0;
            write(base + attempted.rva, ResidentOriginalBytes(attempted), attempted.size, &error);
            if (std::memcmp(reinterpret_cast<void*>(base + attempted.rva), ResidentOriginalBytes(attempted), attempted.size))
                g_residentActiveMask |= 1u << j;
        }
        if (g_residentActiveMask)
            std::snprintf(g_residentStatus, sizeof(g_residentStatus),
                "ERROR: installation/rollback failed; unrestored resident mask 0x%x", g_residentActiveMask);
        else std::strcpy(g_residentStatus, "off (installation failed; original resident code restored)");
        return false;
    }
    g_residentReady = true;
    std::strcpy(g_residentStatus, "enabled at 5 inner-hash sites and 1 gated insertion scope");
    return true;
}
}

bool Tpf2mpInstallResidentHash(uintptr_t imageBase, const char* buildId)
{
    return InstallResidentHash(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}
const char* Tpf2mpResidentHashStatus() { return g_residentStatus; }
