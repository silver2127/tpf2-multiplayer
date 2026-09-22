// tpf2_pluginhost.dll -- loads and services native plugins.
//
// WHY THIS EXISTS
// The proxy used to name every DLL it loads:
//     resolveShipped(L"tpf2_bridge_mp.dll", ...);
//     resolveShipped(L"tpf2_menu.dll",      ...);
//     resolveShipped(L"tpf2_slice.dll",     ...);
// so a new feature meant editing and rebuilding the proxy -- the one component
// that must never break, because it is a static import of the exe and a bad one
// stops the game from starting at all. It also meant every DLL grew its own
// config parser and its own build guard (four parsers, two guards, ~77 scattered
// RVAs at the time of writing).
//
// This host is loaded by the proxy exactly like the other three, and then loads
// everything in the plugins directory itself. The proxy is edited once, ever.
//
// DELIBERATELY NOT A MOD MANAGER. There is no dependency resolution, no load
// ordering, no package format and no versioning of plugins against each other.
// Those are real problems for an ecosystem and this has one author; every one
// of them would be a week spent on a hypothetical second user.
//
// The three existing DLLs are NOT migrated here. They work, they carry the
// two-machine lockstep result, and moving them to prove an architecture is
// exactly the kind of change that loses a hard-won result. They keep their own
// loading and their own config until there is a reason to touch them.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include "../datadir.h"
#include "../hook.h"
#include "cfg.h"
#include "tpf2mp_plugin.h"

// ---------------------------------------------------------------------------
// The build these RVAs were measured on. Same three values slice_hook.cpp
// checks, in one place now: PE FileHeader.TimeDateStamp and
// OptionalHeader.SizeOfImage both change on every rebuild of the game.
// ---------------------------------------------------------------------------
static const DWORD GAME_EXE_TIMEDATESTAMP = 0x675abcc6;   // build 35924, 2024-12-11
static const DWORD GAME_EXE_SIZEOFIMAGE   = 0x046ce000;

static FILE*       g_log = nullptr;
static std::string g_dataDirA;
static const char* g_curPlugin = "host";   // prefix for log lines

static void LogRaw(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Host API implementation
// ---------------------------------------------------------------------------
static void ApiLog(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    const char* nl = (n && buf[n - 1] == '\n') ? "" : "\n";
    fprintf(g_log, "[%s] %s%s", g_curPlugin, buf, nl);
    fflush(g_log);
}

static int         ApiCfgInt (const char* s, const char* k, int def)         { return tpf2mp::CfgInt(s, k, def); }
static int         ApiCfgBool(const char* s, const char* k, int def)         { return tpf2mp::CfgBool(s, k, def != 0) ? 1 : 0; }
static const char* ApiCfgStr (const char* s, const char* k, const char* def) { return tpf2mp::CfgStr(s, k, def); }
static const char* ApiDataDir(void)                                          { return g_dataDirA.c_str(); }

// Base of TransportFever2.exe, or 0 when we are not in it. The DLLs get pulled
// into test harnesses too, and patching a Transport Fever RVA in some other
// process is an access violation at best.
static uintptr_t ApiModuleBase(void)
{
    HMODULE m = GetModuleHandleW(nullptr);
    if (!m) return 0;
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"TransportFever2.exe") != 0) return 0;
    return (uintptr_t)m;
}

static int ApiBuildOk(void)
{
    uintptr_t base = ApiModuleBase();
    if (!base) return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp == GAME_EXE_TIMEDATESTAMP
        && nt->OptionalHeader.SizeOfImage == GAME_EXE_SIZEOFIMAGE;
}

// Is [addr, addr+len) committed and readable? VirtualQuery describes one region,
// so the length check has to stay inside it.
static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static int ApiVerifyBytes(uintptr_t rva, const uint8_t* expected, uint32_t len)
{
    uintptr_t base = ApiModuleBase();
    if (!base || !expected || !len) return 0;
    const void* p = (const void*)(base + rva);
    if (!Readable(p, len)) return 0;
    return memcmp(p, expected, len) == 0 ? 1 : 0;
}

static int ApiInstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut)
{
    return InstallHook(target, detour, stealBytes, trampolineOut) ? 1 : 0;
}

static int ApiPatchBytes(uintptr_t rva, const uint8_t* bytes, uint32_t len)
{
    uintptr_t base = ApiModuleBase();
    if (!base || !bytes || !len) return 0;
    void* p = (void*)(base + rva);
    if (!Readable(p, len)) return 0;
    DWORD old = 0;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, bytes, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return 1;
}

static Tpf2mpHost g_api = {
    (uint32_t)sizeof(Tpf2mpHost),
    TPF2MP_ABI_MAJOR,
    ApiLog,
    ApiCfgInt, ApiCfgBool, ApiCfgStr,
    ApiModuleBase, ApiBuildOk, ApiVerifyBytes,
    ApiInstallHook, ApiPatchBytes,
    ApiDataDir,
};

// ---------------------------------------------------------------------------
// Plugin discovery
// ---------------------------------------------------------------------------
// Two directories, both optional:
//   <datadir>\plugins\          user-installed, survives a reinstall, writable
//   <this dll's dir>\plugins\   shipped by the installer, next to the binaries
// A name found in the first wins, so a user can shadow a shipped plugin without
// deleting it.
struct Found { std::wstring path; std::wstring leaf; };

static void ScanDir(const std::wstring& dir, std::vector<Found>& out)
{
    std::wstring pat = dir + L"*.dll";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring leaf(fd.cFileName);
        bool dup = false;
        for (size_t i = 0; i < out.size(); ++i)
            if (_wcsicmp(out[i].leaf.c_str(), leaf.c_str()) == 0) { dup = true; break; }
        if (dup) continue;
        Found f; f.path = dir + leaf; f.leaf = leaf;
        out.push_back(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::string Narrow(const std::wstring& w)
{
    char buf[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return std::string(buf);
}

// A plugin that faults in init must not take the game down with it: the host
// runs before the exe entry point, so an unhandled exception here means the
// game never starts and the player has no way to tell which plugin did it.
// In its own function because MSVC forbids __try in anything holding objects
// that need unwinding (LoadPlugins is full of std::wstring).
static int CallInitGuarded(Tpf2mpPluginInitFn init, Tpf2mpPluginInfo* info, bool* faulted)
{
    *faulted = false;
    __try {
        return init(&g_api, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return TPF2MP_ERR_FAILED;
    }
}

static const char* ResultName(int rc)
{
    switch (rc) {
    case TPF2MP_OK:           return "OK";
    case TPF2MP_ERR_ABI:      return "ABI MISMATCH";
    case TPF2MP_ERR_BUILD:    return "WRONG GAME BUILD";
    case TPF2MP_ERR_DISABLED: return "disabled in config";
    default:                  return "FAILED";
    }
}

static void LoadPlugins(const std::wstring& dataDirW, const std::wstring& selfDirW)
{
    std::vector<Found> found;
    ScanDir(dataDirW + L"plugins\\", found);
    ScanDir(selfDirW + L"plugins\\", found);

    if (found.empty()) {
        LogRaw("[host] no plugins found (looked in %lsplugins\\ and %lsplugins\\)\n",
               dataDirW.c_str(), selfDirW.c_str());
        return;
    }
    LogRaw("[host] %zu plugin(s) found\n", found.size());

    for (size_t i = 0; i < found.size(); ++i) {
        // Each plugin gets its own config section named after the DLL stem, so
        // "enabled=0" can switch one off without deleting the file. Checked
        // BEFORE loading: a disabled plugin should not even be mapped, because
        // its DllMain may already do work.
        std::string stem = Narrow(found[i].leaf);
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);

        // A per-plugin config beside the DLL (plugins\<stem>.cfg) is merged
        // over tpf2mp.cfg BEFORE the enabled check, so it can carry enabled=0
        // too. Plugins from a separate installer ship their settings this way
        // rather than editing tpf2mp.cfg, which another product may own.
        {
            std::wstring pcfg = found[i].path;
            size_t wd = pcfg.find_last_of(L'.');
            if (wd != std::wstring::npos) pcfg = pcfg.substr(0, wd);
            pcfg += L".cfg";
            if (tpf2mp::CfgMergeFile(pcfg.c_str()))
                LogRaw("[host] %s: merged %ls\n", stem.c_str(), pcfg.c_str());
        }

        if (!tpf2mp::CfgBool(stem.c_str(), "enabled", true)) {
            LogRaw("[host] %s: skipped (enabled=0)\n", stem.c_str());
            continue;
        }

        HMODULE h = LoadLibraryW(found[i].path.c_str());
        if (!h) {
            LogRaw("[host] %ls: LoadLibrary FAILED (err %lu)\n",
                   found[i].leaf.c_str(), GetLastError());
            continue;
        }
        Tpf2mpPluginInitFn init =
            (Tpf2mpPluginInitFn)GetProcAddress(h, "Tpf2mpPluginInit");
        if (!init) {
            LogRaw("[host] %ls: no Tpf2mpPluginInit export -- not a plugin, left loaded\n",
                   found[i].leaf.c_str());
            continue;
        }
        Tpf2mpPluginInfo info;
        memset(&info, 0, sizeof(info));
        g_curPlugin = stem.c_str();
        bool faulted = false;
        int rc = CallInitGuarded(init, &info, &faulted);
        g_curPlugin = "host";
        if (faulted) {
            LogRaw("[host] %s: EXCEPTION in Tpf2mpPluginInit -- plugin left inert\n",
                   stem.c_str());
            continue;
        }
        LogRaw("[host] %s: %s%s%s%s -> %s\n", stem.c_str(),
               info.name ? info.name : stem.c_str(),
               info.version ? " " : "", info.version ? info.version : "",
               info.summary ? "" : "", ResultName(rc));
        if (info.summary) LogRaw("[host]   %s\n", info.summary);
    }
}

// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    wchar_t dataDir[MAX_PATH] = L"";
    if (!Tpf2mpDataDirW(dataDir, MAX_PATH, (const void*)&InitThread)) return 1;

    wchar_t selfPath[MAX_PATH] = L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InitThread, &self);
    GetModuleFileNameW(self, selfPath, MAX_PATH);
    wchar_t* slash = wcsrchr(selfPath, L'\\');
    if (slash) slash[1] = 0;

    {
        std::wstring lp(dataDir);
        lp += L"tpf2mp_host.log";
        HANDLE lh = CreateFileW(lp.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lh != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)lh, _O_WRONLY | _O_APPEND | _O_BINARY);
            if (fd >= 0) g_log = _fdopen(fd, "ab");
            else CloseHandle(lh);
        }
    }

    g_dataDirA.assign(MAX_PATH * 2, '\0');
    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, dataDir, -1, narrow, sizeof(narrow), nullptr, nullptr);
    g_dataDirA = narrow;

    const std::string& cfg = tpf2mp::CfgLoad(selfPath, dataDir);
    LogRaw("[host] pid=%lu abi=%d\n", GetCurrentProcessId(), TPF2MP_ABI_MAJOR);
    LogRaw("[host] data dir: %ls\n", dataDir);
    LogRaw("[host] config: %s\n", cfg.empty() ? "(none found -- built-in defaults)" : cfg.c_str());
    LogRaw("[host] game build: %s\n",
           ApiModuleBase() == 0 ? "not TransportFever2.exe -- plugins that patch will refuse"
                                : (ApiBuildOk() ? "MATCHES 35924" : "MISMATCH -- plugins that patch will refuse"));

    LoadPlugins(dataDir, selfPath);
    LogRaw("[host] done\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        // LoadLibrary from inside DllMain deadlocks on the loader lock, and we
        // load plugins -- so all of it happens on its own thread. It still runs
        // long before the exe entry point.
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
