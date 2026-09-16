// POSIX build of the bridge transport (net.h). The wire format, the ack bitmap
// and every liveness, overflow and filtering rule mirror net.cpp exactly; the
// reasons behind them are commented there. Only the platform calls differ: BSD
// sockets for Winsock, CLOCK_MONOTONIC for GetTickCount64, std::thread for
// CreateThread.
//
// Container state is leaked on purpose: the net thread is still running when
// exit() runs static destructors, and a destroyed map under it would crash the
// game on its way out.
#include "net.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

static const uint32_t MAGIC = 0x32545046;  // 'TPF2'
static const int RESEND_MS = 250;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t session;   // sender instance id; receiver resets seq state on change
    uint32_t seq;
    uint32_t ack;
    uint32_t ackBits;
    uint8_t  type;      // 0 = keepalive/ack-only, 1 = event
};
struct Packet {
    Header   h;
    NetEvent ev;
};
#pragma pack(pop)

struct NetState {
    std::mutex peerMtx;                       // g_peer's own lock, as in net.cpp
    sockaddr_in peer{};
    std::string rxAccum;                      // partial line being reassembled
    std::map<uint32_t, Packet> pending;       // sent, awaiting ack
    std::map<uint32_t, uint64_t> lastSent;    // seq -> last send time
    std::map<uint32_t, Packet> early;         // received out of order
    std::queue<NetEvent> outQueue;
    std::mutex mtx;
    std::thread thread;
};
static NetState& N()
{
    static NetState* s = new NetState;
    return *s;
}

static std::atomic<int> g_sock{-1};
static uint16_t g_localPort = 0;
static std::atomic<bool> g_loopbackOnly{true};
static uint64_t g_droppedStrangers = 0;       // under N().mtx
static void (*g_deliver)(const char*) = nullptr;
static std::atomic<bool> g_running{false};

static uint32_t g_nextSeq = 0;
static uint32_t g_expectedSeq = 0;
static uint32_t g_session = 0;
static uint32_t g_peerSession = 0;
static uint32_t g_lastReceivedSeq = 0;
static uint32_t g_receivedBits = 0;

static const uint32_t PEER_TIMEOUT_MS   = 10000;
static const uint32_t KEEPALIVE_MS      = 500;
static const size_t   MAX_PENDING       = 512;
static uint64_t g_lastRecvMs   = 0;
static bool     g_peerEverSeen = false;
static uint64_t g_droppedNoPeer = 0;
static uint64_t g_droppedOverflow = 0;
static uint64_t g_droppedOversize = 0;

static uint64_t NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool PeerAlive()
{
    return g_peerEverSeen && (NowMs() - g_lastRecvMs) < PEER_TIMEOUT_MS;
}

void Net_Stats(uint64_t* droppedNoPeer, uint64_t* droppedOverflow,
               size_t* pending, bool* peerAlive, uint64_t* droppedOversize)
{
    std::lock_guard<std::mutex> lk(N().mtx);
    if (droppedNoPeer)   *droppedNoPeer = g_droppedNoPeer;
    if (droppedOverflow) *droppedOverflow = g_droppedOverflow;
    if (pending)         *pending = N().pending.size();
    if (peerAlive)       *peerAlive = PeerAlive();
    if (droppedOversize) *droppedOversize = g_droppedOversize;
}

static void SendRaw(uint32_t seq, uint32_t type, const NetEvent* ev)
{
    Packet p{};
    p.h.magic = MAGIC;
    p.h.session = g_session;
    p.h.seq = seq;
    p.h.ack = g_lastReceivedSeq;
    p.h.ackBits = g_receivedBits;
    p.h.type = (uint8_t)type;
    if (ev) p.ev = *ev;
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(N().peerMtx);
        peer = N().peer;
    }
    int sock = g_sock;
    if (sock < 0) return;
    sendto(sock, &p, ev ? sizeof(p) : sizeof(Header), 0, (sockaddr*)&peer, sizeof(peer));
}

static void ProcessAck(uint32_t ack, uint32_t bits)
{
    std::lock_guard<std::mutex> lk(N().mtx);
    for (auto it = N().pending.begin(); it != N().pending.end();) {
        uint32_t s = it->first;
        bool acked = (s == ack) || (s < ack && ((ack - s) > 32)) ||
                     (s < ack && (bits & (1u << (ack - s - 1))));
        if (acked) { N().lastSent.erase(s); it = N().pending.erase(it); }
        else { ++it; }
    }
}

static void Reassemble(const NetEvent& ev)
{
    if (!g_deliver) return;
    size_t n = strnlen(ev.text, NET_CHUNK_TEXT);
    if (ev.chunkIdx == 0) N().rxAccum.clear();
    N().rxAccum.append(ev.text, n);
    if (ev.chunkIdx + 1 >= ev.chunkCount) {
        g_deliver(N().rxAccum.c_str());
        N().rxAccum.clear();
    }
}

static const uint32_t EARLY_WINDOW = 32;

static void NoteReceived(uint32_t s)
{
    if (s == g_lastReceivedSeq) return;
    if (s > g_lastReceivedSeq) {
        uint32_t shift = s - g_lastReceivedSeq;
        if (shift > 32)       g_receivedBits = 0;
        else if (shift == 32) g_receivedBits = 1u << 31;
        else                  g_receivedBits = (g_receivedBits << shift) | (1u << (shift - 1));
        g_lastReceivedSeq = s;
    } else {
        uint32_t back = g_lastReceivedSeq - s;
        if (back <= 32) g_receivedBits |= (1u << (back - 1));
    }
}

static void DeliverInOrder(const Packet& p)
{
    uint32_t s = p.h.seq;
    if (s < g_expectedSeq) {
        NoteReceived(s);
        return;
    }
    if (s > g_expectedSeq) {
        if (s - g_expectedSeq < EARLY_WINDOW) {
            N().early[s] = p;
            NoteReceived(s);
        }
        return;
    }
    if (p.h.type == 1) Reassemble(p.ev);
    NoteReceived(s);
    g_expectedSeq++;
    for (;;) {
        auto it = N().early.find(g_expectedSeq);
        if (it == N().early.end()) break;
        if (it->second.h.type == 1) Reassemble(it->second.ev);
        NoteReceived(g_expectedSeq);
        N().early.erase(it);
        g_expectedSeq++;
    }
}

static void NetThread()
{
    while (g_running) {
        int sock = g_sock;
        if (sock < 0) break;
        // 1. flush outbound queue
        for (;;) {
            NetEvent ev;
            {
                std::lock_guard<std::mutex> lk(N().mtx);
                if (N().outQueue.empty()) break;
                ev = N().outQueue.front();
                N().outQueue.pop();
            }
            uint32_t seq = g_nextSeq++;
            Packet p{};
            p.h.magic = MAGIC; p.h.seq = seq; p.h.type = 1; p.ev = ev;
            {
                std::lock_guard<std::mutex> lk(N().mtx);
                N().pending[seq] = p;
                N().lastSent[seq] = NowMs();
            }
            SendRaw(seq, 1, &ev);
        }
        // 2. resend unacked
        {
            std::lock_guard<std::mutex> lk(N().mtx);
            if (N().pending.size() > MAX_PENDING) {
                g_droppedOverflow += N().pending.size();
                N().pending.clear();
                N().lastSent.clear();
                g_peerEverSeen = false;
            }
            for (auto& kv : N().pending) {
                uint64_t& last = N().lastSent[kv.first];
                if ((int)(NowMs() - last) > RESEND_MS) {
                    last = NowMs();
                    SendRaw(kv.first, 1, &kv.second.ev);
                }
            }
        }
        // 2b. keepalive
        {
            static uint64_t lastKeepalive = 0;
            uint64_t now = NowMs();
            if (now - lastKeepalive >= KEEPALIVE_MS) {
                lastKeepalive = now;
                std::lock_guard<std::mutex> lk(N().mtx);
                SendRaw(g_nextSeq, 0, nullptr);
            }
        }
        // 3. receive. Linux select() writes the time left back into the
        // timeval, so it is rebuilt every pass; reusing it would spin at 0.
        timeval tv{ 0, 20000 };
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        int n = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (n > 0 && FD_ISSET(sock, &fds)) {
            Packet p{};
            sockaddr_in from{}; socklen_t fromLen = sizeof(from);
            ssize_t got = recvfrom(sock, &p, sizeof(p), 0, (sockaddr*)&from, &fromLen);
            in_addr peerAddr;
            {
                std::lock_guard<std::mutex> lk(N().peerMtx);
                peerAddr = N().peer.sin_addr;
            }
            if (got >= 0 && from.sin_addr.s_addr != peerAddr.s_addr) {
                std::lock_guard<std::mutex> lk(N().mtx);
                g_droppedStrangers++;
            } else if (got >= (ssize_t)sizeof(Header) && p.h.magic == MAGIC) {
                {
                    std::lock_guard<std::mutex> lk(N().mtx);
                    g_lastRecvMs = NowMs();
                    g_peerEverSeen = true;
                }
                if (p.h.session != g_peerSession) {
                    g_peerSession = p.h.session;
                    g_expectedSeq = p.h.seq;
                    g_lastReceivedSeq = p.h.seq ? p.h.seq - 1 : 0;
                    g_receivedBits = 0;
                    N().early.clear();
                    N().rxAccum.clear();
                }
                ProcessAck(p.h.ack, p.h.ackBits);
                if (p.h.type == 1) DeliverInOrder(p);
            }
        }
    }
}

static bool IsLoopback(const in_addr& a)
{
    return (ntohl(a.s_addr) >> 24) == 127;
}

// True if no socket holds `port` on any local address. On Linux a wildcard
// bind without SO_REUSEADDR already conflicts with every existing binding of
// the port, loopback-only ones included: the guarantee SO_EXCLUSIVEADDRUSE buys
// on Windows.
static bool PortFree(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    close(s);
    return ok;
}

bool Net_PortAvailable(uint16_t port)
{
    return PortFree(port);
}

bool Net_Init(uint16_t localPort, const char* peerIp, uint16_t peerPort,
              void (*deliverCb)(const char*))
{
    in_addr peerAddr{};
    if (!peerIp || inet_pton(AF_INET, peerIp, &peerAddr) != 1)
        inet_pton(AF_INET, "127.0.0.1", &peerAddr);
    const bool loopback = IsLoopback(peerAddr);
    if (!PortFree(localPort)) return false;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    local.sin_port = htons(localPort);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0) {
        close(sock);
        return false;
    }
    {
        sockaddr_in bound{}; socklen_t boundLen = sizeof(bound);
        if (getsockname(sock, (sockaddr*)&bound, &boundLen) == 0)
            g_localPort = ntohs(bound.sin_port);
        else
            g_localPort = localPort;
    }

    g_loopbackOnly = loopback;
    {
        std::lock_guard<std::mutex> lk(N().peerMtx);
        N().peer = sockaddr_in{};
        N().peer.sin_family = AF_INET;
        N().peer.sin_port = htons(peerPort);
        N().peer.sin_addr = peerAddr;
    }
    g_deliver = deliverCb;
    g_session = (uint32_t)(NowMs() ^ (uintptr_t)&g_session);
    g_sock = sock;
    g_running = true;
    // A thread from an earlier Init that was never shut down: let it go rather
    // than assign over a joinable std::thread, which would terminate the game.
    if (N().thread.joinable()) N().thread.detach();
    N().thread = std::thread(NetThread);
    return true;
}

void Net_QueueLine(const char* line)
{
    size_t len = strlen(line);
    size_t chunks = (len / (NET_CHUNK_TEXT - 1)) + 1;
    std::lock_guard<std::mutex> lk(N().mtx);
    if (chunks > 0xFFFF) { g_droppedOversize++; return; }
    if (!PeerAlive()) { g_droppedNoPeer++; return; }
    for (size_t i = 0; i < chunks; ++i) {
        NetEvent ev{};
        ev.type = 1;
        ev.chunkIdx = (uint16_t)i;
        ev.chunkCount = (uint16_t)chunks;
        size_t off = i * (NET_CHUNK_TEXT - 1);
        size_t n = len - off;
        if (n > NET_CHUNK_TEXT - 1) n = NET_CHUNK_TEXT - 1;
        memcpy(ev.text, line + off, n);
        ev.text[n] = 0;
        N().outQueue.push(ev);
    }
}

bool Net_SetPeer(const char* ip, int port)
{
    if (!ip || port <= 0 || port > 0xFFFF) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return false;
    if (g_loopbackOnly && !IsLoopback(a.sin_addr)) return false;
    {
        std::lock_guard<std::mutex> lk(N().peerMtx);
        N().peer = a;
    }
    {
        std::lock_guard<std::mutex> lk(N().mtx);
        N().pending.clear();
        N().lastSent.clear();
        g_peerEverSeen = false;
    }
    return true;
}

uint16_t Net_LocalPort()
{
    return g_localPort;
}

uint64_t Net_DroppedStrangers()
{
    std::lock_guard<std::mutex> lk(N().mtx);
    return g_droppedStrangers;
}

// Never blocks: safe from a library destructor. shutdown() wakes a select()
// in progress; the thread then sees g_running false and returns.
void Net_SignalShutdown()
{
    g_running = false;
    int s = g_sock.exchange(-1);
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
}

void Net_Shutdown()
{
    Net_SignalShutdown();
    if (N().thread.joinable()) N().thread.join();
    g_deliver = nullptr;
    g_localPort = 0;
    std::lock_guard<std::mutex> lk(N().mtx);
    N().pending.clear();
    N().lastSent.clear();
    N().early.clear();
    N().rxAccum.clear();
    g_peerEverSeen = false;
}
