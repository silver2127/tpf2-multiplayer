// Adapted from upstream tools/test_net_packet_size.py (0.5.3).
#include "../src/net_posix.cpp"
#include <cassert>
static void TestSleep(unsigned ms) { usleep(ms * 1000); }
template<size_t N> static void strcpy_s(char (&out)[N], const char* value) {
    assert(strlen(value) < N); memcpy(out, value, strlen(value) + 1);
}

#include <cassert>
#include <functional>
#include <vector>

static std::mutex receivedMutex;
static std::vector<std::string> received;
static void delivered(const char* line) { std::lock_guard<std::mutex> l(receivedMutex); received.emplace_back(line); }
static size_t count() { std::lock_guard<std::mutex> l(receivedMutex); return received.size(); }
static std::string nth(size_t i) { std::lock_guard<std::mutex> l(receivedMutex); return received[i]; }
static void waitFor(std::function<bool()> predicate) {
    auto deadline=NowMs()+4000;
    while(!predicate() && NowMs()<deadline) TestSleep(5);
    assert(predicate());
}
static bool alive() { bool yes; Net_Stats(nullptr,nullptr,nullptr,&yes,nullptr); return yes; }
static const int HEAD=(int)(sizeof(Header)+offsetof(NetEvent,text));
int main() {
    assert(sizeof(Header)==57 && HEAD==62 && sizeof(Packet)==1086);

    int peer=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    sockaddr_in endpoint{}; endpoint.sin_family=AF_INET; endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(peer,(sockaddr*)&endpoint,sizeof(endpoint))==0);
    socklen_t size=sizeof(endpoint); assert(getsockname(peer,(sockaddr*)&endpoint,&size)==0);
    assert(Net_Init(0,"127.0.0.1",ntohs(endpoint.sin_port),delivered));
    sockaddr_in bridge=endpoint; bridge.sin_port=htons(Net_LocalPort());
    const std::string zero(32,'0');
    // a datagram from this test's peer (session 77): `bytes` long, or sized as a protocol-5 sender sizes it
    auto send=[&](uint32_t seq,const char* line,int bytes=0,uint32_t magic=MAGIC) {
        Packet p{}; p.h.magic=magic; memcpy(p.h.world,zero.data(),32);
        p.h.session=77; p.h.ackSession=g_session; p.h.seq=seq; p.h.ack=NO_ACK;
        p.h.type=line ? 1 : 0; p.ev.type=1; p.ev.chunkIdx=0; p.ev.chunkCount=1;
        if(line) strcpy_s(p.ev.text,line);
        int n=bytes ? bytes : (line ? HEAD+(int)strlen(line)+1 : (int)sizeof(Header));
        assert(sendto(peer,(char*)&p,n,0,(sockaddr*)&bridge,sizeof(bridge))==n);
    };
    // the bridge's next EVENT datagram to us; a keepalive in between must be the header alone
    auto nextEvent=[&](Packet& got)->int {
        auto deadline=NowMs()+3000;
        while(NowMs()<deadline) {
            fd_set fds; FD_ZERO(&fds); FD_SET(peer,&fds); timeval tv{0,50000};
            if(select(peer + 1,&fds,nullptr,nullptr,&tv)<=0) continue;
            got=Packet{};
            int n=recv(peer,(char*)&got,sizeof(got),0);
            if(n>(int)sizeof(Header) && got.h.type==1) return n;
            assert(n==(int)sizeof(Header));
        }
        assert(!"no event datagram arrived");
        return -1;
    };
    send(0,nullptr); waitFor(alive);

    // SENDING: "hello" (5 characters and the NUL), then a 2,500-character line in three chunks
    Net_QueueLine("hello");
    std::string big(2500,'x');
    for(int i=0;i<2500;i++) big[i]=(char)('a'+i%26);
    Net_QueueLine(big.c_str());
    std::map<uint32_t,int> sizes;
    std::map<uint32_t,std::string> texts;
    while(sizes.size()<4) {
        Packet p{}; int n=nextEvent(p);
        auto it=sizes.find(p.h.seq);
        if(it!=sizes.end()) { assert(it->second==n); continue; }   // a resend is sized the same
        assert(p.h.magic==MAGIC);
        sizes[p.h.seq]=n; texts[p.h.seq]=p.ev.text;
    }
    assert(sizes[0]==HEAD+6 && texts[0]=="hello");
    assert(sizes[1]==HEAD+1024 && sizes[2]==HEAD+1024 && sizes[3]==HEAD+455);
    assert(texts[1]+texts[2]+texts[3]==big);

    // RECEIVING: a trimmed event, then a full-size one as protocol 4 sized every event
    send(0,"short");
    waitFor([]{return count()==1;}); assert(nth(0)=="short");
    send(1,"full",(int)sizeof(Packet));
    waitFor([]{return count()==2;}); assert(nth(1)=="full");
    // dropped: cut before its text ends, too short for even a NUL, and a protocol-4 magic
    send(2,"truncated",HEAD+5);
    send(2,"",HEAD);
    send(2,"old magic",0,0x34545046);
    TestSleep(250); assert(count()==2);
    send(2,"whole");
    waitFor([]{return count()==3;}); assert(nth(2)=="whole");
    Net_Shutdown(); close(peer);
    printf("PASS: events sized to their text (hello %d B, a 2,500-char line %d/%d/%d B, was 1086 each), "
           "trimmed and full-size events read, truncated/empty/protocol-4 datagrams dropped\n",
           sizes[0], sizes[1], sizes[2], sizes[3]);
}
