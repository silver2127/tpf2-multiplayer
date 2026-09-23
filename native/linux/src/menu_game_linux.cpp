// menu_game_linux.cpp -- menu_game_linux.h: the save folder, AUTO-LOAD, HOT
// JOIN's forced autosave and the automod edit, on Linux. menu_hook.cpp has the
// rules and the incidents behind them; the ids in brackets are the rows of
// docs/re/linux/MENU_GAME.md that prove each site.
//
// THE SAVE FOLDER [SAVE-01..04] is found on disk, never in the game's memory:
//   1. TPF2MP_USERDATA, when set to an existing .../1066780/local folder;
//   2. the game's own log, <local>/crash_dump/stdout.txt (Run 0x9b3d50 joins
//      "crash_dump/" at 0x9b4ac1 and "stdout.txt" at 0x9b4c90 onto the folder
//      it logs as "user data folder: "), when /proc/self/fd shows it open. That
//      the file stays open is only observed, so a miss falls through;
//   3. every <root>/userdata/<id>/1066780/local under the Steam roots
//      ($HOME/.steam/steam and root, $XDG_DATA_HOME/Steam,
//      $HOME/.local/share/Steam, Flatpak's, the Snap's, and the root the game
//      runs from). Code before main() cannot ask Steam which account plays, so
//      with several it takes the one whose settings.lua was written last: the
//      game rewrites that file at every exit [AM-04].
// A save is <name>.sav, <name>.sav.lua and <name>.jpg [SAVE-05]. The game reads
// the .sav.lua only when there is one [SAVE-06], so a stale one is removed, never
// left beside a different .sav. Copies go to temporary names first and are
// renamed together at the end.
//
// THE FRAME GATE [AL-03, AL-04, AL-19]. CMenuUI's and CGameUI's per-frame
// updates are vtable slot 34, dispatched by UI::CComponent::Step2 0x305a1c0,
// which only Step 0x305a830 enters. Step has three callers: the frame step in
// 0x30803d0 (0x30822e6), UI builders through 0x312af90 with t = dt = 0
// (0x312afb2), and the step-listener lambda 0x112e8a0 with the frame's own t/dt
// (0x112e948). All three calls are redirected (Tpf2mpRedirectCall checks the e8
// and its target) to wrappers that mark their frames per thread. The detours
// act only below exactly one live frame Step and no live nested one; every
// other call goes straight on to the game. Without all three redirects no slot
// is swapped.
//
// THE HOOKS [AL-01, AL-02, AL-05, HJ-01, HJ-02]. RTTI N2UI7CMenuUIE -> typeinfo
// 0x5a16e68 -> its only vtable 0x5a16f98, slot 34 (0x5a170a8) = 0x1140a90;
// N2UI7CGameUIE -> 0x5a11248 -> 0x5a11468, slot 34 (0x5a11578) = 0x100fb20.
// Nothing derives from either class. The slots are in PT_GNU_RELRO: a swap is
// mprotect(+W), one 8-byte store, mprotect back, after checking slot -1, the
// typeinfo's name, the slot's value and the target's first 23 bytes. Both
// detours are void(void* self, int64 t, int64 dt), the registers Step2 passes
// (0x305a7a8..0x305a7b1); both updates leave the xmm registers alone or write
// them before reading.
//
// AUTO-LOAD [AL-06..18]. After the original CMenuUI update, on a gated frame
// with a request pending: a game running (+0x4c8) turns the request into "open
// LOAD GAME and pick <name>"; initialisation (+0x5e8) or a queued load (+0x5f8)
// waits; otherwise the Missions page's own sequence [AL-15] for {path "", name,
// namespace "savegame"}: LoadGameParams() 0x1162170, the SavegameInfo getter
// 0xc7e520(out, *(app 0x18ba890 + 0xc8), &id), ~SaveGameId 0xf31740,
// StartSavegame 0x113f450, ~SavegameInfo 0xc84f00, ~LoadGameParams 0x101da10.
// Strings are built by the game's own std::string(const char*) 0x1128b80, and
// the layout the params ctor leaves is checked before anything is written.
//
// HOT JOIN [HJ-03..07]. The game autosaves once CGameUI+0x628 (int64 us) passes
// autosaveIntervalMinutes (GlobalSettings+0x120) * 60 s. On a gated frame with
// a request pending, and only while +0x597 (the update skips the block while it
// is set) and +0xae8 (IsCurrentlySaving: AutoSave asserts it is clear) are 0
// and the interval is > 0, the accumulator holds 2^62 for exactly one call of
// the original update and gets its old value back if the block did not use it.
// Afterwards, +0xae8 set means the save started. Windows wrote the value from
// another thread and left it there; here a manual save cannot cancel it
// silently, and it never fires later at a moment nobody chose.
// panel::OnGameUiFrame is signalled from every call on the CGameUI that
// g_gameUI (.bss 0x5a4fb38) names.
//
// AUTOMOD [AM-01..06]. activeMods in <local>/settings.lua gets
// { "mp_lockstep", <n>, } for the mp_lockstep_<n> folder found in <game>/mods
// or <local>/mods, before the game parses the file (Run2 -> 0xc3ea70).
//
// EXCEPTIONS. This library links libstdc++ and libgcc statically; the game
// throws with libstdc++.so.6 and unwinds with libgcc_s.so.1. For a frame of ours
// with an LSDA (a try/catch, or a destructor to run), that unwinder calls our
// static __gxx_personality_v0, whose static _Unwind_SetGR aborts on a context it
// did not build (measured off-game: abort in _Unwind_SetGR.cold). So:
//   - the wrappers and detours between game frames (marked GAME PATH) have no
//     try/catch and no destructors. A game exception passes straight through
//     them, and what it leaves behind is put right later from per-thread
//     records (the gate's frame marks, the accumulator's old value);
//   - the load runs inside tpf2mp_mg_guarded, a catch(...) in assembly whose
//     personality, __cxa_begin_catch and __cxa_end_catch are the game's own
//     (dlsym): it catches with the game's runtime, so unwinder and counters
//     agree. The load needs one: the getter throws std::runtime_error [AL-11];
//     the game's Missions page catches std::exception around that call (LSDA
//     of 0x113ffa0), but nothing on the frame path above the updates does (Step
//     0x305a830 and Run2 catch only the game's own Exception, Run adds
//     bad_alloc), so an escape would end the game in std::terminate;
//   - our own code that can throw (strings, the panel) runs in noinline helpers
//     that catch our own exceptions themselves.
//
// Process-lifetime state is leaked on purpose: static destructors run while the
// game's threads still call in.
#include "menu_game_linux.h"
#include "datadir_linux.h"
#include "near_alloc.h"
#include "hook.h"
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

// ---- sites: build 35924, Linux (docs/re/linux/MENU_GAME.md) ---------------------------------------
static const uintptr_t RVA_STEP          = 0x305a830;   // UI::CComponent::Step(this, int64 t, int64 dt)
static const uintptr_t RVA_SITE_FRAME    = 0x30822e6;   // in 0x30803d0, the frame loop's step
static const uintptr_t RVA_SITE_ZERO     = 0x312afb2;   // in 0x312af90: Step(c, 0, 0) for UI builders
static const uintptr_t RVA_SITE_LISTENER = 0x112e948;   // in the step-listener lambda 0x112e8a0

static const uintptr_t RVA_MENUUI_TYPEINFO = 0x5a16e68;
static const uintptr_t RVA_MENUUI_TYPENAME = 0x41840a8;   // "N2UI7CMenuUIE"
static const uintptr_t RVA_MENUUI_VTABLE   = 0x5a16f98;   // slot 0
static const uintptr_t RVA_MENUUI_UPDATE   = 0x1140a90;
static const uintptr_t RVA_GAMEUI_TYPEINFO = 0x5a11248;
static const uintptr_t RVA_GAMEUI_TYPENAME = 0x410fa98;   // "N2UI7CGameUIE"
static const uintptr_t RVA_GAMEUI_VTABLE   = 0x5a11468;
static const uintptr_t RVA_GAMEUI_UPDATE   = 0x100fb20;
static const int       SLOT_UPDATE         = 34;
// Both updates open the same way.
static const uint8_t UPDATE_PROLOGUE[23] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x41, 0x56,                    // push r14
    0x49, 0x89, 0xD6,              // mov  r14, rdx
    0x41, 0x55,                    // push r13
    0x41, 0x54,                    // push r12
    0x53,                          // push rbx
    0x48, 0x89, 0xFB,              // mov  rbx, rdi
};

static const uintptr_t RVA_PARAMS_CTOR     = 0x1162170;   // LoadGameParams::LoadGameParams(), 0x130 bytes
static const uintptr_t RVA_PARAMS_DTOR     = 0x101da10;
static const uintptr_t RVA_STR_FROM_CSTR   = 0x1128b80;   // std::string(const char*)
static const uintptr_t RVA_APP             = 0x18ba890;   // returns *(.bss 0x5a52d80)
static const uintptr_t RVA_SAVEINFO_GET    = 0xc7e520;    // SavegameInfo* (out, manager, const SaveGameId*)
static const uintptr_t RVA_SAVEINFO_DTOR   = 0xc84f00;    // SavegameInfo: 0x108 bytes
static const uintptr_t RVA_SAVEGAMEID_DTOR = 0xf31740;    // SaveGameId: 0x60 bytes, three strings
static const uintptr_t RVA_START_SAVEGAME  = 0x113f450;   // bool CMenuUI::StartSavegame(const LoadGameParams&, const SavegameInfo&)
static const size_t    OFF_APP_SAVEMGR     = 0xc8;
static const size_t    MENU_OFF_GAMEUI     = 0x4c8;       // CGameUI* while a game runs
static const size_t    MENU_OFF_INITING    = 0x5e8;       // "Game initialization is already active!"
static const size_t    MENU_OFF_QUEUED     = 0x5f8;       // a queued load

static const uintptr_t RVA_GLOBALSETTINGS  = 0x5a4f748;   // .bss pointer that GetGlobalSettings 0xc1a300 returns
static const uintptr_t RVA_G_GAMEUI        = 0x5a4fb38;   // .bss g_gameUI: Set/GetGlobalGameUI
static const size_t    GS_OFF_AUTOSAVE_MIN = 0x120;       // int32 autosaveIntervalMinutes
static const size_t    GAMEUI_OFF_SKIP     = 0x597;       // byte: the update returns before the autosave block
static const size_t    GAMEUI_OFF_ACC      = 0x628;       // int64 us towards the next autosave
static const size_t    GAMEUI_OFF_SAVING   = 0xae8;       // byte: IsCurrentlySaving()
// Above any interval * 60e6 (at most 0x7fffffff * 60e6, about 1.29e17) and far
// from overflowing when the update adds dt (INT64_MAX + dt would wrap negative).
static const int64_t   FORCE_ACC           = (int64_t)1 << 62;

// Every other instruction this file relies on, as build 35924 has it. A group
// whose bytes differ stays off.
enum : uint8_t { G_GATE = 1, G_AUTOLOAD = 2, G_HOTJOIN = 4 };
struct Expect {
    uintptr_t rva;
    uint8_t groups;
    uint8_t n;
    uint8_t bytes[24];
    const char* what;
};
static const Expect EXPECTS[] = {
    { 0x305a830, G_GATE, 22, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x55, 0x49, 0x89, 0xD5,
                               0x41, 0x54, 0x49, 0x89, 0xF4, 0x53, 0x48, 0x89, 0xFB },
      "Step: reads rdi, rsi, rdx" },
    // the calls of the load
    { 0x1162170, G_AUTOLOAD, 16, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x41, 0x55, 0x4C, 0x8D, 0x6F, 0x10 },
      "LoadGameParams()" },
    { 0x101da10, G_AUTOLOAD, 16, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x89, 0xFB },
      "~LoadGameParams" },
    { 0xc7e520, G_AUTOLOAD, 18, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x49, 0x89, 0xF7, 0x41, 0x56, 0x49, 0x89, 0xD6 },
      "SavegameInfo getter" },
    { 0xc84f00, G_AUTOLOAD, 16, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x89, 0xFB },
      "~SavegameInfo" },
    { 0xf31740, G_AUTOLOAD, 16, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x89, 0xFB, 0x48, 0x8D, 0x43, 0x50 },
      "~SaveGameId: lea rax, [rbx+50h]" },
    { 0x113f450, G_AUTOLOAD, 16, { 0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54 },
      "StartSavegame" },
    { 0x113f47e, G_AUTOLOAD, 7, { 0x0F, 0xB6, 0x87, 0xE8, 0x05, 0x00, 0x00 },
      "StartSavegame: movzx eax, byte [rdi+5e8h]" },
    { 0x18ba890, G_AUTOLOAD, 12, { 0xF3, 0x0F, 0x1E, 0xFA, 0x48, 0x8B, 0x05, 0xE5, 0x84, 0x19, 0x04, 0xC3 },
      "app accessor: mov rax, [.bss 5a52d80h]; ret" },
    { 0x1128b80, G_AUTOLOAD, 15, { 0x55, 0x48, 0x8D, 0x47, 0x10, 0x48, 0xC7, 0xC2, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0xE5 },
      "std::string(const char*)" },
    { 0x1140353, G_AUTOLOAD, 7, { 0x48, 0x8B, 0xB0, 0xC8, 0x00, 0x00, 0x00 },
      "Missions lambda: mov rsi, [rax+0c8h] (save manager)" },
    // the CMenuUI fields the tick reads
    { 0x1140c50, G_AUTOLOAD, 7, { 0x4C, 0x8B, 0xB3, 0xC8, 0x04, 0x00, 0x00 },
      "CMenuUI update: mov r14, [rbx+4c8h]" },
    { 0x1140cf8, G_AUTOLOAD, 7, { 0x0F, 0xB6, 0x83, 0xE8, 0x05, 0x00, 0x00 },
      "CMenuUI update: movzx eax, byte [rbx+5e8h]" },
    { 0x1140b9b, G_AUTOLOAD, 7, { 0x48, 0x8B, 0xBB, 0xF8, 0x05, 0x00, 0x00 },
      "CMenuUI update: mov rdi, [rbx+5f8h]" },
    // the autosave block and its fields
    { 0x100fc70, G_HOTJOIN, 13, { 0x80, 0xBB, 0x97, 0x05, 0x00, 0x00, 0x00, 0x0F, 0x85, 0xA3, 0x05, 0x00, 0x00 },
      "CGameUI update: cmp byte [rbx+597h], 0; jne" },
    { 0x10101ac, G_HOTJOIN, 14, { 0x8B, 0x80, 0x20, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x0F, 0x8F, 0xE6, 0x00, 0x00, 0x00 },
      "CGameUI update: interval > 0" },
    { 0x1010358, G_HOTJOIN, 10, { 0x48, 0x8B, 0x93, 0x28, 0x06, 0x00, 0x00, 0x49, 0x01, 0xD6 },
      "CGameUI update: accumulator += dt" },
    { 0x10102c4, G_HOTJOIN, 21, { 0x48, 0x63, 0x80, 0x20, 0x01, 0x00, 0x00, 0x48, 0x69, 0xC0, 0x00, 0x87, 0x93, 0x03,
                                  0x48, 0x39, 0x83, 0x28, 0x06, 0x00, 0x00 },
      "CGameUI update: accumulator > interval * 60e6" },
    { 0x10102e2, G_HOTJOIN, 16, { 0xE8, 0x79, 0xB2, 0xFD, 0xFF, 0x48, 0xC7, 0x83, 0x28, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
      "CGameUI update: call AutoSave; accumulator = 0" },
    { 0xfeb79c, G_HOTJOIN, 7, { 0x80, 0xBB, 0xE8, 0x0A, 0x00, 0x00, 0x00 },
      "AutoSave: cmp byte [rbx+0ae8h], 0 (asserts)" },
    { 0xfeb7ad, G_HOTJOIN, 7, { 0xC6, 0x83, 0xE8, 0x0A, 0x00, 0x00, 0x01 },
      "AutoSave: mov byte [rbx+0ae8h], 1" },
    { 0xc1a304, G_HOTJOIN, 7, { 0x48, 0x8B, 0x05, 0x3D, 0x54, 0xE3, 0x04 },
      "GetGlobalSettings: mov rax, [.bss 5a4f748h]" },
    { 0xfe29e9, G_HOTJOIN, 7, { 0x48, 0x89, 0x3D, 0x48, 0xD1, 0xA6, 0x04 },
      "SetGlobalGameUI: mov [.bss 5a4fb38h], rdi" },
    { 0xfe2a35, G_HOTJOIN, 8, { 0x48, 0x0F, 0x45, 0x05, 0xFB, 0xD0, 0xA6, 0x04 },
      "GetGlobalGameUI: cmovne rax, [.bss 5a4fb38h]" },
};

// libstdc++ std::__cxx11::string: pointer, length, 16-byte local buffer.
struct GStr { char* p; size_t len; char buf[16]; };
static_assert(sizeof(GStr) == 32, "libstdc++ std::string layout");

static const char kSharedName[] = "mp_shared";

// ---- small things ---------------------------------------------------------------------------------
static std::atomic<Tpf2mpLogFn> g_log{nullptr};

// No destructors, no catch: safe between game frames.
__attribute__((format(printf, 1, 2)))
static void Log(const char* fmt, ...)
{
    const Tpf2mpLogFn fn = g_log.load(std::memory_order_relaxed);
    if (!fn) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fn("%s", line);
}

static uint64_t NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// For the few fields both a game frame and the lobby touch: never throws, and
// never held across a call into the game.
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() noexcept { while (flag.test_and_set(std::memory_order_acquire)) sched_yield(); }
    void unlock() noexcept { flag.clear(std::memory_order_release); }
};

static bool IsDir(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool IsFile(const std::string& p, struct stat* out)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (out) *out = st;
    return true;
}

static int64_t MtimeNs(const struct stat& st)
{
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

static bool EndsWith(const std::string& s, const char* tail)
{
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

static bool AllDigits(const char* s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

// The game's folder (TransportFever2's), without a trailing slash.
static std::string ExeDir()
{
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return std::string();
    exe[n] = 0;
    const char* slash = strrchr(exe, '/');
    return slash ? std::string(exe, (size_t)(slash - exe)) : std::string();
}

// Where tpf2_menu.so lives, with a trailing slash (its flags file).
static std::string OurDir()
{
    Dl_info info = {};
    if (!dladdr((void*)&OurDir, &info) || !info.dli_fname) return "./";
    const char* slash = strrchr(info.dli_fname, '/');
    return slash ? std::string(info.dli_fname, (size_t)(slash - info.dli_fname) + 1) : std::string("./");
}

// src -> dst (created or truncated) and synced; on failure dst is removed and
// *err holds the errno.
static bool CopyFileSynced(const std::string& src, const std::string& dst, int* err)
{
    *err = 0;
    const int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) { *err = errno; return false; }
    const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) { *err = errno; close(in); return false; }
    static const size_t kBuf = 1 << 20;
    char* buf = (char*)malloc(kBuf);
    bool ok = buf != nullptr;
    if (!ok) *err = ENOMEM;
    while (ok) {
        const ssize_t n = read(in, buf, kBuf);
        if (n < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            ok = false;
            break;
        }
        if (n == 0) break;
        for (ssize_t off = 0; off < n;) {
            const ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                *err = errno;
                ok = false;
                break;
            }
            off += w;
        }
    }
    free(buf);
    if (ok && fsync(out) != 0) { *err = errno; ok = false; }
    if (close(out) != 0 && ok) { *err = errno; ok = false; }
    close(in);
    if (!ok) unlink(dst.c_str());
    return ok;
}

static void StampNow(const std::string& path)
{
    const timespec now[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
    utimensat(AT_FDCWD, path.c_str(), now, 0);
}

// ---- the Steam user data folder [SAVE-01..04] -----------------------------------------------------
static const char kLocalTail[] = "/1066780/local";

// Every <root>/userdata/<digits>/1066780/local.
static void AccountsUnder(const std::string& root, std::vector<std::string>* out)
{
    const std::string userdata = root + "/userdata";
    DIR* d = opendir(userdata.c_str());
    if (!d) return;
    while (const dirent* e = readdir(d)) {
        if (!AllDigits(e->d_name)) continue;
        const std::string local = userdata + "/" + e->d_name + kLocalTail;
        if (IsDir(local)) out->push_back(local);
    }
    closedir(d);
}

// ".../userdata/<digits>/1066780/local/crash_dump/stdout.txt" -> ".../userdata/<digits>/1066780/local"
static bool LocalFromLogPath(const char* link, std::string* local)
{
    static const char kLog[] = "/crash_dump/stdout.txt";
    std::string s = link;
    if (!EndsWith(s, kLog)) return false;
    s.resize(s.size() - (sizeof(kLog) - 1));
    if (!EndsWith(s, kLocalTail)) return false;
    std::string head = s.substr(0, s.size() - (sizeof(kLocalTail) - 1));   // .../userdata/<digits>
    const size_t slash = head.rfind('/');
    if (slash == std::string::npos || !AllDigits(head.c_str() + slash + 1)) return false;
    head.resize(slash);
    if (!EndsWith(head, "/userdata") || !IsDir(s)) return false;
    *local = s;
    return true;
}

static bool LocalFromOpenLog(std::string* local)
{
    DIR* d = opendir("/proc/self/fd");
    if (!d) return false;
    bool found = false;
    char link[4096];
    std::string path;
    while (!found) {
        const dirent* e = readdir(d);
        if (!e) break;
        if (!AllDigits(e->d_name)) continue;
        path = "/proc/self/fd/";
        path += e->d_name;
        const ssize_t n = readlink(path.c_str(), link, sizeof(link) - 1);
        if (n <= 0) continue;
        link[n] = 0;
        found = LocalFromLogPath(link, local);
    }
    closedir(d);
    return found;
}

static void SteamRoots(std::vector<std::string>* roots)
{
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_DATA_HOME");
    const bool haveHome = home && home[0] == '/';
    if (haveHome) {
        roots->push_back(std::string(home) + "/.steam/steam");
        roots->push_back(std::string(home) + "/.steam/root");
    }
    if (xdg && xdg[0] == '/') roots->push_back(std::string(xdg) + "/Steam");
    if (haveHome) {
        roots->push_back(std::string(home) + "/.local/share/Steam");
        roots->push_back(std::string(home) + "/.var/app/com.valvesoftware.Steam/.local/share/Steam");
        roots->push_back(std::string(home) + "/snap/steam/common/.local/share/Steam");
    }
    const std::string exe = ExeDir();   // <root>/steamapps/common/<game>
    const size_t at = exe.rfind("/steamapps/common/");
    if (at != std::string::npos) roots->push_back(exe.substr(0, at));
}

struct DirState {
    std::mutex mtx;
    std::string fromLog;   // settled once the game's open log names it
    std::string last;      // the last answer, so that only a change is logged
    bool logged = false;
};
static DirState& DS()
{
    static DirState* s = new DirState;
    return *s;
}

// <steam>/userdata/<account>/1066780/local, or empty.
static std::string UserDataLocal()
{
    DirState& ds = DS();
    std::lock_guard<std::mutex> lk(ds.mtx);
    std::string local;
    const char* rule = "";
    size_t accounts = 0;
    const char* env = getenv("TPF2MP_USERDATA");
    if (env && env[0] == '/' && IsDir(env)) {
        local = env;
        while (local.size() > 1 && local.back() == '/') local.pop_back();
        rule = "TPF2MP_USERDATA";
    } else if (!ds.fromLog.empty() || LocalFromOpenLog(&ds.fromLog)) {
        local = ds.fromLog;
        rule = "the game's open crash_dump/stdout.txt";
    } else {
        std::vector<std::string> roots, seen, found;
        SteamRoots(&roots);
        for (const std::string& r : roots) {
            char* real = realpath(r.c_str(), nullptr);
            if (!real) continue;
            const std::string rr = real;
            free(real);
            if (std::find(seen.begin(), seen.end(), rr) != seen.end()) continue;
            seen.push_back(rr);
            AccountsUnder(rr, &found);
        }
        accounts = found.size();
        if (found.size() == 1) {
            local = found[0];
            rule = "the only Steam account with Transport Fever 2 data";
        } else if (found.size() > 1) {
            bool bestHas = false;
            int64_t bestT = -1;
            for (const std::string& a : found) {
                struct stat st;
                const bool has = IsFile(a + "/settings.lua", &st);
                if (!has && stat(a.c_str(), &st) != 0) continue;
                const int64_t t = MtimeNs(st);
                if ((has && !bestHas) || (has == bestHas && t > bestT)) {
                    local = a;
                    bestHas = has;
                    bestT = t;
                }
            }
            rule = "of several Steam accounts, the one whose settings.lua was written last";
        }
    }
    if (!ds.logged || local != ds.last) {
        ds.logged = true;
        ds.last = local;
        if (local.empty())
            Log("[menugame] no Steam user data folder for Transport Fever 2 (userdata/<id>/1066780/local) found\n");
        else if (accounts > 1)
            Log("[menugame] user data folder %s (%s; %zu accounts have one)\n", local.c_str(), rule, accounts);
        else
            Log("[menugame] user data folder %s (%s)\n", local.c_str(), rule);
    }
    return local;
}

// ---- saves [SAVE-05..07] ---------------------------------------------------------------------------
std::string MenuGame_SaveDir()
{
    const std::string local = UserDataLocal();
    return local.empty() ? std::string() : local + "/save";
}

bool MenuGame_NewestSave(std::string* path)
{
    const std::string dir = MenuGame_SaveDir();
    if (dir.empty()) return false;
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    const std::string shared = std::string(kSharedName) + ".sav";
    std::string best;
    int64_t bestT = -1;
    bool haveShared = false;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() <= 4 || !EndsWith(name, ".sav")) continue;
        struct stat st;
        if (!IsFile(dir + "/" + name, &st)) continue;
        if (name == shared) { haveShared = true; continue; }
        const int64_t t = MtimeNs(st);
        if (t > bestT || (t == bestT && name > best)) {
            bestT = t;
            best = name;
        }
    }
    closedir(d);
    if (best.empty() && haveShared) best = shared;   // nothing else: our own copy
    if (best.empty()) return false;
    if (path) *path = dir + "/" + best;
    return true;
}

// A companion copied to `tmp` (or not) takes the place of `dst`; a companion
// the source lacks leaves no stale `dst` behind.
static bool CommitCompanion(bool copied, const std::string& tmp, const std::string& dst)
{
    if (copied && rename(tmp.c_str(), dst.c_str()) == 0) {
        StampNow(dst);
        return true;
    }
    if (copied) unlink(tmp.c_str());
    unlink(dst.c_str());
    return false;
}

bool MenuGame_PlaceSharedSave(const std::string& src, std::string* placedName)
{
    if (placedName) placedName->clear();
    const std::string dir = MenuGame_SaveDir();
    if (dir.empty()) {
        Log("[saves] no save folder -- %s not placed\n", src.c_str());
        return false;
    }
    struct stat srcSt;
    if (src.size() <= 4 || !EndsWith(src, ".sav") || !IsFile(src, &srcSt) || srcSt.st_size <= 0) {
        Log("[saves] %s is not a non-empty .sav file -- not placed\n", src.c_str());
        return false;
    }
    if (!IsDir(dir) && mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
        Log("[saves] cannot create %s: %s -- not placed\n", dir.c_str(), strerror(errno));
        return false;
    }
    const std::string base = dir + "/" + kSharedName;
    const std::string dstSav = base + ".sav", dstLua = base + ".sav.lua", dstJpg = base + ".jpg";
    const std::string srcLua = src + ".lua";
    const std::string srcJpg = src.substr(0, src.size() - 4) + ".jpg";

    struct stat st;
    if (IsFile(dstSav, &st) && st.st_dev == srcSt.st_dev && st.st_ino == srcSt.st_ino) {
        // the host shares mp_shared.sav itself: already in place, only made newest
        if (IsFile(dstLua, nullptr)) StampNow(dstLua);
        if (IsFile(dstJpg, nullptr)) StampNow(dstJpg);
        StampNow(dstSav);
        Log("[saves] %s is the shared save already (stamped newest)\n", dstSav.c_str());
        if (placedName) *placedName = kSharedName;
        return true;
    }

    // The slow part to temporary names; the folder changes only in the renames.
    static const char kTmp[] = ".mptmp";
    int err = 0;
    if (!CopyFileSynced(src, dstSav + kTmp, &err)) {
        Log("[saves] copying %s to %s failed: %s -- not placed\n", src.c_str(), (dstSav + kTmp).c_str(), strerror(err));
        return false;
    }
    const bool haveLua = IsFile(srcLua, nullptr), haveJpg = IsFile(srcJpg, nullptr);
    const bool luaCopied = haveLua && CopyFileSynced(srcLua, dstLua + kTmp, &err);
    if (haveLua && !luaCopied)
        Log("[saves] copying %s failed: %s -- the save goes without its script state\n", srcLua.c_str(), strerror(err));
    const bool jpgCopied = haveJpg && CopyFileSynced(srcJpg, dstJpg + kTmp, &err);
    if (haveJpg && !jpgCopied)
        Log("[saves] copying %s failed: %s -- the save goes without its screenshot\n", srcJpg.c_str(), strerror(err));

    // The old companions go first: in between, the game finds a .sav with no
    // .sav.lua (allowed [SAVE-06]), never one with another save's.
    unlink(dstLua.c_str());
    unlink(dstJpg.c_str());
    if (rename((dstSav + kTmp).c_str(), dstSav.c_str()) != 0) {
        err = errno;
        unlink((dstSav + kTmp).c_str());
        if (luaCopied) unlink((dstLua + kTmp).c_str());
        if (jpgCopied) unlink((dstJpg + kTmp).c_str());
        Log("[saves] renaming into %s failed: %s -- not placed\n", dstSav.c_str(), strerror(err));
        return false;
    }
    const bool luaOk = CommitCompanion(luaCopied, dstLua + kTmp, dstLua);
    const bool jpgOk = CommitCompanion(jpgCopied, dstJpg + kTmp, dstJpg);
    StampNow(dstSav);
    Log("[saves] placed %s as %s (script state %s, screenshot %s; stamped newest)\n", src.c_str(), dstSav.c_str(),
        luaOk ? "copied" : haveLua ? "FAILED, removed" : "none", jpgOk ? "copied" : haveJpg ? "failed, removed" : "none");
    if (placedName) *placedName = kSharedName;
    return true;
}

// ---- automod [AM-01..06] -------------------------------------------------------------------------
// tpf2_menu_flags.txt as panel_linux.cpp reads it: key=value, trailing blanks dropped.
static bool FlagIs(const std::string& path, const char* key, const char* value)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    const size_t klen = strlen(key);
    bool hit = false;
    while (fgets(line, sizeof(line), f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (strncmp(line, key, klen) == 0 && line[klen] == '=' && strcmp(line + klen + 1, value) == 0) hit = true;
    }
    fclose(f);
    return hit;
}

// The highest n of an mp_lockstep_<n> folder with a mod.lua in `dir`; 0 = none.
static int ModVersionIn(const std::string& dir)
{
    static const char kPrefix[] = "mp_lockstep_";
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int best = 0;
    while (const dirent* e = readdir(d)) {
        if (strncmp(e->d_name, kPrefix, sizeof(kPrefix) - 1) != 0) continue;
        const char* num = e->d_name + sizeof(kPrefix) - 1;
        if (!AllDigits(num) || strlen(num) > 6) continue;
        const int v = atoi(num);
        if (v > best && IsFile(dir + "/" + e->d_name + "/mod.lua", nullptr)) best = v;
    }
    closedir(d);
    return best;
}

static bool ReadAll(const std::string& path, size_t size, std::string* out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out->assign(size, '\0');
    size_t got = 0;
    while (got < size) {
        const ssize_t n = read(fd, &(*out)[got], size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return got == size;
}

static bool WriteReplace(const std::string& path, const std::string& tmp, const std::string& text, mode_t mode, int* err)
{
    *err = 0;
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode & 0777);
    if (fd < 0) { *err = errno; return false; }
    if (fchmod(fd, mode & 07777) != 0) { /* the umask's mode then: still readable by the game */ }
    bool ok = true;
    for (size_t off = 0; ok && off < text.size();) {
        const ssize_t w = write(fd, text.data() + off, text.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            *err = errno;
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) { *err = errno; ok = false; }
    if (close(fd) != 0 && ok) { *err = errno; ok = false; }
    if (ok && rename(tmp.c_str(), path.c_str()) != 0) { *err = errno; ok = false; }
    if (!ok) unlink(tmp.c_str());
    return ok;
}

static void AutoEnableMod()
{
    const std::string flags = OurDir() + "tpf2_menu_flags.txt";
    if (FlagIs(flags, "automod", "0")) {
        Log("[automod] off: automod=0 in %s\n", flags.c_str());
        return;
    }
    const std::string local = UserDataLocal();
    if (local.empty()) {
        Log("[automod] no user data folder -- settings.lua untouched\n");
        return;
    }
    const std::string gameMods = ExeDir() + "/mods", userMods = local + "/mods";
    const int inGame = ModVersionIn(gameMods), inUser = ModVersionIn(userMods);
    const int version = std::max(inGame, inUser);
    if (!version) {
        Log("[automod] no mp_lockstep_<n>/mod.lua in %s or %s -- activeMods left alone\n", gameMods.c_str(), userMods.c_str());
        return;
    }
    const std::string path = local + "/settings.lua";
    struct stat st;
    if (!IsFile(path, &st)) {
        // first launch ever: the game writes settings.lua at exit; the next launch adds the mod
        Log("[automod] no %s yet -- the mod is added on the next launch\n", path.c_str());
        return;
    }
    if (st.st_size <= 0 || st.st_size > (4 << 20)) {
        Log("[automod] %s is %lld bytes -- left alone\n", path.c_str(), (long long)st.st_size);
        return;
    }
    std::string txt;
    if (!ReadAll(path, (size_t)st.st_size, &txt)) {
        Log("[automod] reading %s failed -- left alone\n", path.c_str());
        return;
    }
    if (txt.find("\"mp_lockstep\"") != std::string::npos) {
        Log("[automod] mp_lockstep already in activeMods (%s)\n", path.c_str());
        return;
    }
    const std::string nl = txt.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    char entry[64];
    snprintf(entry, sizeof(entry), "\t\t{ \"mp_lockstep\", %d, },", version);
    std::string out;
    const size_t at = txt.find("activeMods = {");
    if (at != std::string::npos) {
        const size_t brace = txt.find('{', at);
        out = txt.substr(0, brace + 1) + nl + entry + txt.substr(brace + 1);
    } else {
        const size_t ret = txt.find("return {");
        if (ret == std::string::npos) {
            Log("[automod] %s has neither 'activeMods = {' nor 'return {' -- left alone\n", path.c_str());
            return;
        }
        const size_t brace = ret + 7;
        out = txt.substr(0, brace + 1) + nl + "\tactiveMods = {" + nl + entry + nl + "\t}," + txt.substr(brace + 1);
    }
    const std::string bak = path + ".mpbak";
    int err = 0;
    if (!IsFile(bak, nullptr) && !CopyFileSynced(path, bak, &err))
        Log("[automod] no backup %s (%s) -- going on\n", bak.c_str(), strerror(err));
    if (!WriteReplace(path, path + ".mptmp", out, st.st_mode, &err)) {
        Log("[automod] replacing %s failed: %s -- settings.lua untouched\n", path.c_str(), strerror(err));
        return;
    }
    Log("[automod] { \"mp_lockstep\", %d, } added to activeMods in %s (%s; the mod is in %s; backup settings.lua.mpbak)\n",
        version, path.c_str(), at != std::string::npos ? "existing list" : "new list",
        inGame == version ? gameMods.c_str() : userMods.c_str());
}

void MenuGame_AutoEnableMod(Tpf2mpLogFn log)
{
    if (log) g_log = log;
    try {
        AutoEnableMod();
    } catch (...) {   // our own exceptions only (no game frame below): nothing may leave an ELF constructor
        Log("[automod] failed with an exception -- settings.lua untouched\n");
    }
}

// ---- the frame gate [AL-19] ------------------------------------------------------------------------
// From here to the install section, functions marked GAME PATH run between game
// frames: no try/catch, no destructors, nothing of ours that can throw except
// through the noinline helpers (see EXCEPTIONS at the top).
using StepFn   = void (*)(void* component, int64_t t, int64_t dt);
using UpdateFn = void (*)(void* self, int64_t t, int64_t dt);

static uintptr_t g_base = 0;
#include "progress_checks_linux.h"
#include <sys/uio.h>
static std::atomic<uintptr_t> g_progressMenu{0};
static std::atomic<bool> g_progressReady{false};
static bool ProgressRead(uintptr_t address, void* out, size_t size)
{
    if (!address || address+size<address) return false;
    iovec local{out,size}, remote{reinterpret_cast<void*>(address),size};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==ssize_t(size);
}
void MenuGame_ObserveMenu(void* menu) { g_progressMenu=uintptr_t(menu); }
int MenuGame_LoadPercent()
{
    if (!g_progressReady.load()) return -1;
    const uintptr_t menu=g_progressMenu.load();
    uintptr_t bar=0, monitor=0, vtable=0;
    float value=0;
    if (!menu || !ProgressRead(menu+0x498,&bar,sizeof(bar)) || !bar ||
        !ProgressRead(bar+0x440,&monitor,sizeof(monitor)) || !monitor ||
        !ProgressRead(monitor,&vtable,sizeof(vtable)) || vtable!=g_base+0x59d8c60 ||
        !ProgressRead(monitor+8,&value,sizeof(value)) || !(value>=0 && value<=1)) return -1;
    return int(value*100);
}
static bool CheckProgress(uintptr_t base)
{
    for (const auto& check:kProgressChecks) {
        uint8_t bytes[64];
        if (!ProgressRead(base+check.rva,bytes,check.size) || memcmp(bytes,check.bytes,check.size)) return false;
    }
    uintptr_t slots[4];
    if (!ProgressRead(base+0x59d8c60,slots,sizeof(slots))) return false;
    return slots[0]==base+0x30ebcd0 && slots[1]==base+0x30ebe00 &&
           slots[2]==base+0x30ebd30 && slots[3]==base+0x30ebd10;
}

static std::atomic<bool> g_gateOk{false};

// The Step wrappers a thread is inside, by frame address. The stack grows down,
// so a live wrapper's mark lies above every frame it encloses; a mark at or
// below a frame being entered belongs to a wrapper an exception unwound.
static const int kMarks = 32;
struct StepMarks {
    uintptr_t frame[kMarks];
    uintptr_t nested[kMarks];
    int nFrame;
    int nNested;
    int unmarked;       // wrappers entered while their array was full: no action until they are gone
    uint64_t frameGen;  // frame Steps entered on this thread
};
static thread_local StepMarks t_marks;

// GAME PATH
static void DropMarksBelow(StepMarks& m, uintptr_t here)
{
    while (m.nFrame > 0 && m.frame[m.nFrame - 1] <= here) m.nFrame--;
    while (m.nNested > 0 && m.nested[m.nNested - 1] <= here) m.nNested--;
    if (m.nFrame == 0 && m.nNested == 0) m.unmarked = 0;
}

// GAME PATH
__attribute__((noinline))
static void StepFromFrame(void* component, int64_t t, int64_t dt)
{
    StepMarks& m = t_marks;
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    DropMarksBelow(m, here);
    m.frameGen++;
    const bool marked = m.nFrame < kMarks;
    if (marked) m.frame[m.nFrame++] = here;
    else m.unmarked++;
    ((StepFn)(g_base + RVA_STEP))(component, t, dt);
    if (!marked) m.unmarked--;
    DropMarksBelow(m, here);   // ours, and any a deeper exception left
}

// GAME PATH
__attribute__((noinline))
static void StepNested(void* component, int64_t t, int64_t dt)
{
    StepMarks& m = t_marks;
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    DropMarksBelow(m, here);
    const bool marked = m.nNested < kMarks;
    if (marked) m.nested[m.nNested++] = here;
    else m.unmarked++;
    ((StepFn)(g_base + RVA_STEP))(component, t, dt);
    if (!marked) m.unmarked--;
    DropMarksBelow(m, here);
}

// GAME PATH. `here` is the caller's frame address.
static bool OnFrameStep(uintptr_t here)
{
    if (!g_gateOk.load(std::memory_order_relaxed)) return false;
    const StepMarks& m = t_marks;
    if (m.unmarked) return false;
    int frames = 0;
    for (int i = 0; i < m.nFrame; i++) frames += m.frame[i] > here;
    if (frames != 1) return false;
    for (int i = 0; i < m.nNested; i++)
        if (m.nested[i] > here) return false;
    return true;
}

static bool CallsStep(uintptr_t base, uintptr_t siteRva)
{
    const uint8_t* s = (const uint8_t*)(base + siteRva);
    int32_t rel;
    memcpy(&rel, s + 1, 4);
    return s[0] == 0xE8 && base + siteRva + 5 + (intptr_t)rel == base + RVA_STEP;
}

// Undo of Tpf2mpRedirectCall: the rel32 at `site` points at `callee` again.
static bool RestoreCall(uintptr_t site, uintptr_t callee)
{
    const intptr_t d = (intptr_t)callee - (intptr_t)(site + 5);
    if (((const uint8_t*)site)[0] != 0xE8 || d > INT32_MAX || d < INT32_MIN) return false;
    const int32_t rel = (int32_t)d;
    int err = 0;
    return Tpf2mpCodeWriteSelf(site + 1, (const uint8_t*)&rel, sizeof(rel), &err) == TPF2MP_CW_OK;
}

static bool InstallGate(uintptr_t base)
{
    struct Site { uintptr_t rva; void* wrapper; const char* what; };
    // The nested two first: until the frame site is in, no frame mark exists.
    const Site sites[] = {
        { RVA_SITE_LISTENER, (void*)&StepNested,    "the step-listener lambda's Step" },
        { RVA_SITE_ZERO,     (void*)&StepNested,    "0x312af90's Step(c, 0, 0)" },
        { RVA_SITE_FRAME,    (void*)&StepFromFrame, "the frame step" },
    };
    for (const Site& s : sites) {
        if (!CallsStep(base, s.rva)) {
            Log("[menugame] %lx (%s) is not a call of Step %lx -- no frame gate\n",
                (unsigned long)s.rva, s.what, (unsigned long)RVA_STEP);
            return false;
        }
    }
    size_t done = 0;
    while (done < 3 && Tpf2mpRedirectCall(base + sites[done].rva, base + RVA_STEP, sites[done].wrapper)) done++;
    if (done == 3) {
        g_gateOk = true;
        Log("[menugame] frame gate: Step calls %lx, %lx and %lx redirected\n",
            (unsigned long)RVA_SITE_FRAME, (unsigned long)RVA_SITE_ZERO, (unsigned long)RVA_SITE_LISTENER);
        return true;
    }
    Log("[menugame] redirecting %lx (%s) failed (no stub within reach, or code write refused) -- no frame gate\n",
        (unsigned long)sites[done].rva, sites[done].what);
    while (done-- > 0) {
        if (!RestoreCall(base + sites[done].rva, base + RVA_STEP))
            Log("[menugame] %lx could not be put back; its wrapper stays, passing every call through\n",
                (unsigned long)sites[done].rva);
    }
    return false;
}

// ---- AUTO-LOAD [AL-06..18] ------------------------------------------------------------------------
static std::atomic<bool>     g_autoloadOn{false};
static std::atomic<uint64_t> g_loadAccepted{0};
static UpdateFn              g_menuUpdate = nullptr;   // the original, published before the swap
static std::atomic<bool> g_modRefresh{false};
static std::atomic<uint64_t> g_alPending{0};           // the request waiting for a menu frame; 0 = none
static std::atomic<uint64_t> g_alSeen{0};              // the last request a gated menu frame looked at
static std::atomic<uint64_t> g_alWaitLogged{0};
static std::atomic<bool>     g_menuFrameLogged{false};
static const int kAlWatchdogMs = 15000;
// A load this thread started and that has not returned: an exception from the
// game took it (checked on the next gated frame).
static thread_local uint64_t t_alInFlight = 0;
static thread_local char     t_alInFlightName[256];

static std::atomic<MenuGameLoadObserver> g_loadObserver{nullptr};
static void* g_originalStartSavegame;
static uint8_t StartSavegameDetour(void* menu, void* params, void* info)
{
    // No C++ cleanup/catch encloses the foreign engine call.
    const uint8_t accepted = reinterpret_cast<uint8_t (*)(void*,void*,void*)>(g_originalStartSavegame)(menu,params,info);
    if (!accepted || t_alInFlight) return accepted;
    const GStr* name = static_cast<const GStr*>(params); // libstdc++ string +0
    if (!name || !name->p || !name->len || name->len > 200) return accepted;
    char text[201]; memcpy(text,name->p,name->len); text[name->len]=0;
    if (strlen(text)!=name->len || text[0]=='.' || strpbrk(text,"/\\")) return accepted;
    if (const auto observer=g_loadObserver.load()) {
        try { observer(text); }
        catch (...) { Log("[menu] accepted load could not be queued for sharing\n"); }
    }
    return accepted;
}
void MenuGame_ObserveLoads(MenuGameLoadObserver observer) { g_loadObserver.store(observer); }

struct AlState {
    SpinLock lock;
    std::string name;
    uint64_t gen = 0;
};
static AlState& AL()
{
    static AlState* s = new AlState;
    return *s;
}

__attribute__((noinline))
static void StatusPickByHand(const char* name)
{
    char text[400];
    snprintf(text, sizeof(text), "Save ready -- open LOAD GAME and pick \"%s\".", name);
    try {
        panel::SetStatus(text);
    } catch (...) {
    }
}

__attribute__((noinline))
static void StatusCouldNotStart(const char* name)
{
    char text[400];
    snprintf(text, sizeof(text), "Couldn't start the shared save by itself -- open LOAD GAME and pick \"%s\".", name);
    try {
        panel::SetStatus(text);
    } catch (...) {
    }
}

// The status line from a thread of our own: the caller may hold the panel's
// lock. With `gen`, only if that request is still pending and no menu frame
// has looked at it (the watchdog of a request nothing takes).
static void StatusLater(uint64_t gen, const std::string& name, int delayMs)
{
    try {
        std::thread([gen, name, delayMs] {
            if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (gen) {
                uint64_t expect = gen;
                if (g_alSeen.load() == gen || !g_alPending.compare_exchange_strong(expect, 0)) return;
                Log("[autoload] no title-menu frame took %s within %d s -- the player loads it by hand\n",
                    name.c_str(), delayMs / 1000);
            }
            StatusPickByHand(name.c_str());
        }).detach();
    } catch (...) {
        Log("[autoload] no thread for the status line\n");
    }
}

// The request's name, if `gen` is still the latest request.
__attribute__((noinline))
static bool RequestName(uint64_t gen, char* out, size_t outLen)
{
    AlState& al = AL();
    std::lock_guard<SpinLock> lk(al.lock);
    if (al.gen != gen) return false;
    snprintf(out, outLen, "%s", al.name.c_str());
    return true;
}

// <save dir>/<name>.sav is a file (the getter throws "file not found" otherwise [SAVE-06]).
__attribute__((noinline))
static bool SharedSaveThere(const char* name, char* path, size_t pathLen)
{
    try {
        const std::string p = MenuGame_SaveDir() + "/" + name + ".sav";
        snprintf(path, pathLen, "%s", p.c_str());
        struct stat st;
        return IsFile(p, &st) && st.st_size > 0;
    } catch (...) {
        snprintf(path, pathLen, "(out of memory)");
        return false;
    }
}

// ---- catching with the game's C++ runtime ---------------------------------------------------------
// tpf2mp_mg_guarded(fn, ctx) calls fn(ctx) and returns 0, or 1 when fn threw a
// C++ exception, which it has then caught and destroyed. It is GCC's output for
//     int f(void (*fn)(void*), void* ctx) { try { fn(ctx); } catch (...) { return 1; } return 0; }
// (the LSDA is that output's, a record for the handler's own calls included)
// with three changes: the personality is an indirect pointer to a slot holding
// libstdc++.so.6's __gxx_personality_v0; the handler calls libstdc++.so.6's
// __cxa_begin_catch and __cxa_end_catch through slots; and an unwind that is not
// a C++ exception (class other than "GNUCC++", e.g. a forced unwind) is sent on
// with libgcc_s's _Unwind_Resume instead of being caught. The slots are filled
// at install (ResolveGameRuntime); autoload stays off unless all are.
extern "C" {
__attribute__((visibility("hidden"))) void* tpf2mp_mg_personality = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_mg_begin_catch = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_mg_end_catch = nullptr;
__attribute__((visibility("hidden"))) void* tpf2mp_mg_unwind_resume = nullptr;
__attribute__((visibility("hidden"))) int tpf2mp_mg_guarded(void (*fn)(void*), void* ctx);
__attribute__((visibility("hidden"))) void tpf2mp_mg_note_caught(void* thrown);
}
__asm__(R"ASM(
    .text
    .p2align 4
    .type   tpf2mp_mg_guarded, @function
tpf2mp_mg_guarded:
.Lmg_fb:
    .cfi_startproc
    .cfi_personality 0x9b, tpf2mp_mg_personality
    .cfi_lsda 0x1b, .Lmg_lsda
    endbr64
    subq    $8, %rsp
    .cfi_def_cfa_offset 16
    movq    %rdi, %rax
    movq    %rsi, %rdi
.Lmg_call_b:
    call    *%rax
.Lmg_call_e:
    xorl    %eax, %eax
.Lmg_ret:
    addq    $8, %rsp
    .cfi_remember_state
    .cfi_def_cfa_offset 8
    ret
.Lmg_pad:
    .cfi_restore_state
    endbr64
    movq    (%rax), %rdx
    andq    $-256, %rdx
    movabsq $0x474e5543432b2b00, %rcx
    cmpq    %rcx, %rdx
    jne     .Lmg_foreign
    movq    %rax, %rdi
    call    *tpf2mp_mg_begin_catch(%rip)
    movq    %rax, %rdi
    call    tpf2mp_mg_note_caught
    call    *tpf2mp_mg_end_catch(%rip)
    movl    $1, %eax
    jmp     .Lmg_ret
.Lmg_foreign:
    movq    %rax, %rdi
    call    *tpf2mp_mg_unwind_resume(%rip)
    ud2
.Lmg_pad_e:
    .cfi_endproc
    .size   tpf2mp_mg_guarded, .-tpf2mp_mg_guarded

    .section .gcc_except_table,"a",@progbits
    .p2align 2
.Lmg_lsda:
    .byte   0xff
    .byte   0x9b
    .uleb128 .Lmg_tt-.Lmg_ttd
.Lmg_ttd:
    .byte   0x1
    .uleb128 .Lmg_cse-.Lmg_csb
.Lmg_csb:
    .uleb128 .Lmg_call_b-.Lmg_fb
    .uleb128 .Lmg_call_e-.Lmg_call_b
    .uleb128 .Lmg_pad-.Lmg_fb
    .uleb128 0x1
    .uleb128 .Lmg_pad-.Lmg_fb
    .uleb128 .Lmg_pad_e-.Lmg_pad
    .uleb128 0
    .uleb128 0
.Lmg_cse:
    .byte   0x1
    .byte   0
    .p2align 2
    .long   0
.Lmg_tt:
    .text
)ASM");

static void* (*g_currentExceptionType)() = nullptr;   // libstdc++.so.6's __cxa_current_exception_type
static const std::type_info* g_stdException = nullptr; // libstdc++.so.6's typeinfo for std::exception
static thread_local char t_caught[400];

// Between __cxa_begin_catch and __cxa_end_catch: what was caught, for the log.
void tpf2mp_mg_note_caught(void* thrown)
{
    snprintf(t_caught, sizeof(t_caught), "an exception of unknown type");
    const auto* type = g_currentExceptionType ? (const std::type_info*)g_currentExceptionType() : nullptr;
    if (!type) return;
    int status = -1;
    char* const pretty = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
    const char* const name = status == 0 && pretty ? pretty : type->name();
    void* obj = thrown;
    if (g_stdException && g_stdException->__do_catch(type, &obj, 1))
        snprintf(t_caught, sizeof(t_caught), "%s: %s", name, static_cast<const std::exception*>(obj)->what());
    else
        snprintf(t_caught, sizeof(t_caught), "%s", name);
    free(pretty);
}

// The game's runtime for tpf2mp_mg_guarded. Ours is hidden (--exclude-libs), so
// the global scope has libstdc++.so.6's and libgcc_s.so.1's only; anything that
// resolves into this library is refused.
static bool ResolveGameRuntime()
{
    Dl_info self = {};
    dladdr((void*)&ResolveGameRuntime, &self);
    struct Need { const char* name; void** slot; void* found; };
    Need need[] = {
        { "__gxx_personality_v0", &tpf2mp_mg_personality, nullptr },
        { "__cxa_begin_catch", &tpf2mp_mg_begin_catch, nullptr },
        { "__cxa_end_catch", &tpf2mp_mg_end_catch, nullptr },
        { "_Unwind_Resume", &tpf2mp_mg_unwind_resume, nullptr },
    };
    bool ok = true;
    for (Need& n : need) {
        n.found = dlsym(RTLD_DEFAULT, n.name);
        Dl_info of = {};
        if (!n.found || !dladdr(n.found, &of) || of.dli_fbase == self.dli_fbase) {
            Log("[menugame] %s %s\n", n.name, n.found ? "resolves into this library" : "is not in the process");
            ok = false;
        }
    }
    if (!ok) return false;
    g_currentExceptionType = (void* (*)())dlsym(RTLD_DEFAULT, "__cxa_current_exception_type");
    g_stdException = (const std::type_info*)dlsym(RTLD_DEFAULT, "_ZTISt9exception");
    for (Need& n : need) *n.slot = n.found;   // before the vtable swap: nothing enters the guard until then
    Dl_info of = {};
    dladdr(tpf2mp_mg_personality, &of);
    Log("[menugame] the load catches with the game's own C++ runtime (%s)\n", of.dli_fname ? of.dli_fname : "?");
    return true;
}

static GStr* StrAt(unsigned char* obj, size_t off) { return (GStr*)(void*)(obj + off); }
static bool IsEmptyLocal(const GStr* s) { return s->p == s->buf && s->len == 0 && s->buf[0] == 0; }
static bool Holds(const GStr* s, const char* want)
{
    const size_t n = strlen(want);
    return s->p && s->len == n && memcmp(s->p, want, n + 1) == 0;
}

// One load's objects and progress. Flags rise after a constructor returns and
// drop before a destructor runs, so a throw leaves exactly the live objects marked.
struct LoadCtx {
    void* menu;
    const char* name;
    int stage;
    int result;
    bool paramsLive, idLive, infoLive;
    char why[400];
    alignas(16) unsigned char params[0x180];   // LoadGameParams, 0x130 bytes
    alignas(16) unsigned char info[0x160];     // SavegameInfo, 0x108 bytes
    alignas(16) unsigned char id[0x80];        // SaveGameId, 0x60 bytes
};
static const char* const kLoadStages[] = {
    "(nothing)", "LoadGameParams()", "std::string(const char*)", "the app accessor", "the SavegameInfo getter",
    "~SaveGameId", "StartSavegame", "~SavegameInfo", "~LoadGameParams",
};

// GAME PATH, inside tpf2mp_mg_guarded. The Missions page's sequence [AL-15] for
// {"", name, "savegame"}. result: 1 started, 0 refused by StartSavegame, -1 no
// save manager, -2 a layout that is not build 35924's, -3 an unexpected getter result.
static void LoadBody(void* p)
{
    LoadCtx& c = *static_cast<LoadCtx*>(p);
    const auto paramsCtor = (void (*)(void*))(g_base + RVA_PARAMS_CTOR);
    const auto paramsDtor = (void (*)(void*))(g_base + RVA_PARAMS_DTOR);
    const auto strCtor    = (void (*)(void*, const char*))(g_base + RVA_STR_FROM_CSTR);
    const auto app        = (void* (*)())(g_base + RVA_APP);
    const auto infoGet    = (void* (*)(void*, void*, void*))(g_base + RVA_SAVEINFO_GET);
    const auto infoDtor   = (void (*)(void*))(g_base + RVA_SAVEINFO_DTOR);
    const auto idDtor     = (void (*)(void*))(g_base + RVA_SAVEGAMEID_DTOR);
    const auto start      = (uint8_t (*)(void*, void*, void*))(g_base + RVA_START_SAVEGAME);

    c.stage = 1;
    paramsCtor(c.params);
    c.paramsLive = true;
    // What the ctor leaves (0x116219d..0x1162286): every string empty in its
    // local buffer, the namespace "savegame" in its own, the flags clear.
    if (!IsEmptyLocal(StrAt(c.params, 0x00)) || !IsEmptyLocal(StrAt(c.params, 0x20)) ||
        !Holds(StrAt(c.params, 0x40), "savegame") || StrAt(c.params, 0x40)->p != StrAt(c.params, 0x40)->buf ||
        !IsEmptyLocal(StrAt(c.params, 0xf0)) || !IsEmptyLocal(StrAt(c.params, 0x110)) ||
        c.params[0x60] || c.params[0x80] || c.params[0xa0] || c.params[0xe0] || c.params[0xe8]) {
        snprintf(c.why, sizeof(c.why), "LoadGameParams() left a layout that is not build 35924's");
        c.result = -2;
    } else {
        c.stage = 2;
        strCtor(c.params + 0x00, c.name);   // over an empty local string: nothing to free
        // SaveGameId {path, name, namespace}: empty local strings, the shape the
        // ctor just showed, then filled by the game's own constructor.
        for (size_t off = 0; off < 0x60; off += 0x20) {
            GStr* const str = StrAt(c.id, off);
            str->p = str->buf;
            str->len = 0;
            str->buf[0] = 0;
        }
        c.idLive = true;
        strCtor(c.id + 0x20, c.name);
        strCtor(c.id + 0x40, "savegame");
        if (!Holds(StrAt(c.params, 0x00), c.name) || !IsEmptyLocal(StrAt(c.id, 0x00)) ||
            !Holds(StrAt(c.id, 0x20), c.name) || !Holds(StrAt(c.id, 0x40), "savegame")) {
            snprintf(c.why, sizeof(c.why), "std::string(const char*) did not build the strings as expected");
            c.result = -2;
        } else {
            c.stage = 3;
            const void* const a = app();
            void* const mgr = a ? *(void* const*)((const char*)a + OFF_APP_SAVEMGR) : nullptr;
            if (!mgr) {
                snprintf(c.why, sizeof(c.why), "no save manager (app %p)", (void*)a);
                c.result = -1;
            } else {
                c.stage = 4;
                const void* const got = infoGet(c.info, mgr, c.id);
                if (got != (void*)c.info) {
                    // It constructs and returns `out` [AL-11]; anything else is left alone.
                    snprintf(c.why, sizeof(c.why), "the SavegameInfo getter returned %p, not %p", (void*)got, (void*)c.info);
                    c.result = -3;
                } else {
                    c.infoLive = true;
                    c.stage = 5;
                    c.idLive = false;
                    idDtor(c.id);
                    c.stage = 6;
                    c.result = start(c.menu, c.params, c.info) != 0 ? 1 : 0;
                    c.stage = 7;
                    c.infoLive = false;
                    infoDtor(c.info);
                }
            }
        }
        if (c.idLive) {
            c.stage = 5;
            c.idLive = false;
            idDtor(c.id);
        }
    }
    c.stage = 8;
    c.paramsLive = false;
    paramsDtor(c.params);
}

// GAME PATH, inside tpf2mp_mg_guarded: what a throw left live, as the Missions
// page's landing pads destroy it (never the getter's `out` after its own throw [AL-11]).
static void LoadCleanUp(void* p)
{
    LoadCtx& c = *static_cast<LoadCtx*>(p);
    if (c.infoLive) {
        c.infoLive = false;
        ((void (*)(void*))(g_base + RVA_SAVEINFO_DTOR))(c.info);
    }
    if (c.idLive) {
        c.idLive = false;
        ((void (*)(void*))(g_base + RVA_SAVEGAMEID_DTOR))(c.id);
    }
    if (c.paramsLive) {
        c.paramsLive = false;
        ((void (*)(void*))(g_base + RVA_PARAMS_DTOR))(c.params);
    }
}

#include "native_io_linux.inl"

// GAME PATH
static void AutoloadTick(void* menu)
{
    uint64_t gen = g_alPending.load();
    if (!gen) return;
    g_alSeen.store(gen);
    char name[256];
    if (!RequestName(gen, name, sizeof(name))) return;   // a newer request is on its way in
    const char* m = (const char*)menu;
    if (*(const uint64_t*)(m + MENU_OFF_GAMEUI) != 0) {
        if (!g_alPending.compare_exchange_strong(gen, 0)) return;
        Log("[autoload] a game is running -- %s is for the player to load from the in-game menu\n", name);
        StatusPickByHand(name);
        return;
    }
    if (*(const uint8_t*)(m + MENU_OFF_INITING) != 0 || *(const uint64_t*)(m + MENU_OFF_QUEUED) != 0) {
        if (g_alWaitLogged.exchange(gen) != gen) Log("[autoload] the menu is already starting a game -- %s waits\n", name);
        return;
    }
    if (!g_alPending.compare_exchange_strong(gen, 0)) return;
    char path[4096];
    if (!SharedSaveThere(name, path, sizeof(path))) {
        Log("[autoload] %s is not there -- not starting it\n", path);
        StatusCouldNotStart(name);
        return;
    }
    LoadCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.menu = menu;
    ctx.name = name;
    ctx.result = -1;
    memcpy(t_alInFlightName, name, sizeof(t_alInFlightName));
    t_alInFlight = gen;
    const bool threw = tpf2mp_mg_guarded(&LoadBody, &ctx) != 0;
    if (threw) {
        char caught[sizeof(t_caught)];
        memcpy(caught, t_caught, sizeof(caught));
        const int stage = ctx.stage;
        const bool cleanUpThrew = tpf2mp_mg_guarded(&LoadCleanUp, &ctx) != 0;
        t_alInFlight = 0;
        Log("[autoload] %s threw while loading %s: %s%s -- the player loads it by hand\n",
            kLoadStages[stage], name, caught, cleanUpThrew ? " (destroying what was built threw too; left as it is)" : "");
        StatusCouldNotStart(name);
        return;
    }
    t_alInFlight = 0;
    if (ctx.result == 1) {
        g_loadAccepted = NowMs();
        Log("[autoload] StartSavegame(%s) started the load\n", name);
    } else if (ctx.result == 0) {
        Log("[autoload] StartSavegame(%s) refused (mods, or a load already starting) -- the player loads it by hand\n", name);
        StatusCouldNotStart(name);
    } else {
        Log("[autoload] %s not started (%d): %s\n", name, ctx.result, ctx.why);
        StatusCouldNotStart(name);
    }
}

// GAME PATH
static void RefreshModsBody(void* menu) {reinterpret_cast<void(*)(void*,int)>(g_base+0x1154b20)(menu,8);}
static void MenuUpdateDetour(void* menu, int64_t t, int64_t dt)
{
    g_menuUpdate(menu, t, dt);   // first and untouched
    if (!OnFrameStep((uintptr_t)__builtin_frame_address(0))) return;
    NativeIo::Tick(menu,false);
    if(g_modRefresh.load() && !*reinterpret_cast<uintptr_t*>(static_cast<char*>(menu)+MENU_OFF_GAMEUI) &&
       !*reinterpret_cast<uint8_t*>(static_cast<char*>(menu)+MENU_OFF_INITING) &&
       !*reinterpret_cast<uintptr_t*>(static_cast<char*>(menu)+MENU_OFF_QUEUED) && g_modRefresh.exchange(false)) {
        const bool threw=tpf2mp_mg_guarded(RefreshModsBody,menu)!=0;
        Log("[menugame] mod catalogue refresh %s\n",threw?"threw":"requested on load page");
    }

    if (!g_menuFrameLogged.load(std::memory_order_relaxed)) {
        g_menuFrameLogged = true;
        Log("[autoload] the title menu's frame update runs (CMenuUI %p)\n", menu);
    }
    if (t_alInFlight) {   // not inside it: a live load would have made this a nested step
        t_alInFlight = 0;
        Log("[autoload] loading %s never returned (unwound by something that is not a C++ exception)\n", t_alInFlightName);
        StatusCouldNotStart(t_alInFlightName);
    }
    if (g_alPending.load(std::memory_order_relaxed) == 0) return;
    if (*(const uintptr_t*)menu != g_base + RVA_MENUUI_VTABLE) return;
    AutoloadTick(menu);
}

void MenuGame_RequestAutoload(const std::string& placedName)
{
    // a save name as MenuGame_PlaceSharedSave returns it, never a path
    if (placedName.empty() || placedName.size() > 200 || placedName[0] == '.' ||
        placedName.find_first_of(std::string("/\\\0", 3)) != std::string::npos) {
        Log("[autoload] refused save name \"%s\"\n", placedName.c_str());
        return;
    }
    uint64_t gen;
    {
        AlState& al = AL();
        std::lock_guard<SpinLock> lk(al.lock);
        al.name = placedName;
        gen = ++al.gen;
    }
    if (!g_autoloadOn.load()) {
        Log("[autoload] not installed -- the player loads %s from LOAD GAME\n", placedName.c_str());
        StatusLater(0, placedName, 0);
        return;
    }
    g_alPending.store(gen);
    Log("[autoload] %s: starting it on the next title-menu frame\n", placedName.c_str());
    StatusLater(gen, placedName, kAlWatchdogMs);
}

void MenuGame_RequestModRefresh() {g_modRefresh=true;}
bool MenuGame_Loading() {
    auto since = g_loadAccepted.load();
    if (since && NowMs() - since >= 30ull * 60 * 1000 && g_loadAccepted.compare_exchange_strong(since, 0))
        Log("[autoload] accepted load produced no world for 30 minutes; allowing retry\n");
    return g_alPending.load() != 0 || g_loadAccepted.load() != 0 || NativeIo::Loading();
}

// ---- HOT JOIN [HJ-03..07] -------------------------------------------------------------------------
static std::atomic<bool>  g_forceOn{false};
static UpdateFn           g_gameUiUpdate = nullptr;   // the original, published before the swap
static std::atomic<void*> g_frameGameUi{nullptr};     // the CGameUI whose gated frame ran last
static std::atomic<bool>  g_forcePending{false};
static std::atomic<int>   g_forceThrows{0};
static const int      kForceRetries   = 3;
static const uint64_t kForceTimeoutMs = 60000;        // the lobby waits 90 s for the file

struct ForceState {
    SpinLock lock;
    void* ui = nullptr;
    uint64_t deadlineMs = 0;
    int retries = 0;
    int waitLogged = 0;   // bits: 1 skip flag, 2 saving, 4 interval, 8 block not run
};
static ForceState& FS()
{
    static ForceState* s = new ForceState;
    return *s;
}

// The forced call this thread is in (or was in, when an exception unwound it).
struct ForceCall {
    void* ui;
    int64_t old;
    uintptr_t frame;     // the detour's frame address
    uint64_t frameGen;   // t_marks.frameGen at the call
    bool active;
};
static thread_local ForceCall t_forceCall;

static int AutosaveMinutes()
{
    const void* gs = *(void* const*)(g_base + RVA_GLOBALSETTINGS);
    return gs ? *(const int32_t*)((const char*)gs + GS_OFF_AUTOSAVE_MIN) : 0;
}

// 1 = apply the pending request to `ui` now; 0 = nothing to do (a stale request is dropped).
__attribute__((noinline))
static int TakeForce(void* ui)
{
    ForceState& fs = FS();
    const char* drop = nullptr;
    int apply = 0;
    {
        std::lock_guard<SpinLock> lk(fs.lock);
        if (!g_forcePending.load()) return 0;
        if (fs.ui != ui) drop = "it was for another game";
        else if (NowMs() > fs.deadlineMs) drop = "no frame could take it within 60 s";
        else if (g_forceThrows.load() >= kForceRetries) drop = "the update threw each time";
        else apply = 1;
        if (drop) g_forcePending = false;
    }
    if (drop) Log("[hotjoin] autosave request dropped: %s\n", drop);
    return apply;
}

// Once per request per reason.
__attribute__((noinline))
static bool FirstWait(int bit)
{
    ForceState& fs = FS();
    std::lock_guard<SpinLock> lk(fs.lock);
    const bool first = !(fs.waitLogged & bit);
    fs.waitLogged |= bit;
    return first;
}

__attribute__((noinline))
static void ForceOutcome(void* ui, bool blockSkipped, bool saving, int minutes)
{
    ForceState& fs = FS();
    if (blockSkipped) {   // the next frame tries again
        if (FirstWait(8)) Log("[hotjoin] the update skipped its autosave block this frame -- trying again\n");
        return;
    }
    int retries = 0;
    {
        std::lock_guard<SpinLock> lk(fs.lock);
        if (saving) g_forcePending = false;
        else if ((retries = ++fs.retries) >= kForceRetries) g_forcePending = false;
    }
    if (saving)
        Log("[hotjoin] the game's own autosave started (CGameUI %p, interval %d min)\n", ui, minutes);
    else
        Log("[hotjoin] the accumulator was reset but no save is running (%d of %d)%s\n", retries, kForceRetries,
            retries >= kForceRetries ? " -- giving up" : "");
}

// GAME PATH. A forced call that an exception unwound: the old value goes back
// before the update runs again. Every call of the update comes through the
// detour, so the force can never reach a later autosave block.
__attribute__((noinline))
static void SettleUnwoundForce(void* ui, uintptr_t here)
{
    ForceCall& fc = t_forceCall;
    if (fc.frameGen == t_marks.frameGen && fc.frame > here) return;   // we are inside the forced call
    fc.active = false;
    // only an object that is certainly alive: the one being updated, or the running game's
    if (fc.ui == ui || fc.ui == *(void* const*)(g_base + RVA_G_GAMEUI)) {
        const auto acc = (int64_t*)(void*)((char*)fc.ui + GAMEUI_OFF_ACC);
        if (*acc >= FORCE_ACC) *acc = fc.old;
    }
    g_forceThrows.fetch_add(1, std::memory_order_relaxed);
    Log("[hotjoin] the game's update threw with the forced accumulator in -- the old value is back\n");
}

// GAME PATH
static void ForcedUpdate(void* ui, int64_t t, int64_t dt, uintptr_t here)
{
    const int apply = TakeForce(ui);
    const char* g = (const char*)ui;
    const bool skip = *(const uint8_t*)(g + GAMEUI_OFF_SKIP) != 0;
    const bool saving = *(const uint8_t*)(g + GAMEUI_OFF_SAVING) != 0;
    const int minutes = AutosaveMinutes();
    if (!apply || skip || saving || minutes <= 0) {
        if (apply && skip && FirstWait(1)) Log("[hotjoin] the game's UI is hidden (CGameUI+0x597): its autosave waits\n");
        if (apply && !skip && saving && FirstWait(2)) Log("[hotjoin] a save is running already: the forced one waits for it\n");
        if (apply && !skip && !saving && minutes <= 0 && FirstWait(4)) Log("[hotjoin] autosaveIntervalMinutes is %d now: nothing to force\n", minutes);
        g_gameUiUpdate(ui, t, dt);
        return;
    }
    const auto acc = (int64_t*)(void*)((char*)ui + GAMEUI_OFF_ACC);
    ForceCall& fc = t_forceCall;
    fc.ui = ui;
    fc.old = *acc;
    fc.frame = here;
    fc.frameGen = t_marks.frameGen;
    fc.active = true;
    *acc = FORCE_ACC;
    g_gameUiUpdate(ui, t, dt);
    fc.active = false;
    const bool blockSkipped = *acc >= FORCE_ACC;
    const bool savingAfter = *(const uint8_t*)(g + GAMEUI_OFF_SAVING) != 0;
    if (blockSkipped) *acc = fc.old;   // never left behind
    ForceOutcome(ui, blockSkipped, savingAfter, minutes);
}

// GAME PATH
static void GameUiUpdateDetour(void* ui, int64_t t, int64_t dt)
{
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (t_forceCall.active) SettleUnwoundForce(ui, here);
    const bool current = *(const uintptr_t*)ui == g_base + RVA_GAMEUI_VTABLE &&
                         *(void* const*)(g_base + RVA_G_GAMEUI) == ui;
    if (current) { g_loadAccepted = false; panel::OnGameUiFrame(); }
    if (!current || !OnFrameStep(here)) {
        g_gameUiUpdate(ui, t, dt);
        return;
    }
    if(const auto menu=g_progressMenu.load()) NativeIo::Tick(reinterpret_cast<void*>(menu),true);
    if (g_frameGameUi.exchange(ui, std::memory_order_relaxed) != ui)
        Log("[hotjoin] a game's frame update runs (CGameUI %p)\n", ui);
    if (!g_forcePending.load(std::memory_order_relaxed)) {
        g_gameUiUpdate(ui, t, dt);
        return;
    }
    ForcedUpdate(ui, t, dt, here);
}

bool MenuGame_ForceAutosave()
{
    if (!g_forceOn.load()) {
        Log("[hotjoin] the forced autosave is not installed -- no save\n");
        return false;
    }
    void* const ui = g_frameGameUi.load();
    void* const current = *(void* const*)(g_base + RVA_G_GAMEUI);
    if (!ui || ui != current) {
        Log("[hotjoin] no game is running (CGameUI last seen %p, g_gameUI %p) -- no save\n", ui, current);
        return false;
    }
    const int minutes = AutosaveMinutes();
    if (minutes <= 0) {
        Log("[hotjoin] autosaveIntervalMinutes is %d: the game's autosave is off and cannot be forced\n", minutes);
        return false;
    }
    {
        ForceState& fs = FS();
        std::lock_guard<SpinLock> lk(fs.lock);
        fs.ui = ui;
        fs.deadlineMs = NowMs() + kForceTimeoutMs;
        fs.retries = 0;
        fs.waitLogged = 0;
        g_forceThrows = 0;
        g_forcePending = true;
    }
    Log("[hotjoin] autosave requested (CGameUI %p, interval %d min): the next frame forces it\n", ui, minutes);
    return true;
}

// ---- install ----------------------------------------------------------------------------------------
// The groups whose bytes differ from build 35924's, each difference logged.
static uint8_t CheckBytes(uintptr_t base)
{
    uint8_t bad = 0;
    for (const Expect& e : EXPECTS) {
        if (memcmp((void*)(base + e.rva), e.bytes, e.n) == 0) continue;
        bad |= e.groups;
        Log("[menugame] %lx (%s) differs from build 35924\n", (unsigned long)e.rva, e.what);
    }
    return bad;
}

// The class's vtable as build 35924 has it: slot -1 is its typeinfo, which
// names it; slot 34 holds `update`, which starts with UPDATE_PROLOGUE.
static bool VtableIs(uintptr_t base, const char* cls, uintptr_t vtable, uintptr_t typeinfo, uintptr_t typeName,
                     const char* mangled, uintptr_t update)
{
    const auto* slots = (const uintptr_t*)(base + vtable);
    const auto* ti = (const uintptr_t*)(base + typeinfo);
    const char* why = nullptr;
    if (slots[-1] != base + typeinfo) why = "slot -1 is not its typeinfo";
    else if (ti[1] != base + typeName || strcmp((const char*)(base + typeName), mangled) != 0) why = "its typeinfo names another class";
    else if (slots[SLOT_UPDATE] != base + update) why = "slot 34 does not hold its update (hooked by someone else?)";
    else if (memcmp((void*)(base + update), UPDATE_PROLOGUE, sizeof(UPDATE_PROLOGUE)) != 0) why = "its update starts differently";
    if (why) Log("[menugame] %s vtable %lx: %s\n", cls, (unsigned long)vtable, why);
    return !why;
}

// PROT_* of the mapping holding `addr`; -1 when none does.
static int ProtectionOf(uintptr_t addr)
{
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return -1;
    char line[4096];
    int prot = -1;
    bool atLineStart = true;
    while (prot < 0 && fgets(line, sizeof(line), f)) {
        const bool whole = strchr(line, '\n') != nullptr;
        if (atLineStart) {
            unsigned long lo = 0, hi = 0;
            char perms[5] = "";
            if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi)
                prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
        }
        atLineStart = whole;
    }
    fclose(f);
    return prot;
}

// Write one aligned vtable slot in RELRO [AL-05], without changing permissions.
static bool StoreSlot(uintptr_t slot, uintptr_t expected, uintptr_t value, const char* cls)
{
    const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t lo = slot & ~(page - 1);
    const int prot = ProtectionOf(slot);
    if (prot < 0 || !(prot & PROT_READ) || ((slot + 7) & ~(page - 1)) != lo) {
        Log("[menugame] %s slot %lx: not one readable page (protection %d)\n", cls, (unsigned long)slot, prot);
        return false;
    }
    if (*(const uintptr_t*)slot != expected) {
        Log("[menugame] %s slot %lx holds %lx -- left alone\n", cls, (unsigned long)slot, (unsigned long)*(const uintptr_t*)slot);
        return false;
    }
    int err = 0;
    const int result = Tpf2mpCodeWriteSelf(slot, (const uint8_t*)&value, sizeof(value), &err);
    if (result != TPF2MP_CW_OK)
        Log("[menugame] %s slot %lx: code write failed (%s, result %d)\n",
            cls, (unsigned long)slot, strerror(err), result);
    return result == TPF2MP_CW_OK;
}

static bool Install(uintptr_t base)
{
    if (!base) {
        Log("[menugame] no game image -- autoload and the forced autosave stay off\n");
        return false;
    }
    g_base = base;
    g_progressReady=CheckProgress(base);
    Log("[menugame] load percentage %s\n",g_progressReady ? "verified" : "OFF (byte/vtable check failed)");
    const bool guard = ResolveGameRuntime();
    const uint8_t bad = CheckBytes(base);
    const bool menuVt = VtableIs(base, "CMenuUI", RVA_MENUUI_VTABLE, RVA_MENUUI_TYPEINFO, RVA_MENUUI_TYPENAME,
                                 "N2UI7CMenuUIE", RVA_MENUUI_UPDATE);
    const bool gameVt = VtableIs(base, "CGameUI", RVA_GAMEUI_VTABLE, RVA_GAMEUI_TYPEINFO, RVA_GAMEUI_TYPENAME,
                                 "N2UI7CGameUIE", RVA_GAMEUI_UPDATE);
    const bool wantAutoload = !(bad & (G_GATE | G_AUTOLOAD)) && menuVt && guard;
    if (!guard)
        Log("[menugame] autoload stays off: without catching with the game's runtime, a throw from the SavegameInfo getter would end the game\n");
    const bool wantHotjoin = !(bad & (G_GATE | G_HOTJOIN)) && gameVt;
    if (!wantAutoload && !wantHotjoin) {
        Log("[menugame] autoload and the forced autosave stay off: the checks above failed\n");
        return false;
    }
    if (!InstallGate(base)) {
        Log("[menugame] autoload and the forced autosave stay off: without the frame gate an update cannot tell a frame from a UI builder's step\n");
        return false;
    }
    if (wantAutoload) {
        // CheckBytes includes the exact 16-byte, relocation-free prologue.
        const bool observed = InstallHook(base+RVA_START_SAVEGAME,
            reinterpret_cast<void*>(&StartSavegameDetour),16,&g_originalStartSavegame);
        Log("[menu] accepted vanilla load observer %s\n", observed ? "ON" : "OFF (hook refused)");
        g_menuUpdate = (UpdateFn)(base + RVA_MENUUI_UPDATE);
        g_autoloadOn = StoreSlot(base + RVA_MENUUI_VTABLE + SLOT_UPDATE * sizeof(uintptr_t), base + RVA_MENUUI_UPDATE,
                                 (uintptr_t)&MenuUpdateDetour, "CMenuUI");
    }
    if (wantHotjoin) {
        g_gameUiUpdate = (UpdateFn)(base + RVA_GAMEUI_UPDATE);
        g_forceOn = StoreSlot(base + RVA_GAMEUI_VTABLE + SLOT_UPDATE * sizeof(uintptr_t), base + RVA_GAMEUI_UPDATE,
                              (uintptr_t)&GameUiUpdateDetour, "CGameUI");
    }
    Log("[menugame] autoload %s; forced autosave (hot join) and the in-game signal %s\n",
        g_autoloadOn ? "ON (CMenuUI update 1140a90 hooked in its vtable)" : "OFF",
        g_forceOn ? "ON (CGameUI update 100fb20 hooked in its vtable)" : "OFF");
    const bool ready=g_autoloadOn && g_forceOn && guard && NativeIo::Install();
    char dataDir[4096]{};
    if(Tpf2mpDataDirA(dataDir,sizeof(dataDir))) NativeControl::Start(dataDir,ready);
    Log("[native] pause/save/load controller %s\n",ready?"ON":"OFF");
    return g_autoloadOn && g_forceOn;
}

bool MenuGame_Install(uintptr_t gameBase, Tpf2mpLogFn log)
{
    if (log) g_log = log;
    static std::mutex* mtx = new std::mutex;
    static int result = -1;
    std::lock_guard<std::mutex> lk(*mtx);
    if (result < 0) {
        try {
            result = Install(gameBase) ? 1 : 0;
        } catch (...) {   // only our own code runs in Install, on the caller's thread
            Log("[menugame] install failed with an exception\n");
            result = 0;
        }
    }
    return result == 1;
}
