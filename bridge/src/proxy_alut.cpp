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
// This is deliberately a thin loader: it only pulls in the real bridge dll from
// the workshop out dir, so the bridge can be rebuilt without touching any file
// inside the game installation.
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

// Where our shipped files live, in order of preference. Portable first, the
// original dev workshop path last so the existing rig keeps working untouched.
static const wchar_t* FALLBACK_DIR =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\1066780"
    L"\\3710243057\\recon\\m4\\out";

// Resolve a shipped file by name: %LOCALAPPDATA%\tpf2mp\<name>, then next to THIS
// proxy dll (the game dir), then the dev workshop path. Writes the first that
// exists (or the fallback if none) into out.
#include "datadir.h"

static void resolveShipped(const wchar_t* name, wchar_t* out, size_t cch)
{
    wchar_t buf[MAX_PATH];
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        _snwprintf_s(buf, MAX_PATH, _TRUNCATE, L"%s\\tpf2mp\\%s", la, name);
        if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(out, cch, buf); return; }
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&resolveShipped, &self);
    if (self && GetModuleFileNameW(self, buf, MAX_PATH)) {
        wchar_t* s = wcsrchr(buf, L'\\');
        if (s) { s[1] = 0; wcscat_s(buf, MAX_PATH, name);
                 if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(out, cch, buf); return; } }
    }
    _snwprintf_s(out, cch, _TRUNCATE, L"%s\\%s", FALLBACK_DIR, name);
}

static void Log(const char* fmt, ...)
{
    // The log lives in the runtime data dir (%LOCALAPPDATA%\tpf2mp\data), like
    // every other log. Resolving it through resolveShipped sent it to the dev
    // workshop path on a fresh machine (no such dir -> log silently dropped).
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
    wchar_t bridgePath[MAX_PATH], menuPath[MAX_PATH], slicePath[MAX_PATH];
    resolveShipped(L"tpf2_bridge_mp.dll", bridgePath, MAX_PATH);
    resolveShipped(L"tpf2_menu.dll",      menuPath,   MAX_PATH);
    resolveShipped(L"tpf2_slice.dll",     slicePath,  MAX_PATH);
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
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        Log("[proxy] attached to pid %lu\n", GetCurrentProcessId());
        CreateThread(nullptr, 0, LoadBridge, nullptr, 0, nullptr);
    }
    return TRUE;
}
