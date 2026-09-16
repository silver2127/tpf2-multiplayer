// lobby_linux.h -- the lobby process, its events and the files it shares with
// the bridge and the game script, on Linux (menu_hook.cpp: "lobby.py: N-player
// host-relay lobby", StartLobby, LobbyThread, applyRoster, writeBridgeCtl,
// writeCompanyCfg, SyncStart / SyncPoll, "the public game list", OPEN LOGS).
//
// The panel (panel_linux.cpp) draws View and turns clicks and typing into the
// calls below. Everything slow -- starting and stopping the process, reading its
// events, writing files, looking at the save folder, fetching the public list,
// gathering logs -- runs on the lobby's own threads, so the SDL event filter and
// the render thread never wait on it. docs/linux/MENU_LOBBY.md has the details.
//
// LOCKS. The panel calls in here with its own lock held, so nothing below calls
// back into the panel on the caller's thread. The lobby's threads report through
// the StatusFn and DirtyFn given to Init, never while holding a lock of their
// own. Order: the panel's lock, then the lobby's. No MenuGame_* call is ever
// made with a panel or lobby lock held (the menu-game area may report through
// panel::SetStatus, or take locks of its own on the UI thread).
//
// THREADS. std::thread's constructor throws when the system refuses a thread;
// every one here is caught, logged and turned into a refusal, because these
// calls run inside SDL's C frames and on the game's own threads.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

using Tpf2mpLogFn = void (*)(const char* fmt, ...);

namespace lobby {

struct Config {
    std::string dataDir;          // trailing slash: tpf2_instance.txt, tpf2_bridge_ctl.txt, mp_company_cfg.txt
    std::string gameDir;          // trailing slash: <game>/netpunch, TPF2MP_GAME_DIR, the log archive
    std::string masterUrl;        // master_url: the public list; empty = no list and no --publish
    int  relayAutosaveMin = 2;    // relay_autosave_min: the relay leader's periodic upload (0 = never)
    int  shareMods = 0;           // share_mods: 0 ask, 1 always, 2 never
    bool autoload = true;         // autoload: START loads the shared save in-process
};

using StatusFn = void (*)(const char* utf8);   // the panel's status line
using DirtyFn  = void (*)();                   // View or the public list changed: draw again

// Once, from panel::Init, without the panel's lock (it asks MenuGame_SaveDir).
// Starts the lobby thread (and the public list's). A thread that cannot be
// started is logged: Start then refuses, or the list shows why it is empty.
void Init(const Config& cfg, Tpf2mpLogFn log, StatusFn status, DirtyFn dirty);

// ---- the lobby ----------------------------------------------------------------------
struct StartRequest {
    bool join = false;
    std::string code;        // join: the host's code, base32 only (it becomes an argument)
    std::string name;        // --name
    std::string password;    // --password, when not empty
    std::string lobbyName;   // host: --lobby-name
    bool pub = false;        // host: --public
};

// Checks the request and hands the launch to the lobby thread, which stops a
// lobby still running first. False, with the text for the status line, when
// nothing was started.
bool Start(const StartRequest& req, std::string* why);
void Leave();                                    // quit; after 1.5 s SIGTERM; after 2 s more SIGKILL

// "" when done; otherwise a text for the status line. A lobby whose program is
// missing, failed to start or has exited answers "The lobby is not running --
// ...", so the panel keeps the typed text and the status says why.
std::string SendChat(const std::string& text);
std::string StartGame();                         // host: share the newest save, then start
std::string SetPublic(bool on);                  // host: list or delist the running lobby
std::string AnswerMods(bool yes);                // YES / NO to the mods question
void CycleCompany(int rosterIndex);              // a company chip was clicked

// The room code for the clipboard. The panel copies it on the UI thread; the
// code is never drawn (menu_hook.cpp, "ROOM CODE, DELIBERATELY NOT RENDERED").
bool CopyCode(std::string* code);
bool AutoCopyPending();                          // lock-free: a new code wants the clipboard
bool TakeAutoCopy(std::string* code);
bool CapturesTyping();                           // lock-free: false once the shared save is placed

struct Player {
    std::string name;
    int company = 1;
    bool you = false, host = false;
};
struct View {
    bool haveCode = false;
    bool isHost = false;          // START GAME: the host, or a relay lobby's leader
    bool youAreHost = false;      // chips: the host may set anyone's company
    bool lobbyDone = false;
    std::string title;            // the lobby's name, from the roster
    std::string modsPrompt;       // a pending "download the mods?" question
    std::vector<Player> players;
    std::vector<std::string> chat;   // oldest first, at most 14
};
void Snapshot(View* v);

// ---- the public game list (HOST / JOIN page) ------------------------------------------
struct PubRow {
    std::string name, code, game, type, version;
    int players = 0, max = 8, age = 0;
    bool locked = false;
};
void PublicPoll();       // while the page is up: fetch when 10 s have passed
void PublicRefresh();    // REFRESH: fetch on the next poll
void PublicSnapshot(std::vector<PubRow>* rows, std::string* note);
bool PublicRow(int index, PubRow* row);

// OPEN LOGS: a copy of this run's logs (logarchive_linux.h), shown with xdg-open
// when the runtime has one, named in the status line. Returns at once: true when
// the gathering runs (or already did); false, logged, when its thread could not
// be started -- the caller sets the status then (it holds the panel's lock).
bool OpenLogs();

// ---- where the game is ------------------------------------------------------------------
// menu_hook.cpp tells "a game is running" from its own capture of CGameUI; here
// the lobby is told. Both are cheap and callable from any thread.
void OnMenuPage(int page);   // UI::CMenuUI::CreatePage(page): 2 is the title menu, 3+ cover it
void OnGameUiFrame();        // a CGameUI frame ran: a game is loaded

}  // namespace lobby
