// A hook over a RIP-relative prologue (native/src/hook.cpp: PrologueStealRip,
// InstallHookRip). Wine's vkGetDeviceProcAddr (CrossOver on macOS) opens with
// test byte [rip+x],8 inside the 14 bytes the hook overwrites; the plain
// decoder refused it and the in-game panel never appeared (2026-10-03).
//
//   tools\msvc_env.bat && cl /nologo /EHsc /I native\src tools\test_hook_riprel.cpp native\src\hook.cpp /Fe:out\test_hook_riprel.exe
#include "hook.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

typedef int (*Fn)();
static Fn g_orig = nullptr;
static int Detour() { return g_orig() + 10; }

int main()
{
    // 1. the exact prologue from the player's tpf2_menu.log
    const unsigned char wine[] = {
        0x41, 0x54, 0x55, 0x57, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0xf6, 0x05, 0x6f, 0x5a, 0x00, 0x00,
        0x08, 0x48, 0x8d, 0x3d, 0x68, 0x5a, 0x00, 0x00, 0x48, 0x89, 0xce, 0x48 };
    check(PrologueSteal(wine, 14) == 0, "the plain decoder still refuses a RIP-relative prologue");
    int fix[4] = {0}, nfix = 0;
    int steal = PrologueStealRip(wine, 14, fix, 4, &nfix);
    check(steal == 17, "the RIP-aware decoder cuts after test byte [rip+x],8 (17 bytes)");
    check(nfix == 1 && fix[0] == 12, "its disp32 is at offset 12");

    // 2. a live function with that prologue: returns 1 when bit 3 of a byte it
    //    reads RIP-relative is set, else 0
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    const unsigned char body[] = {
        0x41, 0x54, 0x55, 0x57, 0x56, 0x53,             // push r12, rbp, rdi, rsi, rbx
        0x48, 0x83, 0xec, 0x40,                         // sub rsp, 0x40
        0xf6, 0x05, 0, 0, 0, 0, 0x08,                   // test byte [rip+X], 8   (ends at 17)
        0x0f, 0x95, 0xc0,                               // setnz al
        0x0f, 0xb6, 0xc0,                               // movzx eax, al
        0x48, 0x83, 0xc4, 0x40,                         // add rsp, 0x40
        0x5b, 0x5e, 0x5f, 0x5d, 0x41, 0x5c,             // pop rbx, rsi, rdi, rbp, r12
        0xc3 };                                         // ret
    memcpy(code, body, sizeof body);
    const int dataOff = 0x200;
    int32_t disp = dataOff - 17;
    memcpy(code + 12, &disp, 4);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof body);
    Fn fn = (Fn)code;
    code[dataOff] = 0x08;
    check(fn() == 1, "unhooked: bit set -> 1");
    code[dataOff] = 0x00;
    check(fn() == 0, "unhooked: bit clear -> 0");

    steal = PrologueStealRip(code, 14, fix, 4, &nfix);
    void* tramp = nullptr;
    check(steal == 17 && nfix == 1 && InstallHookRip((uintptr_t)code, (void*)&Detour, steal, fix, nfix, &tramp),
          "InstallHookRip hooks it");
    g_orig = (Fn)tramp;
    long long dist = (long long)(uintptr_t)tramp - (long long)(uintptr_t)code;
    check(dist > -0x7FFFFFFFLL && dist < 0x7FFFFFFFLL, "the trampoline is within 2 GB of the target");
    code[dataOff] = 0x08;
    check(fn() == 11, "hooked: the detour runs and the relocated test still reads the same byte (bit set -> 1+10)");
    code[dataOff] = 0x00;
    check(fn() == 10, "hooked: bit clear -> 0+10");

    printf(failures ? "FAILED: %d check(s)\n" : "PASS: a RIP-relative prologue is hooked and still reads the right memory\n", failures);
    return failures ? 1 : 0;
}
