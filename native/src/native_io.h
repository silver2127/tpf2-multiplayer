#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Build-specific native I/O. Requests are marshalled to the observed engine UI
// thread. Queueing a request never means that saving/loading has completed.
namespace NativeIo {
struct Event { std::string operation, step, detail; bool success; };
bool Initialize(uintptr_t imageBase, HMODULE module, const wchar_t* saveDirectory);
void ObserveMenu(uintptr_t menu);
bool Save(const std::string& operation, const std::string& basename);
bool Load(const std::string& operation, const std::string& basename);
// Owner must first stop Lua/network action producers. The native pause command
// forms a FIFO fence behind earlier commands and repeats if callbacks enqueue
// follow-up work. A successful "paused" event is the completion acknowledgement.
bool PauseAndDrain(const std::string& operation);
bool Poll(Event& event);
bool HasWorld();
bool Busy();
// The threads that do the engine's work, so the lobby can read whether a
// save or load is ALIVE (native_control.cpp reports their CPU time next to
// busy): the observed UI thread, and the world's command thread -- the one
// that executed the last pause/save completion, 0 until a completion has run
// on it and 0 again once that world is destroyed (a later world may run its
// commands on another thread).
void WorkThreads(DWORD& ui, DWORD& command);
// Suppress new game input before it can create a command/callback. Native MP
// progress controls use their separate input path. Escape remains available.
bool SetActionsHeld(bool held);
}
