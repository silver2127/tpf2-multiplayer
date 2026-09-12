// Proxy alut.dll -- the earliest reliable foothold in the process.
//
// Why: the title-screen menu (UI::CMenuUI::CreatePageMain) is built long before
// a save is loaded, so injecting into a running game is far too late to touch
// it. alut.dll is a *static* import of TransportFever2.exe, so the loader maps
// it before the exe's entry point runs. Dropping ourselves in its place gets us
// running earlier than anything else we can do, in both the normal and the
// sandboxed instance, with no launcher and no timing race.
//
// Install: rename the stock alut.dll to alut_real.dll and put this beside it.
// Every export is forwarded straight through, so the game's audio is untouched.
//
// This is deliberately a thin loader: it only pulls in the DLLs that do the
// work (bridge, menu, slice, plugin host), from %LOCALAPPDATA%\tpf2mp\ or from
// its own folder.
#include <windows.h>
#include <cstdio>

#pragma comment(linker, "/export:alutCreateBufferFromFile=alut_real.alutCreateBufferFromFile")
#pragma comment(linker, "/export:alutCreateBufferFromFileImage=alut_real.alutCreateBufferFromFileImage")
#pragma comment(linker, "/export:alutCreateBufferHelloWorld=alut_real.alutCreateBufferHelloWorld")
#pragma comment(linker, "/export:alutCreateBufferWaveform=alut_real.alutCreateBufferWaveform")
#pragma comment(linker, "/export:alutExit=alut_real.alutExit")
#pragma comment(linker, "/export:alutGetError=alut_real.alutGetError")
#pragma comment(linker, "/export:alutGetErrorString=alut_real.alutGetErrorString")
#pragma comment(linker, "/export:alutGetMIMETypes=alut_real.alutGetMIMETypes")
#pragma comment(linker, "/export:alutGetMajorVersion=alut_real.alutGetMajorVersion")
#pragma comment(linker, "/export:alutGetMinorVersion=alut_real.alutGetMinorVersion")
#pragma comment(linker, "/export:alutInit=alut_real.alutInit")
#pragma comment(linker, "/export:alutInitWithoutContext=alut_real.alutInitWithoutContext")
#pragma comment(linker, "/export:alutLoadMemoryFromFile=alut_real.alutLoadMemoryFromFile")
#pragma comment(linker, "/export:alutLoadMemoryFromFileImage=alut_real.alutLoadMemoryFromFileImage")
#pragma comment(linker, "/export:alutLoadMemoryHelloWorld=alut_real.alutLoadMemoryHelloWorld")
#pragma comment(linker, "/export:alutLoadMemoryWaveform=alut_real.alutLoadMemoryWaveform")
#pragma comment(linker, "/export:alutLoadWAVFile=alut_real.alutLoadWAVFile")
#pragma comment(linker, "/export:alutLoadWAVMemory=alut_real.alutLoadWAVMemory")
#pragma comment(linker, "/export:alutSleep=alut_real.alutSleep")
#pragma comment(linker, "/export:alutUnloadWAV=alut_real.alutUnloadWAV")

#include "datadir.h"
#include "logarchive.h"

// Resolve a shipped file by name: %LOCALAPPDATA%\tpf2mp\<name> when that file
// exists, otherwise next to THIS proxy dll (the game dir). There is no third
// place: a DLL in neither fails to load, and the log names the path it tried.
static void resolveShipped(const wchar_t* name, wchar_t* out, size_t cch)
{
    wchar_t buf[MAX_PATH];
    wchar_t la[MAX_PATH];
    out[0] = 0;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        _snwprintf_s(buf, MAX_PATH, _TRUNCATE, L"%s\\tpf2mp\\%s", la, name);
        if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(out, cch, buf); return; }
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&resolveShipped, &self);
    if (self && GetModuleFileNameW(self, buf, MAX_PATH)) {
        wchar_t* s = wcsrchr(buf, L'\\');
        if (s) { s[1] = 0; wcscat_s(buf, MAX_PATH, name); wcscpy_s(out, cch, buf); }
    }
}

static void Log(const char* fmt, ...)
{
    // The log lives in the runtime data dir (%LOCALAPPDATA%\tpf2mp\data), like
    // every other log.
    wchar_t path[MAX_PATH];
    if (!Tpf2mpDataDirW(path, MAX_PATH, (const void*)&resolveShipped)) return;
    wcscat_s(path, MAX_PATH, L"tpf2_proxy.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

// Loading a dll from inside DllMain would deadlock on the loader lock, so the
// real work happens on its own thread. It still lands well before the menu is
// built -- the exe has not even reached its entry point yet.
static DWORD WINAPI LoadBridge(LPVOID)
{
    wchar_t bridgePath[MAX_PATH], menuPath[MAX_PATH], slicePath[MAX_PATH], hostPath[MAX_PATH];
    resolveShipped(L"tpf2_bridge_mp.dll",  bridgePath, MAX_PATH);
    resolveShipped(L"tpf2_menu.dll",       menuPath,   MAX_PATH);
    resolveShipped(L"tpf2_slice.dll",      slicePath,  MAX_PATH);
    resolveShipped(L"tpf2_pluginhost.dll", hostPath,   MAX_PATH);
    HMODULE h = LoadLibraryW(bridgePath);
    Log("[proxy] pid=%lu bridge load %s (err %lu) from %ls\n",
        GetCurrentProcessId(), h ? "OK" : "FAILED", h ? 0 : GetLastError(), bridgePath);
    HMODULE hm = LoadLibraryW(menuPath);
    Log("[proxy] pid=%lu menu load %s (err %lu) from %ls\n",
        GetCurrentProcessId(), hm ? "OK" : "FAILED", hm ? 0 : GetLastError(), menuPath);
    // The slice dll (command capture/replay) is optional: a missing file just
    // means no replication this run, the menu + bridge still come up.
    HMODULE hs = LoadLibraryW(slicePath);
    Log("[proxy] pid=%lu slice load %s (err %lu) from %ls\n",
        GetCurrentProcessId(), hs ? "OK" : "FAILED", hs ? 0 : GetLastError(), slicePath);
    // The plugin host loads everything in plugins\ -- the LAST name this proxy
    // will ever need to know. It is optional and loaded last on purpose: the
    // three DLLs above carry the working multiplayer stack, and a plugin must
    // not be able to stop them coming up. A missing host just means no plugins.
    HMODULE hh = LoadLibraryW(hostPath);
    Log("[proxy] pid=%lu pluginhost load %s (err %lu) from %ls\n",
        GetCurrentProcessId(), hh ? "OK" : "FAILED", hh ? 0 : GetLastError(), hostPath);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        // First: a non-ASCII profile folder gets an openable TPF2MP_DATADIR before the
        // game's CRT and Lua read the environment, and before every half of the mod
        // (the archive below included) resolves the data folder (datadir.h).
        Tpf2mpPublishDataDir();
        // Save the previous run's logs before anything of this run truncates or
        // appends to them (logarchive.h). Here, in DllMain and not on the loader
        // thread below: the game's entry point, which truncates stdout.txt, may
        // run before that thread gets the loader lock. File and registry calls
        // only, no LoadLibrary. In the game process only.
        Tpf2mpLogArchive arch = {};
        bool archived = false;
        {
            wchar_t exe[MAX_PATH], dir[MAX_PATH];
            const wchar_t* base = GetModuleFileNameW(nullptr, exe, MAX_PATH) ? wcsrchr(exe, L'\\') : nullptr;
            if (base && !_wcsicmp(base + 1, L"TransportFever2.exe") && GetModuleFileNameW(hinst, dir, MAX_PATH)) {
                wchar_t* s = wcsrchr(dir, L'\\');
                if (s) { s[1] = 0; archived = Tpf2mpArchiveLogsSafe(true, dir, &arch); }
            }
        }
        Log("[proxy] attached to pid %lu\n", GetCurrentProcessId());
        if (archived) {
            const wchar_t* leaf = wcsrchr(arch.folder, L'\\');
            Log("[proxy] the previous run's logs are in tpf2mp\\logs\\%ls (%d file(s), %d skipped)\n",
                leaf ? leaf + 1 : arch.folder, arch.files, arch.skipped);
        }
        CreateThread(nullptr, 0, LoadBridge, nullptr, 0, nullptr);
    }
    return TRUE;
}
