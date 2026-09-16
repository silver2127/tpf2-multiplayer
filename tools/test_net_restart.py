"""A joiner whose game restarted, over real loopback UDP against the compiled transport.

The field case (2026-09-15): a player's game restarted between attempts, so its
bridge came back under a new random session. The host's transport had frozen its
roster at the last world change and refused the new session for the rest of the
lobby's life, while every broadcast waited for the dead one -- the joiner sat in
the load gate with "0 peer(s) in" and the host read "pending=33". This drives the
host side of that: admission of a new session that presents the current world,
eviction of the silent old one, the old world still rejected, and a receiver
whose sender has moved past what it waited for.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/net-restart'
out.mkdir(parents=True, exist_ok=True)
(out / 'test.cpp').write_text('#include "' + (root / 'native/src/net.cpp').as_posix() + '"\n' + r'''
#include <cassert>
#include <functional>
#include <vector>
#include <crtdbg.h>
static std::mutex receivedMutex;
static std::vector<std::string> received, logged;
static void delivered(const char* line) { std::lock_guard<std::mutex> l(receivedMutex); received.emplace_back(line); }
static void logger(const char* line) { std::lock_guard<std::mutex> l(receivedMutex); logged.emplace_back(line); }
static size_t count() { std::lock_guard<std::mutex> l(receivedMutex); return received.size(); }
static std::string lastLine() { std::lock_guard<std::mutex> l(receivedMutex); return received.empty() ? "" : received.back(); }
static size_t loggedLines(const char* needle) {
    std::lock_guard<std::mutex> l(receivedMutex);
    size_t n=0; for(const auto& s : logged) if(s.find(needle)!=std::string::npos) ++n; return n;
}
static void waitFor(std::function<bool()> predicate) {
    auto deadline=GetTickCount64()+4000;
    while(!predicate() && GetTickCount64()<deadline) Sleep(5);
    assert(predicate());
}
static size_t pending() { size_t n; Net_Stats(nullptr,nullptr,&n,nullptr,nullptr); return n; }
static size_t members() { size_t n; Net_Stats(nullptr,nullptr,nullptr,nullptr,nullptr,&n); return n; }
static bool alive() { bool yes; Net_Stats(nullptr,nullptr,nullptr,&yes,nullptr); return yes; }
static bool member(uint32_t session) { std::lock_guard<std::mutex> l(g_epochMtx); return g_streams.count(session)==1; }
int main() {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    assert(EnsureWsa());
    SOCKET relay=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in endpoint{}; endpoint.sin_family=AF_INET; endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(relay,(sockaddr*)&endpoint,sizeof(endpoint))==0);
    int size=sizeof(endpoint); assert(getsockname(relay,(sockaddr*)&endpoint,&size)==0);
    Net_SetLogger(logger);
    assert(Net_Init(0,"127.0.0.1",ntohs(endpoint.sin_port),delivered));
    g_peerTimeoutMs=1500;  // the production 10 s, shortened: liveness AND eviction (600 left a slow box ~400 ms of slack)
    sockaddr_in bridge=endpoint; bridge.sin_port=htons(Net_LocalPort());
    const std::string lobby(32,'a'), world(32,'1');
    const uint32_t OLD=77, NEW=78;   // the joiner's process before and after its restart
    auto send=[&](uint32_t session,const std::string& epoch,uint32_t seq,const char* line,
                  uint32_t ack=NO_ACK,uint32_t bits=0,int chunk=0,int chunks=1) {
        Packet p{}; p.h.magic=MAGIC; memcpy(p.h.world,epoch.data(),32);
        p.h.session=session; p.h.ackSession=g_session; p.h.seq=seq; p.h.ack=ack; p.h.ackBits=bits;
        p.h.type=line ? 1 : 0; p.ev.chunkIdx=(uint16_t)chunk; p.ev.chunkCount=(uint16_t)chunks;
        if(line) strcpy_s(p.ev.text,line);
        assert(sendto(relay,(char*)&p,line ? sizeof(p) : sizeof(Header),0,(sockaddr*)&bridge,sizeof(bridge))>0);
    };
    // keep `session` alive with keepalives (acknowledging `ack`) while waiting
    auto keepAliveUntil=[&](uint32_t session,uint32_t ack,std::function<bool()> predicate) {
        auto deadline=GetTickCount64()+4000;
        while(!predicate() && GetTickCount64()<deadline) { send(session,world,0,nullptr,ack,ack==NO_ACK?0:0xffffffff); Sleep(50); }
        assert(predicate());
    };
    auto reset=[](const char*) {};

    // 1. the joiner's first process is in the lobby, then a fresh shared-save session
    assert(Net_BeginLobby(lobby.c_str(),"127.0.0.1",ntohs(endpoint.sin_port),reset));
    send(OLD,lobby,0,nullptr); waitFor(alive);
    assert(member(OLD) && members()==1);
    assert(Net_SetWorldEpoch(world.c_str(),reset));
    assert(member(OLD) && loggedLines("1 member(s) carried over")==1);
    send(OLD,world,0,"before-restart"); waitFor([]{return count()==1;});
    Net_QueueLine("host-line",world.c_str()); waitFor([]{return pending()==1;});
    // ...and its process dies with that line unacknowledged.

    // 2. the game restarts: a new session, the same world (it has it from the lobby)
    send(NEW,world,0,nullptr);
    waitFor([&]{return member(NEW);});
    assert(members()==2 && loggedLines("session 0000004e joins world 11111111..")==1);
    send(NEW,world,0,"after-restart"); waitFor([]{return count()==2;});
    assert(lastLine()=="after-restart");

    // 3. the dead process is evicted for silence and its hold on the window is released;
    //    the live one keeps talking meanwhile and is untouched
    send(NEW,world,1,nullptr,0,0xffffffff);   // NEW acknowledges the host's seq 0
    Sleep(150); assert(pending()==1);          // OLD is still awaited: not yet silent for long enough
    keepAliveUntil(NEW,0,[&]{return !member(OLD);});
    assert(pending()==0 && member(NEW) && members()==1 && alive());
    assert(loggedLines("session 0000004d silent for 1500 ms -- out of the cohort; 1 packet(s) stop waiting for it, 1 member(s) remain")==1);

    // 4. the old world is still rejected whoever claims it -- and the right world admits
    //    even the evicted session id: the epoch is the credential, not the id
    send(NEW,lobby,1,"old-world"); send(OLD,lobby,1,"old-world"); send(OLD,world,1,"old-session-in-new-world");
    Sleep(150); assert(count()==2 && member(OLD));   // data before its floor is known: admitted, not delivered
    assert(loggedLines("is in world aaaaaaaa.., we are in 11111111..")==2);
    { uint64_t w=0; Net_Stats(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&w); assert(w==2); }
    { std::lock_guard<std::mutex> l(g_epochMtx); g_streams.erase(OLD); }

    // 5. the sender moved past what we waited for (we were out of its cohort meanwhile):
    //    skip to its floor, abandon the half line, deliver from the floor on
    send(NEW,world,1,"half-",NO_ACK,0,0,2);   // chunk 0 of 2 at seq 1: assembling
    Sleep(50); assert(count()==2);
    send(NEW,world,5,nullptr,0,0xffffffff);   // keepalive: nothing retained before seq 5
    waitFor([&]{return loggedLines("retains nothing before seq 5 and we waited for 2: 0 stashed packet(s) delivered, 3 went by")==1;});
    send(NEW,world,5,"-tail",NO_ACK,0,1,2);   // the tail of a line whose head is gone: never delivered
    send(NEW,world,6,"after-gap"); waitFor([]{return count()==3;});
    assert(lastLine()=="after-gap");
    send(NEW,world,4,nullptr,0,0xffffffff);   // a late keepalive with an older floor changes nothing
    send(NEW,world,7,"in-order"); waitFor([]{return count()==4;});

    // 5b. what we STASHED below a floor was acknowledged, so the sender erased it:
    //     it is delivered before the skip, and a line cut by a hole is abandoned
    send(NEW,world,10,"stashed-ten");         // expected 8: stashed and acked
    Sleep(50); assert(count()==4);
    send(NEW,world,11,nullptr,0,0xffffffff);  // floor 11: 8 and 9 are gone, 10 is ours
    waitFor([]{return count()==5;});
    assert(lastLine()=="stashed-ten");
    assert(loggedLines("retains nothing before seq 11 and we waited for 8: 1 stashed packet(s) delivered, 2 went by")==1);
    send(NEW,world,11,"after-skip"); waitFor([]{return count()==6;});
    send(NEW,world,12,"head-",NO_ACK,0,0,2);  // chunk 0 of 2, in order: assembling
    send(NEW,world,14,"-x",NO_ACK,0,1,2);     // chunk 1 of 2 at 14: stashed across a hole at 13
    send(NEW,world,15,nullptr,0,0xffffffff);  // floor 15: the hole cuts the line; nothing short is delivered
    waitFor([&]{return loggedLines("retains nothing before seq 15 and we waited for 13: 1 stashed packet(s) delivered, 1 went by")==1;});
    send(NEW,world,15,"clean"); waitFor([]{return count()==7;});
    assert(lastLine()=="clean");

    // 6. the last member evicted: nobody is listening again -- what waited for it and
    //    what was queued is dropped, new lines are dropped at the queue, and when it is
    //    back it is admitted afresh, told the floor, and gets nothing old
    Net_QueueLine("to-new",world.c_str()); waitFor([]{return pending()==1;});
    waitFor([&]{return !member(NEW);});
    assert(members()==0 && pending()==0 && !alive());
    assert(loggedLines("cohort empty -- 1 pending/queued line(s) dropped, nobody is listening")==1);
    { uint64_t noPeer=0; Net_Stats(&noPeer,nullptr,nullptr,nullptr,nullptr);
      Net_QueueLine("to-nobody",world.c_str()); Sleep(50);
      uint64_t after=0; Net_Stats(&after,nullptr,nullptr,nullptr,nullptr);
      assert(after==noPeer+1 && pending()==0); }
    send(NEW,world,17,nullptr,0,0xffffffff); waitFor([&]{return member(NEW);});
    send(NEW,world,16,"before-the-floor"); send(NEW,world,17,"returned"); waitFor([]{return count()==8;});
    assert(lastLine()=="returned" && loggedLines("joins world 11111111..")==3);
    Net_QueueLine("fresh",world.c_str()); waitFor([]{return pending()==1;});
    send(NEW,world,18,nullptr,2,0xffffffff);  // "fresh" is the host's seq 2 (0 and 1 were dropped, never resent)
    waitFor([]{return pending()==0;});

    Net_Shutdown(); closesocket(relay);
    puts("PASS: restarted joiner admitted into the current world, dead session evicted and its packets released, "
         "old world rejected, receiver skips to a sender's floor, re-admission after eviction");
}
''', encoding='utf-8')
(out / 'build.cmd').write_text(f'@echo off\ncall "{root / "tools/msvc_env.bat"}" || exit /b 1\n'
    'cl /nologo /EHsc /W4 test.cpp /Fe:test.exe >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\nexit /b 0\n', encoding='utf-8')
subprocess.run(['cmd', '/d', '/c', str(out / 'build.cmd')], cwd=out, check=True, timeout=180)
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=30)
