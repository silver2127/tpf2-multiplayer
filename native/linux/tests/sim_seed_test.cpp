// Sim seed adapter test. Oracle values come from executing the ORIGINAL
// Windows build-35924 seed instructions (a74070, a69670, aa4a90, a81810,
// a7a770, a7b070, a78110, 95f730) under Unicorn; the native register values
// were captured at each hook address by executing the original ELF
// instructions from the GetTime return (scratch emu_simseed.py).
#include "../src/sim_seed_linux.cpp"
#include "../src/codewrite_linux.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>

struct Vector { unsigned site; uint64_t rax, rcx, rdx; uint32_t entity, windows; };
static const Vector kVectors[] = {
#include "sim_seed_vectors.inc"
};

extern "C" {
__attribute__((visibility("hidden"))) uint64_t g_fixIn[16];   // rax rcx rdx rbx rsi rdi rbp r8..r15
__attribute__((visibility("hidden"))) uint64_t g_fixOut[16];
__attribute__((visibility("hidden"))) uint64_t g_fixTarget;
__attribute__((visibility("hidden"))) uint64_t g_fixSavedRsp;
void Tpf2mpSimSeedFixtureRun();
void Tpf2mpSimSeedFixtureDone();
}
__asm__(
    ".text\n"
    ".globl Tpf2mpSimSeedFixtureRun\n.hidden Tpf2mpSimSeedFixtureRun\n"
    "Tpf2mpSimSeedFixtureRun:\n"
    "  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "  mov %rsp, g_fixSavedRsp(%rip)\n"
    "  sub $0x1000, %rsp\n"
    "  mov g_fixIn+0(%rip), %rax\n mov g_fixIn+8(%rip), %rcx\n mov g_fixIn+16(%rip), %rdx\n"
    "  mov g_fixIn+24(%rip), %rbx\n mov g_fixIn+32(%rip), %rsi\n mov g_fixIn+40(%rip), %rdi\n"
    "  mov g_fixIn+48(%rip), %rbp\n mov g_fixIn+56(%rip), %r8\n mov g_fixIn+64(%rip), %r9\n"
    "  mov g_fixIn+72(%rip), %r10\n mov g_fixIn+80(%rip), %r11\n mov g_fixIn+88(%rip), %r12\n"
    "  mov g_fixIn+96(%rip), %r13\n mov g_fixIn+104(%rip), %r14\n mov g_fixIn+112(%rip), %r15\n"
    "  jmp *g_fixTarget(%rip)\n"
    ".globl Tpf2mpSimSeedFixtureDone\n.hidden Tpf2mpSimSeedFixtureDone\n"
    "Tpf2mpSimSeedFixtureDone:\n"
    "  mov %rax, g_fixOut+0(%rip)\n mov %rcx, g_fixOut+8(%rip)\n mov %rdx, g_fixOut+16(%rip)\n"
    "  mov %rbx, g_fixOut+24(%rip)\n mov %rsi, g_fixOut+32(%rip)\n mov %rdi, g_fixOut+40(%rip)\n"
    "  mov %rbp, g_fixOut+48(%rip)\n mov %r8, g_fixOut+56(%rip)\n mov %r9, g_fixOut+64(%rip)\n"
    "  mov %r10, g_fixOut+72(%rip)\n mov %r11, g_fixOut+80(%rip)\n mov %r12, g_fixOut+88(%rip)\n"
    "  mov %r13, g_fixOut+96(%rip)\n mov %r14, g_fixOut+104(%rip)\n mov %r15, g_fixOut+112(%rip)\n"
    "  mov %rsp, g_fixOut+120(%rip)\n"
    "  mov g_fixSavedRsp(%rip), %rsp\n"
    "  pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

static void Write(uintptr_t address, const void* bytes, size_t size)
{
    int error = 0;
    assert(Tpf2mpCodeWriteSelf(address, static_cast<const uint8_t*>(bytes), size, &error) == TPF2MP_CW_OK);
}

static void JumpAt(uintptr_t at, uintptr_t target)
{
    unsigned char bytes[14] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(bytes + 6, &target, 8);
    Write(at, bytes, sizeof(bytes));
}

int main()
{
    // 1. Pure transform against the original-Windows oracle.
    unsigned pure = 0;
    for (const auto& v : kVectors) {
        const auto& site = kSimSeedSites[v.site];
        Tpf2mpSimSeedRegs r{v.rax, v.rcx, v.rdx};
        Tpf2mpSimSeedTransform(site.kind, site.tag, v.entity, &r);
        uint32_t seed = 0;
        switch (site.kind) {
        case SeedKind::FnvRax: assert(r.rax >> 32 == 0); seed = uint32_t(r.rax); break;
        case SeedKind::TimeRdx:
        case SeedKind::MixedRaxRdx:
            assert(r.rdx >> 32 == v.rdx >> 32); assert(r.rax == v.rax); seed = uint32_t(r.rdx); break;
        case SeedKind::S1RcxRax: assert(r.rcx == v.rcx); seed = uint32_t(r.rax ^ r.rcx); break;
        }
        assert(seed == v.windows);
        ++pure;
    }

    // 2. Refusals: unknown image, kill switch, tampered context bytes.
    assert(!Tpf2mpInstallSimSeeds(1, "unknown"));
    assert(!Tpf2mpInstallSimSeeds(0, kSimSeedBuildId));
    constexpr size_t length = 0x1730000;
    void* memory = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    for (const auto& site : kSimSeedSites)
        std::memcpy(reinterpret_cast<void*>(base + site.contextRva), site.context, site.contextSize);
    assert(mprotect(memory, length, PROT_READ | PROT_EXEC) == 0);
    setenv("TPF2MP_SIM_SEED", "0", 1);
    assert(!Tpf2mpInstallSimSeeds(base, kSimSeedBuildId));
    assert(std::strstr(Tpf2mpSimSeedStatus(), "TPF2MP_SIM_SEED=0"));
    unsetenv("TPF2MP_SIM_SEED");
    for (const auto& site : kSimSeedSites) {
        for (size_t off : {size_t(0), site.contextSize / 2, size_t(site.hookRva - site.contextRva), site.contextSize - 1}) {
            const uintptr_t at = base + site.contextRva + off;
            const unsigned char old = *reinterpret_cast<unsigned char*>(at), changed = old ^ 1;
            Write(at, &changed, 1);
            assert(!Tpf2mpInstallSimSeeds(base, kSimSeedBuildId));
            assert(g_activeMask == 0);
            Write(at, &old, 1);
        }
    }
    assert(Tpf2mpInstallSimSeeds(base, kSimSeedBuildId));
    assert(g_activeMask == 0xffu);
    assert(!Tpf2mpInstallSimSeeds(base, kSimSeedBuildId));

    // 3. Execute every installed hook: enter at the patched instruction with the
    //    native register state, let the trampoline replay the stolen bytes, and
    //    capture at the first untouched instruction after them.
    for (const auto& site : kSimSeedSites)
        JumpAt(base + site.hookRva + site.steal, reinterpret_cast<uintptr_t>(&Tpf2mpSimSeedFixtureDone));
    static unsigned char frame[0x4000];
    unsigned executed = 0;
    for (const auto& v : kVectors) {
        const auto& site = kSimSeedSites[v.site];
        std::memset(frame, 0xa5, sizeof(frame));
        static uint32_t entity;
        entity = v.entity;
        const uintptr_t rbp = reinterpret_cast<uintptr_t>(frame) + 0x3000;
        const uintptr_t entityAddress = reinterpret_cast<uintptr_t>(&entity);
        std::memcpy(reinterpret_cast<void*>(rbp - 0xb48), &entityAddress, 8);
        uint64_t in[16];
        for (unsigned i = 0; i < 16; ++i) in[i] = UINT64_C(0x5a5a000000000000) + i * UINT64_C(0x1111111);
        in[0] = v.rax; in[1] = v.rcx; in[2] = v.rdx; in[6] = rbp;
        in[12] = entityAddress; in[13] = entityAddress;   // r13, r14
        std::memcpy(g_fixIn, in, sizeof(in));
        g_fixTarget = base + site.hookRva;
        Tpf2mpSimSeedFixtureRun();
        uint32_t seed = 0, stored = 0;
        switch (site.kind) {
        case SeedKind::FnvRax:
            seed = uint32_t(g_fixOut[0]);
            assert(g_fixOut[0] >> 32 == 0);
            if (site.hookRva == 0x16e4987) {       // mov [rbp-0x13c0],rax
                uint64_t word; std::memcpy(&word, reinterpret_cast<void*>(rbp - 0x13c0), 8);
                assert(word == g_fixOut[0]); stored = uint32_t(word);
            } else {                                // mov edx,eax; mov [rbp-0xa00],eax
                std::memcpy(&stored, reinterpret_cast<void*>(rbp - 0xa00), 4);
                assert(uint32_t(g_fixOut[2]) == seed);
            }
            break;
        case SeedKind::TimeRdx:
        case SeedKind::MixedRaxRdx:
            seed = uint32_t(g_fixOut[2]);
            std::memcpy(&stored, reinterpret_cast<void*>(rbp - 0xa00), 4);
            assert(g_fixOut[0] == v.rax && g_fixOut[2] >> 32 == v.rdx >> 32);
            break;
        case SeedKind::S1RcxRax:
            seed = uint32_t(g_fixOut[0] ^ g_fixOut[1]);   // the native "xor rax,rcx" follows
            stored = seed;
            assert(g_fixOut[2] == 1 && g_fixOut[1] == v.rcx);
            break;
        }
        assert(seed == v.windows && stored == v.windows);
        for (unsigned i : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u}) assert(g_fixOut[i] == in[i]);
        ++executed;
    }
    std::printf("Sim seeds: %u oracle transforms and %u executed hook cases passed; %s\n",
                pure, executed, Tpf2mpSimSeedStatus());
    return 0;
}
