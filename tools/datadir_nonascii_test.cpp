// datadir_nonascii_test.cpp -- offline check of Tpf2mpPublishDataDir (native/src/datadir.h)
// for a Windows profile folder with non-ASCII characters.
//
// Built with /MD so getenv below is the SAME UCRT getenv the game's Lua os.getenv calls.
// Build and run with tools\datadir_nonascii_test.ps1.
//
// A player's game aborted creating a new game: the Lua side could not open its data folder
// because os.getenv returns ANSI bytes and the game's io.open expects UTF-8 (2026-09-12).
#include "../native/src/datadir.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

static int fails = 0;
static void check(const char* name, bool ok, const std::string& extra = "")
{
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, extra.empty() ? "" : "  -- ", extra.c_str());
    if (!ok) fails++;
}

static bool asciiA(const char* s)
{
    for (; *s; ++s) if ((unsigned char)*s > 0x7f) return false;
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: datadir_nonascii_test <scratch dir>\n"); return 2; }

    // 1. an ASCII profile is left alone
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", nullptr);
    std::wstring asciiRoot = std::wstring(argv[1]) + L"\\ascii_profile";
    CreateDirectoryW(asciiRoot.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", asciiRoot.c_str());
    Tpf2mpPublishDataDir();
    wchar_t w[MAX_PATH] = L"";
    check("ASCII profile: TPF2MP_DATADIR not published", GetEnvironmentVariableW(L"TPF2MP_DATADIR", w, MAX_PATH) == 0);

    // 2. a non-ASCII profile gets an ASCII short path
    std::wstring root = std::wstring(argv[1]) + L"\\profile éü中";
    CreateDirectoryW(root.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    Tpf2mpPublishDataDir();
    wchar_t pub[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"TPF2MP_DATADIR", pub, MAX_PATH);
    check("non-ASCII profile: TPF2MP_DATADIR published", n > 0);
    check("non-ASCII profile: the published path is ASCII", n > 0 && Tpf2mpIsAsciiW(pub));

    // the CRT getenv (Lua's os.getenv) sees it, as ASCII
    const char* g = getenv("TPF2MP_DATADIR");
    check("CRT getenv sees TPF2MP_DATADIR", g != nullptr);
    check("CRT getenv value is ASCII", g && asciiA(g), g ? g : "");

    // the slice's narrow path is ASCII, and an ANSI fopen through it lands in the real folder
    char a[MAX_PATH] = "";
    check("Tpf2mpDataDirA succeeds", Tpf2mpDataDirA(a, sizeof(a), nullptr));
    check("Tpf2mpDataDirA is ASCII", asciiA(a), a);
    std::string file = std::string(a) + "tpf2_instance.txt";
    FILE* f = fopen(file.c_str(), "w");
    check("ANSI fopen through the published path", f != nullptr, file);
    if (f) { fputs("a\npid=1\n", f); fclose(f); }
    std::wstring longFile = root + L"\\tpf2mp\\data\\tpf2_instance.txt";
    check("the file is in the real non-ASCII folder", GetFileAttributesW(longFile.c_str()) != INVALID_FILE_ATTRIBUTES);

    // 3. an existing pin is never overwritten
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", L"C:\\pinned\\");
    Tpf2mpPublishDataDir();
    wchar_t again[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"TPF2MP_DATADIR", again, MAX_PATH);
    check("a harness pin is left alone", wcscmp(again, L"C:\\pinned\\") == 0);

    // 4. a RELATIVE pin is made absolute against the current folder (the game's folder
    //    in the game process) and republished, so every process sees one folder
    SetCurrentDirectoryW(argv[1]);
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", L"pin_rel");
    Tpf2mpPublishDataDir();
    wchar_t rel[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"TPF2MP_DATADIR", rel, MAX_PATH);
    std::wstring wantRel = std::wstring(argv[1]) + L"\\pin_rel";
    check("relative pin: republished absolute", Tpf2mpPathIsAbsoluteW(rel) && _wcsnicmp(rel, wantRel.c_str(), wantRel.size()) == 0);
    check("relative pin: the folder was created", GetFileAttributesW(wantRel.c_str()) != INVALID_FILE_ATTRIBUTES);
    const char* gr = getenv("TPF2MP_DATADIR");
    check("relative pin: CRT getenv sees the absolute form", gr && strchr(gr, ':') != nullptr, gr ? gr : "");

    // 5. a Unix-style (rootless) pin, the Proton case: /tmp/tpf2mp-data becomes
    //    <current drive>:\tmp\tpf2mp-data in every consumer
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", L"/tpf2mp_datadir_test_rootless/data");
    wchar_t rootless[MAX_PATH] = L"";
    check("rootless pin: Tpf2mpDataDirW resolves it", Tpf2mpDataDirW(rootless, MAX_PATH, nullptr));
    check("rootless pin: absolute, on the current drive", Tpf2mpPathIsAbsoluteW(rootless) && towupper(rootless[0]) == towupper(argv[1][0]), std::string(rootless, rootless + wcslen(rootless)));
    check("rootless pin: backslashes, trailing backslash", wcsstr(rootless, L":\\tpf2mp_datadir_test_rootless\\data\\") != nullptr);
    Tpf2mpPublishDataDir();
    wchar_t pubRootless[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"TPF2MP_DATADIR", pubRootless, MAX_PATH);
    check("rootless pin: republished absolute", Tpf2mpPathIsAbsoluteW(pubRootless));
    RemoveDirectoryW(rootless);   // created at the drive root only when that is writable
    { std::wstring parent = rootless; parent.erase(parent.find_last_of(L'\\')); parent.erase(parent.find_last_of(L'\\')); RemoveDirectoryW(parent.c_str()); }

    // 6. an absolute pin with forward slashes is absolute already and stays as it is
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", L"C:/pinned/fwd");
    Tpf2mpPublishDataDir();
    wchar_t fwd[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"TPF2MP_DATADIR", fwd, MAX_PATH);
    check("forward-slash absolute pin is left alone", wcscmp(fwd, L"C:/pinned/fwd") == 0);

    printf("\n%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
