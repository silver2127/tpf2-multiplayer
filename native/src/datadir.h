// datadir.h -- the ONE runtime data directory every half of the mod agrees on.
//
// The bridge DLL, the slice DLL and the Lua game script exchange commands
// through files. On the dev rig they all pointed at a Steam workshop folder
// (bridge: its own DLL dir; slice + Lua: compile-time literals) -- three
// independent path sources that only agreed by construction, which is why the
// mod could not be installed anywhere else.
//
// Shipping layout:
//   binaries + cfg   <game dir>  (alut.dll proxy, tpf2_bridge_mp.dll, tpf2_menu.dll,
//                                 tpf2_slice.dll, tpf2_slice.cfg)
//   runtime data     %LOCALAPPDATA%\tpf2mp\data   (identity, events, captures,
//                                 injects, status, logs -- everything written at run time)
// Program Files is not writable by the game process, LOCALAPPDATA is; the Lua
// side finds the same directory via os.getenv("LOCALAPPDATA").
//
// TPF2MP_DATADIR, if set in the environment, wins; otherwise LOCALAPPDATA.
// There is no third candidate: the Lua side follows TPF2MP_DATADIR only once
// that folder holds tpf2_instance.txt (the bridge writes it at start) and
// otherwise uses LOCALAPPDATA, so any other directory would leave the halves
// talking past each other.
// Header-only so the bridge and slice builds stay single-file.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

// A pin is taken as ABSOLUTE. A relative or Unix-style one ("/tmp/tpf2mp-data" from a
// Steam launch option under Proton, 2026-09-21) names a different folder in every
// process: Windows resolves a rootless path against the process's CURRENT DRIVE, so
// the game (working folder on Z:) created and used /tmp/tpf2mp-data while the lobby
// it started from a folder on C: looked for C:\tmp\tpf2mp-data and a resync died
// with "[Errno 2] No such file or directory". GetFullPathNameW here, in the game
// process, resolves it once, against the game's folder; Tpf2mpPublishDataDir then
// hands the absolute form to the Lua and every child process.
static inline bool Tpf2mpPathIsAbsoluteW(const wchar_t* p)
{
    if (!p || !p[0]) return false;
    if (p[0] == L'\\' && p[1] == L'\\') return true;                         // UNC
    return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z'))
        && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');                  // X:\ or X:/
}

static inline void Tpf2mpAbsoluteW(const wchar_t* in, wchar_t* out, size_t cch)
{
    if (!Tpf2mpPathIsAbsoluteW(in)) {
        DWORD n = GetFullPathNameW(in, (DWORD)cch, out, nullptr);
        if (n > 0 && n < cch) return;
    }
    _snwprintf_s(out, cch, _TRUNCATE, L"%s", in);
}

// Fills `out` (MAX_PATH wide chars) with the data dir INCLUDING a trailing
// backslash, creating it if needed. Returns false when neither TPF2MP_DATADIR
// nor LOCALAPPDATA is set. `self` is unused; the callers still pass it.
static inline bool Tpf2mpDataDirW(wchar_t* out, size_t cch, const void* self)
{
    (void)self;
    wchar_t buf[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"TPF2MP_DATADIR", buf, MAX_PATH) && buf[0]) {
        Tpf2mpAbsoluteW(buf, out, cch);
    } else if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) && buf[0]) {
        _snwprintf_s(out, cch, _TRUNCATE, L"%s\\tpf2mp", buf);
        CreateDirectoryW(out, nullptr);
        _snwprintf_s(out, cch, _TRUNCATE, L"%s\\tpf2mp\\data", buf);
    } else {
        return false;
    }
    CreateDirectoryW(out, nullptr);
    size_t n = wcslen(out);
    if (n + 1 < cch && out[n - 1] != L'\\') { out[n] = L'\\'; out[n + 1] = 0; }
    return true;
}

// NON-ASCII WINDOWS USER NAMES. The game's Lua os.getenv is the CRT's narrow getenv
// (ANSI code page bytes), and its io.open treats every path as UTF-8 and converts it
// to wide for _wfopen (decompiled 0x7f4a0 -> 0x23a5f10). A profile folder with a
// non-ASCII name is therefore unopenable from Lua: the conversion throws, and the
// game reported "finding the data folder failed: file (0000000000000000)" and
// aborted creating a game (2026-09-12). Before the game's entry point runs, the
// proxy publishes TPF2MP_DATADIR as a path both halves can open -- the folder's 8.3
// short name, pure ASCII, which also makes the slice's ANSI paths safe; UTF-8 when
// the volume has no short names (only Lua is helped then). An ASCII profile, or a
// TPF2MP_DATADIR already set by a harness, is left exactly as it was.
static inline bool Tpf2mpIsAsciiW(const wchar_t* s)
{
    for (; *s; ++s) if (*s > 0x7f) return false;
    return true;
}

static inline void Tpf2mpPublishDataDir()
{
    wchar_t pin[MAX_PATH] = L"";
    const bool pinned = GetEnvironmentVariableW(L"TPF2MP_DATADIR", pin, MAX_PATH) && pin[0];
    if (pinned && Tpf2mpPathIsAbsoluteW(pin)) return;   // a harness pin is left exactly as it was
    wchar_t dir[MAX_PATH];
    if (!Tpf2mpDataDirW(dir, MAX_PATH, nullptr)) return;   // creates the folder; absolute
    wchar_t shortp[MAX_PATH] = L"";
    bool ascii = Tpf2mpIsAsciiW(dir);
    if (!pinned && ascii) return;                          // the shipping folder, ASCII: nothing to publish
    if (!ascii) {
        DWORD n = GetShortPathNameW(dir, shortp, MAX_PATH);
        ascii = n > 0 && n < MAX_PATH && Tpf2mpIsAsciiW(shortp);
    }
    const wchar_t* pub = (!Tpf2mpIsAsciiW(dir) && ascii) ? shortp : dir;
    // The game's CRT keeps its own narrow copy of the environment once initialised;
    // its getenv (what Lua's os.getenv calls) reads that copy, so set it there too.
    // UTF-8 bytes for the no-short-name case, since that is what the game's io.open
    // expects. Done BEFORE SetEnvironmentVariableW: the CRT also writes the OS block,
    // and the wide call below must be the one that stays.
    char narrow[MAX_PATH * 4];
    if (WideCharToMultiByte(ascii ? CP_ACP : CP_UTF8, 0, pub, -1, narrow, (int)sizeof(narrow), nullptr, nullptr) > 0) {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        using PutEnvS = int (*)(const char*, const char*);
        PutEnvS putenvS = ucrt ? (PutEnvS)GetProcAddress(ucrt, "_putenv_s") : nullptr;
        if (putenvS) putenvS("TPF2MP_DATADIR", narrow);
    }
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", pub);
}

// Narrow (UTF-8) convenience for code that formats paths with snprintf.
static inline bool Tpf2mpDataDirA(char* out, size_t cch, const void* self)
{
    wchar_t w[MAX_PATH];
    if (!Tpf2mpDataDirW(w, MAX_PATH, self)) return false;
    // CP_ACP, not CP_UTF8: every consumer hands this to the narrow CRT
    // (fopen/_fsopen), which takes the ANSI codepage. On an ASCII profile the
    // two agree; on a user name outside ASCII the UTF-8 bytes named a folder
    // that does not exist, the slice log failed to open and the slice gave
    // up silently -- no hooks, no replication, nothing logged (2026-09-10).
    return WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cch, nullptr, nullptr) > 0;
}
