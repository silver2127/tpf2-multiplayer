// tpf2ca.dll -- custom actions for the TpF2 Multiplayer MSI (installer\Package.wxs).
//
// Why a DLL and not cmd.exe / VBScript: the game-folder check has to put a
// readable sentence in front of the user (an ExeCommand can only fail with
// "a program run as part of the setup did not finish as expected"), VBScript
// is on Microsoft's removal list, and a console window flashing during install
// is not something a player should see. Four entry points, all tiny:
//
//   CheckGameDir      immediate, UI sequence (DoAction from the folder dialog).
//                     Sets TPF2_GAMEDIR_OK to 1/0 and shows a warning box when
//                     INSTALLFOLDER holds no TransportFever2.exe, or one that
//                     is not Steam build 35924 (GOG, a patched exe, a game
//                     update), or an alut.dll another mod replaced. Asks
//                     whether to go on when mods ship native DLLs. Never fails.
//   RequireGameDir    immediate, execute sequence (covers /qn installs).
//                     Fails the install with a clear message on the same test.
//   PreserveStockAlut deferred, before InstallFiles, when alut.dll is being
//                     installed. Keeps the game's own alut.dll as alut_real.dll
//                     (the proxy forwards every export to that name) and makes
//                     sure no alut.dll is left on disk, so InstallFiles always
//                     lays down this package's proxy regardless of the file-
//                     versioning rules (both files are unversioned).
//   RollbackStockAlut rollback partner of PreserveStockAlut: puts a COPY of
//                     alut_real.dll back as alut.dll if the install fails
//                     after the stock file was moved. Copy, not move: a failed
//                     major upgrade also rolls the previous proxy back in, and
//                     that proxy still needs alut_real.dll beside it.
//   RestoreStockAlut  deferred, after RemoveFiles on a real uninstall (not the
//                     RemoveExistingProducts leg of an upgrade): moves
//                     alut_real.dll back to alut.dll.
//
// Deferred actions get the game folder through CustomActionData (a SetProperty
// with the action's own Id in Package.wxs); the immediate ones read INSTALLFOLDER.
// Build: installer\ca\build_ca.bat -> installer\out\tpf2ca.dll (x64, static CRT).
#include <windows.h>
#include <bcrypt.h>
#include <msi.h>
#include <msiquery.h>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

std::wstring GetProp(MSIHANDLE h, const wchar_t* name)
{
    DWORD n = 0;
    wchar_t probe[1] = L"";
    UINT r = MsiGetPropertyW(h, name, probe, &n);
    if (r != ERROR_MORE_DATA && r != ERROR_SUCCESS) return L"";
    std::wstring s(n + 1, L'\0');
    n = (DWORD)s.size();
    if (MsiGetPropertyW(h, name, &s[0], &n) != ERROR_SUCCESS) return L"";
    s.resize(n);
    return s;
}

std::wstring WithSlash(std::wstring d)
{
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return d;
}

bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void Log(MSIHANDLE h, const std::wstring& text)
{
    PMSIHANDLE rec = MsiCreateRecord(1);
    MsiRecordSetStringW(rec, 0, (L"[tpf2ca] " + text).c_str());
    MsiProcessMessage(h, INSTALLMESSAGE_INFO, rec);
}

// Shows `tmpl` (with [1] = arg) as an error or warning box in the full UI and
// writes it to the log otherwise. Returns the button (IDYES, IDNO, ...), or 0
// when there was no one to ask.
int Say(MSIHANDLE h, INSTALLMESSAGE kind, const std::wstring& tmpl, const std::wstring& arg)
{
    PMSIHANDLE rec = MsiCreateRecord(2);
    MsiRecordSetStringW(rec, 0, tmpl.c_str());
    MsiRecordSetStringW(rec, 1, arg.c_str());
    return MsiProcessMessage(h, kind, rec);
}

const wchar_t* NO_GAME_MSG =
    L"The folder [1] does not contain TransportFever2.exe.\n\n"
    L"TpF2 Multiplayer has to be installed into the Transport Fever 2 game folder. "
    L"In Steam, right-click the game, choose Manage, then Browse local files, and "
    L"pick that folder.";

// The only game build the DLLs work on. The same two PE header values
// native\src\plugin\host.cpp and native\src\slice_hook.cpp compare at run time,
// where a mismatch leaves every hook switched off: installing onto any other
// exe gives a game that starts normally and never shows multiplayer.
const DWORD GAME_EXE_TIMEDATESTAMP = 0x675abcc6;   // IMAGE_FILE_HEADER.TimeDateStamp, build 35924
const DWORD GAME_EXE_SIZEOFIMAGE   = 0x046ce000;   // IMAGE_OPTIONAL_HEADER64.SizeOfImage

const wchar_t* GOG_MSG =
    L"The folder [1] holds the GOG version of Transport Fever 2.\n\n"
    L"TpF2 Multiplayer works only with the Steam version (build 35924): it hooks that exact "
    L"TransportFever2.exe, and on any other executable multiplayer never starts.";

const wchar_t* WRONG_EXE_MSG =
    L"The TransportFever2.exe in [1] is not the Steam build 35924 that TpF2 Multiplayer is made "
    L"for, so multiplayer would never start.\n\n"
    L"This happens when another tool has replaced or patched the executable (CommonAPI2, for "
    L"example), with a copy of the game from another store, or after a game update.\n\n"
    L"To put back the Steam executable: close the game, right-click Transport Fever 2 in Steam, "
    L"choose Properties, Installed Files, Verify integrity of game files, then run this "
    L"installer again.";

enum class ExeBuild { Match, Mismatch, Unreadable };

// Reads the PE header of the exe on disk. Unreadable covers a file too short or
// not a PE32+ image, and a file that cannot be opened.
ExeBuild ReadExeBuild(const std::wstring& exe, DWORD* ts, DWORD* soi)
{
    *ts = 0;
    *soi = 0;
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return ExeBuild::Unreadable;
    unsigned char b[4096] = {};
    DWORD got = 0;
    BOOL ok = ReadFile(f, b, sizeof(b), &got, nullptr);
    CloseHandle(f);
    if (!ok || got < 0x40 || b[0] != 'M' || b[1] != 'Z') return ExeBuild::Unreadable;
    LONG lfanew = 0;
    memcpy(&lfanew, b + 0x3c, 4);
    if (lfanew < 0x40 || (DWORD)lfanew + 0x18 + 0x3c > got) return ExeBuild::Unreadable;
    const unsigned char* nt = b + lfanew;
    WORD magic = 0;
    memcpy(&magic, nt + 0x18, 2);
    if (memcmp(nt, "PE\0\0", 4) != 0 || magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return ExeBuild::Unreadable;
    memcpy(ts, nt + 0x08, 4);
    memcpy(soi, nt + 0x18 + 0x38, 4);
    return (*ts == GAME_EXE_TIMEDATESTAMP && *soi == GAME_EXE_SIZEOFIMAGE) ? ExeBuild::Match : ExeBuild::Mismatch;
}

// GOG installs put goggame-<product id>.info beside the executable.
bool IsGogInstall(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"goggame-*.info").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    FindClose(hf);
    return true;
}

std::wstring Hex(DWORD v)
{
    wchar_t s[16];
    swprintf_s(s, L"0x%08lx", (unsigned long)v);
    return s;
}

// ---- native DLLs that are not the game's or ours ---------------------------

// The game's own alut.dll (build 35924). The proxy parks it as alut_real.dll.
const wchar_t* STOCK_ALUT_SHA256 = L"3DF103AE3D94A6B90C4D2A6D75DCB388CD835F5E3AF9962B22C20D4473CFC035";

// Every DLL build 35924 ships beside the exe, plus this project's own. A DLL
// beside the exe that is not listed is loaded by name before the system copy
// (the trick the alut.dll proxy itself uses), so it is almost always a mod.
const wchar_t* KNOWN_TOP_DLLS[] = {
    L"concrt140.dll", L"GFSDK_Aftermath_Lib.x64.dll", L"icudt61.dll", L"icuin61.dll", L"icuuc61.dll",
    L"llvm_7_0_1.dll", L"msvcp140.dll", L"nvtt.dll", L"OpenAL32.dll", L"SDL2.dll", L"steam_api64.dll",
    L"vcruntime140.dll", L"alut.dll", L"alut_real.dll",
};

const wchar_t* FOREIGN_ALUT_MSG =
    L"The alut.dll in [1] is not the game's own file: another mod or tool has replaced it.\n\n"
    L"TpF2 Multiplayer loads through alut.dll, and two replacements of that file cannot both work: the "
    L"game would fail to start. Uninstall that mod, or close the game and run Steam's Verify integrity "
    L"of game files, then run this installer again.";

const wchar_t* MOD_DLLS_MSG =
    L"These native DLLs belong neither to Transport Fever 2 nor to TpF2 Multiplayer:\n\n[1]\n\n"
    L"Mods that load their own DLLs (CommonAPI2, for example) change the game underneath the game "
    L"script, and multiplayer does not expect that: it can fail to start, refuse commands or fall out "
    L"of sync. Remove or disable them before playing multiplayer.\n\nInstall anyway?";

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

std::wstring Sha256Hex(const std::wstring& path)
{
    std::wstring out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buf(1 << 16);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0)
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return out;
    wchar_t hex[3];
    for (unsigned char c : digest) { swprintf_s(hex, L"%02X", c); out += hex; }
    return out;
}

// Appends the *.dll files under dir (recursive) to out, as full paths.
void FindDlls(const std::wstring& dir, std::vector<std::wstring>* out, int depth = 0)
{
    if (depth > 8 || out->size() > 200) return;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            FindDlls(dir + name + L"\\", out, depth + 1);
        else if (name.size() > 4 && EqualsNoCase(name.substr(name.size() - 4), L".dll"))
            out->push_back(dir + name);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

std::wstring SteamRoot()
{
    wchar_t v[MAX_PATH] = L"";
    DWORD n = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath",
                     RRF_RT_REG_SZ, nullptr, v, &n) != ERROR_SUCCESS) {
        n = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                         RRF_RT_REG_SZ, nullptr, v, &n) != ERROR_SUCCESS)
            return L"";
    }
    std::wstring s = v;
    for (auto& c : s) if (c == L'/') c = L'\\';
    return WithSlash(s);
}

// The mod's display name from its mod.lua when that is a literal string. A
// translation key ("mod_name") says nothing and is left out.
std::wstring ModNameIn(const std::wstring& modDir)
{
    HANDLE f = CreateFileW((modDir + L"mod.lua").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::vector<char> buf(65536);
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf.data(), (DWORD)buf.size() - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok) return L"";
    buf[got] = 0;
    const char* p = buf.data();
    while ((p = strstr(p, "name")) != nullptr) {
        const char* q = p + 4;
        p = q;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '_' || *q == '(') q++;
        if (*q != '"') continue;
        const char* e = strchr(q + 1, '"');
        if (!e || e - q > 81) continue;
        std::string s(q + 1, e);
        if (s.find(' ') == std::string::npos) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    return L"";
}

// One line per mod folder under root that holds DLLs:
// "<label><folder> (<name>): <first dll>[ and N more]".
void AddModDlls(const std::wstring& root, const std::wstring& label, std::vector<std::wstring>* lines, size_t* count)
{
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((root + L"*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring d = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d == L"." || d == L"..") continue;
        std::wstring modDir = root + d + L"\\";
        std::vector<std::wstring> dlls;
        FindDlls(modDir, &dlls);
        if (dlls.empty()) continue;
        std::wstring name = ModNameIn(modDir);
        std::wstring line = label + d + (name.empty() ? L"" : L" (" + name + L")") + L": " + dlls[0].substr(modDir.size());
        if (dlls.size() > 1) line += L" and " + std::to_wstring(dlls.size() - 1) + L" more";
        lines->push_back(line);
        *count += dlls.size();
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

// The native DLLs the game may load that are neither its own nor ours: beside
// the exe, in the game's mods folder, in every Steam user's local mods folder,
// and in the Workshop content of the library the game is in.
std::vector<std::wstring> ForeignDllLines(const std::wstring& dir, size_t* count)
{
    std::vector<std::wstring> lines;
    *count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            if (name.size() < 5) continue;
            std::wstring ext = name.substr(name.size() - 4);
            if (!EqualsNoCase(ext, L".dll") && !EqualsNoCase(ext, L".asi")) continue;
            bool known = _wcsnicmp(name.c_str(), L"tpf2_", 5) == 0;   // this project's DLLs, suffixed dev builds included
            for (const wchar_t* k : KNOWN_TOP_DLLS) known = known || EqualsNoCase(name, k);
            if (!known) { lines.push_back(L"game folder: " + name); ++*count; }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    AddModDlls(dir + L"mods\\", L"mods\\", &lines, count);
    std::wstring steam = SteamRoot();
    if (!steam.empty()) {
        hf = FindFirstFileW((steam + L"userdata\\*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                std::wstring id = fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || id == L"." || id == L"..") continue;
                AddModDlls(steam + L"userdata\\" + id + L"\\1066780\\local\\mods\\", L"your mods\\", &lines, count);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }
    // <library>\steamapps\common\Transport Fever 2\ -> <library>\steamapps\workshop\content\1066780\ .
    std::wstring lower = dir;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    size_t at = lower.rfind(L"\\steamapps\\common\\");
    if (at != std::wstring::npos)
        AddModDlls(dir.substr(0, at) + L"\\steamapps\\workshop\\content\\1066780\\", L"Workshop item ", &lines, count);
    return lines;
}

// alut.dll: with alut_real.dll present, alut_real.dll must be the stock file
// (alut.dll is then a proxy: ours, or a sibling product's); without it,
// alut.dll itself must be. Anything else is another mod's replacement.
const wchar_t* AlutProblem(MSIHANDLE h, const std::wstring& dir, const wchar_t* who)
{
    std::wstring real = dir + L"alut_real.dll", live = dir + L"alut.dll";
    std::wstring checked = FileExists(real) ? real : live;
    if (!FileExists(checked)) return nullptr;   // PreserveStockAlut reports a missing alut.dll itself
    std::wstring hash = Sha256Hex(checked);
    if (hash.empty() || _wcsicmp(hash.c_str(), STOCK_ALUT_SHA256) == 0) return nullptr;
    Log(h, std::wstring(who) + L": " + checked.substr(dir.size()) + L" is not the stock alut.dll (SHA-256 " + hash + L")");
    return FOREIGN_ALUT_MSG;
}

// nullptr when the folder is usable; otherwise the message to show.
const wchar_t* GameDirProblem(MSIHANDLE h, std::wstring* dirOut, const wchar_t* who)
{
    std::wstring dir = WithSlash(GetProp(h, L"INSTALLFOLDER"));
    *dirOut = dir;
    std::wstring exe = dir + L"TransportFever2.exe";
    if (dir.empty() || !FileExists(exe)) {
        Log(h, std::wstring(who) + L": " + dir + L" -> TransportFever2.exe NOT found");
        return NO_GAME_MSG;
    }
    DWORD ts = 0, soi = 0;
    switch (ReadExeBuild(exe, &ts, &soi)) {
    case ExeBuild::Match:
        Log(h, std::wstring(who) + L": " + dir + L" -> TransportFever2.exe is build 35924");
        return AlutProblem(h, dir, who);
    case ExeBuild::Unreadable:
        // Not a verdict on the build: the DLLs still refuse a wrong exe at run
        // time, so an install is not blocked on a header this code could not read.
        Log(h, std::wstring(who) + L": " + dir + L" -> TransportFever2.exe header unreadable; not blocking");
        return AlutProblem(h, dir, who);
    default:
        break;
    }
    bool gog = IsGogInstall(dir);
    Log(h, std::wstring(who) + L": " + dir + L" -> TransportFever2.exe is NOT build 35924 (TimeDateStamp " +
               Hex(ts) + L", SizeOfImage " + Hex(soi) + L")" + (gog ? L", GOG install" : L""));
    return gog ? GOG_MSG : WRONG_EXE_MSG;
}

} // namespace

extern "C" __declspec(dllexport) UINT __stdcall CheckGameDir(MSIHANDLE h)
{
    std::wstring dir;
    const wchar_t* problem = GameDirProblem(h, &dir, L"CheckGameDir");
    bool ok = problem == nullptr;
    if (problem) {
        Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING), problem, dir);
    } else {
        // Mod DLLs do not block: the player decides. Declining keeps the wizard
        // on the folder page like any other refusal.
        size_t count = 0;
        std::vector<std::wstring> lines = ForeignDllLines(dir, &count);
        if (!lines.empty()) {
            std::wstring list;
            for (size_t i = 0; i < lines.size(); i++) {
                Log(h, L"CheckGameDir: foreign DLL: " + lines[i]);
                if (i < 12) list += L"  " + lines[i] + L"\n";
            }
            if (lines.size() > 12) list += L"  ... and " + std::to_wstring(lines.size() - 12) + L" more\n";
            int answer = Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_WARNING | MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2),
                             MOD_DLLS_MSG, list);
            if (answer == IDNO) {
                ok = false;
                Log(h, L"CheckGameDir: declined because of " + std::to_wstring(count) + L" foreign DLL(s)");
            }
        }
    }
    MsiSetPropertyW(h, L"TPF2_GAMEDIR_OK", ok ? L"1" : L"0");
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RequireGameDir(MSIHANDLE h)
{
    std::wstring dir;
    const wchar_t* problem = GameDirProblem(h, &dir, L"RequireGameDir");
    if (!problem) {
        // A silent install cannot ask, and mod DLLs are the player's call: log them only.
        size_t count = 0;
        for (const std::wstring& l : ForeignDllLines(dir, &count))
            Log(h, L"RequireGameDir: foreign DLL (not blocking): " + l);
        return ERROR_SUCCESS;
    }
    Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR), problem, dir);
    return ERROR_INSTALL_FAILURE;
}


// ---------------------------------------------------------------------------
// Shared-proxy refcount.
//
// alut.dll is shipped by MORE THAN ONE product (TpF2 Multiplayer and TpF2 Big
// Maps) under the SAME component GUID, so Windows Installer reference-counts
// the file itself: the second product to install finds it present, the first
// to uninstall leaves it for the other. What MSI cannot know is that our
// deferred actions ALSO touch the file -- RestoreStockAlut moves alut_real.dll
// back over it -- and would do so while a sibling product still needs the
// proxy. So every action that touches alut.dll first asks the installer who
// else owns the component. If anyone does, the file is theirs too: leave it.
//
// MsiEnumClients lists the product codes registered as clients of a component.
// By the time a deferred action runs on uninstall, ProcessComponents has
// already dropped THIS product from that list, but ProductCode is one of the
// few properties a deferred action can read, so it is excluded explicitly in
// case the sequence ever changes.
static const wchar_t* PROXY_COMPONENT = L"{F7DC2819-9B08-4477-B651-A8AAA406270D}";

static int OtherProxyClients(MSIHANDLE h)
{
    std::wstring me = GetProp(h, L"ProductCode");
    int others = 0;
    for (DWORD i = 0; ; ++i) {
        wchar_t code[39] = L"";
        UINT r = MsiEnumClientsW(PROXY_COMPONENT, i, code);
        if (r != ERROR_SUCCESS) break;
        if (_wcsicmp(code, me.c_str()) != 0) ++others;
    }
    return others;
}

extern "C" __declspec(dllexport) UINT __stdcall PreserveStockAlut(MSIHANDLE h)
{
    std::wstring dir = WithSlash(GetProp(h, L"CustomActionData"));
    std::wstring live = dir + L"alut.dll";
    std::wstring real = dir + L"alut_real.dll";
    if (dir.empty()) {
        Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
            L"Internal error: the game folder was not passed to PreserveStockAlut.", L"");
        return ERROR_INSTALL_FAILURE;
    }
    if (!FileExists(real)) {
        // First install on this machine: the alut.dll on disk is the game's own.
        if (!FileExists(live)) {
            Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
                L"Neither alut.dll nor alut_real.dll exists in [1]. The game installation looks "
                L"damaged; run Steam's Verify integrity of game files, then run this installer again.",
                dir);
            return ERROR_INSTALL_FAILURE;
        }
        if (!MoveFileExW(live.c_str(), real.c_str(), MOVEFILE_WRITE_THROUGH)) {
            Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
                L"Could not rename alut.dll to alut_real.dll in [1] (Windows error " +
                    std::to_wstring(GetLastError()) + L"). Is the game running?",
                dir);
            return ERROR_INSTALL_FAILURE;
        }
        Log(h, L"PreserveStockAlut: stock alut.dll kept as alut_real.dll in " + dir);
        return ERROR_SUCCESS;
    }
    // alut_real.dll is already there (earlier install of this package, the
    // developer script, or a SIBLING product that shares the proxy). If a
    // sibling owns the proxy component, the alut.dll on disk is its live proxy:
    // leave it -- InstallFiles either overwrites it with ours (same binary) or
    // keeps it, and either way a working proxy stays in place. Deleting it here
    // would leave a window with NO alut.dll if InstallFiles then decides the
    // file is already current.
    int others = OtherProxyClients(h);
    if (others > 0) {
        Log(h, L"PreserveStockAlut: " + std::to_wstring(others) +
               L" other product(s) own the proxy; leaving alut.dll in " + dir);
        return ERROR_SUCCESS;
    }
    // Otherwise whatever alut.dll is on disk is an old proxy of ours or a copy
    // Steam put back. Drop it; InstallFiles installs this package's proxy next.
    if (FileExists(live) && !DeleteFileW(live.c_str())) {
        Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR),
            L"Could not replace the existing alut.dll in [1] (Windows error " +
                std::to_wstring(GetLastError()) + L"). Is the game running?",
            dir);
        return ERROR_INSTALL_FAILURE;
    }
    Log(h, L"PreserveStockAlut: alut_real.dll already present in " + dir + L"; old alut.dll removed");
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RollbackStockAlut(MSIHANDLE h)
{
    std::wstring dir = WithSlash(GetProp(h, L"CustomActionData"));
    std::wstring live = dir + L"alut.dll";
    std::wstring real = dir + L"alut_real.dll";
    if (dir.empty() || !FileExists(real)) return ERROR_SUCCESS;
    if (FileExists(live)) return ERROR_SUCCESS;   // MSI's own rollback already put something back
    if (OtherProxyClients(h) > 0) {
        // A sibling product still needs a PROXY here, not the stock library.
        // MSI's rollback restores what it removed; we cannot reconstruct the
        // sibling's file, so say so rather than break it.
        Log(h, L"RollbackStockAlut: another product owns the proxy; NOT restoring the "
               L"stock alut.dll -- run Repair on that product if the game fails to start");
        return ERROR_SUCCESS;
    }
    if (CopyFileW(real.c_str(), live.c_str(), FALSE))
        Log(h, L"RollbackStockAlut: alut.dll restored from alut_real.dll in " + dir);
    else
        Log(h, L"RollbackStockAlut: copy failed, error " + std::to_wstring(GetLastError()));
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RestoreStockAlut(MSIHANDLE h)
{
    std::wstring dir = WithSlash(GetProp(h, L"CustomActionData"));
    std::wstring live = dir + L"alut.dll";
    std::wstring real = dir + L"alut_real.dll";
    if (dir.empty() || !FileExists(real)) {
        Log(h, L"RestoreStockAlut: no alut_real.dll in " + dir + L"; nothing to restore");
        return ERROR_SUCCESS;
    }
    int others = OtherProxyClients(h);
    if (others > 0) {
        // The proxy file itself survives this uninstall (MSI refcounts the
        // shared component); moving the stock library over it would silently
        // break the product that still uses it. alut_real.dll stays too: that
        // product's uninstall restores it when it is the last one out.
        Log(h, L"RestoreStockAlut: " + std::to_wstring(others) +
               L" other product(s) still own the proxy; leaving alut.dll and alut_real.dll in " + dir);
        return ERROR_SUCCESS;
    }
    if (MoveFileExW(real.c_str(), live.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        Log(h, L"RestoreStockAlut: stock alut.dll restored in " + dir);
    else
        Log(h, L"RestoreStockAlut: move failed, error " + std::to_wstring(GetLastError()) +
               L" -- Steam's Verify integrity of game files restores the file");
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
