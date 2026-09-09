#include "speedhook.h"
#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cmath>

// Build 35924 (ImageBase 0x140000000), all RVAs.
static const uintptr_t RVA_GETSPEED   = 0x2877a0;   // CGameTime::GetSpeed(this) -> int
static const uintptr_t RVA_SITE_PAUSE = 0x15aa30;   // GameSim::Step: call GetSpeed; test eax
static const uintptr_t RVA_SITE_COUNT = 0x15aae4;   // GameSim::Step: call GetSpeed; loop count
// The getter's prologue we reversed (25-byte function); refuse on any change.
static const uint8_t GETTER_EXPECTED[12] = {
    0x48, 0x83, 0xEC, 0x28,        // sub  rsp, 28h
    0x48, 0x8D, 0x51, 0x10,        // lea  rdx, [rcx+10h]
    0x48, 0x8B, 0x49, 0x08,        // mov  rcx, [rcx+8]
};

typedef int (*GetSpeedFn)(void* gameTime);
static GetSpeedFn      g_real = nullptr;
static uintptr_t       g_base = 0;
static SpeedLogFn      g_log  = nullptr;

static volatile double   g_target  = 0.0;   // 0 = off
static double            g_acc     = 0.0;   // dither accumulator (sim thread only)
static volatile LONG     g_lastN   = 0;
static volatile uint64_t g_frames  = 0;
static uintptr_t         g_retPause = 0, g_retCount = 0;

// Called from the sim thread in place of GetSpeed, twice per frame. The
// pause-test call decides the frame's count; the count call reuses it, so the
// engine never sees two different answers within one frame.
static int __fastcall Handler(void* gameTime)
{
    const int real = g_real(gameTime);
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    const double target = g_target;
    if (target <= 0.0 || real <= 0) {            // off, or the engine says paused
        g_acc = 0.0;
        InterlockedExchange(&g_lastN, real);
        if (ret == g_retPause) g_frames = g_frames + 1;
        return real;
    }
    if (ret == g_retPause) {
        g_frames = g_frames + 1;
        g_acc += target;
        int n = (int)floor(g_acc + 1e-9);
        if (n > 64) n = 64;                      // sanity: never ask for a runaway frame
        g_acc -= n;
        if (g_acc < 0.0) g_acc = 0.0;
        InterlockedExchange(&g_lastN, n);
        return n;
    }
    return g_lastN;                              // the count call: same answer as the pause test
}

// A 12-byte stub within +/-2 GB of the exe so a rel32 call can reach our
// handler: mov rax, imm64 ; jmp rax.
static void* AllocNear(uintptr_t anchor)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (int s = 0; s < 2; s++) {
            uintptr_t a = s ? anchor - d : anchor + d;
            void* p = VirtualAlloc((void*)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) {
                g_log("[speed] stub page %p (asked %llx, base %llx)\n", p, (unsigned long long)a, (unsigned long long)anchor);
                return p;
            }
        }
    }
    return nullptr;
}

static bool PatchCall(uintptr_t site, uintptr_t stub)
{
    uint8_t cur[5]; memcpy(cur, (void*)site, 5);
    int32_t rel; memcpy(&rel, cur + 1, 4);
    if (cur[0] != 0xE8 || site + 5 + rel != g_base + RVA_GETSPEED) {
        g_log("[speed] call site %llx does not call GetSpeed (bytes %02x %02x %02x %02x %02x) -- refusing\n",
              (unsigned long long)(site - g_base), cur[0], cur[1], cur[2], cur[3], cur[4]);
        return false;
    }
    intptr_t d = (intptr_t)stub - (intptr_t)(site + 5);
    if (d > INT32_MAX || d < INT32_MIN) {
        g_log("[speed] stub %llx out of rel32 range of site %llx (base %llx)\n",
              (unsigned long long)stub, (unsigned long long)site, (unsigned long long)g_base);
        return false;
    }
    int32_t rel2 = (int32_t)d;
    DWORD old;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(site + 1), &rel2, 4);
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}

bool SpeedHook_Install(SpeedLogFn log)
{
    g_log = log;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    const uintptr_t getter = g_base + RVA_GETSPEED;
    if (memcmp((void*)getter, GETTER_EXPECTED, sizeof(GETTER_EXPECTED)) != 0) {
        log("[speed] CGameTime::GetSpeed prologue differs from build 35924 -- not installed\n");
        return false;
    }
    void* stub = AllocNear(g_base);
    if (!stub) { log("[speed] no executable page within 2 GB of the exe -- not installed\n"); return false; }
    uint8_t code[12] = { 0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0 };
    uintptr_t h = (uintptr_t)&Handler;
    memcpy(code + 2, &h, 8);
    memcpy(stub, code, 12);
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    g_real = (GetSpeedFn)getter;
    g_retPause = g_base + RVA_SITE_PAUSE + 5;
    g_retCount = g_base + RVA_SITE_COUNT + 5;
    if (!PatchCall(g_base + RVA_SITE_PAUSE, (uintptr_t)stub)) return false;
    if (!PatchCall(g_base + RVA_SITE_COUNT, (uintptr_t)stub)) return false;
    log("[speed] hooked GameSim::Step's two GetSpeed calls (%llx, %llx) -> stub %p; target off\n",
        (unsigned long long)RVA_SITE_PAUSE, (unsigned long long)RVA_SITE_COUNT, stub);
    return true;
}

void SpeedHook_SetTarget(double t) { g_target = (t > 0.0 && t < 64.0) ? t : 0.0; }
double SpeedHook_Target()          { return g_target; }
uint64_t SpeedHook_Frames()        { return g_frames; }
int SpeedHook_LastCount()          { return g_lastN; }
