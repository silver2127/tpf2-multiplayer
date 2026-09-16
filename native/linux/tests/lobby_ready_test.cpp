// Actual lobby entry points, without Init or worker threads: no subprocesses,
// network requests, save reads or game calls may occur in these tests.
#include "../src/lobby_linux.cpp"
#include <cassert>

std::string MenuGame_SaveDir() { assert(false && "unexpected save lookup"); return {}; }
bool MenuGame_NewestSave(std::string*) { assert(false && "unexpected save lookup"); return false; }

static void Write(const std::string& path, const std::string& body)
{
    FILE* f = fopen(path.c_str(), "w");
    assert(f && fwrite(body.data(), 1, body.size(), f)==body.size());
    assert(fclose(f)==0);
}

int main()
{
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
        request.join=join;
        assert(lobby::Start(request,&why));
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
    assert(unlink(path.c_str())==0 && rmdir(temporary)==0);
    puts("lobby readiness: current-process hooks required for host, join and start; no external effects");
}
