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
// Every UI load path -- the title menu, the in-game menu, our own menu-frame
// autoload and Load() below -- converges on UI::CMenuUI::StartSavegame, which
// this file already detours. A second InstallHook at that address would steal
// the bytes the first one wrote, so an observer is registered instead of a
// second hook. It runs on the engine's UI thread right after the engine
// answered, with:
//   params    const LoadGameParams& (rdx at the call); +0x00 is the save NAME
//   accepted  what StartSavegame returned (false = the load did not start)
//   ours      the load was queued by Load() here, not asked for by the player
using StartObserver = void (*)(const void* params, bool accepted, bool ours);
void ObserveStart(StartObserver observer);
bool Save(const std::string& operation, const std::string& basename);
bool Load(const std::string& operation, const std::string& basename);
// Owner must first stop Lua/network action producers. The native pause command
// forms a FIFO fence behind earlier commands and repeats if callbacks enqueue
// follow-up work. A successful "paused" event is the completion acknowledgement.
bool PauseAndDrain(const std::string& operation);
bool Poll(Event& event);
bool HasWorld();
bool Busy();
// A load queued by Load() is in flight: from the request until the new world's
// CGameUI constructs, or the engine refused it. The engine builds the title
// menu (CreatePage 2) on the way from the old world to the loading screen, so
// the menu hook asks this before it reads a title menu as "the player left".
bool Loading();
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
