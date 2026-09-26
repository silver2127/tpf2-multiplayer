// Actual lobby entry points, without Init or worker threads: no subprocesses,
// network requests, save reads or game calls may occur in these tests.
#include "../src/lobby_linux.cpp"
#include <cassert>

static std::string fixtureSaveDir;
static bool allowPlace=false;
static int placed=0;
static int loadPercent=-1;
int MenuGame_LoadPercent() { return loadPercent; }
static int modRefreshes=0;
void MenuGame_RequestModRefresh() { ++modRefreshes; }
bool MenuGame_Loading() { return false; }
std::string MenuGame_SaveDir() { assert(!fixtureSaveDir.empty()); return fixtureSaveDir; }
static bool saveTest = false, nativeBusy = false, forceAllowed = true;
static int savesForced = 0;
static std::string newestSave;
static bool allowLoad = true;
static int loads = 0;
static std::string lastStatus;
namespace NativeIo {
bool Busy() { return nativeBusy; }
bool Load(const std::string& operation, const std::string& name) {
    assert(operation.find("world_switch_") == 0 && name == "mp_shared");
    ++loads; return allowLoad;
}
}
bool MenuGame_ForceAutosave() { assert(saveTest); ++savesForced; return forceAllowed; }
bool MenuGame_NewestSave(std::string* path) { assert(saveTest); *path = newestSave; return !path->empty(); }

bool MenuGame_PlaceSharedSave(const std::string& source, std::string* name) { assert(allowPlace && source.find("incoming_save.sav")!=std::string::npos); ++placed; *name="mp_shared"; return true; }
void MenuGame_RequestAutoload(const std::string&) { assert(false && "unexpected load"); }

static void Write(const std::string& path, const std::string& body)
{
    FILE* f = fopen(path.c_str(), "w");
    assert(f && fwrite(body.data(), 1, body.size(), f)==body.size());
    assert(fclose(f)==0);
}

int main()
{
    using namespace lobby;
    std::string list="{\"servers\":[";
    for(int i=0;i<40;++i) { if(i)list+=",";list+="{\"code\":\"fixture-"+std::to_string(i)+"\",\"name\":\"Game\"}"; }
    list+="]}";std::vector<PubRow> publicRows;std::string publicNote;
    assert(ParsePublic(list,&publicRows,&publicNote));
    assert(publicRows.size()==32 && publicRows.back().code=="fixture-31");
    publicRows.clear();assert(!ParsePublic("invalid",&publicRows,&publicNote));
    S().m.active=true; g_childPid=123; g_gameUiSeen=false;
    OnMenuPage(2); assert(S().m.active && S().q.empty()); // waiting joiner
    OnGameUiFrame(); OnMenuPage(16); assert(S().m.active && S().q.empty()); // switch
    OnMenuPage(2); assert(!S().m.active && S().q.size()==1 && S().q.front().kind==Request::Stop);
    OnMenuPage(2); assert(S().q.size()==1); // only once
    g_childPid=0; S().q.clear();
    char temporary[] = "/tmp/tpf2mp-ready.XXXXXX";
    assert(mkdtemp(temporary));
    const std::string dir = std::string(temporary)+"/";
    const std::string path = dir+"tpf2_slice_ready.txt";
    const uint64_t start = SliceReadyProcessStart();
    assert(start && !SliceHooksReady(dir.c_str()));
    assert(SlicePublishReady(dir.c_str(), false));
    assert(!SliceHooksReady(dir.c_str()));
    assert(SlicePublishReady(dir.c_str(), true));
    assert(SliceHooksReady(dir.c_str()));
    for (const auto& body : {
        std::string(), std::string("pid="), std::string("ready=1\n"),
        "pid="+std::to_string(getpid()+1)+"\nstart="+std::to_string(start)+"\nready=1\n",
        "pid="+std::to_string(getpid())+"\nstart="+std::to_string(start+1)+"\nready=1\n",
        "pid="+std::to_string(getpid())+"\nstart="+std::to_string(start)+"\nready=1junk\n",
        std::string(256, 'x')}) {
        Write(path, body);
        assert(!SliceHooksReady(dir.c_str()));
    }
    assert(unlink(path.c_str())==0);
    assert(mkfifo(path.c_str(), 0600)==0);
    assert(!SliceHooksReady(dir.c_str())); // Must not block opening a FIFO.
    assert(unlink(path.c_str())==0);
    assert(symlink("/dev/zero", path.c_str())==0);
    assert(!SliceHooksReady(dir.c_str()));
    assert(unlink(path.c_str())==0);

    lobby::S().cfg.dataDir=dir;
    // Nonce before the first successful ctl write, subsequent rewrites,
    // malformed events and a fresh session all use the actual dispatcher.
    S().child.gen=S().m.gen;
    const std::string nonce(32, 'a'), nextNonce(32, 'b');
    auto nonceEvent=[](const std::string& value) {
        Dispatch("{\"type\":\"transport_lobby\",\"epoch\":\""+value+"\"}");
    };
    S().cfg.dataDir=dir+"missing/";
    nonceEvent(nonce);
    assert(S().m.transportLobby==nonce && S().ctlLast.empty());
    S().cfg.dataDir=dir;
    WriteBridgeCtl(true);
    std::string ctl;
    auto checkNonce=[&](const std::string& value) {
        assert(ReadSmallFile(dir+"tpf2_bridge_ctl.txt", &ctl));
        assert(ctl.find("lobby="+value+"\n")!=std::string::npos);
    };
    checkNonce(nonce);
    S().m.speedReq="2"; WriteBridgeCtl(false); checkNonce(nonce);
    for(const auto& bad : {std::string(), std::string(31,'a'), std::string(33,'a'),
                          std::string(32,'A'), std::string(31,'a')+"\n"}) {
        nonceEvent(bad); checkNonce(nonce);
    }
    nonceEvent(nextNonce); checkNonce(nextNonce);
    nonceEvent(nextNonce); checkNonce(nextNonce);
    ++S().m.gen; nonceEvent(nonce); checkNonce(nextNonce); // stale child
    S().m=Model{}; S().child.gen=S().m.gen;
    WriteBridgeCtl(true);
    assert(ReadSmallFile(dir+"tpf2_bridge_ctl.txt", &ctl) && ctl.find("lobby=")==std::string::npos);

    lobby::g_inited=true; // Do not start the real worker or server browser.
    lobby::StartRequest request;
    request.name="Readiness test";
    std::string why;
    for (bool join : {false, true}) {
        request.join=join; request.code="ABCDEFGH";
        const uint64_t previous=lobby::S().m.gen;
        assert(!lobby::Start(request,&why));
        assert(!why.empty() && lobby::S().q.empty() && lobby::S().m.gen==previous);
    }
    assert(SlicePublishReady(dir.c_str(),true));
    for (const std::string code : {"76561198000000001", "https://steamcommunity.com/profiles/76561198000000001/"}) {
        request.join=true;request.code=code;
        assert(lobby::Start(request,&why));
        assert(lobby::S().q.back().start.code=="76561198000000001");
        lobby::S().q.clear();
    }
    for (const std::string code : {"https://steamcommunity.com/profiles/", "76561198000000001 --flag"}) {
        request.join=true;request.code=code;
        assert(!lobby::Start(request,&why) && lobby::S().q.empty());
    }
    lobby::S().child.gen=lobby::S().m.gen;
    lobby::Dispatch("{\"type\":\"code\",\"code\":\"76561198000000001\",\"steam\":\"76561198000000001\",\"crossplay\":false}");
    assert(lobby::S().m.hostSteam && !lobby::S().m.crossplay && lobby::S().q.empty());
    lobby::Dispatch("{\"type\":\"code\",\"code\":\"ABCDEFGH\",\"steam\":\"76561198000000001\",\"crossplay\":true}");
    assert(lobby::S().m.crossplay && lobby::S().m.code=="ABCDEFGH" && lobby::S().q.empty());
    request.code="ABCDEFGH";
    for (bool join : {false, true}) {
        request.join=join; request.separateCompanies=true;
        assert(lobby::Start(request,&why));
        assert(lobby::S().m.separateCompanies==!join && lobby::S().q.front().start.separateCompanies);
        assert(lobby::S().q.size()==1 && lobby::S().m.active && lobby::S().m.isHost==!join);
        assert(lobby::S().q.front().start.join==join);
        const auto queued=lobby::S().q.front();
        lobby::S().q.clear();
        // Readiness lost after queuing must be rechecked before any launch.
        assert(SlicePublishReady(dir.c_str(),false));
        lobby::Launch(queued);
        assert(lobby::S().m.dead && !lobby::S().child.pid);
        assert(SlicePublishReady(dir.c_str(),true));
    }
    lobby::S().m.isHost=true; lobby::S().m.lobbyReady=true; lobby::S().m.dead=false;
    assert(!lobby::StartGame().empty() && lobby::S().q.empty());
    lobby::S().m.selectedSave="fixture.sav";
    assert(lobby::StartGame().empty() && lobby::S().q.size()==1);
    lobby::S().q.clear();
    assert(SlicePublishReady(dir.c_str(),false));
    assert(!lobby::StartGame().empty() && lobby::S().q.empty());
    lobby::ShareAndStart(); // Worker recheck must return without reading a save.
    // Every roster refresh exports the same stable origins used by bridge/company state.
    auto& model = lobby::S().m;
    model.players = {"Joiner with spaces", std::string(100, 'H')};
    model.host = model.players[1]; model.relay = false;
    lobby::WritePlayerNames();
    auto readNames = [&] {
        FILE* f = fopen((dir + "mp_players.txt").c_str(), "r"); assert(f);
        char buf[1024]{}; const size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
        return std::string(buf, n);
    };
    assert(readNames() == "b=Joiner with spaces\na=" + model.host + "\n");
    model.relay = true; model.letters = {"ac", "z"};
    lobby::WritePlayerNames();
    assert(readNames() == "ac=Joiner with spaces\nz=" + model.host + "\n");
    model.players.resize(1); lobby::WritePlayerNames();
    assert(readNames() == "ac=Joiner with spaces\n");
    auto readLoading = [&] {
        std::string content;
        assert(lobby::ReadSmallFile(dir + "mp_loading.txt", &content));
        return content;
    };
    assert(readLoading().empty()); // Missing stages are safe, including short vectors.
    // Exercise real roster parsing/export, stable relay letters and stage-only updates.
    auto rosterLoading = [&](const std::string& stages) {
        lobby::Json roster;
        assert(lobby::ParseJson(R"({"players":["host","joiner","third"],"host":"host","you":"joiner","relay":true,"letters":{"host":"z","joiner":"ac","third":"b"},"stages":)"
                               + stages + "}", &roster));
        lobby::ApplyRoster(roster);
    };
    rosterLoading(R"({"host":"","joiner":"receiving save 40%","third":"loading world"})");
    assert(readLoading() == "ac=joiner=receiving save 40%\nb=third=loading world\n");
    rosterLoading(R"json({"third":"catching up (12 s behind)"})json");
    assert(readLoading() == "b=third=catching up (12 s behind)\n");
    rosterLoading("{}"); assert(readLoading().empty());
    rosterLoading(R"({"third":"loading world"})");
    lobby::Json departed;
    assert(lobby::ParseJson(R"({"players":["host","joiner"]})", &departed));
    lobby::ApplyRoster(departed); assert(readLoading().empty());
    // A complete roster over 1 MiB survives the mailbox tail and parser.
    lobby::S().lobbyDir=dir; lobby::S().child.gen=model.gen;
    std::string event="{\"type\":\"roster\",\"players\":[";
    const std::string longName(5000, 'n');
    for (int i=0;i<220;++i) event += (i ? "," : "") + std::string("\"") + longName + std::to_string(i) + "\"";
    event += "],\"stages\":{\"" + longName + "219\":\"loading world\"}}\n";
    Write(dir+"lobby_out.jsonl",event.substr(0,event.size()/2));
    lobby::TailOut(); assert(lobby::S().child.lines==0);
    Write(dir+"lobby_out.jsonl",event);
    lobby::TailOut(); assert(model.players.size()==220 && model.players.back()==longName+"219");
    lobby::View view; lobby::Snapshot(&view);
    assert(view.players.back().stage=="loading world");
    // Pause time does not age a reusable snapshot; unknown/running time does.
    const std::string letter=lobby::OwnLetter();
    Write(dir+"lockstep_dash_"+letter+".txt", "paused=yes\n");
    lobby::S().unpausedLast=lobby::NowMs()-1000; lobby::S().unpausedMs=10;
    lobby::UnpausedTick(); assert(lobby::S().unpausedMs==10);
    Write(dir+"lockstep_dash_"+letter+".txt", "paused=no\n");
    lobby::S().unpausedLast=lobby::NowMs()-1000; lobby::UnpausedTick();
    assert(lobby::S().unpausedMs>=1010);
    lobby::MarkSaveShared(dir+"snapshot.sav");
    assert(lobby::S().sharedUnpaused==lobby::S().unpausedMs);
    // Reuse explicitly starts a session even if it has never shared to a peer.
    Write(dir+"snapshot.sav", "fixture"); model.isHost=true;
    lobby::g_gameUiSeen=true; lobby::g_titleMenu=false;
    lobby::SyncStart("hot join: first peer");
    std::string sent; assert(lobby::ReadSmallFile(dir+"lobby_in.jsonl", &sent));
    assert(sent.find("\"cmd\":\"start\",\"save\":\""+dir+"snapshot.sav\"")!=std::string::npos);
    // Tokens from other processes cannot invalidate our cached snapshot.
    Write(dir+"tpf2mp_world_gen.txt", "pid=-1\ngen=foreign\n");
    lobby::PollWorldGen(); assert(lobby::S().worldGen.empty());
    Write(dir+"tpf2mp_world_gen.txt", "pid="+std::to_string(getpid())+"\ngen=one\n");
    lobby::PollWorldGen(); assert(lobby::S().worldGen=="one");
    lobby::S().worldGenHold=true;
    Write(dir+"tpf2mp_world_gen.txt", "gen=two\n");
    lobby::PollWorldGen(); assert(!lobby::S().worldGenHold && !lobby::S().sharedSave.empty());
    Write(dir+"tpf2mp_world_gen.txt", "gen=three\n");
    lobby::PollWorldGen(); assert(lobby::S().sharedSave.empty()); // never reuse across worlds
    unlink((dir+"tpf2mp_world_gen.txt").c_str()); unlink((dir+"snapshot.sav").c_str());
    // FROZEN JOIN: with join_freeze the host takes no hot-join save (the stubbed
    // autosave asserts) and sends no start; the flag clears on the next roster.
    lobby::g_gameUiSeen=true; lobby::g_titleMenu=false; model.isHost=true; model.lastCount=2;
    {
        const lobby::Model saved=model;
        std::string before; lobby::ReadSmallFile(dir+"lobby_in.jsonl", &before);
        lobby::Json frozen;
        assert(lobby::ParseJson(R"({"players":["host","joiner","late"],"host":"host","you":"host","join_freeze":true})", &frozen));
        lobby::ApplyRoster(frozen);
        assert(model.joinFreeze && model.lastCount==3);
        std::string after; lobby::ReadSmallFile(dir+"lobby_in.jsonl", &after);
        assert(after==before);
        lobby::Json same;
        assert(lobby::ParseJson(R"({"players":["host","joiner","late"],"host":"host","you":"host"})", &same));
        lobby::ApplyRoster(same); assert(!model.joinFreeze);
        model=saved;   // the later stage checks use the earlier roster's letter
    }
    // Stage updates are sent once and cleared when the script reports live.
    lobby::g_gameUiSeen=true; lobby::g_titleMenu=false; lobby::S().stageWatch=true;
    Write(dir+"lockstep_status_"+letter+".txt", "stage=catchup:fetch:25\n");
    lobby::StageTick(); assert(lobby::S().stageSent=="catching up: fetching history (25 s behind)");
    Write(dir+"lockstep_status_"+letter+".txt", "stage=live\n");
    lobby::S().stageNext=0; lobby::StageTick(); assert(!lobby::S().stageWatch && lobby::S().stageSent.empty());
    // Previous load's 100% and old-world status cannot prematurely clear a switch.
    loadPercent=100; lobby::ArmStageWatch("loading world");
    lobby::StageTick(); assert(lobby::S().stageWatch && lobby::S().stageSent=="loading world");
    loadPercent=42; lobby::S().stageNext=0; lobby::StageTick();
    assert(lobby::S().stageSent=="loading world 42%");
    loadPercent=100; lobby::S().stageNext=0; lobby::StageTick();
    assert(!lobby::S().stageWatch && lobby::S().stageSent.empty());
    lobby::g_gameUiSeen=false; lobby::ArmStageWatch("loading world");
    lobby::StageTick(); assert(lobby::S().stageSent=="loading world");
    loadPercent=-1; lobby::g_gameUiSeen=true; lobby::S().stageNext=0; lobby::StageTick();
    assert(!lobby::S().stageWatch); // unavailable percentage falls back to script stage
    // Company chip direction, authority, no-op and mode requests use the real queue.
    model.players={"host", "joiner", "third"}; model.companies={1,1,3};
    model.you="host"; model.host="host"; model.isHost=true; model.lobbyReady=true;
    model.dead=false; model.active=true;
    auto& queue=lobby::S().q;
    auto chip = [&](int index, bool previous, int expected) {
        queue.clear(); lobby::CycleCompany(index,previous);
        assert(queue.size()==1 && queue.back().gen==model.gen);
        assert(queue.back().line.find("\"id\":"+std::to_string(expected)+"}")!=std::string::npos);
    };
    chip(0,false,3); chip(2,false,4); chip(2,true,1); chip(0,true,3);
    model.companies={1,1,1}; queue.clear(); lobby::CycleCompany(0,true); assert(queue.empty());
    model.companies={1,1,200}; chip(2,false,1);
    model.isHost=false; queue.clear(); lobby::CycleCompany(1,false); assert(queue.empty());
    assert(!lobby::SetSeparateCompanies(true).empty() && queue.empty());
    model.isHost=true;
    lobby::SetSeparateCompanies(true); assert(queue.size()==1 && queue.back().line=="{\"cmd\":\"mode\",\"mode\":\"companies\"}");
    queue.clear(); lobby::SetSeparateCompanies(false);
    assert(queue.size()==1 && queue.back().line=="{\"cmd\":\"mode\",\"mode\":\"coop\"}");
    queue.clear(); model.lobbyReady=false; lobby::SetSeparateCompanies(true); assert(queue.empty());
    model.lobbyReady=true;
    // Empty title-menu rosters avoid engine calls; mode must reach the view even alone.
    lobby::g_titleMenu=true;
    lobby::Json roster;
    assert(lobby::ParseJson("{\"players\":[],\"mode\":\"companies\"}",&roster));
    lobby::ApplyRoster(roster); lobby::Snapshot(&view); assert(view.separateCompanies);
    assert(lobby::ParseJson("{\"players\":[],\"mode\":\"coop\"}",&roster));
    lobby::ApplyRoster(roster); lobby::Snapshot(&view); assert(!view.separateCompanies);
    lobby::g_titleMenu=false;
    // Company config is ready before a frozen join (which never calls HandleStart).
    model.relay=false;
    std::string companyConfig;
    assert(lobby::ParseJson(R"({"players":["host"],"you":"host","host":"host","mode":"companies"})", &roster));
    lobby::ApplyRoster(roster);
    assert(lobby::ReadSmallFile(dir+"mp_company_cfg.txt", &companyConfig));
    assert(companyConfig == "companies\n1\n1\na=1\n");
    assert(lobby::ParseJson(R"({"players":["host","joiner"],"you":"joiner","mode":"companies","companies":{"host":1,"joiner":2}})", &roster));
    lobby::ApplyRoster(roster);
    assert(lobby::ReadSmallFile(dir+"mp_company_cfg.txt", &companyConfig));
    assert(companyConfig == "companies\n2\n1,2\na=1,b=2\n");
    // A world switch consumes its transfer once and queues the engine's load.
    model.players={"host","joiner"}; model.companies={1,2}; model.you="joiner";
    model.isHost=false; model.saveReady=true; allowPlace=true;
    lobby::Json sw; assert(lobby::ParseJson("{\"save\":true,\"switch\":true}",&sw));
    lobby::HandleStart(sw); assert(placed==1 && !model.saveReady);
    assert(loads==1 && lobby::S().stageWatch);
    lobby::HandleStart(sw); assert(placed==1); // stale start cannot reuse an old transfer
    model.saveReady=true; allowLoad=false;
    lobby::S().status=[](const char* status) { lastStatus=status; };
    lobby::HandleStart(sw); assert(placed==2 && loads==2 && !model.saveReady);
    assert(lastStatus.find("LOAD GAME") != std::string::npos);
    placed=1; allowLoad=true;
    model.isHost=true; model.saveReady=true;
    lobby::HandleStart(sw); assert(placed==1); // host keeps its already loaded world
    // A vanilla load queues the same session and shares its named file.
    fixtureSaveDir=dir; Write(dir+"chosen.sav","fixture");
    model.lobbyReady=true; lobby::OnMenuLoad("chosen");
    assert(lobby::S().q.back().kind==lobby::Request::LoadedSave && lobby::S().q.back().gen==model.gen);
    lobby::ShareLoadedSave("chosen"); assert(lobby::S().hostLoadedItself && lobby::S().worldGenHold);
    assert(lobby::ReadSmallFile(dir+"lobby_in.jsonl",&sent) && sent.find("\"switch\":true")!=std::string::npos);
    lobby::HandleStart(sw); assert(!lobby::S().hostLoadedItself && placed==1);
    // Selection only accepts an enumerated regular save, advertises its mods,
    // and stays fixed while START is pending.
    model.isHost=true;model.lobbyDone=false;model.startPending=false;lobby::S().q.clear();
    assert(mkdir((dir+"directory.sav").c_str(),0700)==0);
    Write(dir+"empty.sav","");
    lobby::ReadSaves(model.gen);
    assert(model.saves.size()==1 && model.saves[0].name=="chosen.sav");
    assert(!lobby::SelectSave(dir+"absent.sav").empty());
    assert(lobby::SelectSave(dir+"chosen.sav").empty());
    assert(model.selectedSave==dir+"chosen.sav" && lobby::S().q.size()==1);
    assert(lobby::S().q.back().line.find("advertise_mods")!=std::string::npos);
    model.startPending=true;assert(!lobby::SelectSave(dir+"chosen.sav").empty());
    assert(unlink((dir+"empty.sav").c_str())==0 && rmdir((dir+"directory.sav").c_str())==0);
    // Closing is local: periodic wire progress cannot reopen a hidden resync.
    {
        const auto saved=model;const auto queued=queue.size();
        lobby::Dispatch(R"({"type":"sync_state","phase":"transferring","operation":"hide-test"})");
        model.recoveryRequestedAt=NowMs();
        assert(lobby::RecoveryAction("sync_hide").empty());
        for(int i=0;i<3;++i)lobby::Dispatch(R"({"type":"sync_state","phase":"transferring","operation":"hide-test"})");
        lobby::Snapshot(&view);assert(view.recoveryHidden && view.recoveryPresent);
        assert(model.recoveryOperation=="hide-test" && queue.size()==queued);
        lobby::RecoveryAction("sync_show");assert(!model.recoveryHidden);
        for(const char* event:{
            R"({"type":"sync_state","phase":"error"})",
            R"({"type":"sync_ready_state","phase":"waiting","is_ready":0})",
            R"({"type":"sync_state","phase":"complete"})",
            R"({"type":"sync_prompt","phase":"clear"})",
            R"({"type":"sync_prompt","phase":"detected"})"}) {
            model.recoveryHidden=true;lobby::Dispatch(event);assert(!model.recoveryHidden);
        }
        lobby::Dispatch(R"({"type":"sync_ready_state","phase":"waiting","is_ready":1})");
        lobby::RecoveryAction("sync_hide");
        lobby::Dispatch(R"({"type":"sync_ready_state","phase":"waiting","is_ready":1})");
        assert(model.recoveryHidden && model.recoveryPresent);
        for(const char* phase:{"manual","detected","unavailable"}) {
            model.recoveryPhase=phase;model.recoveryPresent=true;model.recoveryRequestedAt=NowMs();
            lobby::RecoveryAction("sync_hide");assert(!model.recoveryHidden && !model.recoveryPresent);
        }
        assert(queue.size()==queued);model=saved;
    }
    // Recovery controls follow actual wire events, retain the readiness token,
    // reject duplicate/non-host requests, and allow another sync after success.
    model.active=true;model.dead=false;model.isHost=true;
    lobby::S().child.gen=model.gen;queue.clear();
    lobby::Dispatch(R"({"type":"sync_prompt","phase":"detected"})");
    assert(model.recoveryPresent && model.recoveryPhase=="detected");
    assert(lobby::RecoveryAction("sync_request")=="Request sent." && queue.size()==1);
    lobby::RecoveryAction("sync_request");assert(queue.size()==1);
    lobby::Dispatch(R"({"type":"sync_ready_state","phase":"waiting","token":"barrier-1","ready_count":1,"total":2,"is_ready":0})");
    lobby::Snapshot(&view);assert(view.readyCount==1 && view.readyTotal==2 && !view.readyMine);
    assert(lobby::RecoveryAction("sync_ready")=="Request sent." && queue.size()==2);
    assert(queue.back().line.find("\"token\":\"barrier-1\"")!=std::string::npos);
    lobby::Dispatch(R"({"type":"sync_ready_state","phase":"waiting","token":"barrier-1","ready_count":2,"total":2,"is_ready":1})");
    lobby::RecoveryAction("sync_ready");assert(queue.size()==2);
    lobby::Dispatch(R"({"type":"sync_state","phase":"error","operation":"operation-1","step":"load","detail":"load failed"})");
    model.isHost=false;lobby::RecoveryAction("sync_retry");assert(queue.size()==2);
    model.isHost=true;assert(lobby::RecoveryAction("sync_retry")=="Request sent.");
    assert(queue.back().line.find("\"operation\":\"operation-1\"")!=std::string::npos);
    lobby::Dispatch(R"({"type":"sync_state","phase":"complete","operation":"operation-1"})");
    assert(!model.recoveryPresent && model.lobbyDone && !model.startPending);
    assert(lobby::RecoveryAction("sync_request")=="Request sent.");
    {
        const auto savedModel=model;const auto savedQueue=queue;
        model.recoveryRequestedAt=0;model.recoveryPhase="detected";model.recoveryPresent=true;
        assert(RecoveryAction("sync_dismiss").empty() && !model.recoveryPresent);
        model.recoveryPhase="loading";model.recoveryPresent=true;
        assert(!RecoveryAction("sync_dismiss").empty() && model.recoveryPresent);
        model.recoveryPhase="detected";model.isHost=false;
        assert(RecoveryAction("sync_decline")!="Request sent.");
        model.isHost=true;assert(RecoveryAction("sync_decline")=="Request sent.");
        assert(queue.back().line.find("sync_decline")!=std::string::npos);
        model=savedModel;queue=savedQueue;
    }
    // Transfer details survive TCP handshakes, and clear on completion for either role.
    Dispatch(R"({"type":"transfer","role":"recv","detail":"TCP 20 MiB/s","hint":"Allow TCP port 29471","pct":42})");
    assert(S().m.transferDetail=="TCP 20 MiB/s" && S().m.transferHint=="Allow TCP port 29471");
    Dispatch(R"({"type":"transfer","state":"tcp"})");
    assert(S().m.transferDetail=="TCP 20 MiB/s");
    Dispatch(R"({"type":"transfer","role":"recv","state":"done","pct":100})");
    assert(S().m.transferDetail.empty() && S().m.transferHint.empty() && S().m.xfer.empty());
    g_childPid=123;OnMenuPage(16);assert(g_loadingStagePending);
    loadPercent=37;StageTick();assert(!g_loadingStagePending && S().stageSent=="loading world 37%");
    lastStatus="stale transfer";S().stageNext=0;StageTick();assert(lastStatus=="loading world 37%");
    g_childPid=0;loadPercent=-1;S().stageWatch=false;
    lobby::Dispatch(R"({"type":"mods_refresh"})");assert(modRefreshes==1);
    // Restart selection uses real files and nanosecond mtimes, without a game.
    saveTest=true;
    const std::string restartDir = dir+"restart";
    assert(mkdir(restartDir.c_str(), 0700)==0);
    fixtureSaveDir=restartDir;
    auto dated = [&](const char* name, long ns) {
        const std::string file=restartDir+"/"+name;
        Write(file,"save");
        const timespec times[2]={{100,ns},{100,ns}};
        assert(utimensat(AT_FDCWD,file.c_str(),times,0)==0);
    };
    newestSave.clear();
    assert(DedicatedStartupSave("missing").empty());
    dated("seed.sav",100);
    assert(DedicatedStartupSave("seed")==restartDir+"/seed.sav");
    dated("autosave_mp_shared_old.sav",90);
    dated("autosave_mp_shared_equal.sav",100);
    dated("autosave_unrelated.sav",900);
    dated("mp_shared.sav",900);
    dated("autosave_mp_shared_new.sav.lua",900);
    dated("prefix_autosave_mp_shared.sav",900);
    assert(mkdir((restartDir+"/autosave_mp_shared_dir.sav").c_str(),0700)==0);
    assert(DedicatedStartupSave("seed")==restartDir+"/seed.sav");
    dated("autosave_mp_shared_new.sav",101);
    dated("autosave_mp_shared_newest.sav",102);
    assert(DedicatedStartupSave("seed")==restartDir+"/autosave_mp_shared_newest.sav");
    dated("seed.sav",103);
    assert(DedicatedStartupSave("seed")==restartDir+"/seed.sav");
    newestSave=restartDir+"/autosave_unrelated.sav";
    assert(DedicatedStartupSave("missing")==newestSave);
    assert(DedicatedStartupSave("")==newestSave);
    for (const char* name : {"seed.sav", "autosave_mp_shared_old.sav",
         "autosave_mp_shared_equal.sav", "autosave_unrelated.sav", "mp_shared.sav",
         "autosave_mp_shared_new.sav.lua", "prefix_autosave_mp_shared.sav",
         "autosave_mp_shared_new.sav", "autosave_mp_shared_newest.sav"})
        assert(unlink((restartDir+"/"+name).c_str())==0);
    assert(rmdir((restartDir+"/autosave_mp_shared_dir.sav").c_str())==0);
    assert(rmdir(restartDir.c_str())==0);
    fixtureSaveDir=dir; newestSave.clear();
    // Dedicated saves request a session hold, wait for its ack, then release
    // only after a newer save is stable. Recovery and failure paths also run.
    saveTest=true; S().cfg.dedicated.autosaveMinutes=1;
    uint64_t lastSave=0;
    nativeBusy=true; DedicatedAutosaveTick(61000,1,lastSave);
    assert(DedSave().phase==0 && savesForced==0);
    nativeBusy=false; DedicatedAutosaveTick(61000,1,lastSave);
    assert(DedSave().phase==1 && savesForced==0);
    std::string marker;
    assert(ReadSmallFile(dir+"tpf2_ded_autosave.txt",&marker) && marker=="hold\n");
    DedicatedAutosaveTick(62000,1,lastSave); assert(savesForced==0);
    Write(dir+"tpf2_ded_autosave_ack.txt","held\n");
    DedicatedAutosaveTick(63000,1,lastSave); assert(DedSave().phase==2 && savesForced==1);
    newestSave=dir+"new-auto.sav"; Write(newestSave,"fresh save");
    DedicatedAutosaveTick(64000,1,lastSave); assert(DedSave().phase==2);
    DedicatedAutosaveTick(65000,1,lastSave); assert(DedSave().phase==0);
    assert(ReadSmallFile(dir+"tpf2_ded_autosave_done.txt",&marker) && marker=="saved\n");
    DedicatedAutosaveTick(122000,1,lastSave); assert(DedSave().phase==1);
    DedicatedAutosaveTick(138000,1,lastSave); assert(DedSave().phase==0 && savesForced==1);
    DedicatedAutosaveTick(197000,1,lastSave); assert(DedSave().phase==0);
    DedicatedAutosaveTick(198000,1,lastSave); assert(DedSave().phase==1);
    Write(dir+"tpf2_ded_autosave_ack.txt","held\n"); forceAllowed=false;
    DedicatedAutosaveTick(199000,1,lastSave); assert(DedSave().phase==0 && savesForced==2);
    assert(ReadSmallFile(dir+"tpf2_ded_autosave_done.txt",&marker) && marker=="failed\n");
    forceAllowed=true; DedicatedAutosaveTick(260000,1,lastSave);
    Write(dir+"tpf2_ded_autosave_ack.txt","alone\n");
    DedicatedAutosaveTick(261000,1,lastSave); assert(DedSave().phase==2);
    DedicatedAutosaveTick(352000,1,lastSave); assert(DedSave().phase==0);
    assert(ReadSmallFile(dir+"tpf2_ded_autosave_done.txt",&marker) && marker=="timeout\n");
    for (const auto& name : {"new-auto.sav","tpf2_ded_autosave.txt","tpf2_ded_autosave_ack.txt","tpf2_ded_autosave_done.txt"}) unlink((dir+name).c_str());
    // Live join: roster growth takes the existing autosave path; the dedicated
    // waiting-member request uses that same watcher even without roster growth.
    saveTest=true; forceAllowed=true; newestSave.clear(); nativeBusy=false;
    S().syncAskedAt=0; S().sharedSave.clear();
    model.isHost=true; model.lastCount=1; model.lobbyReady=true;
    g_gameUiSeen=true; g_titleMenu=false;
    Json live;
    assert(ParseJson(R"({"players":["host","late"],"host":"host","you":"host","join_freeze":false})", &live));
    const int beforeLive=savesForced;
    ApplyRoster(live);
    assert(!model.joinFreeze && savesForced==beforeLive+1 && S().syncAskedAt);
    ApplyRoster(live); assert(savesForced==beforeLive+1);
    S().syncAskedAt=0;
    Write(dir+"tpf2_sync_save.txt", "live join\n");
    OnMenuPage(2); // No world yet: the request must survive repeated polls.
    for (int i=0; i<3; ++i) {
        SyncPoll();
        assert(Exists(dir+"tpf2_sync_save.txt"));
        assert(savesForced==beforeLive+1 && !S().syncAskedAt);
    }
    OnMenuPage(16); // Loading page alone is not readiness.
    SyncPoll();
    assert(Exists(dir+"tpf2_sync_save.txt") && savesForced==beforeLive+1);
    OnGameUiFrame();
    SyncPoll();
    assert(!Exists(dir+"tpf2_sync_save.txt") && savesForced==beforeLive+2 && S().syncAskedAt);
    SyncPoll(); assert(savesForced==beforeLive+2);
    Write(dir+"tpf2_sync_save.txt", "another joiner\n");
    SyncPoll(); // The pending save still serves every joiner.
    assert(!Exists(dir+"tpf2_sync_save.txt") && savesForced==beforeLive+2);
    newestSave=dir+"live-auto.sav"; Write(newestSave,"live world");
    SyncPoll(); SyncPoll();
    assert(!S().syncAskedAt && S().sharedSave==newestSave);
    std::string liveCommands;
    assert(ReadSmallFile(dir+"lobby_in.jsonl", &liveCommands));
    assert(liveCommands.find("\"cmd\":\"start\",\"save\":\""+newestSave+"\"")!=std::string::npos);
    assert(!Exists(dir+"tpf2_native_request.txt"));
    unlink(newestSave.c_str()); unlink((dir+"tpf2_sync_sent.txt").c_str());
    unlink((dir+"chosen.sav").c_str()); unlink((dir+"mp_company_cfg.txt").c_str());
    for (const auto& name : {"lobby_out.jsonl", "lobby_in.jsonl", "tpf2_bridge_ctl.txt"}) unlink((dir+name).c_str());
    unlink((dir+"lockstep_dash_"+letter+".txt").c_str());
    unlink((dir+"lockstep_status_"+letter+".txt").c_str());
    assert(unlink((dir + "mp_players.txt").c_str()) == 0);
    assert(unlink((dir + "mp_loading.txt").c_str()) == 0);
    assert(unlink(path.c_str())==0 && rmdir(temporary)==0);
    puts("lobby readiness: current-process hooks required for host, join and start; no external effects");
}
