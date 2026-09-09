// Hook on the buyVehicle command factory (RVA 0x9dca00, mapped by M2 and
// confirmed live at "exactly 1 hit per bus purchased").
//
// Why native: a vehicle sitting in a depot is NOT a world VEHICLE entity.
// getEntities({type="VEHICLE"}) returns an identical set before and after a
// purchase, the depot's VEHICLE_DEPOT component exposes only its carrier type,
// and transportVehicleSystem.getVehicles() is not callable. There is no Lua
// path to an in-depot vehicle, so the command factory is the only place a
// purchase is observable.
//
// This is currently a PROBE: it records the raw arguments so the vehicle-config
// structure behind rdx can be decoded. Replay stays in Lua.
#pragma once
#include <cstdint>

typedef void (*BuyLogFn)(const char* fmt, ...);

// outPath: file the hook appends "BUY depot=<id> mids=<a,b,c>" lines to. The
// Lua mod tails it and decides what to do -- keeping policy (echo suppression,
// depot id -> position) in Lua, with the native side as a dumb sensor.
bool BuyHook_Install(BuyLogFn log, const wchar_t* outPath);
bool BuyHook_InstallAt(uintptr_t target, BuyLogFn log);
uint64_t BuyHook_HitCount();
