// datadir.h -- the ONE runtime data directory every half of the mod agrees on.
//
// The bridge DLL, the slice DLL and the Lua game script exchange commands
// through files. On the dev rig they all pointed at a Steam workshop folder
// (bridge: its own DLL dir; slice + Lua: compile-time literals) -- three
// independent path sources that only agreed by construction, which is why the
// mod could not be installed anywhere else (docs/PORTABILITY.md item 2).
//
// Shipping layout:
//   binaries + cfg   <game dir>  (alut.dll proxy, tpf2_bridge_mp.dll, tpf2_menu.dll,
//                                 tpf2_slice.dll, tpf2_bridge_mp.cfg, tpf2_slice.cfg)
//   runtime data     %LOCALAPPDATA%\tpf2mp\data   (identity, events, captures,
//                                 injects, status, logs -- everything written at run time)
// Program Files is not writable by the game process, LOCALAPPDATA is; the Lua
// side finds the same directory via os.getenv("LOCALAPPDATA").
//
// Dev-rig compatibility: if TPF2MP_DATADIR is set in the environment it wins
// (the harness can pin the old workshop out dir); otherwise LOCALAPPDATA; if
// even that is missing, fall back to the directory the calling DLL lives in.
// Header-only so the bridge and slice builds stay single-file.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Fills `out` (MAX_PATH wide chars) with the data dir INCLUDING a trailing
// backslash, creating it if needed. `self` = any address inside the calling
// module (for the last-resort fallback). Returns false only if nothing could
// be resolved at all.
static inline bool Tpf2mpDataDirW(wchar_t* out, size_t cch, const void* self)
{
    wchar_t buf[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"TPF2MP_DATADIR", buf, MAX_PATH) && buf[0]) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%s", buf);
    } else if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) && buf[0]) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%s\\tpf2mp", buf);
        CreateDirectoryW(out, nullptr);
        _snwprintf_s(out, cch, _TRUNCATE, L"%s\\tpf2mp\\data", buf);
    } else {
        HMODULE h = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)self, &h) || !GetModuleFileNameW(h, buf, MAX_PATH)) return false;
        wchar_t* p = wcsrchr(buf, L'\\'); if (p) *p = 0;
        _snwprintf_s(out, cch, _TRUNCATE, L"%s", buf);
    }
    CreateDirectoryW(out, nullptr);
    size_t n = wcslen(out);
    if (n + 1 < cch && out[n - 1] != L'\\') { out[n] = L'\\'; out[n + 1] = 0; }
    return true;
}

// Narrow (UTF-8) convenience for code that formats paths with snprintf.
static inline bool Tpf2mpDataDirA(char* out, size_t cch, const void* self)
{
    wchar_t w[MAX_PATH];
    if (!Tpf2mpDataDirW(w, MAX_PATH, self)) return false;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cch, nullptr, nullptr) > 0;
}
