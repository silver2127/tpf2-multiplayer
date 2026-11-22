#include "sim_seed_linux.h"
#include "windows_person_seed_linux.h"
#include "codewrite_linux.h"
#include "hook.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char kSimSeedBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
using SeedKind = Tpf2mpSimSeedKind;
// Where the verified native code keeps the entity id at the hook.
enum class EntitySource : uint8_t { None, R13, R14, RbpMinusB48 };
struct SimSeedSite {
    uintptr_t contextRva, hookRva;
    int steal;
    SeedKind kind;
    uint32_t tag;
    EntitySource entity;
    const unsigned char* context;
    size_t contextSize;
    const char* name;
};
#include "sim_seed_sites_linux.h"
constexpr size_t kSimSeedCount = sizeof(kSimSeedSites) / sizeof(kSimSeedSites[0]);
static_assert(kSimSeedCount == 8);

constexpr uint64_t FnvInt(uint32_t value)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}
// Native time-only seed: low32((sext(time) + off) ^ first), first = k + tag,
// off = k + (first << 6) + (first >> 2). Both steps invert modulo 2^32.
constexpr uint32_t NativeTimeFromSeed(uint32_t tag, uint32_t nativeSeed)
{
    const uint64_t first = UINT64_C(0x9e3779b9) + tag;
    const uint64_t offset = UINT64_C(0x9e3779b9) + (first << 6) + (first >> 2);
    return (nativeSeed ^ uint32_t(first)) - uint32_t(offset);
}

// Order matches the common entry's pushes; `resume` initially holds the stub's
// site index and is replaced with that site's trampoline.
struct SeedRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(SeedRegisters) == 136);
static_assert(offsetof(SeedRegisters, resume) == 128);

void* g_trampolines[kSimSeedCount] = {};
unsigned g_activeMask = 0;
char g_status[160] = "off (not initialized)";

uint32_t ReadEntity(EntitySource source, const SeedRegisters& r)
{
    uintptr_t address = 0;
    switch (source) {
    case EntitySource::R13: address = r.r13; break;
    case EntitySource::R14: address = r.r14; break;
    case EntitySource::RbpMinusB48:
        std::memcpy(&address, reinterpret_cast<const void*>(r.rbp - 0xb48), sizeof(address));
        break;
    case EntitySource::None: return 0;
    }
    uint32_t entity;
    std::memcpy(&entity, reinterpret_cast<const void*>(address), sizeof(entity));
    return entity;
}
} // namespace

void Tpf2mpSimSeedTransform(Tpf2mpSimSeedKind kind, uint32_t tag, uint32_t entity,
                            Tpf2mpSimSeedRegs* regs)
{
    constexpr uint64_t kHigh = UINT64_C(0xffffffff00000000);
    switch (kind) {
    case SeedKind::FnvRax:
        // Replayed stores take eax/rax; Windows stores low32(std::hash<int>).
        regs->rax = uint32_t(FnvInt(uint32_t(regs->rax)));
        break;
    case SeedKind::TimeRdx: {
        const uint32_t time = NativeTimeFromSeed(tag, uint32_t(regs->rdx));
        regs->rdx = (regs->rdx & kHigh) | Tpf2mpWindowsTimeSeed(tag, time);
        break;
    }
    case SeedKind::MixedRaxRdx:
        regs->rdx = (regs->rdx & kHigh)
            | Tpf2mpWindowsEntitySeedFromNative(tag, regs->rax, entity, true);
        break;
    case SeedKind::S1RcxRax:
        // The replayed "mov edx,1; xor rax,rcx" then leaves the Windows seed.
        regs->rax = uint64_t(Tpf2mpWindowsEntitySeedFromNative(tag, regs->rcx, entity, false))
            ^ regs->rcx;
        break;
    }
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpSimSeedDispatch(SeedRegisters* registers)
{
    const size_t index = size_t(registers->resume);
    const auto& site = kSimSeedSites[index];
    Tpf2mpSimSeedRegs regs{registers->rax, registers->rcx, registers->rdx};
    Tpf2mpSimSeedTransform(site.kind, site.tag, ReadEntity(site.entity, *registers), &regs);
    registers->rax = regs.rax;
    registers->rcx = regs.rcx;
    registers->rdx = regs.rdx;
    registers->resume = reinterpret_cast<uintptr_t>(g_trampolines[index]);
}

// Mid-function hooks: preserve every GP/XMM register and flags, align for the
// C++ call, then continue in the trampoline that replays the stolen bytes.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpSimSeedEntry()
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
        "mov %rbx, %rdi\n\tcall Tpf2mpSimSeedDispatch\n\t"
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
#define SEED_STUB(index) \
    __attribute__((naked, noinline)) void SimSeedStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpSimSeedEntry\n\t"); \
    }
SEED_STUB(0)
SEED_STUB(1)
SEED_STUB(2)
SEED_STUB(3)
SEED_STUB(4)
SEED_STUB(5)
SEED_STUB(6)
SEED_STUB(7)
#undef SEED_STUB
void (*const kSeedStubs[])() = {SimSeedStub0, SimSeedStub1, SimSeedStub2, SimSeedStub3,
                                SimSeedStub4, SimSeedStub5, SimSeedStub6, SimSeedStub7};
static_assert(sizeof(kSeedStubs) / sizeof(kSeedStubs[0]) == kSimSeedCount);

const unsigned char* OriginalStolen(const SimSeedSite& site)
{
    return site.context + (site.hookRva - site.contextRva);
}
} // namespace

bool Tpf2mpInstallSimSeeds(uintptr_t imageBase, const char* buildId)
{
    if (g_activeMask) { std::strcpy(g_status, "unchanged (already installed)"); return false; }
    const char* flag = std::getenv("TPF2MP_SIM_SEED");
    if (flag && std::strcmp(flag, "0") == 0) {
        std::strcpy(g_status, "off (TPF2MP_SIM_SEED=0)"); return false;
    }
    if (!imageBase || !buildId || std::strcmp(buildId, kSimSeedBuildId) != 0) {
        std::strcpy(g_status, "off (unverified image)"); return false;
    }
    // Verify every site before patching any: GetTime call, seed arithmetic and
    // the exact stolen instruction(s) must all be the original bytes.
    for (const auto& site : kSimSeedSites) {
        if (site.contextRva >= site.hookRva || site.steal < 5 || site.steal > 13 ||
            site.hookRva + size_t(site.steal) != site.contextRva + site.contextSize ||
            std::memcmp(reinterpret_cast<void*>(imageBase + site.contextRva),
                        site.context, site.contextSize) != 0) {
            std::snprintf(g_status, sizeof(g_status), "off (unverified seed window: %s)", site.name);
            return false;
        }
    }
    for (size_t i = 0; i < kSimSeedCount; ++i) {
        const auto& site = kSimSeedSites[i];
        if (InstallHook(imageBase + site.hookRva, reinterpret_cast<void*>(kSeedStubs[i]),
                        site.steal, &g_trampolines[i])) {
            g_activeMask |= 1u << i;
            continue;
        }
        // No game thread runs these systems yet; put every patched site back.
        unsigned stuck = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& done = kSimSeedSites[j];
            int error = 0;
            const uintptr_t at = imageBase + done.hookRva;
            if (std::memcmp(reinterpret_cast<void*>(at), OriginalStolen(done), done.steal) != 0)
                Tpf2mpCodeWriteSelf(at, OriginalStolen(done), done.steal, &error);
            if (std::memcmp(reinterpret_cast<void*>(at), OriginalStolen(done), done.steal) != 0)
                stuck |= 1u << j;
        }
        g_activeMask = stuck;
        if (stuck)
            std::snprintf(g_status, sizeof(g_status), "ERROR: hook %s failed; unrestored mask 0x%x", site.name, stuck);
        else
            std::snprintf(g_status, sizeof(g_status), "off (hook %s failed; all sites restored)", site.name);
        return false;
    }
    std::snprintf(g_status, sizeof(g_status),
                  "enabled at %zu verified seeds (SimBuilding, runway, stock list, terminal, cargo x3, parcel)",
                  kSimSeedCount);
    return true;
}

const char* Tpf2mpSimSeedStatus() { return g_status; }
