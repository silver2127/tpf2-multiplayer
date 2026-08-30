// Per-tick hook on GameSim::Step -- the foothold for native state reads.
//
// Why this function: it runs once per simulation step on the Simulation Thread,
// which gives our code the same "on the sim thread at a defined point in the
// frame" guarantee that Lua's update() has. Reading ECS memory from any other
// thread races the sim (see docs/M7_NATIVE_STATE.md).
#pragma once
#include <cstdint>

typedef void (*SimLogFn)(const char* fmt, ...);

// Installs the detour. Verifies the bytes at the target match the build we
// reversed before patching anything -- on a game update the pattern will not
// match and we refuse rather than corrupting a random function.
bool SimHook_Install(SimLogFn log);

// Same, at an explicit address. Exists so the detour + trampoline + register
// preservation can be exercised offline against a stand-in function carrying
// the identical prologue, instead of first finding out inside the game.
bool SimHook_InstallAt(uintptr_t target, SimLogFn log);

// Ticks observed so far (0 until the hook fires, i.e. until a game is running).
uint64_t SimHook_TickCount();

// Last frameTime passed to Step, in millis.
uint64_t SimHook_LastFrameTime();
