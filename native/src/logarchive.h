// logarchive.h -- every log a bug report needs, gathered in one folder:
//
//   %LOCALAPPDATA%\tpf2mp\logs\<yyyymmdd-hhmmss>-previous\   the last run, saved at the next start
//   %LOCALAPPDATA%\tpf2mp\logs\<yyyymmdd-hhmmss>-now\        a copy taken with the menu's OPEN LOGS
//
// Why at start: a restart destroys the evidence. The game truncates its own log
// (stdout.txt) on launch, the slice, bridge and lobby recreate theirs, and the
// proxy and the Lua script append theirs to the last run's. The proxy runs this
// from DllMain, before the game's entry point and before it loads any other DLL
// of ours, so the previous run's files are still intact.
//
// What goes in: the data folder's *.log (MOVED at start, since the next run
// writes fresh ones), tpf2_menu.log and netpunch\*.log from the game folder, the
// game's stdout.txt (the Lua script's log lines), and the game's crash dumps
// since the last archive with the log the game keeps beside each one
// (<id>_stdout_old.txt). The newest TPF2_LOG_KEEP folders of each kind are kept.
// about.txt names the files without the user's paths: these folders get sent in
// bug reports.
//
// Built for a game that crashes a lot, because that is when the logs matter:
//  - it must never stop the game starting: the entry point is SEH-guarded
//    (Tpf2mpArchiveLogsSafe), and every step that fails is noted and skipped;
//  - a crashed game can still be exiting (Windows Error Reporting keeps it alive
//    for seconds) and hold its logs open when the player relaunches: a log that
//    cannot be moved is copied instead, so its content is not lost when the new
//    run recreates the file;
//  - two starts in quick succession take turns (a named mutex, bounded wait);
//  - the time and disk one start can cost are bounded: logs and dumps keep
//    their last TPF2_LOG_TAIL_BYTES, a dump over TPF2_DUMP_MAX_BYTES is skipped,
//    at most TPF2_DUMPS_PER_RUN dumps are copied, and all copying stops at
//    TPF2_COPY_BUDGET_BYTES; moves are renames and cost nothing;
//  - a crash loop cannot fill the disk: each start keeps at most TPF2_LOG_KEEP
//    archives of its kind, and a half-written archive is pruned like any other.
//
// Header-only so the proxy and menu builds stay single-file.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "datadir.h"

#pragma comment(lib, "advapi32.lib")

static const ULONGLONG TPF2_LOG_TAIL_BYTES    = 32ull << 20;
static const ULONGLONG TPF2_DUMP_MAX_BYTES    = 64ull << 20;
static const ULONGLONG TPF2_COPY_BUDGET_BYTES = 200ull << 20;
static const int       TPF2_DUMPS_PER_RUN     = 3;
static const int       TPF2_LOG_KEEP          = 2;   // per kind: "previous" runs, "now" copies

struct Tpf2mpLogArchive {
    wchar_t folder[MAX_PATH];   // the folder written (empty when nothing was found)
    wchar_t root[MAX_PATH];     // ...\tpf2mp\logs\ (trailing backslash)
    int files;                  // files placed in the folder
    int skipped;                // files that could not be moved or copied
};

// Copies src to dst while another process may still be writing it. A file over
// the cap keeps its tail: the end of a log is what a bug report needs.
// *copied is the number of bytes written.
static inline bool LaCopyShared(const wchar_t* src, const wchar_t* dst, ULONGLONG* copied)
{
    *copied = 0;
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(in, &size);
    if ((ULONGLONG)size.QuadPart > TPF2_LOG_TAIL_BYTES) {
        LARGE_INTEGER off;
        off.QuadPart = size.QuadPart - (LONGLONG)TPF2_LOG_TAIL_BYTES;
        SetFilePointerEx(in, off, nullptr, FILE_BEGIN);
    }
    HANDLE out = CreateFileW(dst, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return false; }
    const DWORD chunk = 1 << 20;
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, chunk);
    bool ok = buf != nullptr;
    DWORD got = 0, put = 0;
    while (ok && ReadFile(in, buf, chunk, &got, nullptr) && got > 0) {
        ok = WriteFile(out, buf, got, &put, nullptr) && put == got;
        *copied += put;
    }
    FILETIME ft;
    if (GetFileTime(in, nullptr, nullptr, &ft)) SetFileTime(out, nullptr, nullptr, &ft);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(out);
    CloseHandle(in);
    return ok;
}

static inline void LaNote(FILE* f, const wchar_t* fmt, ...)
{
    if (!f) return;
    wchar_t w[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(w, _countof(w), _TRUNCATE, fmt, ap);
    va_end(ap);
    char u[3072];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), nullptr, nullptr) > 0) { fputs(u, f); fflush(f); }
}

// Steam's install folder with a trailing backslash, or empty.
static inline void LaSteamRoot(wchar_t* out, DWORD cch)
{
    out[0] = 0;
    DWORD n = cch * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, out, &n) != ERROR_SUCCESS) {
        n = cch * sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ,
                         nullptr, out, &n) != ERROR_SUCCESS) { out[0] = 0; return; }
    }
    for (wchar_t* p = out; *p; ++p) if (*p == L'/') *p = L'\\';
    size_t len = wcslen(out);
    if (len && out[len - 1] != L'\\' && len + 1 < cch) { out[len] = L'\\'; out[len + 1] = 0; }
}

// yyyymmdd-hhmmss-<kind>[n]: returns the kind (0 = previous, 1 = now), or -1.
static inline int LaArchiveKind(const wchar_t* n)
{
    if (wcslen(n) < 18) return -1;
    for (int i = 0; i < 15; i++) {
        if (i == 8) { if (n[i] != L'-') return -1; }
        else if (n[i] < L'0' || n[i] > L'9') return -1;
    }
    if (n[15] != L'-') return -1;
    if (!wcsncmp(n + 16, L"previous", 8)) return 0;
    if (!wcsncmp(n + 16, L"now", 3)) return 1;
    return -1;
}

// Deletes a folder this code wrote (files only; it never writes subfolders).
static inline void LaRemoveArchive(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

// previousSession: run at process start; moves the data folder's logs.
// Otherwise copies everything as it is now. gameDir has a trailing backslash.
static inline bool Tpf2mpArchiveLogs(bool previousSession, const wchar_t* gameDir, Tpf2mpLogArchive* out)
{
    memset(out, 0, sizeof(*out));
    wchar_t data[MAX_PATH];
    if (!Tpf2mpDataDirW(data, MAX_PATH, nullptr)) return false;
    wchar_t la[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH) && la[0]) {
        // the parent too: with TPF2MP_DATADIR set nothing else creates it
        wchar_t parent[MAX_PATH];
        _snwprintf_s(parent, _TRUNCATE, L"%s\\tpf2mp", la);
        CreateDirectoryW(parent, nullptr);
        _snwprintf_s(out->root, _TRUNCATE, L"%s\\tpf2mp\\logs\\", la);
    } else {
        _snwprintf_s(out->root, _TRUNCATE, L"%slogs\\", data);
    }
    CreateDirectoryW(out->root, nullptr);

    // The existing archives, by kind; the newest one's time bounds the dumps.
    FILETIME since = {};
    wchar_t names[2][64][32];
    size_t count[2] = { 0, 0 };
    {
        wchar_t pat[MAX_PATH];
        _snwprintf_s(pat, _TRUNCATE, L"%s*", out->root);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                int kind = LaArchiveKind(fd.cFileName);
                if (kind < 0) continue;
                if (CompareFileTime(&fd.ftCreationTime, &since) > 0) since = fd.ftCreationTime;
                if (count[kind] < 64 && wcslen(fd.cFileName) < 32) wcscpy_s(names[kind][count[kind]++], fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (since.dwLowDateTime == 0 && since.dwHighDateTime == 0) {
        // no archive yet: dumps of the last day
        GetSystemTimeAsFileTime(&since);
        ULARGE_INTEGER u = { since.dwLowDateTime, since.dwHighDateTime };
        u.QuadPart -= 24ull * 3600ull * 10000000ull;
        since.dwLowDateTime = u.LowPart;
        since.dwHighDateTime = u.HighPart;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    const int myKind = previousSession ? 0 : 1;
    const wchar_t* tag = previousSession ? L"previous" : L"now";
    _snwprintf_s(out->folder, _TRUNCATE, L"%s%04u%02u%02u-%02u%02u%02u-%s", out->root,
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tag);
    bool made = CreateDirectoryW(out->folder, nullptr) != 0;
    for (int k = 2; !made && k < 10; k++) {
        _snwprintf_s(out->folder, _TRUNCATE, L"%s%04u%02u%02u-%02u%02u%02u-%s%d", out->root,
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tag, k);
        made = CreateDirectoryW(out->folder, nullptr) != 0;
    }
    if (!made) { out->folder[0] = 0; return false; }

    wchar_t aboutPath[MAX_PATH];
    _snwprintf_s(aboutPath, _TRUNCATE, L"%s\\about.txt", out->folder);
    FILE* about = nullptr;
    _wfopen_s(&about, aboutPath, L"wb");
    LaNote(about, L"TpF2 Multiplayer logs, gathered %04u-%02u-%02u %02u:%02u:%02u (%s)\r\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
           previousSession ? L"the previous run, saved when the game started again" : L"the running game, copied from the menu");
    LaNote(about, L"Paths below are relative: data = %%LOCALAPPDATA%%\\tpf2mp\\data, game = the Transport Fever 2 folder.\r\n\r\n");

    ULONGLONG budget = TPF2_COPY_BUDGET_BYTES;
    auto copyNoted = [&](const wchar_t* src, const wchar_t* dst, const wchar_t* shown, const wchar_t* why) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA ad;
        ULONGLONG size = GetFileAttributesExW(src, GetFileExInfoStandard, &ad)
                         ? ((ULONGLONG)ad.nFileSizeHigh << 32) | ad.nFileSizeLow : 0;
        ULONGLONG need = size > TPF2_LOG_TAIL_BYTES ? TPF2_LOG_TAIL_BYTES : size;
        if (need > budget) {
            LaNote(about, L"  %-52s skipped: this archive's copy budget is used up\r\n", shown);
            return false;
        }
        ULONGLONG copied = 0;
        bool ok = LaCopyShared(src, dst, &copied);
        budget -= copied < budget ? copied : budget;
        if (ok)
            LaNote(about, L"  %-52s %12llu bytes%s%s\r\n", shown, size,
                   size > TPF2_LOG_TAIL_BYTES ? L" (only the last 32 MB kept)" : L"", why);
        else
            LaNote(about, L"  %-52s could not be read\r\n", shown);
        return ok;
    };
    auto place = [&](const wchar_t* src, const wchar_t* destName, const wchar_t* shown, bool move) {
        wchar_t dst[MAX_PATH];
        _snwprintf_s(dst, _TRUNCATE, L"%s\\%s", out->folder, destName);
        bool ok = false;
        if (move) {
            WIN32_FILE_ATTRIBUTE_DATA ad;
            ULONGLONG size = GetFileAttributesExW(src, GetFileExInfoStandard, &ad)
                             ? ((ULONGLONG)ad.nFileSizeHigh << 32) | ad.nFileSizeLow : 0;
            ok = MoveFileExW(src, dst, 0) != 0;   // a rename only: never a copy that could half-succeed
            if (ok) LaNote(about, L"  %-52s %12llu bytes\r\n", shown, size);
            // Still held open: a game that crashed and is not gone yet, or another
            // running instance. Copy it, or the next run's DLLs recreate it empty.
            else ok = copyNoted(src, dst, shown, L" (in use: copied)");
        } else {
            ok = copyNoted(src, dst, shown, L"");
        }
        if (ok) out->files++; else out->skipped++;
    };
    auto each = [&](const wchar_t* dir, const wchar_t* mask, const wchar_t* prefix, const wchar_t* shownDir, bool move) {
        wchar_t pat[MAX_PATH];
        _snwprintf_s(pat, _TRUNCATE, L"%s%s", dir, mask);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t src[MAX_PATH], dest[MAX_PATH], shown[MAX_PATH];
            _snwprintf_s(src, _TRUNCATE, L"%s%s", dir, fd.cFileName);
            _snwprintf_s(dest, _TRUNCATE, L"%s%s", prefix, fd.cFileName);
            _snwprintf_s(shown, _TRUNCATE, L"%s\\%s", shownDir, fd.cFileName);
            place(src, dest, shown, move);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };

    // 1. the data folder: bridge, slice, proxy, plugin host, company logs
    each(data, L"*.log", L"", L"data", previousSession);
    // 2. the game folder: the menu and the lobby
    if (gameDir && gameDir[0]) {
        wchar_t net[MAX_PATH];
        _snwprintf_s(net, _TRUNCATE, L"%snetpunch\\", gameDir);
        each(gameDir, L"tpf2_menu.log", L"game_", L"game", false);
        each(net, L"*.log", L"game_netpunch_", L"game\\netpunch", false);
    }
    // 3. the game's own log and crash dumps (the Steam user that ran the game last)
    wchar_t steam[MAX_PATH];
    LaSteamRoot(steam, MAX_PATH);
    wchar_t best[MAX_PATH] = L"";
    if (steam[0]) {
        wchar_t pat[MAX_PATH];
        FILETIME bestT = {};
        _snwprintf_s(pat, _TRUNCATE, L"%suserdata\\*", steam);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                wchar_t cd[MAX_PATH], so[MAX_PATH];
                _snwprintf_s(cd, _TRUNCATE, L"%suserdata\\%s\\1066780\\local\\crash_dump\\", steam, fd.cFileName);
                _snwprintf_s(so, _TRUNCATE, L"%sstdout.txt", cd);
                WIN32_FILE_ATTRIBUTE_DATA ad;
                if (GetFileAttributesExW(so, GetFileExInfoStandard, &ad) && CompareFileTime(&ad.ftLastWriteTime, &bestT) > 0) {
                    bestT = ad.ftLastWriteTime;
                    wcscpy_s(best, cd);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (best[0]) {
        wchar_t so[MAX_PATH];
        _snwprintf_s(so, _TRUNCATE, L"%sstdout.txt", best);
        place(so, L"game_stdout.txt", L"game log (stdout.txt, includes the Lua script)", false);

        // newest dumps since the last archive, each with the log the game kept beside it
        struct Dump { wchar_t name[80]; FILETIME t; ULONGLONG size; } dumps[64];
        int nd = 0;
        wchar_t pat[MAX_PATH];
        _snwprintf_s(pat, _TRUNCATE, L"%s*.dmp", best);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (CompareFileTime(&fd.ftLastWriteTime, &since) <= 0 || wcslen(fd.cFileName) >= 80) continue;
                Dump d;
                wcscpy_s(d.name, fd.cFileName);
                d.t = fd.ftLastWriteTime;
                d.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (nd < 64) dumps[nd++] = d;
                else {   // keep the 64 newest
                    int oldest = 0;
                    for (int i = 1; i < nd; i++) if (CompareFileTime(&dumps[i].t, &dumps[oldest].t) < 0) oldest = i;
                    if (CompareFileTime(&d.t, &dumps[oldest].t) > 0) dumps[oldest] = d;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        for (int i = 0; i < nd; i++)   // newest first
            for (int j = i + 1; j < nd; j++)
                if (CompareFileTime(&dumps[j].t, &dumps[i].t) > 0) { Dump t = dumps[i]; dumps[i] = dumps[j]; dumps[j] = t; }
        if (nd > TPF2_DUMPS_PER_RUN)
            LaNote(about, L"  %s%d crash dumps since the last archive; the newest %d are here\r\n",
                   nd == 64 ? L"at least " : L"", nd, TPF2_DUMPS_PER_RUN);
        for (int i = 0; i < nd && i < TPF2_DUMPS_PER_RUN; i++) {
            wchar_t src[MAX_PATH], dest[MAX_PATH], shown[MAX_PATH], base[80];
            wcscpy_s(base, dumps[i].name);
            wchar_t* dot = wcsrchr(base, L'.');
            if (dot) *dot = 0;
            if (dumps[i].size > TPF2_DUMP_MAX_BYTES) {
                LaNote(about, L"  crash dump %-41s skipped: %llu bytes\r\n", dumps[i].name, dumps[i].size);
                out->skipped++;
            } else {
                _snwprintf_s(src, _TRUNCATE, L"%s%s", best, dumps[i].name);
                _snwprintf_s(dest, _TRUNCATE, L"crash_%s", dumps[i].name);
                _snwprintf_s(shown, _TRUNCATE, L"crash dump %s", dumps[i].name);
                place(src, dest, shown, false);
            }
            _snwprintf_s(src, _TRUNCATE, L"%s%s_stdout_old.txt", best, base);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                _snwprintf_s(dest, _TRUNCATE, L"crash_%s_stdout.txt", base);
                _snwprintf_s(shown, _TRUNCATE, L"game log of that crash (%s)", base);
                place(src, dest, shown, false);
            }
        }
    }
    if (about) fclose(about);

    if (out->files == 0) {
        LaRemoveArchive(out->folder);
        out->folder[0] = 0;
        return false;
    }

    // Keep the newest TPF2_LOG_KEEP of this kind, this one included (names sort
    // by time). A folder a killed start left half-written goes the same way.
    size_t n = count[myKind];
    if (n + 1 > (size_t)TPF2_LOG_KEEP) {
        wchar_t (*list)[32] = names[myKind];
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++)
                if (wcscmp(list[j], list[i]) < 0) { wchar_t t[32]; wcscpy_s(t, list[i]); wcscpy_s(list[i], list[j]); wcscpy_s(list[j], t); }
        size_t drop = n + 1 - TPF2_LOG_KEEP;
        for (size_t i = 0; i < drop && i < n; i++) {
            wchar_t p[MAX_PATH];
            _snwprintf_s(p, _TRUNCATE, L"%s%s", out->root, list[i]);
            LaRemoveArchive(p);
        }
    }
    return true;
}

// The entry point both callers use. A fault anywhere above must never cost the
// game its start (the proxy calls this from DllMain) or the menu its thread:
// it is caught and reported as "nothing archived". Starts in quick succession
// (a relaunch after a crash, the rig's instances) take turns on the same
// folders; a wait longer than 10 s gives up rather than delay the game.
static inline bool Tpf2mpArchiveLogsSafe(bool previousSession, const wchar_t* gameDir, Tpf2mpLogArchive* out)
{
    memset(out, 0, sizeof(*out));
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\tpf2mp_log_archive");
    if (m) {
        DWORD w = WaitForSingleObject(m, 10000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) { CloseHandle(m); return false; }
    }
    bool ok = false;
    __try {
        ok = Tpf2mpArchiveLogs(previousSession, gameDir, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (m) { ReleaseMutex(m); CloseHandle(m); }
    return ok;
}
