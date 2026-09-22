// menu_game_linux.h -- the parts of the title menu that act on the game itself,
// on Linux: the save folder and the shared save, AUTO-LOAD (starting that save
// from the menu's own per-frame update), HOT JOIN's forced autosave, and the
// automod settings.lua edit. The port of menu_hook.cpp's newestSave /
// stampNow / placeSaveNewest, AutoLoadTick, ForceAutosave and
// AutoEnableLockstepMod; the Linux sites are in docs/re/linux/MENU_GAME.md.
//
// THREADS. MenuGame_AutoEnableMod runs synchronously in tpf2_menu.so's ELF
// constructor. The rest may be called from any thread (the lobby's). None of
// these calls panel:: on the caller's thread, so they are safe with the panel's
// lock held; the verdicts of a load or a save reach panel::SetStatus from the
// game's UI thread or from a thread of this file's own.
#pragma once
#include <cstdint>
#include <string>
#include "panel.h"   // using Tpf2mpLogFn = void (*)(const char* fmt, ...);

// Once, after the caller has checked the build (gameBase = the image of build
// 35924). Verifies every site against build 35924's bytes, redirects the three
// Step calls of the frame gate and swaps CMenuUI's and CGameUI's per-frame
// update slots. True when both autoload and the forced autosave are installed;
// false = one or both stay off, and the log says which and why. Idempotent.
bool MenuGame_Install(uintptr_t gameBase, Tpf2mpLogFn log);

// Adds { "mp_lockstep", <n>, } to activeMods in settings.lua when the mod is
// installed and the entry is missing. Must run before the game reads
// settings.lua (after main() starts), so call it synchronously from the ELF
// constructor. `automod=0` in tpf2_menu_flags.txt beside tpf2_menu.so turns it
// off. Safe to call before MenuGame_Install; `log` must work that early.
void MenuGame_AutoEnableMod(Tpf2mpLogFn log);

// <steam>/userdata/<account>/1066780/local/save, without a trailing slash;
// empty when no Steam user data folder for the game is found.
std::string MenuGame_SaveDir();

// The newest *.sav in MenuGame_SaveDir() (full path). mp_shared.sav, our own
// placed copy, only when it is the only save there.
bool MenuGame_NewestSave(std::string* path);

// Copies `src` (a .sav path) with its <src>.lua and <src minus .sav>.jpg into
// the save folder as mp_shared.sav / .sav.lua / .jpg, stamped newest. A
// companion the source lacks is removed from the destination. `placedName` is
// the save's name without extension ("mp_shared"). False, placing nothing,
// when the .sav itself cannot be placed.
bool MenuGame_PlaceSharedSave(const std::string& src, std::string* placedName);

// Start the placed save on the next title-menu frame (taken by CMenuUI's
// per-frame update on the UI thread). A game already running, a refusal, or
// autoload not installed ends in the status line's "open LOAD GAME and pick
// <name>" instead.
void MenuGame_RequestAutoload(const std::string& placedName);
bool MenuGame_Loading();
void MenuGame_RequestModRefresh();

// HOT JOIN: ask the running game for one of its own autosaves on the next
// frame. False when unavailable (not installed, no game running, the game's
// autosave interval is 0); true means queued. The save shows up as a new
// autosave_*.sav in MenuGame_SaveDir().
bool MenuGame_ForceAutosave();

// Accepted vanilla UI loads only; called on the UI thread. The receiver must
// copy the name and enqueue work, never load another world in this callback.
using MenuGameLoadObserver = void (*)(const char* name);
void MenuGame_ObserveLoads(MenuGameLoadObserver observer);

// Capture the menu from the verified CreatePage hook; safe reads, no game calls.
void MenuGame_ObserveMenu(void* menu);
// -1 unavailable; otherwise floored 0..100 from the verified ProgressMonitor.
int MenuGame_LoadPercent();
