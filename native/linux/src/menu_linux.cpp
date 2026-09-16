// tpf2_menu.so -- the Linux build of the title menu's Multiplayer entry and
// panel (menu_hook.cpp). The entry is a real Button, built and wired like the
// game's own entries; its click opens the panel (panel_linux.cpp), which is
// drawn into the game's Vulkan frames (overlay_vk_linux.cpp).
//
// Build 35924, Linux. The main-page builder (CreatePageMain(this, UserProfile*,
// const ModRep&, const std::vector<ModId>&)) is 0x113ca70: the one function that
// references "MainMenu", "Load Game", "continue", "list-item" and "Exit". GCC
// builds every button first and wires them afterwards, but each entry is made
// from the same pieces as on Windows (docs/re/GAME_LOOP_AND_UI.md, "Title menu"):
//   0x2fd1420(&label, "Load Game")          std::string tr(const char*), hidden return pointer
//   0x1128b80(&icon, "")                    std::string(const char*)
//   0x13387c0(&label, &iconA, &iconB)       -> Button*
//   0x3028dd0(&conn, button, &fn)           button->clickSignal(+0x450).connect(std::move(fn))
//   0x3190430(&conn)                        ~Connection
//   0x30550d0(widget, &"class")             addStyleClass
//   0x3058b50(button, 4, 0)                 setWidgetFlag: every entry, right before its add
//   0x30de940(list, button, &"list-item")   list-add -- the hook point, as on Windows
// As on Windows, an entry appended after the builder returns never renders, so
// ours goes in from the list-add hook while the builder runs, before the add
// selected by slot=0..7 (default 0).
// UI::CMenuUI::CreatePage(Page) is 0x1154b20; the panel hides on full-screen pages.
//
// std::function<void()> is libstdc++'s 32-byte object: 16 bytes of functor
// storage, _M_manager at +0x10, _M_invoker at +0x18. connect moves it out (the
// source's manager is cleared). Our manager mirrors the one the game's menu
// lambdas use (0x1117040): op 0 type_info, 1 functor address, 2 clone, 3 destroy.
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <typeinfo>
#include "datadir_linux.h"
#include "game_image.h"
#include "hook.h"
#include "panel.h"
#include "menu_game_linux.h"

// ---- sites ------------------------------------------------------------------
static const uintptr_t RVA_MAINBUILD = 0x113ca70;
static const int       STEAL_MAINBUILD = 15;
static const uint8_t   MAINBUILD_EXPECTED[17] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x41, 0x56,                    // push r14
    0x49, 0x89, 0xFE,              // mov  r14, rdi
    0x41, 0x55,                    // push r13
};
static const uintptr_t RVA_LIST_ADD = 0x30de940;
static const int       STEAL_LIST_ADD = 15;
static const uint8_t   LIST_ADD_EXPECTED[16] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x55,                    // push r13
    0x49, 0x89, 0xF5,              // mov  r13, rsi
    0x41, 0x54,                    // push r12
    0x53,                          // push rbx
};
static const uintptr_t RVA_CREATEPAGE = 0x1154b20;
static const int       STEAL_CREATEPAGE = 14;
static const uint8_t   CREATEPAGE_EXPECTED[16] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x41, 0x56,                    // push r14
    0x41, 0x55,                    // push r13
    0x41, 0x54,                    // push r12
};
static const uintptr_t RVA_TR            = 0x2fd1420;
static const uintptr_t RVA_STR_FROM_CSTR = 0x1128b80;
static const uintptr_t RVA_BUTTON_CREATE = 0x13387c0;
static const uintptr_t RVA_CONNECT_CLICK = 0x3028dd0;
static const uintptr_t RVA_CONNECTION_DTOR = 0x3190430;
static const uintptr_t RVA_ADD_STYLE     = 0x30550d0;
static const uintptr_t RVA_SET_FLAG      = 0x3058b50;

// libstdc++ std::__cxx11::string: pointer, length, 16-byte local buffer.
struct GStr { char* p; size_t len; char buf[16]; };
static_assert(sizeof(GStr) == 32, "libstdc++ std::string layout");
// libstdc++ std::function<void()>.
struct GFunc { void* functor[2]; void* manager; void* invoker; };
static_assert(sizeof(GFunc) == 32, "libstdc++ std::function layout");

using TrFn          = void (*)(GStr* out, const char* key);
using StrFromCstrFn = void (*)(GStr* out, const char* s);
using ButtonFn      = void* (*)(GStr* label, GStr* iconA, GStr* iconB);
using ConnectFn     = void* (*)(void* connOut, void* button, GFunc* fn);
using ConnDtorFn    = void (*)(void* conn);
using AddStyleFn    = void (*)(void* widget, GStr* cls);
using SetFlagFn     = void (*)(void* widget, int bit, bool on);
using ListAddFn     = void* (*)(void* list, void* widget, GStr* style);
using MainBuildFn   = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using CreatePageFn  = void (*)(void* menu, int page);

static uintptr_t g_base = 0;
static void* g_mainBuildTramp = nullptr;
static void* g_listAddTramp = nullptr;
static void* g_createPageTramp = nullptr;
static int g_flagSlot = 0;   // published before the builder/list-add hooks

// ---- log --------------------------------------------------------------------
static FILE* g_log = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---- the click slot ---------------------------------------------------------
enum { OpGetTypeInfo = 0, OpGetFunctorPtr = 1, OpCloneFunctor = 2, OpDestroyFunctor = 3 };
struct MultiplayerClick {};

static bool MpManager(void** dest, void* const* source, int op)
{
    switch (op) {
        case OpGetTypeInfo:   *dest = (void*)&typeid(MultiplayerClick); break;
        case OpGetFunctorPtr: *dest = (void*)source; break;
        case OpCloneFunctor:  dest[0] = source[0]; break;   // 8 bytes of nothing, like the game's lambdas
        default: break;                                     // destroy: nothing owned
    }
    return false;
}

static void MpInvoke(const void* /*functor*/)
{
    Log("[menu] Multiplayer clicked: opening the panel\n");
    panel::Open();
}

static void GStrFree(GStr* s)
{
    if (s->p != s->buf) ::operator delete(s->p);
}

// ---- insertion --------------------------------------------------------------
static thread_local bool t_inMainBuild = false;
static thread_local int  t_mainListAdds = 0;
static thread_local bool t_inserted = false;

// Same slot range and default as release 0.4.22's ReadFlags. Read this before
// installing the hooks: panel::Init runs later, after the entry can be built.
static int ReadMenuSlot(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int slot = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* value = eq + 1;
        if (strcmp(line, "slot") || *value < '0' || *value > '9') continue;
        errno = 0;
        const long n = strtol(value, nullptr, 10);
        if (errno != ERANGE && n >= 0 && n <= 7) slot = (int)n;
    }
    fclose(f);
    return slot;
}

static void InsertMultiplayer(void* list)
{
    const auto tr        = (TrFn)(g_base + RVA_TR);
    const auto str       = (StrFromCstrFn)(g_base + RVA_STR_FROM_CSTR);
    const auto button    = (ButtonFn)(g_base + RVA_BUTTON_CREATE);
    const auto connect   = (ConnectFn)(g_base + RVA_CONNECT_CLICK);
    const auto connDtor  = (ConnDtorFn)(g_base + RVA_CONNECTION_DTOR);
    const auto addStyle  = (AddStyleFn)(g_base + RVA_ADD_STYLE);
    const auto setFlag   = (SetFlagFn)(g_base + RVA_SET_FLAG);
    const auto listAdd   = (ListAddFn)g_listAddTramp;

    GStr label, iconA, iconB;
    tr(&label, "Multiplayer");
    str(&iconA, "");
    str(&iconB, "");
    void* btn = button(&label, &iconA, &iconB);
    GStrFree(&label); GStrFree(&iconA); GStrFree(&iconB);
    if (!btn) { Log("[menu] Button::Create returned null -- no entry\n"); return; }

    GFunc fn = {};
    fn.manager = (void*)&MpManager;
    fn.invoker = (void*)&MpInvoke;
    void* conn[2] = { nullptr, nullptr };
    connect(conn, btn, &fn);
    connDtor(conn);
    if (fn.manager) ((bool (*)(GFunc*, GFunc*, int))fn.manager)(&fn, &fn, OpDestroyFunctor);

    GStr cls;
    str(&cls, "multiplayer");
    addStyle(btn, &cls);
    GStrFree(&cls);
    setFlag(btn, 4, false);

    GStr item;
    str(&item, "list-item");
    listAdd(list, btn, &item);
    GStrFree(&item);
    Log("[menu] Multiplayer entry added to the MainMenu list %p (button %p)\n", list, btn);
}

static void* ListAddDetour(void* list, void* widget, GStr* style)
{
    if (t_inMainBuild && !t_inserted && t_mainListAdds == g_flagSlot) {
        t_inserted = true;
        InsertMultiplayer(list);
    }
    void* r = ((ListAddFn)g_listAddTramp)(list, widget, style);
    if (t_inMainBuild) t_mainListAdds++;
    return r;
}

static uintptr_t MainBuildDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    t_inMainBuild = true;
    t_mainListAdds = 0;
    t_inserted = false;
    const uintptr_t r = ((MainBuildFn)g_mainBuildTramp)(a, b, c, d, e, f);
    t_inMainBuild = false;
    Log("[menu] main page built: %d list adds, Multiplayer %s\n", t_mainListAdds,
        t_inserted ? "inserted" : "NOT inserted (no list add ran during the build)");
    return r;
}

static void CreatePageDetour(void* menu, int page)
{
    MenuGame_ObserveMenu(menu);
    ((CreatePageFn)g_createPageTramp)(menu, page);
    panel::OnMenuPage(page);
}

// ---- install ----------------------------------------------------------------
static bool Prologue(uintptr_t rva, const uint8_t* expected, size_t n, int steal)
{
    return memcmp((void*)(g_base + rva), expected, n) == 0
        && PrologueSteal((const unsigned char*)(g_base + rva), 14) == steal;
}

static std::string DirOf(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? std::string(path, (size_t)(slash - path) + 1) : std::string("./");
}

static void Init()
{
    char dataDir[4096] = "";
    Tpf2mpDataDirA(dataDir, sizeof(dataDir));
    const Tpf2GameImage img = Tpf2mpGameImage();
    Log("[menu] attached to pid %d, base %lx\n", (int)getpid(), (unsigned long)img.base);
    if (!img.buildOk) {
        Log("[menu] the game is not build 35924 (GNU build-id differs) -- no Multiplayer entry\n");
        return;
    }
    g_base = img.base;

    const char* noPatches = getenv("TPF2MP_NO_PATCHES");
    if (noPatches && noPatches[0] == '1') {
        Log("[menu] TPF2MP_NO_PATCHES=1 -- menu and game hooks skipped\n");
        return;
    }

    Dl_info self = {};
    dladdr((void*)&Init, &self);
    const std::string libDir = DirOf(self.dli_fname ? self.dli_fname : "");
    g_flagSlot = ReadMenuSlot((libDir + "tpf2_menu_flags.txt").c_str());
    Log("[menu] flags: slot=%d\n", g_flagSlot);

    // First the overlay: the game builds its Vulkan device a few seconds in,
    // and the redirect has to be in place before that call runs.
    OverlayInstall(g_base, Log);
    MenuGame_Install(g_base, Log);

    if (!Prologue(RVA_CREATEPAGE, CREATEPAGE_EXPECTED, sizeof(CREATEPAGE_EXPECTED), STEAL_CREATEPAGE) ||
        !InstallHook(g_base + RVA_CREATEPAGE, (void*)&CreatePageDetour, STEAL_CREATEPAGE, &g_createPageTramp))
        Log("[menu] CreatePage not hooked -- the panel stays up on every menu page\n");

    const bool okBuild = Prologue(RVA_MAINBUILD, MAINBUILD_EXPECTED, sizeof(MAINBUILD_EXPECTED), STEAL_MAINBUILD);
    const bool okAdd   = Prologue(RVA_LIST_ADD, LIST_ADD_EXPECTED, sizeof(LIST_ADD_EXPECTED), STEAL_LIST_ADD);
    if (!okBuild || !okAdd) {
        Log("[menu] prologue mismatch (builder %d, list-add %d) -- no Multiplayer entry\n", okBuild, okAdd);
    } else if (!InstallHook(g_base + RVA_MAINBUILD, (void*)&MainBuildDetour, STEAL_MAINBUILD, &g_mainBuildTramp)) {
        // The builder first: harmless on its own. The list-add hook runs on the UI
        // thread the moment it is written; its trampoline is published before the patch.
        Log("[menu] InstallHook FAILED on the main-page builder -- no Multiplayer entry\n");
    } else if (!InstallHook(g_base + RVA_LIST_ADD, (void*)&ListAddDetour, STEAL_LIST_ADD, &g_listAddTramp)) {
        Log("[menu] InstallHook FAILED on list-add -- no Multiplayer entry (the builder hook only counts)\n");
    } else {
        Log("[menu] hooked the main-page builder (%lx) and list-add (%lx): the Multiplayer entry goes before entry %d\n",
            (unsigned long)RVA_MAINBUILD, (unsigned long)RVA_LIST_ADD, g_flagSlot);
    }

    // Fonts and names last: the Noto fallback is 16 MB, and nothing waits on it
    // until the entry is clicked.
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = 0;
    panel::Init(dataDir, DirOf(exe).c_str(), libDir.c_str(), Log);
}

// The builder runs long after main() starts; hooking before the menu exists is
// the whole point of loading this early. A thread keeps the game's startup from
// waiting on our file and memory work.
static void* InitEntry(void*)
{
    try {
        Init();
    } catch (...) {
        Log("[menu] initialization failed with an exception\n");
    }
    return nullptr;
}

__attribute__((constructor))
static void MenuLoad()
{
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const char* name = strrchr(exe, '/');
    if (!name || strcmp(name + 1, "TransportFever2") != 0) return;

    char dataDir[4096] = "";
    if (Tpf2mpDataDirA(dataDir, sizeof(dataDir))) {
        char path[8192];
        snprintf(path, sizeof(path), "%stpf2_menu.log", dataDir);
        g_log = fopen(path, "ab");
    }
    // The game reads settings.lua after main starts; update activeMods before it.
    const char* noPatches = getenv("TPF2MP_NO_PATCHES");
    if (!(noPatches && noPatches[0] == '1') && Tpf2mpGameImage().buildOk)
        MenuGame_AutoEnableMod(Log);
    pthread_t thread;
    const int err = pthread_create(&thread, nullptr, InitEntry, nullptr);
    if (err == 0) pthread_detach(thread);
    else Log("[menu] could not start initialization thread: %s\n", strerror(err));
}
