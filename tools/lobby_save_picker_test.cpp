// Offline integration test of the real native picker and START GAME callback.
// Build the menu once, then link this TU with its support objects (not menu_hook.obj):
// cl /nologo /utf-8 /std:c++17 /EHsc /MT tools\lobby_save_picker_test.cpp
//    native\out\hook_menu.obj native\out\native_io.obj native\out\native_control.obj
//    native\out\gameuirelay_menu.obj user32.lib gdi32.lib advapi32.lib /Fe:<test.exe>
// Run with a NEW empty test directory as the only argument. Never uses real saves or a game.
#include "../native/src/menu_hook.cpp"
#include <cassert>
#include <fstream>
#include <filesystem>
namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
    assert(argc==2);
    fs::path root=fs::absolute(argv[1]);
    assert(!fs::exists(root));
    assert(fs::create_directory(root));
    std::wstring folder=root.wstring(); SAVE_DIR=folder.c_str(); NETDIR=folder.c_str();
    InitializeCriticalSection(&g_statusCs); g_csInit=true;
    g_isHost=1; g_lobbyReady=1;
    RefreshLobbySaves(); assert(g_lobbySaves.empty());
    OnHit(6); assert(g_savePicker && g_selectedSave.empty());
    assert(!fs::exists(root/L"lobby_in.jsonl"));
    fs::create_directory(root/L"directory.sav");
    std::ofstream(root/L"ignored.txt") << "not a save";
    for (int i=0;i<11;++i) {
        fs::path path=root/(std::to_wstring(i)+L" - Grüße.sav");
        std::ofstream(path) << "fixture " << i;
        HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        assert(file!=INVALID_HANDLE_VALUE);
        ULARGE_INTEGER ticks; ticks.QuadPart=132000000000000000ULL+i*10000000ULL;
        FILETIME time={ticks.LowPart,ticks.HighPart};
        assert(SetFileTime(file,nullptr,nullptr,&time)); CloseHandle(file);
    }
    OnHit(92); assert(g_lobbySaves.size()==11 && g_savePage==0);
    g_uiState=2;
    for (float scale : {0.75f,1.0f,1.5f}) {
        g_flagScale=scale;
        int width=(int)(800*scale), height=(int)(560*scale);
        RenderPanelLayer(width,height);
        int rows=0;
        for (int i=0;i<g_hitCount;++i) {
            const auto& hit=g_hits[i];
            assert(hit.x>=0 && hit.y>=0 && hit.x+hit.w<=width && hit.y+hit.h<=height);
            if (hit.id>=100 && hit.id<108) ++rows;
        }
        assert(rows==8);
    }
    assert(g_lobbySaves.front().name==L"10 - Grüße.sav");
    OnHit(94); assert(g_savePage==1);
    OnHit(94); assert(g_savePage==1); // clamp at final page
    OnHit(107); assert(g_selectedSave.empty()); // blank row cannot select anything
    OnHit(102); assert(g_selectedSave==(root/L"0 - Grüße.sav").wstring());
    assert(!g_savePicker);
    std::wstring selected=g_selectedSave;
    OnHit(90); OnHit(91); assert(g_selectedSave==selected); // cancel preserves choice
    g_isHost=0; OnHit(90); OnHit(6); assert(!g_savePicker);
    g_isHost=1;
    g_gameUi=1; OnHit(90); assert(!g_savePicker); g_gameUi=0;
    g_lobbyReady=0; OnHit(6); assert(!g_saveStartPending); g_lobbyReady=1;
    fs::path inbox=root/L"lobby_in.jsonl";
    auto length=fs::file_size(inbox); // selection advertised mods, but did not start
    fs::remove(selected); OnHit(6); assert(fs::file_size(inbox)==length);
    assert(strstr(g_status,"no longer available"));
    std::ofstream(fs::path(selected)) << "restored fixture";
    OnHit(6); assert(g_saveStartPending && g_startSaveW==selected);
    length=fs::file_size(inbox);
    OnHit(90); OnHit(6); assert(!g_savePicker && fs::file_size(inbox)==length);
    SaveStartStatus("connected","Waiting for mod downloads before starting the game.");
    assert(g_saveStartPending);
    for (const char* refusal : {"Not shared: missing multiplayer mod", "no players to share with -- wait"}) {
        SaveStartStatus("connected",refusal); assert(!g_saveStartPending);
        g_saveStartPending=1;
    }
    SaveStartStatus("failed","save transfer failed"); assert(!g_saveStartPending);
    std::ifstream input(inbox); std::string body((std::istreambuf_iterator<char>(input)),{});
    assert(body.find("\"cmd\":\"start\",\"save\":\""+jsonEscape(utf8Of(selected.c_str()).c_str())+"\"")!=std::string::npos);
    assert(body.find("10 -")==std::string::npos); // deliberately chose the oldest, never the newest
    input.close();
    g_saveStartPending=0;
    fs::path missing=root/L"missing"; std::wstring missingDir=missing.wstring(); NETDIR=missingDir.c_str();
    OnHit(6); assert(!g_saveStartPending && strstr(g_status,"Could not send"));
    puts("PASS: enumeration, sorting, Unicode, paging, cancellation, host/world guards, missing save, exact selected start, duplicate start and write failure");
    return 0;
}
