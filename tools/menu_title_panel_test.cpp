// Offline: render the real menu/session code and exercise hit targets, without
// starting a lobby, touching the installation or loading a game.
#include "../native/src/menu_hook.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

static bool hit(int id) { for(int i=0;i<g_hitCount;++i) if(g_hits[i].id==id) return true; return false; }
static void check(int w,int h)
{
    for(int i=0;i<g_hitCount;++i) {
        const auto& a=g_hits[i];
        assert(a.w>0 && a.h>0 && a.x>=0 && a.y>=0 && a.x+a.w<=w && a.y+a.h<=h);
        for(int j=0;j<i;++j) {
            const auto& b=g_hits[j];
            if(a.id==b.id && a.id!=91 && a.id!=4) { fprintf(stderr,"Duplicate %d page=%ld tab=%d picker=%d scale=%g\n",a.id,g_uiState,g_titleTab,g_savePicker,g_flagScale); assert(false); }
            if(a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h) {
                printf("Overlapping controls %d and %d\n",a.id,b.id); assert(false);
            }
        }
    }
}
static void snapshot(const fs::path& path,int w,int h)
{
    std::vector<unsigned char> pixels;
    if(WorldLoaded()) {
        pixels.resize((size_t)w*h*4);
        ComposeLayer(nullptr,0,pixels.data(),(size_t)w*4,w,h);
    } else {
        int dw=w,dh=h; w=1280;h=800;
        const int x=(w-dw)/2,y=(h-dh)/2;
        pixels.resize((size_t)w*h*4);
        PaintTitleBackdrop(pixels.data(),(size_t)w*4,w,h,x,y,dw,dh);
        ComposeLayer(g_titleBackdrop.data()+((size_t)y*w+x)*4,(size_t)w*4,
            pixels.data()+((size_t)y*w+x)*4,(size_t)w*4,dw,dh,true);
    }
    BITMAPFILEHEADER file{}; file.bfType=0x4d42;
    file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+(DWORD)pixels.size();
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=w; info.biHeight=-h;
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary); out.write((char*)&file,sizeof(file)); out.write((char*)&info,sizeof(info));
    out.write((char*)pixels.data(),pixels.size()); assert(out.good());
}
int wmain(int argc,wchar_t** argv)
{
    assert(argc==2); fs::path folder=fs::absolute(argv[1]); assert(!fs::exists(folder)); fs::create_directories(folder);
    InitializeCriticalSection(&g_statusCs); g_csInit=true;
    InitializeCriticalSection(&g_modelCs); g_modelCsInit=true;
    wchar_t artwork[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"TPF2_MENU_TEST_ARTWORK",artwork,MAX_PATH)) {
        assert(ReadTitleArt(artwork,g_titleArt,g_titleArtW,g_titleArtH));
        g_titleArtTried=true;
    }
    InitializeCriticalSection(&g_pubCs); g_pubCsInit=true;
    // The installed font is read only; missing fonts use the production fallback.
    g_latoLoaded=AddFontResourceExW(L"D:/SteamLibrary/steamapps/common/Transport Fever 2/res/fonts/Lato2OFL/Lato-Regular.ttf",FR_PRIVATE,nullptr)>0;
    g_titleArtTried=true;
    ReadTitleArt(L"D:/SteamLibrary/steamapps/common/Transport Fever 2/res/textures/ui/ui.zip",g_titleArt,g_titleArtW,g_titleArtH);
    // A missing/corrupt asset is optional: never crash or require game assets in CI.
    { std::vector<unsigned char> invalid; int iw=0,ih=0;
      assert(!ReadTitleArt((folder/L"missing.zip").c_str(),invalid,iw,ih));
      std::ofstream bad(folder/L"bad.zip",std::ios::binary); bad<<"not a zip"; bad.close();
      assert(!ReadTitleArt((folder/L"bad.zip").c_str(),invalid,iw,ih)); }
    PrepareTitleBackdrop(1280,800);
    const auto* cached=g_titleBackdrop.data(); PrepareTitleBackdrop(1280,800); assert(cached==g_titleBackdrop.data());
    strcpy_s(g_username,"Alex"); strcpy_s(g_lobbyName,"Alpine railways");
    strcpy_s(g_passCode,"fixture"); g_passLen=7;
    g_pubCount=8;
    for(int i=0;i<8;++i) {
        sprintf_s(g_pub[i].name,"Community game %d",i+1); sprintf_s(g_pub[i].code,"fixture-%d",i);
        strcpy_s(g_pub[i].version,"0.6.2.10"); strcpy_s(g_pub[i].type,i%2?"host":"dedicated");
        g_pub[i].players=i+1; g_pub[i].max=16; g_pub[i].locked=i%2;
    }
    g_titleTab=0; assert(TitleNextFocus(0,false)==3 && TitleNextFocus(3,false)==2 && TitleNextFocus(2,false)==1 && TitleNextFocus(1,false)==3 && TitleNextFocus(3,true)==1);
    g_titleTab=1; assert(TitleNextFocus(2,false)==4 && TitleNextFocus(3,true)==4);
    g_flagScale=3; g_scExtent={1280,720}; g_uiState=1;
    assert(UiScale()*800<=1281 && UiScale()*560<=721);
    g_scExtent={0,0};
    std::wstring longName(180,L'W');
    for(float scale : {0.5f,0.75f,1.0f,1.5f,2.0f}) {
        g_flagScale=scale; int w=(int)(780*scale),h=(int)(540*scale);
        g_uiState=1; g_titleTab=0; g_titleServerPage=0; g_gameUi=0;
        strcpy_s(g_flagMaster,"https://example.invalid"); strcpy_s(g_joinCode,"fixture-0");
        RenderPanelLayer(w,h); check(w,h); assert(hit(3) && hit(110) && hit(111) && hit(40) && hit(113));
        if(scale==1) snapshot(folder/L"join.bmp",w,h);
        OnHit(113); RenderPanelLayer(w,h); check(w,h); assert(hit(112) && hit(47));
        OnHit(110); assert(!strcmp(g_joinCode,"fixture-0") && !strcmp(g_passCode,"fixture"));
        OnHit(111); RenderPanelLayer(w,h); check(w,h); assert(hit(2) && hit(14) && !hit(3));
        if(scale==1) snapshot(folder/L"host.bmp",w,h);
        g_flagMaster[0]=0; RenderPanelLayer(w,h); check(w,h); assert(!hit(11));
        g_titleTab=0; g_joinCode[0]=0; RenderPanelLayer(w,h); check(w,h); assert(!hit(3));
        strcpy_s(g_flagMaster,"https://example.invalid");
        g_uiState=2; g_isHost=1; g_haveCode=1; g_lobbyReady=1; g_savePicker=false;
        g_lobbyTitle="Alpine railways - a very long multiplayer lobby name to test ellipsis and the close button";
        g_selectedSave=L"C:\\fixture\\Alpine railway network.sav";
        g_you="Alex";g_host="Alex";g_players.clear();g_companies.assign(16,1);
        for(int i=0;i<16;++i) { g_players.push_back(i?"Player "+std::to_string(i):"Alex");g_companies[i]=i%8+1; }
        g_chatCount=2;g_chatHead=0;
        strcpy_s(g_chatLog[0],"Alex: Ready to build the mountain railway?");strcpy_s(g_chatLog[1],"Player 1: Ready!");
        g_titlePlayerPage=0;
        RenderPanelLayer(w,h); check(w,h); assert(hit(6) && hit(90) && hit(115));
        if(scale==1) snapshot(folder/L"lobby.bmp",w,h);
        SetTransferDetail("To Player 1 | Steam (TCP failed) | 94.9 / 113.0 MB | 0.48 MB/s");
        RenderPanelLayer(w,h); check(w,h);
        if(scale==1) snapshot(folder/L"lobby-transfer.bmp",w,h);
        SetTransferDetail("");
        OnHit(115); RenderPanelLayer(w,h); check(w,h); assert(hit(114) && hit(35));
        g_saveStartPending=1;RenderPanelLayer(w,h);check(w,h);assert(!hit(6) && !hit(90));g_saveStartPending=0;
        g_isHost=0;RenderPanelLayer(w,h);check(w,h);assert(!hit(6) && !hit(50) && !hit(90));g_isHost=1;
        g_savePicker=true;g_lobbySaves.clear();
        for(int i=0;i<11;++i) { LobbySave save{}; save.name=i?L"Regional network.sav":longName; save.path=L"C:\\fixture\\"+std::to_wstring(i)+L".sav";g_lobbySaves.push_back(save); }
        g_savePage=0;RenderPanelLayer(w,h);check(w,h);assert(hit(107) && hit(94));
        if(scale==1) snapshot(folder/L"saves.bmp",w,h);
        g_savePicker=false;strcpy_s(g_modsPrompt,"This savegame needs 3 additional mods. Download them from the host?");
        RenderPanelLayer(w,h);check(w,h);assert(g_hitCount==3 && hit(16) && hit(17) && hit(19));
        if(scale==1) snapshot(folder/L"mods.bmp",w,h);g_modsPrompt[0]=0;
        g_gameUi=1; g_uiState=2; g_sessionStarted=1; g_isHost=1; g_hostSteam=1;
        g_savePicker=true;
        g_titlePlayerPage=0; RenderPanelLayer(w,h);check(w,h);
        assert(hit(87) && hit(4) && hit(51) && hit(50) && hit(11));
        assert(!hit(5) && !hit(6) && !hit(90) && !hit(100));
        if(scale==1) snapshot(folder/L"session-host.bmp",w,h);
        OnHit(115);assert(g_titlePlayerPage==1);RenderPanelLayer(w,h);check(w,h);assert(hit(114));
        g_isHost=0;RenderPanelLayer(w,h);check(w,h);
        assert(hit(4) && hit(9) && !hit(87) && !hit(50) && !hit(51) && !hit(11) && !hit(6));
        if(scale==1) snapshot(folder/L"session-client.bmp",w,h);
        g_uiState=1;g_savePicker=false;RenderPanelLayer(w,h);check(w,h);
        assert(hit(2) && hit(4) && hit(13) && hit(14) && hit(51) && !hit(3) && !hit(110));
        if(scale==1) snapshot(folder/L"session-setup.bmp",w,h);
        g_gameUi=0;g_sessionStarted=0;g_hostSteam=0;
        assert(!RenderTitlePanel(w,h,4));
        g_uiState=3; g_readyTotal=16; g_readyCount=8; g_readyMine=false;
        g_stages.assign(16,"loading world 42%");
        for(bool host : {false,true}) for(bool world : {false,true}) {
            g_isHost=host; g_gameUi=world?1:0; g_titlePlayerPage=0;
            for(const char* phase : {"manual","detected","readiness","holding","saving","transferring","loading","checking","releasing","complete","error","unavailable","waiting","aborted"}) {
                strcpy_s(g_recoveryPhase,phase); g_recoveryRequestedAt=0;
                g_recoveryDetail[0]=0;
                SetTransferDetail(!strcmp(phase,"transferring") ?
                    "To Player 1 | Steam (TCP failed) | 94.9 / 113.0 MB | 0.48 MB/s" : "",
                    !strcmp(phase,"transferring") ?
                    "Check host router/firewall: allow TCP 29471. IPv4 may need port forwarding to the host PC." : "");
                RenderPanelLayer(w,h); check(w,h);
                const bool preflight=!strcmp(phase,"manual") || !strcmp(phase,"detected") || !strcmp(phase,"unavailable");
                const bool start=!strcmp(phase,"manual") || !strcmp(phase,"detected") || !strcmp(phase,"waiting") || !strcmp(phase,"aborted");
                assert(hit(85)==preflight);
                assert(hit(84)==(host && start));
                assert(hit(82)==(host && !strcmp(phase,"error")));
                assert(hit(86)==!strcmp(phase,"readiness"));
                assert(hit(88)==(host && !strcmp(phase,"detected")));
                assert(!hit(5) && !hit(6) && hit(9) && !hit(20) && !hit(50));
                assert(hit(83)==world && !hit(4)); // the resync view's x, in game only
                if(scale==1 && host && world && (!strcmp(phase,"manual") || !strcmp(phase,"loading") || !strcmp(phase,"error")))
                    snapshot(folder/(std::string("resync-")+phase+".bmp"),w,h);
                g_recoveryRequestedAt=1; RenderPanelLayer(w,h); check(w,h);
                assert(!hit(84) && !hit(82) && !hit(86) && !hit(88));
            }
        }
        strcpy_s(g_recoveryPhase,"readiness");g_recoveryRequestedAt=0;g_readyMine=true;
        RenderPanelLayer(w,h);check(w,h);assert(!hit(86));
        // x during a running resync hides the view and keeps the resync; saving/loading draws no x
        g_gameUi=1;g_recoveryPresent=1;g_uiState=3;strcpy_s(g_recoveryPhase,"transferring");
        RenderPanelLayer(w,h);check(w,h);assert(hit(83));
        OnHit(83);assert(g_uiState==0 && g_recoveryPresent && g_recoveryHidden);
        g_recoveryWorldIo=1;g_uiState=3;RenderPanelLayer(w,h);check(w,h);assert(!hit(83));g_recoveryWorldIo=0;
        // x on a preflight notice dismisses it like Close
        strcpy_s(g_recoveryPhase,"detected");g_uiState=3;RenderPanelLayer(w,h);check(w,h);
        OnHit(83);assert(g_uiState==0 && !g_recoveryPresent && !g_recoveryHidden);
        g_uiState=3;g_recoveryPresent=0;
        g_stages.clear();g_gameUi=0;g_recoveryPhase[0]=0;
    }
    // Representative two-player previews, separate from the stress fixtures above.
    g_flagScale=1;g_uiState=3;g_gameUi=0;g_isHost=1;g_titlePlayerPage=0;
    g_players={"Alex","Sam"};g_you="Alex";g_host="Alex";g_companies={1,2};
    g_lobbyTitle="Alpine railways";g_chatCount=2;g_chatHead=0;
    strcpy_s(g_chatLog[0],"Alex: Ready for resync?");strcpy_s(g_chatLog[1],"Sam: Ready!");
    g_stages.clear();g_recoveryRequestedAt=0;g_recoveryDetail[0]=0;SetStatus("");
    strcpy_s(g_recoveryPhase,"manual");RenderPanelLayer(780,540);check(780,540);
    snapshot(folder/L"resync-manual.bmp",780,540);
    strcpy_s(g_recoveryPhase,"loading");g_stages={"loading world 63%","loading world 42%"};
    SetStatus("loading world 63%");RenderPanelLayer(780,540);check(780,540);
    snapshot(folder/L"resync-loading.bmp",780,540);
    strcpy_s(g_recoveryPhase,"transferring");g_stages={"sending save 84%","receiving save 84%"};
    SetTransferDetail("To Sam | Steam (TCP failed) | 94.9 / 113.0 MB | 0.48 MB/s",
        "Check host router/firewall: allow TCP 29471. IPv4 may need port forwarding to the host PC.");
    RenderPanelLayer(780,540);check(780,540);
    snapshot(folder/L"resync-transfer.bmp",780,540);
    SetTransferDetail(""); assert(!g_transferHint[0]);
    g_recoveryPhase[0]=0;g_stages.clear();
    // Both Vulkan and OpenGL use this CPU surface path. Exercise full-frame
    // composition with padded rows and a rebuilt allocation at the same size.
    g_gameUi=0; g_uiState=1; g_titleTab=0; g_scExtent={1280,800};
    g_panelW=1280; g_panelH=800; g_flagScale=1; PanelLayout();
    const size_t pitch=1280*4+16, bytes=pitch*800;
    std::vector<unsigned char> surface(bytes+32,0xCD);
    g_titleSurfaceNeedsUpload=true;
    assert(ComposePanelIfDirty(surface.data()+16,pitch));
    for(size_t i=0;i<16;++i) assert(surface[i]==0xCD && surface[16+bytes+i]==0xCD);
    for(int y=0;y<800;++y) for(size_t x=1280*4;x<pitch;++x) assert(surface[16+y*pitch+x]==0xCD);
    assert(surface[16]!=0xCD || surface[17]!=0xCD || surface[18]!=0xCD);
    std::vector<unsigned char> rebuilt(bytes,0xCD);
    g_titleSurfaceNeedsUpload=true;
    assert(ComposePanelIfDirty(rebuilt.data(),pitch));
    assert(!memcmp(surface.data()+16,rebuilt.data(),1280*4));
    g_gameUi=1; g_uiState=1; PanelLayout();
    assert(ComposePanelIfDirty(rebuilt.data(),pitch));
    g_gameUi=0;
    // Resync enters the engine loading page without the lobby start handler.
    // Exercise the real page hook, stage tick, progress reader and footer state.
    const auto netdir=folder.wstring(); NETDIR=netdir.c_str();
    g_lobbyProc=CreateEventW(nullptr,TRUE,FALSE,nullptr); assert(g_lobbyProc);
    g_origCreatePage=[](uint64_t,int) {};
    alignas(8) unsigned char menu[0x500]{},bar[0x480]{},monitor[16]{};
    *reinterpret_cast<uint64_t*>(menu+MENU_OFF_PROGRESSBAR)=reinterpret_cast<uint64_t>(bar);
    *reinterpret_cast<uint64_t*>(bar+BAR_OFF_MONITOR)=reinterpret_cast<uint64_t>(monitor);
    *reinterpret_cast<uint64_t*>(monitor)=g_base+RVA_PROGRESSMON_VFT;
    float& percent=*reinterpret_cast<float*>(monitor+PM_OFF_PROGRESS);
    for(float value : {0.0f,0.5f,1.0f}) {
        percent=value; g_stageWatch=0; SetStatus("Receiving save... 100%");
        MyCreatePage(reinterpret_cast<uint64_t>(menu),16);
        assert(g_loadingPanel && g_showOverlay && g_loadingStagePending);
        StageTick();
        assert(g_stageWatch && !g_loadingStagePending);
        const char* expected=value==0?"loading world 0%":value==0.5f?"loading world 50%":"loading world";
        assert(!strcmp(g_status,expected) && !strcmp(g_stageSent,expected));
        // A late transfer event cannot permanently pin the footer at 100%.
        SetStatus("Receiving save... 100%");
        SetTransferDetail("Receiving | Steam (TCP failed) | 113.0 / 113.0 MB | 0.48 MB/s",
            "Check host router/firewall: allow TCP 29471.");
        ReportStage(expected);
        assert(!strcmp(g_status,expected) && !g_transferDetail[0] && !g_transferHint[0]);
    }
    *reinterpret_cast<uint64_t*>(bar+BAR_OFF_MONITOR)=0;
    MyCreatePage(reinterpret_cast<uint64_t>(menu),16); StageTick();
    assert(!strcmp(g_status,"loading world")); // unavailable progress, no made-up %
    g_recoveryPresent=1;strcpy_s(g_recoveryPhase,"loading");g_isHost=1;
    RenderPanelLayer(780,540);check(780,540);
    assert(g_uiState==2 && !hit(6) && !hit(90) && hit(9)); // loading page keeps the resync lobby
    g_gameUi=1; bool quiet=false;
    assert(OverlayWanted(quiet) && quiet && !g_loadingPanel && g_ingameOverlay);
    g_recoveryPresent=0;g_recoveryPhase[0]=0;
    ReportStage("world loaded, waiting for the session");
    assert(!strcmp(g_status,"world loaded, waiting for the session"));
    ReportStage(""); assert(!strcmp(g_status,"World loaded. Session running."));
    std::ifstream commands(folder/L"lobby_in.jsonl");
    const std::string wire((std::istreambuf_iterator<char>(commands)),{});
    assert(wire.find("loading world 50%")!=std::string::npos);
    CloseHandle(g_lobbyProc); g_lobbyProc=nullptr;
    puts("PASS: resync page rearms progress; footer/roster advance; late transfer, missing progress and world handover");
    puts("PASS: five scales; disjoint controls; all server/player pages; host/client/transfer guards; modal isolation; title and in-world session flows");
}
