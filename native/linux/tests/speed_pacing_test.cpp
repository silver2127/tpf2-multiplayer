#include "../src/speedhook_linux.cpp"
#include <cassert>

// The override is imposed right after CGame::Sync, which always writes the
// engine's fresh estimate first: each step below is "Sync wrote X, then
// ImposeInterval ran". Nothing writes the interval between two Syncs, so it is
// constant for the whole batch the renderer interpolates (ShipFoamRenderer's
// `startAge >= 0` assert, 2026-09-22).
static void SyncWrote(unsigned char* game, int& interval, int estimate)
{
    interval = estimate;
    ImposeInterval(game);
}

static int estimate = 50000;
static bool syncOk = true;
static const void* callback;
static bool FakeSync(void* game, const void* fn) {
    assert(fn == callback);
    if (syncOk) {
        auto data = *reinterpret_cast<uintptr_t*>(static_cast<unsigned char*>(game) + OFF_MDATA);
        *reinterpret_cast<int*>(data + OFF_GUIFRAMETIME) = estimate;
    }
    return syncOk;
}
static void FakeStep(void*, long long dt, const void* fn) {
    assert(dt == 123 && fn == callback);
}

int main() {
    alignas(16) unsigned char game[0x168]{}, data[0x1b0]{};
    *reinterpret_cast<uintptr_t*>(game + OFF_MDATA) = uintptr_t(data);
    auto& interval = *reinterpret_cast<int*>(data + OFF_GUIFRAMETIME);
    SyncWrote(game, interval, 50000);
    long base; int lever;
    SpeedHook_Pace(&base, &lever);
    assert(base == 50000 && lever == 0 && interval == 50000);
    SpeedHook_SetPin(200000); SyncWrote(game, interval, 50000);
    assert(interval == 200000);
    SpeedHook_Pace(&base, &lever);
    assert(base == 50000);                     // the engine's estimate, not the pin we imposed
    g_lever = 2; SpeedHook_SetTarget(1.0); SyncWrote(game, interval, 50000);
    assert(interval == 400000);
    // An engine estimate replaces the baseline without replacing the pin.
    SyncWrote(game, interval, 80000);
    SpeedHook_Pace(&base, &lever);
    assert(base == 80000 && lever == 2 && interval == 400000);
    SpeedHook_SetTarget(0); SpeedHook_SetPin(0); SyncWrote(game, interval, 80000);
    assert(interval == 80000);
    SpeedHook_SetPin(-1); SyncWrote(game, interval, 80000); assert(interval == 80000);
    // A pin or target change takes effect at the NEXT Sync, never in between:
    // nothing else in this file writes the interval.
    SpeedHook_SetPin(300000);
    assert(interval == 80000);
    SyncWrote(game, interval, 80000);
    assert(interval == 300000);
    g_syncTramp = reinterpret_cast<void*>(FakeSync);
    g_stepTramp = reinterpret_cast<void*>(FakeStep);
    callback = game;
    SpeedHook_SetPin(400000);
    for (int i=0; i<10; ++i) CGameStepDetour(game, 123, callback);
    assert(interval == 300000); // no per-frame write, including after controls change
    syncOk = false;
    assert(!CGameSyncDetour(game, callback) && interval == 300000);
    SpeedHook_Pace(&base, &lever);
    assert(base == 80000); // failed Sync does not publish an estimate
    syncOk = true;
    assert(CGameSyncDetour(game, callback) && interval == 400000);
    SpeedHook_Pace(&base, &lever);
    assert(base == 50000);
    // A fresh estimate equal to our previous override must still become the baseline.
    estimate = 400000;
    SpeedHook_SetPin(0);
    assert(CGameSyncDetour(game, callback) && interval == 400000);
    SpeedHook_Pace(&base, &lever);
    assert(base == 400000);
}
