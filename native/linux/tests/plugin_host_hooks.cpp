// Exercise the plugin API itself: direct writer tests missed the old 14-byte floor.
#define TPF2MP_PLUGINHOST_TEST
#include "../src/plugin/host_linux.cpp"
#include <cassert>

static void* original;
static int Detour() { return reinterpret_cast<int (*)()>(original)() + 7; }

int main()
{
    // Build-35924 AddTile entry, independently checked in the lab ELF.
    const unsigned char addTile[] = {
        0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x49,0x89,0xff,
        0x48,0x8d,0x3d,0x83,0xe4,0x22,0x03
    };
    assert(PrologueSteal(addTile, 13) == 13);
    assert(PrologueSteal(addTile, 14) == 0);
    auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    memset(page, 0x90, 4096);
    // Each function leaves its RIP-relative read immediately after the stolen span.
    // The trampoline must jump back to that read at its original address.
    for (int n = 5; n <= 32; ++n) {
        auto* fn = page + (n - 5) * 64;
        fn[n] = 0x8b; fn[n + 1] = 0x05;
        const int32_t displacement = 48 - n - 6;
        memcpy(fn + n + 2, &displacement, 4);
        fn[n + 6] = 0xc3;
        const int answer = 11;
        memcpy(fn + 48, &answer, 4);
    }
    memcpy(page + 2048, addTile, sizeof(addTile));
    assert(mprotect(page, 4096, PROT_READ | PROT_EXEC) == 0);
    for (int n = 5; n <= 32; ++n) {
        auto* fn = page + (n - 5) * 64;
        auto call = reinterpret_cast<int (*)()>(fn);
        assert(call() == 11);
        original = nullptr;
        assert(g_api.installHook(reinterpret_cast<uintptr_t>(fn), (void*)&Detour, n, &original));
        assert(original && call() == 18);
        assert(reinterpret_cast<int (*)()>(original)() == 11);
        assert(fn[0] == (n < 14 ? 0xe9 : 0xff));
        assert(fn[n] == 0x8b && fn[n + 1] == 0x05);
        assert(munmap(original, 4096) == 0);
    }
    const uintptr_t at = reinterpret_cast<uintptr_t>(page + 2048);
    original = nullptr;
    for (int n : {0, 4, 33, 6, 7, 9, 11, 12}) {
        assert(!g_api.installHook(at, (void*)&Detour, n, &original));
        assert(!original);
        assert(memcmp(page + 2048, addTile, sizeof(addTile)) == 0);
    }
    assert(!g_api.installHook(0, (void*)&Detour, 13, &original));
    assert(!g_api.installHook(at, nullptr, 13, &original));
    assert(!g_api.installHook(at, (void*)&Detour, 13, nullptr));
    assert(mprotect(page, 4096, PROT_READ) == 0);
    assert(!g_api.installHook(at, (void*)&Detour, 13, &original));
    assert(munmap(page, 4096) == 0);
    puts("PASS: host API 5..32-byte hook execution, RIP-relative continuation and refusal");
}
