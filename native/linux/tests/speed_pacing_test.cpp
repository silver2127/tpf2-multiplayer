#include "../src/speedhook_linux.cpp"
#include <cassert>

int main() {
    alignas(16) unsigned char game[0x168]{}, data[0x1b0]{};
    *reinterpret_cast<uintptr_t*>(game + OFF_MDATA) = uintptr_t(data);
    auto& interval = *reinterpret_cast<int*>(data + OFF_GUIFRAMETIME);
    interval = 50000;
    CGameStepSeen(game);
    long base; int lever;
    SpeedHook_Pace(&base, &lever);
    assert(base == 50000 && lever == 0 && interval == 50000);
    SpeedHook_SetPin(200000); CGameStepSeen(game);
    assert(interval == 200000);
    g_lever = 2; SpeedHook_SetTarget(1.0); CGameStepSeen(game);
    assert(interval == 400000);
    // An engine estimate replaces the baseline without replacing the pin.
    interval = 80000; CGameStepSeen(game);
    SpeedHook_Pace(&base, &lever);
    assert(base == 80000 && lever == 2 && interval == 400000);
    SpeedHook_SetTarget(0); SpeedHook_SetPin(0); CGameStepSeen(game);
    assert(interval == 80000);
    SpeedHook_SetPin(-1); CGameStepSeen(game); assert(interval == 80000);
}
