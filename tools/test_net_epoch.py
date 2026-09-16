"""Exercise the compiled transport over real loopback UDP across world resets."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/net-epoch'
out.mkdir(parents=True, exist_ok=True)
(out / 'test.cpp').write_text('#include "' + (root / 'native/src/net.cpp').as_posix() + '"\n' + r'''
#include <cassert>
#include <functional>
#include <vector>
#include <crtdbg.h>
static std::mutex receivedMutex;
static std::vector<std::string> received;
static int resets=0;
static void delivered(const char* line) { std::lock_guard<std::mutex> l(receivedMutex); received.emplace_back(line); }
static size_t count() { std::lock_guard<std::mutex> l(receivedMutex); return received.size(); }
static void waitFor(std::function<bool()> predicate) {
    auto deadline=GetTickCount64()+4000;
    while(!predicate() && GetTickCount64()<deadline) Sleep(5);
    assert(predicate());
}
static size_t pending() { size_t n; Net_Stats(nullptr,nullptr,&n,nullptr,nullptr); return n; }
static bool alive() { bool yes; Net_Stats(nullptr,nullptr,nullptr,&yes,nullptr); return yes; }
int main() {
    // CI has no interactive desktop: failed assertions must not open a dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    assert(EnsureWsa());
    SOCKET peer=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in endpoint{}; endpoint.sin_family=AF_INET; endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(peer,(sockaddr*)&endpoint,sizeof(endpoint))==0);
    int size=sizeof(endpoint); assert(getsockname(peer,(sockaddr*)&endpoint,&size)==0);
    assert(Net_Init(0,"127.0.0.1",ntohs(endpoint.sin_port),delivered));
    sockaddr_in bridge=endpoint; bridge.sin_port=htons(Net_LocalPort());
    const std::string zero(32,'0'), first(32,'1'), second(32,'2');
    auto send=[&](const std::string& epoch,uint32_t seq,const char* line,uint32_t ack=NO_ACK,int chunk=0,int chunks=1) {
        Packet p{}; p.h.magic=MAGIC; memcpy(p.h.world,epoch.data(),32);
        p.h.session=77; p.h.ackSession=g_session; p.h.seq=seq; p.h.ack=ack;
        p.h.type=line ? 1 : 0; p.ev.chunkIdx=(uint16_t)chunk; p.ev.chunkCount=(uint16_t)chunks;
        if(line) strcpy_s(p.ev.text,line);
        assert(sendto(peer,(char*)&p,line ? sizeof(p) : sizeof(Header),0,(sockaddr*)&bridge,sizeof(bridge))>0);
    };
    send(zero,0,nullptr); waitFor(alive);
    send(zero,0,"old partial",NO_ACK,0,2);
    Net_QueueLine("old pending"); waitFor([]{return pending()>0;});
    auto reset=[](const char*) { ++resets; };
    assert(Net_SetWorldEpoch(first.c_str(),reset));
    assert(resets==1 && pending()==0 && count()==0);
    send(first,0,nullptr); waitFor(alive);
    Net_QueueLine("new pending",first.c_str()); waitFor([]{return pending()==1;});
    send(zero,0,"stale command",100); // must not ACK the new seq 0
    Net_QueueLine("stale tail read",zero.c_str());
    Sleep(150);
    assert(count()==0 && pending()==1);
    assert(Net_SetWorldEpoch(first.c_str(),reset));
    assert(resets==1 && pending()==1); // duplicate control is idempotent
    send(first,1,"second"); send(first,0,"first"); // reorder and recover hole
    waitFor([]{return count()==2;});
    send(first,0,"duplicate"); Sleep(100); assert(count()==2);
    { std::lock_guard<std::mutex> l(receivedMutex); assert(received[0]=="first" && received[1]=="second"); }
    send(first,2,"unfinished",NO_ACK,0,2);
    Sleep(100);
    assert(Net_SetWorldEpoch(second.c_str(),reset));
    send(first,3,"old tail",NO_ACK,1,2);
    send(second,0,"fresh"); waitFor([]{return count()==3;});
    { std::lock_guard<std::mutex> l(receivedMutex); assert(received.back()=="fresh"); }
    assert(!Net_SetWorldEpoch("bad") && !Net_SetWorldEpoch(zero.c_str()));
    // Rehost in the same process with an abandoned 33-packet send window.
    {
        std::lock_guard<std::mutex> e(g_epochMtx);
        std::lock_guard<std::mutex> l(g_mtx);
        for(uint32_t i=0;i<33;i++) {
            g_pending[i]=Packet{}; g_awaiting[i]={77,88};
        }
        g_streams[88]=PeerStream{};
        g_nextSeq=33;
        g_outQueue.push(NetEvent{});
    }
    const std::string lobbyEpoch(32,'a'), recovered(32,'b');
    const uint32_t oldSession=g_session;
    assert(Net_BeginLobby(lobbyEpoch.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    assert(g_session!=oldSession && pending()==0 && !alive());
    { std::lock_guard<std::mutex> l(g_epochMtx);
      assert(g_streams.empty() && g_awaiting.empty() && g_outQueue.empty()); }
    const int lobbyResets=resets;
    send(second,0,"old-lobby-data",32);
    Sleep(100); assert(!alive() && count()==3);
    send(lobbyEpoch,500,nullptr); waitFor(alive);
    send(lobbyEpoch,500,"new-lobby"); waitFor([]{return count()==4;});
    Net_QueueLine("new command",lobbyEpoch.c_str()); waitFor([]{return pending()==1;});
    assert(Net_BeginLobby(lobbyEpoch.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    assert(resets==lobbyResets && pending()==1); // roster refresh keeps live traffic
    assert(Net_SetPeer("127.0.0.1",ntohs(endpoint.sin_port)) && pending()==1);
    assert(Net_SetWorldEpoch(recovered.c_str(),reset));
    assert(Net_BeginLobby(lobbyEpoch.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    assert(Net_WorldEpoch()==recovered && resets==lobbyResets+1); // no undo of resync
    assert(!Net_BeginLobby("invalid","127.0.0.1",1,reset));
    // After a resync the lobby advertises the resync epoch as its nonce, so a
    // later joiner starts in that world. For a member already in it the new
    // nonce is a rename: no reset, same session, the cohort and its traffic kept.
    send(recovered,0,nullptr); waitFor(alive);
    Net_QueueLine("after resync",recovered.c_str()); waitFor([]{return pending()==1;});
    const uint32_t resyncSession=g_session;
    assert(Net_BeginLobby(recovered.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    assert(g_session==resyncSession && pending()==1 && alive() && resets==lobbyResets+1);
    assert(Net_WorldEpoch()==recovered);
    { std::lock_guard<std::mutex> l(g_epochMtx); assert(g_streams.count(77)==1 && g_lobbyEpoch==recovered); }
    assert(Net_BeginLobby(recovered.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    assert(g_session==resyncSession && pending()==1); // and idempotent from then on
    Net_Shutdown(); closesocket(peer);
    puts("PASS: real UDP world reset, stale ACK/data/tail, duplicate control, reordered data, partial chunks, lobby nonce catching up with a resync");
}
''', encoding='utf-8')
vcvars = root / 'tools' / 'msvc_env.bat'
(out / 'build.cmd').write_text(f'@echo off\ncall "{vcvars}" || exit /b 1\n'
    'cl /nologo /EHsc /W4 test.cpp /Fe:test.exe >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\n'
    'exit /b 0\n', encoding='utf-8')
# Initial toolchain discovery can take longer on a cold hosted runner. Keep
# execution separately bounded so a compiler timeout cannot hide a hung test.
subprocess.run(['cmd', '/d', '/c', str(out / 'build.cmd')], cwd=out, check=True, timeout=180)  # absolute: a relative name fails on some shells
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=30)
