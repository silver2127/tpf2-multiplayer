// Actual lobby entry points, without Init or worker threads: no subprocesses,
// network requests, save reads or game calls may occur in these tests.
#include "../src/lobby_linux.cpp"
#include <cassert>

static std::string fixtureSaveDir;
static bool allowPlace=false;
static int placed=0;
static int loadPercent=-1;
int MenuGame_LoadPercent() { return loadPercent; }
std::string MenuGame_SaveDir() { assert(!fixtureSaveDir.empty()); return fixtureSaveDir; }
bool MenuGame_ForceAutosave() { assert(false && "unexpected autosave"); return false; }
bool MenuGame_NewestSave(std::string*) { assert(false && "unexpected save lookup"); return false; }

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
    // A world switch consumes its own transfer once, with a manual load in-game.
    model.players={"host","joiner"}; model.companies={1,2}; model.you="joiner";
    model.isHost=false; model.saveReady=true; allowPlace=true;
    lobby::Json sw; assert(lobby::ParseJson("{\"save\":true,\"switch\":true}",&sw));
    lobby::HandleStart(sw); assert(placed==1 && !model.saveReady);
    lobby::HandleStart(sw); assert(placed==1); // stale start cannot reuse an old transfer
    model.isHost=true; model.saveReady=true;
    lobby::HandleStart(sw); assert(placed==1); // host keeps its already loaded world
    // A vanilla load queues the same session and shares its named file.
    fixtureSaveDir=dir; Write(dir+"chosen.sav","fixture");
    model.lobbyReady=true; lobby::OnMenuLoad("chosen");
    assert(lobby::S().q.back().kind==lobby::Request::LoadedSave && lobby::S().q.back().gen==model.gen);
    lobby::ShareLoadedSave("chosen"); assert(lobby::S().hostLoadedItself && lobby::S().worldGenHold);
    assert(lobby::ReadSmallFile(dir+"lobby_in.jsonl",&sent) && sent.find("\"switch\":true")!=std::string::npos);
    lobby::HandleStart(sw); assert(!lobby::S().hostLoadedItself && placed==1);
    unlink((dir+"chosen.sav").c_str()); unlink((dir+"mp_company_cfg.txt").c_str());
    for (const auto& name : {"lobby_out.jsonl", "lobby_in.jsonl", "tpf2_bridge_ctl.txt"}) unlink((dir+name).c_str());
    unlink((dir+"lockstep_dash_"+letter+".txt").c_str());
    unlink((dir+"lockstep_status_"+letter+".txt").c_str());
    assert(unlink((dir + "mp_players.txt").c_str()) == 0);
    assert(unlink((dir + "mp_loading.txt").c_str()) == 0);
    assert(unlink(path.c_str())==0 && rmdir(temporary)==0);
    puts("lobby readiness: current-process hooks required for host, join and start; no external effects");
}
