// panel.h -- the Multiplayer panel on Linux, between the title menu hooks
// (menu_linux.cpp), the Vulkan overlay that draws it (overlay_vk_linux.cpp) and
// the SDL event filter that feeds it input (panel_linux.cpp).
//
// menu_hook.cpp keeps all of this in one file with Win32 threads and polling;
// here the panel owns its state behind one mutex, because its callers run on
// different threads: the UI thread (the menu hooks and SDL's event filter), the
// render thread (the overlay) and the lobby's threads (lobby_linux.h).
#pragma once
#include <cstdint>

using Tpf2mpLogFn = void (*)(const char* fmt, ...);

namespace panel {

// Fonts from the game folder, the saved names, the flags file, then the lobby
// (lobby::Init). `dataDir` has a trailing slash; `libDir` is where
// tpf2_menu.so lives (its flags file). The fonts and lobby::Init run without
// the panel's lock, so Visible() never waits on them; the panel shows only
// once Init has returned.
void Init(const char* dataDir, const char* gameDir, const char* libDir, Tpf2mpLogFn log);

void Open();                 // the Multiplayer entry was clicked
void OnMenuPage(int page);   // UI::CMenuUI::CreatePage(page) ran
bool Visible();

// Render thread, once per presented frame while Visible(). Lays the panel out
// for a screen of screenW x screenH, re-renders layer:: when it changed (or the
// caret is due to blink), and returns the panel's rect on the screen.
// `changed` reports a layer redraw so the overlay can reuse its composition.
bool Frame(int screenW, int screenH, int* x, int* y, int* w, int* h, bool* changed);

// The largest panel for this screen height, for sizing the overlay's images.
void MaxSize(int screenW, int screenH, int* w, int* h);

// The button under the cursor, in panel pixels, for the hover wash.
bool Hover(int* x, int* y, int* w, int* h, bool* pressed);

// Install the SDL event filter. Called from the render thread on the first
// presented frame, when SDL and the window certainly exist. Idempotent.
void InstallInput();
bool SetActionsHeld(bool held);

// The status line under the panel (and the lobby page's). Any thread; takes the
// panel's lock, so never from inside a panel or lobby call. For the menu-game
// area: the autoload's own verdicts ("open LOAD GAME and pick mp_shared").
void SetStatus(const char* utf8);

// A CGameUI frame ran: a game is loaded. Hot join and the relay leader's
// periodic upload need it (menu_hook.cpp captured CGameUI's 'this' for the
// same test). Any thread, lock-free; meant for the menu-game area's CGameUI
// update hook.
void OnGameUiFrame();

}  // namespace panel

// overlay_vk_linux.cpp: point the game's device dispatcher setup at ours.
bool OverlayInstall(uintptr_t gameBase, Tpf2mpLogFn log);
