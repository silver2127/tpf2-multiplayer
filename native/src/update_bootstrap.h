#pragma once
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "datadir.h"

// ABI 1: one immutable release, selected before the game's CRT starts.
// Missing/incomplete installations fall back as a whole to the MSI payload.
static inline void Tpf2mpPinRelease()
{
    SetEnvironmentVariableW(L"TPF2MP_RELEASE_ROOT", nullptr);
    using PutEnv = int (*)(const char*, const char*);
    auto crt = GetModuleHandleW(L"ucrtbase.dll");
    auto put = crt ? (PutEnv)GetProcAddress(crt, "_putenv_s") : nullptr;
    if (put) put("TPF2MP_RELEASE_ROOT", "");
    wchar_t local[MAX_PATH], root[MAX_PATH], pointer[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    _snwprintf_s(pointer, _TRUNCATE, L"%s\\tpf2mp\\updates\\active.txt", local);
    FILE* file = nullptr;
    if (_wfopen_s(&file, pointer, L"rb") || !file) return;
    char ver[32] = {}; size_t count = fread(ver, 1, sizeof(ver) - 1, file); fclose(file);
    if (!count || count >= sizeof(ver) - 1) return;
    int dots = 0, digits = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ver[i] == '.') { if (!digits || digits > 5) return; ++dots; digits = 0; }
        else if (ver[i] >= '0' && ver[i] <= '9') ++digits;
        else return;
    }
    if (dots != 2 || !digits || digits > 5) return;
    // A newer MSI must take precedence over an older cached update.
    wchar_t installed[MAX_PATH];
    bool installedMp = false;
    if (GetModuleFileNameW(nullptr, installed, MAX_PATH)) {
        wchar_t* slash = wcsrchr(installed, L'\\');
        if (slash) {
            slash[1] = 0; wcscat_s(installed, L"tpf2mp_version.txt");
            FILE* baseline = nullptr;
            if (!_wfopen_s(&baseline, installed, L"rb") && baseline) {
                unsigned a=0,b=0,c=0,x=0,y=0,z=0;
                int n = fscanf_s(baseline, "%u.%u.%u", &a,&b,&c); fclose(baseline);
                installedMp = n == 3;
                if (n == 3 && sscanf_s(ver, "%u.%u.%u", &x,&y,&z) == 3 &&
                    (x < a || (x == a && (y < b || (y == b && z < c))))) return;
            }
        }
    }
    if (!installedMp) return; // Uninstalling Multiplayer disables its cached releases.
    _snwprintf_s(root, _TRUNCATE, L"%s\\tpf2mp\\updates\\releases\\%S", local, ver);
    const wchar_t* required[] = { L"ready", L"tpf2_bridge_mp.dll", L"tpf2_menu.dll", L"tpf2_slice.dll",
        L"plugins\\tpf2_previews.dll", L"netpunch\\netpunch.exe", L"mod\\res\\scripts\\mp\\entry.lua",
        L"mod\\res\\scripts\\mp\\mod_data.lua" };
    for (auto name : required) {
        wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\%s", root, name);
        DWORD attr = GetFileAttributesW(path);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    }
    wchar_t shortRoot[MAX_PATH];
    const wchar_t* published = root;
    if (!Tpf2mpIsAsciiW(root) && GetShortPathNameW(root, shortRoot, MAX_PATH) && Tpf2mpIsAsciiW(shortRoot)) published = shortRoot;
    char utf8[MAX_PATH * 4];
    if (WideCharToMultiByte(CP_UTF8, 0, published, -1, utf8, sizeof(utf8), nullptr, nullptr)) {
        if (put) put("TPF2MP_RELEASE_ROOT", utf8);
    }
    SetEnvironmentVariableW(L"TPF2MP_RELEASE_ROOT", published);
}
