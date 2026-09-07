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
//                     INSTALLFOLDER holds no TransportFever2.exe. Never fails.
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
#include <msi.h>
#include <msiquery.h>
#include <string>

#pragma comment(lib, "msi.lib")

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
// writes it to the log otherwise.
void Say(MSIHANDLE h, INSTALLMESSAGE kind, const std::wstring& tmpl, const std::wstring& arg)
{
    PMSIHANDLE rec = MsiCreateRecord(2);
    MsiRecordSetStringW(rec, 0, tmpl.c_str());
    MsiRecordSetStringW(rec, 1, arg.c_str());
    MsiProcessMessage(h, kind, rec);
}

const wchar_t* NO_GAME_MSG =
    L"The folder [1] does not contain TransportFever2.exe.\n\n"
    L"TpF2 Multiplayer has to be installed into the Transport Fever 2 game folder. "
    L"In Steam, right-click the game, choose Manage, then Browse local files, and "
    L"pick that folder.";

bool GameDirOk(MSIHANDLE h, std::wstring* dirOut)
{
    std::wstring dir = WithSlash(GetProp(h, L"INSTALLFOLDER"));
    *dirOut = dir;
    return !dir.empty() && FileExists(dir + L"TransportFever2.exe");
}

} // namespace

extern "C" __declspec(dllexport) UINT __stdcall CheckGameDir(MSIHANDLE h)
{
    std::wstring dir;
    bool ok = GameDirOk(h, &dir);
    MsiSetPropertyW(h, L"TPF2_GAMEDIR_OK", ok ? L"1" : L"0");
    Log(h, L"CheckGameDir: " + dir + (ok ? L" -> TransportFever2.exe found" : L" -> TransportFever2.exe NOT found"));
    if (!ok)
        Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_WARNING | MB_OK | MB_ICONWARNING), NO_GAME_MSG, dir);
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RequireGameDir(MSIHANDLE h)
{
    std::wstring dir;
    if (GameDirOk(h, &dir)) {
        Log(h, L"RequireGameDir: " + dir + L" -> ok");
        return ERROR_SUCCESS;
    }
    Say(h, (INSTALLMESSAGE)(INSTALLMESSAGE_ERROR | MB_OK | MB_ICONERROR), NO_GAME_MSG, dir);
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
