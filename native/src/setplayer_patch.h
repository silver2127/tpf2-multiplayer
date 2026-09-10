// game.interface.setPlayer for every entity kind: a 2-byte patch in the engine's
// Lua binding (setplayer_patch.cpp has the disassembly).
//
// Unpatched, setPlayer re-owns only a construction, a line or an asset group.
// Any other entity -- a track or road edge, a node, a signal, a station, a
// station group, a vehicle -- hits assert(false) at interface.cpp:2340: a crash
// dump, an error back to Lua, and the owner left as it was.
#pragma once

typedef void (*SetPlayerLogFn)(const char* fmt, ...);

// Verifies the bytes first; refuses on a mismatch and changes nothing.
bool SetPlayerPatch_Install(SetPlayerLogFn log);
