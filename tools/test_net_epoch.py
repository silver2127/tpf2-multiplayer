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
    Net_Shutdown(); closesocket(peer);
    puts("PASS: real UDP world reset, stale ACK/data/tail, duplicate control, reordered data, partial chunks");
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
