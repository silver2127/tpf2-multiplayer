// Linux build of speedhook.cpp: fractional game speed by scaling the engine's
// sim batch interval. How the engine paces itself, and why the interval is
// scaled rather than the iteration count dithered, is commented in
// speedhook.cpp; only the sites and offsets differ here.
//
// Build 35924, Linux (PIE: RVAs are the file's virtual addresses).
//   CGameTime::GetSpeed  0xc0dc30  an int read from the GameSpeed component.
//                        Unnamed in the binary; it sits among CGameTime's
//                        methods, and GameSim::Step (0xa61250) calls it twice,
//                        exactly as on Windows: 0xa616b9 the pause test,
//                        0xa61710 the iteration count.
//   CGame::Step          0xa31c30  "void CGame::Step(long long int, const std::function<void()>&)"
//   m_data               CGame+0x160            (Windows +0x168)
//   guiFrameTime         m_data+0x1a8, int32 us (Windows +0x1a0)
//   Step's own assert names the three fields it compares,
//   "m_data->totalTime < m_data->lastSyncTime + m_data->guiFrameTime": totalTime
//   is +0xe0 (Step adds dt to it), lastSyncTime +0xe8, guiFrameTime +0x1a8.
//
// Two differences from Windows, neither in behaviour:
//   - GetSpeed is hooked at its entry, not at the two call sites. The handler
//     hands every caller the engine's value either way; an entry hook just needs
//     no stub within 2 GB of the executable. Only a call that returns to the
//     pause test records the lever.
//   - CGame::Step needs no assembly relay: its SysV signature is known, so the
//     detour is an ordinary function that looks at `this` and calls on.
#include "speedhook.h"
#include "game_image.h"
#include "hook.h"
#include <atomic>
#include <cstring>

static const uintptr_t RVA_GETSPEED     = 0xc0dc30;
static const uintptr_t RVA_SITE_PAUSE   = 0xa616b9;   // GameSim::Step: call GetSpeed; test eax
static const uintptr_t RVA_SITE_COUNT   = 0xa61710;   // GameSim::Step: call GetSpeed; loop count
static const uintptr_t RVA_CGAME_STEP   = 0xa31c30;
static const uintptr_t RVA_CGAME_SYNC   = 0xa30cc0;   // "bool CGame::Sync(const std::function<void()>&)"
static const uintptr_t OFF_MDATA        = 0x160;
static const uintptr_t OFF_GUIFRAMETIME = 0x1a8;

static const uint8_t GETTER_EXPECTED[14] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x55,                    // push r13
    0x4C, 0x8D, 0x6F, 0x10,        // lea  r13, [rdi+10h]
};
static const uint8_t CGAME_STEP_EXPECTED[15] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x55,                    // push r13
    0x49, 0x89, 0xFD,              // mov  r13, rdi
    0x41, 0x54,                    // push r12
};

static const uint8_t CGAME_SYNC_EXPECTED[15] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x49, 0x89, 0xFF,              // mov  r15, rdi
    0x41, 0x56,                    // push r14
};

using GetSpeedFn  = int (*)(void* gameTime);
using CGameSyncFn = bool (*)(void* cgame, const void* fn);
using CGameStepFn = void (*)(void* cgame, long long dt, const void* fn);

static void*      g_getSpeedTramp = nullptr;
static void*      g_stepTramp     = nullptr;
static void*      g_syncTramp     = nullptr;
static uintptr_t  g_retPause      = 0;
static SpeedLogFn g_log           = nullptr;

static std::atomic<double>   g_target{0.0};   // 0 = off
static std::atomic<int>      g_lever{0};      // the engine's own speed, as GetSpeed last returned it
static std::atomic<uint64_t> g_frames{0};
// Main thread only (ImposeInterval, after CGame::Sync):
static int g_lastWritten = 0;   // the interval we last imposed (0 = none)
static std::atomic<int> g_engineBase{0}; // published to the bridge's control thread
static std::atomic<int> g_pinUs{0};
static int g_logEvery    = 0;

static int GetSpeedDetour(void* gameTime)
{
    const int real = ((GetSpeedFn)g_getSpeedTramp)(gameTime);
    if ((uintptr_t)__builtin_return_address(0) == g_retPause) {
        g_frames.fetch_add(1, std::memory_order_relaxed);
        g_lever.store(real, std::memory_order_relaxed);
    }
    return real;
}

// THE INTERVAL CHANGES ONLY AT A BATCH BOUNDARY (2026-09-22; speedhook.cpp has
// the whole story). CGame::Step calls Sync for each due batch and then, in the
// same frame, clamps totalTime and computes the render alpha with whatever
// guiFrameTime Sync wrote. Re-imposing the override at the next frame's Step
// entry changed the interval inside a batch: when ours was the larger (the
// dedicated server's 200 ms pin over a faster estimate) alpha fell, the render
// clock stepped back, and a ship wake started in between asserted
// ShipFoamRenderer.cpp:137 `startAge >= 0` (lab, retained host after a live
// join). So the override is imposed right after Sync returns, and nowhere else.
static void ImposeInterval(void* cgame)
{
    if (!cgame) return;
    const uintptr_t mdata = *(uintptr_t*)((uintptr_t)cgame + OFF_MDATA);
    if (!mdata) return;
    int* field = (int*)(mdata + OFF_GUIFRAMETIME);
    const int cur = *field;
    if (cur <= 0) return;
    g_engineBase = cur;                                // Sync always writes its estimate
    const double target = g_target.load(std::memory_order_relaxed);
    const int lever = g_lever.load(std::memory_order_relaxed);
    const int pin = g_pinUs.load(std::memory_order_relaxed);
    const int engineBase = g_engineBase.load(std::memory_order_relaxed);
    if (target <= 0.0 || lever <= 0) {
        if (pin > 0) {
            if (cur != pin) *field = pin;
            g_lastWritten = pin;
            return;
        }
        g_lastWritten = 0;                             // the engine's own estimate stands
        return;
    }
    double m = target / (double)lever;
    if (m < 0.25) m = 0.25; else if (m > 2.0) m = 2.0;
    int want = (int)((double)(pin > 0 ? pin : engineBase) / m + 0.5);
    if (want < 20000) want = 20000;                    // never below 20 ms per batch
    if (want != cur) *field = want;
    g_lastWritten = want;
    if (++g_logEvery >= 50) {                          // once per 50 batches (~10 s at 200 ms)
        g_logEvery = 0;
        g_log("[speed] target %.2f over lever %d -> batch interval %d us (engine's own %d us)\n",
              target, lever, want, engineBase);
    }
}

static bool CGameSyncDetour(void* cgame, const void* fn)
{
    const bool ok = ((CGameSyncFn)g_syncTramp)(cgame, fn);
    if (ok) ImposeInterval(cgame);
    return ok;
}

// CGame::Step, main thread, once per frame: nothing is written here any more.
static void CGameStepDetour(void* cgame, long long dt, const void* fn)
{
    ((CGameStepFn)g_stepTramp)(cgame, dt, fn);
}

static bool CallsGetSpeed(uintptr_t base, uintptr_t siteRva)
{
    const uint8_t* s = (const uint8_t*)(base + siteRva);
    int32_t rel;
    memcpy(&rel, s + 1, 4);
    return s[0] == 0xE8 && base + siteRva + 5 + rel == base + RVA_GETSPEED;
}

bool SpeedHook_Install(SpeedLogFn log)
{
    g_log = log;
    const Tpf2GameImage img = Tpf2mpGameImage();
    if (!img.buildOk) {
        log("[speed] the game is not build 35924 (GNU build-id differs) -- not installed\n");
        return false;
    }
    const uintptr_t base = img.base;
    if (memcmp((void*)(base + RVA_GETSPEED), GETTER_EXPECTED, sizeof(GETTER_EXPECTED)) != 0) {
        log("[speed] CGameTime::GetSpeed prologue differs from build 35924 -- not installed\n");
        return false;
    }
    if (memcmp((void*)(base + RVA_CGAME_STEP), CGAME_STEP_EXPECTED, sizeof(CGAME_STEP_EXPECTED)) != 0) {
        log("[speed] CGame::Step prologue differs from build 35924 -- not installed\n");
        return false;
    }
    if (!CallsGetSpeed(base, RVA_SITE_PAUSE) || !CallsGetSpeed(base, RVA_SITE_COUNT)) {
        log("[speed] GameSim::Step no longer calls GetSpeed at %lx/%lx -- not installed\n",
            (unsigned long)RVA_SITE_PAUSE, (unsigned long)RVA_SITE_COUNT);
        return false;
    }
    if (memcmp((void*)(base + RVA_CGAME_SYNC), CGAME_SYNC_EXPECTED, sizeof(CGAME_SYNC_EXPECTED)) != 0) {
        log("[speed] CGame::Sync prologue differs from build 35924 -- not installed\n");
        return false;
    }
    g_retPause = base + RVA_SITE_PAUSE + 5;
    if (!InstallHook(base + RVA_GETSPEED, (void*)&GetSpeedDetour, sizeof(GETTER_EXPECTED), &g_getSpeedTramp)) {
        log("[speed] InstallHook FAILED on CGameTime::GetSpeed -- fractional speed unavailable\n");
        return false;
    }
    if (!InstallHook(base + RVA_CGAME_STEP, (void*)&CGameStepDetour, sizeof(CGAME_STEP_EXPECTED), &g_stepTramp)) {
        log("[speed] InstallHook FAILED on CGame::Step -- fractional speed unavailable (GetSpeed stays hooked, passing values through)\n");
        return false;
    }
    if (!InstallHook(base + RVA_CGAME_SYNC, (void*)&CGameSyncDetour, sizeof(CGAME_SYNC_EXPECTED), &g_syncTramp)) {
        log("[speed] InstallHook FAILED on CGame::Sync -- fractional speed unavailable (GetSpeed stays hooked, passing values through)\n");
        return false;
    }
    log("[speed] hooked CGameTime::GetSpeed (%lx), CGame::Sync (%lx) and CGame::Step (%lx): fractional speed scales the batch interval, imposed after Sync only; target off\n",
        (unsigned long)RVA_GETSPEED, (unsigned long)RVA_CGAME_SYNC, (unsigned long)RVA_CGAME_STEP);
    return true;
}

void SpeedHook_SetTarget(double t) { g_target = (t > 0.0 && t < 64.0) ? t : 0.0; }
double SpeedHook_Target()          { return g_target; }
uint64_t SpeedHook_Frames()        { return g_frames; }
int SpeedHook_LastCount()          { return g_lever; }
void SpeedHook_SetPin(long us)     { g_pinUs = (us > 0 && us <= INT32_MAX) ? int(us) : 0; }
void SpeedHook_Pace(long* engineBaseUs, int* lever)
{
    if (engineBaseUs) *engineBaseUs = g_engineBase.load(std::memory_order_relaxed);
    if (lever) *lever = g_lever.load(std::memory_order_relaxed);
}
