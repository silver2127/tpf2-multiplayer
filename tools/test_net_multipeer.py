"""Compile production transport; exercise independent relay senders over real UDP."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/net-multipeer'
out.mkdir(parents=True, exist_ok=True)
(out / 'test.cpp').write_text('#include "' + (root / 'native/src/net.cpp').as_posix() + '"\n' + r'''
#include <cassert>
#include <functional>
#include <vector>
#include <crtdbg.h>
static std::mutex receivedMutex;
static std::vector<std::string> received;
static void delivered(const char* line) {
    std::lock_guard<std::mutex> l(receivedMutex); received.emplace_back(line);
}
static size_t count() { std::lock_guard<std::mutex> l(receivedMutex); return received.size(); }
static void waitFor(std::function<bool()> predicate) {
    auto deadline=GetTickCount64()+4000;
    while(!predicate() && GetTickCount64()<deadline) Sleep(5);
    assert(predicate());
}
static size_t pending() { size_t n; Net_Stats(nullptr,nullptr,&n,nullptr,nullptr); return n; }
int main(int argc, char** argv) {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    assert(argc==2);
    const int peers=atoi(argv[1])-1;
    assert(peers>=1);
    assert(EnsureWsa());
    SOCKET relay=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(relay,(sockaddr*)&address,sizeof(address))==0);
    int size=sizeof(address); assert(getsockname(relay,(sockaddr*)&address,&size)==0);
    assert(Net_Init(0,"127.0.0.1",ntohs(address.sin_port),delivered));
    sockaddr_in bridge=address; bridge.sin_port=htons(Net_LocalPort());
    const std::string zero(32,'0');
    auto send=[&](int sender,const std::string& epoch,uint32_t seq,const char* text,
                  uint32_t ack=NO_ACK,int chunk=0,int chunks=1,uint32_t target=0) {
        Packet p{}; p.h.magic=MAGIC; memcpy(p.h.world,epoch.data(),32);
        p.h.session=1000+sender; p.h.ackSession=target ? target : g_session;
        p.h.seq=seq; p.h.ack=ack; p.h.ackBits=0xffffffff; p.h.type=text ? 1 : 0;
        p.ev.chunkIdx=(uint16_t)chunk; p.ev.chunkCount=(uint16_t)chunks;
        if(text) strcpy_s(p.ev.text,text);
        assert(sendto(relay,(char*)&p,text ? sizeof(p) : sizeof(Header),0,(sockaddr*)&bridge,sizeof(bridge))>0);
    };
    for(int i=0;i<peers;i++) send(i,zero,0,nullptr);
    waitFor([&]{std::lock_guard<std::mutex> l(g_epochMtx); return g_streams.size()==(size_t)peers;});
    for(int round=0;round<3;round++) {
        const std::string epoch(32,char('1'+round));
        const size_t before=count();
        assert(Net_SetWorldEpoch(epoch.c_str()));
        // Interleave partial lines, reverse chunk order, repeat duplicates.
        // Keepalives ahead of a missing seq 0 must not skip that hole.
        for(int i=0;i<peers;i++) {
            send(i,epoch,2,nullptr);
            send(i,epoch,1,"tail",NO_ACK,1,2);
            send(i,epoch,1,"tail",NO_ACK,1,2);
        }
        Sleep(100); assert(count()==before);
        for(int i=0;i<peers;i++) {
            const std::string prefix=std::to_string(i)+":";
            send(i,epoch,0,prefix.c_str(),NO_ACK,0,2);
        }
        waitFor([&]{return count()==before+peers;});
        {
            std::lock_guard<std::mutex> l(receivedMutex);
            std::set<std::string> lines(received.begin()+before,received.end());
            for(int i=0;i<peers;i++) assert(lines.count(std::to_string(i)+":tail")==1);
        }
        Net_QueueLine("broadcast",epoch.c_str());
        waitFor([]{return pending()==1;});
        for(int i=0;i<peers-1;i++) send(i,epoch,2,nullptr,0);
        send(peers-1,zero,2,"stale-world",99);
        send(peers+20,epoch,0,"restarted-process",99);
        send(peers-1,epoch,2,nullptr,0,0,1,g_session+1);
        Sleep(100); assert(pending()==1 && count()==before+peers);
        // Observe a real retransmission and an ACK stream for EVERY sender.
        int transmissions=0;
        std::set<uint32_t> acknowledgements;
        const auto deadline=GetTickCount64()+1800;
        while(GetTickCount64()<deadline) {
            fd_set fds; FD_ZERO(&fds); FD_SET(relay,&fds); timeval tv{0,20000};
            if(select(0,&fds,nullptr,nullptr,&tv)<=0) continue;
            Packet p{};
            int got=recvfrom(relay,(char*)&p,sizeof(p),0,nullptr,nullptr);
            if(got<(int)sizeof(Header) || memcmp(p.h.world,epoch.data(),32)) continue;
            if(p.h.type==1 && p.h.seq==0) ++transmissions;
            if(p.h.ack==1 && (p.h.ackBits&1)) acknowledgements.insert(p.h.ackSession);
        }
        assert(transmissions>=2 && acknowledgements.size()==(size_t)peers);
        assert(pending()==1); // fastest peers cannot ACK on behalf of the lagger
        send(peers-1,epoch,2,nullptr,0);
        waitFor([]{return pending()==0;});
        for(int i=0;i<peers;i++) send(i,epoch,1,"duplicate",NO_ACK,1,2);
        Sleep(50); assert(count()==before+peers);
    }
    Net_Shutdown(); closesocket(relay);
    printf("PASS: %d players, 3 epochs, interleaved/reordered chunks, duplicates, all-peer ACKs, retransmission, stale world/process rejection\n",peers+1);
}
''', encoding='utf-8')
(out / 'build.cmd').write_text(f'@echo off\ncall "{root / "tools/msvc_env.bat"}" || exit /b 1\n'
    'cl /nologo /EHsc /W4 test.cpp /Fe:test.exe >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\nexit /b 0\n', encoding='utf-8')
subprocess.run(['cmd', '/d', '/c', str(out / 'build.cmd')], cwd=out, check=True, timeout=180)
for players in (2, 3, 5, 8):
    subprocess.run([str(out / 'test.exe'), str(players)], cwd=out, check=True, timeout=30)
