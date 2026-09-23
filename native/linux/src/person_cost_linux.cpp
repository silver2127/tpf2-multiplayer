#include "person_cost_linux.h"
#include "windows_person_cost_linux.h"
#include "codewrite_linux.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kCostBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
enum class CostKind { Walk, Drive, Line, PathBatch, PathRevision };
enum class CostInput { Rax, Rdx, R8 };
enum class CostAux { None, MagicRdx, Divisor10000 };
enum : unsigned { HashRax = 1, HashRbx = 2, HashR12 = 4, HashR13 = 8, HashRdi = 16 };
struct PersonCostSite {
    uintptr_t contextRva, patchRva, resumeRva;
    CostKind kind;
    CostInput input;
    unsigned outputs;
    CostAux aux;
    const unsigned char* context;
    size_t contextSize;
};
#include "person_cost_sites_linux.h"
constexpr size_t kCostSiteCount = sizeof(kPersonCostSites) / sizeof(kPersonCostSites[0]);
static_assert(kCostSiteCount == 8);

// Order matches the common entry's pushes. The final slot initially holds the
// entry stub's site index; dispatch replaces it with the verified resume PC.
struct CostRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(CostRegisters) == 136);
static_assert(offsetof(CostRegisters, flags) == 120);
static_assert(offsetof(CostRegisters, resume) == 128);
uintptr_t g_costImageBase = 0;
unsigned g_costActiveMask = 0;
char g_costStatus[128] = "off (not initialized)";
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpPersonCostDispatch(CostRegisters* registers)
{
    const auto& site = kPersonCostSites[registers->resume];
    registers->resume = g_costImageBase + site.resumeRva;
    if (site.kind == CostKind::PathBatch) {
        // Replace only the inlined time/batch hash before the original MT seed.
        registers->rdx = Tpf2mpWindowsPathHash(uint32_t(registers->rax), uint32_t(registers->r14));
        registers->rsi = UINT64_C(0x9e3779b9);
        return;
    }
    if (site.kind == CostKind::PathRevision) {
        // The caller already saved the complete Revision. Supply the two
        // FNV inputs to its remaining hash_combine instructions and replay
        // the one stack load inside this guarded window.
        registers->r13 = Tpf2mpWindowsPathHashInput(uint32_t(registers->r13));
        registers->rax = Tpf2mpWindowsPathHashInput(uint32_t(registers->rax));
        std::memcpy(&registers->r9, reinterpret_cast<const void*>(registers->rbp - 0xbe0), sizeof(registers->r9));
        return;
    }
    const uint64_t original = site.input == CostInput::Rax ? registers->rax
        : site.input == CostInput::R8 ? registers->r8 : registers->rdx;
    const uint32_t person = uint32_t(original) - UINT32_C(0x9e3779b9);
    uint32_t line = 0; uint16_t stop = 0;
    if (site.kind == CostKind::Line) {
        // Verified LineSectionData prefix: entity LE32, stop index LE16.
        std::memcpy(&line, reinterpret_cast<const void*>(registers->rsi), sizeof(line));
        std::memcpy(&stop, reinterpret_cast<const void*>(registers->rsi + 4), sizeof(stop));
    }
    const uint64_t hash = site.kind == CostKind::Line
        ? Tpf2mpWindowsLineCostHash(person, line, stop) : site.kind == CostKind::Walk
        ? Tpf2mpWindowsWalkCostHash(person) : Tpf2mpWindowsDriveCostHash(person);
    if (site.outputs & HashRdi) registers->rdi = hash;
    if (site.outputs & HashRax) registers->rax = hash;
    if (site.outputs & HashRbx) registers->rbx = hash;
    if (site.outputs & HashR12) registers->r12 = hash;
    if (site.outputs & HashR13) registers->r13 = hash;
    if (site.aux == CostAux::MagicRdx) registers->rdx = UINT64_C(0x346dc5d63886594b);
    else if (site.aux == CostAux::Divisor10000) { registers->rdx = 0; registers->rsi = 10000; }
    registers->resume = g_costImageBase + site.resumeRva;
}

// These sites interrupt functions with live integer and vector registers, and
// one has an extra outgoing stack argument. Save all 15 non-RSP GP registers,
// flags and all 16 XMM registers, then align dynamically for the C++ call.
// No helper uses floating arithmetic; the original modulo/SSE resumes below.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpPersonCostEntry()
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
        "mov %rbx, %rdi\n\tcall Tpf2mpPersonCostDispatch\n\t"
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
#define COST_STUB(index) \
    __attribute__((naked, noinline)) void CostStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpPersonCostEntry\n\t"); \
    }
COST_STUB(0)
COST_STUB(1)
COST_STUB(2)
COST_STUB(3)
COST_STUB(4)
COST_STUB(5)
COST_STUB(6)
COST_STUB(7)
#undef COST_STUB
void (*const kCostStubs[])() = {CostStub0, CostStub1, CostStub2, CostStub3, CostStub4, CostStub5, CostStub6, CostStub7};

using CostWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);
const unsigned char* OriginalPatch(const PersonCostSite& site)
{
    return site.context + (site.patchRva - site.contextRva);
}

// Writer is supplied only to permit isolated failure-path tests. Production
// always uses the common constructor-stage /proc/self/mem code writer.
bool InstallPersonCosts(uintptr_t imageBase, const char* buildId, CostWriter write)
{
    if (g_costActiveMask) {
        std::snprintf(g_costStatus, sizeof(g_costStatus), "unchanged (cost windows already active: mask 0x%x)", g_costActiveMask);
        return false;
    }
    if (!imageBase || !buildId || std::strcmp(buildId, kCostBuildId) != 0) {
        std::strcpy(g_costStatus, "off (unverified image)"); return false;
    }
    // Check every prefix first: the later drive branch's prefix includes an
    // earlier drive patch site. No partially checked prefix may be patched.
    for (const auto& site : kPersonCostSites) {
        const size_t n = site.resumeRva - site.patchRva;
        if (site.contextRva > site.patchRva || site.contextRva + site.contextSize != site.resumeRva ||
            n < 14 || n > 64 ||
            std::memcmp(reinterpret_cast<void*>(imageBase + site.contextRva), site.context, site.contextSize) != 0) {
            std::strcpy(g_costStatus, "off (unverified cost window)"); return false;
        }
    }
    g_costImageBase = imageBase;
    for (size_t i = 0; i < kCostSiteCount; ++i) {
        const auto& site = kPersonCostSites[i];
        const size_t n = site.resumeRva - site.patchRva;
        unsigned char patch[64]; std::memset(patch, 0x90, n);
        const unsigned char jump[] = {0xff,0x25,0,0,0,0};
        std::memcpy(patch, jump, sizeof(jump));
        const uintptr_t target = reinterpret_cast<uintptr_t>(kCostStubs[i]);
        std::memcpy(patch + 6, &target, sizeof(target));
        int error = 0;
        if (write(imageBase + site.patchRva, patch, n, &error) == TPF2MP_CW_OK) {
            g_costActiveMask |= 1u << i; continue;
        }
        // No game thread can enter these functions yet. Restore all attempted
        // windows, including the failing one, and inspect the actual bytes.
        g_costActiveMask = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& attempted = kPersonCostSites[j];
            const size_t length = attempted.resumeRva - attempted.patchRva;
            int restoreError = 0;
            write(imageBase + attempted.patchRva, OriginalPatch(attempted), length, &restoreError);
            if (std::memcmp(reinterpret_cast<void*>(imageBase + attempted.patchRva), OriginalPatch(attempted), length) != 0)
                g_costActiveMask |= 1u << j;
        }
        if (g_costActiveMask)
            std::snprintf(g_costStatus, sizeof(g_costStatus), "ERROR: installation/rollback failed; unrestored cost-window mask 0x%x", g_costActiveMask);
        else std::strcpy(g_costStatus, "off (installation failed; all attempted windows restored)");
        return false;
    }
    std::strcpy(g_costStatus, "enabled at 8 verified windows");
    return true;
}
}

bool Tpf2mpInstallPersonCosts(uintptr_t imageBase, const char* buildId)
{
    return InstallPersonCosts(imageBase, buildId, Tpf2mpCodeWriteSelf);
}

const char* Tpf2mpPersonCostStatus() { return g_costStatus; }
