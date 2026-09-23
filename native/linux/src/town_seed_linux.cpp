#include "town_seed_linux.h"
#include "windows_person_seed_linux.h"
#include "hook.h"
#include <cstddef>
#include <cstring>

namespace {
constexpr char kTownBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
constexpr uintptr_t kTownContextRva = 0x1746790;
constexpr uintptr_t kTownSeedStoreRva = 0x1746854;
constexpr uintptr_t kTownSeedResumeRva = 0x174685a;
constexpr uintptr_t kTownInitDoneRva = 0x17468e3;
// Complete TownSystem entry, GetTime/tag21 seed, and inline MT initializer.
// The only replaced instruction is MOV [RBP-0xa00],EDX (six bytes).
constexpr unsigned char kTownContextBytes[] = {
    0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
    0x41,0x55,0x41,0x54,0x41,0x89,0xd4,0x53,0x48,0x89,0xfb,0x48,
    0x81,0xec,0x48,0x0c,0x00,0x00,0x48,0x89,0xbd,0x08,0xf4,0xff,
    0xff,0x48,0x8d,0x3d,0x67,0x16,0x7f,0x02,0x48,0x89,0xb5,0x18,
    0xf4,0xff,0xff,0x64,0x48,0x8b,0x04,0x25,0x28,0x00,0x00,0x00,
    0x48,0x89,0x45,0xc8,0x31,0xc0,0xe8,0x39,0xeb,0xad,0x01,0x48,
    0x8b,0x7b,0x20,0x48,0xc7,0x85,0xd0,0xf4,0xff,0xff,0x00,0x00,
    0x00,0x00,0x48,0xc7,0x85,0xd8,0xf4,0xff,0xff,0x00,0x00,0x00,
    0x00,0x48,0xc7,0x85,0xe0,0xf4,0xff,0xff,0x00,0x00,0x00,0x00,
    0x48,0xc7,0x85,0xf0,0xf4,0xff,0xff,0x00,0x00,0x00,0x00,0x48,
    0xc7,0x85,0xf8,0xf4,0xff,0xff,0x00,0x00,0x00,0x00,0x48,0xc7,
    0x85,0x00,0xf5,0xff,0xff,0x00,0x00,0x00,0x00,0xe8,0x0e,0x67,
    0x4c,0xff,0x48,0x63,0xd0,0x48,0x8d,0xbd,0x00,0xf6,0xff,0xff,
    0xbe,0x01,0x00,0x00,0x00,0x48,0xb8,0xac,0xcb,0xa3,0x53,0x28,
    0x00,0x00,0x00,0x48,0x01,0xc2,0xb8,0xce,0x79,0x37,0x9e,0x48,
    0x8d,0x4f,0x04,0x48,0x89,0xbd,0xa8,0xf3,0xff,0xff,0x48,0x31,
    0xc2,0x48,0x89,0xcf,0x89,0x95,0x00,0xf6,0xff,0xff,0xeb,0x08,
    0x0f,0x1f,0x40,0x00,0x48,0x83,0xc7,0x04,0x89,0xd0,0xc1,0xe8,
    0x1e,0x31,0xd0,0x69,0xc0,0x65,0x89,0x07,0x6c,0x8d,0x14,0x30,
    0x48,0x83,0xc6,0x01,0x89,0x17,0x48,0x81,0xfe,0x70,0x02,0x00,
    0x00,0x75,0xdd,0x8b,0x85,0x30,0xfc,0xff,0xff,0x33,0x45,0xbc,
    0x48,0xc7,0x45,0xc0,0x70,0x02,0x00,0x00,0x8d,0x14,0x00,0x89,
    0xd6,0x81,0xf6,0xbf,0x61,0x11,0x32,0x85,0xc0,0x8b,0x85,0x00,
    0xf6,0xff,0xff,0x0f,0x48,0xd6,0x48,0x8b,0xb5,0xa8,0xf3,0xff,
    0xff,0x25,0x00,0x00,0x00,0x80,0x81,0xe2,0xff,0xff,0xff,0x7f,
    0x09,0xd0,0x48,0x8d,0x96,0xc0,0x09,0x00,0x00,0x89,0x85,0x00,
    0xf6,0xff,0xff,0xeb,0x12,0x0f,0x1f,0x00,0x48,0x39,0xca,0x0f,
    0x84,0x07,0x17,0x00,0x00,0x8b,0x01,0x48,0x83,0xc1,0x04,0x85,
    0xc0,0x74,0xed,
};
static_assert(kTownContextRva + sizeof(kTownContextBytes) == kTownInitDoneRva);
static_assert(kTownSeedResumeRva - kTownSeedStoreRva == 6);
struct TownRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags;
};
static_assert(sizeof(TownRegisters) == 128);
static_assert(offsetof(TownRegisters, rdx) == 96);
const char* g_townStatus = "off (not initialized)";
bool g_townInstalled = false;

uint32_t WindowsTownSeed(uint32_t nativeSeed)
{
    // Invert native identity hash-combine(tag21,time) modulo 2^32.
    // GetTime's int32 bits survive exactly; do not read the clock again.
    const uint32_t time = (nativeSeed ^ UINT32_C(0x9e3779ce)) - UINT32_C(0x53a3cbac);
    return Tpf2mpWindowsTimeSeed(21, time);
}
}

extern "C" {
__attribute__((visibility("hidden"))) void* Tpf2mpOriginalTownSeed = nullptr;
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpTownSeedDispatch(TownRegisters* registers)
{
    // Preserve all upper bits too: only the four-byte seed store is changed.
    registers->rdx = (registers->rdx & UINT64_C(0xffffffff00000000))
        | WindowsTownSeed(uint32_t(registers->rdx));
}

namespace {
// This is a jump inside a function, not a call boundary. Preserve every live
// GP/XMM register and flags. Dynamic alignment also handles an outgoing stack
// argument, without relying on the original function's current stack residue.
// The trampoline replays the original store, then resumes its original jump
// into the unchanged 624-word MT initializer.
__attribute__((naked, noinline)) void TownSeedEntry()
{
    __asm__(
        "pushfq\n\t"
        "push %rax\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, %rbx\n\t"
        "and $-16, %rsp\n\t"
        "sub $256, %rsp\n\t"
        "movdqu %xmm0, 0(%rsp)\n\t"
        "movdqu %xmm1, 16(%rsp)\n\t"
        "movdqu %xmm2, 32(%rsp)\n\t"
        "movdqu %xmm3, 48(%rsp)\n\t"
        "movdqu %xmm4, 64(%rsp)\n\t"
        "movdqu %xmm5, 80(%rsp)\n\t"
        "movdqu %xmm6, 96(%rsp)\n\t"
        "movdqu %xmm7, 112(%rsp)\n\t"
        "movdqu %xmm8, 128(%rsp)\n\t"
        "movdqu %xmm9, 144(%rsp)\n\t"
        "movdqu %xmm10, 160(%rsp)\n\t"
        "movdqu %xmm11, 176(%rsp)\n\t"
        "movdqu %xmm12, 192(%rsp)\n\t"
        "movdqu %xmm13, 208(%rsp)\n\t"
        "movdqu %xmm14, 224(%rsp)\n\t"
        "movdqu %xmm15, 240(%rsp)\n\t"
        "mov %rbx, %rdi\n\t"
        "call Tpf2mpTownSeedDispatch\n\t"
        "movdqu 0(%rsp), %xmm0\n\t"
        "movdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\t"
        "movdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\t"
        "movdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\t"
        "movdqu 112(%rsp), %xmm7\n\t"
        "movdqu 128(%rsp), %xmm8\n\t"
        "movdqu 144(%rsp), %xmm9\n\t"
        "movdqu 160(%rsp), %xmm10\n\t"
        "movdqu 176(%rsp), %xmm11\n\t"
        "movdqu 192(%rsp), %xmm12\n\t"
        "movdqu 208(%rsp), %xmm13\n\t"
        "movdqu 224(%rsp), %xmm14\n\t"
        "movdqu 240(%rsp), %xmm15\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rax\n\t"
        "popfq\n\t"
        "jmp *Tpf2mpOriginalTownSeed(%rip)\n\t"
    );
}
}

bool Tpf2mpInstallTownSeed(uintptr_t imageBase, const char* buildId)
{
    if (g_townInstalled) { g_townStatus = "enabled (already installed)"; return false; }
    if (!imageBase || !buildId || std::strcmp(buildId, kTownBuildId) != 0) {
        g_townStatus = "off (unverified image)"; return false;
    }
    if (std::memcmp(reinterpret_cast<void*>(imageBase + kTownContextRva),
                    kTownContextBytes, sizeof(kTownContextBytes)) != 0) {
        g_townStatus = "off (unverified TownSystem seed/initializer)"; return false;
    }
    g_townInstalled = InstallHook(imageBase + kTownSeedStoreRva,
        reinterpret_cast<void*>(TownSeedEntry), 6, &Tpf2mpOriginalTownSeed);
    if (g_townInstalled) g_townStatus = "enabled at verified TownSystem tag21 seed store";
    else if (std::memcmp(reinterpret_cast<void*>(imageBase + kTownSeedStoreRva),
                         kTownContextBytes + (kTownSeedStoreRva - kTownContextRva), 6) == 0)
        g_townStatus = "off (hook failed; original seed store intact)";
    else g_townStatus = "ERROR: hook failed and seed store is altered";
    return g_townInstalled;
}

const char* Tpf2mpTownSeedStatus() { return g_townStatus; }
