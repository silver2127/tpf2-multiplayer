// Offline: the OPEN LOGS archive (native/src/logarchive.h) against a fake data
// folder, game folder and lobby folder. Checks what a bug report needs is there
// (versions, state files, the lobby's message stream), what must stay out stays
// out (the incoming save, terrain dumps), invitation codes are masked, state
// files are copied and not moved, and each kind keeps TPF2_LOG_KEEP archives.
//   cl /nologo /std:c++17 /EHsc /I native\src tools\logarchive_test.cpp advapi32.lib
#include "logarchive.h"
#include <assert.h>
#include <string>
#include <fstream>
#include <sstream>

static std::wstring g_root;

static void put(const std::wstring& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary);
    f << text;
    assert(f.good());
}
static std::string get(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static int countKind(const std::wstring& logs, const wchar_t* kind)
{
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((logs + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && LaArchiveKind(fd.cFileName) >= 0 &&
            wcsstr(fd.cFileName, kind)) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

int main()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"tpf2mp-logarchive-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    const std::wstring la = g_root + L"la", data = g_root + L"data\\", game = g_root + L"game\\";
    for (const std::wstring& d : { g_root, la, data, game, game + L"netpunch", game + L"plugins" })
        CreateDirectoryW(d.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", la.c_str());
    SetEnvironmentVariableW(L"TPF2MP_DATADIR", data.c_str());

    // the data folder
    put(data + L"tpf2_bridge.log", "[m5] bridge init\n");
    put(data + L"tpf2_bridge_ctl.txt", "instance=b\npeer=127.0.0.1:7774\nlobby=0123456789abcdef0123456789abcdef\n");
    std::string stream(9u << 20, 'x');
    stream.replace(0, 5, "FIRST");
    stream.replace(stream.size() - 4, 4, "LAST");
    put(data + L"lockstep_inject_a.txt", stream);
    put(data + L"terrain_a_1_001.bin", "binary");
    // the game folder (any PE file stands in for the game's exe)
    put(game + L"tpf2mp_version.txt", "0.7.0.3\n");
    put(game + L"tpf2_slice.cfg", "towntrace=0\n");
    put(game + L"tpf2_menu.log", "[menu] attached\n");
    put(game + L"plugins\\tpf2_bigmap.cfg", "octree_depth=13\n");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    assert(CopyFileW(self, (game + L"TransportFever2.exe").c_str(), FALSE));
    // the lobby folder
    const std::string state = "{\"session\": \"94d4\", \"code\": \"7FVLP7EIYCUAB\\\"X\", \"players\": [\"A\"], \"mode\": \"coop\"}";
    put(game + L"netpunch\\lobby_state.json", state);
    const std::string outLine = "{\"type\": \"code\", \"code\": \"7FVLP7EI\", \"steam\": \"76561198\", \"crossplay\": true, \"cross_code\": \"7FVLP7EI\"}\n"
                                "{\"type\": \"roster\", \"players\": [\"A\", \"B\"]}\n";
    put(game + L"netpunch\\lobby_out.jsonl", outLine);
    put(game + L"netpunch\\lobby_proc.log", "[join] connected\n");
    put(game + L"netpunch\\incoming_save.sav", "a save");

    // six older OPEN LOGS copies: only TPF2_LOG_KEEP of that kind may remain
    const std::wstring logs = la + L"\\tpf2mp\\logs\\";
    CreateDirectoryW((la + L"\\tpf2mp").c_str(), nullptr);
    CreateDirectoryW(logs.c_str(), nullptr);
    for (int i = 0; i < 6; i++) {
        std::wstring d = logs + L"2020010" + std::to_wstring(i) + L"-000000-now";
        CreateDirectoryW(d.c_str(), nullptr);
        put(d + L"\\about.txt", "old");
    }

    // Under Wine (Proton): the game's userdata is in the host's Steam, named by
    // STEAM_COMPAT_CLIENT_INSTALL_PATH; a fake one holds the newest stdout.txt.
    const bool wine = LaWineVersion() != nullptr;
    char hostRoot[MAX_PATH * 3] = "";
    if (wine) {
        const std::wstring hs = g_root + L"hoststeam\\";
        for (const wchar_t* d : { L"", L"userdata", L"userdata\\42", L"userdata\\42\\1066780",
                                  L"userdata\\42\\1066780\\local", L"userdata\\42\\1066780\\local\\crash_dump" })
            CreateDirectoryW((hs + d).c_str(), nullptr);
        put(hs + L"userdata\\42\\1066780\\local\\crash_dump\\stdout.txt", "HOST STEAM STDOUT\n");
        assert(LaUnixPath(hs.c_str(), hostRoot, sizeof(hostRoot)) && hostRoot[0] == '/');
        wchar_t w[MAX_PATH * 3];
        MultiByteToWideChar(CP_UTF8, 0, hostRoot, -1, w, MAX_PATH * 3);
        SetEnvironmentVariableW(L"STEAM_COMPAT_CLIENT_INSTALL_PATH", w);
    }

    Tpf2mpLogArchive a;
    assert(Tpf2mpArchiveLogsSafe(false, game.c_str(), &a) && a.folder[0]);
    const std::wstring f = std::wstring(a.folder) + L"\\";
    const std::string about = get(f + L"about.txt");
    printf("%s\n", about.c_str());

    assert(about.find("TpF2 Multiplayer") != std::string::npos && about.find("0.7.0.3") != std::string::npos);
    assert(about.find("Windows") != std::string::npos && about.find("link stamp") != std::string::npos);
    assert(about.find("UTC") != std::string::npos && about.find("masked") != std::string::npos);
    // logs as before
    assert(exists(f + L"tpf2_bridge.log") && exists(f + L"game_tpf2_menu.log") && exists(f + L"game_netpunch_lobby_proc.log"));
    // state: copied (still in the data folder), the tail of a big stream
    assert(get(f + L"state_tpf2_bridge_ctl.txt").find("lobby=0123") != std::string::npos);
    assert(exists(data + L"tpf2_bridge_ctl.txt"));
    const std::string tail = get(f + L"state_lockstep_inject_a.txt");
    assert(tail.size() == (8u << 20) && tail.substr(tail.size() - 4) == "LAST" && tail.find("FIRST") == std::string::npos);
    assert(about.find("only the last 8 MB kept") != std::string::npos);
    // game and plugin configuration, the version file
    assert(get(f + L"game_tpf2mp_version.txt") == "0.7.0.3\n");
    assert(exists(f + L"game_tpf2_slice.cfg") && exists(f + L"game_plugins_tpf2_bigmap.cfg"));
    // never the save or the terrain dumps
    assert(!exists(f + L"game_netpunch_incoming_save.sav") && !exists(f + L"state_terrain_a_1_001.bin"));
    // the lobby's files: codes masked, same length, everything else intact
    const std::string st = get(f + L"game_netpunch_lobby_state.json");
    assert(st.size() == state.size() && st.find("7FVLP7") == std::string::npos);
    assert(st.find("\"code\": \"****************\"") != std::string::npos && st.find("\"mode\": \"coop\"") != std::string::npos);
    const std::string ol = get(f + L"game_netpunch_lobby_out.jsonl");
    assert(ol.size() == outLine.size() && ol.find("7FVLP7") == std::string::npos && ol.find("76561198") == std::string::npos);
    assert(ol.find("\"type\": \"code\"") != std::string::npos && ol.find("\"crossplay\": true") != std::string::npos);
    assert(ol.find("\"players\": [\"A\", \"B\"]") != std::string::npos);
    // the originals are untouched
    assert(get(game + L"netpunch\\lobby_state.json") == state);
    if (wine) {
        assert(about.find("Proton") != std::string::npos && about.find("Wine ") != std::string::npos);
        assert(get(f + L"game_stdout.txt") == "HOST STEAM STDOUT\n");   // found through the host's Steam
        char folderUnix[MAX_PATH * 3];
        assert(LaUnixPath(a.folder, folderUnix, sizeof(folderUnix)) && folderUnix[0] == '/');
        printf("wine: the archive is %s on the host\n", folderUnix);
    }
    // retention: the five newest "now" copies, this one included
    assert(countKind(logs, L"-now") == TPF2_LOG_KEEP);

    // a start-of-run archive moves the logs but only copies the state
    put(data + L"tpf2_slice.log", "[slice] up\n");
    Tpf2mpLogArchive b;
    assert(Tpf2mpArchiveLogsSafe(true, game.c_str(), &b) && b.folder[0]);
    assert(!exists(data + L"tpf2_slice.log") && exists(std::wstring(b.folder) + L"\\tpf2_slice.log"));
    assert(exists(data + L"tpf2_bridge_ctl.txt") && exists(std::wstring(b.folder) + L"\\state_tpf2_bridge_ctl.txt"));

    printf("PASS: versions, state files (copied, 8 MB tail), game/plugin cfg, lobby stream with codes masked, no save/terrain, keep %d\n",
           TPF2_LOG_KEEP);
    return 0;
}
