#include "speedhook.h"
#include "hook.h"
#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cmath>

// Build 35924 (ImageBase 0x140000000), all RVAs.
//
// HOW THE ENGINE PACES ITSELF (RunGameSimLoop / CGame::Step / CGame::Sync,
// decompiled 2026-09-09). The sim thread does not step per render frame. The
// main thread's CGame::Step accumulates wall time and, every "guiFrameTime"
// microseconds (m_data+0x1a0, m_data = CGame+0x168), hands the sim ONE BATCH
// through CGame::Sync; the batch runs GetSpeed() iterations of 200 ms each,
// and the renderer interpolates vehicles between the last two batches using
// (totalTime - lastSync) / guiFrameTime as the alpha. Sync recomputes
// guiFrameTime from the measured cost of the last batch (that is how speed 4
// "keeps up" on a slow machine: by stretching the interval).
//
// FRACTIONAL SPEED, SMOOTHLY: scale that interval. Speed 1.5 with lever 1 is
// one 200 ms iteration every 133 ms of wall time; the sim steps are the
// ordinary ones (lockstep by step count is untouched) and the interpolation
// stays uniform. The first version dithered the iteration COUNT per batch
// (1,2,1,2 for 1.5) -- every iteration was still a normal step, but the
// interpolation speed pulsed at 5 Hz and vehicles visibly juddered. Gone.
static const uintptr_t RVA_GETSPEED   = 0x2877a0;   // CGameTime::GetSpeed(this) -> int
static const uintptr_t RVA_SITE_PAUSE = 0x15aa30;   // GameSim::Step: call GetSpeed; test eax
static const uintptr_t RVA_SITE_COUNT = 0x15aae4;   // GameSim::Step: call GetSpeed; loop count
static const uintptr_t RVA_CGAME_STEP = 0x118e90;   // CGame::Step(this, dt, fn&)
static const int       STEAL_CGAME_STEP = 16;
static const uintptr_t OFF_MDATA        = 0x168;    // CGame+0x168 -> m_data
static const uintptr_t OFF_GUIFRAMETIME = 0x1a0;    // m_data+0x1a0: int32 microseconds per sim batch
static const uint8_t GETTER_EXPECTED[12] = {
    0x48, 0x83, 0xEC, 0x28,        // sub  rsp, 28h
    0x48, 0x8D, 0x51, 0x10,        // lea  rdx, [rcx+10h]
    0x48, 0x8B, 0x49, 0x08,        // mov  rcx, [rcx+8]
};
static const uint8_t CGAME_STEP_EXPECTED[STEAL_CGAME_STEP] = {
    0x40, 0x53,                                  // push rbx
    0x48, 0x83, 0xEC, 0x20,                      // sub  rsp, 20h
    0x48, 0x8B, 0x81, 0x68, 0x01, 0x00, 0x00,    // mov  rax, [rcx+168h]
    0x48, 0x8B, 0xD9,                            // mov  rbx, rcx
};

typedef int (*GetSpeedFn)(void* gameTime);
static GetSpeedFn      g_real = nullptr;
static uintptr_t       g_base = 0;
static SpeedLogFn      g_log  = nullptr;

static volatile double   g_target  = 0.0;   // 0 = off
static volatile LONG     g_lever   = 0;     // the engine's own speed, as GetSpeed last returned it
static volatile uint64_t g_frames  = 0;
static uintptr_t         g_retPause = 0, g_retCount = 0;
static volatile LONG     g_lastWritten = 0; // the interval we last imposed (0 = none)
static volatile LONG     g_engineBase  = 0; // the engine's own interval as last seen
static volatile LONG     g_applied     = 0; // what is in the field now
static int               g_logEvery    = 0;

extern "C" {
    void* g_cgameStepTramp = nullptr;
    void  CGameStepRelay();
}

// GetSpeed, observed at GameSim::Step's two call sites: pass the engine's
// value through unchanged, remember it as the lever.
static int __fastcall Handler(void* gameTime)
{
    const int real = g_real(gameTime);
    if ((uintptr_t)_ReturnAddress() == g_retPause) {
        g_frames = g_frames + 1;
        InterlockedExchange(&g_lever, real);
    }
    return real;
}

// CGame::Step, main thread, once per render frame, BEFORE the "is a batch
// due" test. Sync (inside Step) rewrites guiFrameTime from measurements every
// batch, so the override is re-applied here each frame: a value we did not
// write is the engine's fresh estimate and becomes the new base.
extern "C" void CGameStepSeen(uint64_t cgame)
{
    if (!cgame) return;
    uint64_t mdata = *(uint64_t*)(cgame + OFF_MDATA);
    if (!mdata) return;
    volatile LONG* field = (volatile LONG*)(mdata + OFF_GUIFRAMETIME);
    LONG cur = *field;
    if (cur <= 0) return;
    if (cur != g_lastWritten) g_engineBase = cur;      // the engine spoke since we last wrote
    const double target = g_target;
    const int lever = g_lever;
    if (target <= 0.0 || lever <= 0) {
        if (g_lastWritten && cur == g_lastWritten) { *field = g_engineBase; g_applied = g_engineBase; }
        g_lastWritten = 0;
        return;
    }
    double m = target / (double)lever;                 // desired rate over the engine's own
    if (m < 0.25) m = 0.25; else if (m > 2.0) m = 2.0;
    LONG want = (LONG)((double)g_engineBase / m + 0.5);
    if (want < 20000) want = 20000;                    // never below 20 ms per batch
    if (want != cur) { *field = want; }
    g_lastWritten = want; g_applied = want;
    if (++g_logEvery >= 600) {                         // ~10 s at 60 fps
        g_logEvery = 0;
        g_log("[speed] target %.2f over lever %d -> batch interval %ld us (engine's own %ld us)\n",
              target, lever, want, (long)g_engineBase);
    }
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
            if (p) return p;
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
    if (memcmp((void*)(g_base + RVA_CGAME_STEP), CGAME_STEP_EXPECTED, STEAL_CGAME_STEP) != 0) {
        log("[speed] CGame::Step prologue differs from build 35924 -- not installed\n");
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
    if (!InstallHook(g_base + RVA_CGAME_STEP, (void*)&CGameStepRelay, STEAL_CGAME_STEP, &g_cgameStepTramp)) {
        log("[speed] InstallHook FAILED on CGame::Step -- fractional speed unavailable\n");
        return false;
    }
    log("[speed] hooked GetSpeed at GameSim::Step (%llx, %llx) and CGame::Step (%llx): fractional speed scales the batch interval; target off\n",
        (unsigned long long)RVA_SITE_PAUSE, (unsigned long long)RVA_SITE_COUNT, (unsigned long long)RVA_CGAME_STEP);
    return true;
}

void SpeedHook_SetTarget(double t) { g_target = (t > 0.0 && t < 64.0) ? t : 0.0; }
double SpeedHook_Target()          { return g_target; }
uint64_t SpeedHook_Frames()        { return g_frames; }
int SpeedHook_LastCount()          { return g_lever; }
