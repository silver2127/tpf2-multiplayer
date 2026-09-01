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
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define VK_NO_PROTOTYPES
#include "vk/vulkan_core.h"
#include "hook.h"
#include "datadir.h"

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

// --- insertion primitives (correct RVAs, see docs/re/MENU_UI.md) ---
static const uintptr_t RVA_MAINBUILD = 0x667bc0;   // main-page builder (hook here)
static const int       STEAL_MAINBUILD = 14;       // mov rax,rsp + 7 pushes
static const uintptr_t RVA_SEED  = 0x4c0f40;       // 4c0f40(&out, param_3)
static const uintptr_t RVA_FIN   = 0x63e6e0;       // 63e6e0(&binding,&{p1}) -> writes +0x38
static const uintptr_t RVA_ADD   = 0x22518f0;      // 2518f0(button,&{container},&binding)
static const uintptr_t RVA_CLEAN = 0x2357910;      // 357910(&{container})
typedef void  (*SeedFn)(void* out, void* p3);
typedef void* (*FinFn)(void* binding, void* pP1);
typedef void* (*AddFn)(void* button, void* pContainer, void* binding);
typedef void  (*CleanFn)(void* pContainer);
static SeedFn  g_seed  = nullptr;
static FinFn   g_fin   = nullptr;
static AddFn   g_add   = nullptr;
static CleanFn g_clean = nullptr;

typedef void (*MainBuildFn)(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4);
static MainBuildFn g_origMainBuild = nullptr;
static void* g_myButton = nullptr;    // set after a successful build, for later vtable click override

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

// <steam>\userdata\<accountid>\1066780\local\save -- enumerate accounts, pick the
// one that actually has a 1066780 save dir (newest wins). 1066780 = TF2 appid.
static void resolveSaveDir(wchar_t* out, int cch)
{
    wchar_t steam[MAX_PATH];
    if (steamPath(steam, MAX_PATH)) {
        wchar_t pat[MAX_PATH]; _snwprintf_s(pat, _TRUNCATE, L"%s\\userdata\\*", steam);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        wchar_t best[MAX_PATH] = L""; ULONGLONG bestT = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                wchar_t cand[MAX_PATH]; _snwprintf_s(cand, _TRUNCATE, L"%s\\userdata\\%s\\1066780\\local\\save", steam, fd.cFileName);
                DWORD a = GetFileAttributesW(cand);
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                    ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                    if (t >= bestT) { bestT = t; wcscpy_s(best, cand); }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (best[0]) { wcscpy_s(out, cch, best); return; }
    }
    // no Steam install found: a generic path that CopyFileW will simply fail on (logged)
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

// netpunch working dir, in order of preference:
//   %LOCALAPPDATA%\tpf2mp\netpunch   portable, writable everywhere, sandbox-safe
//   <dir of this dll>\netpunch        where the installer drops netpunch.exe
//   %USERPROFILE%\tpf2-multiplayer\netpunch   dev checkout fallback
// A candidate counts if it holds either lobby.py or the frozen netpunch.exe.
static void resolveNetDir(wchar_t* out, int cch)
{
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        wchar_t cand[MAX_PATH]; _snwprintf_s(cand, _TRUNCATE, L"%s\\tpf2mp\\netpunch", la);
        if (netDirUsable(cand)) { wcscpy_s(out, cch, cand); return; }
    }
    {
        wchar_t cand[MAX_PATH]; _snwprintf_s(cand, _TRUNCATE, L"%snetpunch", ourDirW());   // ourDirW has a trailing '\'
        if (netDirUsable(cand)) { wcscpy_s(out, cch, cand); return; }
    }
    // dev checkout: %USERPROFILE%\tpf2-multiplayer\netpunch
    wchar_t up[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH)) _snwprintf_s(out, cch, _TRUNCATE, L"%s\\tpf2-multiplayer\\netpunch", up);
    else wcscpy_s(out, cch, L"C:\\tpf2-multiplayer\\netpunch");
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
static bool           g_panelBuilt = false, g_panelInPlace = false;
static char           g_code[128] = "";                   // host/own code to display
static volatile LONG  g_haveCode = 0;
static wchar_t        g_startSaveW[600] = L"";             // host: the .sav it chose to share
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

static volatile LONG g_showOverlay = 0;   // set by the CreatePage detour (page==2)
static HWND g_gameWnd = nullptr;
static BOOL CALLBACK FindGameWnd(HWND h, LPARAM lp);
static void StartLobby(int join);     // host=0 / join=1 -> spawns lobby.py
static void LeaveLobby();
static DWORD WINAPI KbHookThread(LPVOID);
static void LobbySend(const char* jsonLine);
static void ClipboardSet(const char* utf8);
static bool newestSave(wchar_t* out, int cch);
static bool doStartLoad(const wchar_t* srcSav);
// Lobby lifecycle flags (all cleared in StartLobby):
//  g_lobbyReady -- set when the FIRST event line is read from lobby_out.jsonl.
//    lobby.py truncates lobby_in.jsonl at startup, so a command appended before
//    that first event would be silently lost; START GAME / chat wait for it.
//  g_saveReady  -- joiner: the 'save_ready' event arrived this session, i.e. the
//    incoming_save.* files are complete and a 'start' with save=true may load them.
//  g_lobbyAbort -- set by a teardown (LEAVE / re-HOST) so a LobbyThread parked in
//    the wait-for-title-screen / click loop gives up instead of clicking later.
static volatile LONG g_lobbyReady = 0;
static volatile LONG g_saveReady  = 0;
static volatile LONG g_lobbyAbort = 0;
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

// screen-space button rect (matches the menu column position)
// ---- multiplayer panel state ----
static volatile LONG g_uiState = 0;     // 0 collapsed, 1 host/join choice, 2 lobby
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
static char g_status[256] = "";
// lobby model (fed from lobby_out.jsonl)
static char g_players[8][40]; static int g_playerCount = 0;
static char g_you[40] = ""; static char g_host[40] = "";
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
struct Hit { int x, y, w, h; int id; };   // id: 1=multiplayer/host-toggle, 2=HOST, 3=JOIN, 4=back
static Hit g_hits[6]; static int g_hitCount = 0;
static void addHit(int x,int y,int w,int h,int id){ if(g_hitCount<6){g_hits[g_hitCount++]={x,y,w,h,id};} }

// ---------------- A/B test flags (tpf2_menu_flags.txt next to this dll) ----------------
// Two candidate looks for the collapsed Multiplayer button, both live at once so
// they can be compared in one launch:
//   overlay=native   GDI overlay restyled to res/config/style_sheet/main-menu.lua:
//                    transparent, Lato 24 uppercase white, padding 8/15, hover =
//                    white @50/255, pressed = white @100/255 (alpha-blended over a
//                    readback of the game frame -- the swapchain has TRANSFER_SRC).
//   overlay=classic  the previous filled Settings-dialog style.
//   native=1         ALSO insert a REAL UI::Button into the title menu's "MainMenu"
//                    list (hook the page builder, add via the list's own add call,
//                    connect a click slot). It appends after the game's last entry.
//   scale=<f>        UI scale for the overlay (default = screen height / 1080).
//   ox=<f> oy=<f>    overlay top-left as a fraction of the screen (default .05/.30).
//   fontpx=<n>       override the overlay label pixel size (default 24*scale).
static int   g_flagOverlayNative = 1;
static int   g_flagNativeBtn     = 1;
static float g_flagScale = 0.f, g_flagOx = 0.05f, g_flagOy = 0.30f;
static int   g_flagFontPx = 0;
static bool  g_latoLoaded = false;
static int   g_hover = 0, g_active = 0;     // hit id under the cursor / pressed (native overlay look)
static void ReadFlags()
{
    char p[MAX_PATH]; snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", ourDirA());
    FILE* f = fopen(p, "r"); if (!f) { Log("[menu] flags: no %s (defaults overlay=native native=1)\n", p); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0; const char* v = eq + 1;
        if (!strcmp(line, "overlay")) g_flagOverlayNative = strncmp(v, "classic", 7) != 0;
        else if (!strcmp(line, "native")) g_flagNativeBtn = atoi(v);
        else if (!strcmp(line, "scale")) g_flagScale = (float)atof(v);
        else if (!strcmp(line, "ox")) g_flagOx = (float)atof(v);
        else if (!strcmp(line, "oy")) g_flagOy = (float)atof(v);
        else if (!strcmp(line, "fontpx")) g_flagFontPx = atoi(v);
    }
    fclose(f);
    Log("[menu] flags: overlay=%s native=%d scale=%.2f ox=%.3f oy=%.3f fontpx=%d\n",
        g_flagOverlayNative ? "native" : "classic", g_flagNativeBtn, g_flagScale, g_flagOx, g_flagOy, g_flagFontPx);
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
static float UiScale() { return g_flagScale > 0.f ? g_flagScale : (g_scExtent.height ? g_scExtent.height / 1080.f : 1.f); }

// TF2 palette (flat, cool blue-grey; matches the Settings dialog)
#define TF_PANEL    RGB(52, 66, 84)
#define TF_PANEL_BD RGB(96, 112, 134)
#define TF_FIELD    RGB(64, 80, 99)      // value boxes / buttons
#define TF_FIELD_BD RGB(112, 132, 156)
#define TF_INSET    RGB(38, 50, 66)      // recessed (code box)
#define TF_TEXT     RGB(232, 238, 245)   // primary
#define TF_TEXT_DIM RGB(176, 190, 208)   // labels
#define TF_ACCENT   RGB(150, 200, 210)   // the teal underline TF2 uses

static HFONT mkFont(int px, int weight, bool mono = false)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, mono ? L"Consolas" : L"Segoe UI");
}
// the menu face, grayscale-antialiased (coverage is read back as alpha, so no ClearType fringes)
static HFONT mkLato(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, g_latoLoaded ? L"Lato" : L"Segoe UI");
}
static void flatRect(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF border);
static void textIn(HDC dc, int x, int y, int w, int h, const wchar_t* s, HFONT f, COLORREF col, UINT fmt, int track);
// coverage mask for the native-look collapsed button (0..255 per pixel, w*h), plus
// the label box inside it. Rebuilt on dirty; composed over the frame every present.
static unsigned char* g_cov = nullptr; static int g_covW = 0, g_covH = 0;
static int g_nativeFontPx() { int px = g_flagFontPx ? g_flagFontPx : (int)(24 * UiScale() + .5f); return px < 8 ? 8 : px; }
// size of the native-look button = label extent + stylesheet padding {8,15,8,15} * scale
static void MeasureNative(int* w, int* h)
{
    float s = UiScale(); int px = g_nativeFontPx();
    HDC dc = GetDC(nullptr); HFONT f = mkLato(px); HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = { 0, 0 }; GetTextExtentPoint32W(dc, L"MULTIPLAYER", 11, &sz);
    SelectObject(dc, of); DeleteObject(f); ReleaseDC(nullptr, dc);
    *w = sz.cx + (int)(30 * s + .5f); *h = sz.cy + (int)(16 * s + .5f);
    if (*w > g_panelW) *w = g_panelW; if (*h > g_panelH) *h = g_panelH;
}
static void RenderNativeCoverage(int w, int h)
{
    if (g_covW != w || g_covH != h) { free(g_cov); g_cov = (unsigned char*)calloc((size_t)w * h, 1); g_covW = w; g_covH = h; }
    HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbm = SelectObject(mem, dib);
    flatRect(mem, 0, 0, w, h, RGB(0, 0, 0), CLR_INVALID);          // white-on-black = coverage
    HFONT f = mkLato(g_nativeFontPx());
    textIn(mem, 0, 0, w, h, L"MULTIPLAYER", f, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0);
    DeleteObject(f);
    const unsigned char* src = (const unsigned char*)bits;
    for (int i = 0; i < w * h; i++) { unsigned char r = src[i * 4], g = src[i * 4 + 1], b = src[i * 4 + 2];
        unsigned char m = r > g ? r : g; g_cov[i] = m > b ? m : b; }
    SelectObject(mem, oldbm); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
// out = bg + (255-bg)*a, a = hoverFill + text*(1-hoverFill); both the fill and the
// text are white, exactly like Button:hover / Button:active in default.lua.
static void ComposeNative(const unsigned char* bg, size_t bgPitch, void* dst, size_t pitch, int w, int h)
{
    int fill = g_active ? 100 : (g_hover ? 50 : 0);
    for (int y = 0; y < h; y++) {
        const unsigned char* s = bg + y * bgPitch; unsigned char* d = (unsigned char*)dst + y * pitch;
        for (int x = 0; x < w; x++) {
            int cov = g_cov ? g_cov[y * w + x] : 0;
            int a = fill + cov * (255 - fill) / 255;
            for (int c = 0; c < 3; c++) { int v = s[x * 4 + c]; d[x * 4 + c] = (unsigned char)(v + (255 - v) * a / 255); }
            d[x * 4 + 3] = 255;
        }
    }
}
static void flatRect(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF border)
{
    RECT rc = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(fill); FillRect(dc, &rc, b); DeleteObject(b);
    if (border != CLR_INVALID) { HBRUSH bb = CreateSolidBrush(border); FrameRect(dc, &rc, bb); DeleteObject(bb); }
}
// uppercase, letter-spaced text (TF2's title/tab/action style)
static void textIn(HDC dc, int x, int y, int w, int h, const wchar_t* s, HFONT f, COLORREF col, UINT fmt, int track = 0)
{
    HGDIOBJ of = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, col);
    int oldx = SetTextCharacterExtra(dc, track);
    RECT rc = { x, y, x + w, y + h }; DrawTextW(dc, s, -1, &rc, fmt);
    SetTextCharacterExtra(dc, oldx); SelectObject(dc, of);
}
// a flat TF2-style button: field fill, thin border, uppercase tracked label
static void drawBtn(HDC dc, int x, int y, int w, int h, const wchar_t* label, int fontpx, bool accent)
{
    flatRect(dc, x, y, w, h, TF_FIELD, TF_FIELD_BD);
    if (accent) { HBRUSH a = CreateSolidBrush(TF_ACCENT); RECT ln = { x, y, x + w, y + 2 }; FillRect(dc, &ln, a); DeleteObject(a); }
    HFONT f = mkFont(fontpx, FW_NORMAL);
    textIn(dc, x, y, w, h, label, f, TF_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 3);
    DeleteObject(f);
}

// GDI-render the current panel into a top-down BGRA buffer, TF2 flat style.
static void RenderPanelGDI(void* dst, size_t pitch, int w, int h)
{
    HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbm = SelectObject(mem, dib);

    g_hitCount = 0;
    if (InterlockedCompareExchange(&g_uiState, 0, 0) == 0) {
        // collapsed: a flat menu-toned bar with a light tracked label + accent
        flatRect(mem, 0, 0, w, h, TF_FIELD, TF_FIELD_BD);
        HBRUSH a = CreateSolidBrush(TF_ACCENT); RECT ln = { 0, 0, 4, h }; FillRect(mem, &ln, a); DeleteObject(a);
        textIn(mem, 16, 0, w - 16, h, L"MULTIPLAYER", mkFont(24, FW_NORMAL), TF_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 4);
        addHit(0, 0, w, h, 1);
    } else if (InterlockedCompareExchange(&g_uiState, 0, 0) == 2) {
        // ---- LOBBY view ----
        flatRect(mem, 0, 0, w, h, TF_PANEL, TF_PANEL_BD);
        textIn(mem, 30, 22, w - 200, 38, L"MULTIPLAYER LOBBY", mkFont(28, FW_NORMAL), TF_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 5);
        { HBRUSH a = CreateSolidBrush(TF_ACCENT); RECT u = { 30, 62, 30 + 330, 64 }; FillRect(mem, &u, a); DeleteObject(a); }
        // close
        flatRect(mem, w - 50, 22, 30, 30, TF_FIELD, TF_FIELD_BD);
        textIn(mem, w - 50, 20, 30, 30, L"×", mkFont(20, FW_NORMAL), TF_TEXT_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        addHit(w - 50, 22, 30, 30, 5);   // close lobby == LEAVE (tear down lobby.py)
        // room-code strip: the host has a code to share (auto-copied); click to re-copy.
        int contentY = 80;
        if (InterlockedCompareExchange(&g_haveCode, 0, 0)) {
            int cy = 70, ch = 34;
            flatRect(mem, 30, cy, w - 60, ch, TF_INSET, TF_FIELD_BD);
            textIn(mem, 44, cy, 96, ch, L"ROOM CODE", mkFont(13, FW_NORMAL), TF_ACCENT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 2);
            wchar_t wcode[160]; MultiByteToWideChar(CP_UTF8, 0, g_code, -1, wcode, 160);
            textIn(mem, 146, cy, w - 60 - 146 - 150, ch, wcode, mkFont(16, FW_NORMAL, true), TF_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            textIn(mem, w - 30 - 150, cy, 138, ch, L"click to copy", mkFont(13, FW_NORMAL), TF_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            addHit(30, cy, w - 60, ch, 7);   // re-copy the code to the clipboard
            contentY = 116;
        }
        int contentH = h - contentY - 118;
        // left: player list
        int listW = 240;
        flatRect(mem, 30, contentY, listW, contentH, TF_INSET, TF_FIELD_BD);
        char hdr[48];
        if (g_modelCsInit) { EnterCriticalSection(&g_modelCs); snprintf(hdr, sizeof(hdr), "PLAYERS (%d)", g_playerCount); }
        else snprintf(hdr, sizeof(hdr), "PLAYERS");
        wchar_t whdr[48]; MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, 48);
        textIn(mem, 46, contentY + 12, listW - 24, 22, whdr, mkFont(14, FW_NORMAL), TF_ACCENT, DT_LEFT | DT_TOP | DT_SINGLELINE, 2);
        for (int i = 0; i < g_playerCount && i < 8; i++) {
            wchar_t wn[64]; MultiByteToWideChar(CP_UTF8, 0, g_players[i], -1, wn, 64);
            bool isYou = strcmp(g_players[i], g_you) == 0;
            bool isHost = strcmp(g_players[i], g_host) == 0;
            COLORREF c = isYou ? RGB(150, 210, 170) : TF_TEXT;
            HBRUSH dot = CreateSolidBrush(isHost ? TF_ACCENT : RGB(90, 108, 130));
            RECT dr = { 48, contentY + 44 + i * 30 + 6, 56, contentY + 44 + i * 30 + 14 }; FillRect(mem, &dr, dot); DeleteObject(dot);
            wchar_t line[80]; _snwprintf_s(line, _TRUNCATE, L"%s%s", wn, isYou ? L"  (you)" : L"");
            textIn(mem, 64, contentY + 44 + i * 30, listW - 40, 26, line, mkFont(16, FW_NORMAL), c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (g_modelCsInit) LeaveCriticalSection(&g_modelCs);
        // right: chat log
        int chatX = 30 + listW + 16, chatW = w - chatX - 30;
        flatRect(mem, chatX, contentY, chatW, contentH, TF_INSET, TF_FIELD_BD);
        if (g_modelCsInit) {
            EnterCriticalSection(&g_modelCs);
            int n = g_chatCount, ly = contentY + 12;
            for (int i = 0; i < n; i++) {
                wchar_t wl[220]; MultiByteToWideChar(CP_UTF8, 0, g_chatLog[(g_chatHead + i) % 14], -1, wl, 220);
                textIn(mem, chatX + 14, ly, chatW - 28, 24, wl, mkFont(15, FW_NORMAL), TF_TEXT_DIM, DT_LEFT | DT_TOP | DT_SINGLELINE);
                ly += 24;
            }
            LeaveCriticalSection(&g_modelCs);
        }
        // chat input line
        int inY = contentY + contentH + 10;
        flatRect(mem, chatX, inY, chatW, 40, RGB(30, 40, 54), TF_FIELD_BD);
        wchar_t wi[220]; MultiByteToWideChar(CP_UTF8, 0, g_chatInput, -1, wi, 220);
        wchar_t withcaret[224]; _snwprintf_s(withcaret, _TRUNCATE, L"%s%s", wi, (GetTickCount64() / 500) % 2 ? L"|" : L" ");
        const wchar_t* shown = g_chatLen ? withcaret : L"type a message and press Enter…";
        textIn(mem, chatX + 12, inY, chatW - 24, 40, shown, mkFont(15, FW_NORMAL), g_chatLen ? TF_TEXT : RGB(120, 138, 160), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // bottom buttons: LEAVE (all), START (host)
        int by = h - 60;
        drawBtn(mem, 30, by, 150, 40, L"LEAVE", 18, false); addHit(30, by, 150, 40, 5);
        if (InterlockedCompareExchange(&g_isHost, 0, 0)) { drawBtn(mem, w - 30 - 180, by, 180, 40, L"START GAME", 18, true); addHit(w - 30 - 180, by, 180, 40, 6); }
        // status (small, center-bottom)
        char st[256]; if (g_csInit) { EnterCriticalSection(&g_statusCs); strncpy_s(st, g_status, _TRUNCATE); LeaveCriticalSection(&g_statusCs); } else st[0] = 0;
        wchar_t wst[256]; MultiByteToWideChar(CP_UTF8, 0, st, -1, wst, 256);
        textIn(mem, 200, by + 8, w - 420, 24, wst, mkFont(13, FW_NORMAL), TF_TEXT_DIM, DT_CENTER | DT_TOP | DT_SINGLELINE);
    } else {
        // host/join choice card, TF2 Settings-dialog styling
        flatRect(mem, 0, 0, w, h, TF_PANEL, TF_PANEL_BD);
        // title (uppercase, tracked) + accent underline, like SETTINGS
        textIn(mem, 34, 26, w - 120, 40, L"MULTIPLAYER", mkFont(30, FW_NORMAL), TF_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 6);
        { HBRUSH a = CreateSolidBrush(TF_ACCENT); RECT u = { 34, 70, 34 + 260, 72 }; FillRect(mem, &u, a); DeleteObject(a); }
        textIn(mem, 36, 84, w - 72, 24, L"Direct peer-to-peer, no servers.  Share the code over Discord.",
               mkFont(15, FW_NORMAL), TF_TEXT_DIM, DT_LEFT | DT_TOP | DT_SINGLELINE);
        // close (flat, top-right)
        flatRect(mem, w - 54, 24, 30, 30, TF_FIELD, TF_FIELD_BD);
        textIn(mem, w - 54, 22, 30, 30, L"×", mkFont(20, FW_NORMAL), TF_TEXT_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        addHit(w - 54, 24, 30, 30, 4);
        // HOST / JOIN (flat fields)
        int bw = (w - 88) / 2, bh = 64, by = 124;
        drawBtn(mem, 36, by, bw, bh, L"HOST", 24, true);              addHit(36, by, bw, bh, 2);
        drawBtn(mem, w - 36 - bw, by, bw, bh, L"JOIN", 24, true);     addHit(w - 36 - bw, by, bw, bh, 3);
        textIn(mem, 36, by + bh + 8, bw, 22, L"create a game, share your code", mkFont(14, FW_NORMAL), TF_TEXT_DIM, DT_CENTER | DT_TOP | DT_SINGLELINE);
        textIn(mem, w - 36 - bw, by + bh + 8, bw, 22, L"paste friend's code, connect", mkFont(14, FW_NORMAL), TF_TEXT_DIM, DT_CENTER | DT_TOP | DT_SINGLELINE);
        // code box (recessed, like a value field)
        int cy = by + bh + 42;
        flatRect(mem, 36, cy, w - 72, 92, TF_INSET, TF_FIELD_BD);
        textIn(mem, 52, cy + 12, w - 104, 20, L"YOUR CODE", mkFont(13, FW_NORMAL), TF_ACCENT, DT_LEFT | DT_TOP | DT_SINGLELINE, 3);
        wchar_t wcode[160];
        if (InterlockedCompareExchange(&g_haveCode, 0, 0)) MultiByteToWideChar(CP_UTF8, 0, g_code, -1, wcode, 160);
        else wcscpy_s(wcode, L"— press HOST to generate —");
        textIn(mem, 52, cy + 34, w - 104, 30, wcode, mkFont(19, FW_NORMAL, true), TF_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (InterlockedCompareExchange(&g_haveCode, 0, 0))
            textIn(mem, 52, cy + 66, w - 104, 20, L"copied to clipboard", mkFont(13, FW_NORMAL), TF_TEXT_DIM, DT_LEFT | DT_TOP | DT_SINGLELINE);
        // status line
        char st[256]; if (g_csInit) { EnterCriticalSection(&g_statusCs); strncpy_s(st, g_status, _TRUNCATE); LeaveCriticalSection(&g_statusCs); } else st[0] = 0;
        wchar_t wst[256]; MultiByteToWideChar(CP_UTF8, 0, st, -1, wst, 256);
        textIn(mem, 36, cy + 108, w - 72, 60, wst, mkFont(15, FW_NORMAL), TF_TEXT_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    const unsigned char* src = (const unsigned char*)bits;
    for (int row = 0; row < h; row++)
        memcpy((unsigned char*)dst + row * pitch, src + row * (w * 4), w * 4);
    SelectObject(mem, oldbm); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
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
static bool BuildPanelImage()
{
    if (g_panelBuilt) return true;
    if (!pCreateImage || !pAllocMem || !pMapMem) return false;
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
    pResetFences(g_dev, 1, &g_fence); pSubmit(g_qFromFam ? g_qFromFam : VK_NULL_HANDLE, 1, &si, g_fence);
    g_panelBuilt = true;
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
static bool BuildBackdropImage()
{
    if (g_bdBuilt) return true; if (g_bdFail) return false;
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
    pResetFences(g_dev, 1, &g_fence); pSubmit(g_qFromFam ? g_qFromFam : VK_NULL_HANDLE, 1, &si, g_fence);
    pWaitFences(g_dev, 1, &g_fence, VK_TRUE, 100000000ull);
    g_bdBuilt = true;
    Log("[menu] vk: backdrop image ready pitch=%zu\n", g_bdPitch);
    return true;
}
// Pull the region under the button out of this frame's swapchain image (usage has
// TRANSFER_SRC) so the CPU can blend the label over it.
static void CopyBackdrop(VkQueue q, uint32_t imgIndex)
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
    pResetFences(g_dev, 1, &g_fence); pSubmit(q, 1, &si, g_fence);
    pWaitFences(g_dev, 1, &g_fence, VK_TRUE, 100000000ull);
    if (pInvalidate) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; r.memory = g_bdMem; r.size = VK_WHOLE_SIZE; pInvalidate(g_dev, 1, &r); }
}
static bool nativeLookActive() { return g_flagOverlayNative && InterlockedCompareExchange(&g_uiState, 0, 0) == 0; }

static void PanelLayout()
{
    if (nativeLookActive()) {
        // collapsed, native look: sized to the label like a real MainMenu Button,
        // placed by the ox/oy flags (default 5vw in, like the stylesheet's margin)
        MeasureNative(&g_copyW, &g_copyH);
        g_panelX = (int)(g_scExtent.width * g_flagOx);
        g_panelY = (int)(g_scExtent.height * g_flagOy);
        if (g_panelX + g_copyW > (int)g_scExtent.width)  g_panelX = (int)g_scExtent.width - g_copyW;
        if (g_panelY + g_copyH > (int)g_scExtent.height) g_panelY = (int)g_scExtent.height - g_copyH;
    } else if (InterlockedCompareExchange(&g_uiState, 0, 0) == 0) {
        // collapsed: small button in the menu's left column
        g_copyW = 300; g_copyH = 60;
        g_panelX = (int)(g_scExtent.width * 0.062f);
        g_panelY = (int)(g_scExtent.height * 0.40f);
    } else {
        // expanded: the full card, centered on screen
        g_copyW = g_panelW; g_copyH = g_panelH;
        g_panelX = ((int)g_scExtent.width - g_panelW) / 2;
        g_panelY = ((int)g_scExtent.height - g_panelH) / 2;
    }
}

static void DrawButton(VkQueue q, uint32_t imgIndex)
{
    if (imgIndex >= g_scImgCount) return;
    if (!BuildPanelImage()) return;
    PanelLayout();
    if (nativeLookActive() && BuildBackdropImage()) {
        // native look: readback the frame under the button, blend the label over
        // it on the CPU, then the usual copy puts the blended block back.
        if (InterlockedCompareExchange(&g_panelDirty, 0, 1) == 1 || g_covW != g_copyW || g_covH != g_copyH)
            RenderNativeCoverage(g_copyW, g_copyH);
        g_hitCount = 0; addHit(0, 0, g_copyW, g_copyH, 1);
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        CopyBackdrop(q, imgIndex);
        ComposeNative((const unsigned char*)g_bdPtr, g_bdPitch, g_panelPtr, g_panelPitch, g_copyW, g_copyH);
        if (pFlush) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; r.memory = g_panelMem; r.size = VK_WHOLE_SIZE; pFlush(g_dev, 1, &r); }
        QueryPerformanceCounter(&t1);
        static int n = 0; if (++n % 600 == 1) Log("[menu] native-look blend %dx%d hover=%d active=%d %.2f ms\n",
            g_copyW, g_copyH, g_hover, g_active, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    } else if (InterlockedCompareExchange(&g_panelDirty, 0, 1) == 1 && g_panelPtr) {
        RenderPanelGDI(g_panelPtr, g_panelPitch, g_copyW, g_copyH);
        if (pFlush) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; r.memory = g_panelMem; r.size = VK_WHOLE_SIZE; pFlush(g_dev, 1, &r); }
    }
    VkCommandBuffer cb = g_cmd[imgIndex]; pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
    region.dstOffset = { g_panelX, g_panelY, 0 };
    region.extent = { (uint32_t)g_copyW, (uint32_t)g_copyH, 1 };
    pCmdCopyImage(cb, g_panelImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrierImage(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    pEndCB(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    pResetFences(g_dev, 1, &g_fence);
    pSubmit(q, 1, &si, g_fence);
    pWaitFences(g_dev, 1, &g_fence, VK_TRUE, 100000000ull);
}

// In-frame click: the button is not a window, so poll the cursor + left button
// against the button rect (converted to the game window's client area). One-shot
// per press, 1s debounce.
static void OnHit(int id);
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

static void PollClick()
{
    static bool prevDown = false;
    static ULONGLONG lastFire = 0;
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
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
    if (down && !prevDown && gameHasFocus()) {
        POINT pt; GetCursorPos(&pt);
        if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = nullptr; EnumWindows(FindGameWnd, (LPARAM)&g_gameWnd); }
        POINT origin = { 0, 0 };
        if (g_gameWnd) ClientToScreen(g_gameWnd, &origin);
        // panel-local click coords
        int lx = pt.x - origin.x - g_panelX;
        int ly = pt.y - origin.y - g_panelY;
        ULONGLONG now = GetTickCount64();
        if (now - lastFire > 400) {
            for (int i = 0; i < g_hitCount; i++) {
                const Hit& hh = g_hits[i];
                if (lx >= hh.x && lx < hh.x + hh.w && ly >= hh.y && ly < hh.y + hh.h) {
                    lastFire = now; OnHit(hh.id); break;
                }
            }
        }
    }
    prevDown = down;
}

static void OnHit(int id)
{
    Log("[menu] hit id=%d\n", id);
    switch (id) {
    case 1: InterlockedExchange(&g_uiState, 1); InterlockedExchange(&g_panelDirty, 1); break; // expand
    case 4: InterlockedExchange(&g_uiState, 0); InterlockedExchange(&g_panelDirty, 1); break; // collapse
    case 2: StartLobby(0); break;   // HOST  -> lobby (host)
    case 3: StartLobby(1); break;   // JOIN  -> lobby (join)
    case 5: LeaveLobby(); break;                                    // LEAVE lobby
    case 6: if (InterlockedCompareExchange(&g_isHost,0,0)) {   // START GAME (host): share newest save, then start
        // lobby.py truncates lobby_in.jsonl when it starts: a command appended
        // before its first event line would be lost. Wait for that first line.
        if (!InterlockedCompareExchange(&g_lobbyReady, 0, 0)) { SetStatus("Lobby is starting…"); break; }
        if (newestSave(g_startSaveW, 600)) {
            char u[900]; WideCharToMultiByte(CP_UTF8, 0, g_startSaveW, -1, u, sizeof(u), nullptr, nullptr);
            char esc[1024]; int j = 0; for (int i = 0; u[i] && j < 1010; i++) { if (u[i] == '\\' || u[i] == '"') esc[j++] = '\\'; esc[j++] = u[i]; } esc[j] = 0;
            char line[1200]; snprintf(line, sizeof(line), "{\"cmd\":\"start\",\"save\":\"%s\"}", esc);
            LobbySend(line); SetStatus("Sharing save & starting game…");
        } else { LobbySend("{\"cmd\":\"start\"}"); SetStatus("No save found to share."); }
    } break;
    case 7: if (InterlockedCompareExchange(&g_haveCode,0,0)) { ClipboardSet(g_code); SetStatus("Code copied to clipboard — share it in Discord."); } break;
    }
}

static VkResult myPresent(VkQueue q, const VkPresentInfoKHR* pi)
{
    LONG n = InterlockedIncrement(&g_presentCount);
    if (n == 1) Log("[menu] PRESENT #1 swapchains=%u dev=%p\n", pi->swapchainCount, g_dev);
    if (n % 300 == 0) Log("[menu] present state: show=%ld swc=%u rInit=%d rFail=%d dev=%p fam=%u fmt=%d\n",
        InterlockedCompareExchange(&g_showOverlay, 0, 0), pi->swapchainCount,
        (int)g_rInit, (int)g_rFail, g_dev, g_qfam, (int)g_scFormat);
    __try {
        if (InterlockedCompareExchange(&g_showOverlay, 0, 0) && pi->swapchainCount >= 1) {
            VkSwapchainKHR sc = pi->pSwapchains[0];
            uint32_t idx = pi->pImageIndices[0];
            if ((!g_rInit || sc != g_theSc) && !g_rFail) InitRender(sc);
            if (g_rInit && sc == g_theSc) { DrawButton(q, idx); PollClick(); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool once = false;
        if (!once) { once = true; Log("[menu] myPresent FAULT exc=%lx (rInit=%d rFail=%d)\n",
            GetExceptionCode(), (int)g_rInit, (int)g_rFail); }
    }
    return g_realPresent(q, pi);
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
        g_scFormat = ci->imageFormat; g_scExtent = ci->imageExtent;
        g_rInit = false; g_rFail = false;   // rebuild on next present
        Log("[menu] swapchain created fmt=%d %ux%u usage=0x%x (TRANSFER_DST=%d)\n",
            (int)ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height,
            ci->imageUsage, (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? 1 : 0);
    }
    return r;
}

static PFN_vkVoidFunction myGdpa(VkDevice dev, const char* name)
{
    PFN_vkVoidFunction real = g_origGdpa(dev, name);
    if (!name || !real) return real;
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
static const char* GStringData(const GString* s) {
    return (s->cap > 15) ? *(const char* const*)s->buf : s->buf;
}

// Step 2a probe: prove we can build a game std::string from our detour thread
// (the UI thread -- the correct ABI context). Runs once, on the first main page.
static bool g_probedString = false;
static void ProbeString()
{
    __try {
        GString s; GStringInit(&s);
        const char* key = "mp.multiplayer";
        g_strAssign(&s, key, strlen(key));
        // log the raw 32 bytes + the interpreted value
        char hex[80]; int o = 0;
        const unsigned char* b = (const unsigned char*)&s;
        for (int i = 0; i < 32 && o < 76; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02x", b[i]);
        Log("[menu] str probe: size=%llu cap=%llu data='%s' bytes=%s\n",
            (unsigned long long)s.size, (unsigned long long)s.cap, GStringData(&s), hex);
        // free heap storage if it allocated (cap>15 means it went off the SSO buffer)
        // -- "mp.multiplayer" is 14 chars so it stays inline; nothing to free.
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[menu] str probe FAULTED (exc=%lx) -- string ABI is off\n", GetExceptionCode());
    }
}

// Step 2b: build a real Button widget from injected code. Action ctx then the
// Button factory. Log the returned pointers; do NOT insert into the layout yet.
static bool g_probedWidget = false;
static void ProbeWidget()
{
    __try {
        // action context for our key -- outBuf sized generously in case the
        // built object is larger than a std::string (32 B).
        alignas(16) unsigned char ctxBuf[128];
        memset(ctxBuf, 0, sizeof(ctxBuf));
        void* ctx = g_actionCtx(ctxBuf, "mp.multiplayer");
        Log("[menu] widget probe: actionCtx ret=%p ctxBuf[0..7]=%016llx\n",
            ctx, *(unsigned long long*)ctxBuf);

        // two empty icon strings (like the "Free Game" button)
        GString iconA, iconB; GStringInit(&iconA); GStringInit(&iconB);
        void* btn = g_btn(ctx ? ctx : ctxBuf, &iconA, &iconB);
        Log("[menu] widget probe: BUTTON ret=%p (vtable=%016llx)\n",
            btn, btn ? *(unsigned long long*)btn : 0ull);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[menu] widget probe FAULTED (exc=%lx) at step -- signature/ctx off\n",
            GetExceptionCode());
    }
}

// ---------------- native title-menu button (A/B test, flag native=1) ----------------
// What the page builder 667bc0 really does per entry (decompiled, mainmenu_ref.c):
//   btn = 7c5d30(221c930(&s, "Load Game"), &empty, &empty)   build the Button
//   227a1e0(btn, &"continue")                                 style class (optional)
//   2518f0(btn, &connOut, &std::function<void()>)             connect the click slot
//   227f880(btn, 4)                                           prepare
//   2d99e0(list, btn, &"list-item")                           ADD TO THE "MainMenu" LIST
// The list (a 0x508-byte widget named "MainMenu") is a local of the builder, so we
// hook the list's add call and remember the list pointer while the builder runs;
// after it returns we run the same five steps for our own entry. The click slot is
// an MSVC std::function whose in-place impl vtable is {Copy, Move, DoCall,
// TargetType, DeleteThis} (docs/M6_MENU_UI.md) -- we supply a static one.
static const uintptr_t RVA_LIST_ADD = 0x22d99e0;  static const int STEAL_LIST_ADD = 15;   // push rdi; sub rsp,60; mov [rsp+20],-2
static const uintptr_t RVA_SETNAME  = 0x227a1e0;
static const uintptr_t RVA_PREP     = 0x227f880;
typedef void* (*ListAddFn)(void* list, void* widget, void* styleStr);
typedef void  (*SetNameFn)(void* widget, void* str);
typedef void  (*PrepFn)(void* widget, int flags);
static ListAddFn g_origListAdd = nullptr;
static SetNameFn g_setName = nullptr;
static PrepFn    g_prep = nullptr;
static volatile LONG g_inMainBuild = 0;
static void* g_mainList = nullptr;
static int   g_mainListAdds = 0;
static void OnHit(int id);

struct FuncBase { const void* const* vptr; void* capture; };
static FuncBase* __fastcall MpCopy(const FuncBase* self, void* dest) { FuncBase* d = (FuncBase*)dest; d->vptr = self->vptr; d->capture = self->capture; return d; }
static FuncBase* __fastcall MpMove(FuncBase* self, void* dest)       { FuncBase* d = (FuncBase*)dest; d->vptr = self->vptr; d->capture = self->capture; return d; }
static void      __fastcall MpDoCall(FuncBase*)
{
    Log("[menu] NATIVE BUTTON CLICKED -> expanding the overlay panel\n");
    InterlockedExchange(&g_uiState, 1); InterlockedExchange(&g_panelDirty, 1);
}
static const void* __fastcall MpTargetType(const FuncBase* self) { return self; }   // never consulted by the signal
static void      __fastcall MpDeleteThis(FuncBase*, bool)          { }              // 16-byte impl is always in-place
static const void* const g_mpFuncVtbl[5] = { (const void*)&MpCopy, (const void*)&MpMove, (const void*)&MpDoCall, (const void*)&MpTargetType, (const void*)&MpDeleteThis };
struct GFunc { FuncBase impl; void* pad[5]; void* ptr; };   // MSVC std::function: 64 B, impl ptr at +0x38
static_assert(sizeof(GFunc) == 64, "std::function layout");

static void* MyListAdd(void* list, void* widget, void* style)
{
    if (InterlockedCompareExchange(&g_inMainBuild, 0, 0)) { g_mainList = list; g_mainListAdds++; }
    return g_origListAdd(list, widget, style);
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
        g_prep(btn, 4);
        GString li; GStringInit(&li); g_strAssign(&li, "list-item", 9);
        g_origListAdd(g_mainList, btn, &li);
        g_myButton = btn;
        Log("[menu] native: BUTTON APPENDED to the MainMenu list -- look below the game's last entry\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[menu] native: FAULTED exc=%lx (button not inserted; set native=0 in tpf2_menu_flags.txt if the menu is unstable)\n", GetExceptionCode());
    }
}

static void MyMainBuild(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4)
{
    g_mainList = nullptr; g_mainListAdds = 0;
    InterlockedExchange(&g_inMainBuild, 1);
    g_origMainBuild(p1, p2, p3, p4);
    InterlockedExchange(&g_inMainBuild, 0);
    Log("[menu] main-page builder ran (list=%p adds=%d)\n", g_mainList, g_mainListAdds);
    if (g_flagNativeBtn) NativeInsert();
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
static const wchar_t* NETDIR = L"C:\\tpf2-multiplayer\\netpunch";   // placeholder: resolveNetDir() replaces it at init

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
    // Copy up to the closing (unescaped) quote, decoding the escapes json.dumps
    // emits for our payloads: \\ -> \, \" -> ", \/ -> /. Anything else (\uXXXX,
    // \n ...) is left verbatim -- the backslash is copied and the next character
    // follows on the next iteration.
    int i = 0; const char* r = last;
    while (*r && *r != '"' && i < dsz - 1) {
        if (*r == '\\' && (r[1] == '\\' || r[1] == '"' || r[1] == '/')) { dst[i++] = r[1]; r += 2; }
        else dst[i++] = *r++;
    }
    dst[i] = 0;
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
static char   g_username[40] = "";
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

static void LobbySend(const char* jsonLine)   // append a command to lobby_in.jsonl
{
    wchar_t p[512]; _snwprintf_s(p, _TRUNCATE, L"%s\\lobby_in.jsonl", NETDIR);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w; char line[1600]; int L = snprintf(line, sizeof(line), "%s\n", jsonLine);   // start lines carry a full save path
    WriteFile(h, line, L, &w, nullptr); CloseHandle(h);
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

static void writeBridgeCtl(bool isHost)
{
    static char last[128] = "";
    char content[128];
    // pid= addresses the ctl to OUR bridge. Two instances sharing a data dir
    // (a sandboxed second instance reads through to the real dir until it has
    // its own copy) otherwise apply each other's role for a moment.
    unsigned long bpid = readBridgePid();
    snprintf(content, sizeof(content), "instance=%c\npeer=127.0.0.1:%d\npid=%lu\n",
             isHost ? 'a' : 'b', isHost ? GAME_RELAY_PORT_HOST : GAME_RELAY_PORT_JOIN, bpid);
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
    Log("[menu] bridge ctl -> %ls: instance=%c peer=127.0.0.1:%d\n",
        path, isHost ? 'a' : 'b', isHost ? GAME_RELAY_PORT_HOST : GAME_RELAY_PORT_JOIN);
}

// parse a roster event: "players":["a","b"], "you":"a", "host":"a"
static void applyRoster(const char* s)
{
    if (!g_modelCsInit) return; EnterCriticalSection(&g_modelCs);
    g_playerCount = 0;
    // whitespace-tolerant: find "players", then its '[' .. ']', pull each "quoted" name.
    const char* pa = strstr(s, "\"players\"");
    if (pa) { pa = strchr(pa, '['); const char* end = pa ? strchr(pa, ']') : nullptr;
        if (pa && end) { const char* q = pa;
            while (g_playerCount < 8) {
                q = strchr(q, '"'); if (!q || q > end) break; q++;
                int k = 0; while (*q && *q != '"' && k < 38) g_players[g_playerCount][k++] = *q++;
                g_players[g_playerCount][k] = 0; g_playerCount++;
                if (*q == '"') q++;
            } } }
    char v[40];
    jsonStr(s, "you", v, sizeof(v)); if (v[0]) strcpy_s(g_you, v);
    jsonStr(s, "host", v, sizeof(v)); if (v[0]) strcpy_s(g_host, v);
    // Role is decided by the roster: you==host -> instance a, else b. Re-evaluated
    // on every roster (a host change re-points the bridge); writeBridgeCtl is a
    // no-op when nothing changed.
    bool roleKnown = g_you[0] && g_host[0];
    bool isHost = roleKnown && strcmp(g_you, g_host) == 0;
    LeaveCriticalSection(&g_modelCs); InterlockedExchange(&g_panelDirty, 1);
    if (roleKnown) writeBridgeCtl(isHost);
}

// ---------------- START GAME: place the shared save + load it in place ----------------
// The host's userdata save folder. For the sandboxed joiner (B) the game process
// is inside Sandboxie, so a CopyFileW/FindFirstFile to this path is transparently
// redirected to B's overlay -- both peers use this same constant.
static const wchar_t* SAVE_DIR =   // placeholder: resolveSaveDir() replaces it at init
    L"C:\\Program Files (x86)\\Steam\\userdata\\0\\1066780\\local\\save";

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
    if (!same && !CopyFileW(srcSav, dst, FALSE)) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION && _wcsicmp(srcSav, dst) == 0) same = true;
        else { Log("[menu] placeSave copy failed err=%lu src=%ls\n", e, srcSav); return false; }
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

// Click the title-menu Continue button (loads the newest save) on our own window.
// The overlay is post-present pixels (not a window), so the game's Continue stays
// hittable at its measured position; we collapse the overlay first for clarity.
// Returns true once the click took the game off the title menu (load started).
// UNUSED since 2026-08-30 -- doStartLoad now asks the player to load the save.
// Kept for reference until the menu's own load action is called directly.
static bool clickContinueLoad()
{
    // Continue only exists on the title page. If the user is in Settings / Load
    // Game (page >= 3 => overlay hidden) wait for them to come back, up to 60 s.
    if (InterlockedCompareExchange(&g_showOverlay, 0, 0) == 0) {
        SetStatus("Return to the title screen to load the shared save…");
        Log("[menu] clickContinue: not on title screen, waiting up to 60 s\n");
        bool visible = false;
        for (int i = 0; i < 120; i++) {   // 60 s, 500 ms poll
            if (InterlockedCompareExchange(&g_lobbyAbort, 0, 0)) { Log("[menu] clickContinue: aborted (lobby torn down)\n"); return false; }
            if (InterlockedCompareExchange(&g_showOverlay, 0, 0)) { visible = true; break; }
            Sleep(500);
        }
        if (!visible) { SetStatus("Timed out waiting for the title screen -- press Continue to load the shared save"); Log("[menu] clickContinue: gave up waiting for title screen\n"); return false; }
        Sleep(400);   // let the page settle before clicking
    }
    if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = nullptr; EnumWindows(FindGameWnd, (LPARAM)&g_gameWnd); }
    if (!g_gameWnd) { Log("[menu] clickContinue: no game window\n"); return false; }
    RECT wr; GetWindowRect(g_gameWnd, &wr);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    struct C { int w, h, x, y; };
    static const C T[] = { {3856,2128,349,1110}, {3840,2161,348,1108}, {2420,1399,236,680}, {1600,900,237,682} };
    int ox = -1, oy = -1;
    for (const C& c : T) if (c.w == W && c.h == H) { ox = c.x; oy = c.y; break; }
    if (ox < 0) {   // nearest measured size within 5%, scaled
        double be = 1e9; const C* bc = nullptr;
        for (const C& c : T) { double e = ((double)c.w / W - 1); if (e < 0) e = -e; double e2 = ((double)c.h / H - 1); if (e2 < 0) e2 = -e2; e += e2; if (e < be) { be = e; bc = &c; } }
        if (bc && be < 0.05) { ox = (int)((double)bc->x * W / bc->w); oy = (int)((double)bc->y * H / bc->h); }
    }
    if (ox < 0) { Log("[menu] clickContinue: no offset for %dx%d\n", W, H); return false; }
    int sx = wr.left + ox, sy = wr.top + oy;
    Log("[menu] clickContinue at %d,%d (win %dx%d)\n", sx, sy, W, H);
    POINT saved; GetCursorPos(&saved);
    bool started = false;
    // Click FIRST, then test: the overlay is visible at entry (checked above), so a
    // pre-click test could never distinguish "load started" from "not yet clicked".
    for (int i = 0; i < 14; i++) {
        if (InterlockedCompareExchange(&g_lobbyAbort, 0, 0)) break;
        SetForegroundWindow(g_gameWnd);
        SetCursorPos(sx, sy); Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); Sleep(40);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        Sleep(700);
        if (InterlockedCompareExchange(&g_showOverlay, 0, 0) == 0) { started = true; break; }  // left the menu => load started
    }
    SetCursorPos(saved.x, saved.y);
    Log("[menu] clickContinue: %s\n", started ? "load started" : "menu never left");
    return started;
}

// Place the shared save and click Continue. Returns true once a load was kicked
// off; false (with a status) if the save could not be placed or the click failed.
static bool doStartLoad(const wchar_t* srcSav)
{
    if (!srcSav || !srcSav[0]) { Log("[menu] doStartLoad: empty src\n"); SetStatus("No save to load."); return false; }
    if (!placeSaveNewest(srcSav)) { SetStatus("Couldn't place the shared save -- not loading"); return false; }
    // THE PLAYER LOADS IT. Synthesising a click on the title menu's Continue button
    // needed a table of measured button positions per window size, missed entirely on
    // any resolution not in it (2880x1801 on the test laptop), fought the other
    // instance for the one system cursor when two games ran on a single machine, and
    // did nothing at all if the player happened to be on another menu page. The save
    // is placed and stamped newest either way, so asking for one click always works.
    // clickContinueLoad() is kept below, unused, until the proper fix lands: calling
    // the menu's own load action (docs/re/LOAD_SAVE.md).
    InterlockedExchange(&g_lobbyDone, 1);   // release the keyboard: the lobby's work is done
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
    if (WaitForSingleObject(proc, waitMs) == WAIT_OBJECT_0) Log("[menu] lobby.py exited on quit\n");
    else { TerminateProcess(proc, 0); Log("[menu] lobby.py did not exit within %d ms -- terminated\n", waitMs); }
}

struct LobbyArg { int join; char code[160]; char name[40]; };

static DWORD WINAPI LobbyThread(LPVOID param)
{
    LobbyArg* a = (LobbyArg*)param;
    wchar_t wname[40]; MultiByteToWideChar(CP_UTF8, 0, a->name, -1, wname, 40);
    wchar_t cmd[1024];
    // Prefer the frozen netpunch.exe next to the scripts (no Python dependency on
    // the target machine); fall back to `python lobby.py` for the dev checkout.
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
    int relayPort  = a->join ? GAME_RELAY_PORT_JOIN : GAME_RELAY_PORT_HOST;
    int bridgePort = readBridgePort();
    if (a->join) { wchar_t wc[200]; MultiByteToWideChar(CP_UTF8, 0, a->code, -1, wc, 200);
                   _snwprintf_s(cmd, _TRUNCATE, L"%s join %s --name %s --local-port 0 --game-relay-port %d --game-local-port %d",
                                base, wc, wname, relayPort, bridgePort); }
    else _snwprintf_s(cmd, _TRUNCATE, L"%s host --name %s --game-relay-port %d --game-local-port %d",
                      base, wname, relayPort, bridgePort);
    Log("[menu] lobby cmd: %ls\n", cmd);

    wchar_t outPath[512]; _snwprintf_s(outPath, _TRUNCATE, L"%s\\lobby_out.jsonl", NETDIR);
    wchar_t inPath[512];  _snwprintf_s(inPath,  _TRUNCATE, L"%s\\lobby_in.jsonl", NETDIR);
    wchar_t logPath[512]; _snwprintf_s(logPath, _TRUNCATE, L"%s\\lobby_proc.log", NETDIR);
    DeleteFileW(outPath); DeleteFileW(inPath);
    if (a->join) {   // never let last session's transfer pass for this one (lobby.py does this too)
        static const wchar_t* const stale[] = { L"incoming_save.sav", L"incoming_save.sav.lua", L"incoming_save.jpg" };
        for (const wchar_t* nm : stale) { wchar_t f[560]; _snwprintf_s(f, _TRUNCATE, L"%s\\%s", NETDIR, nm); DeleteFileW(f); }
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hLog = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    LARGE_INTEGER off = { 0 }; char rem[2048]; int remLen = 0; char buf[8192];
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
                        rem[remLen] = 0;
                        // dispatch one event line
                        InterlockedExchange(&g_lobbyReady, 1);   // lobby.py is up and has truncated lobby_in.jsonl
                        char ty[24]; jsonStr(rem, "type", ty, sizeof(ty));
                        if (strcmp(ty, "code") == 0) { char cd[160]; jsonStr(rem, "code", cd, sizeof(cd)); if (cd[0]) { strcpy_s(g_code, cd); ClipboardSet(cd); InterlockedExchange(&g_haveCode, 1); SetStatus("Your code is copied — share it in Discord."); } }
                        else if (strcmp(ty, "roster") == 0) applyRoster(rem);
                        else if (strcmp(ty, "chat") == 0) { char fr[40], tx[256]; jsonStr(rem, "from", fr, sizeof(fr)); jsonStr(rem, "text", tx, sizeof(tx)); chatPush(fr, tx); }
                        else if (strcmp(ty, "status") == 0) { char de[200]; jsonStr(rem, "detail", de, sizeof(de)); if (de[0]) SetStatus(de); }
                        else if (strcmp(ty, "transfer") == 0) {
                            char role[16], st[16]; jsonStr(rem, "role", role, sizeof(role)); jsonStr(rem, "state", st, sizeof(st));
                            int pct = jsonInt(rem, "pct"); char msg[96];
                            if (strcmp(role, "recv") == 0) { if (pct >= 0) { snprintf(msg, sizeof(msg), "Receiving save\xE2\x80\xA6 %d%%", pct); SetStatus(msg); } }
                            else if (strcmp(st, "done") == 0) SetStatus("Save sent.");
                            else if (pct >= 0) { snprintf(msg, sizeof(msg), "Sending save\xE2\x80\xA6 %d%%", pct); SetStatus(msg); }
                        }
                        else if (strcmp(ty, "save_ready") == 0) { InterlockedExchange(&g_saveReady, 1); SetStatus("Save received \xE2\x80\x94 waiting for start\xE2\x80\xA6"); }
                        else if (strcmp(ty, "start") == 0) {
                            // {"type":"start","save":true|false}: save=true means a save
                            // transfer completed for this peer this session; absent => true.
                            bool withSave = jsonBool(rem, "save", true);
                            bool saveReady = InterlockedCompareExchange(&g_saveReady, 0, 0) != 0;
                            wchar_t src[600] = L""; bool go = true;
                            if (InterlockedCompareExchange(&g_isHost, 0, 0)) {
                                wcscpy_s(src, g_startSaveW[0] ? g_startSaveW : L""); if (!src[0]) newestSave(src, 600);
                            } else if (withSave) {
                                if (!saveReady) {   // the transfer never completed here: loading would pick a stale/unrelated save
                                    Log("[menu] start(save=true) but no save_ready this session -- ignoring\n");
                                    SetStatus("Start received but no save arrived -- ask the host to START again"); go = false;
                                } else _snwprintf_s(src, _TRUNCATE, L"%s\\incoming_save.sav", NETDIR);
                            } else {
                                // legacy no-save start: load what this side has (a received save if any, else our newest)
                                if (saveReady) _snwprintf_s(src, _TRUNCATE, L"%s\\incoming_save.sav", NETDIR);
                                else if (!newestSave(src, 600)) { SetStatus("No save to load."); go = false; }
                            }
                            if (go) {
                                SetStatus("Loading shared save…"); Sleep(400);
                                if (doStartLoad(src)) {
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
                        remLen = 0;
                    } else if (remLen < (int)sizeof(rem) - 1) rem[remLen++] = c;
                }
            }
            CloseHandle(h);
        }
        if (stop || WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
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
static HANDLE g_lobbyThread = nullptr;
static void TeardownLobby(int waitMs, bool joinThread)
{
    InterlockedExchange(&g_lobbyAbort, 1);   // unpark a thread waiting for the title screen / clicking
    if (g_lobbyCsInit) EnterCriticalSection(&g_lobbyCs);
    QuitLobbyProc(g_lobbyProc, waitMs);
    if (g_lobbyCsInit) LeaveCriticalSection(&g_lobbyCs);
    if (joinThread && g_lobbyThread) {
        if (WaitForSingleObject(g_lobbyThread, 3000) != WAIT_OBJECT_0) Log("[menu] lobby tail thread did not exit in time\n");
        CloseHandle(g_lobbyThread); g_lobbyThread = nullptr;
    }
}

static void StartLobby(int join)
{
    ensureUsername();
    LobbyArg* a = (LobbyArg*)calloc(1, sizeof(LobbyArg)); if (!a) return;
    a->join = join; strcpy_s(a->name, g_username);
    if (join) {
        if (!ClipboardGet(a->code, sizeof(a->code)) || strlen(a->code) < 8) {
            SetStatus("Copy your friend's code first, then press JOIN."); free(a); return;
        }
        char* s = a->code; while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t') memmove(s, s + 1, strlen(s));
        int L = (int)strlen(s); while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\r' || s[L-1] == '\n' || s[L-1] == '\t')) s[--L] = 0;
    }
    // A previous lobby.py still up (LEAVE not pressed, or a tail thread still
    // finishing) would fight the new one over lobby_in/out.jsonl: tear it down first.
    if (g_lobbyProc || g_lobbyThread) TeardownLobby(1500, true);
    InterlockedExchange(&g_lobbyAbort, 0);
    InterlockedExchange(&g_lobbyReady, 0);
    InterlockedExchange(&g_saveReady, 0);
    InterlockedExchange(&g_isHost, join ? 0 : 1);
    InterlockedExchange(&g_lobbyDone, 0);   // a new lobby captures typing again
    InterlockedExchange(&g_uiState, 2); InterlockedExchange(&g_panelDirty, 1);
    g_chatCount = 0; g_chatHead = 0; g_playerCount = 0;
    g_lobbyThread = CreateThread(nullptr, 0, LobbyThread, a, 0, nullptr);
    if (!g_lobbyThread) { free(a); SetStatus("Couldn't start the lobby thread."); }
}
static void LeaveLobby()
{
    TeardownLobby(1500, false);   // quit, wait up to 1.5 s for a clean exit, then kill
    InterlockedExchange(&g_uiState, 1); InterlockedExchange(&g_panelDirty, 1);
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
static LRESULT CALLBACK LlKeyboard(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION &&
        InterlockedCompareExchange(&g_uiState, 0, 0) == 2 &&
        InterlockedCompareExchange(&g_lobbyDone, 0, 0) == 0 &&
        InterlockedCompareExchange(&g_showOverlay, 0, 0) != 0 &&
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
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (vk == VK_BACK) { if (g_chatLen > 0) { g_chatInput[--g_chatLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else if (vk == VK_RETURN) { if (g_chatLen > 0) { SendChat(g_chatInput); g_chatInput[0] = 0; g_chatLen = 0; InterlockedExchange(&g_panelDirty, 1); } }
            else { char c = vkToChar((int)vk, shift); if (c && g_chatLen < 190) { g_chatInput[g_chatLen++] = c; g_chatInput[g_chatLen] = 0; InterlockedExchange(&g_panelDirty, 1); } }
        }
        return 1;   // swallow down AND up so no WM_CHAR / keyup binding leaks to the game
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}
static DWORD WINAPI KbHookThread(LPVOID)
{
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LlKeyboard, GetModuleHandleW(nullptr), 0);
    Log("[menu] LL keyboard hook %s\n", g_kbHook ? "installed" : "FAILED");
    MSG msg;   // the LL hook needs a message pump on its installing thread
    while (GetMessage(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}

// The detour: run the original so the page still builds, then flag the in-frame
// button visible on the main page (page 2), hidden elsewhere.
static volatile uint64_t g_menuThis = 0;   // the UI::CMenuUI 'this' -- needed to call StartSavegame

static void MyCreatePage(uint64_t thisp, int page)
{
    g_origCreatePage(thisp, page);
    g_menuThis = thisp;   // captured for the future in-process save-load call (RVA 0x6785c0)
    // The main menu builds pages 0 -> 2 -> 1 (2 is the main content, 0/1 are its
    // sub-layers). Full-screen replacements (Settings/Campaign/Load...) are all
    // page >= 3. So SET on 2, CLEAR only on >= 3; leave 0/1 alone -- otherwise
    // the trailing page=1 hid the overlay on the idle main menu.
    if (page == 2)      InterlockedExchange(&g_showOverlay, 1);
    else if (page >= 3) InterlockedExchange(&g_showOverlay, 0);
    static int seen = 0;
    if (seen < 30) { seen++; Log("[menu] CreatePage page=%d show=%ld\n", page,
        InterlockedCompareExchange(&g_showOverlay, 0, 0)); }
}

static DWORD WINAPI Init(LPVOID)
{
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    resolveSaveDir(g_saveDirW, 600); SAVE_DIR = g_saveDirW;   // discovered, not hardcoded
    resolveNetDir(g_netDirW, 600);   NETDIR   = g_netDirW;
    // the runtime data dir the bridge uses for tpf2_instance.txt / tpf2_bridge_ctl.txt
    if (!Tpf2mpDataDirW(g_dataDirW, MAX_PATH, (const void*)&Init)) wcscpy_s(g_dataDirW, ourDirW());
    InitializeCriticalSection(&g_statusCs); g_csInit = true;
    InitializeCriticalSection(&g_modelCs); g_modelCsInit = true;
    InitializeCriticalSection(&g_lobbyCs); g_lobbyCsInit = true;
    CreateThread(nullptr, 0, KbHookThread, nullptr, 0, nullptr);  // chat keyboard capture/swallow
    Log("[menu] attached, base=%llx  save=%ls  net=%ls  data=%ls  our=%ls\n",
        (unsigned long long)g_base, g_saveDirW, g_netDirW, g_dataDirW, ourDirW());

    void* tramp = nullptr;
    if (!InstallHook(g_base + RVA_CREATEPAGE, (void*)&MyCreatePage,
                     STEAL_CREATEPAGE, &tramp)) {
        Log("[menu] InstallHook FAILED on CreatePage\n");
        return 0;
    }
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
                    if (InstallHook((uintptr_t)gdpaReal, (void*)&myGdpa, 15, &gt)) {
                        g_origGdpa = (PFN_vkGetDeviceProcAddr)gt;
                        Log("[menu] hooked vkGetDeviceProcAddr real=%p steal=15\n", gdpaReal);
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

    g_origCreatePage = (CreatePageFn)tramp;
    Log("[menu] hooked CreatePage rva=%llx steal=%d tramp=%p -- overlay thread started\n",
        (unsigned long long)RVA_CREATEPAGE, STEAL_CREATEPAGE, tramp);

    // ---- A/B test: native-look overlay + real list-inserted button ----
    ReadFlags();
    LoadLato();
    g_strAssign = (StrAssignFn)(g_base + RVA_STR_ASSIGN);
    g_actionCtx = (ActionCtxFn)(g_base + RVA_ACTION_CTX);
    g_btn       = (BtnFn)(g_base + RVA_BTN);
    g_add       = (AddFn)(g_base + RVA_ADD);
    g_clean     = (CleanFn)(g_base + RVA_CLEAN);
    g_setName   = (SetNameFn)(g_base + RVA_SETNAME);
    g_prep      = (PrepFn)(g_base + RVA_PREP);
    if (g_flagNativeBtn) {
        // verify both prologues before patching: a game update moves everything.
        static const unsigned char kMainBuild[14] = { 0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
        static const unsigned char kListAdd[15]   = { 0x40,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff };
        bool okA = memcmp((void*)(g_base + RVA_MAINBUILD), kMainBuild, 14) == 0;
        bool okB = memcmp((void*)(g_base + RVA_LIST_ADD),  kListAdd, 15) == 0;
        if (!okA || !okB) {
            Log("[menu] native: prologue mismatch (mainbuild=%d listadd=%d) -- native button disabled\n", okA, okB);
            g_flagNativeBtn = 0;
        } else {
            void* tA = nullptr; void* tB = nullptr;
            if (InstallHook(g_base + RVA_LIST_ADD, (void*)&MyListAdd, STEAL_LIST_ADD, &tB) &&
                InstallHook(g_base + RVA_MAINBUILD, (void*)&MyMainBuild, STEAL_MAINBUILD, &tA)) {
                g_origListAdd = (ListAddFn)tB; g_origMainBuild = (MainBuildFn)tA;
                Log("[menu] native: hooked list-add %llx (steal %d) and main builder %llx (steal %d)\n",
                    (unsigned long long)RVA_LIST_ADD, STEAL_LIST_ADD, (unsigned long long)RVA_MAINBUILD, STEAL_MAINBUILD);
            } else {
                Log("[menu] native: InstallHook FAILED -- native button disabled\n"); g_flagNativeBtn = 0;
            }
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Orderly shutdown: ask the lobby to quit. Nothing may block here (the
        // loader lock is held), so no waiting and no thread joins -- the job
        // object above is what guarantees the kill if this never runs.
        if (g_lobbyProc) { LobbySend("{\"cmd\":\"quit\"}"); }
    }
    return TRUE;
}
