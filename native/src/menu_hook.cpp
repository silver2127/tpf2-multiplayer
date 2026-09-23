// menu_hook.cpp -- native "Multiplayer" button in the TF2 title menu.
//
// Injected while the game sits at the main menu. Step 1 (this file, minimal):
// detour UI::CMenuUI::CreatePage(this, page) at RVA 0x663370, log every call,
// and call through to the original. This proves the hook point and that the
// menu survives the detour BEFORE any widget construction is attempted.
//
// Prologue at 0x663370 (verified from the exe):
//   40 55 56 57 41 54 41 55 41 56 41 57   push rbp/rsi/rdi/r12/r13/r14/r15
//   48 8d ac 24 20 fe ff ff               lea rbp,[rsp-0x1e0]      -> ends 0x14
//   48 81 ec ...                          sub rsp,imm
// All position-independent (pushes + rsp-relative lea, no RIP-relative), so a
// 20-byte (0x14) steal is a safe trampoline.
#include <share.h>
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iphlpapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cwctype>
#define VK_NO_PROTOTYPES
#include "../third_party/vk/vulkan_core.h"
#include "hook.h"
#include "native_io.h"
#include "native_control.h"
#include "gl_overlay.h"
static HMODULE g_nativeModule = nullptr;
#include "datadir.h"
#include "logarchive.h"

static const uintptr_t RVA_CREATEPAGE = 0x663370;
static const int       STEAL_CREATEPAGE = 20;
// str_assign 83270(std::string* dest, const char* src, size_t len): MSVC
// basic_string::assign. dest is the 32-byte SSO struct {char buf[16]; size_t
// size; size_t capacity}; reads capacity at dest+0x18, uses the inline buffer
// when len fits. An empty string has size=0, capacity=15.
static const uintptr_t RVA_STR_ASSIGN = 0x83270;
typedef void* (*StrAssignFn)(void* dest, const char* src, size_t len);
static StrAssignFn g_strAssign = nullptr;

// action-context factory: FUN_14221c930(outBuf, const char* key) -> outBuf.
// Builds the labelled/localized context a menu button is created from.
static const uintptr_t RVA_ACTION_CTX = 0x221c930;
typedef void* (*ActionCtxFn)(void* outBuf, const char* key);
static ActionCtxFn g_actionCtx = nullptr;

// Button widget factory: FUN_1407c5d30(ctx, std::string* iconA, std::string* iconB)
// -> Button*. Sets text via the button vtable +0xd8.
static const uintptr_t RVA_BTN = 0x7c5d30;
typedef void* (*BtnFn)(void* ctx, void* strA, void* strB);
static BtnFn g_btn = nullptr;

// --- insertion primitives (see docs/re/GAME_LOOP_AND_UI.md, "Title menu") ---
static const uintptr_t RVA_MAINBUILD = 0x667bc0;   // main-page builder (hook here)
static const int       STEAL_MAINBUILD = 14;       // mov rax,rsp + 7 pushes
static const uintptr_t RVA_ADD   = 0x22518f0;      // 2518f0(button,&{container},&binding)
static const uintptr_t RVA_CLEAN = 0x2357910;      // 357910(&{container})
typedef void* (*AddFn)(void* button, void* pContainer, void* binding);
typedef void  (*CleanFn)(void* pContainer);
static AddFn   g_add   = nullptr;
static CleanFn g_clean = nullptr;

typedef void (*MainBuildFn)(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4);
static MainBuildFn g_origMainBuild = nullptr;

static uintptr_t g_base = 0;

// ---------------- portable runtime path discovery ----------------
// Nothing here is hardcoded to a machine: our own files are found relative to
// THIS dll, and the game's paths are discovered from the Steam registry. Every
// resolver FALLS BACK to the original dev-box value, so the local rig keeps
// working even when discovery finds nothing.
static wchar_t g_saveDirW[600] = L"";   // resolved TF2 userdata save folder
static wchar_t g_netDirW[600]  = L"";   // resolved netpunch/lobby working dir
static wchar_t g_dataDirW[MAX_PATH] = L"";   // runtime data dir shared with the bridge (trailing '\')

// Game-relay ports (see lobby.py --game-relay-port). The bridge sends its
// lockstep frames to 127.0.0.1:<relay port> and lobby.py forwards them over the
// punched socket. Host and joiner use DIFFERENT ports so two instances on one
// machine can both run.
static const int GAME_RELAY_PORT_HOST = 7773;
static const int GAME_RELAY_PORT_JOIN = 7774;
// The relay port this instance actually uses. The host's is fixed (7773). A
// joiner's used to be fixed too (7774), which is fine for one joiner per
// machine and silently mutes the second: Sandboxie does not virtualise the
// network stack, so two boxed joiners on one PC both tried to bind 7774 and the
// loser's lobby ran without a relay. Now a joiner takes the first free port
// from 7774 upward, decided once when the lobby is launched, and the bridge
// ctl is written with that same number (2026-09-01, three-instance test rig).
static int g_relayPort = 0;
static bool udpPortInUse(int port)
{
    ULONG sz = 0;
    if (GetUdpTable(nullptr, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER || sz == 0) return false;
    MIB_UDPTABLE* t = (MIB_UDPTABLE*)malloc(sz);
    if (!t) return false;
    bool used = false;
    if (GetUdpTable(t, &sz, FALSE) == NO_ERROR) {
        for (DWORD i = 0; i < t->dwNumEntries; i++)
            if ((int)((((t->table[i].dwLocalPort) & 0xff) << 8) | ((t->table[i].dwLocalPort >> 8) & 0xff)) == port) { used = true; break; }
    }
    free(t);
    return used;
}
static int pickRelayPort(bool join)
{
    if (!join) return GAME_RELAY_PORT_HOST;
    for (int p = GAME_RELAY_PORT_JOIN; p < GAME_RELAY_PORT_JOIN + 32; p++)
        if (!udpPortInUse(p)) return p;
    return GAME_RELAY_PORT_JOIN;
}
static int relayPortFor(bool isHost)
{
    if (g_relayPort) return g_relayPort;
    return isHost ? GAME_RELAY_PORT_HOST : GAME_RELAY_PORT_JOIN;
}
static const int BRIDGE_PORT_DEFAULT  = 7771;   // if tpf2_instance.txt has no port= line

// Directory holding this dll (trailing backslash). Logs + siblings live here.
static const char* ourDirA()
{
    static char d[MAX_PATH] = "";
    if (!d[0]) {
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&ourDirA, &hm);
        if (!GetModuleFileNameA(hm, d, MAX_PATH)) d[0] = 0;
        char* s = strrchr(d, '\\'); if (s) s[1] = 0; else strcpy_s(d, ".\\");
    }
    return d;
}
static const wchar_t* ourDirW()
{
    static wchar_t d[MAX_PATH] = L"";
    if (!d[0]) {
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&ourDirW, &hm);
        if (!GetModuleFileNameW(hm, d, MAX_PATH)) d[0] = 0;
        wchar_t* s = wcsrchr(d, L'\\'); if (s) s[1] = 0; else wcscpy_s(d, L".\\");
    }
    return d;
}

// Steam install dir from the registry (HKCU SteamPath, else HKLM InstallPath).
static bool steamPath(wchar_t* out, int cch)
{
    DWORD sz = (DWORD)(cch * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, out, &sz) == ERROR_SUCCESS && out[0]) {
        for (wchar_t* p = out; *p; p++) if (*p == L'/') *p = L'\\';   // registry uses '/'
        return true;
    }
    sz = (DWORD)(cch * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath",
                     RRF_RT_REG_SZ, nullptr, out, &sz) == ERROR_SUCCESS && out[0]) return true;
    return false;
}

static void Log(const char* fmt, ...);

// <steam>\userdata\<accountid>\1066780\local\save -- enumerate accounts and pick the
// best one. A game that has NEVER SAVED has no local\save yet: on 2026-09-15 a first-time
// tester's DLL found no account with one, fell through to the userdata\0 fallback and
// every shared save failed to place with ERROR_PATH_NOT_FOUND -- "can't load the save".
// So rank: an account with local\save (newest wins), else one with the 1066780 folder
// Steam makes on first launch, else any numeric account dir. placeSaveNewest creates
// whatever folder it is handed. 1066780 = TF2 appid.
static void resolveSaveDir(wchar_t* out, int cch)
{
    wchar_t steam[MAX_PATH];
    if (steamPath(steam, MAX_PATH)) {
        wchar_t pat[MAX_PATH]; _snwprintf_s(pat, _TRUNCATE, L"%s\\userdata\\*", steam);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        wchar_t best[MAX_PATH] = L""; ULONGLONG bestT = 0; int bestTier = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                bool numeric = fd.cFileName[0] != 0;
                for (const wchar_t* c = fd.cFileName; *c; c++) if (*c < L'0' || *c > L'9') { numeric = false; break; }
                if (!numeric) continue;
                wchar_t app[MAX_PATH], save[MAX_PATH];
                _snwprintf_s(app, _TRUNCATE, L"%s\\userdata\\%s\\1066780", steam, fd.cFileName);
                _snwprintf_s(save, _TRUNCATE, L"%s\\local\\save", app);
                int tier = 1;
                DWORD a = GetFileAttributesW(app);
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                    tier = 2;
                    a = GetFileAttributesW(save);
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) tier = 3;
                }
                ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (tier > bestTier || (tier == bestTier && t >= bestT)) { bestTier = tier; bestT = t; wcscpy_s(best, save); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (best[0]) {
            if (bestTier < 3) Log("[menu] save folder %ls does not exist yet (%s) -- it is created when a shared save is placed\n",
                                  best, bestTier == 2 ? "this game has never saved" : "no 1066780 folder in the account yet");
            wcscpy_s(out, cch, best); return;
        }
        Log("[menu] no Steam account folder under %ls\\userdata -- a shared save cannot be placed\n", steam);
    } else {
        Log("[menu] Steam not found in the registry -- a shared save cannot be placed\n");
    }
    // no usable path at all: a generic one that CopyFileW will fail on, logged with dst by the placer
    wcscpy_s(out, cch, L"C:\\Program Files (x86)\\Steam\\userdata\\0\\1066780\\local\\save");
}

// True if `dir` holds a runnable lobby: the frozen netpunch.exe OR lobby.py.
static bool netDirUsable(const wchar_t* dir)
{
    static const wchar_t* const probes[] = { L"netpunch.exe", L"lobby.py" };
    for (const wchar_t* nm : probes) {
        wchar_t probe[MAX_PATH]; _snwprintf_s(probe, _TRUNCATE, L"%s\\%s", dir, nm);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

// netpunch working dir:
//   %LOCALAPPDATA%\tpf2mp\netpunch   when it holds lobby.py or the frozen netpunch.exe
//   <dir of this dll>\netpunch        otherwise: where the installer drops netpunch.exe,
//                                     so a lobby that cannot start names that folder
static void resolveNetDir(wchar_t* out, int cch)
{
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        wchar_t cand[MAX_PATH]; _snwprintf_s(cand, _TRUNCATE, L"%s\\tpf2mp\\netpunch", la);
        if (netDirUsable(cand)) { wcscpy_s(out, cch, cand); return; }
    }
    _snwprintf_s(out, cch, _TRUNCATE, L"%snetpunch", ourDirW());   // ourDirW has a trailing '\'
}

static void Log(const char* fmt, ...)
{
    char line[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%stpf2_menu.log", ourDirA());
    FILE* f = fopen(p, "a");
    if (f) { fputs(line, f); fclose(f); }
    OutputDebugStringA(line);
}

typedef void (*CreatePageFn)(uint64_t thisp, int page);
static CreatePageFn g_origCreatePage = nullptr;

// ---------------- Vulkan in-frame overlay (option 2) ----------------
// The game presents borderless direct-flip; no external window can draw over the
// menu, so we hook the game's OWN present and render into its swapchain. gdpa is
// the clean hook point (15-byte steal); we wrap the fns we need as the game
// resolves them, then draw a clear-attachment button rect each frame.
static PFN_vkGetDeviceProcAddr g_origGdpa    = nullptr;
static PFN_vkQueuePresentKHR   g_realPresent = nullptr;
// NO RENDERING (2026-09-18, user: "can we disable the renderer"): a dedicated
// server has no GPU; its Vulkan is lavapipe, which rasterizes on the CPU at
// vkQueueSubmit -- six llvmpipe threads at 180% for a town nobody looks at, and
// the sim, which advances per frame, crawled. With dedicated_render=0 (the
// default in dedicated mode) every submit goes through EMPTY: the same fences
// and semaphores are signalled, no command buffer runs; query results read as
// zero so nothing waits on a query that never executed; the panel is not drawn.
// The engine's frame loop, the swapchain and the present are untouched.
static PFN_vkQueueSubmit        g_origSubmit  = nullptr;
static PFN_vkGetQueryPoolResults g_origQueryResults = nullptr;
// ... and NO WINDOW SYSTEM either (same evening): with the command buffers gone
// the server still presented through lavapipe's X11 swapchain at 30 frames a
// second, and the engine steps the simulation per frame as if there were 60 --
// the world ran at exactly half of every speed asked (0.45 units/s at 1x, 0.9 at
// 2x; a PC does 0.9 at 1x). With dedicated_render=0 the swapchain is never
// acquired from or presented to: acquire hands out image indices in turn and
// signals the semaphore/fence through an empty submit, present consumes its
// wait semaphores the same way and returns, and the frame loop is paced here to
// NORENDER_FPS instead of by the X server.
static PFN_vkAcquireNextImageKHR g_origAcquire = nullptr;
static PFN_vkAcquireNextImage2KHR g_origAcquire2 = nullptr;
static PFN_vkGetSwapchainImagesKHR g_origGetScImages = nullptr;
static volatile LONG g_noWsi = 0;            // dedicated_nowsi=1 (opt-in, needs render=0): acquire/present answered here -- crashed the engine on the VPS 2026-09-18 22:42, under study
static volatile LONG g_nullScCount = 0;      // images in the current swapchain (for the round robin)
static volatile LONG g_nullScNext = 0;
static const double NORENDER_FPS = 60.0;     // the frame rate the engine's per-frame stepping assumes (the DLL-answered present)
// dedicated_fps=<n> (default 30): with render=0 and the window system still in
// place, the present is paced here to this many frames a second. The engine
// needs only 5 batches a second; every headless frame beyond that is scene
// prep for nobody on the thread that hands the sim its batches (2026-09-18).
static int g_flagDedFps = 30;
static int g_flagDedPinBatch = 1;             // dedicated_pin_batch=0|1: 1 (default) pins the engine's batch interval at 200 ms (tpf2_bridge_mp.dll speedhook)
static void NullSignal(VkQueue q, VkSemaphore signalSem, VkFence fence, uint32_t waitCount, const VkSemaphore* waitSems);
static void NullPace(double fps);
static volatile LONG g_noRender = 0;
static volatile LONG g_noRenderSubmits = 0, g_noRenderCmdBufs = 0;
static bool NoRender() { return InterlockedCompareExchange(&g_noRender, 0, 0) != 0; }
static bool NoWsi() { return InterlockedCompareExchange(&g_noWsi, 0, 0) != 0; }
static PFN_vkGetDeviceQueue    g_origGetQueue = nullptr;
static PFN_vkCreateSwapchainKHR g_origCreateSc = nullptr;
static VkDevice   g_dev   = VK_NULL_HANDLE;
static uint32_t   g_qfam  = 0; static bool g_qfamKnown = false;
static VkQueue    g_qFromFam = VK_NULL_HANDLE;
static VkFormat   g_scFormat = VK_FORMAT_UNDEFINED;
static VkExtent2D g_scExtent = { 0, 0 };
static volatile LONG g_presentCount = 0;

// resolved device fns (lazy, via g_origGdpa)
static PFN_vkGetSwapchainImagesKHR pGetImages = nullptr;
static PFN_vkCreateImageView   pCreateView   = nullptr;
static PFN_vkCreateRenderPass  pCreateRP     = nullptr;
static PFN_vkCreateFramebuffer pCreateFB     = nullptr;
static PFN_vkCreateCommandPool pCreatePool   = nullptr;
static PFN_vkAllocateCommandBuffers pAllocCB = nullptr;
static PFN_vkBeginCommandBuffer pBeginCB      = nullptr;
static PFN_vkCmdBeginRenderPass pCmdBeginRP   = nullptr;
static PFN_vkCmdClearAttachments pCmdClear    = nullptr;
static PFN_vkCmdEndRenderPass   pCmdEndRP     = nullptr;
static PFN_vkEndCommandBuffer   pEndCB        = nullptr;
static PFN_vkQueueSubmit        pSubmit       = nullptr;
static PFN_vkCreateFence        pCreateFence  = nullptr;
static PFN_vkWaitForFences      pWaitFences   = nullptr;
static PFN_vkResetFences        pResetFences  = nullptr;
static PFN_vkResetCommandBuffer pResetCB      = nullptr;
// image-copy path (TRANSFER_DST swapchain): GDI-render the panel -> host image -> copy
static PFN_vkCreateImage        pCreateImage  = nullptr;
static PFN_vkGetImageMemoryRequirements pImgMemReq = nullptr;
static PFN_vkAllocateMemory     pAllocMem     = nullptr;
static PFN_vkBindImageMemory    pBindImgMem   = nullptr;
static PFN_vkMapMemory          pMapMem       = nullptr;
static PFN_vkGetImageSubresourceLayout pImgSubLayout = nullptr;
static PFN_vkCmdCopyImage       pCmdCopyImage = nullptr;
static PFN_vkCmdPipelineBarrier pCmdBarrier   = nullptr;
static PFN_vkFlushMappedMemoryRanges pFlush   = nullptr;
static PFN_vkInvalidateMappedMemoryRanges pInvalidate = nullptr;

// panel: a host-visible linear image holding the GDI-rendered UI, copied onto
// the swapchain each frame.
static VkImage        g_panelImg = VK_NULL_HANDLE;
static VkDeviceMemory g_panelMem = VK_NULL_HANDLE;
static void*          g_panelPtr = nullptr;   // mapped
static size_t         g_panelPitch = 0;       // row bytes
static int            g_panelW = 780, g_panelH = 580;   // image alloc = max (lobby)
static int            g_copyW = 300, g_copyH = 60;        // region actually shown/copied
static bool           g_panelBuilt = false;
static char           g_code[128] = "";                   // host/own code to display
static volatile LONG  g_haveCode = 0;
static wchar_t        g_startSaveW[600] = L"";             // host: the .sav it chose to share
// Picker state belongs to the presentation thread; g_startSaveW is the transfer snapshot.
struct LobbySave { std::wstring path, name; FILETIME modified; };
static std::vector<LobbySave> g_lobbySaves;
static std::wstring g_selectedSave;
static bool g_savePicker = false;
static volatile LONG g_saveStartPending = 0;
static void SaveStartStatus(const char* state, const char* detail)
{
    // A refusal permits choosing another world; ordinary progress must keep the snapshot fixed.
    if (!strcmp(state,"failed") || strstr(detail,"no players to share with")==detail || strstr(detail,"Not shared:")==detail)
        InterlockedExchange(&g_saveStartPending,0);
}
static int g_savePage = 0;
static const int SAVE_ROWS = 8;
static void RefreshLobbySaves();
static volatile LONG  g_panelDirty = 1;       // re-render the GDI content
static int            g_panelX = 0, g_panelY = 0;   // top-left on the swapchain

// render resources
static bool g_rInit = false, g_rFail = false;
static VkRenderPass  g_rp = VK_NULL_HANDLE;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkFence       g_fence = VK_NULL_HANDLE;
static VkImage       g_scImages[8] = {};
static VkImageView   g_scViews[8]  = {};
static VkFramebuffer g_scFbs[8]    = {};
static VkCommandBuffer g_cmd[8]    = {};
static uint32_t      g_scImgCount = 0;
static VkSwapchainKHR g_theSc = VK_NULL_HANDLE;

static volatile LONG g_ingameOverlay = 0;
static bool WorldLoaded();
static bool LobbyRunning();
static void SyncStart(const char* why);
static void PollLobbyOpen();
static void StageTick();
static void MarkSaveShared();
static volatile LONG g_showOverlay = 0;   // set by the CreatePage detour (page==2)
// LEAVING THE WORLD LEAVES THE LOBBY (2026-09-16). Set by the CreatePage detour
// when the title menu comes up while a CGameUI was still known (a world was up
// a frame ago) and a lobby runs; consumed on the present thread (myPresent),
// where LEAVE's teardown already runs, so the menu build is never stalled by
// the 1.5 s the lobby gets to quit. A joiner waiting at the title menu for a
// save never had a CGameUI, so it is not affected; a world switch (a start with
// switch=1) and a resync load happen in place and never build the title menu.
static volatile LONG g_leaveOnMenu = 0;
static HWND g_gameWnd = nullptr;
static BOOL CALLBACK FindGameWnd(HWND h, LPARAM lp);
static void StartLobby(int join);     // host=0 / join=1 -> spawns lobby.py
static void DedicatedTick();          // dedicated server mode (tpf2_menu_flags.txt dedicated=1): host, load, keep up
static void LeaveLobby();
static DWORD WINAPI KbHookThread(LPVOID);
static bool LobbySend(const char* jsonLine);
static std::string originLetterFor(const std::string& name);
static void ClipboardSet(const char* utf8);
static bool ClipboardGet(char* out, int outsz);
static bool newestSave(wchar_t* out, int cch);
static bool doStartLoad(const wchar_t* srcSav);
// Lobby lifecycle flags (all cleared in StartLobby):
//  g_lobbyReady -- set when the FIRST event line is read from lobby_out.jsonl.
//    lobby.py truncates lobby_in.jsonl at startup, so a command appended before
//    that first event would be silently lost; START GAME / chat wait for it.
//  g_saveReady  -- joiner: the 'save_ready' event arrived this session, i.e. the
//    incoming_save.* files are complete and a 'start' with save=true may load them.
static volatile LONG g_lobbyReady = 0;
static volatile LONG g_saveReady  = 0;
// WORLD SWITCH: the host loads ANOTHER world while a session is running (the
// game's own LOAD GAME, in the title menu or in game, or NEW GAME/CONTINUE).
// Everyone else has to leave the world they are playing and load that one, so
// the save goes out with "switch":true and a joiner takes it even though it
// already started. See OnStartSavegame (the load the player asked for) and
// PollWorldGen (the loads that carry no save name).
//  g_selfLoad         -- our own AutoLoadCall is inside StartSavegame: never share it
//  g_hostLoadedItself -- the host shared the save its OWN menu is loading; the
//                        'start' that comes back must not load it a second time
//  g_sessionStarted   -- the lobby has started this session (a start event arrived)
//  g_worldGen         -- last value read from the mod's tpf2mp_world_gen.txt
//  g_worldGenHold     -- absorb the next change: it is a load WE caused
//  g_switchShare      -- the save SyncPoll is about to share is a world switch
static volatile LONG g_selfLoad = 0, g_hostLoadedItself = 0, g_lastPage = -1;
//  g_hostMenuLoad -- the host's own mid-session LOAD GAME is in flight (a world
//  switch it shared); the title page the engine builds on the way is not a
//  "left the world" (consumed by MyCreatePage, one shot).
static volatile LONG g_hostMenuLoad = 0;
static volatile LONG g_sessionStarted = 0;
static volatile LONG g_worldGenHold = 0, g_switchShare = 0;
static char g_worldGen[160] = "";
static void PollWorldGen();
static CRITICAL_SECTION g_lobbyCs; static bool g_lobbyCsInit = false;   // guards g_lobbyProc handle use vs close

template <class T> static T rget(const char* n) { return (T)g_origGdpa(g_dev, n); }

static bool InitRender(VkSwapchainKHR sc)
{
    if (g_rFail) return false;
    if (!g_dev || !g_qfamKnown || g_scFormat == VK_FORMAT_UNDEFINED) return false;
    Log("[menu] InitRender begin dev=%p origGdpa=%p fmt=%d fam=%u\n", g_dev, (void*)g_origGdpa, (int)g_scFormat, g_qfam);
    pGetImages = rget<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    Log("[menu] rget pGetImages=%p\n", (void*)pGetImages);
    pCreateView = rget<PFN_vkCreateImageView>("vkCreateImageView");
    pCreateRP  = rget<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    pCreateFB  = rget<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    pCreatePool= rget<PFN_vkCreateCommandPool>("vkCreateCommandPool");
    pAllocCB   = rget<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    pBeginCB   = rget<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    pCmdBeginRP= rget<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    pCmdClear  = rget<PFN_vkCmdClearAttachments>("vkCmdClearAttachments");
    pCmdEndRP  = rget<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    pEndCB     = rget<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    pSubmit    = rget<PFN_vkQueueSubmit>("vkQueueSubmit");
    pCreateFence = rget<PFN_vkCreateFence>("vkCreateFence");
    pWaitFences = rget<PFN_vkWaitForFences>("vkWaitForFences");
    pResetFences= rget<PFN_vkResetFences>("vkResetFences");
    pResetCB   = rget<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
    pCreateImage = rget<PFN_vkCreateImage>("vkCreateImage");
    pImgMemReq   = rget<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    pAllocMem    = rget<PFN_vkAllocateMemory>("vkAllocateMemory");
    pBindImgMem  = rget<PFN_vkBindImageMemory>("vkBindImageMemory");
    pMapMem      = rget<PFN_vkMapMemory>("vkMapMemory");
    pImgSubLayout= rget<PFN_vkGetImageSubresourceLayout>("vkGetImageSubresourceLayout");
    pCmdCopyImage= rget<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    pCmdBarrier  = rget<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pFlush       = rget<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges");
    pInvalidate  = rget<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges");
    void* need[] = { (void*)pGetImages,(void*)pCreateView,(void*)pCreateRP,(void*)pCreateFB,
                     (void*)pCreatePool,(void*)pAllocCB,(void*)pBeginCB,(void*)pCmdBeginRP,
                     (void*)pCmdClear,(void*)pCmdEndRP,(void*)pEndCB,(void*)pSubmit,
                     (void*)pCreateFence,(void*)pWaitFences,(void*)pResetFences,(void*)pResetCB };
    for (int i = 0; i < 16; i++) if (!need[i]) { g_rFail = true; Log("[menu] vk: fn %d is NULL -- resolve failed\n", i); return false; }
    Log("[menu] vk: all 16 fns resolved, building...\n");

    uint32_t n = 0; pGetImages(g_dev, sc, &n, nullptr);
    Log("[menu] vk: swapchain image count=%u\n", n);
    if (n == 0 || n > 8) { g_rFail = true; Log("[menu] vk: bad image count %u\n", n); return false; }
    pGetImages(g_dev, sc, &n, g_scImages); g_scImgCount = n;

    // render pass: LOAD (keep the game frame), color attachment in PRESENT_SRC
    VkAttachmentDescription att = {};
    att.format = g_scFormat; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    if (pCreateRP(g_dev, &rpci, nullptr, &g_rp) != VK_SUCCESS) { g_rFail = true; Log("[menu] vk: renderpass failed\n"); return false; }

    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = g_scImages[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = g_scFormat;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; iv.subresourceRange.levelCount = 1; iv.subresourceRange.layerCount = 1;
        if (pCreateView(g_dev, &iv, nullptr, &g_scViews[i]) != VK_SUCCESS) { g_rFail = true; return false; }
        VkFramebufferCreateInfo fb = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb.renderPass = g_rp; fb.attachmentCount = 1; fb.pAttachments = &g_scViews[i];
        fb.width = g_scExtent.width; fb.height = g_scExtent.height; fb.layers = 1;
        if (pCreateFB(g_dev, &fb, nullptr, &g_scFbs[i]) != VK_SUCCESS) { g_rFail = true; return false; }
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g_qfam;
    if (pCreatePool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) { g_rFail = true; return false; }
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = g_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = n;
    if (pAllocCB(g_dev, &cbi, g_cmd) != VK_SUCCESS) { g_rFail = true; return false; }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    pCreateFence(g_dev, &fci, nullptr, &g_fence);

    g_theSc = sc; g_rInit = true;
    Log("[menu] vk: render resources ready (%u images %ux%u fmt=%d fam=%u)\n",
        n, g_scExtent.width, g_scExtent.height, (int)g_scFormat, g_qfam);
    return true;
}

// ---- multiplayer panel state ----
static volatile LONG g_recoveryPresent = 0;
static volatile LONG g_recoveryWorldIo = 0;
// THE PANEL STAYS UP WHILE A WORLD LOADS (2026-09-18, user): with a lobby running,
// the loading screen (CreatePage 16) keeps the lobby view -- the roster with what
// every player is doing (receiving the save N%, loading world N%, catching up).
// It is drawn even during a native save/load (the frozen join is one) but takes
// no clicks then; when the world is up it hands over to the in-game panel, open,
// so the catch-up is visible too, until the player closes it.
static volatile LONG g_loadingPanel = 0;
// NativeControl resync loads bypass the lobby's start handler. The engine's
// loading page must arm progress too; consume this on the render/stage thread.
static volatile LONG g_loadingStagePending = 0;
static ULONGLONG g_recoveryRequestedAt = 0; // guarded by g_modelCs
static char g_recoveryOperation[40] = "", g_recoveryEpoch[40] = "";
static char g_recoveryPhase[24] = "", g_recoveryDetail[420] = "", g_recoveryFailedStep[24] = "";
static char g_readyToken[40] = "";
static int g_readyCount = 0, g_readyTotal = 0;
static bool g_readyMine = false;
static volatile LONG g_uiState = 0;     // 0 collapsed, 1 host/join choice, 2 lobby, 3 recovery (only on desync or during recovery)
// Low-level keyboard hook. While the lobby chat is open (state 2) and the game is
// focused, route typing into the chat box and SWALLOW the key so the game's own
// bindings never fire -- crucially Enter, which on the title menu opens Load Game.
// Passive polling (GetAsyncKeyState) can read keys but cannot stop the game from
// also receiving them, so a real hook is required to consume the input.
// Once the save is placed and the player has been told to load it, the lobby is
// finished and MUST stop eating keystrokes. The hook used to be released as a side
// effect of doStartLoad resetting g_uiState before it clicked Continue; when that
// click was replaced by "open Load Game yourself", the reset went with it, the panel
// stayed in lobby state and every key in the loaded game was swallowed -- the game
// looked like it had lost the keyboard completely.
static volatile LONG g_lobbyDone = 0;
// auto-load (see "AUTO-LOAD" below): set when the shared save is placed, taken
// by CMenuUI's per-frame update on the main thread, given up after 12 s
static volatile LONG g_autoLoadPending = 0;
static ULONGLONG     g_autoLoadSince = 0;
static char g_status[256] = "";
// A pending "download the mods this save needs?" question from the lobby
// (guarded by g_statusCs). YES / NO buttons take the status line while set.
static char g_modsPrompt[300] = "";
static int g_modsOffer = 0;
static volatile LONG g_modRefreshPending = 0;
static volatile LONG g_modLeavePending = 0;
static char g_modLeaveReason[300] = "";
static int  g_flagShareMods = 0;      // share_mods=ask (0, default) | always (1) | never (2)
static void ModDownloadPreference(bool save) {
    wchar_t path[MAX_PATH]; _snwprintf_s(path,_TRUNCATE,L"%smod_download_preference.txt",g_dataDirW);
    FILE* f=nullptr;
    if (save) { _wfopen_s(&f,path,L"wb"); if (f) { fprintf(f,"%d\n",g_flagShareMods==1); fclose(f); } }
    else { _wfopen_s(&f,path,L"rb"); if (f) { int v=0; if (fscanf_s(f,"%d",&v)==1) g_flagShareMods=v==1 ? 1:0; fclose(f); } }
}

// Player and lobby names (2026-09-16: longer names). A Steam persona is up to 32
// characters, in UTF-8 up to 96 bytes; the panel takes 64 typed characters. The
// roster itself is unbounded (below); these size what THIS player types/launches.
#define NAME_MAX 128
#define NAME_TYPED_MAX 64
// lobby model (fed from lobby_out.jsonl). One entry per roster row in roster
// order. No cap of ours on the count or on a name's length: the roster is
// whatever lobby.py sends (its CAP is the admission rule, not this parser), and
// the origin letter, the company map and the panel all work from the full name.
// A name cut short here would make two players look alike, give a joiner the
// wrong letter and leave the load gate waiting for the wrong player count.
static std::vector<std::string> g_players;
static std::vector<int>         g_companies;   // company id per roster entry (1..200), 0 = unset -> 1
static std::vector<std::string> g_stages;      // hot-join progress per roster entry ("loading world"), "" = none (2026-09-16)
static int playerCount() { return (int)g_players.size(); }

// ---- small string helpers (no fixed buffers) ----
// JSON-escape the two characters json.dumps escapes in our payloads.
static std::string jsonEscape(const char* text)
{
    std::string out; for (const char* p = text; *p; ++p) { if (*p == '\\' || *p == '"') out += '\\'; out += *p; }
    return out;
}
// Read a JSON string body: r points just past the opening quote and is left on
// the closing (unescaped) quote or the NUL. Decodes the escapes json.dumps
// emits for our payloads: \\ -> \, \" -> ", \/ -> /. Anything else (\uXXXX,
// \n ...) is left verbatim -- the backslash is copied and the next character
// follows on the next iteration.
static std::string jsonUnquote(const char*& r)
{
    std::string out;
    while (*r && *r != '"') {
        if (*r == '\\' && (r[1] == '\\' || r[1] == '"' || r[1] == '/')) { out += r[1]; r += 2; }
        else out += *r++;
    }
    return out;
}
static std::wstring wideOf(const char* utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w; if (n > 1) { w.resize(n - 1); MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n); }
    return w;
}
static std::string utf8Of(const wchar_t* wide)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string u; if (n > 1) { u.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, wide, -1, &u[0], n, nullptr, nullptr); }
    return u;
}
// Chip colour per company id: a hue walk (golden angle) so neighbouring ids differ.
static COLORREF coColor(int cid)
{
    static const COLORREF first[20] = { RGB(230,25,75), RGB(0,130,200), RGB(60,180,75), RGB(245,130,48), RGB(145,30,180), RGB(70,240,240), RGB(240,50,230), RGB(255,225,25), RGB(0,128,128), RGB(170,110,40), RGB(210,245,60), RGB(128,0,0), RGB(0,0,128), RGB(128,128,0), RGB(250,190,212), RGB(220,190,255), RGB(170,255,195), RGB(255,215,180), RGB(128,128,128), RGB(255,250,200) };
    if (cid >= 1 && cid <= 20) return first[cid - 1];
    float h = (float)(((cid - 21) * 137.508) - (int)(((cid - 21) * 137.508) / 360.0) * 360.0);   // degrees
    float sat = 0.62f, val = 0.85f, c = val * sat, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)), m = val - c;
    float r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    return RGB((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}
// Origin name for roster index idx (0 = the host): a..z, then aa, ab, ... (702 names).
static void originName(int idx, char* out)
{
    if (idx < 0) idx = 0;
    if (idx < 26) { out[0] = (char)('a' + idx); out[1] = 0; return; }
    idx -= 26; out[0] = (char)('a' + (idx / 26) % 26); out[1] = (char)('a' + idx % 26); out[2] = 0;
}
static std::string g_you, g_host, g_lobbyTitle;   // the lobby's name, from the roster; all three are whatever the roster says, any length
static volatile LONG g_lobbyRelay = 0;   // the host is a relay-only server: "host" in the roster is the LEADER (oldest joiner)
static std::vector<std::string> g_letters;   // relay lobbies: origin letter per roster entry, assigned by the relay (sticky)
static char g_chatLog[14][200]; static int g_chatHead = 0, g_chatCount = 0;
static char g_chatInput[200] = ""; static int g_chatLen = 0;
static volatile LONG g_isHost = 0;       // this instance is the lobby host
static CRITICAL_SECTION g_modelCs; static bool g_modelCsInit = false;
static void chatPush(const char* from, const char* text)
{
    if (!g_modelCsInit) return; EnterCriticalSection(&g_modelCs);
    char* slot = g_chatLog[(g_chatHead + g_chatCount) % 14];
    if (g_chatCount == 14) { slot = g_chatLog[g_chatHead]; g_chatHead = (g_chatHead + 1) % 14; } else g_chatCount++;
    if (from && from[0]) snprintf(slot, 200, "%s: %s", from, text); else snprintf(slot, 200, "%s", text);
    LeaveCriticalSection(&g_modelCs); InterlockedExchange(&g_panelDirty, 1);
}
static CRITICAL_SECTION g_statusCs; static bool g_csInit = false;
static void SetStatus(const char* s) { if (!g_csInit) return; EnterCriticalSection(&g_statusCs);
    strncpy_s(g_status, s, _TRUNCATE); LeaveCriticalSection(&g_statusCs); InterlockedExchange(&g_panelDirty, 1); }

// button rects WITHIN the panel image (local coords). Filled by RenderPanelGDI.
static int g_hover = 0, g_active = 0;     // hit id under the cursor / pressed
struct Hit { int x, y, w, h; int id; bool btn; };   // id: 2=HOST 3=JOIN 4=close 5=LEAVE 6=START 7=copy code 8=code field 11=PUBLIC 12=REFRESH 50=SEPARATE COMPANIES 30..37=public game rows; btn = hover wash
static const int MAX_COMPANIES = 200;   // lobby.py MAX_COMPANIES (one addPlayer() entity each on every peer); the roster itself has no cap here -- origins a..z then aa, ab, ...
static const int ROSTER_ROWS = 16;                        // rows the lobby page can show; the rest is a "+N more" line
static Hit g_hits[64]; static int g_hitCount = 0;
static void addHit(int x,int y,int w,int h,int id,bool btn=false){ if(g_hitCount<64){g_hits[g_hitCount++]={x,y,w,h,id,btn};} }
static const Hit* hoveredHit(){ for(int i=0;i<g_hitCount;i++) if(g_hits[i].btn && g_hits[i].id==g_hover) return &g_hits[i]; return nullptr; }

// ---------------- flags (tpf2_menu_flags.txt next to this dll) ----------------
// Every value is checked: a garbled file, or a value out of range, leaves that
// setting at its default instead of breaking the menu.
//   scale=<f>               UI scale, 0.5-3 (default: screen height / 1080)
//   slot=<n>                position of the Multiplayer entry, 0-7 (default 0 = top)
//   master_url=<url>        public game list, a plain http(s) URL; empty hides the list
//   relay_autosave_min=<n>  relay leader's save upload interval, 0-60 minutes (0 = never)
//   autoload=0|1            START loads the shared save in-process (0: the player opens LOAD GAME)
static float g_flagScale = 0.f;
static int   g_flagSlot = 0;
static char  g_flagMaster[256] = "https://srv1306562.hstgr.cloud/tpf2mp";   // master server base URL ("" disables the browser)
static int   g_flagRelayAutosaveMin = 2;    // relay lobbies: the leader uploads a fresh save this often (0 = never)
static int   g_flagAutoLoad = 1;            // START loads the shared save in-process (autoload=0: the player opens LOAD GAME)
// DEDICATED SERVER (2026-09-18): a game that hosts by itself. With dedicated=1 the
// title menu hosts a lobby the moment it is up, loads dedicated_save (else the
// newest save), keeps the game's own autosave going, and hosts + loads again
// after a crash to the title menu. The world pauses while nobody else is in
// (dedicated_pause_empty=1, mp/pacing.lua) and joiners come in through the
// frozen-join round like on any host. DedicatedTick, once a second from myPresent.
static int   g_flagDedicated = 0;
static char  g_flagDedSave[64] = "";        // dedicated_save=<name without .sav>; empty = the newest save in the save folder
static char  g_flagDedLobby[64] = "";       // dedicated_lobby=<lobby name shown in the public list>
static char  g_flagDedName[32] = "";        // dedicated_name=<the server's player name>
static char  g_flagDedPassword[40] = "";    // dedicated_password=<lobby password>; empty = open
static int   g_flagDedPublic = 1;           // dedicated_public=0|1: listed on the master server
static int   g_flagDedCompanies = 0;        // dedicated_companies=0|1: SEPARATE COMPANIES (a company per player)
static int   g_flagDedAutosaveMin = 10;     // dedicated_autosave_min=<n>, 0-600: the game's own autosave this often (0 = never)
static int   g_flagDedEmptySpeed = 1;       // dedicated_empty_speed=0..4: the world's speed while nobody else is in (0 = paused); dedicated_pause_empty=1 is 0
static int   g_flagDedPort = 0;             // dedicated_port=<udp/tcp port> for the lobby (0 = the default 29471); a box that also runs the relay needs another
static int   g_flagDedRender = 0;           // dedicated_render=0|1: 0 (default) submits no command buffers -- a software renderer then costs nothing; 1 draws (screenshots)
static int   g_flagDedNoWsi = 0;            // dedicated_nowsi=0|1: 1 also answers acquire/present in the DLL, paced to 60 frames/s (opt-in: it crashed the engine on the VPS)
static volatile LONG g_storedAge = -1, g_storedMax = -1;   // relay roster: age of the relay's stored world / how fresh counts as fresh
static volatile LONG g_joinFreeze = 0;   // roster join_freeze: the lobby brings a late joiner in through a world sync (everyone reloads); this DLL takes no hot-join save (2026-09-16)
static bool  g_latoLoaded = false;
static void ReadFlags()
{
    char p[MAX_PATH]; snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", ourDirA());
    FILE* f = fopen(p, "r"); if (!f) { Log("[menu] flags: no %s (defaults)\n", p); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        char v[256]; strcpy_s(v, eq + 1);
        char* e = v + strlen(v); while (e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        const bool digit = v[0] >= '0' && v[0] <= '9';
        if (!strcmp(line, "scale")) {
            float s = (float)atof(v);
            g_flagScale = (s >= 0.5f && s <= 3.0f) ? s : 0.f;
        } else if (!strcmp(line, "relay_autosave_min")) {
            int m = atoi(v);
            if (digit && m >= 0 && m <= 60) g_flagRelayAutosaveMin = m;
        } else if (!strcmp(line, "share_mods")) {
            if (!strcmp(v, "always")) g_flagShareMods = 1; else if (!strcmp(v, "never")) g_flagShareMods = 2; else g_flagShareMods = 0;
        } else if (!strcmp(line, "autoload")) {
            if (!strcmp(v, "0")) g_flagAutoLoad = 0; else if (!strcmp(v, "1")) g_flagAutoLoad = 1;
        } else if (!strcmp(line, "dedicated")) {
            if (!strcmp(v, "0")) g_flagDedicated = 0; else if (!strcmp(v, "1")) g_flagDedicated = 1;
        } else if (!strcmp(line, "dedicated_save")) {
            // a save NAME (it becomes <save dir>\<name>.sav): no path parts, no quotes
            if (v[0] && strlen(v) < sizeof(g_flagDedSave) && !strpbrk(v, "\\/:*?\"<>|")) strcpy_s(g_flagDedSave, v);
            else if (v[0]) Log("[menu] flags: dedicated_save ignored (a plain save name, under 64 characters)\n");
        } else if (!strcmp(line, "dedicated_lobby")) {
            if (v[0] && strlen(v) < sizeof(g_flagDedLobby) && !strpbrk(v, "\"")) strcpy_s(g_flagDedLobby, v);
        } else if (!strcmp(line, "dedicated_name")) {
            if (v[0] && strlen(v) < sizeof(g_flagDedName) && !strpbrk(v, "\" \t")) strcpy_s(g_flagDedName, v);
        } else if (!strcmp(line, "dedicated_password")) {
            // it becomes a netpunch.exe argument: no blanks or quotes
            if (strlen(v) < sizeof(g_flagDedPassword) && !strpbrk(v, "\" \t")) strcpy_s(g_flagDedPassword, v);
        } else if (!strcmp(line, "dedicated_public")) {
            if (!strcmp(v, "0")) g_flagDedPublic = 0; else if (!strcmp(v, "1")) g_flagDedPublic = 1;
        } else if (!strcmp(line, "dedicated_companies")) {
            if (!strcmp(v, "0")) g_flagDedCompanies = 0; else if (!strcmp(v, "1")) g_flagDedCompanies = 1;
        } else if (!strcmp(line, "dedicated_autosave_min")) {
            int m = atoi(v);
            if (digit && m >= 0 && m <= 600) g_flagDedAutosaveMin = m;
        } else if (!strcmp(line, "dedicated_empty_speed")) {
            int es = atoi(v);
            if (digit && es >= 0 && es <= 4) g_flagDedEmptySpeed = es;
        } else if (!strcmp(line, "dedicated_pause_empty")) {
            // the older flag: 1 = paused while empty; 0 = the (old) "keep simulating" = 1x
            if (!strcmp(v, "0")) g_flagDedEmptySpeed = 1; else if (!strcmp(v, "1")) g_flagDedEmptySpeed = 0;
        } else if (!strcmp(line, "dedicated_render")) {
            if (!strcmp(v, "0")) g_flagDedRender = 0; else if (!strcmp(v, "1")) g_flagDedRender = 1;
        } else if (!strcmp(line, "dedicated_nowsi")) {
            if (!strcmp(v, "0")) g_flagDedNoWsi = 0; else if (!strcmp(v, "1")) g_flagDedNoWsi = 1;
        } else if (!strcmp(line, "input_hold")) {
            // 1: a held session (resync, join) swallows all game input again; default off (native_io.h)
            NativeIo::SetInputBlocking(!strcmp(v, "1"));
        } else if (!strcmp(line, "dedicated_pin_batch")) {
            if (!strcmp(v, "0")) g_flagDedPinBatch = 0; else if (!strcmp(v, "1")) g_flagDedPinBatch = 1;
        } else if (!strcmp(line, "dedicated_fps")) {
            int fv = atoi(v);
            if (digit && fv >= 5 && fv <= 240) g_flagDedFps = fv;
        } else if (!strcmp(line, "dedicated_port")) {
            int pt = atoi(v);
            if (digit && pt >= 1024 && pt <= 65535) g_flagDedPort = pt;
        } else if (!strcmp(line, "slot")) {
            // the title menu builds 8 entries (9 with CONTINUE): a slot past them never inserts ours
            int s = atoi(v);
            if (digit && s >= 0 && s <= 7) g_flagSlot = s;
        } else if (!strcmp(line, "master_url")) {
            while (e > v && e[-1] == '/') *--e = 0;
            // it becomes a process argument and a WinHTTP request: a space or quote breaks both
            bool ok = !v[0] || ((!strncmp(v, "https://", 8) || !strncmp(v, "http://", 7)) && !strpbrk(v, " \t\"'"));
            if (ok) strcpy_s(g_flagMaster, v); else Log("[menu] flags: master_url ignored (not a plain http(s) URL)\n");
        }
    }
    fclose(f);
    Log("[menu] flags: slot=%d scale=%.2f autoload=%d relay_autosave_min=%d\n", g_flagSlot, g_flagScale, g_flagAutoLoad, g_flagRelayAutosaveMin);
    if (g_flagDedicated)
        Log("[dedicated] on: save='%s' lobby='%s' name='%s' password=%s public=%d companies=%d autosave_min=%d empty_speed=%d port=%d render=%d\n",
            g_flagDedSave, g_flagDedLobby, g_flagDedName, g_flagDedPassword[0] ? "yes" : "no", g_flagDedPublic,
            g_flagDedCompanies, g_flagDedAutosaveMin, g_flagDedEmptySpeed, g_flagDedPort, g_flagDedRender);
    if (g_flagDedicated && !g_flagDedRender) InterlockedExchange(&g_noRender, 1);
    if (g_flagDedicated && !g_flagDedRender && g_flagDedNoWsi) InterlockedExchange(&g_noWsi, 1);
}
// The game's own menu face: <gamedir>\res\fonts\Lato2OFL\Lato-Regular.ttf, loaded
// process-private so GDI can select "Lato" without touching the system font table.
static void LoadLato()
{
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* s = wcsrchr(exe, L'\\'); if (s) s[1] = 0;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%sres\\fonts\\Lato2OFL\\Lato-Regular.ttf", exe);
    int n = AddFontResourceExW(path, FR_PRIVATE, nullptr);
    g_latoLoaded = n > 0;
    Log("[menu] font: %ls -> %d faces\n", path, n);
}
static float UiScale() {
    float scale = g_flagScale > 0.f ? g_flagScale : (g_scExtent.height ? g_scExtent.height / 1080.f : 1.f);
    // Keep the menu dialog inside the viewport even with a large manual scale.
    if ((g_uiState>=1 && g_uiState<=3) && g_scExtent.width && g_scExtent.height) {
        scale = (std::min)(scale, g_scExtent.width / 800.f);
        scale = (std::min)(scale, g_scExtent.height / 560.f);
    }
    return scale;
}

// the menu face, grayscale-antialiased (coverage is read back as alpha, so no ClearType fringes)
static HFONT mkLato(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, g_latoLoaded ? L"Lato" : L"Segoe UI");
}

// ---------------- software compositing layer ----------------
// The panel is drawn into a straight-alpha BGRA layer (rect fills with alpha, GDI
// text rendered white-on-black and used as coverage), then composited over a
// cached native title artwork (or a solid in-game base). This gives it a
// MenuWindow (window.lua): a translucent (5,25,40) sheet, transparent buttons
// whose hover/press is a white wash, black@50 text fields.
struct Layer { int w, h; unsigned char* px; };
static Layer g_layer = { 0, 0, nullptr };
static float g_s = 1.f;                                  // UI scale = screen height / 1080
static int S(float v) { return (int)(v * g_s + 0.5f); }
static void layerBegin(int w, int h)
{
    if (g_layer.w != w || g_layer.h != h) { free(g_layer.px); g_layer.px = (unsigned char*)malloc((size_t)w * h * 4); g_layer.w = w; g_layer.h = h; }
    memset(g_layer.px, 0, (size_t)w * h * 4);
}
static inline void pxOver(unsigned char* d, int r, int g, int b, int a)
{
    if (a <= 0) return;
    int da = d[3];
    if (a >= 255 || da == 0) { d[0] = (unsigned char)b; d[1] = (unsigned char)g; d[2] = (unsigned char)r; d[3] = (unsigned char)a; return; }
    int k = da * (255 - a) / 255, outA = a + k; if (outA <= 0) return;
    d[0] = (unsigned char)((b * a + d[0] * k) / outA);
    d[1] = (unsigned char)((g * a + d[1] * k) / outA);
    d[2] = (unsigned char)((r * a + d[2] * k) / outA);
    d[3] = (unsigned char)outA;
}
static void layerRect(int x, int y, int w, int h, COLORREF c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_layer.w ? g_layer.w : x + w, y1 = y + h > g_layer.h ? g_layer.h : y + h;
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    for (int yy = y0; yy < y1; yy++) { unsigned char* d = g_layer.px + ((size_t)yy * g_layer.w + x0) * 4; for (int xx = x0; xx < x1; xx++, d += 4) pxOver(d, r, g, b, a); }
}
static void layerText(int x, int y, int w, int h, const wchar_t* s, HFONT f, COLORREF c, UINT fmt, int alpha = 255)
{
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbm = SelectObject(mem, dib);
    RECT rc = { 0, 0, w, h }; HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0)); FillRect(mem, &rc, bk); DeleteObject(bk);
    HGDIOBJ of = SelectObject(mem, f); SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, s, -1, &rc, fmt); SelectObject(mem, of);
    const unsigned char* src = (const unsigned char*)bits;
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    for (int yy = 0; yy < h; yy++) { int ly = y + yy; if (ly < 0 || ly >= g_layer.h) continue;
        for (int xx = 0; xx < w; xx++) { int lx = x + xx; if (lx < 0 || lx >= g_layer.w) continue;
            const unsigned char* q = src + ((size_t)yy * w + xx) * 4; int m = q[0] > q[1] ? q[0] : q[1]; if (q[2] > m) m = q[2];
            if (m) pxOver(g_layer.px + ((size_t)ly * g_layer.w + lx) * 4, r, g, b, m * alpha / 255); } }
    SelectObject(mem, oldbm); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
static int textW(const wchar_t* s, HFONT f)
{
    HDC dc = GetDC(nullptr); HGDIOBJ of = SelectObject(dc, f); SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz); SelectObject(dc, of); ReleaseDC(nullptr, dc); return sz.cx;
}
// Composite: game frame (readback) -> hover wash under the hovered button
// (Button:hover white@50, :active white@100) -> the layer, src-over. The mapped
// images are write-combined, so rows are staged through ordinary RAM.
struct Hit; static const Hit* hoveredHit();
static unsigned char* g_stage = nullptr; static size_t g_stageSz = 0;
// MenuWindow has blurRadius = 64 behind its sheet (main-menu-windows.lua). Same
// legacy readback path: average down 4x, two running-sum box blurs
// (radius 16 at quarter size ~= 64 full-size), bilinear back up. Not used by the title renderer.
static unsigned char* g_blurA = nullptr; static unsigned char* g_blurB = nullptr; static size_t g_blurSz = 0;
static void boxBlurH(const unsigned char* src, unsigned char* dst, int w, int h, int r)
{
    for (int y = 0; y < h; y++) {
        const unsigned char* s = src + (size_t)y * w * 4; unsigned char* d = dst + (size_t)y * w * 4;
        for (int c = 0; c < 3; c++) {
            int sum = 0, n = 0;
            for (int x = 0; x <= r && x < w; x++) { sum += s[x * 4 + c]; n++; }
            for (int x = 0; x < w; x++) {
                d[x * 4 + c] = (unsigned char)(sum / n);
                int add = x + r + 1, sub = x - r;
                if (add < w) { sum += s[add * 4 + c]; n++; }
                if (sub >= 0) { sum -= s[sub * 4 + c]; n--; }
            }
        }
    }
}
static void boxBlurV(const unsigned char* src, unsigned char* dst, int w, int h, int r)
{
    for (int x = 0; x < w; x++) {
        for (int c = 0; c < 3; c++) {
            int sum = 0, n = 0;
            for (int y = 0; y <= r && y < h; y++) { sum += src[((size_t)y * w + x) * 4 + c]; n++; }
            for (int y = 0; y < h; y++) {
                dst[((size_t)y * w + x) * 4 + c] = (unsigned char)(sum / n);
                int add = y + r + 1, sub = y - r;
                if (add < h) { sum += src[((size_t)add * w + x) * 4 + c]; n++; }
                if (sub >= 0) { sum -= src[((size_t)sub * w + x) * 4 + c]; n--; }
            }
        }
    }
}
static void BlurStage(int w, int h)
{
    const int D = 4; int sw = w / D, sh = h / D; if (sw < 2 || sh < 2) return;
    size_t need = (size_t)sw * sh * 4;
    if (g_blurSz < need) { free(g_blurA); free(g_blurB); g_blurA = (unsigned char*)malloc(need); g_blurB = (unsigned char*)malloc(need); g_blurSz = need; }
    // downsample: 4x4 box average
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        int acc[3] = { 0, 0, 0 };
        for (int yy = 0; yy < D; yy++) { const unsigned char* s = g_stage + (((size_t)(y * D + yy)) * w + x * D) * 4;
            for (int xx = 0; xx < D; xx++, s += 4) { acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; } }
        unsigned char* d = g_blurA + ((size_t)y * sw + x) * 4; d[0] = (unsigned char)(acc[0] / 16); d[1] = (unsigned char)(acc[1] / 16); d[2] = (unsigned char)(acc[2] / 16);
    }
    int r = (int)(16 * g_s / 2 + 0.5f); if (r < 4) r = 4;   // 64px full-size at 1080p, scaled
    boxBlurH(g_blurA, g_blurB, sw, sh, r); boxBlurV(g_blurB, g_blurA, sw, sh, r);
    boxBlurH(g_blurA, g_blurB, sw, sh, r); boxBlurV(g_blurB, g_blurA, sw, sh, r);
    // bilinear upsample back into the stage
    for (int y = 0; y < h; y++) {
        float fy = ((y + 0.5f) / D) - 0.5f; int y0 = (int)fy; if (y0 < 0) { y0 = 0; fy = 0; } int y1 = y0 + 1 < sh ? y0 + 1 : y0; float ty = fy - y0; if (ty < 0) ty = 0;
        unsigned char* d = g_stage + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, d += 4) {
            float fx = ((x + 0.5f) / D) - 0.5f; int x0 = (int)fx; if (x0 < 0) { x0 = 0; fx = 0; } int x1 = x0 + 1 < sw ? x0 + 1 : x0; float tx = fx - x0; if (tx < 0) tx = 0;
            const unsigned char* a = g_blurA + ((size_t)y0 * sw + x0) * 4; const unsigned char* b = g_blurA + ((size_t)y0 * sw + x1) * 4;
            const unsigned char* c2 = g_blurA + ((size_t)y1 * sw + x0) * 4; const unsigned char* e = g_blurA + ((size_t)y1 * sw + x1) * 4;
            for (int c = 0; c < 3; c++) {
                float top = a[c] + (b[c] - a[c]) * tx, bot = c2[c] + (e[c] - c2[c]) * tx;
                d[c] = (unsigned char)(top + (bot - top) * ty + 0.5f);
            }
        }
    }
}
static void ComposeLayer(const unsigned char* bg, size_t bgPitch, void* dst, size_t pitch, int w, int h, bool preblurred = false)
{
    size_t need = (size_t)w * 4 * h;
    if (g_stageSz < need) { free(g_stage); g_stage = (unsigned char*)malloc(need); g_stageSz = need; }
    if (bg) {
        for (int y = 0; y < h; y++) memcpy(g_stage + (size_t)y * w * 4, bg + y * bgPitch, (size_t)w * 4);
        if(!preblurred) BlurStage(w, h);
    } else {
        // Opaque sheet (bg == nullptr): no read-back of the game frame, no blur.
        // 40,25,5 is MW_BG = RGB(5,25,40) in the swapchain's B,G,R,A order; the
        // layer's own background rect is that same colour, so the panel keeps its
        // look and only loses the game showing through it.
        for (int y = 0; y < h; y++) {
            unsigned char* d = g_stage + (size_t)y * w * 4;
            for (int x = 0; x < w; x++, d += 4) { d[0] = 40; d[1] = 25; d[2] = 5; d[3] = 255; }
        }
    }
    const Hit* hv = hoveredHit();
    if (hv) {
        int fill = g_active ? 100 : 50;
        int x0 = *(const int*)hv, y0 = *((const int*)hv + 1), x1 = x0 + *((const int*)hv + 2), y1 = y0 + *((const int*)hv + 3);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
        for (int y = y0; y < y1; y++) { unsigned char* d = g_stage + ((size_t)y * w + x0) * 4;
            for (int x = x0; x < x1; x++, d += 4) for (int c = 0; c < 3; c++) d[c] = (unsigned char)(d[c] + (255 - d[c]) * fill / 255); }
    }
    for (int y = 0; y < h; y++) {
        unsigned char* d = g_stage + (size_t)y * w * 4; const unsigned char* l = g_layer.px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, d += 4, l += 4) { int a = l[3]; if (!a) continue;
            for (int c = 0; c < 3; c++) d[c] = (unsigned char)((d[c] * (255 - a) + l[c] * a) / 255); }
    }
    for (int y = 0; y < h; y++) memcpy((unsigned char*)dst + y * pitch, g_stage + (size_t)y * w * 4, (size_t)w * 4);
}

// ---------------- the Multiplayer window (MenuWindow look) ----------------
#define MW_BG     RGB(5, 25, 40)        // Window, MenuWindow backgroundColor
#define MW_BG_A   190                   //   (175 in the sheet; a touch more without the blur)
#define MW_TEXT   RGB(255, 255, 255)
#define MW_DIM    RGB(190, 205, 218)
#define MW_YOU    RGB(150, 210, 170)
static char g_joinCode[256] = ""; static int g_joinLen = 0; static volatile LONG g_joinFocus = 0;   // 1 = code field, 2 = password field
static char g_passCode[40] = "";  static int g_passLen = 0;   // optional lobby password (mixed into the session key)
static char g_username[NAME_MAX] = "";   // the player name (random two-word default, see ensureUsername)
static char g_lobbyName[NAME_MAX] = ""; static int g_lobbyNameLen = 0;   // what the host calls the lobby (focus 4); the player name is g_username (focus 3)
static int  g_userLen = 0;
static void ensureUsername();
static void SaveNames();
static bool IsGeneratedName(const char* n);   // one of ensureUsername's Adjective+Noun defaults
// The player name follows the STEAM persona name (2026-09-16) unless the
// player typed one. g_userAuto says which: auto names are re-read from Steam
// every launch (the persona can change), a typed one is kept as typed. Clearing
// the field and pressing Enter goes back to Steam.
static bool g_userAuto = true;
static char g_steamName[NAME_MAX] = "";
static bool SteamPersonaName(char* out, size_t cap)
{
    HMODULE h = GetModuleHandleW(L"steam_api64.dll"); if (!h) return false;
    typedef int (*HUserFn)(); typedef void* (*FriendsFn)(); typedef const char* (*PersonaFn)(void*);
    HUserFn huser = (HUserFn)GetProcAddress(h, "SteamAPI_GetHSteamUser");
    FriendsFn friends = (FriendsFn)GetProcAddress(h, "SteamAPI_SteamFriends_v017");
    PersonaFn persona = (PersonaFn)GetProcAddress(h, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!huser || !friends || !persona || huser() == 0) return false;   // the game has not initialised Steam yet
    void* fr = friends(); if (!fr) return false;
    const char* n = persona(fr); if (!n || !n[0]) return false;
    // The name is a command-line argument, a JSON string and a roster key:
    // printable only, no quotes or backslashes, no '#' (the lobby's own
    // de-dup suffix, "name#2"), spaces collapsed and trimmed.
    size_t o = 0; bool sp = false;
    for (size_t i = 0; n[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 32 || c == '"' || c == '\\' || c == '#') continue;
        if (c == ' ') { sp = o > 0; continue; }
        if (sp) { out[o++] = ' '; sp = false; if (o + 1 >= cap) break; }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o > 0;
}
// Names persist in <data dir>\tpf2_names.txt (player=..., lobby=..., auto=1)
// so they survive a relaunch. No file, or auto=1: the name follows Steam
// (the random two-word default stands in until Steam answers).
static void LoadNames()
{
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2_names.txt", g_dataDirW);
    FILE* f = _wfopen(p, L"r"); if (!f) { SaveNames(); return; }   // first run: follow Steam from now on
    char line[512]; bool sawAuto = false, sawPlayer = false;
    while (fgets(line, sizeof(line), f)) {
        char* e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0; const char* v = eq + 1;
        if (!strcmp(line, "player") && v[0]) { strncpy_s(g_username, v, NAME_MAX - 1); sawPlayer = true; }
        else if (!strcmp(line, "lobby")) { strncpy_s(g_lobbyName, v, NAME_MAX - 1); }
        else if (!strcmp(line, "auto")) { sawAuto = true; g_userAuto = v[0] == '1'; }
    }
    fclose(f);
    // a file from before auto= existed holds a name the player kept: treat it as typed
    if (!sawAuto) g_userAuto = !sawPlayer;
    // ...unless it is one of OUR random defaults (HappyDingo, DaringOcelot): nobody
    // typed that. The first run of the Steam-name build found such a name in the
    // file, called it typed, wrote auto=0, and the persona ("steam persona:
    // ComradeSilver" in every log since) never replaced it on either rig
    // instance (2026-09-17). A generated name is never a typed one.
    if (sawPlayer && !g_userAuto && IsGeneratedName(g_username)) {
        g_userAuto = true;
        Log("[menu] names: %s is one of our random defaults, not a typed name -- following Steam\n", g_username);
    }
    g_userLen = (int)strlen(g_username); g_lobbyNameLen = (int)strlen(g_lobbyName);
    Log("[menu] names: player=%s (%s) lobby=%s\n", g_username, g_userAuto ? "follows Steam" : "typed", g_lobbyName);
}
static void SaveNames()
{
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2_names.txt", g_dataDirW);
    FILE* f = _wfopen(p, L"w"); if (!f) return;
    fprintf(f, "player=%s\nlobby=%s\nauto=%d\n", g_username, g_lobbyName, g_userAuto ? 1 : 0); fclose(f);
}
// Called from the present hook: Steam is initialised by the game some time
// after our DLL loads, so the persona is asked for until it answers, then
// re-checked now and then (the player can rename themselves in Steam).
static void SteamNameTick()
{
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + (g_steamName[0] ? 30000 : 2000);
    char n[NAME_MAX];
    if (!SteamPersonaName(n, sizeof(n))) return;
    if (strcmp(n, g_steamName) != 0) { strcpy_s(g_steamName, n); Log("[menu] steam persona: %s\n", n); }
    if (!g_userAuto || InterlockedCompareExchange(&g_joinFocus, 0, 0) == 3) return;   // typed, or being typed right now
    if (strcmp(g_username, n) == 0) return;
    if (InterlockedCompareExchange(&g_uiState, 0, 0) >= 2) return;   // in a lobby already: the roster has the old name; next time
    strcpy_s(g_username, n); g_userLen = (int)strlen(g_username);
    SaveNames();
    InterlockedExchange(&g_panelDirty, 1);
    Log("[menu] username follows Steam: %s\n", g_username);
}
static volatile LONG g_public = 0;   // PUBLIC ticked: the lobby announces itself to the master server
// SEPARATE COMPANIES ticked (2026-09-16): the lobby gives every player their own
// company; unticked, everyone shares company 1 (co-op). The lobby assigns the
// chips from it, on a change and for each joiner; the roster carries the mode
// back, so a joiner's panel shows it (and the host's stays in step).
static volatile LONG g_sepCompanies = 0;
// CROSS-PLAY (2026-09-22). A host on Steam shares its Steam ID as the join code and
// is reached through Steam only; ticking CROSS-PLAY switches the lobby to the classic
// code, which players without Steam (GOG, a Steam-offline box) can use too. The lobby
// re-emits its "code" event on every switch; g_hostSteam says whether the host has a
// Steam ID at all (without one the classic code is the only code and the box is hidden).
static volatile LONG g_crossplay = 0;
static volatile LONG g_hostSteam = 0;
static volatile LONG g_codeSeen = 0;    // the first code event of this lobby: starts the host's snapshot

// ---------------- the public game list (server browser) ----------------
// GET <master>/list on a background thread every PUB_EVERY ms while the
// HOST/JOIN page is up; rows render below the password field and a click
// drops the row's code into the join field. The list is what hosts chose to
// publish (see _Publisher in lobby.py); nothing here talks to a host directly.
struct PubRow { char name[NAME_MAX]; char code[256]; char game[64]; char type[16]; char version[24]; int players, max, age; bool locked; };
static PubRow g_pub[8]; static int g_pubCount = 0; static char g_pubNote[96] = "";
static CRITICAL_SECTION g_pubCs; static bool g_pubCsInit = false;
static volatile LONG g_pubBusy = 0; static ULONGLONG g_pubLast = 0; static volatile LONG g_pubForce = 0;
static const ULONGLONG PUB_EVERY = 10000;   // 10 s, a third of the master TTL (30 s)

// minimal JSON field readers for the flat objects the master server emits
static bool pubStr(const char* obj, const char* key, char* out, int n)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
    const char* p = strstr(obj, k); if (!p) return false;
    p = strchr(p + strlen(k), ':'); if (!p) return false; p++;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++; int j = 0;
    while (*p && *p != '"' && j < n - 1) {
        if (*p == '\\' && p[1]) { p++; if (*p == 'n' || *p == 't') { p++; continue; } if (*p == 'u') { p += 5; out[j++] = '?'; continue; } }
        out[j++] = *p++;
    }
    out[j] = 0; return true;
}
static int pubInt(const char* obj, const char* key, int def)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
    const char* p = strstr(obj, k); if (!p) return def;
    p = strchr(p + strlen(k), ':'); if (!p) return def; p++;
    while (*p == ' ') p++;
    if (*p == 't') return 1; if (*p == 'f') return 0;
    return atoi(p);
}
static bool httpGet(const char* url, char* out, int n)
{
    wchar_t wurl[512]; MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
    URL_COMPONENTS uc = { sizeof(uc) }; wchar_t host[256], path[512];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;
    bool ok = false; out[0] = 0;
    // 2026-09-18: a player's browser answered "no response" 99 times out of 99
    // while his own lobby.py published to the same host over Python: the
    // failure was silent (no error code) and the 5 s budget covered one
    // address family. The master has an AAAA record, so a machine whose IPv6
    // routes nowhere spent the whole budget on the v6 connect. Now: IPv6 fast
    // fallback (the connect races both families), a longer budget, the
    // WinHTTP error code in the note the panel shows, and one retry.
    // WINHTTP_OPTION_IPV6_FAST_FALLBACK is 140 (Windows 10 1709+); an older
    // WinHTTP just refuses the option.
    HINTERNET s = WinHttpOpen(L"tpf2mp-menu/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { snprintf(out, n, "winhttp open error %lu", (unsigned long)GetLastError()); return false; }
    { DWORD on = 1; WinHttpSetOption(s, 140 /* WINHTTP_OPTION_IPV6_FAST_FALLBACK */, &on, sizeof(on)); }
    WinHttpSetTimeouts(s, 8000, 10000, 10000, 10000);   // resolve, connect, send, receive
    DWORD err = 0; const char* stage = "";
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        if (!c) { err = GetLastError(); stage = "connect"; }
        else if (!r) { err = GetLastError(); stage = "request"; }
        else if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { err = GetLastError(); stage = "send"; }
        else if (!WinHttpReceiveResponse(r, nullptr)) { err = GetLastError(); stage = "receive"; }
        else {
            DWORD st = 0, sl = sizeof(st);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
            int got = 0; DWORD rd = 0;
            while (got < n - 1 && WinHttpReadData(r, out + got, (DWORD)(n - 1 - got), &rd) && rd) got += (int)rd;
            out[got] = 0; ok = (st == 200);
            if (!ok) { snprintf(out, n, "HTTP %lu", (unsigned long)st); err = 0; stage = ""; }
        }
        if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c);
        if (!ok && stage[0] && attempt == 0) Log("[menu] server browser: %s failed (winhttp %lu), retrying", stage, (unsigned long)err);
    }
    WinHttpCloseHandle(s);
    if (!ok && stage[0]) snprintf(out, n, "winhttp %lu at %s", (unsigned long)err, stage);   // 12002 timeout, 12007 no such host, 12029 cannot connect
    return ok;
}
static DWORD WINAPI PubFetchThread(LPVOID)
{
    static char body[32768];
    char url[300]; snprintf(url, sizeof(url), "%s/list", g_flagMaster);
    bool ok = httpGet(url, body, sizeof(body));
    PubRow rows[8]; int cnt = 0; char note[96] = "";
    if (!ok) snprintf(note, sizeof(note), "Server browser unavailable (%s)", body[0] ? body : "no response");
    else {
        const char* p = strstr(body, "\"servers\"");
        if (p) p = strchr(p, '[');
        while (p && cnt < 8) {
            const char* o = strchr(p, '{'); if (!o) break;
            // find the object's closing brace, skipping quoted text
            const char* e = o + 1; bool q = false;
            for (; *e; e++) { if (*e == '\\' && q) { e++; continue; } if (*e == '"') q = !q; else if (*e == '}' && !q) break; }
            if (!*e) break;
            char obj[1024]; int L = (int)(e - o + 1); if (L > 1023) L = 1023; memcpy(obj, o, L); obj[L] = 0;
            PubRow& r = rows[cnt]; memset(&r, 0, sizeof(r));
            if (pubStr(obj, "code", r.code, sizeof(r.code)) && r.code[0]) {
                pubStr(obj, "name", r.name, sizeof(r.name)); pubStr(obj, "game", r.game, sizeof(r.game)); pubStr(obj, "type", r.type, sizeof(r.type)); pubStr(obj, "version", r.version, sizeof(r.version));
                r.players = pubInt(obj, "players", 0); r.max = pubInt(obj, "max", 8); r.age = pubInt(obj, "age", 0); r.locked = pubInt(obj, "locked", 0) != 0;
                cnt++;
            }
            p = e + 1;
        }
        if (cnt == 0) strcpy_s(note, "No public games right now.");
    }
    if (g_pubCsInit) { EnterCriticalSection(&g_pubCs); memcpy(g_pub, rows, sizeof(rows)); g_pubCount = cnt; strcpy_s(g_pubNote, note); LeaveCriticalSection(&g_pubCs); }
    static int logged = 0; if (logged++ % 30 == 0 || !ok) Log("[menu] server browser: %d game(s) %s\n", cnt, note);
    g_pubLast = GetTickCount64(); InterlockedExchange(&g_pubBusy, 0); InterlockedExchange(&g_panelDirty, 1);
    return 0;
}
static void PubPoll()   // called from the present hook while the HOST/JOIN page is shown
{
    if (!g_flagMaster[0]) return;
    ULONGLONG now = GetTickCount64();
    bool due = (g_pubLast == 0) || (now - g_pubLast > PUB_EVERY) || InterlockedCompareExchange(&g_pubForce, 0, 1) == 1;
    if (!due || InterlockedCompareExchange(&g_pubBusy, 1, 0) != 0) return;
    if (!g_pubCsInit) { InitializeCriticalSection(&g_pubCs); g_pubCsInit = true; }
    HANDLE t = CreateThread(nullptr, 0, PubFetchThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t); else InterlockedExchange(&g_pubBusy, 0);
}
static void mwCheck(int x, int y, const wchar_t* label, bool on, int id)
{
    int sz = S(16); layerRect(x, y + S(7), sz, sz, RGB(0, 0, 0), 60);
    layerRect(x, y + S(7), sz, 1, RGB(255, 255, 255), 90); layerRect(x, y + S(7) + sz - 1, sz, 1, RGB(255, 255, 255), 90);
    layerRect(x, y + S(7), 1, sz, RGB(255, 255, 255), 90); layerRect(x + sz - 1, y + S(7), 1, sz, RGB(255, 255, 255), 90);
    HFONT f = mkLato(S(13));
    if (on) layerText(x, y + S(5), sz, sz + S(4), L"\u2713", f, MW_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    layerText(x + sz + S(8), y, S(360), S(30), label, f, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int lw = textW(label, f); DeleteObject(f);
    addHit(x, y, sz + S(8) + lw, S(30), id, true);
}

static void mwButton(int x, int y, int w, int h, const wchar_t* label, int id)
{
    HFONT f = mkLato(S(13));
    layerText(x, y, w, h, label, f, MW_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f); addHit(x, y, w, h, id, true);
}
static int mwButtonW(const wchar_t* label) { HFONT f = mkLato(S(13)); int w = textW(label, f) + S(2 * 10); DeleteObject(f); return w; }
static void mwHeader(int x, int y, int w, const wchar_t* text)
{
    HFONT f = mkLato(S(13)); layerText(x, y, w, S(22), text, f, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE); DeleteObject(f);
}
static void mwBody(int x, int y, int w, int h, const wchar_t* text, COLORREF c = MW_TEXT)
{
    HFONT f = mkLato(S(13)); layerText(x, y, w, h, text, f, c, DT_LEFT | DT_TOP | DT_WORDBREAK); DeleteObject(f);
}
// TextInputField: black@50, padding {5,10}; caret while focused
static void mwField(int x, int y, int w, int h, const char* utf8, bool focused, const wchar_t* placeholder, int id)
{
    layerRect(x, y, w, h, RGB(0, 0, 0), focused ? 90 : 50);
    wchar_t wt[256]; MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wt, 256);
    wchar_t shown[260];
    if (wt[0] || focused) _snwprintf_s(shown, _TRUNCATE, L"%s%s", wt, (focused && (GetTickCount64() / 500) % 2 == 0) ? L"|" : L"");
    else wcscpy_s(shown, placeholder);
    HFONT f = mkLato(S(13));
    layerText(x + S(10), y, w - S(20), h, shown, f, wt[0] ? MW_TEXT : MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE, wt[0] ? 255 : 160);
    DeleteObject(f); addHit(x, y, w, h, id);
}
static void mwClose(int w, int id)
{
    int sz = S(32), x = w - S(25) - sz + S(8), y = S(8);
    HFONT f = mkLato(S(20)); layerText(x, y, sz, sz, L"\u00D7", f, MW_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE); DeleteObject(f);
    addHit(x, y, sz, sz, id, true);
}
static void mwTitle(const wchar_t* t) { HFONT f = mkLato(S(18)); layerText(S(25), S(8), S(400), S(32), t, f, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); DeleteObject(f); }
static void mwStatus(int w, int h)
{
    char st[256]; if (g_csInit) { EnterCriticalSection(&g_statusCs); strncpy_s(st, g_status, _TRUNCATE); LeaveCriticalSection(&g_statusCs); } else st[0] = 0;
    wchar_t wst[256]; MultiByteToWideChar(CP_UTF8, 0, st, -1, wst, 256);
    HFONT f = mkLato(S(12)); layerText(S(25), h - S(34), w - S(50), S(24), wst, f, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE); DeleteObject(f);
}

#include "menu_title_panel.inl"
#include "menu_title_backdrop.inl"

// Build the layer + hit rects for the current page. Called only when dirty.
static void RenderPanelLayer(int w, int h)
{
    g_s = UiScale(); layerBegin(w, h); g_hitCount = 0;
    layerRect(0, 0, w, h, MW_BG, !WorldLoaded() && (g_uiState>=1 && g_uiState<=3) ? 175 : MW_BG_A);
    int pad = S(25), cy = S(56);
    const LONG page=InterlockedCompareExchange(&g_uiState,0,0);
    if(RenderTitlePanel(w,h,page)) return;
    if (page == 2) {
        // ---------------- LOBBY ----------------
        if (g_savePicker && g_isHost && !WorldLoaded() && !g_sessionStarted) {
            mwTitle(L"SELECT SAVE"); mwClose(w, 91);
            mwBody(pad, cy, w-2*pad, S(40), L"Choose the world to share. Newest saves first, including autosaves.");
            HFONT font = mkLato(S(14));
            for (int row=0; row<SAVE_ROWS; ++row) {
                int i=g_savePage*SAVE_ROWS+row;
                if (i >= (int)g_lobbySaves.size()) break;
                const auto& save=g_lobbySaves[i];
                int y=cy+S(48)+row*S(40);
                layerRect(pad,y,w-2*pad,S(36),save.path==g_selectedSave ? MW_YOU : RGB(0,0,0),65);
                layerText(pad+S(10),y,w-2*pad-S(185),S(36),save.name.c_str(),font,MW_TEXT,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
                FILETIME local; SYSTEMTIME date; wchar_t stamp[40]=L"";
                if (FileTimeToLocalFileTime(&save.modified,&local) && FileTimeToSystemTime(&local,&date))
                    _snwprintf_s(stamp,_TRUNCATE,L"%04u-%02u-%02u %02u:%02u",date.wYear,date.wMonth,date.wDay,date.wHour,date.wMinute);
                layerText(w-pad-S(170),y,S(160),S(36),stamp,font,MW_DIM,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
                addHit(pad,y,w-2*pad,S(36),100+row,true);
            }
            DeleteObject(font);
            if (g_lobbySaves.empty()) mwBody(pad,cy+S(55),w-2*pad,S(60),L"No saves found. Create and save a world with the Multiplayer mod enabled, then refresh.");
            int y=h-S(85);
            mwButton(pad,y,S(110),S(30),L"BACK",91);
            mwButton(pad+S(125),y,S(110),S(30),L"REFRESH",92);
            if (g_savePage>0) mwButton(w-pad-S(240),y,S(110),S(30),L"PREVIOUS",93);
            if ((g_savePage+1)*SAVE_ROWS<(int)g_lobbySaves.size()) mwButton(w-pad-S(110),y,S(110),S(30),L"NEXT",94);
            mwStatus(w,h);
            return;
        }
        int titleW = S(90);
        { std::wstring wt = L"LOBBY";
          if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); if (!g_lobbyTitle.empty()) wt = L"LOBBY  --  " + wideOf(g_lobbyTitle.c_str()); LeaveCriticalSection(&g_modelCs); }
          mwTitle(wt.c_str()); HFONT ft = mkLato(S(18)); titleW = textW(wt.c_str(), ft) + S(16); DeleteObject(ft); } mwClose(w, 4);
        if (InterlockedCompareExchange(&g_haveCode, 0, 0)) {
            // ROOM CODE, DELIBERATELY NOT RENDERED.
            //
            // The code IS the credential: anyone who can read it can join
            // the lobby. On a stream, a screenshot or over a shoulder it is
            // handed to everyone watching, and unlike a password nobody ever
            // needs to TYPE it -- the legitimate way to pass it on is the
            // clipboard, which the click below already does. So the button
            // shows a placeholder and the code itself only ever leaves via
            // ClipboardSet.
            //
            // The placeholder is a FIXED string, not the real code masked:
            // sizing the button from the code would leak its length.
            const wchar_t* wcode = L"\u2022\u2022\u2022  ROOM CODE  \u2022\u2022\u2022";
            HFONT fm = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Consolas");
            int cw = textW(wcode, fm) + S(20), cx = S(25) + titleW;
            layerRect(cx, S(11), cw, S(26), RGB(0, 0, 0), 50);
            layerText(cx + S(10), S(11), cw, S(26), wcode, fm, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); DeleteObject(fm);
            HFONT fh = mkLato(S(11)); layerText(cx + cw + S(10), S(11), S(160), S(26), L"click to copy (never shown)", fh, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 180); DeleteObject(fh);
            addHit(cx, S(11), cw, S(26), 7, true);
        }
        if (g_isHost && !WorldLoaded() && !g_sessionStarted) {
            int bw=mwButtonW(L"SELECT SAVE");
            mwButton(pad,cy,bw,S(30),L"SELECT SAVE",90);
            const wchar_t* name=g_selectedSave.empty() ? L"Choose a save before starting" : wcsrchr(g_selectedSave.c_str(),L'\\');
            if (!g_selectedSave.empty()) name=name ? name+1 : g_selectedSave.c_str();
            HFONT font=mkLato(S(14));
            layerText(pad+bw+S(12),cy,w-2*pad-bw-S(12),S(30),name,font,MW_TEXT,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            DeleteObject(font);
            cy+=S(44);
        }
        int bottom = h - S(44);
        int listW = S(220), chatX = pad + listW + S(20), chatW = w - chatX - pad;
        int contentH = bottom - cy - S(12);
        // players
        char hdr[48]; int n = 0;
        if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); n = playerCount(); }
        snprintf(hdr, sizeof(hdr), "PLAYERS (%d)", n);
        wchar_t whdr[48]; MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, 48);
        mwHeader(pad, cy, listW, whdr);
        HFONT fr = mkLato(S(14)), fs = mkLato(S(11));
        for (int i = 0; i < n && i < ROSTER_ROWS; i++) {
            std::wstring wn = wideOf(g_players[i].c_str());
            bool isYou = g_players[i] == g_you, isHost = g_players[i] == g_host;
            int ry = cy + S(30) + i * S(26);
            // company chip: colour + number; left/right-click your own (the host: anyone's) to cycle
            int cid = g_companies[i] < 1 ? 1 : (g_companies[i] > MAX_COMPANIES ? MAX_COMPANIES : g_companies[i]);
            layerRect(pad, ry + S(4), S(22), S(16), coColor(cid), 220);
            wchar_t wc[4]; _snwprintf_s(wc, _TRUNCATE, L"%d", cid);
            layerText(pad, ry + S(4), S(22), S(16), wc, fs, RGB(0, 0, 0), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            bool amHost = g_you == g_host;
            if (isYou || amHost) addHit(pad, ry + S(2), S(24), S(20), 20 + i, true);   // chip ids 20..35
            bool staged = i < (int)g_stages.size() && !g_stages[i].empty();
            layerText(pad + S(30), ry, listW - (staged ? S(150) : S(80)), S(24), wn.c_str(), fr, isYou ? MW_YOU : MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (staged) {   // hot-join progress, dim, in place of the HOST tag (a host has none)
                std::wstring ws = wideOf(g_stages[i].c_str());
                layerText(pad + listW - S(120), ry, S(120), S(24), ws.c_str(), fs, MW_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 200);
            }
            else if (isHost) layerText(pad + listW - S(50), ry, S(50), S(24), L"HOST", fs, MW_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 180);
        }
        { HFONT fl = mkLato(S(11));
          int shown = n < ROSTER_ROWS ? n : ROSTER_ROWS;
          if (n > ROSTER_ROWS) { wchar_t more[48]; _snwprintf_s(more, _TRUNCATE, L"+ %d more", n - ROSTER_ROWS);
              layerText(pad + S(30), cy + S(30) + ROSTER_ROWS * S(26), listW, S(20), more, fl, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 180); }
          int legendY = cy + S(30) + (shown < 8 ? 8 : shown) * S(26) + S(6) + (n > ROSTER_ROWS ? S(22) : 0);
          if (legendY > bottom - S(44)) legendY = bottom - S(44);
          layerText(pad, legendY, listW, S(40), InterlockedCompareExchange(&g_sepCompanies, 0, 0)
                    ? L"Separate companies: each player runs their own. Left-click a chip for the next company, right-click for the previous."
                    : L"Co-op: everyone runs company 1 together. Left-click a chip for the next company, right-click for the previous.",
                    fl, MW_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK, 170); DeleteObject(fl); }
        DeleteObject(fr); DeleteObject(fs);
        if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
        // chat
        int inH = S(30), logH = contentH - inH - S(8);
        layerRect(chatX, cy, chatW, logH, RGB(0, 0, 0), 50);
        if (g_modelCsInit) {
            EnterCriticalSection(&g_modelCs);
            HFONT fc = mkLato(S(13)); int lh = S(22), maxLines = (logH - S(16)) / lh, cnt = g_chatCount;
            int first = cnt > maxLines ? cnt - maxLines : 0, ly = cy + S(8);
            for (int i = first; i < cnt; i++) {
                wchar_t wl[220]; MultiByteToWideChar(CP_UTF8, 0, g_chatLog[(g_chatHead + i) % 14], -1, wl, 220);
                layerText(chatX + S(10), ly, chatW - S(20), lh, wl, fc, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE); ly += lh;
            }
            DeleteObject(fc); LeaveCriticalSection(&g_modelCs);
        }
        mwField(chatX, cy + logH + S(8), chatW, inH, g_chatInput, true, L"Type a message and press Enter", 9);
        // A running map keeps its transport alive when the panel is hidden.
        int bw1 = 0;
        if (!WorldLoaded()) { bw1 = mwButtonW(L"LEAVE"); mwButton(pad, bottom, bw1, S(30), L"LEAVE", 5); }
        if (InterlockedCompareExchange(&g_isHost, 0, 0)) { int bw2 = mwButtonW(L"START GAME"); mwButton(w - pad - bw2, bottom, bw2, S(30), L"START GAME", 6);
            int px2 = w - pad - bw2 - S(110);
            if (g_flagMaster[0]) mwCheck(px2, bottom, L"PUBLIC", InterlockedCompareExchange(&g_public, 0, 0) != 0, 11);
            mwCheck(px2 - S(230), bottom, L"SEPARATE COMPANIES", InterlockedCompareExchange(&g_sepCompanies, 0, 0) != 0, 50);
            if (InterlockedCompareExchange(&g_hostSteam, 0, 0))
                mwCheck(px2 - S(370), bottom, L"CROSS-PLAY", InterlockedCompareExchange(&g_crossplay, 0, 0) != 0, 51); }
        if(WorldLoaded() && g_isHost) {
            bw1=mwButtonW(L"RESYNC...");
            mwButton(pad,bottom,bw1,S(30),L"RESYNC...",87);
        }
        // status between them -- or the mod-download question with its YES / NO
        char st[256]; char mp[300] = ""; if (g_csInit) { EnterCriticalSection(&g_statusCs); strncpy_s(st, g_status, _TRUNCATE); strncpy_s(mp, g_modsPrompt, _TRUNCATE); LeaveCriticalSection(&g_statusCs); } else st[0] = 0;
        int rightCut = S(160);
        if (mp[0]) {
            // Modal: discard background hit targets while consent is pending.
            g_hitCount=0;
            int dx=S(90), dy=S(140), dw=w-S(180);
            layerRect(0,0,w,h,RGB(0,0,0),170);
            layerRect(dx,dy,dw,S(230),RGB(28,32,38),255);
            mwHeader(dx+S(20),dy+S(20),dw-S(40),L"REQUIRED MODS");
            wchar_t prompt[400]; MultiByteToWideChar(CP_UTF8,0,mp,-1,prompt,400);
            mwBody(dx+S(20),dy+S(55),dw-S(40),S(65),prompt);
            mwCheck(dx+S(20),dy+S(130),L"Auto-accept mod downloads",g_flagShareMods==1,19);
            int bwN=mwButtonW(L"CANCEL"), bwY=mwButtonW(L"DOWNLOAD MODS");
            mwButton(dx+S(20),dy+S(175),bwY,S(32),L"DOWNLOAD MODS",16);
            mwButton(dx+dw-S(20)-bwN,dy+S(175),bwN,S(32),L"CANCEL",17);
        }
        wchar_t wst[256]; MultiByteToWideChar(CP_UTF8, 0, st, -1, wst, 256);
        // The status sits between LEAVE and the right-hand buttons: a line longer than
        // that space ends in an ellipsis rather than a hard cut (the mods-round line
        // with a joiner's name in front ran to the edge, 2026-09-20).
        HFONT fst = mkLato(S(12)); layerText(pad + bw1 + S(20), bottom, w - 2 * pad - bw1 - rightCut, S(30), wst, fst, mp[0] ? MW_TEXT : MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); DeleteObject(fst);
    } else {
        // ---------------- HOST / JOIN ----------------
        mwTitle(L"MULTIPLAYER"); mwClose(w, 4);
        { int lb = mwButtonW(L"OPEN LOGS"); mwButton(w - S(65) - lb, S(10), lb, S(28), L"OPEN LOGS", 15); }
        int colW = (w - 2 * pad - S(40)) / 2, lx = pad, rx = pad + colW + S(40);
        layerRect(pad + colW + S(20), cy, 1, S(130), RGB(255, 255, 255), 40);
        mwHeader(lx, cy, colW, L"HOST A GAME");
        mwBody(lx, cy + S(24), colW, S(36), WorldLoaded()
            ? L"Opens a lobby and saves this world for everyone who joins."
            : L"Open a lobby, choose a save, then share it with everyone who joins.");
        // NOT ensureUsername() here: this runs every frame, so emptying the name
        // field made the next frame roll a new random name before anything could be
        // typed (2026-09-11). An empty name is filled only on HOST/JOIN or Enter.
        { char def[NAME_MAX + 48];
          if (g_username[0]) snprintf(def, sizeof(def), "%s's game  (click to name the lobby)", g_username);
          else snprintf(def, sizeof(def), "Your game  (click to name the lobby)");
          wchar_t wd[64]; MultiByteToWideChar(CP_UTF8, 0, def, -1, wd, 64);
          mwField(lx, cy + S(60), colW, S(30), g_lobbyName, InterlockedCompareExchange(&g_joinFocus, 0, 0) == 4, wd, 14); }
        { int hb = mwButtonW(L"HOST GAME"); mwButton(lx, cy + S(96), hb, S(30), L"HOST GAME", 2);
          if (g_flagMaster[0]) mwCheck(lx + hb + S(16), cy + S(96), L"PUBLIC (listed in the browser)", InterlockedCompareExchange(&g_public, 0, 0) != 0, 11);
          mwCheck(lx, cy + S(128), L"SEPARATE COMPANIES", InterlockedCompareExchange(&g_sepCompanies, 0, 0) != 0, 50);
          mwCheck(lx + S(200), cy + S(128), L"CROSS-PLAY (players without Steam)", InterlockedCompareExchange(&g_crossplay, 0, 0) != 0, 51); }
        mwHeader(rx, cy, colW, L"JOIN A GAME");
        mwBody(rx, cy + S(28), colW, S(24), L"Paste or type the code from your host.");
        mwField(rx, cy + S(58), colW, S(30), g_joinCode, InterlockedCompareExchange(&g_joinFocus, 0, 0) == 1, L"Click to paste the code", 8);
        mwButton(rx, cy + S(96), mwButtonW(L"JOIN GAME"), S(30), L"JOIN GAME", 3);
        mwCheck(rx + S(150), cy + S(98), L"Auto-accept mod downloads", g_flagShareMods==1, 19);
        // The mod has to be on in the shared save: without it nothing replicates,
        // and START GAME refuses such a save (2026-09-10). Said up front here.
        mwBody(rx, cy + S(134), colW, S(20), L"The shared save must have the Multiplayer mod enabled.");
        // optional password: mixed into the session key, so the host and every
        // joiner must type the same one. Shown masked.
        mwHeader(pad, cy + S(162), S(260), L"YOUR NAME");
        mwField(pad, cy + S(186), S(260), S(30), g_username, InterlockedCompareExchange(&g_joinFocus, 0, 0) == 3, L"Steam name (click to type your own)", 13);
        mwHeader(pad + S(290), cy + S(162), w - 2 * pad - S(290), L"PASSWORD  --  optional; anyone who has the code can read your IP address");
        { char masked[40]; int i = 0; for (; i < g_passLen && i < 39; i++) masked[i] = '*'; masked[i] = 0;
          mwField(pad + S(290), cy + S(186), S(260), S(30), masked, InterlockedCompareExchange(&g_joinFocus, 0, 0) == 2, L"Click to type a password", 10); }
        // ---- PUBLIC GAMES: the server browser (OpenTTD style) ----
        if (g_flagMaster[0]) {
            int ly = cy + S(230); int lw = w - 2 * pad;
            mwHeader(pad, ly, lw - S(120), L"PUBLIC GAMES  --  click a row, then JOIN GAME");
            { int rb = mwButtonW(L"REFRESH"); mwButton(w - pad - rb, ly - S(4), rb, S(30), L"REFRESH", 12); }
            ly += S(26);
            PubRow rows[8]; int cnt = 0; char note[96] = "";
            if (g_pubCsInit) { EnterCriticalSection(&g_pubCs); memcpy(rows, g_pub, sizeof(rows)); cnt = g_pubCount; strcpy_s(note, g_pubNote); LeaveCriticalSection(&g_pubCs); }
            HFONT fr = mkLato(S(13));
            // HOST takes the room the save name had: the list shows the server TYPE,
            // one short phrase, never the host's save file name (2026-09-10)
            int cName = pad + S(10), cType = pad + S(395), cPl = pad + S(520), cVer = pad + S(600), cAge = pad + S(670);
            layerText(cName, ly, S(375), S(20), L"HOST", fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            layerText(cType, ly, S(120), S(20), L"TYPE", fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            layerText(cPl, ly, S(70), S(20), L"PLAYERS", fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            layerText(cVer, ly, S(60), S(20), L"VERSION", fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            layerText(cAge, ly, S(80), S(20), L"SEEN", fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ly += S(22);
            int maxRows = (h - S(40) - ly) / S(24); if (maxRows > 8) maxRows = 8;
            for (int i = 0; i < cnt && i < maxRows; i++) {
                const PubRow& r = rows[i]; int rh = S(24);
                layerRect(pad, ly, lw, rh, RGB(0, 0, 0), (i & 1) ? 35 : 55);
                wchar_t wn[64], wv[32], wp[32], wa[32];
                MultiByteToWideChar(CP_UTF8, 0, r.name, -1, wn, 64); MultiByteToWideChar(CP_UTF8, 0, r.version, -1, wv, 32);
                // a master from before the type field: the relay is known by its game string
                const wchar_t* wt = (!strcmp(r.type, "relay") || !strcmp(r.type, "dedicated")) ? L"dedicated server" : !strcmp(r.type, "host") ? L"player hosted"
                                  : !strcmp(r.game, "dedicated relay") ? L"dedicated server" : L"player hosted";
                if (r.locked) { wchar_t t[64]; _snwprintf_s(t, _TRUNCATE, L"%s  [locked]", wn); wcscpy_s(wn, t); }
                _snwprintf_s(wp, _TRUNCATE, L"%d / %d", r.players, r.max);
                if (r.age < 60) wcscpy_s(wa, L"just now"); else _snwprintf_s(wa, _TRUNCATE, L"%d min ago", r.age / 60);
                layerText(cName, ly, S(375), rh, wn, fr, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                layerText(cType, ly, S(120), rh, wt, fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                layerText(cPl, ly, S(70), rh, wp, fr, MW_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                layerText(cVer, ly, S(60), rh, wv, fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                layerText(cAge, ly, S(80), rh, wa, fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                addHit(pad, ly, lw, rh, 40 + i, true);
                ly += rh + S(2);
            }
            if (cnt == 0) { wchar_t wnote[96]; MultiByteToWideChar(CP_UTF8, 0, note[0] ? note : "Looking for public games…", -1, wnote, 96);
                            layerText(cName, ly, lw - S(20), S(24), wnote, fr, MW_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
            DeleteObject(fr);
        }
        mwStatus(w, h);
    }
}

static void barrierImage(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                         VkAccessFlags srcA, VkAccessFlags dstA)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    pCmdBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Create the host-visible linear panel image + memory, once.
static bool g_titleSurfaceNeedsUpload=true;
static bool BuildPanelImage()
{
    if (g_panelBuilt) return true;
    if (!pCreateImage || !pAllocMem || !pMapMem) return false;
    g_s = UiScale(); g_panelW = (int)g_scExtent.width; g_panelH = (int)g_scExtent.height;
    if (g_panelW > (int)g_scExtent.width) g_panelW = (int)g_scExtent.width; if (g_panelH > (int)g_scExtent.height) g_panelH = (int)g_scExtent.height;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = g_scFormat;
    ici.extent = { (uint32_t)g_panelW, (uint32_t)g_panelH, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR; ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (pCreateImage(g_dev, &ici, nullptr, &g_panelImg) != VK_SUCCESS) { Log("[menu] vk: panel image create failed\n"); return false; }
    VkMemoryRequirements mr; pImgMemReq(g_dev, g_panelImg, &mr);
    // pick a host-visible memory type by trying to map each allowed type (no
    // physical device needed).
    bool ok = false;
    for (uint32_t ti = 0; ti < 32 && !ok; ti++) {
        if (!(mr.memoryTypeBits & (1u << ti))) continue;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = ti;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (pAllocMem(g_dev, &mai, nullptr, &m) != VK_SUCCESS) continue;
        void* ptr = nullptr;
        if (pMapMem(g_dev, m, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS && ptr) {
            g_panelMem = m; g_panelPtr = ptr; ok = true;
            Log("[menu] vk: panel mem type=%u mapped\n", ti);
        }
        // (a failed type leaks its allocation; acceptable one-shot)
    }
    if (!ok) { Log("[menu] vk: no mappable memory type for panel\n"); return false; }
    pBindImgMem(g_dev, g_panelImg, g_panelMem, 0);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl; pImgSubLayout(g_dev, g_panelImg, &sub, &sl);
    g_panelPitch = (size_t)sl.rowPitch;
    // transition PREINITIALIZED -> TRANSFER_SRC_OPTIMAL, once
    VkCommandBuffer cb = g_cmd[0]; pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    barrierImage(cb, g_panelImg, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    pEndCB(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (pResetFences(g_dev, 1, &g_fence) != VK_SUCCESS || pSubmit(g_qFromFam ? g_qFromFam : VK_NULL_HANDLE, 1, &si, g_fence) != VK_SUCCESS) {
        g_rFail = true; return false;
    }
    // Wait: BuildBackdropImage runs next and resets this very command buffer and
    // fence, which is invalid while this submission is pending (review, 2026-09-01).
    if (pWaitFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        g_rFail = true;
        return false;
    }
    g_panelBuilt = true;
    g_titleSurfaceNeedsUpload=true;
    Log("[menu] vk: panel image ready %dx%d pitch=%zu\n", g_panelW, g_panelH, g_panelPitch);
    return true;
}

// backdrop: a second host-visible linear image the game frame is copied INTO under
// the button, so the native-look overlay can alpha-blend instead of overwrite.
static VkImage        g_bdImg = VK_NULL_HANDLE;
static VkDeviceMemory g_bdMem = VK_NULL_HANDLE;
static void*          g_bdPtr = nullptr;
static size_t         g_bdPitch = 0;
static bool           g_bdBuilt = false, g_bdFail = false;
static VkImageUsageFlags g_scUsage = 0;   // from myCreateSwapchain: the backdrop copy needs TRANSFER_SRC
static bool BuildBackdropImage()
{
    if (g_bdBuilt) return true; if (g_bdFail) return false;
    if (g_scUsage && !(g_scUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        g_bdFail = true;
        Log("[menu] vk: swapchain has no TRANSFER_SRC (usage=0x%x) -- panel backdrop disabled\n", g_scUsage);
        return false;
    }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = g_scFormat;
    ici.extent = { (uint32_t)g_panelW, (uint32_t)g_panelH, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR; ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (pCreateImage(g_dev, &ici, nullptr, &g_bdImg) != VK_SUCCESS) { g_bdFail = true; Log("[menu] vk: backdrop image create failed\n"); return false; }
    VkMemoryRequirements mr; pImgMemReq(g_dev, g_bdImg, &mr);
    bool ok = false;
    for (uint32_t ti = 0; ti < 32 && !ok; ti++) {
        if (!(mr.memoryTypeBits & (1u << ti))) continue;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = ti;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (pAllocMem(g_dev, &mai, nullptr, &m) != VK_SUCCESS) continue;
        void* ptr = nullptr;
        if (pMapMem(g_dev, m, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS && ptr) { g_bdMem = m; g_bdPtr = ptr; ok = true; }
    }
    if (!ok) { g_bdFail = true; Log("[menu] vk: no mappable memory for backdrop\n"); return false; }
    pBindImgMem(g_dev, g_bdImg, g_bdMem, 0);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl; pImgSubLayout(g_dev, g_bdImg, &sub, &sl);
    g_bdPitch = (size_t)sl.rowPitch;
    VkCommandBuffer cb = g_cmd[0]; pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    barrierImage(cb, g_bdImg, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    pEndCB(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (pResetFences(g_dev, 1, &g_fence) != VK_SUCCESS || pSubmit(g_qFromFam ? g_qFromFam : VK_NULL_HANDLE, 1, &si, g_fence) != VK_SUCCESS) {
        g_rFail = true; return false;
    }
    if (pWaitFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        g_rFail = true;
        return false;
    }
    g_bdBuilt = true;
    Log("[menu] vk: backdrop image ready pitch=%zu\n", g_bdPitch);
    return true;
}
// Pull the region under the button out of this frame's swapchain image (usage has
// TRANSFER_SRC) so the CPU can blend the label over it.
static bool CopyBackdrop(VkQueue q, uint32_t imgIndex)
{
    VkCommandBuffer cb = g_cmd[imgIndex]; pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
    region.srcOffset = { g_panelX, g_panelY, 0 };
    region.extent = { (uint32_t)g_copyW, (uint32_t)g_copyH, 1 };
    pCmdCopyImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_bdImg, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    barrierImage(cb, g_bdImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    pEndCB(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (pResetFences(g_dev, 1, &g_fence) != VK_SUCCESS || pSubmit(q, 1, &si, g_fence) != VK_SUCCESS) {
        g_rFail = true; return false;
    }
    if (pWaitFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        g_rFail = true;
        return false;
    }
    if (pInvalidate) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; r.memory = g_bdMem; r.size = VK_WHOLE_SIZE; pInvalidate(g_dev, 1, &r); }
    return true;
}
static void PanelLayout()
{
    g_s = UiScale();
    g_copyW = S(780); g_copyH = S(540);
    if (g_copyW > g_panelW) g_copyW = g_panelW; if (g_copyH > g_panelH) g_copyH = g_panelH;
    g_panelX = ((int)g_scExtent.width - g_copyW) / 2;
    g_panelY = ((int)g_scExtent.height - g_copyH) / 2;
}

// Both renderers, each frame the panel may show: false when it is collapsed (the
// native list entry IS the button then).  The public list is polled on its page.
static bool PanelFramePrep()
{
    if (InterlockedCompareExchange(&g_uiState, 0, 0) == 0) { g_hitCount = 0; return false; }
    if (InterlockedCompareExchange(&g_uiState, 0, 0) == 1) PubPoll();
    return true;
}

// Shared CPU composition for Vulkan and OpenGL. Title artwork is cached per viewport.
// dst is a full-frame surface; the in-game panel remains at its origin.
static bool ComposePanelIfDirty(void* dst, size_t pitch)
{
    const bool title=!WorldLoaded() && (g_uiState>=1 && g_uiState<=3);
    static bool lastTitle=false;
    static void* lastDst=nullptr;
    static int bgX=-1,bgY=-1,bgW=0,bgH=0;
    const bool surfaceChanged=lastDst!=dst || g_titleSurfaceNeedsUpload;
    const bool backgroundChanged=title && (surfaceChanged || !lastTitle || bgX!=g_panelX || bgY!=g_panelY || bgW!=g_panelW || bgH!=g_panelH);
    if(backgroundChanged) {
        PaintTitleBackdrop(dst,pitch,g_panelW,g_panelH,g_panelX,g_panelY,g_copyW,g_copyH);
        bgX=g_panelX;bgY=g_panelY;bgW=g_panelW;bgH=g_panelH;
    }
    static ULONGLONG lastRender=0;
    static int lastHover=-1,lastActive=-1;
    const ULONGLONG now=GetTickCount64();
    const bool requested=InterlockedExchange(&g_panelDirty,0)!=0;
    const bool dirty=surfaceChanged || backgroundChanged || title!=lastTitle || requested
        || g_layer.w!=g_copyW || g_layer.h!=g_copyH
        || g_hover!=lastHover || g_active!=lastActive || now-lastRender>500;
    if(!dirty) return false;
    RenderPanelLayer(g_copyW,g_copyH);
    if(title) {
        const auto* bg=g_titleBackdrop.data()+((size_t)g_panelY*g_panelW+g_panelX)*4;
        auto* panel=(unsigned char*)dst+(size_t)g_panelY*pitch+g_panelX*4;
        ComposeLayer(bg,(size_t)g_panelW*4,panel,pitch,g_copyW,g_copyH,true);
    } else ComposeLayer(nullptr,0,dst,pitch,g_copyW,g_copyH);
    lastTitle=title;lastDst=dst;lastRender=now;lastHover=g_hover;lastActive=g_active;
    g_titleSurfaceNeedsUpload=false;
    return true;
}

static void DrawButton(VkQueue q, uint32_t imgIndex)
{
    if(imgIndex>=g_scImgCount || !PanelFramePrep() || !BuildPanelImage()) return;
    PanelLayout();
    const bool title=!WorldLoaded() && (g_uiState>=1 && g_uiState<=3);
    if(ComposePanelIfDirty(g_panelPtr,g_panelPitch) && pFlush) {
        VkMappedMemoryRange r={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=g_panelMem;r.size=VK_WHOLE_SIZE;pFlush(g_dev,1,&r);
    }
    VkCommandBuffer cb = g_cmd[imgIndex]; pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
    region.dstOffset = { title?0:g_panelX, title?0:g_panelY, 0 };
    region.extent = { (uint32_t)(title?g_panelW:g_copyW), (uint32_t)(title?g_panelH:g_copyH), 1 };
    pCmdCopyImage(cb, g_panelImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    pEndCB(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (pResetFences(g_dev, 1, &g_fence) != VK_SUCCESS || pSubmit(q, 1, &si, g_fence) != VK_SUCCESS) {
        g_rFail = true; return;
    }
    if (pWaitFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) g_rFail = true;
}

// In-frame click: the button is not a window, so poll the cursor + left button
// against the button rect (converted to the game window's client area). One-shot
// per press, 1s debounce.
static void OnHit(int id, int button = 1);
// True only when the foreground window belongs to THIS game process. GetAsyncKeyState
// reads GLOBAL input, so without this gate the overlay would steal the user's mouse
// and keyboard while they are alt-tabbed to another app (e.g. typing in a terminal).
static bool gameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static volatile LONG g_pendingPanelClick=0, g_mouseInstalled=0;
static volatile LONG g_panelClickButton=1;   // 1 = left, 2 = right (chips cycle backwards)
static volatile LONG64 g_panelClickPoint=0;
static void PollClick()
{
    static bool prevDown = false, prevRDown = false;
    static ULONGLONG lastFire = 0;
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rdown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool clicked=InterlockedExchange(&g_pendingPanelClick,0)!=0;
    int button = clicked ? (int)InterlockedCompareExchange(&g_panelClickButton, 0, 0) : (down && !prevDown ? 1 : 2);
    // hover / pressed tracking for the native-look overlay (Button:hover / :active)
    {
        int hv = 0;
        if (gameHasFocus()) {
            POINT pt; GetCursorPos(&pt);
            if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = nullptr; EnumWindows(FindGameWnd, (LPARAM)&g_gameWnd); }
            POINT origin = { 0, 0 }; if (g_gameWnd) ClientToScreen(g_gameWnd, &origin);
            int lx = pt.x - origin.x - g_panelX, ly = pt.y - origin.y - g_panelY;
            for (int i = 0; i < g_hitCount; i++) { const Hit& hh = g_hits[i];
                if (lx >= hh.x && lx < hh.x + hh.w && ly >= hh.y && ly < hh.y + hh.h) { hv = hh.id; break; } }
        }
        g_hover = hv; g_active = (hv && down) ? hv : 0;
    }
    if ((clicked || (!g_mouseInstalled && ((down && !prevDown) || (rdown && !prevRDown)))) && gameHasFocus()) {
        POINT pt; GetCursorPos(&pt);
        if(clicked) { const auto packed=InterlockedCompareExchange64(&g_panelClickPoint,0,0);
            pt.x=(LONG)(packed&0xffffffff); pt.y=(LONG)((unsigned long long)packed>>32); }
        if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = nullptr; EnumWindows(FindGameWnd, (LPARAM)&g_gameWnd); }
        POINT origin = { 0, 0 };
        if (g_gameWnd) ClientToScreen(g_gameWnd, &origin);
        // panel-local click coords
        int lx = pt.x - origin.x - g_panelX;
        int ly = pt.y - origin.y - g_panelY;
        ULONGLONG now = GetTickCount64();
        if (now - lastFire > 400) {
            InterlockedExchange(&g_joinFocus, 0);
            for (int i = 0; i < g_hitCount; i++) {
                const Hit& hh = g_hits[i];
                if (lx >= hh.x && lx < hh.x + hh.w && ly >= hh.y && ly < hh.y + hh.h) {
                    lastFire = now; OnHit(hh.id, button); break;
                }
            }
        }
    }
    prevDown = down; prevRDown = rdown;
}

// OPEN LOGS remains a worker operation; it must not block presentation.
static volatile LONG g_logsBusy = 0;
static DWORD WINAPI CollectLogsThread(LPVOID)
{
    Tpf2mpLogArchive a;
    bool ok = Tpf2mpArchiveLogsSafe(false, ourDirW(), &a);
    Log("[menu] OPEN LOGS: %d file(s) copied, %d unreadable\n", a.files, a.skipped);
    wchar_t cmd[MAX_PATH * 2 + 40];
    if (ok) _snwprintf_s(cmd, _TRUNCATE, L"explorer.exe /select,\"%s\"", a.folder);
    else    _snwprintf_s(cmd, _TRUNCATE, L"explorer.exe \"%s\"", a.root);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetStatus(ok ? "Logs gathered in %LOCALAPPDATA%\\tpf2mp\\logs (opened in Explorer). Send the newest folders with a bug report."
                 : "No logs were found to gather.");
    InterlockedExchange(&g_logsBusy, 0);
    return 0;
}

static void OnHit(int id, int button)
{
    if (button == 2 && !(id >= 20 && id <= 35)) return;   // right-click: company chips only
    Log("[menu] hit id=%d\n", id);
    if (id>=90 && id<100+SAVE_ROWS) {
        if (!g_isHost || WorldLoaded() || g_sessionStarted) return;
        if (g_saveStartPending) { SetStatus("The selected save is being shared. Wait for the transfer to finish."); return; }
        if (id==90 || id==92) { RefreshLobbySaves(); g_savePicker=true; }
        else if (id==91) g_savePicker=false;
        else if (id==93 && g_savePage>0) --g_savePage;
        else if (id==94 && (g_savePage+1)*SAVE_ROWS<(int)g_lobbySaves.size()) ++g_savePage;
        else if (id>=100 && g_savePicker) {
            int i=g_savePage*SAVE_ROWS+id-100;
            if (i<(int)g_lobbySaves.size()) {
                g_selectedSave=g_lobbySaves[i].path;
                g_savePicker=false;
                if (g_lobbyReady) {
                    std::string line="{\"cmd\":\"advertise_mods\",\"save\":\""+jsonEscape(utf8Of(g_selectedSave.c_str()).c_str())+"\"}";
                    LobbySend(line.c_str());
                }
                SetStatus("Save selected. Press START GAME to share it.");
            }
        }
        InterlockedExchange(&g_panelDirty,1);
        return;
    }
    if(id>=110 && id<=115) {
        if(WorldLoaded() && id<114) return;
        if(g_uiState==1 && id<=113) {
            if(id<=111) { g_titleTab=id-110; InterlockedExchange(&g_joinFocus,0); }
            else g_titleServerPage=(std::max)(0,g_titleServerPage+(id==112?-1:1));
        } else if(g_uiState==2 && id>=114) g_titlePlayerPage=(std::max)(0,g_titlePlayerPage+(id==114?-1:1));
        InterlockedExchange(&g_panelDirty,1);
        return;
    }
    switch (id) {
    case 19:
        g_flagShareMods = g_flagShareMods==1 ? 0:1;
        ModDownloadPreference(true);
        if (g_flagShareMods==1 && g_modsPrompt[0]) OnHit(16);
        InterlockedExchange(&g_panelDirty,1);
        break;
    case 16: case 17: {   // YES / NO to the mod download
        const bool yes = (id == 16);
        if (g_csInit) { EnterCriticalSection(&g_statusCs); g_modsPrompt[0] = 0; LeaveCriticalSection(&g_statusCs); }
        char answer[128]; snprintf(answer,sizeof(answer),"{\"cmd\":\"mods\",\"accept\":%s,\"offer\":%d}",yes ? "true":"false",g_modsOffer); LobbySend(answer);
        SetStatus(yes ? "Downloading the mods this save needs from the host\xE2\x80\xA6" : "Mod download declined.");
        InterlockedExchange(&g_panelDirty, 1);
        break;
    }
    case 15:   // OPEN LOGS
        if (!InterlockedExchange(&g_logsBusy, 1)) {
            SetStatus("Gathering logs...");
            HANDLE t = CreateThread(nullptr, 0, CollectLogsThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t); else InterlockedExchange(&g_logsBusy, 0);
        }
        break;
    case 88: { // The host keeps playing: every game's panel closes and stays closed for this world.
        if(!InterlockedCompareExchange(&g_isHost,0,0)) break;
        static LONG declineNo = 0;
        char line[160]; snprintf(line,sizeof(line),"{\"cmd\":\"sync_decline\",\"id\":\"decline-%lu-%llu-%ld\"}",
            GetCurrentProcessId(), GetTickCount64(), InterlockedIncrement(&declineNo));
        LobbySend(line);
        EnterCriticalSection(&g_modelCs);
        if(!strcmp(g_recoveryPhase,"detected") || !strcmp(g_recoveryPhase,"unavailable") || !strcmp(g_recoveryPhase,"manual")) {
            g_recoveryPhase[0]=0;
            InterlockedExchange(&g_uiState,0); InterlockedExchange(&g_recoveryPresent,0);
            InterlockedExchange(&g_panelDirty,1);
        }
        LeaveCriticalSection(&g_modelCs);
    } break;
    case 85: // Dismiss a preflight notice; never hide a held operation.
        EnterCriticalSection(&g_modelCs);
        if(!strcmp(g_recoveryPhase,"unavailable") || !strcmp(g_recoveryPhase,"manual") || !strcmp(g_recoveryPhase,"detected")) {
            InterlockedExchange(&g_uiState,0); InterlockedExchange(&g_recoveryPresent,0);
            InterlockedExchange(&g_panelDirty,1);
        }
        LeaveCriticalSection(&g_modelCs);
        break;
    case 87: // Always reachable from the host lobby, even if only a client detected drift.
        if(!g_isHost || !WorldLoaded()) break;
        EnterCriticalSection(&g_modelCs);
        if(!g_recoveryPhase[0] || !strcmp(g_recoveryPhase,"complete") ||
           !strcmp(g_recoveryPhase,"unavailable")) {
            strcpy_s(g_recoveryPhase,"manual");
            g_recoveryDetail[0]=0;
        }
        InterlockedExchange(&g_recoveryPresent,1);
        InterlockedExchange(&g_uiState,3);
        InterlockedExchange(&g_panelDirty,1);
        LeaveCriticalSection(&g_modelCs);
        break;
    case 82: case 84: case 86: {
        char operation[40],token[40];
        EnterCriticalSection(&g_modelCs);
        bool allowed = id==86 ? (!strcmp(g_recoveryPhase,"readiness") && !g_readyMine) : id==82 ? !strcmp(g_recoveryPhase,"error") :
            (!strcmp(g_recoveryPhase,"manual") || !strcmp(g_recoveryPhase,"detected") || !strcmp(g_recoveryPhase,"waiting") || !strcmp(g_recoveryPhase,"aborted"));
        if(!allowed || g_recoveryRequestedAt || (id!=86 && !g_isHost)) { LeaveCriticalSection(&g_modelCs); break; }
        g_recoveryRequestedAt=GetTickCount64();
        strcpy_s(operation,g_recoveryOperation);
        strcpy_s(token,g_readyToken);
        LeaveCriticalSection(&g_modelCs);
        InterlockedExchange(&g_panelDirty,1);
        static LONG requestNo = 0;
        char line[320]; snprintf(line,sizeof(line),"{\"cmd\":\"%s\",\"operation\":\"%s\",\"token\":\"%s\",\"id\":\"native-%lu-%llu-%ld\"}",
            id==86?"sync_ready":id==82?"sync_retry":"sync_request",operation,token,
            GetCurrentProcessId(), GetTickCount64(), InterlockedIncrement(&requestNo));
        if(!LobbySend(line)) {
            EnterCriticalSection(&g_modelCs);
            g_recoveryRequestedAt=0;
            strcpy_s(g_recoveryDetail,"Could not send the request to the lobby. Reopen Manage Lobby and try again.");
            InterlockedExchange(&g_panelDirty,1);
            LeaveCriticalSection(&g_modelCs);
        }
    } break;
    case 4: InterlockedExchange(&g_ingameOverlay, 0); InterlockedExchange(&g_uiState, 0); InterlockedExchange(&g_panelDirty, 1); break; // collapse
    case 2: StartLobby(0); break;   // HOST  -> lobby (host)
    case 3: if (WorldLoaded()) SetStatus("Return to the main menu to join another world."); else StartLobby(1); break;
    case 5: if (!WorldLoaded()) LeaveLobby(); break;                // title-menu LEAVE only
    case 6: if (InterlockedCompareExchange(&g_isHost,0,0)) {   // START GAME (host): share the selected save
        // Hosting from a running map already starts its snapshot/hot-join flow.
        // Dismiss the panel without resending the world or waiting for loaders.
        if (WorldLoaded()) { OnHit(4); break; }
        // lobby.py truncates lobby_in.jsonl when it starts: a command appended
        // before its first event line would be lost. Wait for that first line.
        if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) { SetStatus("Lobby is starting…"); break; }
        if (g_saveStartPending) { SetStatus("The selected save is being shared. Please wait."); break; }
        if (g_selectedSave.empty()) { OnHit(90); SetStatus("Choose a save before starting."); break; }
        DWORD attributes=GetFileAttributesW(g_selectedSave.c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) {
            SetStatus("The selected save is no longer available. Choose another save."); break;
        }
        if (wcscpy_s(g_startSaveW,g_selectedSave.c_str())==0) {
            std::string line = "{\"cmd\":\"start\",\"save\":\"" + jsonEscape(utf8Of(g_startSaveW).c_str()) + "\"}";
            InterlockedExchange(&g_saveStartPending,1);
            if (LobbySend(line.c_str())) { SetStatus("Sharing save & starting game…"); MarkSaveShared(); }
            else { InterlockedExchange(&g_saveStartPending,0); SetStatus("Could not send the start request. Please try again."); }
        }
    } break;
    case 7: if (InterlockedCompareExchange(&g_haveCode,0,0)) { ClipboardSet(g_code); SetStatus("Code copied to clipboard — share it in Discord."); } break;
    case 10: InterlockedExchange(&g_joinFocus, 2); InterlockedExchange(&g_panelDirty, 1); break;   // password field
    case 13: InterlockedExchange(&g_joinFocus, 3); g_userLen = (int)strlen(g_username); InterlockedExchange(&g_panelDirty, 1); break;   // player name
    case 14: InterlockedExchange(&g_joinFocus, 4); g_lobbyNameLen = (int)strlen(g_lobbyName); InterlockedExchange(&g_panelDirty, 1); break;   // lobby name
    case 11: {   // PUBLIC checkbox; while hosting it toggles the announcement live
        LONG on = InterlockedCompareExchange(&g_public, 0, 0) ? 0 : 1; InterlockedExchange(&g_public, on);
        if (InterlockedCompareExchange(&g_uiState, 0, 0) == 2 && InterlockedCompareExchange(&g_isHost, 0, 0)) {
            if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) SetStatus("Lobby is starting…");
            else { LobbySend(on ? "{\"cmd\":\"publish\",\"on\":true}" : "{\"cmd\":\"publish\",\"on\":false}");
                   SetStatus(on ? "Listed in the public server browser." : "Removed from the public server browser."); }
        } else SetStatus(on ? "Your game will be listed publicly when you host." : "Your game will not be listed.");
        InterlockedExchange(&g_panelDirty, 1); } break;
    case 12: InterlockedExchange(&g_pubForce, 1); g_pubLast = 0; SetStatus("Refreshing the public game list…"); break;
    case 50: {   // SEPARATE COMPANIES checkbox; while hosting the lobby re-assigns every chip at once
        LONG on = InterlockedCompareExchange(&g_sepCompanies, 0, 0) ? 0 : 1; InterlockedExchange(&g_sepCompanies, on);
        if (InterlockedCompareExchange(&g_uiState, 0, 0) == 2 && InterlockedCompareExchange(&g_isHost, 0, 0)) {
            if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) SetStatus("Lobby is starting…");
            else { LobbySend(on ? "{\"cmd\":\"mode\",\"mode\":\"companies\"}" : "{\"cmd\":\"mode\",\"mode\":\"coop\"}");
                   SetStatus(on ? "Separate companies: every player gets their own company." : "Co-op: everyone plays company 1 together."); }
        } else SetStatus(on ? "Players will each get their own company." : "Players will share one company.");
        InterlockedExchange(&g_panelDirty, 1); } break;
    case 51: {   // CROSS-PLAY checkbox; while hosting the lobby switches its code live
        LONG on = InterlockedCompareExchange(&g_crossplay, 0, 0) ? 0 : 1; InterlockedExchange(&g_crossplay, on);
        if (InterlockedCompareExchange(&g_uiState, 0, 0) == 2 && InterlockedCompareExchange(&g_isHost, 0, 0)) {
            if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) SetStatus("Lobby is starting…");
            else LobbySend(on ? "{\"cmd\":\"crossplay\",\"on\":true}" : "{\"cmd\":\"crossplay\",\"on\":false}");
        } else SetStatus(on ? "Cross-play: you will share a classic code that players without Steam can use too."
                            : "Steam only: your Steam ID will be the join code.");
        InterlockedExchange(&g_panelDirty, 1); } break;
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: {   // a public game row -> its code goes into the join field
        int i = id - 40; char code[256] = ""; char name[NAME_MAX] = ""; bool locked = false;
        if (g_pubCsInit) { EnterCriticalSection(&g_pubCs); if (i < g_pubCount) { strcpy_s(code, g_pub[i].code); strcpy_s(name, g_pub[i].name); locked = g_pub[i].locked; } LeaveCriticalSection(&g_pubCs); }
        if (code[0]) { strcpy_s(g_joinCode, code); g_joinLen = (int)strlen(g_joinCode); InterlockedExchange(&g_joinFocus, 1);
                       char st[200]; snprintf(st, sizeof(st), locked ? "%s's game needs its password: type it below, then JOIN GAME." : "%s's code is filled in -- press JOIN GAME.", name); SetStatus(st); }
        InterlockedExchange(&g_panelDirty, 1); } break;
    case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27:
    case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: {   // company chip
        int i = id - 20; std::string name; int cur = 1;
        if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); if (i < playerCount()) { name = g_players[i]; cur = g_companies[i]; } LeaveCriticalSection(&g_modelCs); }
        // Left click increases: the next company id somebody already uses, then
        // one brand-new id (which is then in use, so the next click makes another).
        // Right click (2026-09-16) decreases: the previous used id, and from the
        // lowest round to the highest in use. Only a left click creates a company.
        bool used[MAX_COMPANIES + 2] = {}; int maxUsed = 0;
        if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); for (int k = 0; k < playerCount(); k++) { int c2 = g_companies[k]; if (c2 >= 1 && c2 <= MAX_COMPANIES) { used[c2] = true; if (c2 > maxUsed) maxUsed = c2; } } LeaveCriticalSection(&g_modelCs); }
        int next = 0;
        bool fresh = maxUsed < MAX_COMPANIES;   // a brand-new id (maxUsed + 1) is on the ring
        if (button == 2) {
            for (int c2 = cur - 1; c2 >= 1; c2--) if (used[c2]) { next = c2; break; }
            if (!next) next = maxUsed;
            if (next == cur) break;   // the only company there is
        } else {
            for (int c2 = cur + 1; c2 <= maxUsed; c2++) if (used[c2]) { next = c2; break; }
            if (!next) next = (cur <= maxUsed && fresh) ? maxUsed + 1 : 1;
        }
        if (next < 1) next = 1;
        if (!name.empty()) { std::string line = "{\"cmd\":\"company\",\"player\":\"" + jsonEscape(name.c_str()) + "\",\"id\":" + std::to_string(next) + "}"; LobbySend(line.c_str()); }
    } break;
    case 8: {   // code field: a click on a code CLEARS it (2026-09-16), a click on the empty field pastes the clipboard
        InterlockedExchange(&g_joinFocus, 1);
        if (g_joinLen > 0) { g_joinCode[0] = 0; g_joinLen = 0; InterlockedExchange(&g_panelDirty, 1); break; }
        if (g_joinLen == 0) { char buf[256]; if (ClipboardGet(buf, sizeof(buf))) { int j = 0; for (int i = 0; buf[i] && j < 200; i++) if ((unsigned char)buf[i] > 32) g_joinCode[j++] = buf[i]; g_joinCode[j] = 0; g_joinLen = j; } }
        InterlockedExchange(&g_panelDirty, 1); } break;
    case 9: break;   // chat field is always focused in the lobby
    }
}

// The per-frame work both renderers run -- the lobby's events, leaving the lobby with the
// world, the load stage, the dedicated server's tick, the frame meter. It lived in the
// Vulkan present hook alone, so on the OpenGL renderer none of it ran (2026-09-21).
// Returns the frame number.
static LONG FrameTick()
{
    PollLobbyOpen();
    if (InterlockedExchange(&g_leaveOnMenu, 0)) {
        // the player left the world for the title menu: leave the lobby with it
        // (the host leaving ends the session for everyone, as LEAVE would)
        Log("[menu] the world was left for the title menu -- leaving the lobby\n");
        LeaveLobby();
        SetStatus("Left the lobby: you left the world. HOST or JOIN to play again.");
    }
    SteamNameTick();
    StageTick();
    PollWorldGen();
    DedicatedTick();
    LONG n = InterlockedIncrement(&g_presentCount);
    {   // The frame meter: frames per second and the LONGEST frame of the last
        // 5 s, in the log. The longest frame is the stutter number (a 60 fps
        // average hides a 200 ms hitch). An overlay's own FPS counter (NVIDIA's)
        // hooks the same present call and does not attach beside this detour
        // (2026-09-20); Steam's counter does. Render thread only: plain statics.
        static LARGE_INTEGER freq{}, last{}, windowStart{}; static double longest = 0; static LONG frames = 0;
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); last = windowStart = t; }
        double dt = double(t.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart); last = t;
        if (dt > longest) longest = dt;
        frames++;
        double win = double(t.QuadPart - windowStart.QuadPart) / double(freq.QuadPart);
        if (win >= 5.0) {
            Log("[menu] frames: %.0f/s, longest %.0f ms, over %.1f s\n", frames / win, longest, win);
            frames = 0; longest = 0; windowStart = t;
        }
    }
    if ((n & 63) == 0 && InterlockedCompareExchange(&g_autoLoadPending, 0, 0) && GetTickCount64() - g_autoLoadSince > 12000) {
        // no menu frame took the load (not on a screen whose update runs): say how to load it by hand
        InterlockedExchange(&g_autoLoadPending, 0);
        Log("[menu] autoload: no menu frame picked the load up within 12 s -- the player loads mp_shared\n");
        SetStatus("Save ready -- open LOAD GAME and pick \"mp_shared\".");
    }
    return n;
}

// Both renderers: whether the panel draws this frame, after the loading view hands over
// to the in-game panel once the world is up. `quiet`: no world I/O in flight, clicks count.
static bool OverlayWanted(bool& quiet)
{
    const bool loadingPanel = InterlockedCompareExchange(&g_loadingPanel, 0, 0) != 0;
    if (loadingPanel && WorldLoaded()) {
        // the world is up: the loading view hands over to the in-game panel, open
        InterlockedExchange(&g_loadingPanel, 0);
        InterlockedExchange(&g_showOverlay, 0);
        InterlockedExchange(&g_ingameOverlay, 1);
        InterlockedExchange(&g_uiState, 2);
        InterlockedExchange(&g_panelDirty, 1);
    }
    quiet = !g_recoveryWorldIo && !NativeIo::Busy();
    return !NoRender() && (InterlockedCompareExchange(&g_showOverlay, 0, 0) || InterlockedCompareExchange(&g_ingameOverlay, 0, 0) || g_recoveryPresent)
        && (quiet || loadingPanel);
}

static VkResult myPresent(VkQueue q, const VkPresentInfoKHR* pi)
{
    const LONG n = FrameTick();
    if (n == 1) Log("[menu] PRESENT #1 swapchains=%u dev=%p\n", pi->swapchainCount, g_dev);
    if (n % 300 == 0) Log("[menu] present state: show=%ld swc=%u rInit=%d rFail=%d dev=%p fam=%u fmt=%d\n",
        InterlockedCompareExchange(&g_showOverlay, 0, 0), pi->swapchainCount,
        (int)g_rInit, (int)g_rFail, g_dev, g_qfam, (int)g_scFormat);
    __try {
        bool quiet = false;
        if (OverlayWanted(quiet) && pi->swapchainCount >= 1) {
            VkSwapchainKHR sc = pi->pSwapchains[0];
            uint32_t idx = pi->pImageIndices[0];
            if ((!g_rInit || sc != g_theSc) && !g_rFail) InitRender(sc);
            if (g_rInit && !g_rFail && sc == g_theSc) { DrawButton(q, idx); if (quiet) PollClick(); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool once = false;
        if (!once) { once = true; Log("[menu] myPresent FAULT exc=%lx (rInit=%d rFail=%d)\n",
            GetExceptionCode(), (int)g_rInit, (int)g_rFail); }
    }
    if (NoWsi() && pi) {
        // the window system never sees this frame: consume the wait semaphores,
        // report success for every swapchain, pace the loop to NORENDER_FPS
        NullSignal(q, VK_NULL_HANDLE, VK_NULL_HANDLE, pi->waitSemaphoreCount, pi->pWaitSemaphores);
        if (pi->pResults) for (uint32_t i = 0; i < pi->swapchainCount; ++i) pi->pResults[i] = VK_SUCCESS;
        NullPace(NORENDER_FPS);
        return VK_SUCCESS;
    }
    if (NoRender()) NullPace((double)g_flagDedFps);   // headless with the window system: dedicated_fps
    return g_realPresent(q, pi);
}

// ---------------- OpenGL: the same panel on the OpenGL renderer ----------------
// SDL2.dll presents an OpenGL frame through GDI32!SwapBuffers; InstallGlPresentHook patches
// that import. Each frame: the same FrameTick, the same CPU-composed panel, moved onto the
// window by gl_overlay.h (a texture upload when it changed, a framebuffer blit, every
// touched piece of GL state put back). The game's context is current on this thread.
typedef BOOL (WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn g_origSwapBuffers = nullptr;
static HGLRC (WINAPI* g_wglGetCurrentContext)() = nullptr;
static GlOverlay::State g_gl;
static std::vector<unsigned char> g_glPanel;   // the composed panel, CPU side
static size_t g_glPitch = 0;
static bool g_glPanelBuilt = false, g_glLoggedFail = false;
static void DrawPanelGL(HDC hdc)
{
    HWND wnd = WindowFromDC(hdc);
    RECT rc = {};
    if (!wnd || !GetClientRect(wnd, &rc)) return;
    const int fbW = rc.right - rc.left, fbH = rc.bottom - rc.top;
    if (fbW <= 0 || fbH <= 0) return;
    if ((int)g_scExtent.width != fbW || (int)g_scExtent.height != fbH) {
        // g_scExtent is what UiScale and PanelLayout size against (the swapchain on Vulkan)
        g_scExtent.width = (uint32_t)fbW; g_scExtent.height = (uint32_t)fbH;
        g_glPanelBuilt = false;
        Log("[menu] OpenGL: window %dx%d\n", fbW, fbH);
    }
    if (!PanelFramePrep()) return;
    if (!GlOverlay::Load(g_gl)) return;
    if (!g_glPanelBuilt) {
        g_s = UiScale(); g_panelW = fbW; g_panelH = fbH;   // full-frame title backdrop, as on Vulkan
        if (g_panelW > fbW) g_panelW = fbW; if (g_panelH > fbH) g_panelH = fbH;
        g_glPitch = (size_t)g_panelW * 4;
        g_glPanel.assign(g_glPitch * (size_t)g_panelH, 0);
        if (!GlOverlay::Ensure(g_gl, g_panelW, g_panelH)) return;
        g_glPanelBuilt = true;
        g_titleSurfaceNeedsUpload = true;
        InterlockedExchange(&g_panelDirty, 1);
        Log("[menu] OpenGL: panel texture %dx%d\n", g_panelW, g_panelH);
    }
    PanelLayout();
    const bool upload = ComposePanelIfDirty(g_glPanel.data(), g_glPitch);
    const bool title=!WorldLoaded() && (g_uiState>=1 && g_uiState<=3);
    GlOverlay::Draw(g_gl, g_glPanel.data(), g_glPitch, title?g_panelW:g_copyW, title?g_panelH:g_copyH, upload, title?0:g_panelX, title?0:g_panelY, fbH);
}
// SEH only in this frame (no C++ objects): a fault turns the OpenGL overlay off, never the game.
static void GlFrame(HDC hdc)
{
    __try {
        bool quiet = false;
        if (!g_gl.failed && OverlayWanted(quiet)) { DrawPanelGL(hdc); if (quiet) PollClick(); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_gl.failed = true;
        Log("[menu] OpenGL: FAULT exc=%lx while drawing the panel\n", GetExceptionCode());
    }
    if (g_gl.failed && !g_glLoggedFail) {
        g_glLoggedFail = true;
        Log("[menu] OpenGL overlay off: %s\n", g_gl.why[0] ? g_gl.why : "a fault while drawing");
    }
}
static BOOL WINAPI mySwapBuffers(HDC hdc)
{
    if (g_wglGetCurrentContext && g_wglGetCurrentContext()) {
        const LONG n = FrameTick();
        if (n == 1) Log("[menu] OpenGL: first SwapBuffers (hdc=%p) -- the Multiplayer panel draws with OpenGL\n", hdc);
        if (n % 300 == 0) Log("[menu] OpenGL present state: show=%ld loaded=%d failed=%d %ux%u\n",
            InterlockedCompareExchange(&g_showOverlay, 0, 0), (int)g_gl.loaded, (int)g_gl.failed, g_scExtent.width, g_scExtent.height);
        GlFrame(hdc);
    }
    return g_origSwapBuffers ? g_origSwapBuffers(hdc) : FALSE;
}
// Point `mod`'s import of `func` from `dll` at `repl`. *orig is written BEFORE the slot,
// so a call landing in between already finds the real function.
static bool PatchImport(HMODULE mod, const char* dll, const char* func, void* repl, void** orig)
{
    uint8_t* base = (uint8_t*)mod;
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const IMAGE_IMPORT_BY_NAME* byName = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *orig = (void*)slots->u1.Function;
            InterlockedExchangePointer((PVOID*)&slots->u1.Function, repl);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}
static void InstallGlPresentHook()
{
    // The game itself imports only wglGetProcAddress/wglGetCurrentContext; its window and
    // buffer swap are SDL2's, which imports GDI32!SwapBuffers (build 35924).
    HMODULE sdl = GetModuleHandleW(L"SDL2.dll"), ogl = GetModuleHandleW(L"opengl32.dll");
    if (ogl) g_wglGetCurrentContext = (HGLRC (WINAPI*)())GetProcAddress(ogl, "wglGetCurrentContext");
    if (!sdl) { Log("[menu] OpenGL: SDL2.dll is not loaded -- no OpenGL overlay\n"); return; }
    if (!g_wglGetCurrentContext) { Log("[menu] OpenGL: opengl32.dll is not loaded -- no OpenGL overlay\n"); return; }
    if (PatchImport(sdl, "GDI32.dll", "SwapBuffers", (void*)&mySwapBuffers, (void**)&g_origSwapBuffers))
        Log("[menu] OpenGL: hooked SDL2's SwapBuffers import (real=%p) -- the panel also draws on the OpenGL renderer\n", (void*)g_origSwapBuffers);
    else Log("[menu] OpenGL: SDL2.dll imports no GDI32!SwapBuffers -- no OpenGL overlay\n");
}

static void myGetQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* pQ)
{
    g_origGetQueue(dev, fam, idx, pQ);
    if (!g_qfamKnown) { g_qfam = fam; g_qFromFam = pQ ? *pQ : VK_NULL_HANDLE; g_qfamKnown = true;
        Log("[menu] captured queue family=%u\n", fam); }
}

static VkResult myCreateSwapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                  const VkAllocationCallbacks* a, VkSwapchainKHR* sc)
{
    VkResult r = g_origCreateSc(dev, ci, a, sc);
    if (r == VK_SUCCESS && ci) {
        if (!g_origGetScImages && g_origGdpa) g_origGetScImages = (PFN_vkGetSwapchainImagesKHR)g_origGdpa(dev, "vkGetSwapchainImagesKHR");
        uint32_t cnt = 0;
        if (g_origGetScImages && sc && g_origGetScImages(dev, *sc, &cnt, nullptr) == VK_SUCCESS && cnt > 0) {
            InterlockedExchange(&g_nullScCount, (LONG)cnt); InterlockedExchange(&g_nullScNext, 0);
            if (NoWsi()) Log("[menu] no-render: swapchain of %u images -- acquire and present are answered here, never by the window system\n", cnt);
        }
        g_scFormat = ci->imageFormat; g_scExtent = ci->imageExtent; g_scUsage = ci->imageUsage;
        g_rInit = false; g_rFail = false;   // rebuild on next present
        // The panel and backdrop images were created against the OLD format and
        // size; a graphics-settings change recreates the swapchain and left them
        // stale -- copies between mismatched formats, which is the corrupted
        // button texture reported after changing settings. The old images are
        // not freed (a rare event, and freeing under an in-flight present is the
        // riskier bug); they are simply rebuilt on the next present.
        g_panelBuilt = false; g_bdBuilt = false; g_bdFail = false;
        Log("[menu] swapchain created fmt=%d %ux%u usage=0x%x (TRANSFER_DST=%d)\n",
            (int)ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height,
            ci->imageUsage, (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? 1 : 0);
    }
    return r;
}

// an empty submit that only signals (acquire) or only waits (present): what the
// window system would have done to those semaphores and that fence
static void NullSignal(VkQueue q, VkSemaphore signalSem, VkFence fence, uint32_t waitCount, const VkSemaphore* waitSems)
{
    if (!g_origSubmit || q == VK_NULL_HANDLE) return;
    VkPipelineStageFlags stages[16];
    if (waitCount > 16) waitCount = 16;
    for (uint32_t i = 0; i < waitCount; ++i) stages[i] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitCount; si.pWaitSemaphores = waitCount ? waitSems : nullptr; si.pWaitDstStageMask = waitCount ? stages : nullptr;
    si.signalSemaphoreCount = signalSem != VK_NULL_HANDLE ? 1 : 0; si.pSignalSemaphores = signalSem != VK_NULL_HANDLE ? &signalSem : nullptr;
    g_origSubmit(q, 1, &si, fence);
}
static VkResult NullAcquire(VkSemaphore sem, VkFence fence, uint32_t* pIndex)
{
    LONG cnt = InterlockedCompareExchange(&g_nullScCount, 0, 0);
    if (cnt <= 0) cnt = 3;
    LONG i = InterlockedIncrement(&g_nullScNext) - 1;
    if (pIndex) *pIndex = (uint32_t)(i % cnt);
    NullSignal(g_qFromFam, sem, fence, 0, nullptr);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL myAcquire(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* pIndex)
{
    if (!NoWsi()) return g_origAcquire(dev, sc, timeout, sem, fence, pIndex);
    return NullAcquire(sem, fence, pIndex);
}
static VkResult VKAPI_CALL myAcquire2(VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex)
{
    if (!NoWsi()) return g_origAcquire2(dev, info, pIndex);
    return NullAcquire(info ? info->semaphore : VK_NULL_HANDLE, info ? info->fence : VK_NULL_HANDLE, pIndex);
}
// the frame pace: `fps` frames a second, slept here (the window system's pace,
// or the lack of one, no longer decides)
static void NullPace(double fps)
{
    static LARGE_INTEGER freq{}, next{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const LONGLONG period = (LONGLONG)(freq.QuadPart / (fps > 1.0 ? fps : 1.0));
    if (!next.QuadPart || now.QuadPart > next.QuadPart + 4 * period) next.QuadPart = now.QuadPart;   // first frame, or far behind: restart the grid
    next.QuadPart += period;
    LONGLONG wait = next.QuadPart - now.QuadPart;
    if (wait > 0) {
        DWORD ms = (DWORD)(wait * 1000 / freq.QuadPart);
        if (ms > 1) Sleep(ms - 1);
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
    }
}

// vkQueueSubmit with the command buffers taken out: the fence and the semaphores
// are signalled as the engine expects, nothing is rasterized. Our own overlay
// submits go through pSubmit (the real one) and are skipped anyway (myPresent).
static VkResult VKAPI_CALL mySubmit(VkQueue q, uint32_t n, const VkSubmitInfo* pSubmits, VkFence fence)
{
    if (!InterlockedCompareExchange(&g_noRender, 0, 0) || n == 0 || !pSubmits) return g_origSubmit(q, n, pSubmits, fence);
    VkSubmitInfo local[8];
    VkSubmitInfo* copy = (n <= 8) ? local : (VkSubmitInfo*)_alloca(sizeof(VkSubmitInfo) * n);
    LONG dropped = 0;
    for (uint32_t i = 0; i < n; ++i) {
        copy[i] = pSubmits[i];
        dropped += (LONG)copy[i].commandBufferCount;
        copy[i].commandBufferCount = 0;
        copy[i].pCommandBuffers = nullptr;
    }
    InterlockedIncrement(&g_noRenderSubmits);
    InterlockedAdd(&g_noRenderCmdBufs, dropped);
    return g_origSubmit(q, n, copy, fence);
}
// a query that never ran has no result: zeros, available now (a WAIT would never return)
static VkResult VKAPI_CALL myQueryResults(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t count,
                                          size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags)
{
    if (!InterlockedCompareExchange(&g_noRender, 0, 0)) return g_origQueryResults(dev, pool, first, count, dataSize, pData, stride, flags);
    if (pData && dataSize) memset(pData, 0, dataSize);
    return VK_SUCCESS;
}
static PFN_vkVoidFunction myGdpa(VkDevice dev, const char* name)
{
    PFN_vkVoidFunction real = g_origGdpa(dev, name);
    if (!name || !real) return real;
    if (strcmp(name, "vkQueueSubmit") == 0) {
        g_origSubmit = (PFN_vkQueueSubmit)real;
        if (NoRender()) Log("[menu] intercepted vkQueueSubmit dev=%p -- dedicated_render=0: no command buffer will run\n", dev);
        return (PFN_vkVoidFunction)mySubmit;
    }
    if (strcmp(name, "vkGetQueryPoolResults") == 0) {
        g_origQueryResults = (PFN_vkGetQueryPoolResults)real; return (PFN_vkVoidFunction)myQueryResults;
    }
    if (strcmp(name, "vkAcquireNextImageKHR") == 0) {
        g_origAcquire = (PFN_vkAcquireNextImageKHR)real; return (PFN_vkVoidFunction)myAcquire;
    }
    if (strcmp(name, "vkAcquireNextImage2KHR") == 0) {
        g_origAcquire2 = (PFN_vkAcquireNextImage2KHR)real; return (PFN_vkVoidFunction)myAcquire2;
    }
    if (NoRender() && (strcmp(name, "vkQueueSubmit2") == 0 || strcmp(name, "vkQueueSubmit2KHR") == 0))
        Log("[menu] the game resolves %s -- not intercepted, rendering may still run\n", name);
    if (strcmp(name, "vkQueuePresentKHR") == 0) {
        g_realPresent = (PFN_vkQueuePresentKHR)real; g_dev = dev;
        Log("[menu] intercepted vkQueuePresentKHR dev=%p real=%p\n", dev, real);
        return (PFN_vkVoidFunction)myPresent;
    }
    if (strcmp(name, "vkGetDeviceQueue") == 0) {
        g_origGetQueue = (PFN_vkGetDeviceQueue)real; return (PFN_vkVoidFunction)myGetQueue;
    }
    if (strcmp(name, "vkCreateSwapchainKHR") == 0) {
        g_origCreateSc = (PFN_vkCreateSwapchainKHR)real; return (PFN_vkVoidFunction)myCreateSwapchain;
    }
    return real;
}

// An MSVC std::string laid out for the game's helpers. Kept 32 bytes, 8-aligned.
struct GString { char buf[16]; size_t size; size_t cap; };
static void GStringInit(GString* s) { memset(s, 0, sizeof(*s)); s->cap = 15; }

// ---------------- native title-menu button ----------------
// What the page builder 667bc0 really does per entry (decompiled, mainmenu_ref.c):
//   btn = 7c5d30(221c930(&s, "Load Game"), &empty, &empty)   build the Button
//   227a1e0(btn, &"continue")                                 style class (optional)
//   2518f0(btn, &connOut, &std::function<void()>)             connect the click slot
//   227f880(btn, 4, 1)                                        set widget flag 4 (Exit clears it)
//   2d99e0(list, btn, &"list-item")                           ADD TO THE "MainMenu" LIST
// The list (a 0x508-byte widget named "MainMenu") is a local of the builder, so we
// hook the list's add call and remember the list pointer while the builder runs;
// after it returns we run the same five steps for our own entry. The click slot is
// an MSVC std::function whose in-place impl vtable is {Copy, Move, DoCall,
// TargetType, DeleteThis} (docs/re/GAME_LOOP_AND_UI.md) -- we supply a static one.
static const uintptr_t RVA_LIST_ADD = 0x22d99e0;  static const int STEAL_LIST_ADD = 15;   // push rdi; sub rsp,60; mov [rsp+20],-2
static const uintptr_t RVA_SETNAME  = 0x227a1e0;
static const uintptr_t RVA_PREP     = 0x227f880;
typedef void* (*ListAddFn)(void* list, void* widget, void* styleStr);
typedef void  (*SetNameFn)(void* widget, void* str);
typedef void  (*PrepFn)(void* widget, int flagBit, char value);   // 227f880 = setWidgetFlag(bit, on); the builder sets bit 4 on every entry (Exit clears it)
static ListAddFn g_origListAdd = nullptr;
static SetNameFn g_setName = nullptr;
static PrepFn    g_prep = nullptr;
static volatile LONG g_inMainBuild = 0;
static void* g_mainList = nullptr;
static int   g_mainListAdds = 0;
static void OnHit(int id, int button);   // default on the first declaration

struct FuncBase { const void* const* vptr; void* capture; };
static FuncBase* __fastcall MpCopy(const FuncBase* self, void* dest) { FuncBase* d = (FuncBase*)dest; d->vptr = self->vptr; d->capture = self->capture; return d; }
static FuncBase* __fastcall MpMove(FuncBase* self, void* dest)       { FuncBase* d = (FuncBase*)dest; d->vptr = self->vptr; d->capture = self->capture; return d; }
static void      __fastcall MpDoCall(FuncBase*)
{
    Log("[menu] NATIVE BUTTON CLICKED -> expanding the overlay panel\n");
    InterlockedExchange(&g_uiState, LobbyRunning() ? 2 : 1); InterlockedExchange(&g_panelDirty, 1);
}
static const void* __fastcall MpTargetType(const FuncBase* self) { return self; }   // never consulted by the signal
static void      __fastcall MpDeleteThis(FuncBase*, bool)          { }              // 16-byte impl is always in-place
static const void* const g_mpFuncVtbl[5] = { (const void*)&MpCopy, (const void*)&MpMove, (const void*)&MpDoCall, (const void*)&MpTargetType, (const void*)&MpDeleteThis };
struct GFunc { FuncBase impl; void* pad[5]; void* ptr; };   // MSVC std::function: 64 B, impl ptr at +0x38
static_assert(sizeof(GFunc) == 64, "std::function layout");

static void NativeInsert();
// Inserting AFTER the builder returned appended cleanly but never rendered (the
// list had already been laid out and attached). Now we insert DURING the build,
// right after the game's first add, so ours is laid out with the rest -- it should
// appear as the second entry of the column.
static void* MyListAdd(void* list, void* widget, void* style)
{
    bool inBuild = InterlockedCompareExchange(&g_inMainBuild, 0, 0) != 0;
    // slot=N: insert ours before the game's N-th add (0 = before the first entry,
    // which is CONTINUE when a save exists and otherwise the next one -- so we sit
    // at the top of the column either way).
    if (inBuild && g_mainListAdds == g_flagSlot) { g_mainList = list; NativeInsert(); }
    if (!g_origListAdd) return nullptr;   // hook live before its trampoline was recorded: never call address 0
    void* r = g_origListAdd(list, widget, style);
    if (inBuild) { g_mainList = list; g_mainListAdds++; Log("[menu] list add #%d list=%p widget=%p\n", g_mainListAdds, list, widget); }
    return r;
}

static void NativeInsert()
{
    __try {
        if (!g_mainList) { Log("[menu] native: builder made no list adds -- nothing to append to\n"); return; }
        alignas(16) unsigned char ctxBuf[128]; memset(ctxBuf, 0, sizeof(ctxBuf));
        void* text = g_actionCtx(ctxBuf, "Multiplayer");
        GString iconA, iconB; GStringInit(&iconA); GStringInit(&iconB);
        void* btn = g_btn(text ? text : ctxBuf, &iconA, &iconB);
        Log("[menu] native: list=%p (%d adds) button=%p vtbl=%p\n", g_mainList, g_mainListAdds, btn, btn ? *(void**)btn : nullptr);
        if (!btn) return;
        GString cls; GStringInit(&cls); g_strAssign(&cls, "multiplayer", 11);
        g_setName(btn, &cls);
        GFunc fn; memset(&fn, 0, sizeof(fn));
        fn.impl.vptr = g_mpFuncVtbl; fn.impl.capture = nullptr; fn.ptr = &fn.impl;
        void* conn[4] = { nullptr, nullptr, nullptr, nullptr };
        g_add(btn, conn, &fn);
        g_clean(conn);
        Log("[menu] native: click slot connected\n");
        g_prep(btn, 4, 1);
        GString li; GStringInit(&li); g_strAssign(&li, "list-item", 9);
        g_origListAdd(g_mainList, btn, &li);
        Log("[menu] native: BUTTON INSERTED into the MainMenu list during the build -- it goes at the top of the column\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[menu] native: FAULTED exc=%lx (button not inserted)\n", GetExceptionCode());
    }
}

static void MyMainBuild(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4)
{
    g_mainList = nullptr; g_mainListAdds = 0;
    InterlockedExchange(&g_inMainBuild, 1);
    g_origMainBuild(p1, p2, p3, p4);
    InterlockedExchange(&g_inMainBuild, 0);
    Log("[menu] main-page builder ran (list=%p adds=%d, ours inserted before add #%d)\n", g_mainList, g_mainListAdds, g_flagSlot);
}

// ---------------- game-window helpers ----------------
static BOOL CALLBACK FindGameWnd(HWND h, LPARAM lp)
{
    DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
    if (wpid == GetCurrentProcessId() && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
        RECT r; GetWindowRect(h, &r);
        if ((r.right - r.left) > 400) { *(HWND*)lp = h; return FALSE; }
    }
    return TRUE;
}

// ---------------- connect.py integration ----------------
static const wchar_t* NETDIR = L"netpunch";   // placeholder: resolveNetDir() replaces it at init

static void ClipboardSet(const char* utf8)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, wn * sizeof(wchar_t));
    if (g) { wchar_t* p = (wchar_t*)GlobalLock(g); MultiByteToWideChar(CP_UTF8, 0, utf8, -1, p, wn);
             GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}
static bool ClipboardGet(char* out, int outsz)
{
    out[0] = 0; if (!OpenClipboard(nullptr)) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT); bool ok = false;
    if (h) { wchar_t* p = (wchar_t*)GlobalLock(h);
             if (p) { WideCharToMultiByte(CP_UTF8, 0, p, -1, out, outsz, nullptr, nullptr); ok = out[0] != 0; GlobalUnlock(h); } }
    CloseClipboard(); return ok;
}
static bool ReadFileText(const wchar_t* path, char* buf, int sz)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0; bool ok = ReadFile(h, buf, sz - 1, &n, nullptr) != 0; buf[ok ? n : 0] = 0;
    CloseHandle(h); return ok && n > 0;
}
// pull "key":"value" out of a JSON-ish blob into dst (last occurrence)
// Extract a string value for `key`. Tolerates whitespace after the colon, since
// Python's json.dumps emits `"key": "value"` (a SPACE after the colon) -- matching
// on `"key":"` silently found nothing and broke every event dispatch.
static void jsonStr(const char* s, const char* key, char* dst, int dsz)
{
    dst[0] = 0; char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    const char* last = nullptr; const char* p = s;
    while ((p = strstr(p, pat)) != nullptr) {
        const char* q = p + plen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { q++; while (*q == ' ' || *q == '\t') q++;
                         if (*q == '"') last = q + 1; }
        p += plen;
    }
    if (!last) return;
    // dst holds a protocol-bounded field (a type, a phase, a token); a value the
    // roster feeds (names, the lobby title, a save path) goes through jsonStrS.
    std::string v = jsonUnquote(last);
    if ((int)v.size() > dsz - 1) { Log("[menu] jsonStr: \"%s\" is %zu bytes, the field holds %d -- cut\n", key, v.size(), dsz - 1); v.resize(dsz - 1); }
    memcpy(dst, v.c_str(), v.size() + 1);
}
// The same, any length: the string value for `key` (last occurrence), "" if absent.
static std::string jsonStrS(const char* s, const char* key)
{
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    const char* last = nullptr; const char* p = s;
    while ((p = strstr(p, pat)) != nullptr) {
        const char* q = p + plen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { q++; while (*q == ' ' || *q == '\t') q++;
                         if (*q == '"') last = q + 1; }
        p += plen;
    }
    if (!last) return std::string();
    return jsonUnquote(last);
}

// Extract a boolean for `key` (e.g. start "save"). Absent/unparseable -> dflt.
static bool jsonBool(const char* s, const char* key, bool dflt)
{
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat); if (!p) return dflt; p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++; if (*p != ':') return dflt; p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return dflt;
}

// Extract an integer value for `key` (e.g. transfer "pct"). -1 if absent.
static int jsonInt(const char* s, const char* key)
{
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(s, pat); if (!p) return -1; p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++; if (*p != ':') return -1; p++;
    while (*p == ' ' || *p == '\t') p++;
    int v = 0; bool any = false; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
    return any ? v : -1;
}

// ---- lobby.py: N-player host-relay lobby with roster + chat ----
static HANDLE g_lobbyProc = nullptr;
static HANDLE g_lobbyThread = nullptr;
// The lobby must never outlive the game. Quitting cleanly is handled by
// TeardownLobby, but a crash, a kill from Task Manager or Steam closing the game
// runs no cleanup at all -- and the orphan keeps UDP 29471 and the relay ports,
// so the NEXT session's lobby cannot bind them and the player gets a game that
// silently never connects. A job object with KILL_ON_JOB_CLOSE is the only
// mechanism that survives every one of those paths: when the game process dies
// its handles close, the job goes with them and Windows terminates whatever is
// inside. PyInstaller's onefile bootloader spawns a child; the child inherits
// the job, so both go.
static HANDLE g_lobbyJob = nullptr;
static void EnsureLobbyJob()
{
    if (g_lobbyJob) return;
    g_lobbyJob = CreateJobObjectW(nullptr, nullptr);
    if (!g_lobbyJob) { Log("[menu] CreateJobObject failed (err %lu) -- the lobby will not be killed automatically\n", GetLastError()); return; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_lobbyJob, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
        Log("[menu] SetInformationJobObject failed (err %lu)\n", GetLastError());
        CloseHandle(g_lobbyJob); g_lobbyJob = nullptr;
    }
}
// Random two-word username ("BraveOtter"), generated once per game session.
// The Windows account name was the old default: it leaks the player's real
// name into every lobby, and two instances on one machine got the SAME name,
// which the roster could not tell apart. Words are short, distinct and
// unambiguous when read aloud (the code is shared over voice/Discord anyway).
static const char* const NAME_ADJ[] = {
    "Brave","Calm","Clever","Crisp","Daring","Eager","Fancy","Fuzzy","Gentle","Giant",
    "Golden","Happy","Hasty","Icy","Jolly","Keen","Lucky","Merry","Mighty","Nimble",
    "Noble","Odd","Plucky","Proud","Quick","Quiet","Rapid","Rusty","Shiny","Silent",
    "Sleepy","Sly","Snowy","Solar","Spicy","Steady","Stormy","Swift","Tidy","Witty",
    "Zesty","Amber","Copper","Dusty","Frosty","Misty","Rosy","Sunny","Velvet","Wild" };
static const char* const NAME_NOUN[] = {
    "Otter","Badger","Falcon","Heron","Lynx","Moose","Panda","Raven","Tiger","Walrus",
    "Beaver","Bison","Camel","Dingo","Ferret","Gecko","Ibis","Jaguar","Koala","Lemur",
    "Marmot","Newt","Ocelot","Puffin","Quail","Rabbit","Salmon","Toucan","Urchin","Viper",
    "Wombat","Yak","Zebra","Engine","Signal","Depot","Tender","Boxcar","Caboose","Tram",
    "Ferry","Barge","Trolley","Wagon","Piston","Rail","Switch","Girder","Trestle","Viaduct" };
// Is this exactly one of the names ensureUsername makes up (an adjective from
// NAME_ADJ followed by a noun from NAME_NOUN, nothing else)? Such a name was
// never typed by the player, so it must not be kept over the Steam persona.
static bool IsGeneratedName(const char* n)
{
    if (!n || !n[0]) return false;
    for (size_t a = 0; a < sizeof(NAME_ADJ) / sizeof(NAME_ADJ[0]); a++) {
        const size_t la = strlen(NAME_ADJ[a]);
        if (strncmp(n, NAME_ADJ[a], la) != 0) continue;
        for (size_t b = 0; b < sizeof(NAME_NOUN) / sizeof(NAME_NOUN[0]); b++)
            if (strcmp(n + la, NAME_NOUN[b]) == 0) return true;
    }
    return false;
}
static void ensureUsername()
{
    if (g_username[0]) return;
    unsigned s = (unsigned)GetTickCount() ^ (GetCurrentProcessId() * 2654435761u);
    { FILETIME ft; GetSystemTimeAsFileTime(&ft); s ^= ft.dwLowDateTime; }
    s = s * 1103515245u + 12345u; unsigned a = (s >> 8) % (sizeof(NAME_ADJ) / sizeof(NAME_ADJ[0]));
    s = s * 1103515245u + 12345u; unsigned n = (s >> 8) % (sizeof(NAME_NOUN) / sizeof(NAME_NOUN[0]));
    snprintf(g_username, sizeof(g_username), "%s%s", NAME_ADJ[a], NAME_NOUN[n]);
    Log("[menu] username: %s\n", g_username);
}

static bool LobbySend(const char* jsonLine)   // append a command to lobby_in.jsonl
{
    wchar_t p[512]; _snwprintf_s(p, _TRUNCATE, L"%s\\lobby_in.jsonl", NETDIR);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w; std::string line(jsonLine); line += '\n';   // start lines carry a full save path: no fixed buffer here
    const bool ok = WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr) && w == (DWORD)line.size();
    CloseHandle(h);
    return ok;
}
static void SendChat(const char* text)
{
    if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) { SetStatus("Lobby is starting…"); return; }   // see g_lobbyReady
    // escape quotes/backslashes minimally
    char esc[400]; int j = 0; for (int i = 0; text[i] && j < 390; i++) { char c = text[i]; if (c == '"' || c == '\\') esc[j++] = '\\'; esc[j++] = c; } esc[j] = 0;
    char line[512]; snprintf(line, sizeof(line), "{\"cmd\":\"chat\",\"text\":\"%s\"}", esc);
    LobbySend(line);
}

// ---- bridge handshake via DATADIR (see bridge_main.cpp [ctl]) ----
// The bridge writes DATADIR\tpf2_instance.txt at init:
//   <letter>\npid=<pid>\nport=<bound UDP port>\n
// Line 3 is the port lobby.py must deliver the peer's frames to. Read it fresh
// right before every lobby launch (the bridge may have re-bound). Any line that
// starts with "port=" counts, so a file without it (older bridge) -> default.
static int readBridgePort()
{
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2_instance.txt", g_dataDirW);
    char buf[512];
    if (!ReadFileText(p, buf, sizeof(buf))) { Log("[menu] %ls unreadable -> bridge port default %d\n", p, BRIDGE_PORT_DEFAULT); return BRIDGE_PORT_DEFAULT; }
    const char* q = buf;
    while (q && *q) {
        int v = 0;
        if (sscanf_s(q, "port=%d", &v) == 1 && v > 0 && v < 65536) return v;
        q = strchr(q, '\n'); if (q) q++;
    }
    Log("[menu] no port= line in tpf2_instance.txt -> bridge port default %d\n", BRIDGE_PORT_DEFAULT);
    return BRIDGE_PORT_DEFAULT;
}

// Tell the bridge its role and where to send frames. Written atomically (tmp +
// replace) so the bridge's 500 ms poll never reads a half-written file. Skipped
// when the content is unchanged (the bridge ignores no-op rewrites anyway, but
// this keeps the log quiet).
//   host   -> instance=a, peer=127.0.0.1:7773
//   joiner -> instance=b, peer=127.0.0.1:7774
// The bridge's pid (line 2 of tpf2_instance.txt, 'pid=<n>'); 0 if unknown.
static unsigned long readBridgePid()
{
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2_instance.txt", g_dataDirW);
    char buf[512];
    if (!ReadFileText(p, buf, sizeof(buf))) return 0;
    const char* q = buf;
    while (q && *q) {
        unsigned long v = 0;
        if (sscanf_s(q, "pid=%lu", &v) == 1 && v) return v;
        q = strchr(q, '\n'); if (q) q++;
    }
    return 0;
}

// ---- HOT JOIN (sync point), 2026-09-09 ------------------------------------
// A player who arrives mid-session needs the world at a known sim step, and the
// only carrier is a save. "/sync" in chat (anyone) -> sync=<n> in the bridge
// ctl -> the HOST's game script pauses the session (a pause is a sync point:
// everyone stops at the leader's clock), drains its queue, confirms every peer
// sits at the same step, then asks for a save by writing tpf2_sync_save.txt.
// This DLL cannot build the engine's SaveGame command (GameMetadata,
// screenshot, config...), but the game's own autosave can: CGameUI's update
// (0x5741d0) accumulates microseconds at this+0x648 and calls
// CGameUI::AutoSave (0x563500) once they exceed autosaveIntervalMinutes.
// Setting the accumulator to INT64_MAX fires a full native autosave on the
// next frame. When the new autosave_* file lands, the host lobby pushes it to
// every joiner exactly as START GAME does; a joiner already in the game
// ignores the start (its lobby latches 'started', and the DLL guards below),
// the newcomer at the title menu loads it. The host's script keeps the session
// paused until the newcomer's clock reports at the same step, then resumes.
extern "C" {
    void* g_gameUiTramp = nullptr;
    void  GameUiRelay();
    volatile uint64_t g_gameUi = 0;                 // UI::CGameUI 'this', per frame
    void GameUiSeen(uint64_t rcx) {
        if (!g_gameUi && rcx) InterlockedExchange(&g_hostMenuLoad, 0);   // a NEW world is up: a switch in flight is over
        g_gameUi = rcx;
    }
}
// tpf2_slice.dll reads the CGameUI through this (its game-state provider at
// +0x450 is how the slice builds a MovePathUtilContext for the line platform
// assignment at replay). 0 between worlds.
extern "C" __declspec(dllexport) uint64_t Tpf2mpGameUi() { return g_gameUi; }
static const uintptr_t RVA_GAMEUI_UPDATE = 0x5741d0;
static const int       STEAL_GAMEUI      = 21;
static const uint8_t   GAMEUI_EXPECTED[STEAL_GAMEUI] = {
    0x48, 0x8B, 0xC4,                    // mov  rax, rsp
    0x55, 0x56, 0x57,                    // push rbp ; push rsi ; push rdi
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,   // push r12..r15
    0x48, 0x8D, 0xA8, 0x58, 0xFA, 0xFF, 0xFF,         // lea rbp, [rax-5A8h]
};
static const uintptr_t OFF_AUTOSAVE_ACC = 0x648;    // int64 microseconds since the last autosave
static bool ForceAutosave()
{
    uint64_t ui = g_gameUi;
    if (!ui) { Log("[sync] no CGameUI captured yet -- cannot force an autosave\n"); return false; }
    InterlockedExchange64((volatile LONG64*)(ui + OFF_AUTOSAVE_ACC), 0x4000000000000000LL);   // not INT64_MAX: the frame adds dt first and would overflow negative
    Log("[sync] autosave forced (CGameUI %llx +%llx <- 2^62 us)\n", (unsigned long long)ui, (unsigned long long)OFF_AUTOSAVE_ACC);
    return true;
}
static bool WorldLoaded() { return g_gameUi != 0; }
static bool LobbyRunning()
{
    // The worker exists before it publishes the child process handle. Treat
    // startup as an existing lobby too, including rapid reopen/Host clicks.
    if (g_lobbyThread && WaitForSingleObject(g_lobbyThread, 0) == WAIT_TIMEOUT) return true;
    if (g_lobbyCsInit) EnterCriticalSection(&g_lobbyCs);
    bool running = g_lobbyProc && WaitForSingleObject(g_lobbyProc, 0) == WAIT_TIMEOUT;
    if (g_lobbyCsInit) LeaveCriticalSection(&g_lobbyCs);
    return running;
}
// The Lua GUI can request host controls before a lobby process exists.
static void PollLobbyOpen()
{
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 250 || !g_dataDirW[0]) return;
    last = now;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%stpf2_lobby_open.txt", g_dataDirW);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    if (!DeleteFileW(path) || !WorldLoaded()) return;
    InterlockedExchange(&g_ingameOverlay, 1);
    bool running = LobbyRunning();
    InterlockedExchange(&g_uiState, running ? 2 : 1);
    if (running) SetStatus("Game running. New players can join this lobby.");
    InterlockedExchange(&g_lobbyDone, 0);
    InterlockedExchange(&g_panelDirty, 1);
}
static ULONGLONG saveMtime(const wchar_t* path, ULONGLONG* size)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) { if (size) *size = 0; return 0; }
    if (size) *size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}
static ULONGLONG g_syncBaseline = 0, g_syncAskedAt = 0, g_syncLastSize = 0;
// A RECENT SAVE SERVES THE NEXT HOT JOINER (2026-09-16). Every roster increase
// used to take a fresh autosave; three people joining a minute apart meant
// three saves (each a multi-second freeze for everyone). If the save last
// shared is younger than HOTJOIN_REUSE_MS of UNPAUSED play, the lobby's
// serve-again pushes that one to the newcomer instead. Paused time does not
// count: g_unpausedMs advances only while the mod's dash says paused=no.
static const ULONGLONG HOTJOIN_REUSE_MS = 15000;
static ULONGLONG g_unpausedMs = 0, g_unpausedLast = 0, g_syncSharedUnpaused = 0;
static bool g_syncSharedValid = false;
static void UnpausedTick()
{
    static ULONGLONG nextRead = 0; static bool paused = false;
    ULONGLONG now = GetTickCount64();
    if (now >= nextRead) {
        nextRead = now + 500;
        // the host's dash file (letter a on a plain host; the roster's letter otherwise)
        char letter[3] = "a";
        if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); if (!g_you.empty()) strcpy_s(letter, originLetterFor(g_you).c_str()); LeaveCriticalSection(&g_modelCs); }
        wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%slockstep_dash_%hs.txt", g_dataDirW, letter);
        paused = false;   // unknown counts as running: the window then closes, never lingers
        FILE* f = _wfsopen(p, L"r", _SH_DENYNO);
        if (f) { char line[200]; while (fgets(line, sizeof(line), f)) if (!strncmp(line, "paused=yes", 10)) { paused = true; break; } fclose(f); }
    }
    if (g_unpausedLast && !paused) g_unpausedMs += now - g_unpausedLast;
    g_unpausedLast = now;
}
static void MarkSaveShared() { g_syncSharedUnpaused = g_unpausedMs; g_syncSharedValid = true; }
static wchar_t   g_syncSave[600] = L"";
static CRITICAL_SECTION g_syncCs; static bool g_syncCsInit = false;
// Take the sync save now (host, in game). Called for a "/sync" request and,
// the real hot join, whenever a player appears on the roster mid-game.
static void SyncStart(const char* why)
{
    if (!InterlockedCompareExchange(&g_isHost, 0, 0)) { Log("[sync] %s on a joiner -- ignored (the host saves)\n", why); return; }
    if (!g_gameUi) { Log("[sync] %s before the game is running -- ignored\n", why); return; }
    if (g_syncCsInit) EnterCriticalSection(&g_syncCs);
    if (g_syncAskedAt) { Log("[sync] %s while a save is pending -- one save serves everyone who joined\n", why); }
    else if (strncmp(why, "hot join", 8) == 0 && g_syncSharedValid && g_startSaveW[0]
             && GetFileAttributesW(g_startSaveW) != INVALID_FILE_ATTRIBUTES
             && g_unpausedMs - g_syncSharedUnpaused < HOTJOIN_REUSE_MS) {
        // Re-send the start with the file we already shared: the lobby transfers
        // it to whoever is unstarted. Its serve-again would do that on its own
        // only for a session it has latched as started -- a host that loaded a
        // world ALONE never latched (nobody to share with), so the first joiner
        // got nothing at all (2026-09-16: "fresh enough ... no new save", then
        // silence). The explicit start covers both cases.
        Log("[sync] %s -> the save shared %.1f s of unpaused play ago is fresh enough: sharing it again, no new save\n",
            why, (g_unpausedMs - g_syncSharedUnpaused) / 1000.0);
        std::string line = "{\"cmd\":\"start\",\"save\":\"" + jsonEscape(utf8Of(g_startSaveW).c_str()) + "\"}";
        LobbySend(line.c_str());
        SetStatus("Hot join: sending the recent save\xE2\x80\xA6");
        SendChat("!hotjoin A game is running. Hold on: the host is sending you the world; your game loads it by itself.");
    }
    else {
        wchar_t cur[600] = L""; ULONGLONG sz = 0;
        g_syncBaseline = newestSave(cur, 600) ? saveMtime(cur, &sz) : 0;
        g_syncLastSize = 0; g_syncSave[0] = 0;
        Log("[sync] %s -> taking the save\n", why);
        if (ForceAutosave()) {
            g_syncAskedAt = GetTickCount64(); SetStatus("Hot join: saving\xE2\x80\xA6");
            // Our lobby would otherwise push the save START GAME shared to the
            // newcomer within a second, and this fresh one arrived to "start
            // ignored -- a save transfer is in progress" (2026-09-16). Hold it.
            LobbySend("{\"cmd\":\"sync_taking\"}");
            // tell the newcomer's panel what is going on (a marked chat line;
            // a panel still at the title menu shows it as its status, not as chat)
            // ...but only for a real hot join. The relay's periodic upload
            // took the same path and every panel got "!hotjoin ..." in its
            // chat every two minutes (2026-09-10).
            if (strncmp(why, "world switch", 12) == 0)
                SendChat("!hotjoin The host has moved to another world. Hold on: it is being sent to you and your game loads it by itself.");
            else if (strncmp(why, "relay:", 6) != 0)
                SendChat("!hotjoin A game is running. Hold on: the host is saving and will send you the world; your game loads it by itself.");
        }
    }
    if (g_syncCsInit) LeaveCriticalSection(&g_syncCs);
}
static void SyncPoll()
{
    UnpausedTick();
    wchar_t req[MAX_PATH]; _snwprintf_s(req, _TRUNCATE, L"%stpf2_sync_save.txt", g_dataDirW);
    if (GetFileAttributesW(req) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(req);
        SyncStart("sync request (chat or button)");
    }
    if (!g_syncAskedAt) return;
    wchar_t cur[600] = L""; ULONGLONG sz = 0;
    if (newestSave(cur, 600)) {
        ULONGLONG mt = saveMtime(cur, &sz);
        if (mt > g_syncBaseline && sz > 0) {
            // wait until the file stops growing (the sidecars are written after the .sav)
            if (wcscmp(cur, g_syncSave) == 0 && sz == g_syncLastSize) {
                std::string u = utf8Of(cur);
                const bool sw = InterlockedExchange(&g_switchShare, 0) != 0;
                std::string line = "{\"cmd\":\"start\",\"save\":\"" + jsonEscape(u.c_str()) + "\""
                                 + (sw ? ",\"switch\":true" : "") + "}";
                wcscpy_s(g_startSaveW, cur);
                LobbySend(line.c_str());
                MarkSaveShared();
                Log("[sync] new save %ls (%llu B) -> sharing with every joiner\n", cur, (unsigned long long)sz);
                SetStatus("Sync: sharing the save\xE2\x80\xA6");
                wchar_t sent[MAX_PATH]; _snwprintf_s(sent, _TRUNCATE, L"%stpf2_sync_sent.txt", g_dataDirW);
                HANDLE h = CreateFileW(sent, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr); CloseHandle(h); }
                g_syncAskedAt = 0;
                return;
            }
            wcscpy_s(g_syncSave, cur); g_syncLastSize = sz;
        }
    }
    if (GetTickCount64() - g_syncAskedAt > 90000) {
        Log("[sync] no new save appeared within 90 s -- giving up (is autosave writable? see the game log)\n");
        SetStatus("Sync: the save did not appear");
        g_syncAskedAt = 0;
        InterlockedExchange(&g_switchShare, 0);
    }
}

// A resync loads a world too. Its load is not a world switch: everyone is
// already being moved to it by the recovery protocol, and pushing the save
// again would start a second transfer on top of the one that just finished.
static bool RecoveryBusy()
{
    if (InterlockedCompareExchange(&g_recoveryWorldIo, 0, 0) || NativeIo::Busy()) return true;
    bool busy = false;
    if (g_modelCsInit) {
        EnterCriticalSection(&g_modelCs);
        busy = g_recoveryRequestedAt != 0
            || (g_recoveryPhase[0] && strcmp(g_recoveryPhase, "complete") && strcmp(g_recoveryPhase, "detected")
                && strcmp(g_recoveryPhase, "unavailable") && strcmp(g_recoveryPhase, "manual"));
        LeaveCriticalSection(&g_modelCs);
    }
    return busy;
}

// ---------------- WORLD SWITCH without a save name ----------------
// NEW GAME never passes through StartSavegame, and neither does anything else
// that builds a world from something other than a save file, so the detour
// above cannot see those. The MOD can: lockstep.lua stamps a fresh value into
// tpf2mp_world_gen.txt on the first sim tick of every world it loads (with the
// game's own pid, so a second instance sharing this data dir is not mistaken
// for us). A change in that value means THIS game is in another world now.
// Host only -- a joiner's own token says nothing about what anyone must load.
static void PollWorldGen()
{
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 500 || !g_dataDirW[0]) return;
    last = now;
    // A resync loads a world of its own. Arm the hold for as long as one runs,
    // not only when the change is noticed: the token is stamped on the new
    // world's first tick, and this poll may not see it until the recovery has
    // already reported "complete".
    if (RecoveryBusy()) InterlockedExchange(&g_worldGenHold, 1);
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2mp_world_gen.txt", g_dataDirW);
    FILE* f = _wfsopen(p, L"r", _SH_DENYNO); if (!f) return;
    char buf[400] = ""; size_t got = fread(buf, 1, sizeof(buf) - 1, f); buf[got] = 0; fclose(f);
    char gen[160] = ""; unsigned long pid = 0; bool sawPid = false;
    for (char* line = buf; line && *line; ) {
        char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!strncmp(line, "gen=", 4)) strncpy_s(gen, sizeof(gen), line + 4, _TRUNCATE);
        else if (!strncmp(line, "pid=", 4)) { sawPid = line[4] != 0; pid = strtoul(line + 4, nullptr, 10); }
        line = nl ? nl + 1 : nullptr;
        while (line && (*line == '\r' || *line == '\n')) line++;
    }
    if (!gen[0]) return;
    if (sawPid && pid != GetCurrentProcessId()) return;    // another instance's token
    if (!strcmp(gen, g_worldGen)) return;
    // The value is followed from the title menu on -- a world the game loaded
    // before anyone hosted is still a baseline, and a token first seen at a
    // session boundary would otherwise read as a switch. The hold says "the
    // next value is the world WE just started loading", and is spent on the
    // first sighting too: with no token file at all (a process that has never
    // loaded a world) that first sighting IS our load.
    const bool first = g_worldGen[0] == 0;
    const bool mine = InterlockedExchange(&g_worldGenHold, 0) != 0;
    strcpy_s(g_worldGen, gen);
    if (first) { Log("[menu] world token %s (first seen%s)\n", gen, mine ? ", our own load" : ""); return; }
    if (mine) { Log("[menu] world token %s -- the load we started\n", gen); return; }
    if (!InterlockedCompareExchange(&g_isHost, 0, 0)) return;   // a joiner never switches anyone
    if (RecoveryBusy()) { Log("[menu] world token %s during a resync -- not a switch\n", gen); return; }
    int players = 0;
    if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); players = playerCount(); LeaveCriticalSection(&g_modelCs); }
    const bool live = WorldLoaded() && InterlockedCompareExchange(&g_sessionStarted, 0, 0)
                   && InterlockedCompareExchange(&g_lobbyReady, 0, 0);
    if (!live || players < 2) {
        Log("[menu] world token %s -- nobody to switch (live=%d players=%d)\n", gen, live ? 1 : 0, players);
        return;
    }
    Log("[menu] world token changed to %s while %d player(s) are playing -- switching everyone\n", gen, players - 1);
    InterlockedExchange(&g_switchShare, 1);
    SyncStart("world switch");
}

// "/speed 2.5" typed in the lobby chat (by anyone -- the host's game script
// applies it and broadcasts the session speed). Carried to the game script as
// a speed= line in the bridge ctl file; "/speed off" (or 0) clears it.
// "/sync" the same way as sync=<n> (a counter, so a repeat is a new request);
// "/sync off" clears it.
static char g_speedReq[16] = "";
static int  g_syncReq = 0;
static char g_xfer[48] = "";        // save transfer progress for the in-game window ("uploading 60%", "sending 30%", "")
static char g_transportLobby[33] = ""; // owned by LobbyThread
static void writeBridgeCtl(bool isHost);
// Our own hot-join stage for the roster (2026-09-16). Sent as {"cmd":"stage"}
// when it changes: "loading world" as the shared save starts loading, "loading
// world N%" from the engine's own loading bar while it loads, then what the
// mod's status line says (stage=starting / catchup / behind / live) once the
// world is up, and "" (cleared) at live.
static char g_stageSent[80] = "";
static volatile LONG g_stageWatch = 0;
// The engine's loading bar (RE 2026-09-16, build 35924 -- static: decompiled
// and byte-scanned, the live values are what a run has to confirm):
//   UI::CMenuUI + 0x498   UI::CProgressBar* m_progressBar, stored by the CMenuUI
//                         ctor 0x64e220, asserted non-null by the load's
//                         completion lambda 0x67ca00 (MenuUI.cpp:0xe65)
//   CProgressBar + 0x440  shared_ptr<UI::ProgressMonitor> (ptr; control block at
//                         +0x448), made in the bar's creator 0x22e3140
//                         (lib\ui\popupmanager.cpp), handed out by 0x22e3c50
//   ProgressMonitor+0x08  float 0..1, written atomically by SetProgress (vslot 1,
//                         0x22e3dc0) and read by GetProgress (vslot 2, 0x156720);
//                         vftable 0x389b4a8 is the sanity check below
// The load launcher 0x65a0f0 (the task StartSavegame 0x6785c0 posts) resets
// the bar to menu+0x19b4 and titles it "Loading..."; the loader job 0x67d130
// owns the span up to 0.7 (LoadGame 0x2e5ec0 splits it 0.1 header+mods / 0.9
// CGame::Load through SubProgressMonitor 0x2380000, whose dtor 0x2380050 lands
// the parent on base+span) and the completion lambda 0x67ca00 hands StartGame
// 0x676480 the rest, so 1.0 means the new CGameUI exists. Between loads the
// value keeps the last one (1.0 after a load, 0 from the ctor): a 100 is never
// shown as a number, it is the previous load until the launcher resets it.
static const uintptr_t RVA_PROGRESSMON_VFT  = 0x389b4a8;   // UI::ProgressMonitor::vftable
static const size_t    MENU_OFF_PROGRESSBAR = 0x498;
static const size_t    BAR_OFF_MONITOR      = 0x440;
static const size_t    PM_OFF_PROGRESS      = 0x8;
static volatile uint64_t g_menuUiPtr = 0;          // UI::CMenuUI 'this' (from CreatePage)
static volatile LONG g_stageArmedInWorld = 0;      // armed while a world was up: an in-place switch
static volatile LONG g_stageSawLoad = 0;           // the bar moved, or the old world went, since arming
static volatile LONG g_stageNoPctLogged = 0;
static ULONGLONG     g_stageArmedAt = 0;
// SEH only in this frame (no C++ objects): the pointers are the engine's.
// -1 no menu yet, -2 no bar / monitor, -3 not a ProgressMonitor or out of range,
// -4 faulted; else 0..100 (floored: 100 only at exactly 1.0).
static int ReadLoadPercent()
{
    uint64_t menu = g_menuUiPtr;
    if (!menu) return -1;
    __try {
        uint64_t bar = *(volatile uint64_t*)(menu + MENU_OFF_PROGRESSBAR);
        if (!bar) return -2;
        uint64_t pm = *(volatile uint64_t*)(bar + BAR_OFF_MONITOR);
        if (!pm) return -2;
        if (*(volatile uint64_t*)pm != (uint64_t)(g_base + RVA_PROGRESSMON_VFT)) return -3;
        float f = *(volatile float*)(pm + PM_OFF_PROGRESS);
        if (!(f >= 0.0f && f <= 1.0f)) return -3;
        int pct = (int)(f * 100.0f);
        return pct < 0 ? 0 : pct > 100 ? 100 : pct;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -4; }
}
static void ReportStage(const char* text)
{
    // The footer must advance too, not just the roster sent through the lobby.
    // Refresh before deduplication: a late transfer event can replace the footer.
    SetStatus(text[0] ? text : "World loaded. Session running.");
    if (strcmp(text, g_stageSent) == 0) return;
    strcpy_s(g_stageSent, text);
    char esc[200]; int j = 0;
    for (int i = 0; text[i] && j < 190; i++) { if (text[i] == '"' || text[i] == '\\') esc[j++] = '\\'; esc[j++] = text[i]; }
    esc[j] = 0;
    char line[260]; snprintf(line, sizeof(line), "{\"cmd\":\"stage\",\"text\":\"%s\"}", esc);
    LobbySend(line);
    Log("[menu] stage: %s\n", text[0] ? text : "(clear)");
}
static void ArmStageWatch(const char* text)
{
    ReportStage(text);
    g_stageArmedAt = GetTickCount64();
    InterlockedExchange(&g_stageArmedInWorld, (NativeIo::HasWorld() || WorldLoaded()) ? 1 : 0);
    InterlockedExchange(&g_stageSawLoad, 0);
    InterlockedExchange(&g_stageNoPctLogged, 0);
    InterlockedExchange(&g_stageWatch, 1);
}
static void StageTick()
{
    static ULONGLONG next = 0;
    if (InterlockedExchange(&g_loadingStagePending, 0)) {
        ArmStageWatch("loading world");
        next = 0;
    }
    if (!InterlockedCompareExchange(&g_stageWatch, 0, 0)) return;
    ULONGLONG now = GetTickCount64(); if (now < next) return; next = now + 1000;
    // 1. the engine's loading bar: "loading world N%" while it is short of 1.0
    const int pct = ReadLoadPercent();
    if (pct >= 0 && pct < 100) {
        InterlockedExchange(&g_stageSawLoad, 1);
        char t[48]; snprintf(t, sizeof(t), "loading world %d%%", pct); ReportStage(t); return;
    }
    if (pct < 0 && !InterlockedExchange(&g_stageNoPctLogged, 1))
        Log("[menu] stage: the engine's load progress is not readable (menu=%llx code=%d) -- no percentage\n",
            (unsigned long long)g_menuUiPtr, pct);
    // 2. is the world up? NativeIo sees the CGameUI constructor and the world
    // destructor (g_gameUi stays stale through an in-place switch); the per-frame
    // CGameUI relay covers a title-menu load should the native hooks be off.
    const bool inWorldArm = InterlockedCompareExchange(&g_stageArmedInWorld, 0, 0) != 0;
    const bool worldUp = NativeIo::HasWorld() || (!inWorldArm && WorldLoaded());
    if (!worldUp) {
        InterlockedExchange(&g_stageSawLoad, 1);
        ReportStage("loading world"); // no invented percentage if the bar is unavailable
        return;
    }   // pct 100 here is the LAST load's, not proof that the new world is ready
    // 3. an in-place switch: the old world stays up until StartGame swaps it, so a
    // fresh arm must not read the old world's status line as the new world's
    if (inWorldArm && !InterlockedCompareExchange(&g_stageSawLoad, 0, 0) && now - g_stageArmedAt < 20000) return;
    // 4. the world is up: what the mod's status line says
    char letter[3] = "a";
    if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); if (!g_you.empty()) strcpy_s(letter, originLetterFor(g_you).c_str()); LeaveCriticalSection(&g_modelCs); }
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%slockstep_status_%hs.txt", g_dataDirW, letter);
    FILE* f = _wfsopen(p, L"r", _SH_DENYNO); if (!f) { ReportStage("world loaded"); return; }
    char line[512] = ""; fgets(line, sizeof(line), f); fclose(f);
    const char* st = strstr(line, "stage=");
    if (!st) { ReportStage("world loaded"); return; }
    st += 6; char stage[64]; int k = 0; while (st[k] && st[k] > ' ' && k < 63) { stage[k] = st[k]; k++; } stage[k] = 0;
    if (strcmp(stage, "live") == 0) { ReportStage(""); InterlockedExchange(&g_stageWatch, 0); return; }
    if (strcmp(stage, "starting") == 0) { ReportStage("world loaded, waiting for the session"); return; }
    if (strncmp(stage, "catchup:", 8) == 0) {
        const char* ph = stage + 8; const char* b = strchr(ph, ':'); double behind = b ? atof(b + 1) : 0;
        char t[80];
        if (strncmp(ph, "fetch", 5) == 0) snprintf(t, sizeof(t), "catching up: fetching history (%.0f s behind)", behind);
        else snprintf(t, sizeof(t), "catching up (%.0f s behind)", behind);
        ReportStage(t); return;
    }
    if (strncmp(stage, "behind:", 7) == 0) { char t[80]; snprintf(t, sizeof(t), "%.0f s behind", atof(stage + 7)); ReportStage(t); return; }
}
static void speedFromChat(const char* text)
{
    if (strncmp(text, "/sync", 5) == 0) {
        const char* a = text + 5; while (*a == ' ') a++;
        if (strncmp(a, "off", 3) == 0) g_syncReq = 0; else g_syncReq++;
        Log("[menu] chat /sync -> %d\n", g_syncReq);
        writeBridgeCtl(g_isHost != 0);
        return;
    }
    if (strncmp(text, "/speed", 6) != 0) return;
    const char* a = text + 6;
    while (*a == ' ') a++;
    double v = atof(a);
    if (v > 0.0 && v < 64.0) snprintf(g_speedReq, sizeof(g_speedReq), "%.4g", v);
    else g_speedReq[0] = 0;
    Log("[menu] chat /speed -> %s\n", g_speedReq[0] ? g_speedReq : "off");
    writeBridgeCtl(g_isHost != 0);
}

static void writeBridgeCtl(bool isHost)
{
    static char last[400] = "";
    char content[400];
    // pid= addresses the ctl to OUR bridge. Two instances sharing a data dir
    // (a sandboxed second instance reads through to the real dir until it has
    // its own copy) otherwise apply each other's role for a moment.
    unsigned long bpid = readBridgePid();
    // Letters for N players: the host is 'a'; joiners take b, c, d... in roster
    // order, skipping the host. Every client derives the same assignment from
    // the same roster, so nobody has to be told.
    std::string letter = "a";
    bool fromRelay = false;
    if (InterlockedCompareExchange(&g_lobbyRelay, 0, 0)) {
        if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
        for (int i = 0; i < playerCount(); i++) if (g_players[i] == g_you && !g_letters[i].empty()) { letter = g_letters[i]; fromRelay = true; break; }
        if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
    }
    if (!isHost && !fromRelay) {
        int idx = 0;
        if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
        for (int i = 0; i < playerCount(); i++) {
            if (g_players[i] == g_host) continue;
            if (g_players[i] == g_you) break;
            idx++;
        }
        if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
        char nm[3]; originName(idx + 1, nm); letter = nm;
    }
    // players= is the LOBBY ROSTER SIZE, including the host. The game script
    // needs it to know when everybody has finished loading: nothing else tells
    // it how many instances to expect, and guessing from who has appeared so
    // far cannot distinguish "the last player is still loading" from "that is
    // everyone". Without it the load gate had to fall back to a settle timer
    // and released with two of three players in.
    snprintf(content, sizeof(content), "instance=%s\npeer=127.0.0.1:%d\npid=%lu\nplayers=%d\n",
             letter.c_str(), relayPortFor(isHost), bpid, playerCount());
    if (g_transportLobby[0]) {
        size_t n = strlen(content);
        snprintf(content+n,sizeof(content)-n,"lobby=%s\n",g_transportLobby);
    }
    if (g_speedReq[0]) {
        size_t n = strlen(content);
        snprintf(content + n, sizeof(content) - n, "speed=%s\n", g_speedReq);
    }
    if (g_syncReq) {
        size_t n = strlen(content);
        snprintf(content + n, sizeof(content) - n, "sync=%d\n", g_syncReq);
    }
    if (g_xfer[0]) {
        size_t n = strlen(content);
        snprintf(content + n, sizeof(content) - n, "xfer=%s\n", g_xfer);
    }
    {   // the session clock: the roster's host (a relay lobby moves it when the leader leaves)
        size_t n = strlen(content);
        snprintf(content + n, sizeof(content) - n, "leader=%s\n", !g_host.empty() ? originLetterFor(g_host).c_str() : "a");
    }
    if (strcmp(content, last) == 0) return;
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%stpf2_bridge_ctl.txt", g_dataDirW);
    _snwprintf_s(tmp,  _TRUNCATE, L"%stpf2_bridge_ctl.txt.tmp", g_dataDirW);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("[menu] bridge ctl: cannot create %ls (err %lu)\n", tmp, GetLastError()); return; }
    DWORD w = 0; BOOL ok = WriteFile(h, content, (DWORD)strlen(content), &w, nullptr);
    CloseHandle(h);
    if (!ok || w != strlen(content)) { Log("[menu] bridge ctl: write failed (err %lu)\n", GetLastError()); DeleteFileW(tmp); return; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log("[menu] bridge ctl: replace failed (err %lu)\n", GetLastError()); DeleteFileW(tmp); return;
    }
    strcpy_s(last, content);
    Log("[menu] bridge ctl -> %ls: instance=%s peer=127.0.0.1:%d\n",
        path, letter.c_str(), relayPortFor(isHost));
}

// The origin letter each machine's bridge uses: the host is 'a', joiners take
// b, c, ... in roster order skipping the host (same rule as writeBridgeCtl).
static std::string originLetterFor(const std::string& name)
{
    // the critical section is recursive: writeCompanyCfg calls this with it held
    if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
    std::string out;
    if (InterlockedCompareExchange(&g_lobbyRelay, 0, 0)) {
        for (int i = 0; i < playerCount(); i++) if (g_players[i] == name && !g_letters[i].empty()) { out = g_letters[i]; break; }
    }
    if (out.empty()) {
        if (name == g_host) out = "a";
        else {
            int idx = 0;
            for (int i = 0; i < playerCount(); i++) {
                if (g_players[i] == g_host) continue;
                if (g_players[i] == name) break;
                idx++;
            }
            char nm[3]; originName(idx + 1, nm); out = nm;
        }
    }
    if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
    return out;
}

// mp_company_cfg.txt for the game script (lockstep.lua companies mode):
//   line 1  coop | companies      (companies when more than one distinct id)
//   line 2  my company id
//   line 3  all distinct company ids, comma-separated
//   line 4  origin=company map, e.g. a=1,b=2,c=1  -- authoritative on every peer
// Written at START from the roster every machine already agrees on.
static void writeCompanyCfg()
{
    std::string l3, l4; int mine = 1, distinct = 0; bool seen[MAX_COMPANIES + 1] = {};
    if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
    for (int i = 0; i < playerCount(); i++) {
        int cid = g_companies[i] < 1 ? 1 : (g_companies[i] > MAX_COMPANIES ? MAX_COMPANIES : g_companies[i]);
        if (g_players[i] == g_you) mine = cid;
        if (!seen[cid]) { seen[cid] = true; distinct++; }
        if (!l4.empty()) l4 += ','; l4 += originLetterFor(g_players[i]) + "=" + std::to_string(cid);
    }
    if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
    for (int c = 1; c <= MAX_COMPANIES; c++) if (seen[c]) { if (!l3.empty()) l3 += ','; l3 += std::to_string(c); }
    std::string content = std::string(distinct > 1 ? "companies" : "coop") + "\n" + std::to_string(mine) + "\n" + l3 + "\n" + l4 + "\n";
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%smp_company_cfg.txt", g_dataDirW);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("[menu] company cfg: cannot write %ls\n", path); return; }
    DWORD w = 0; WriteFile(h, content.data(), (DWORD)content.size(), &w, nullptr); CloseHandle(h);
    Log("[menu] company cfg -> %ls: mode=%s me=%d ids=%s map=%s\n", path, distinct > 1 ? "companies" : "coop", mine, l3.c_str(), l4.c_str());
}

// mp_players.txt: "letter=name" per roster entry, with the same letters the
// bridge ctl and the company cfg use. The in-game dashboard's company picker
// shows player names instead of origin letters from it (2026-09-16). Written on
// every roster change so a hot joiner appears by name too.
static void writePlayerNames()
{
    std::string content;
    if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
    for (int i = 0; i < playerCount(); i++) {
        content += originLetterFor(g_players[i]); content += '='; content += g_players[i]; content += '\n';
    }
    if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%smp_players.txt", g_dataDirW);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(h, content.c_str(), (DWORD)content.size(), &w, nullptr); CloseHandle(h);
    // mp_loading.txt (2026-09-16): "letter=name=stage" for every player still loading in
    // (receiving the save, loading the world, catching up); empty once everyone is in.
    // The mod refuses company switches while it is not empty (companies.lua).
    std::string loading;
    if (g_modelCsInit) EnterCriticalSection(&g_modelCs);
    for (int i = 0; i < playerCount() && i < (int)g_stages.size(); i++) {
        if (g_stages[i].empty()) continue;
        loading += originLetterFor(g_players[i]); loading += '='; loading += g_players[i]; loading += '='; loading += g_stages[i]; loading += '\n';
    }
    if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
    _snwprintf_s(path, _TRUNCATE, L"%smp_loading.txt", g_dataDirW);
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, loading.c_str(), (DWORD)loading.size(), &w, nullptr); CloseHandle(h);
}

// parse a roster event: "players":["a","b"], "you":"a", "host":"a"
static void applyRoster(const char* s)
{
    if (!g_modelCsInit) return; EnterCriticalSection(&g_modelCs);
    g_players.clear(); g_companies.clear(); g_letters.clear();
    // whitespace-tolerant: find "players", then its '[' and pull each "quoted"
    // name until the list ends. Every name, in full, however many: the roster
    // is the load gate's player count and the origin-letter order on every peer.
    const char* pa = strstr(s, "\"players\"");
    if (pa) { pa = strchr(pa, '[');
        if (pa) { const char* q = pa + 1;
            for (;;) {
                while (*q == ' ' || *q == '\t' || *q == ',') q++;
                if (*q != '"') break;   // ']' (or anything that is not a name): end of the list
                q++;
                g_players.push_back(jsonUnquote(q));
                if (*q == '"') q++;
            } } }
    g_companies.assign(g_players.size(), 1); g_letters.assign(g_players.size(), std::string());
    // "companies":{"name":id,...} -> g_companies[i] for each roster entry (default 1)
    const char* co = strstr(s, "\"companies\"");
    for (int i = 0; co && i < playerCount(); i++) {
        std::string keyq = "\"" + jsonEscape(g_players[i].c_str()) + "\"";
        const char* k = strstr(co, keyq.c_str());
        if (k) { k += keyq.size(); while (*k == ' ' || *k == ':') k++; int id = atoi(k); if (id >= 1 && id <= MAX_COMPANIES) g_companies[i] = id; }
    }
    // "mode":"coop"|"companies" -> the checkbox (a joiner sees the host's choice)
    { char md[16] = ""; jsonStr(s, "mode", md, sizeof(md));
      if (md[0]) InterlockedExchange(&g_sepCompanies, strcmp(md, "companies") == 0 ? 1 : 0); }
    // "join_freeze":true -> the lobby freezes the session for a late joiner and
    // reloads everyone (a recovery round); absent or false -> the hot-join save below
    { const char* jf = strstr(s, "\"join_freeze\"");
      InterlockedExchange(&g_joinFreeze, (jf && strstr(jf, "true") && strstr(jf, "true") < jf + 24) ? 1 : 0); }
    // "stages":{"name":"text",...} -> g_stages[i]: what each joiner is doing
    g_stages.assign(g_players.size(), std::string());
    { const char* sg = strstr(s, "\"stages\"");
      for (int i = 0; sg && i < playerCount(); i++) {
          std::string keyq = "\"" + jsonEscape(g_players[i].c_str()) + "\"";
          const char* k = strstr(sg, keyq.c_str());
          if (k) { k += keyq.size(); while (*k == ' ' || *k == ':') k++; if (*k == '"') { k++; g_stages[i] = jsonUnquote(k); } }
      } }
    std::string v;
    v = jsonStrS(s, "you");  if (!v.empty()) g_you = v;
    v = jsonStrS(s, "host"); if (!v.empty()) g_host = v;
    g_lobbyTitle = jsonStrS(s, "lobby");
    InterlockedExchange(&g_lobbyRelay, jsonBool(s, "relay", false) ? 1 : 0);
    InterlockedExchange(&g_storedAge, jsonInt(s, "stored_age")); InterlockedExchange(&g_storedMax, jsonInt(s, "stored_max"));
    // relay lobbies: the relay assigns every player a sticky origin letter
    { const char* lm = strstr(s, "\"letters\"");
      if (lm && InterlockedCompareExchange(&g_lobbyRelay, 0, 0)) {
          for (int i = 0; i < playerCount(); i++) {
              std::string keyq = "\"" + jsonEscape(g_players[i].c_str()) + "\"";
              const char* k = strstr(lm, keyq.c_str());
              if (k) { k += keyq.size(); while (*k == ' ' || *k == ':') k++; if (*k == '"') { k++; g_letters[i] = jsonUnquote(k); } }
          }
      } }
    // Role is decided by the roster: you==host -> instance a, else b. Re-evaluated
    // on every roster (a host change re-points the bridge); writeBridgeCtl is a
    // no-op when nothing changed.
    bool roleKnown = !g_you.empty() && !g_host.empty();
    bool isHost = roleKnown && g_you == g_host;
    int count = playerCount();
    LeaveCriticalSection(&g_modelCs); InterlockedExchange(&g_panelDirty, 1);
    // a relay lobby: the leader takes the host role here (START GAME, the sync
    // save for hot joiners); it changes when the leader leaves
    if (roleKnown && InterlockedCompareExchange(&g_lobbyRelay, 0, 0)) {
        LONG was = InterlockedExchange(&g_isHost, isHost ? 1 : 0);
        if (was != (isHost ? 1 : 0)) {
            // stored_age < 0: the relay holds no world, so the leader has to send one
            const bool relayHasWorld = InterlockedCompareExchange(&g_storedAge, 0, 0) >= 0;
            Log("[menu] relay lobby: we are %s the leader now (relay %s a saved world)\n", isHost ? "" : "not", relayHasWorld ? "holds" : "has no");
            SetStatus(relayHasWorld ? "Loading the relay's world\xE2\x80\xA6"
                      : isHost ? "This relay has no saved world yet -- press START GAME to send your most recent save."
                               : "Waiting for the leader to press START GAME.");
        }
    }
    if (roleKnown) writeBridgeCtl(isHost);
    // HOT JOIN (2026-09-09): the roster grew while the game is running and we
    // are the host -- the newcomer is at the title menu with our code. Take a
    // save and share it now; their game loads it by itself (the ordinary
    // start path) and catches up on the command history. No button, no
    // pause, no ordering to get right.
    static int lastCount = 0;
    writePlayerNames();
    bool inGame = InterlockedCompareExchange(&g_showOverlay, 0, 0) == 0 && g_gameUi != 0;
    if (isHost && inGame && count > lastCount && lastCount > 0) {
        LONG age = InterlockedCompareExchange(&g_storedAge, 0, 0), mx = InterlockedCompareExchange(&g_storedMax, 0, 0);
        if (InterlockedCompareExchange(&g_lobbyRelay, 0, 0) && age >= 0 && mx > 0 && age <= mx) {
            // the relay holds a copy fresh enough to serve the newcomer itself
            // (the periodic upload keeps it that way): no autosave, no upload here
            Log("[menu] hot join: roster %d -> %d -- the relay serves its %ld s old world, no sync taken\n", lastCount, count, age);
        } else if (InterlockedCompareExchange(&g_joinFreeze, 0, 0)) {
            // FROZEN JOIN (2026-09-16): the lobby holds the session and runs a
            // recovery round -- the host saves, EVERYONE (this game included)
            // loads that save -- so the newcomer's world registers its entities
            // in the same order as ours. A catch-up joiner never did, and its
            // person sim split within ~35 game units. No autosave from here.
            Log("[menu] hot join: roster %d -> %d -- the lobby freezes the session and reloads everyone; no hot-join save taken here\n", lastCount, count);
            SetStatus("A player joined: holding the game while everyone loads the shared world\xE2\x80\xA6");
        } else {
            char why[96]; snprintf(why, sizeof(why), "hot join: roster %d -> %d", lastCount, count);
            SyncStart(why);
        }
    }
    lastCount = count;
}

// ---------------- START GAME: place the shared save + load it in place ----------------
// The host's userdata save folder. For the sandboxed joiner (B) the game process
// is inside Sandboxie, so a CopyFileW/FindFirstFile to this path is transparently
// redirected to B's overlay -- both peers use this same constant.
static const wchar_t* SAVE_DIR =   // placeholder: resolveSaveDir() replaces it at init
    L"C:\\Program Files (x86)\\Steam\\userdata\\0\\1066780\\local\\save";

static void RefreshLobbySaves()
{
    g_lobbySaves.clear(); g_savePage=0;
    wchar_t pattern[700]; _snwprintf_s(pattern,_TRUNCATE,L"%s\\*.sav",SAVE_DIR);
    WIN32_FIND_DATAW data; HANDLE find=FindFirstFileW(pattern,&data);
    if (find==INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path=std::wstring(SAVE_DIR)+L"\\"+data.cFileName;
        if (path.size()>=600) continue; // the native load/transfer path uses 600 wchar_t
        g_lobbySaves.push_back({path,data.cFileName,data.ftLastWriteTime});
    } while (FindNextFileW(find,&data));
    FindClose(find);
    std::sort(g_lobbySaves.begin(),g_lobbySaves.end(),[](const LobbySave& a,const LobbySave& b) {
        LONG order=CompareFileTime(&a.modified,&b.modified);
        return order ? order>0 : a.name<b.name;
    });
}

// newest *.sav in SAVE_DIR (full path). Returns false if none. mp_shared.sav is
// OUR OWN placed copy (always stamped newest by placeSaveNewest), so it is skipped
// unless it is the only save there -- otherwise every START would re-share the
// previous session's copy instead of the player's real latest save.
static bool newestSave(wchar_t* out, int cch)
{
    wchar_t pat[700]; _snwprintf_s(pat, _TRUNCATE, L"%s\\*.sav", SAVE_DIR);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    ULONGLONG best = 0; wchar_t bestName[300] = L""; wchar_t sharedName[300] = L"";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"mp_shared.sav") == 0) { wcscpy_s(sharedName, fd.cFileName); continue; }
        ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t > best) { best = t; wcscpy_s(bestName, fd.cFileName); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!bestName[0] && sharedName[0]) wcscpy_s(bestName, sharedName);   // nothing else: fall back to our copy
    if (!bestName[0]) return false;
    _snwprintf_s(out, cch, _TRUNCATE, L"%s\\%s", SAVE_DIR, bestName);
    return true;
}

static void stampNow(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME ft; GetSystemTimeAsFileTime(&ft); SetFileTime(f, nullptr, nullptr, &ft); CloseHandle(f);
}

// Copy srcSav (+ its .sav.lua / .jpg sidecars) into SAVE_DIR as mp_shared.* and
// stamp them NEWEST, so the game's Continue (loads most-recent save) loads it.
// Returns false if the .sav itself could not be placed (the caller must NOT click
// Continue then -- it would load whatever unrelated save happens to be newest).
// CreateDirectoryW for every component (no shell32 dependency): the save folder of
// a game that has never saved does not exist until a shared save is placed in it.
static void ensureDir(const wchar_t* path)
{
    wchar_t buf[700]; wcscpy_s(buf, path);
    for (wchar_t* c = buf + 3; *c; c++) {          // past "C:\"
        if (*c == L'\\') { *c = 0; CreateDirectoryW(buf, nullptr); *c = L'\\'; }
    }
    CreateDirectoryW(buf, nullptr);
}

static bool placeSaveNewest(const wchar_t* srcSav)
{
    wchar_t dst[700]; _snwprintf_s(dst, _TRUNCATE, L"%s\\mp_shared.sav", SAVE_DIR);
    // sidecars: <src>.sav.lua and <src minus .sav>.jpg
    wchar_t lua[700], jpg[700], dl[700], dj[700];
    _snwprintf_s(lua, _TRUNCATE, L"%s.lua", srcSav);
    wcscpy_s(jpg, srcSav); { size_t n = wcslen(jpg); if (n > 4) wcscpy_s(jpg + n - 4, 700 - (n - 4), L".jpg"); }
    _snwprintf_s(dl, _TRUNCATE, L"%s\\mp_shared.sav.lua", SAVE_DIR);
    _snwprintf_s(dj, _TRUNCATE, L"%s\\mp_shared.jpg", SAVE_DIR);

    // src == dst (the host is sharing mp_shared.sav itself, e.g. the only save
    // present): CopyFileW onto itself fails with ERROR_SHARING_VIOLATION and
    // would wrongly abort the start. Compare the normalised paths first.
    wchar_t fs[700], fdst[700]; bool same = false;
    if (GetFullPathNameW(srcSav, 700, fs, nullptr) && GetFullPathNameW(dst, 700, fdst, nullptr)) same = _wcsicmp(fs, fdst) == 0;
    else same = _wcsicmp(srcSav, dst) == 0;
    if (!same) ensureDir(SAVE_DIR);
    if (!same && !CopyFileW(srcSav, dst, FALSE)) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION && _wcsicmp(srcSav, dst) == 0) same = true;
        else { Log("[menu] placeSave copy failed err=%lu src=%ls dst=%ls\n", e, srcSav, dst); return false; }
    }
    if (same) {   // already in place: just make sure it (and its sidecars) are newest
        stampNow(dst);
        if (GetFileAttributesW(dl) != INVALID_FILE_ATTRIBUTES) stampNow(dl);
        if (GetFileAttributesW(dj) != INVALID_FILE_ATTRIBUTES) stampNow(dj);
        Log("[menu] shared save already in place -> %ls (stamped newest)\n", dst);
        return true;
    }
    stampNow(dst);
    // Sidecars follow the source: copy when present, otherwise delete the stale
    // one left by a previous share so the game never pairs it with this save.
    if (GetFileAttributesW(lua) != INVALID_FILE_ATTRIBUTES) { if (CopyFileW(lua, dl, FALSE)) stampNow(dl); }
    else DeleteFileW(dl);
    if (GetFileAttributesW(jpg) != INVALID_FILE_ATTRIBUTES) { if (CopyFileW(jpg, dj, FALSE)) stampNow(dj); }
    else DeleteFileW(dj);
    Log("[menu] placed shared save -> %ls (newest)\n", dst);
    return true;
}

// ---------------- VANILLA LOAD = SHARE (host) ----------------
// UI::CMenuUI::StartSavegame 0x6785c0 is where every UI load path converges
// (title menu, in-game menu, CONTINUE), so one observer sees them all. It is
// NOT a second inline hook: native_io.cpp already detours that address with a
// 20-byte steal, and a second InstallHook there would copy the jump the first
// one wrote into its trampoline. NativeIo::ObserveStart calls us instead, on
// the engine's UI thread, with the LoadGameParams the game was handed.
// Re-verified against build 35924 (2026-09-16, capstone over the exe):
//   rcx = UI::CMenuUI* (it reads this+0x1988, the "initialization is already
//         active" flag), rdx = const LoadGameParams&, r8 = const SavegameInfo&
//   prologue: push rbp/rsi/rdi/r12/r13/r14/r15 (12 B) + lea rbp,[rsp-0x3a0]
//         (8 B) = a 20-byte steal on an instruction boundary, nothing
//         RIP-relative before +27 (sub rsp,0x4a0 at +20).
//   LoadGameParams holds 32-byte std::strings: the campaign/mission sizes the
//         engine compares sit at +0x108 and +0x128, i.e. the strings at +0xF8
//         and +0x118; the save NAME is the string at +0x00 (the field our own
//         AutoLoadCall fills for mp_shared).
// LoadGameParams +0x00 is the save NAME (no extension, namespace "savegame");
// the file is <SAVE_DIR>\<name>.sav.
static void GStringRead(const void* gs, char* out, size_t cap)
{
    const GString* g = (const GString*)gs;
    const char* src = g->cap >= 16 ? *(const char* const*)g->buf : g->buf;
    size_t n = g->size < cap - 1 ? g->size : cap - 1;
    if (g->size > 4096 || !src) { out[0] = 0; return; }
    memcpy(out, src, n); out[n] = 0;
}
// SEH only in this frame (no C++ objects): the params pointer is the engine's.
static bool SafeParamsName(const void* params, char* out, size_t cap)
{
    __try { GStringRead(params, out, cap); } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    return out[0] != 0;
}

static void OnStartSavegame(const void* params, bool accepted, bool ours)
{
    // our own autoload / the resync's in-place load: not the player picking a world
    if (ours || InterlockedCompareExchange(&g_selfLoad, 0, 0)) return;
    if (!accepted) return;                     // the engine refused it; nothing changed
    char name[300] = "";
    SafeParamsName(params, name, sizeof(name));
    const bool hosting = InterlockedCompareExchange(&g_isHost, 0, 0) != 0
                      && InterlockedCompareExchange(&g_lobbyReady, 0, 0) != 0;
    int players = 0;
    if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); players = playerCount(); LeaveCriticalSection(&g_modelCs); }
    Log("[menu] menu load of '%s' from page %ld (hosting=%d players=%d)\n",
        name, InterlockedCompareExchange(&g_lastPage, 0, 0), hosting ? 1 : 0, players);
    if (!hosting || !name[0]) return;
    // On a relay lobby the relay keeps the world for the next player, so the
    // leader's load is uploaded even with nobody else in (2026-09-18: a swap
    // taken alone was "not shared", the relay kept the old save, and the
    // leader's next join got the old world back).
    const bool relay = InterlockedCompareExchange(&g_lobbyRelay, 0, 0) != 0;
    if (players < 2 && !relay) { Log("[menu] menu load while hosting with nobody in the lobby -- not shared\n"); return; }
    wchar_t wn[300]; MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 300);
    wchar_t path[600]; _snwprintf_s(path, _TRUNCATE, L"%s\\%s.sav", SAVE_DIR, wn);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        Log("[menu] menu load: %ls not found in the save folder -- not shared\n", path);
        return;
    }
    // A SWITCH is a load taken while the session is already running: the others
    // are IN a world and have to leave it. Before that -- everyone still in the
    // lobby -- it is the ordinary "the host picked the save to start with".
    const bool switching = WorldLoaded() || InterlockedCompareExchange(&g_sessionStarted, 0, 0) != 0;
    wcscpy_s(g_startSaveW, path);
    writeCompanyCfg();
    // the mod stamps a fresh world token the moment this world is up: that
    // change is THIS load, not another switch
    InterlockedExchange(&g_worldGenHold, 1);
    std::string line = "{\"cmd\":\"start\",\"save\":\"" + jsonEscape(utf8Of(path).c_str()) + "\"";
    if (switching) line += ",\"switch\":true";
    line += "}";
    LobbySend(line.c_str());
    MarkSaveShared();     // a joiner arriving right after reuses this save
    // The engine builds the title page between the old world and the loading
    // screen of THIS load; MyCreatePage must not read that as "left the world"
    // (it did: the host's every mid-session load LEFT the lobby, 2026-09-18).
    if (switching) InterlockedExchange(&g_hostMenuLoad, 1);
    char st[240];
    if (switching) {
        if (relay && players < 2) snprintf(st, sizeof(st), "Uploading '%s' to the server\xE2\x80\xA6", name);
        else snprintf(st, sizeof(st), "Switching everyone to '%s'\xE2\x80\xA6", name);
        Log("[menu] world switch: the host loaded %ls mid-session -- pushing it to %d player(s)%s\n", path, players - 1, relay ? " and the relay" : "");
    } else {
        InterlockedExchange(&g_hostLoadedItself, 1);
        snprintf(st, sizeof(st), "Sharing '%s' with %d player(s)\xE2\x80\xA6", name, players - 1);
        Log("[menu] menu load: sharing %ls with the lobby (the game loads it here)\n", path);
    }
    SetStatus(st);
    ArmStageWatch("loading world");   // the host's own row shows its percentage too (2026-09-16)
}

// ---------------- AUTO-LOAD: start the shared save in-process ----------------
// Every menu load ends in bool UI::CMenuUI::StartSavegame(this, const
// LoadGameParams&, const SavegameInfo&) (0x6785c0). CONTINUE feeds it from the
// profile's lastGame, a SaveGameId { std::wstring path; std::string name;
// std::string namespace } that profile.lua writes as path = "", saveGameName =
// "...", saveGameNamespace = "savegame". We build that id for mp_shared, ask the
// save manager for its SavegameInfo (0x2e6ca0, as the Missions page does),
// default-construct LoadGameParams (0x553b70 already sets the namespace;
// CONTINUE's click lambda 0x65e780 adds only the name) and call StartSavegame
// from CMenuUI's own per-frame update (vtable slot 33, 0x672b10): main thread,
// the place the game starts its own queued loads, and it checks the same
// guards first. Decompiled with tools/ghidra, 2026-09-10.
// This replaces the synthesised Continue click dropped on 2026-08-30 (a screen
// position per resolution, one system cursor shared by two games, no effect on
// any other menu page).
static const uintptr_t RVA_MENUUI_VFTABLE  = 0x301dc38;   // UI::CMenuUI::vftable
static const int       MENUUI_SLOT_UPDATE  = 33;          // -> 0x672b10
static const uintptr_t RVA_MENUUI_UPDATE   = 0x672b10;
static const uintptr_t RVA_START_SAVEGAME  = 0x6785c0;
static const uintptr_t RVA_APP_ACCESSOR    = 0xbb23c0;    // returns the app object; +200 is the save manager
static const uintptr_t RVA_SAVEINFO_GET    = 0x2e6ca0;    // SavegameInfo* (SavegameInfo* out, manager, const SaveGameId*); throws "invalid mount point"
static const uintptr_t RVA_SAVEINFO_DTOR   = 0x2de250;    // SavegameInfo: 0x110 bytes
static const uintptr_t RVA_LOADPARAMS_CTOR = 0x553b70;    // LoadGameParams: 0x138 bytes
static const uintptr_t RVA_LOADPARAMS_DTOR = 0x5576a0;
static const size_t    MENU_OFF_GAMEUI     = 0x4e8;       // non-zero while a game runs (loads then go through the in-game menu)
static const size_t    MENU_OFF_INITING    = 0x1988;      // "Game initialization is already active!"
static const size_t    MENU_OFF_QUEUED     = 0x19a0;      // the menu's own queued load (a future)
typedef void (*MenuUpdateFn)(void*, void*, void*, void*);
static MenuUpdateFn  g_origMenuUpdate = nullptr;
static volatile LONG g_menuUpdates = 0;

// SEH only in this frame (no C++ objects): the save manager throws on a bad id.
static int AutoLoadCall(void* menu, const char* name)
{
    unsigned char id[0x100], info[0x400], params[0x400];   // 0x60 / 0x110 / 0x138 plus slack
    volatile int stage = 0;
    __try {
        memset(id, 0, sizeof(id)); memset(info, 0, sizeof(info)); memset(params, 0, sizeof(params));
        ((GString*)(id + 0x00))->cap = 7;     // path: empty std::wstring (SSO capacity 7)
        ((GString*)(id + 0x20))->cap = 15;    // name
        ((GString*)(id + 0x40))->cap = 15;    // namespace
        g_strAssign(id + 0x20, name, strlen(name));
        g_strAssign(id + 0x40, "savegame", 8);
        stage = 1;
        void* app = ((void* (*)())(g_base + RVA_APP_ACCESSOR))();
        void* mgr = app ? *(void**)((char*)app + 200) : nullptr;
        if (!mgr) { Log("[menu] autoload: no save manager (app=%p)\n", app); return -1; }
        ((void* (*)(void*, void*, void*))(g_base + RVA_SAVEINFO_GET))(info, mgr, id);
        stage = 2;
        ((void* (*)(void*))(g_base + RVA_LOADPARAMS_CTOR))(params);
        g_strAssign(params + 0x00, name, strlen(name));
        stage = 3;
        // OUR load, not the player's: the share observer (OnStartSavegame) has
        // to pass it straight through, or a host would re-share mp_shared the
        // moment it loaded the save it had just shared.
        InterlockedExchange(&g_selfLoad, 1);
        char started = ((char (*)(void*, void*, void*))(g_base + RVA_START_SAVEGAME))(menu, params, info);
        InterlockedExchange(&g_selfLoad, 0);
        stage = 4;
        ((void (*)(void*))(g_base + RVA_LOADPARAMS_DTOR))(params);
        ((void (*)(void*))(g_base + RVA_SAVEINFO_DTOR))(info);
        return started ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[menu] autoload: exception %08lx at stage %d (0 id, 1 save info, 2 load params, 3 StartSavegame, 4 cleanup)\n",
            GetExceptionCode(), (int)stage);
        return -10 - (int)stage;
    }
}

// The dedicated server's load request in flight (DedicatedTick, below): set when doStartLoad
// accepts it, cleared when a world is up or the engine refuses the save.
static ULONGLONG g_dedLoadInFlightSince = 0;
static void AutoLoadTick(void* menu)
{
    if (InterlockedExchange(&g_modLeavePending,0)) { LeaveLobby(); SetStatus(g_modLeaveReason); return; }
    if (InterlockedExchange(&g_modRefreshPending,0)) {
        if (!*(uint64_t*)((char*)menu + MENU_OFF_GAMEUI) && g_origCreatePage) {
            g_origCreatePage((uint64_t)menu,8);
            Log("[menu] requested mod catalogue refresh on load page\n");
        }
    }
    LONG n = InterlockedIncrement(&g_menuUpdates);
    if (n == 1) Log("[menu] autoload: first CMenuUI update seen (this=%p)\n", menu);
    if (!InterlockedCompareExchange(&g_autoLoadPending, 0, 0)) return;
    if (*(uint64_t*)((char*)menu + MENU_OFF_GAMEUI) != 0) {
        InterlockedExchange(&g_autoLoadPending, 0);
        Log("[menu] autoload: a game is running -- the player loads mp_shared\n");
        SetStatus("Save ready -- open LOAD GAME and pick \"mp_shared\".");
        return;
    }
    if (*(uint8_t*)((char*)menu + MENU_OFF_INITING) != 0 || *(uint64_t*)((char*)menu + MENU_OFF_QUEUED) != 0)
        return;   // the game is already starting something: wait (the present watchdog gives up after 12 s)
    InterlockedExchange(&g_autoLoadPending, 0);
    int r = AutoLoadCall(menu, "mp_shared");
    Log("[menu] autoload: StartSavegame(mp_shared) -> %d\n", r);
    if (r != 1) g_dedLoadInFlightSince = 0;   // refused: the dedicated server may ask again
    if (r != 1) SetStatus("The game refused to start mp_shared (a mod it needs is not installed here?) -- open LOAD GAME and pick \"mp_shared\" to see why.");
}

static void MyMenuUpdate(void* menu, void* a2, void* a3, void* a4)
{
    g_origMenuUpdate(menu, a2, a3, a4);   // first and untouched: the frame's arguments go through as they came
    AutoLoadTick(menu);
}

// Place the shared save and start it (in-process when the menu hook is in).
// Returns true once the save is placed; false (with a status) if it could not be.
static bool doStartLoad(const wchar_t* srcSav)
{
    if (!srcSav || !srcSav[0]) { Log("[menu] doStartLoad: empty src\n"); SetStatus("No save to load."); return false; }
    if (!placeSaveNewest(srcSav)) { SetStatus("Couldn't place the shared save -- not loading"); return false; }
    // The menu frame starts it (AUTO-LOAD above). Asking the player to open LOAD GAME
    // stays as the fallback: autoload=0, a hook that did not install, or a save that
    // StartSavegame refused. The old synthesised Continue click needed a screen position
    // per resolution and fought another game on the same PC for the cursor.
    InterlockedExchange(&g_lobbyDone, 1);   // release the keyboard: the lobby's work is done
    if (g_flagAutoLoad && g_origMenuUpdate) {
        g_autoLoadSince = GetTickCount64();
        InterlockedExchange(&g_autoLoadPending, 1);
        Log("[menu] shared save placed as mp_shared -- starting it on the next menu frame\n");
        SetStatus("Save ready -- loading it...");
        return true;
    }
    Log("[menu] shared save placed as mp_shared -- the player loads it from LOAD GAME\n");
    SetStatus("Save ready -- open LOAD GAME and pick \"mp_shared\".");
    return true;
}

// Ask lobby.py to quit, give it up to waitMs to exit cleanly, then kill it. The
// caller must hold a valid process handle (LobbyThread owns pi.hProcess; other
// threads read g_lobbyProc under g_lobbyCs so it cannot be closed under them).
static void QuitLobbyProc(HANDLE proc, int waitMs)
{
    if (!proc) return;
    LobbySend("{\"cmd\":\"quit\"}");
    if (WaitForSingleObject(proc, waitMs) == WAIT_OBJECT_0) { Log("[menu] lobby.py exited on quit\n"); return; }
    // netpunch.exe is a PyInstaller ONEFILE build: the process we launched is a
    // bootstrap that unpacks and spawns the REAL python child, and it is the
    // CHILD that owns the lobby UDP port. TerminateProcess on our handle killed
    // only the bootstrap and ORPHANED the child, which kept udp/29471. Every
    // later HOST click then spawned a lobby that could not bind the port, the
    // overlay tail thread read one event line and died, and the panel looked
    // bricked (measured: two host pairs alive, the stale child owning 29471,
    // 2026-09-08). The Job object exists for exactly this and both processes
    // are in it -- terminate the JOB, taking bootstrap and child together.
    if (g_lobbyJob && TerminateJobObject(g_lobbyJob, 0)) {
        Log("[menu] lobby.py did not exit within %d ms -- terminated the job (bootstrap + child)\n", waitMs);
    } else {
        TerminateProcess(proc, 0);
        Log("[menu] lobby.py did not exit within %d ms -- terminated (no job: child may linger)\n", waitMs);
    }
}

struct LobbyArg { int join; char code[160]; char name[NAME_MAX]; char password[40]; int pub; int sep; int xplay; char lobby[NAME_MAX]; };

static DWORD WINAPI LobbyThread(LPVOID param)
{
    LobbyArg* a = (LobbyArg*)param;
    g_transportLobby[0]=0;
    wchar_t wname[NAME_MAX]; MultiByteToWideChar(CP_UTF8, 0, a->name, -1, wname, NAME_MAX);
    wchar_t cmd[4096];
    // Prefer the frozen netpunch.exe next to the scripts (no Python dependency on
    // the target machine); fall back to `python lobby.py` when only the scripts are there.
    wchar_t exe[600]; _snwprintf_s(exe, _TRUNCATE, L"%s\\netpunch.exe", NETDIR);
    bool haveExe = GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
    wchar_t base[620];
    if (haveExe) _snwprintf_s(base, _TRUNCATE, L"\"%s\\netpunch.exe\"", NETDIR);
    else         wcscpy_s(base, L"python lobby.py");
    // Joiner binds an EPHEMERAL local port (--local-port 0): it only dials out to
    // the open host, so its port needn't be pre-agreed, and this avoids colliding
    // with the host's fixed 29471 when host+joiner share one machine's network
    // stack (the local two-instance test; Sandboxie does not virtualise the net).
    // Game relay: lobby.py listens on 127.0.0.1:<relay port> for the local
    // bridge's lockstep frames and forwards them over the punched socket; frames
    // from the peer are delivered to 127.0.0.1:<bridge port>. The relay port is
    // fixed by role (HOST 7773 / JOIN 7774, known at launch); the bridge port is
    // whatever the bridge reports it bound (re-read now, not cached).
    int relayPort  = pickRelayPort(a->join);
    g_relayPort = relayPort;
    if (a->join && relayPort != GAME_RELAY_PORT_JOIN) Log("[menu] relay port %d is taken (another joiner on this machine) -> using %d\n", GAME_RELAY_PORT_JOIN, relayPort);
    int bridgePort = readBridgePort();
    // Every per-machine log rides to the host's merged lobby_peers.log: the
    // lobby's own lines go automatically; these files are tailed (new lines
    // only for ones that already exist; a file that appears later from its start).
    wchar_t fwd[900];
    _snwprintf_s(fwd, _TRUNCATE,
                 L"--forward-log \"%stpf2_bridge.log\" --forward-log \"%stpf2_menu.log\" "
                 L"--forward-log \"%smp_company_a.log\" --forward-log \"%smp_company_b.log\"",
                 g_dataDirW, ourDirW(), g_dataDirW, g_dataDirW);
    wchar_t wpass[96] = L"";
    if (a->password[0]) { wchar_t wp[40]; MultiByteToWideChar(CP_UTF8, 0, a->password, -1, wp, 40); _snwprintf_s(wpass, _TRUNCATE, L" --password %s", wp); }
    if (a->join) { wchar_t wc[200]; MultiByteToWideChar(CP_UTF8, 0, a->code, -1, wc, 200);
                   _snwprintf_s(cmd, _TRUNCATE, L"%s join %s --name \"%s\" --local-port 0 --game-relay-port %d --game-local-port %d %s%s",
                                base, wc, wname, relayPort, bridgePort, fwd, wpass); }
    else {
        // the public list: always tell the lobby where the master server is (the
        // PUBLIC checkbox can be flipped later, in the lobby); --public starts listed
        wchar_t wpub[560 + NAME_MAX] = L"";
        { wchar_t wl[NAME_MAX]; MultiByteToWideChar(CP_UTF8, 0, a->lobby, -1, wl, NAME_MAX); _snwprintf_s(wpub, _TRUNCATE, L" --lobby-name \"%s\"", wl); }
        if (g_flagMaster[0]) { wchar_t wm[300]; MultiByteToWideChar(CP_UTF8, 0, g_flagMaster, -1, wm, 300);
                               wchar_t t[400]; _snwprintf_s(t, _TRUNCATE, L" --publish %s%s", wm, a->pub ? L" --public" : L""); wcscat_s(wpub, t); }
        if (g_flagShareMods == 2) wcscat_s(wpub, L" --no-share-mods");   // the host never sends its mods either
        if (a->sep) wcscat_s(wpub, L" --companies");                    // SEPARATE COMPANIES: the lobby assigns a company per player
        if (a->xplay) wcscat_s(wpub, L" --crossplay");                  // CROSS-PLAY: the classic code, not the Steam ID
        if (g_flagDedicated) wcscat_s(wpub, L" --dedicated");            // a stable code across restarts, listed as a dedicated server
        if (g_flagDedicated && g_flagDedPort) { wchar_t t[40]; _snwprintf_s(t, _TRUNCATE, L" --local-port %d", g_flagDedPort); wcscat_s(wpub, t); }
        _snwprintf_s(cmd, _TRUNCATE, L"%s host --name \"%s\" --game-relay-port %d --game-local-port %d %s%s%s",
                     base, wname, relayPort, bridgePort, fwd, wpass, wpub);
    }
    { wchar_t recoveryArgs[1100];
      // A trailing backslash escapes the closing quote in Windows argv.
      // Forward slashes are accepted by Python/Windows for both directory paths.
      std::wstring runtime = g_dataDirW, saves = g_saveDirW;
      for (auto& c : runtime) if (c == L'\\') c = L'/';
      for (auto& c : saves) if (c == L'\\') c = L'/';
      _snwprintf_s(recoveryArgs, _TRUNCATE, L" --sync-runtime-dir \"%s\" --save-dir \"%s\" --game-pid %lu",
          runtime.c_str(), saves.c_str(), GetCurrentProcessId());
      wcscat_s(cmd, recoveryArgs); }
    { wchar_t shown[4096]; wcscpy_s(shown, cmd); wchar_t* pp = wcsstr(shown, L" --password "); if (pp) wcscpy_s(pp, _countof(shown) - (pp - shown), L" --password ***");
      Log("[menu] lobby cmd: %ls\n", shown); }

    wchar_t outPath[512]; _snwprintf_s(outPath, _TRUNCATE, L"%s\\lobby_out.jsonl", NETDIR);
    wchar_t inPath[512];  _snwprintf_s(inPath,  _TRUNCATE, L"%s\\lobby_in.jsonl", NETDIR);
    wchar_t logPath[512]; _snwprintf_s(logPath, _TRUNCATE, L"%s\\lobby_proc.log", NETDIR);
    DeleteFileW(outPath); DeleteFileW(inPath);
    if (a->join) {   // never let last session's transfer pass for this one (lobby.py does this too)
        static const wchar_t* const stale[] = { L"incoming_save.sav", L"incoming_save.sav.lua", L"incoming_save.jpg" };
        for (const wchar_t* nm : stale) { wchar_t f[560]; _snwprintf_s(f, _TRUNCATE, L"%s\\%s", NETDIR, nm); DeleteFileW(f); }
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    // KEEP LOGS: with <data dir>\tpf2mp_keep_logs.txt present the previous run's
    // lobby_proc.log is kept and this run appends after a banner.
    wchar_t keepFlag[MAX_PATH]; _snwprintf_s(keepFlag, _TRUNCATE, L"%stpf2mp_keep_logs.txt", g_dataDirW);
    const bool keepLogs = g_dataDirW[0] && GetFileAttributesW(keepFlag) != INVALID_FILE_ATTRIBUTES;
    HANDLE hLog = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, keepLogs ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (keepLogs && hLog != INVALID_HANDLE_VALUE) {
        SetFilePointer(hLog, 0, nullptr, FILE_END);
        SYSTEMTIME st; GetLocalTime(&st);
        char banner[160]; int n = snprintf(banner, sizeof(banner), "\n==== lobby session %04u-%02u-%02u %02u:%02u:%02u (tpf2mp_keep_logs.txt present: appending) ====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        DWORD w; WriteFile(hLog, banner, (DWORD)n, &w, nullptr);
    }
    STARTUPINFOW si = { sizeof(si) };
    // Redirect the lobby's output ONLY if the log actually opened. Handing
    // CreateProcess an INVALID_HANDLE_VALUE as stdout/stderr gives the child a
    // broken stdout: the first print() throws and the lobby dies seconds after
    // start, having bound its ports but written no roster -- the menu then waits
    // forever for events that never come, while the peer's own lobby is fine
    // (the save still transfers, lobby-to-lobby). That is exactly the shape of
    // the 2026-08-30 cross-network session on this machine: process alive, ports
    // owned, lobby_proc.log untouched since the previous run.
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hLog; si.hStdError = hLog;
    } else {
        Log("[menu] cannot open %ls (err %lu) -- launching the lobby WITHOUT output capture\n",
            logPath, GetLastError());
    }
    PROCESS_INFORMATION pi = {};
    SetStatus(a->join ? "Joining lobby…" : "Starting lobby…");
    BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, NETDIR, &si, &pi);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (!ok) { SetStatus("Couldn't start Python — is it on PATH?"); free(a); return 0; }
    EnsureLobbyJob();
    if (g_lobbyJob && !AssignProcessToJobObject(g_lobbyJob, pi.hProcess))
        Log("[menu] AssignProcessToJobObject failed (err %lu) -- the lobby may outlive a crash\n", GetLastError());
    g_lobbyProc = pi.hProcess;

    // tail lobby_out.jsonl line by line
    // rem accumulates one event line up to its newline, whatever its length: a
    // roster of a few dozen players is well past any fixed line buffer, and a
    // line cut short would parse as a smaller roster (wrong letters, a load
    // gate waiting for the wrong count).
    LARGE_INTEGER off = { 0 }; std::string remS; char buf[8192];
    bool stop = false;   // set once the game load is under way: lobby.py is done, end the tail
    for (;;) {
        HANDLE h = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
            DWORD got = 0;
            while (!stop && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
                off.QuadPart += got;
                for (DWORD i = 0; i < got && !stop; i++) {
                    char c = buf[i];
                    if (c == '\n') {
                        const char* rem = remS.c_str();
                        // dispatch one event line
                        InterlockedExchange(&g_lobbyReady, 1);   // lobby.py is up and has truncated lobby_in.jsonl
                        char ty[24]; jsonStr(rem, "type", ty, sizeof(ty));
                        if (strcmp(ty, "code") == 0) { char cd[160]; jsonStr(rem, "code", cd, sizeof(cd)); if (cd[0]) {
                            // the lobby re-emits the code on a CROSS-PLAY switch: only the first starts the snapshot
                            if (g_isHost && !InterlockedExchange(&g_codeSeen, 1)) {
                                if (WorldLoaded()) SyncStart("host: initial world snapshot");
                            }
                            char sid[40]; jsonStr(rem, "steam", sid, sizeof(sid));
                            bool xp = jsonBool(rem, "crossplay", true);
                            InterlockedExchange(&g_hostSteam, sid[0] ? 1 : 0);
                            InterlockedExchange(&g_crossplay, xp ? 1 : 0);
                            strcpy_s(g_code, cd); ClipboardSet(cd); InterlockedExchange(&g_haveCode, 1);
                            SetStatus(!sid[0] ? "Your code is copied — share it in Discord."
                                      : xp ? "Cross-play code copied — anyone can join with it, Steam or not."
                                           : "Your Steam ID is the code and is copied — friends on Steam join with it.");
                            InterlockedExchange(&g_panelDirty, 1); } }
                        else if(strcmp(ty,"transport_lobby")==0) {
                            char epoch[40]; jsonStr(rem,"epoch",epoch,sizeof(epoch));
                            if(strlen(epoch)==32 && strspn(epoch,"0123456789abcdef")==32 &&
                               strcmp(epoch,g_transportLobby)) {
                                strcpy_s(g_transportLobby,epoch);
                                writeBridgeCtl(g_isHost != 0);
                            }
                        }
                        else if(strcmp(ty,"sync_prompt")==0) {
                            char phase[24]; jsonStr(rem,"phase",phase,sizeof(phase));
                            EnterCriticalSection(&g_modelCs);
                            const bool idle = !g_recoveryPhase[0] || !strcmp(g_recoveryPhase,"complete") ||
                                !strcmp(g_recoveryPhase,"detected") || !strcmp(g_recoveryPhase,"unavailable");
                            if(idle && (!strcmp(phase,"detected") || !strcmp(phase,"unavailable"))) {
                                strcpy_s(g_recoveryPhase,phase);
                                g_recoveryDetail[0]=g_recoveryFailedStep[0]=0;
                                g_recoveryRequestedAt=0;
                                InterlockedExchange(&g_recoveryPresent,1);
                                InterlockedExchange(&g_uiState,3);
                                InterlockedExchange(&g_panelDirty,1);
                            } else if(idle && !strcmp(phase,"clear")) {
                                g_recoveryPhase[0]=0;
                                InterlockedExchange(&g_recoveryPresent,0);
                                if(g_uiState==3) InterlockedExchange(&g_uiState,0);
                                InterlockedExchange(&g_panelDirty,1);
                            }
                            LeaveCriticalSection(&g_modelCs);
                        }
                        else if(strcmp(ty,"sync_feedback")==0) {
                            EnterCriticalSection(&g_modelCs);
                            g_recoveryRequestedAt=0;
                            jsonStr(rem,"detail",g_recoveryDetail,sizeof(g_recoveryDetail));
                            InterlockedExchange(&g_panelDirty,1);
                            LeaveCriticalSection(&g_modelCs);
                        }
                        else if(strcmp(ty,"sync_ready_state")==0) {
                            char phase[24],kind[24]; jsonStr(rem,"phase",phase,sizeof(phase));
                            jsonStr(rem,"kind",kind,sizeof(kind));
                            EnterCriticalSection(&g_modelCs);
                            g_recoveryRequestedAt=0;
                            if(!strcmp(phase,"waiting")) {
                                strcpy_s(g_recoveryPhase,"readiness");
                                jsonStr(rem,"token",g_readyToken,sizeof(g_readyToken));
                                g_readyCount=pubInt(rem,"ready_count",0); g_readyTotal=pubInt(rem,"total",0);
                                g_readyMine=pubInt(rem,"is_ready",0)!=0;
                                g_recoveryDetail[0]=0;
                                InterlockedExchange(&g_recoveryPresent,1); InterlockedExchange(&g_uiState,3);
                            } else if(!strcmp(g_recoveryPhase,"readiness")) {
                                if(!strcmp(phase,"cancelled")) {
                                    strcpy_s(g_recoveryPhase,!strcmp(kind,"sync_retry") ? "error" : "detected");
                                    strcpy_s(g_recoveryDetail,"The player list changed. The host must request readiness again.");
                                } else strcpy_s(g_recoveryPhase,"holding");
                            }
                            LeaveCriticalSection(&g_modelCs);
                            InterlockedExchange(&g_panelDirty,1);
                        }
                        else if(strcmp(ty,"sync_state")==0) {
                            char operation[40],epoch[40],phase[24],detail[420];
                            jsonStr(rem,"operation",operation,sizeof(operation)); jsonStr(rem,"epoch",epoch,sizeof(epoch));
                            jsonStr(rem,"phase",phase,sizeof(phase)); jsonStr(rem,"detail",detail,sizeof(detail));
                            EnterCriticalSection(&g_modelCs);
                            g_recoveryRequestedAt=0;
                            strcpy_s(g_recoveryOperation,operation); strcpy_s(g_recoveryEpoch,epoch);
                            strcpy_s(g_recoveryPhase,phase); strcpy_s(g_recoveryDetail,detail);
                            InterlockedExchange(&g_recoveryWorldIo, !strcmp(phase,"saving") || !strcmp(phase,"loading"));
                            jsonStr(rem,"step",g_recoveryFailedStep,sizeof(g_recoveryFailedStep));
                            LeaveCriticalSection(&g_modelCs);
                            SetStatus(!strcmp(phase,"complete") ? "Resync complete." : "");
                            InterlockedExchange(&g_recoveryPresent,1);
                            // Keep the same panel from the desync notice through recovery.
                            // Native input remains usable while game widgets are held.
                            if(!strcmp(phase,"complete")) {
                                InterlockedExchange(&g_recoveryPresent,0);
                                InterlockedExchange(&g_lobbyDone,1);
                                // Back to the LOBBY VIEW when the panel is open (2026-09-18, user:
                                // "keep the lobby view up after we load into the game"): the loading
                                // screen hands the panel over to the game open on the roster, then the
                                // frozen join's phases took it to this recovery view and its completion
                                // collapsed it to nothing. Collapsed only when nothing had it open.
                                if(InterlockedCompareExchange(&g_uiState,0,0)==3) {
                                    const bool open = InterlockedCompareExchange(&g_ingameOverlay,0,0) || InterlockedCompareExchange(&g_showOverlay,0,0);
                                    InterlockedExchange(&g_uiState, open ? 2 : 0);
                                }
                            } else {
                                InterlockedExchange(&g_uiState,3);
                            }
                            InterlockedExchange(&g_panelDirty,1);
                        }
                        else if (strcmp(ty, "roster") == 0) applyRoster(rem);
                        else if (strcmp(ty, "chat") == 0) {
                            std::string fr = jsonStrS(rem, "from"); char tx[256]; jsonStr(rem, "text", tx, sizeof(tx));
                            if (strncmp(tx, "!hotjoin ", 9) == 0) {
                                // the host is saving for a newcomer: only a panel still at
                                // the title menu needs it, as its status line
                                if (InterlockedCompareExchange(&g_showOverlay, 0, 0) != 0 && !g_gameUi) SetStatus(tx + 9);
                            } else { chatPush(fr.c_str(), tx); speedFromChat(tx); }
                        }
                        else if (strcmp(ty, "status") == 0) {
                            char de[200], state[24]; jsonStr(rem, "detail", de, sizeof(de)); jsonStr(rem,"state",state,sizeof(state));
                            SaveStartStatus(state,de);
                            if (de[0]) SetStatus(de);
                        }
                        else if (strcmp(ty, "transfer") == 0) {
                            char role[16], st[16]; jsonStr(rem, "role", role, sizeof(role)); jsonStr(rem, "state", st, sizeof(st));
                            int pct = jsonInt(rem, "pct"); char msg[96];
                            char peer[40]; jsonStr(rem, "peer", peer, sizeof(peer));
                            bool toRelay = strcmp(peer, "relay") == 0;
                            if (strcmp(st, "done") == 0) { SetStatus(WorldLoaded() ? "Game running. New players can join this lobby." : "Save transfer complete."); g_xfer[0] = 0; }
                            else if (st[0]) { g_xfer[0] = 0; }
                            else if (strcmp(role, "recv") == 0) { if (pct >= 0) { snprintf(msg, sizeof(msg), "Receiving save\xE2\x80\xA6 %d%%", pct); SetStatus(msg); snprintf(g_xfer, sizeof(g_xfer), "receiving %d%%", pct); } }
                            else if (pct >= 0) { snprintf(msg, sizeof(msg), toRelay ? "Uploading save to the relay\xE2\x80\xA6 %d%%" : "Sending save\xE2\x80\xA6 %d%%", pct); SetStatus(msg);
                                                 snprintf(g_xfer, sizeof(g_xfer), toRelay ? "uploading %d%%" : "sending %d%%", pct); }
                            if (pct >= 100) g_xfer[0] = 0;
                            writeBridgeCtl(g_isHost != 0);   // the in-game window reads xfer= from the ctl
                        }
                        else if (strcmp(ty, "mods_prompt") == 0) {
                            // the host's save needs mods we lack: ask the player, unless the
                            // share_mods flag already answers for them
                            g_modsOffer=jsonInt(rem,"offer");
                            int n = jsonInt(rem, "count"); char tx[200]; jsonStr(rem, "text", tx, sizeof(tx));
                            if (g_flagShareMods == 1) { char answer[128]; snprintf(answer,sizeof(answer),"{\"cmd\":\"mods\",\"accept\":true,\"offer\":%d}",g_modsOffer); LobbySend(answer); SetStatus("Downloading the mods this save needs from the host\xE2\x80\xA6"); }
                            else if (g_flagShareMods == 2) { LobbySend("{\"cmd\":\"mods\",\"accept\":false}"); SetStatus("Mod download is off (share_mods=never)."); }
                            else if (g_csInit) {
                                EnterCriticalSection(&g_statusCs);
                                snprintf(g_modsPrompt, sizeof(g_modsPrompt), "Download %d mod(s) this save needs from the host? (%s)", n, tx);
                                LeaveCriticalSection(&g_statusCs);
                                InterlockedExchange(&g_panelDirty, 1);
                            }
                        }
                        else if (strcmp(ty, "mods_clear") == 0) { EnterCriticalSection(&g_statusCs); g_modsPrompt[0]=0; LeaveCriticalSection(&g_statusCs); InterlockedExchange(&g_panelDirty,1); }
                        else if (strcmp(ty, "mods_refresh") == 0) { InterlockedExchange(&g_modRefreshPending,1); SetStatus("Registering downloaded mods..."); }
                        else if (strcmp(ty, "mods_cancelled") == 0) { jsonStr(rem,"text",g_modLeaveReason,sizeof(g_modLeaveReason)); InterlockedExchange(&g_modLeavePending,1); }
                        else if (strcmp(ty, "mods_ready") == 0) { SetStatus("Mods received \xE2\x80\x94 waiting for start\xE2\x80\xA6"); }
                        else if (strcmp(ty, "save_ready") == 0) { InterlockedExchange(&g_saveReady, 1); SetStatus("Save received \xE2\x80\x94 waiting for start\xE2\x80\xA6"); }
                        else if (strcmp(ty, "start") == 0) {
                            InterlockedExchange(&g_saveStartPending,0);
                            // {"type":"start","save":true|false}: save=true means a save
                            // transfer completed for this peer this session; absent => true.
                            bool withSave = jsonBool(rem, "save", true);
                            // "switch":true -- the host left the world it was in and
                            // everyone follows it into this save (see OnStartSavegame).
                            bool isSwitch = jsonBool(rem, "switch", false);
                            bool saveReady = InterlockedCompareExchange(&g_saveReady, 0, 0) != 0;
                            const bool amHost = InterlockedCompareExchange(&g_isHost, 0, 0) != 0;
                            if (amHost) InterlockedExchange(&g_sessionStarted, 1);
                            wchar_t src[600] = L""; bool go = true, inPlace = false;
                            if (amHost && isSwitch) {
                                // the host IS the new world: it loaded it itself
                                Log("[menu] start(switch) on the host -- it is already in the new world\n");
                                go = false;
                            } else if (amHost && InterlockedExchange(&g_hostLoadedItself, 0)) {
                                // the host loaded the save from the game's own LOAD GAME:
                                // the game is already loading it, the joiners load theirs
                                Log("[menu] start: the host loaded its save itself -- nothing to load here\n");
                                go = false;
                            } else if (isSwitch) {
                                // A WORLD SWITCH is for us even though we are playing.
                                if (!(withSave && saveReady)) {
                                    Log("[menu] start(switch) but no save arrived this session -- staying in this world\n");
                                    SetStatus("The host switched world but its save did not arrive."); go = false;
                                } else {
                                    _snwprintf_s(src, _TRUNCATE, L"%s\\incoming_save.sav", NETDIR);
                                    inPlace = WorldLoaded();
                                    // g_saveReady is latched for the session; a LATER switch
                                    // must wait for its own transfer, not reuse this one
                                    InterlockedExchange(&g_saveReady, 0);
                                }
                            } else if (InterlockedCompareExchange(&g_showOverlay, 0, 0) == 0 && g_gameUi) {
                                // HOT JOIN: we are already playing; this start is the
                                // sync save going out to a newcomer. Nothing to load here.
                                Log("[menu] start while in game -- a sync for a newcomer, ignored here\n");
                                go = false;
                            } else if (amHost && !(withSave && saveReady)) {
                                // our own save (the one we shared / uploaded); a leader that RECEIVED a
                                // save this session (a relay loading its stored world) falls through and loads that.
                                // No guessing: our newest save need not be what anyone else has.
                                wcscpy_s(src, g_startSaveW);
                                if (!src[0]) {
                                    Log("[menu] start: we shared no save this session -- not loading\n");
                                    SetStatus("No save was shared -- press START GAME again"); go = false;
                                }
                            } else if (saveReady) {
                                _snwprintf_s(src, _TRUNCATE, L"%s\\incoming_save.sav", NETDIR);
                            } else {
                                // No save arrived this session. Loading our own newest save instead
                                // would put this player in a different world from everyone else.
                                Log("[menu] start(save=%d) but no save_ready this session -- not loading\n", withSave ? 1 : 0);
                                SetStatus("Start received but no save arrived -- ask the host to START again"); go = false;
                            }
                            if (go && inPlace) {
                                // IN GAME: there is no menu to hand the load to. Place the
                                // save as mp_shared and ask the engine for it the way the
                                // resync does (NativeIo::Load -> CMenuUI::StartSavegame on
                                // the engine's own thread). The mod re-initialises on the
                                // new world and catches up from the host like a hot joiner.
                                writeCompanyCfg();
                                SetStatus("Loading the host's new world\xE2\x80\xA6");
                                char op[64]; snprintf(op, sizeof(op), "world_switch_%lu", (unsigned long)GetTickCount64());
                                InterlockedExchange(&g_worldGenHold, 1);   // the token the new world stamps is ours
                                bool queued = placeSaveNewest(src) && NativeIo::Load(op, "mp_shared");
                                if (queued) {
                                    Log("[menu] world switch: loading mp_shared in place (%s)\n", op);
                                    ArmStageWatch("loading the host's new world");
                                } else {
                                    Log("[menu] world switch: the engine would not take the in-place load -- the player loads mp_shared\n");
                                    SetStatus("The host changed world -- open LOAD GAME and pick \"mp_shared\".");
                                }
                            } else if (go) {
                                writeCompanyCfg();
                                InterlockedExchange(&g_worldGenHold, 1);   // the token the new world stamps is ours
                                SetStatus(isSwitch ? "Loading the host's new world…" : "Loading shared save…"); Sleep(400);
                                if (doStartLoad(src)) {
                                    ArmStageWatch("loading world");
                                    // The game is loading. The lobby process STAYS ALIVE: since the
                                    // game-frame relay (--game-relay-port) the lobby IS the lockstep
                                    // transport between machines -- quitting it here left both bridges
                                    // pointed at relay ports that no longer existed (peer=? on both
                                    // sides, 2026-08-30). It is torn down only by LEAVE / re-HOST /
                                    // process exit. Keep tailing lobby_out (roster heals, relay stats).
                                    Log("[menu] game loading -- lobby kept alive as the game transport\n");
                                }
                            }
                        }
                        remS.clear();
                    } else remS.push_back(c);
                }
            }
            CloseHandle(h);
        }
        if (stop || WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        EnterCriticalSection(&g_modelCs);
        if(g_recoveryRequestedAt && GetTickCount64()-g_recoveryRequestedAt > 5000) {
            g_recoveryRequestedAt=0;
            strcpy_s(g_recoveryDetail,"The lobby did not confirm this request within 5 seconds. Reopen Manage Lobby, check the connection, and try again.");
            InterlockedExchange(&g_panelDirty,1);
        }
        LeaveCriticalSection(&g_modelCs);
        SyncPoll();
        // relay lobbies: the relay's copy of the world is whatever was last
        // uploaded, so the leader refreshes it on a timer -- a resume after
        // everyone left is then at most this many minutes old
        if (g_flagRelayAutosaveMin > 0 && InterlockedCompareExchange(&g_lobbyRelay, 0, 0) && InterlockedCompareExchange(&g_isHost, 0, 0)
            && g_gameUi && InterlockedCompareExchange(&g_showOverlay, 0, 0) == 0) {
            static ULONGLONG lastUp = 0; ULONGLONG nowT = GetTickCount64();
            if (!lastUp) lastUp = nowT;
            if (nowT - lastUp >= (ULONGLONG)g_flagRelayAutosaveMin * 60000ULL) { lastUp = nowT; SyncStart("relay: periodic save"); }
        }
        Sleep(200);
    }
    // Say why the tail ended. A lobby that exits on its own -- rather than after
    // a start or a LEAVE -- left the player staring at "Starting lobby..." with no
    // explanation; the exit code and whether any event was ever read narrow it to
    // the process dying vs the file IPC never producing anything.
    {
        DWORD code = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &code);
        if (!stop) {
            Log("[menu] lobby process exited (code %lu) after %ld event line(s)\n",
                code, InterlockedCompareExchange(&g_lobbyReady, 0, 0));
            if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0))
                SetStatus("The lobby stopped before it reported anything -- see tpf2_menu.log");
        }
    }
    // Close under g_lobbyCs so a concurrent LeaveLobby/StartLobby never waits on
    // or terminates a handle that has just been closed (and possibly reused).
    if (g_lobbyCsInit) EnterCriticalSection(&g_lobbyCs);
    if (g_lobbyProc == pi.hProcess) g_lobbyProc = nullptr;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (g_lobbyCsInit) LeaveCriticalSection(&g_lobbyCs);
    free(a);
    Log("[menu] lobby tail thread exit\n");
    return 0;
}

// Tear down the running lobby: quit/kill lobby.py, and optionally join the tail
// thread (StartLobby must, so the new thread's lobby_out/in reset cannot race the
// old tail; LeaveLobby runs on the present thread and only kills the process).
static void TeardownLobby(int waitMs, bool joinThread)
{
    if (g_lobbyCsInit) EnterCriticalSection(&g_lobbyCs);
    QuitLobbyProc(g_lobbyProc, waitMs);
    if (g_lobbyCsInit) LeaveCriticalSection(&g_lobbyCs);
    // The lobby's company assignment dies with the lobby. Left behind, the file
    // made a single-player load read as a two-company lobby session and the
    // save's own companies record was the loser (2026-09-18); with no file the
    // game script keeps whatever companies the save carries.
    { wchar_t cfg[MAX_PATH]; _snwprintf_s(cfg, _TRUNCATE, L"%smp_company_cfg.txt", g_dataDirW); DeleteFileW(cfg); }
    if (joinThread && g_lobbyThread) {
        if (WaitForSingleObject(g_lobbyThread, 3000) != WAIT_OBJECT_0) Log("[menu] lobby tail thread did not exit in time\n");
        CloseHandle(g_lobbyThread); g_lobbyThread = nullptr;
    }
}

static void StartLobby(int join)
{
    if (LobbyRunning()) {
        InterlockedExchange(&g_uiState, 2);
        InterlockedExchange(&g_panelDirty, 1);
        return;
    }
    ensureUsername();
    LobbyArg* a = (LobbyArg*)calloc(1, sizeof(LobbyArg)); if (!a) return;
    a->join = join; strcpy_s(a->name, g_username); strcpy_s(a->password, g_passCode);
    a->pub = InterlockedCompareExchange(&g_public, 0, 0) ? 1 : 0;
    a->sep = InterlockedCompareExchange(&g_sepCompanies, 0, 0) ? 1 : 0;
    a->xplay = InterlockedCompareExchange(&g_crossplay, 0, 0) ? 1 : 0;
    InterlockedExchange(&g_codeSeen, 0); InterlockedExchange(&g_hostSteam, 0);
    InterlockedExchange(&g_joinFocus, 0); SaveNames();
    if (g_lobbyName[0]) strcpy_s(a->lobby, g_lobbyName); else snprintf(a->lobby, sizeof(a->lobby), "%s's game", g_username);
    if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); g_lobbyTitle.clear(); LeaveCriticalSection(&g_modelCs); }
    // (no game name: the public list shows the server type, not the host's newest save)
    if (join) {
        if (g_joinLen >= 8) strcpy_s(a->code, g_joinCode);
        else if (!ClipboardGet(a->code, sizeof(a->code)) || strlen(a->code) < 8) {
            SetStatus("Paste or type your host's code in the field first."); free(a); return;
        }
        char* s = a->code; while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t') memmove(s, s + 1, strlen(s));
        int L = (int)strlen(s); while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\r' || s[L-1] == '\n' || s[L-1] == '\t' || s[L-1] == '/')) s[--L] = 0;
        // A Steam ID is a code too (2026-09-22): 17 digits, or a pasted profile URL
        // (steamcommunity.com/profiles/<id>) reduced to its digits here.
        { const char* pr = strstr(s, "/profiles/"); if (pr) memmove(s, pr + 10, strlen(pr + 10) + 1); }
        bool steamId = strlen(s) == 17 && strspn(s, "0123456789") == 17;
        // The code becomes a netpunch.exe argument. It is base32 (or a Steam ID's
        // digits) by construction, so refuse anything else: a crafted "code" from
        // Discord must never be able to smuggle extra arguments (e.g. --forward-log
        // <any file>) in.
        if (!steamId) { int k = 0; for (; a->code[k]; k++) { char c = a->code[k]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7') || c == '=')) break; }
          if (a->code[k] || k > 200) { SetStatus("That is not a valid code (a host's Steam ID, or letters A-Z and digits 2-7)."); free(a); return; } }
    }
    // A previous lobby.py still up (LEAVE not pressed, or a tail thread still
    // finishing) would fight the new one over lobby_in/out.jsonl: tear it down first.
    if (g_lobbyProc || g_lobbyThread) TeardownLobby(1500, true);
    InterlockedExchange(&g_lobbyReady, 0);
    InterlockedExchange(&g_saveReady, 0);
    InterlockedExchange(&g_sessionStarted, 0);
    g_selectedSave.clear(); g_savePicker=false; g_lobbySaves.clear(); g_savePage=0;
    InterlockedExchange(&g_saveStartPending,0);
    InterlockedExchange(&g_hostLoadedItself, 0);
    InterlockedExchange(&g_worldGenHold, 0);
    InterlockedExchange(&g_switchShare, 0);
    g_worldGen[0] = 0;
    InterlockedExchange(&g_isHost, join ? 0 : 1);
    InterlockedExchange(&g_lobbyDone, 0);   // a new lobby captures typing again
    InterlockedExchange(&g_uiState, 2); InterlockedExchange(&g_panelDirty, 1);
    g_chatCount = 0; g_chatHead = 0;
    if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); g_players.clear(); g_companies.clear(); g_letters.clear(); LeaveCriticalSection(&g_modelCs); }
    g_lobbyThread = CreateThread(nullptr, 0, LobbyThread, a, 0, nullptr);
    if (!g_lobbyThread) { free(a); SetStatus("Couldn't start the lobby thread."); }
}
static void LeaveLobby()
{
    TeardownLobby(1500, false);   // quit, wait up to 1.5 s for a clean exit, then kill
    EnterCriticalSection(&g_statusCs); g_modsPrompt[0] = 0; LeaveCriticalSection(&g_statusCs);
    InterlockedExchange(&g_modRefreshPending, 0);
    InterlockedExchange(&g_uiState, 1); InterlockedExchange(&g_panelDirty, 1);
}

// ---------------- dedicated server ----------------
// Runs once a second from myPresent while dedicated=1. Three states, each
// self-healing: no lobby -> host one; lobby but no world -> load the configured
// (else the newest) save through the ordinary shared-save autoload, so a joiner
// arriving later reuses that save and the lobby counts the host as started;
// world up -> force the game's own autosave every dedicated_autosave_min, so a
// crash restarts from a fresh world (the newest save is what the next launch
// loads). A crash to the title menu leaves the lobby (g_leaveOnMenu), and the
// first state hosts again.
static ULONGLONG g_dedLastHost = 0, g_dedLastLoad = 0, g_dedLastSave = 0, g_dedWorldUpSince = 0, g_dedMenuSince = 0;
static bool g_dedFileWritten = false;
static const ULONGLONG DED_MENU_SETTLE_MS = 8000;   // the title menu's main page has been up this long before we act on it
// ONE LOAD REQUEST PER LOAD (2026-09-21). The request used to repeat every minute while no
// world was up, guarded only by g_autoLoadPending and NativeIo::Busy() -- and this load is
// neither: the menu frame clears g_autoLoadPending the moment it calls StartSavegame, and
// NativeIo only knows the loads IT queued. A load on the VPS takes 3-5 minutes, so every
// start asked two or three more times (t+110, t+170, t+220 s). Each extra request copied the
// save over mp_shared.sav while the engine was reading it and shared the save again, and the
// lobby answered with a load of its own: the world came up, dropped to the title menu and
// loaded a second time (7 minutes to playable instead of 4). A player who joined in between
// found the server's mod not running: the pause for his frozen join was never acknowledged,
// the recovery timed out ("Timed out waiting for all players"), and the error record it left
// in the data folder held every later start at speed 0. So: a request is in flight from the
// moment doStartLoad accepts it until a world is up; only a load that has shown no world for
// DED_LOAD_GIVE_UP_MS is asked for again, and that is logged.
static const ULONGLONG DED_LOAD_GIVE_UP_MS = 30ull * 60 * 1000;
static const ULONGLONG DED_LOAD_MIN_UPTIME_MS = 45000;   // and this long before the first LOAD: two launches that loaded ~10 s in crashed mid-load (16:26, no assertion)
static void DedicatedTick()
{
    if (!g_flagDedicated) return;
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG lastTick = 0;
    if (now - lastTick < 1000) return;   // by the clock: on a software renderer 60 presents of the title menu took minutes
    lastTick = now;
    if (!g_dedFileWritten) {
        // the mod's half: pause while nobody else is in (mp/pacing.lua CM.dedicatedPauseEmpty)
        g_dedFileWritten = true;
        wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%smp_dedicated.txt", g_dataDirW);
        FILE* f = _wfsopen(p, L"w", _SH_DENYNO);
        if (f) { fprintf(f, "dedicated=1\nempty_speed=%d\npause_empty=%d\npin_batch=%d\n", g_flagDedEmptySpeed, g_flagDedEmptySpeed == 0 ? 1 : 0, g_flagDedPinBatch); fclose(f); }
    }
    const bool world = WorldLoaded();
    // At the title menu, act only once its main page (CreatePage 2 -> g_showOverlay) has
    // been up for a while. Hosting on the first CMenuUI frame and loading a second later
    // ran StartSavegame inside the page's own InvokeStoredFunctions and the engine
    // asserted `!m_isInvoking` (the first run on the VPS, 2026-09-18 16:05).
    if (!world) {
        if (!InterlockedCompareExchange(&g_showOverlay, 0, 0)) { g_dedMenuSince = 0; return; }
        if (!g_dedMenuSince) g_dedMenuSince = now;
        if (now - g_dedMenuSince < DED_MENU_SETTLE_MS) return;
    }
    if (!LobbyRunning()) {
        if (world) return;   // a world without a lobby is the moment after a crash to the menu: wait for it
        if (now - g_dedLastHost < 15000) return;
        g_dedLastHost = now;
        if (g_flagDedName[0]) strcpy_s(g_username, g_flagDedName);
        if (g_flagDedLobby[0]) strcpy_s(g_lobbyName, g_flagDedLobby);
        strcpy_s(g_passCode, g_flagDedPassword); g_passLen = (int)strlen(g_passCode);
        InterlockedExchange(&g_public, g_flagDedPublic ? 1 : 0);
        InterlockedExchange(&g_sepCompanies, g_flagDedCompanies ? 1 : 0);
        ensureUsername();
        Log("[dedicated] hosting lobby '%s' as '%s' (%s, %s)\n", g_lobbyName[0] ? g_lobbyName : "(default)", g_username,
            g_flagDedPublic ? "public" : "unlisted", g_flagDedPassword[0] ? "password" : "open");
        StartLobby(0);
        return;
    }
    if (!world) {
        g_dedWorldUpSince = 0;
        if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) return;
        if (InterlockedCompareExchange(&g_autoLoadPending, 0, 0) || NativeIo::Busy()) return;
        if (now - g_dedLastLoad < 60000) return;   // never more than one request a minute
        if (g_dedLoadInFlightSince) {
            if (now - g_dedLoadInFlightSince < DED_LOAD_GIVE_UP_MS) return;   // that load is still running: leave it alone
            Log("[dedicated] the load asked for %llu min ago never produced a world -- asking again\n", (now - g_dedLoadInFlightSince) / 60000);
            g_dedLoadInFlightSince = 0;
        }
        if (now - g_dedMenuSince < DED_LOAD_MIN_UPTIME_MS) return;   // let the engine finish its own start-up work first
        g_dedLastLoad = now;
        wchar_t path[600] = L"";
        if (g_flagDedSave[0]) {
            wchar_t wn[64]; MultiByteToWideChar(CP_UTF8, 0, g_flagDedSave, -1, wn, 64);
            _snwprintf_s(path, _TRUNCATE, L"%s\\%s.sav", SAVE_DIR, wn);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { Log("[dedicated] %ls not found -- loading the newest save instead\n", path); path[0] = 0; }
        }
        if (!path[0] && !newestSave(path, 600)) {
            Log("[dedicated] no save in %ls -- nothing to load (put one there, or set dedicated_save)\n", SAVE_DIR);
            return;
        }
        wcscpy_s(g_startSaveW, path);
        MarkSaveShared();                     // a joiner arriving before the first autosave reuses this save
        Log("[dedicated] loading %ls\n", path);
        if (doStartLoad(path)) { g_dedLoadInFlightSince = now; ArmStageWatch("loading world"); }
        return;
    }
    g_dedLoadInFlightSince = 0;   // the world is up: the request is served
    if (!g_dedWorldUpSince) g_dedWorldUpSince = now;
    if (NoRender()) {
        static ULONGLONG lastCount = 0;
        if (now - lastCount >= 60000) {
            lastCount = now;
            static LONG lastPresents = 0; static ULONGLONG lastAt = 0;
            const LONG presents = InterlockedCompareExchange(&g_presentCount, 0, 0);
            const double fps = lastAt ? (presents - lastPresents) * 1000.0 / (double)(now - lastAt) : 0.0;
            lastPresents = presents; lastAt = now;
            Log("[dedicated] no-render: %ld submits, %ld command buffers dropped so far; %.1f frames/s (paced to %.0f)\n",
                InterlockedCompareExchange(&g_noRenderSubmits, 0, 0), InterlockedCompareExchange(&g_noRenderCmdBufs, 0, 0), fps,
                NoWsi() ? NORENDER_FPS : (double)g_flagDedFps);
        }
    }
    // THE AUTOSAVE PAUSES THE SESSION FIRST (2026-09-21). A forced autosave stops
    // this game for the save's duration (11 s for a 104 MB world on the VPS) while
    // the players' games run on, then wait for the leader: a player at 4x took it
    // for a broken server and quit. So: ask the mod to hold the session
    // (tpf2_ded_autosave.txt -> mp/pacing.lua CM.dedAutosaveTick: 0 here and on
    // every game, or "alone" with nobody in), save once the ack is back, tell the
    // mod when the save file has stopped growing (the done marker), and say so in
    // the chat both times. No ack within 15 s (a resync in progress holds the
    // mod's tick): not now, again in a minute. The markers are EMPTIED, never
    // deleted: the game's Lua reads an empty file as no marker and has no remove.
    static int g_dedSaveState = 0;   // 0 idle, 1 asked, 2 saving
    static ULONGLONG g_dedSaveAt = 0, g_dedSaveBaseline = 0, g_dedSaveLastSize = 0;
    static wchar_t g_dedSaveFile[600] = L"";
    wchar_t req[MAX_PATH], ack[MAX_PATH], done[MAX_PATH];
    _snwprintf_s(req,  _TRUNCATE, L"%stpf2_ded_autosave.txt", g_dataDirW);
    _snwprintf_s(ack,  _TRUNCATE, L"%stpf2_ded_autosave_ack.txt", g_dataDirW);
    _snwprintf_s(done, _TRUNCATE, L"%stpf2_ded_autosave_done.txt", g_dataDirW);
    auto putMarker = [](const wchar_t* path, const char* text) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0; if (text && *text) WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
        CloseHandle(h); return true;
    };
    auto markerSet = [](const wchar_t* path) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        return GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
    };
    if (g_flagDedAutosaveMin > 0 && g_dedSaveState == 0 && now - g_dedWorldUpSince > 60000 && !NativeIo::Busy()
        && now - g_dedLastSave > (ULONGLONG)g_flagDedAutosaveMin * 60000ULL) {
        g_dedLastSave = now;
        putMarker(ack, ""); putMarker(done, "");
        if (putMarker(req, "hold\n")) {
            g_dedSaveState = 1; g_dedSaveAt = now;
            LobbySend("{\"cmd\":\"chat\",\"text\":\"Server autosave in a moment: the game pauses for about ten seconds.\"}");
            Log("[dedicated] autosave due (every %d min): asking the mod to hold the session\n", g_flagDedAutosaveMin);
        } else {
            if (ForceAutosave()) Log("[dedicated] autosave forced (every %d min), no hold: the request file could not be written\n", g_flagDedAutosaveMin);
        }
    } else if (g_dedSaveState == 1) {
        if (markerSet(ack)) {
            wchar_t cur[600] = L""; ULONGLONG sz = 0;
            g_dedSaveBaseline = newestSave(cur, 600) ? saveMtime(cur, &sz) : 0;
            g_dedSaveFile[0] = 0; g_dedSaveLastSize = 0;
            putMarker(ack, "");
            g_dedSaveState = 2; g_dedSaveAt = now;
            if (ForceAutosave()) Log("[dedicated] autosave forced (every %d min), the session held\n", g_flagDedAutosaveMin);
            else { putMarker(done, "failed\n"); g_dedSaveState = 0; }
        } else if (now - g_dedSaveAt > 15000) {
            putMarker(req, "");
            g_dedSaveState = 0;
            g_dedLastSave = now - (ULONGLONG)g_flagDedAutosaveMin * 60000ULL + 60000ULL;   // again in a minute
            Log("[dedicated] autosave: no hold from the mod within 15 s (a world operation?) -- not now, again in a minute\n");
        }
    } else if (g_dedSaveState == 2) {
        // done when a newer save file has stopped growing (SyncPoll's rule), or after 90 s
        bool finished = false;
        wchar_t cur[600] = L""; ULONGLONG sz = 0;
        if (newestSave(cur, 600)) {
            ULONGLONG mt = saveMtime(cur, &sz);
            if (mt > g_dedSaveBaseline && sz > 0) {
                if (wcscmp(cur, g_dedSaveFile) == 0 && sz == g_dedSaveLastSize) finished = true;
                wcscpy_s(g_dedSaveFile, cur); g_dedSaveLastSize = sz;
            }
        }
        if (finished || now - g_dedSaveAt > 90000) {
            putMarker(done, finished ? "saved\n" : "timeout\n");
            g_dedSaveState = 0;
            LobbySend("{\"cmd\":\"chat\",\"text\":\"Server autosave done, resuming.\"}");
            Log("[dedicated] autosave %s (%llu B) -- releasing the session\n", finished ? "written" : "not seen within 90 s", (unsigned long long)g_dedSaveLastSize);
        }
    }
}

// in-frame chat text input: poll key edges while in the lobby
static char vkToChar(int vk, bool shift)
{
    if (vk >= 'A' && vk <= 'Z') return shift ? (char)vk : (char)(vk + 32);
    if (vk >= '0' && vk <= '9') { const char* sh = ")!@#$%^&*("; return shift ? sh[vk - '0'] : (char)vk; }
    if (vk == VK_SPACE) return ' ';
    switch (vk) {
        case VK_OEM_MINUS: return shift ? '_' : '-';
        case VK_OEM_PLUS:  return shift ? '+' : '=';
        case VK_OEM_1:     return shift ? ':' : ';';
        case VK_OEM_2:     return shift ? '?' : '/';
        case VK_OEM_PERIOD:return shift ? '>' : '.';
        case VK_OEM_COMMA: return shift ? '<' : ',';
        case VK_OEM_7:     return shift ? '"' : '\'';
    }
    return 0;
}
static HHOOK g_kbHook = nullptr;
static HHOOK g_mouseHook = nullptr;
static LRESULT CALLBACK LlMouse(int code,WPARAM wp,LPARAM lp)
{
    static bool captured=false, capturedR=false;
    if(code==HC_ACTION && wp==WM_LBUTTONUP && captured) { captured=false; return 1; }
    if(code==HC_ACTION && wp==WM_RBUTTONUP && capturedR) { capturedR=false; return 1; }
    // g_ingameOverlay belongs here just as much as g_showOverlay: the title-menu
    // detour sets g_showOverlay (page 2 only), while the panel opened from inside a
    // loaded game sets g_ingameOverlay (PollLobbyOpen). Without the second flag this
    // hook captured nothing in-game, and PollClick's GetAsyncKeyState fallback is
    // disabled whenever this hook installed (see g_mouseInstalled), so NO in-game
    // panel button worked -- including the "x" that closes it, which is the only way
    // out in game (LEAVE is drawn only when no world is loaded). The present gate and
    // the keyboard hook already test both flags; this one was the odd man out.
    // A right click on the panel is a panel click too (company chips cycle
    // backwards) and is swallowed like a left one: behind the panel it would
    // cancel or rotate a construction tool.
    if(code==HC_ACTION && (wp==WM_LBUTTONDOWN || wp==WM_RBUTTONDOWN) && gameHasFocus() && g_uiState!=0
       && (InterlockedCompareExchange(&g_showOverlay, 0, 0) != 0
           || InterlockedCompareExchange(&g_ingameOverlay, 0, 0) != 0
           || g_recoveryPresent)
       && !g_recoveryWorldIo && !NativeIo::Busy()) {
        const auto data=reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        POINT origin{}; if(g_gameWnd) ClientToScreen(g_gameWnd,&origin);
        const int x=data->pt.x-origin.x-g_panelX, y=data->pt.y-origin.y-g_panelY;
        if((x>=0 && y>=0 && x<g_copyW && y<g_copyH) ||
           (!WorldLoaded() && g_showOverlay && (g_uiState==1 || g_uiState==2) &&
            x+g_panelX>=0 && y+g_panelY>=0 && x+g_panelX<(int)g_scExtent.width && y+g_panelY<(int)g_scExtent.height)) {
            InterlockedExchange64(&g_panelClickPoint,(LONG64)((unsigned long long)(DWORD)data->pt.y<<32 | (DWORD)data->pt.x));
            InterlockedExchange(&g_panelClickButton, wp==WM_RBUTTONDOWN ? 2 : 1);
            InterlockedExchange(&g_pendingPanelClick,1);
            if (wp==WM_RBUTTONDOWN) capturedR=true; else captured=true;
            return 1; // panel clicks never reach a construction tool behind it
        }
    }
    return CallNextHookEx(g_mouseHook,code,wp,lp);
}

// Ctrl+Shift+D toggles the in-game Multiplayer dashboard. The dashboard lives in
// the game's GUI Lua state, which has no key input of its own, so the toggle is
// a one-byte file it polls: 1 = shown, 0 = hidden. The chord is not bound by the
// game, and it is swallowed here so the game never sees it either.
static volatile LONG g_dashShown = 1;
static void WriteDashFlag()
{
    wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%stpf2mp_dash.txt", g_dataDirW);
    FILE* f = _wfsopen(p, L"w", _SH_DENYNO);
    if (!f) return;
    fputs(InterlockedCompareExchange(&g_dashShown, 0, 0) ? "1" : "0", f);
    fclose(f);
}

static LRESULT CALLBACK LlKeyboard(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && gameHasFocus()) {
        KBDLLHOOKSTRUCT* k0 = (KBDLLHOOKSTRUCT*)lp;
        if (k0->vkCode == 'D' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            // The GUI Hide button also writes this flag. Read its current state
            // so the first shortcut after a mouse hide always shows the panel.
            wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%stpf2mp_dash.txt", g_dataDirW);
            FILE* flag = _wfsopen(path, L"r", _SH_DENYNO);
            if (flag) {
                int value = fgetc(flag); fclose(flag);
                if (value == '0' || value == '1') InterlockedExchange(&g_dashShown, value == '1');
            }
            LONG now = InterlockedCompareExchange(&g_dashShown, 0, 0) ? 0 : 1;
            InterlockedExchange(&g_dashShown, now);
            WriteDashFlag();
            Log("[menu] dashboard %s (Ctrl+Shift+D)\n", now ? "shown" : "hidden");
            return 1;
        }
    }
    // Title-menu field navigation. Consume the matching key-up too, even if
    // Escape just closed the panel. Alt+Tab / OS chords remain untouched.
    static bool titleTabHeld=false, titleEscapeHeld=false;
    if(code==HC_ACTION) {
        const DWORD vk=((KBDLLHOOKSTRUCT*)lp)->vkCode;
        bool* held=vk==VK_TAB?&titleTabHeld:vk==VK_ESCAPE?&titleEscapeHeld:nullptr;
        if(held && (wp==WM_KEYUP || wp==WM_SYSKEYUP) && *held) { *held=false; return 1; }
        if(held && (wp==WM_KEYDOWN || wp==WM_SYSKEYDOWN) && gameHasFocus()
           && !WorldLoaded() && g_showOverlay && (g_uiState==1 || g_uiState==2)
           && !(GetAsyncKeyState(VK_MENU)&0x8000) && !(GetAsyncKeyState(VK_CONTROL)&0x8000)
           && !(GetAsyncKeyState(VK_LWIN)&0x8000) && !(GetAsyncKeyState(VK_RWIN)&0x8000)) {
            if(*held) return 1;
            *held=true;
            if(vk==VK_ESCAPE) {
                bool modal=false;
                if(g_csInit) { EnterCriticalSection(&g_statusCs); modal=g_modsPrompt[0]!=0; LeaveCriticalSection(&g_statusCs); }
                OnHit(modal?17:g_savePicker?91:4);
            } else if(g_uiState==1) {
                InterlockedExchange(&g_joinFocus,TitleNextFocus(g_joinFocus,(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0));
                InterlockedExchange(&g_panelDirty,1);
            }
            return 1;
        }
    }
    LONG st = InterlockedCompareExchange(&g_uiState, 0, 0);
    LONG focus = InterlockedCompareExchange(&g_joinFocus, 0, 0);
    bool codeField = st == 1 && focus == 1;
    bool passField = st == 1 && focus == 2;
    bool nameField = st == 1 && (focus == 3 || focus == 4);
    bool chatField = (st == 2 && InterlockedCompareExchange(&g_lobbyDone, 0, 0) == 0)
        || ((st == 3 || (st == 2 && g_recoveryPresent)) && !g_recoveryWorldIo && !NativeIo::Busy());
    if (code == HC_ACTION && (codeField || passField || nameField || chatField) &&
        (InterlockedCompareExchange(&g_showOverlay, 0, 0) != 0 || InterlockedCompareExchange(&g_ingameOverlay, 0, 0) != 0 || (st == 3 && g_recoveryPresent)) &&
        gameHasFocus())
    {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode;
        // Let modifiers, Esc and Tab pass so the user can shift-type, Alt+Tab out,
        // and is never trapped in the overlay.
        if (vk == VK_ESCAPE || vk == VK_TAB ||
            vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LWIN || vk == VK_RWIN)
            return CallNextHookEx(g_kbHook, code, wp, lp);
        if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && nameField) {
            // player name: one word (it is a bare --name argument); lobby name: words, digits, ' - _ .
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            char* buf = focus == 3 ? g_username : g_lobbyName; int* len = focus == 3 ? &g_userLen : &g_lobbyNameLen; int cap = NAME_TYPED_MAX;
            if (vk == VK_BACK) { if (*len > 0) { buf[--*len] = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else if (vk == VK_RETURN) {
                if (focus == 3) {
                    if (!g_username[0]) {   // cleared: back to the Steam name (or the random default until Steam answers)
                        g_userAuto = true;
                        if (g_steamName[0]) { strcpy_s(g_username, g_steamName); g_userLen = (int)strlen(g_username); } else ensureUsername();
                        Log("[menu] username cleared -> follows Steam (%s)\n", g_username);
                    } else g_userAuto = strcmp(g_username, g_steamName) == 0    // typed back exactly the Steam name: still follows it
                                        || IsGeneratedName(g_username);          // Enter on an untouched random default: not a typed name either
                }
                InterlockedExchange(&g_joinFocus, 0); SaveNames(); InterlockedExchange(&g_panelDirty, 1); }
            else { char c = vkToChar((int)vk, shift);
                   bool word = c && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.');
                   bool okc = word || ((focus == 4 || focus == 3) && c == ' ' && *len > 0) || (focus == 4 && c == '\'' && *len > 0);
                   if (okc && *len < cap) { buf[(*len)++] = c; buf[*len] = 0; InterlockedExchange(&g_panelDirty, 1); } }
        }
        else if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && passField) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (vk == VK_BACK) { if (g_passLen > 0) { g_passCode[--g_passLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else if (vk == VK_RETURN) { InterlockedExchange(&g_joinFocus, 0); InterlockedExchange(&g_panelDirty, 1); }
            else { char c = vkToChar((int)vk, shift);
                   // the password becomes a command-line argument: letters, digits, - _ . only
                   bool okc = c && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.');
                   if (okc && g_passLen < 32) { g_passCode[g_passLen++] = c; g_passCode[g_passLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
        }
        else if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && codeField) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (vk == VK_BACK) { if (g_joinLen > 0) { g_joinCode[--g_joinLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else if (vk == VK_RETURN) { InterlockedExchange(&g_joinFocus, 0); OnHit(3); }
            else if (ctrl && vk == 'V') { char buf[128]; if (ClipboardGet(buf, sizeof(buf))) { int j = g_joinLen; for (int i = 0; buf[i] && j < 200; i++) if ((unsigned char)buf[i] > 32) g_joinCode[j++] = buf[i]; g_joinCode[j] = 0; g_joinLen = j; InterlockedExchange(&g_panelDirty, 1); } }
            else { char c = vkToChar((int)vk, shift); if (c && c > 32 && g_joinLen < 200) { g_joinCode[g_joinLen++] = c; g_joinCode[g_joinLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
        }
        else if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (vk == VK_BACK) { if (g_chatLen > 0) { g_chatInput[--g_chatLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else if (vk == VK_RETURN) { if (g_chatLen > 0) { SendChat(g_chatInput); g_chatInput[0] = 0; g_chatLen = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else { char c = vkToChar((int)vk, shift); if (c && g_chatLen < 190) { g_chatInput[g_chatLen++] = c; g_chatInput[g_chatLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
        }
        return 1;   // swallow down AND up so no WM_CHAR / keyup binding leaks to the game
    }
    // The title sheet covers the original menu. Do not let its navigation keys
    // activate an invisible Continue/Load button when no text field has focus.
    if(code==HC_ACTION && gameHasFocus() && !WorldLoaded() && g_showOverlay &&
       (g_uiState==1 || g_uiState==2) && !(GetAsyncKeyState(VK_MENU)&0x8000) &&
       !(GetAsyncKeyState(VK_CONTROL)&0x8000) && !(GetAsyncKeyState(VK_LWIN)&0x8000) &&
       !(GetAsyncKeyState(VK_RWIN)&0x8000)) {
        DWORD key=((KBDLLHOOKSTRUCT*)lp)->vkCode;
        if(key==VK_RETURN || key==VK_SPACE || key==VK_UP || key==VK_DOWN ||
           key==VK_LEFT || key==VK_RIGHT || key==VK_HOME || key==VK_END) return 1;
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}
static DWORD WINAPI KbHookThread(LPVOID)
{
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LlKeyboard, GetModuleHandleW(nullptr), 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LlMouse, GetModuleHandleW(nullptr), 0);
    InterlockedExchange(&g_mouseInstalled, g_mouseHook != nullptr);
    Log("[menu] LL keyboard hook %s\n", g_kbHook ? "installed" : "FAILED");
    MSG msg;   // the LL hook needs a message pump on its installing thread
    while (GetMessage(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}

// The detour: run the original so the page still builds, then flag the in-frame
// button visible on the main page (page 2), hidden elsewhere.
static void MyCreatePage(uint64_t thisp, int page)
{
    g_menuUiPtr = thisp;
    NativeIo::ObserveMenu(thisp);
    g_origCreatePage(thisp, page);
    InterlockedExchange(&g_lastPage, page);
    // The main menu builds pages 0 -> 2 -> 1 (2 is the main content, 0/1 are its
    // sub-layers). Full-screen replacements (Settings/Campaign/Load...) are all
    // page >= 3. So SET on 2, CLEAR only on >= 3; leave 0/1 alone -- otherwise
    // the trailing page=1 hid the overlay on the idle main menu.
    if (page == 2) {
        InterlockedExchange(&g_showOverlay, 1);
        // The title menu exists only when no game runs: forget the CGameUI pointer
        // captured in the last session. It used to survive "quit to menu", so a
        // start arriving while the title menu sat on another page looked like
        // "start while in game" and was ignored (relay resume, 2026-09-10).
        // A pointer still set here means the player just left a world: with a
        // lobby running, that leaves the lobby too (g_leaveOnMenu, myPresent).
        // Not when the load is ours: a resync loads the host's snapshot through
        // the engine's own load path, which builds this page between the old
        // world and the loading screen. Reading that as "left the world" made
        // every joiner LEAVE mid-resync, and the host's barrier failed with
        // "Player disconnected or roster changed" (2026-09-16).
        if (g_gameUi != 0 && LobbyRunning()) {
            if (NativeIo::Loading()) Log("[menu] the title menu was built by our own load (resync) -- staying in the lobby\n");
            else if (InterlockedExchange(&g_hostMenuLoad, 0)) Log("[menu] the title menu was built by the host's own world switch -- staying in the lobby\n");
            else InterlockedExchange(&g_leaveOnMenu, 1);
        }
        g_gameUi = 0;
        InterlockedExchange(&g_ingameOverlay, 0);
        InterlockedExchange(&g_loadingPanel, 0);
        // Keep recovery reachable at the title menu after a failed load.
        if (InterlockedCompareExchange(&g_recoveryPresent, 0, 0) && g_modelCsInit) {
            EnterCriticalSection(&g_modelCs);
            const bool open = g_recoveryPhase[0] != 0 && strcmp(g_recoveryPhase, "complete") != 0;
            LeaveCriticalSection(&g_modelCs);
            if (open) { InterlockedExchange(&g_uiState, 3); InterlockedExchange(&g_panelDirty, 1); }
        }
    }
    else if (page == 16 && LobbyRunning()) {
        InterlockedExchange(&g_showOverlay, 1);
        InterlockedExchange(&g_loadingPanel, 1);
        InterlockedExchange(&g_loadingStagePending, 1);
        if (InterlockedCompareExchange(&g_uiState, 0, 0) != 2) InterlockedExchange(&g_uiState, 2);
        InterlockedExchange(&g_panelDirty, 1);
        Log("[menu] loading screen with a lobby running -- the panel stays up with the roster\n");
    }
    else if (page >= 3) { InterlockedExchange(&g_showOverlay, 0); InterlockedExchange(&g_loadingPanel, 0); }
    static int seen = 0;
    if (seen < 30) { seen++; Log("[menu] CreatePage page=%d show=%ld\n", page,
        InterlockedCompareExchange(&g_showOverlay, 0, 0)); }
}

// ---------------- auto-enable the lockstep mod ----------------
// Transport Fever 2 activates mods per game. A fresh install had "MP Lockstep"
// sitting in <gamedir>\mods, visible in the Mods panel and OFF, so a new game
// hosted from a new install ran without lockstep at all (reported 2026-09-09).
// settings.lua (<userdata>\<id>\1066780\local\) holds `activeMods`, the list a
// NEW game starts with (a savegame carries its own list, which is how a joiner
// inherits the host's). The game reads the file once at startup and rewrites it
// from memory at exit, so the edit has to land BEFORE the exe's entry point --
// that is why this runs from DllMain (the proxy loads us before main; the work
// is one small file read/write through kernel32, nothing that touches the
// loader lock). Idempotent: nothing is written when the entry is already there.
// Kill switch: `automod=0` in tpf2_menu_flags.txt.
static bool FlagsSayNoAutoMod()
{
    char p[MAX_PATH]; snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", ourDirA());
    FILE* f = fopen(p, "r"); if (!f) return false;
    char line[256]; bool off = false;
    while (fgets(line, sizeof(line), f)) if (!strncmp(line, "automod=0", 9)) off = true;
    fclose(f);
    return off;
}

static void AutoEnableLockstepMod(const wchar_t* saveDir)
{
    if (FlagsSayNoAutoMod()) { Log("[menu] automod: disabled by flags\n"); return; }
    // <...>\1066780\local\save -> <...>\1066780\local\settings.lua
    wchar_t local[600]; wcscpy_s(local, saveDir);
    wchar_t* tail = wcsrchr(local, L'\\');
    if (!tail || _wcsicmp(tail, L"\\save") != 0) { Log("[menu] automod: unexpected save dir %ls\n", saveDir); return; }
    *tail = 0;
    wchar_t path[600], tmp[600], bak[600];
    _snwprintf_s(path, _TRUNCATE, L"%s\\settings.lua", local);
    _snwprintf_s(tmp,  _TRUNCATE, L"%s\\settings.lua.mptmp", local);
    _snwprintf_s(bak,  _TRUNCATE, L"%s\\settings.lua.mpbak", local);

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // first launch ever: the game writes settings.lua at exit; next launch we patch it
        Log("[menu] automod: no settings.lua yet (%ls) -- will add the Transport Fever 2 Multiplayer mod on the next launch\n", path);
        return;
    }
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (4 << 20)) { CloseHandle(h); Log("[menu] automod: settings.lua size %lld, left alone\n", (long long)sz.QuadPart); return; }
    std::string txt; txt.resize((size_t)sz.QuadPart);
    DWORD got = 0; BOOL okR = ReadFile(h, &txt[0], (DWORD)txt.size(), &got, nullptr);
    CloseHandle(h);
    if (!okR || got != txt.size()) { Log("[menu] automod: read failed\n"); return; }

    if (txt.find("\"mp_lockstep\"") != std::string::npos) { Log("[menu] automod: the Transport Fever 2 Multiplayer mod already in activeMods\n"); return; }
    const bool crlf = txt.find("\r\n") != std::string::npos;
    const std::string nl = crlf ? "\r\n" : "\n";
    std::string out;
    size_t at = txt.find("activeMods = {");
    if (at != std::string::npos) {
        size_t brace = txt.find('{', at);
        out = txt.substr(0, brace + 1) + nl + "\t\t{ \"mp_lockstep\", 1, }," + txt.substr(brace + 1);
    } else {
        size_t ret = txt.find("return {");
        if (ret == std::string::npos) { Log("[menu] automod: settings.lua has neither activeMods nor 'return {' -- left alone\n"); return; }
        size_t brace = ret + 7;
        out = txt.substr(0, brace + 1) + nl + "\tactiveMods = {" + nl + "\t\t{ \"mp_lockstep\", 1, }," + nl + "\t}," + txt.substr(brace + 1);
    }
    if (GetFileAttributesW(bak) == INVALID_FILE_ATTRIBUTES) CopyFileW(path, bak, TRUE);
    HANDLE w = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) { Log("[menu] automod: cannot write %ls (err %lu)\n", tmp, GetLastError()); return; }
    DWORD put = 0; BOOL okW = WriteFile(w, out.data(), (DWORD)out.size(), &put, nullptr);
    CloseHandle(w);
    if (!okW || put != out.size() || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log("[menu] automod: replace failed (err %lu) -- settings.lua untouched\n", GetLastError());
        DeleteFileW(tmp);
        return;
    }
    Log("[menu] automod: the Transport Fever 2 Multiplayer mod added to activeMods in %ls (%s; backup settings.lua.mpbak)\n",
        path, at != std::string::npos ? "existing list" : "new list");
}

static DWORD WINAPI Init(LPVOID)
{
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    resolveSaveDir(g_saveDirW, 600); SAVE_DIR = g_saveDirW;   // discovered, not hardcoded
    resolveNetDir(g_netDirW, 600);   NETDIR   = g_netDirW;
    // the runtime data dir the bridge uses for tpf2_instance.txt / tpf2_bridge_ctl.txt
    if (!Tpf2mpDataDirW(g_dataDirW, MAX_PATH, (const void*)&Init)) wcscpy_s(g_dataDirW, ourDirW());
    NativeControl::Start(g_dataDirW, NativeIo::Initialize(g_base, g_nativeModule, g_saveDirW));
    // Every UI load reaches us through the StartSavegame detour native_io.cpp
    // already owns (one hook, one steal): a host that loads another world
    // shares it, mid-session as a world switch (see OnStartSavegame).
    NativeIo::ObserveStart(&OnStartSavegame);
    InitializeCriticalSection(&g_statusCs); g_csInit = true;
    InitializeCriticalSection(&g_modelCs); g_modelCsInit = true;
    InitializeCriticalSection(&g_lobbyCs); g_lobbyCsInit = true;
    InitializeCriticalSection(&g_syncCs); g_syncCsInit = true;
    CreateThread(nullptr, 0, KbHookThread, nullptr, 0, nullptr);  // chat keyboard capture/swallow
    Log("[menu] attached, base=%llx  save=%ls  net=%ls  data=%ls  our=%ls\n",
        (unsigned long long)g_base, g_saveDirW, g_netDirW, g_dataDirW, ourDirW());

    void* tramp = nullptr;
    if (!InstallHook(g_base + RVA_CREATEPAGE, (void*)&MyCreatePage,
                     STEAL_CREATEPAGE, &tramp)) {
        Log("[menu] InstallHook FAILED on CreatePage\n");
        return 0;
    }
    // Hot join needs CGameUI's 'this' (see ForceAutosave). Capture-only detour
    // on its per-frame update; refused, not guessed, if the prologue moved.
    if (memcmp((void*)(g_base + RVA_GAMEUI_UPDATE), GAMEUI_EXPECTED, STEAL_GAMEUI) != 0)
        Log("[menu] CGameUI update prologue differs from build 35924 -- hot join's forced autosave unavailable\n");
    else if (InstallHook(g_base + RVA_GAMEUI_UPDATE, (void*)&GameUiRelay, STEAL_GAMEUI, &g_gameUiTramp))
        Log("[menu] hooked CGameUI update at %llx (capture 'this' for the sync autosave)\n", (unsigned long long)RVA_GAMEUI_UPDATE);
    else
        Log("[menu] InstallHook FAILED on CGameUI update -- hot join unavailable\n");
    // Option 2 groundwork: locate Vulkan present and dump its prologue so we can
    // choose a safe steal for an in-frame overlay (external windows cannot draw
    // over this game's borderless direct-flip present).
    {
        HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
        if (!vk) vk = LoadLibraryW(L"vulkan-1.dll");
        if (vk) {
            void* pres = (void*)GetProcAddress(vk, "vkQueuePresentKHR");
            void* gdpa = (void*)GetProcAddress(vk, "vkGetDeviceProcAddr");
            if (pres) {
                Log("[menu] vulkan-1.dll=%p vkQueuePresentKHR=%p gdpa=%p\n", vk, pres, gdpa);
                // Follow the E9 rel32 export thunks to the real dispatch fns and
                // dump THOSE prologues (that is where an inline hook must land).
                auto dumpFollow = [&](const char* nm, void* thunk) {
                    if (!thunk) return;
                    unsigned char* b = (unsigned char*)thunk;
                    void* real = thunk;
                    if (b[0] == 0xE9) {
                        int32_t rel = *(int32_t*)(b + 1);
                        real = (void*)((uintptr_t)thunk + 5 + rel);
                    }
                    unsigned char* rb = (unsigned char*)real;
                    char hex[96]; int o = 0;
                    for (int i = 0; i < 28 && o < 92; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02x", rb[i]);
                    Log("[menu] %s thunk=%p -> real=%p prologue=%s\n", nm, thunk, real, hex);
                };
                dumpFollow("vkQueuePresentKHR", pres);
                dumpFollow("vkGetDeviceProcAddr", gdpa);
                // Hook the REAL vkGetDeviceProcAddr (clean 15-byte prologue) so
                // we intercept the game's present resolution.
                if (gdpa) {
                    unsigned char* gb = (unsigned char*)gdpa;
                    void* gdpaReal = gdpa;
                    if (gb[0] == 0xE9) gdpaReal = (void*)((uintptr_t)gdpa + 5 + *(int32_t*)(gb + 1));
                    void* gt = nullptr;
                    // The loader ships with the graphics driver, so its prologue
                    // is whatever that build's compiler made: 15 bytes on ours,
                    // 16 on a friend's (mov [rsp+10],rsi; push rdi; sub rsp,20;
                    // mov rsi,rcx; mov rdi,rdx). A fixed 15 cut the last mov in
                    // half and the game died at device creation, before its
                    // first frame, with nothing in stdout.txt (2026-09-10).
                    int steal = PrologueSteal((const unsigned char*)gdpaReal, 14);
                    if (steal <= 0) {
                        Log("[menu] vkGetDeviceProcAddr prologue not decodable -- overlay NOT hooked (the game keeps running without the in-game panel)\n");
                    } else if (InstallHook((uintptr_t)gdpaReal, (void*)&myGdpa, steal, &gt)) {
                        g_origGdpa = (PFN_vkGetDeviceProcAddr)gt;
                        Log("[menu] hooked vkGetDeviceProcAddr real=%p steal=%d\n", gdpaReal, steal);
                    } else {
                        Log("[menu] InstallHook on gdpa FAILED\n");
                    }
                }
            } else {
                Log("[menu] vkQueuePresentKHR not exported by vulkan-1.dll\n");
            }
        } else {
            Log("[menu] vulkan-1.dll not loaded -- game may use a different Vulkan path\n");
        }
    }

    InstallGlPresentHook();   // the same panel and per-frame work on the OpenGL renderer

    g_origCreatePage = (CreatePageFn)tramp;
    Log("[menu] hooked CreatePage rva=%llx steal=%d tramp=%p -- overlay thread started\n",
        (unsigned long long)RVA_CREATEPAGE, STEAL_CREATEPAGE, tramp);

    // ---- native-look overlay + real list-inserted button ----
    ReadFlags();
    ModDownloadPreference(false);
    ensureUsername(); LoadNames();
    LoadLato();
    g_strAssign = (StrAssignFn)(g_base + RVA_STR_ASSIGN);
    g_actionCtx = (ActionCtxFn)(g_base + RVA_ACTION_CTX);
    g_btn       = (BtnFn)(g_base + RVA_BTN);
    g_add       = (AddFn)(g_base + RVA_ADD);
    g_clean     = (CleanFn)(g_base + RVA_CLEAN);
    g_setName   = (SetNameFn)(g_base + RVA_SETNAME);
    g_prep      = (PrepFn)(g_base + RVA_PREP);
    // auto-load: CMenuUI's per-frame update runs the pending-load check on the main thread
    {
        void** slot = (void**)(g_base + RVA_MENUUI_VFTABLE + MENUUI_SLOT_UPDATE * sizeof(void*));
        if ((uintptr_t)*slot != g_base + RVA_MENUUI_UPDATE) {
            Log("[menu] autoload: CMenuUI update slot holds %p, expected %llx -- not hooked; the player loads the shared save\n",
                *slot, (unsigned long long)(g_base + RVA_MENUUI_UPDATE));
        } else {
            DWORD old;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                g_origMenuUpdate = (MenuUpdateFn)*slot;   // set before the slot, so the detour never calls null
                *slot = (void*)&MyMenuUpdate;
                VirtualProtect(slot, sizeof(void*), old, &old);
                Log("[menu] autoload: hooked CMenuUI update (vtable slot %d)%s\n", MENUUI_SLOT_UPDATE,
                    g_flagAutoLoad ? "" : " -- autoload=0: the player loads the shared save");
            } else {
                Log("[menu] autoload: VirtualProtect on the CMenuUI vtable failed (%lu)\n", GetLastError());
            }
        }
    }
    {
        // verify both prologues before patching: a game update moves everything.
        static const unsigned char kMainBuild[14] = { 0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
        static const unsigned char kListAdd[15]   = { 0x40,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff };
        bool okA = memcmp((void*)(g_base + RVA_MAINBUILD), kMainBuild, 14) == 0;
        bool okB = memcmp((void*)(g_base + RVA_LIST_ADD),  kListAdd, 15) == 0;
        if (!okA || !okB) {
            Log("[menu] native: prologue mismatch (mainbuild=%d listadd=%d) -- native button disabled\n", okA, okB);
        } else {
            // ORDER MATTERS. The moment InstallHook returns, the game's list-add
            // jumps into MyListAdd -- on the UI thread, while this runs on ours.
            // The trampoline pointer must therefore be assigned before the next
            // statement, not after a second hook has also succeeded; and if that
            // second hook fails, the first must not be left live calling null
            // (review, 2026-09-01). The builder hook goes first: it is harmless
            // on its own, the list-add hook is not.
            void* tA = nullptr; void* tB = nullptr;
            bool okHook = InstallHook(g_base + RVA_MAINBUILD, (void*)&MyMainBuild, STEAL_MAINBUILD, &tA);
            if (okHook) g_origMainBuild = (MainBuildFn)tA;
            if (okHook) {
                okHook = InstallHook(g_base + RVA_LIST_ADD, (void*)&MyListAdd, STEAL_LIST_ADD, &tB);
                if (okHook) g_origListAdd = (ListAddFn)tB;
            }
            if (okHook) {
                Log("[menu] native: hooked list-add %llx (steal %d) and main builder %llx (steal %d)\n",
                    (unsigned long long)RVA_LIST_ADD, STEAL_LIST_ADD, (unsigned long long)RVA_MAINBUILD, STEAL_MAINBUILD);
            } else {
                Log("[menu] native: InstallHook FAILED -- native button disabled\n");
            }
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_nativeModule = h;
        DisableThreadLibraryCalls(h);
        // Before the game's own startup reads settings.lua (see AutoEnableLockstepMod).
        __try {
            wchar_t sd[600]; resolveSaveDir(sd, 600);
            AutoEnableLockstepMod(sd);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[menu] automod: exception, settings.lua left alone\n");
        }
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        NativeControl::SignalShutdown();
        // Orderly shutdown: ask the lobby to quit. Nothing may block here (the
        // loader lock is held), so no waiting and no thread joins -- the job
        // object above is what guarantees the kill if this never runs.
        if (g_lobbyProc) { LobbySend("{\"cmd\":\"quit\"}"); }
    }
    return TRUE;
}
