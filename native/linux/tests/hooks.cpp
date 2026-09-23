#include "hook.h"
#include "codewrite_linux.h"
#include "near_alloc.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

static void* original;
static void* originalShort;
static int Detour() { return reinterpret_cast<int (*)()>(original)() + 7; }
static int ShortDetour() { return reinterpret_cast<int (*)()>(originalShort)() + 3; }
static int CallTarget() { return 42; }

static int Permissions(uintptr_t addr)
{
    FILE* f = fopen("/proc/self/maps", "r");
    assert(f);
    char line[8192], mode[5];
    unsigned long lo, hi;
    int result = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, mode) == 3 && addr >= lo && addr < hi)
            result = (mode[0] == 'r' ? PROT_READ : 0) | (mode[1] == 'w' ? PROT_WRITE : 0)
                   | (mode[2] == 'x' ? PROT_EXEC : 0);
    }
    fclose(f);
    return result;
}

int main()
{
    const unsigned char imm16[] = {0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,
                                  0x66,0xc7,0x45,0xf0,0x34,0x12,0x48,0x89,0xc7};
    const unsigned char mov16[] = {0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,
                                  0x66,0xb8,0x34,0x12,0x48,0x89,0xc7};
    const unsigned char rex64[] = {0x66,0x48,0xb8,1,2,3,4,5,6,7,8,0x90};
    const unsigned char relative[] = {0x48,0x8b,0x05,0,0,0,0};
    assert(PrologueSteal(imm16, 14) == 14);
    assert(PrologueSteal(mov16, 14) == 15);
    assert(PrologueSteal(rex64, 10) == 11);
    assert(PrologueSteal(relative, 1) == 0);

    auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    memset(page, 0x90, 4096);
    // Fourteen copyable NOPs followed by `mov eax, 5; ret`.
    const unsigned char answer[] = {0xb8,5,0,0,0,0xc3};
    memcpy(page + 14, answer, sizeof(answer));
    // A second function calls a neighboring implementation, then returns.
    page[64] = 0xe8;
    const int32_t rel = 96 - 69;
    memcpy(page + 65, &rel, sizeof(rel));
    page[69] = 0xc3;
    memcpy(page + 96, answer, sizeof(answer));
    const unsigned char shortFn[] = {0xf3,0x0f,0x1e,0xfa,0x55,0x8b,0x05,21,0,0,0,0x5d,0xc3};
    memcpy(page + 192, shortFn, sizeof(shortFn));
    const int shortValue = 11;
    memcpy(page + 224, &shortValue, sizeof(shortValue));
    assert(mprotect(page, 4096, PROT_READ | PROT_EXEC) == 0);
    auto f = reinterpret_cast<int (*)()>(page);
    assert(f() == 5);
    assert(InstallHook(reinterpret_cast<uintptr_t>(page), (void*)&Detour, 14, &original));
    assert(f() == 12);
    assert(reinterpret_cast<int (*)()>(page + 192)() == 11);
    assert(PrologueSteal(page + 192, 5) == 5);
    assert(PrologueSteal(page + 192, 14) == 0); // RIP-relative instruction stays at its original address
    assert(InstallHook(reinterpret_cast<uintptr_t>(page + 192), (void*)&ShortDetour, 5, &originalShort));
    assert(reinterpret_cast<int (*)()>(page + 192)() == 14);
    assert(Permissions(reinterpret_cast<uintptr_t>(page)) == (PROT_READ | PROT_EXEC));
    assert(Tpf2mpRedirectCall(reinterpret_cast<uintptr_t>(page + 64),
                             reinterpret_cast<uintptr_t>(page + 96), (void*)&CallTarget));
    assert(reinterpret_cast<int (*)()>(page + 64)() == 42);
    assert(!Tpf2mpRedirectCall(reinterpret_cast<uintptr_t>(page + 64),
                              reinterpret_cast<uintptr_t>(page + 96), (void*)&CallTarget));

    // Writers on the same RX page never change each other's protections.
    auto writer = [&](size_t offset, unsigned char value) {
        for (int i = 0; i < 1000; ++i) {
            int err;
            assert(Tpf2mpCodeWriteSelf(reinterpret_cast<uintptr_t>(page + offset), &value, 1, &err)
                   == TPF2MP_CW_OK);
        }
    };
    std::thread a(writer, 128, 0xa5), b(writer, 129, 0x5a);
    a.join(); b.join();
    assert(page[128] == 0xa5 && page[129] == 0x5a);
    assert(Permissions(reinterpret_cast<uintptr_t>(page)) == (PROT_READ | PROT_EXEC));
    int err;
    assert(Tpf2mpCodeWriteSelf(1, answer, sizeof(answer), &err) == TPF2MP_CW_UNAVAILABLE);
    assert(!InstallHook(0, (void*)&Detour, 14, &original));
    puts("PASS: hook/trampoline execution, call redirect, imm16 boundaries, concurrent RX writes and refusal");
    munmap(page, 4096);
}
