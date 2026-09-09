// Fractional game speed (2.5x, 0.5x, ...) by dithering the sim-step count.
//
// The engine has no such thing as speed 2.5: GameSim::Step (0x15aa00) asks
// CGameTime::GetSpeed (0x2877a0) for a whole number N and runs N sim
// iterations of one frame time each; 0 is pause. The two call sites in Step --
// 0x15aa30 (the pause test) and 0x15aae4 (the iteration count) -- are
// redirected to our handler, which returns a per-frame integer whose average
// is the target: 2.5 -> 2,3,2,3,...; 0.5 -> 0,1,0,1. Every iteration the
// engine runs is an ordinary sim step, so this is pacing only; lockstep peers
// agree by step, not by wall time, and cannot fork over it.
//
// Every other caller of the getter (the UI's speed buttons and clock) still
// sees the engine's own value: only the two sites inside Step are patched.
#pragma once
#include <cstdint>

typedef void (*SpeedLogFn)(const char* fmt, ...);

// Patch the two call sites. Verifies the bytes first; refuses on a mismatch.
bool SpeedHook_Install(SpeedLogFn log);

// 0 (or negative) = off: the engine's own speed passes through unchanged.
// Otherwise the target multiplier; the engine's pause (speed 0) still wins.
void SpeedHook_SetTarget(double target);
double SpeedHook_Target();

// Diagnostics: frames seen and the last value handed to the engine.
uint64_t SpeedHook_Frames();
int SpeedHook_LastCount();
