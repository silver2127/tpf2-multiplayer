#include "net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <string>

#pragma comment(lib, "ws2_32.lib")

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

static SOCKET g_sock = INVALID_SOCKET;
// g_peer has its own lock: SendRaw runs both with and without g_mtx held
// (g_mtx is not recursive), and Net_SetPeer may swap the address at any time.
static sockaddr_in g_peer{};
static std::mutex g_peerMtx;
static bool g_peerSet = false;
static uint16_t g_localPort = 0;   // set once in Net_Init, after a successful bind
static void (*g_deliver)(const char*) = nullptr;
static std::string g_rxAccum;   // partial line being reassembled from chunks

static HANDLE g_thread = nullptr;
static volatile bool g_running = false;

static uint32_t g_nextSeq = 0;
static uint32_t g_expectedSeq = 0;
static uint32_t g_session = 0;          // our id (set at init)
static uint32_t g_peerSession = 0;      // last seen peer id
static std::map<uint32_t, Packet> g_pending;        // sent, awaiting ack
static std::map<uint32_t, uint64_t> g_lastSent;     // seq -> last send time
static std::map<uint32_t, Packet> g_early;          // received out of order
static std::queue<NetEvent> g_outQueue;
static std::mutex g_mtx;
static uint32_t g_lastReceivedSeq = 0;
static uint32_t g_receivedBits = 0;

// --- peer liveness -----------------------------------------------------------
// Without this, a host with no joiner queued every event forever: g_pending grew
// without bound, each entry was resent every 250 ms, and the moment a peer
// appeared it received the entire backlog at once. Events from before a joiner
// existed are meaningless to it anyway -- it starts from a transferred save --
// so the right behaviour is to drop them.
static const uint32_t PEER_TIMEOUT_MS   = 10000;
static const uint32_t KEEPALIVE_MS      = 500;
static const size_t   MAX_PENDING       = 512;
static uint64_t g_lastRecvMs   = 0;
static bool     g_peerEverSeen = false;
static uint64_t g_droppedNoPeer = 0;
static uint64_t g_droppedOverflow = 0;

static bool PeerAlive()
{
    return g_peerEverSeen && (GetTickCount64() - g_lastRecvMs) < PEER_TIMEOUT_MS;
}

void Net_Stats(uint64_t* droppedNoPeer, uint64_t* droppedOverflow,
               size_t* pending, bool* peerAlive)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (droppedNoPeer)   *droppedNoPeer = g_droppedNoPeer;
    if (droppedOverflow) *droppedOverflow = g_droppedOverflow;
    if (pending)         *pending = g_pending.size();
    if (peerAlive)       *peerAlive = PeerAlive();
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
        std::lock_guard<std::mutex> lk(g_peerMtx);
        peer = g_peer;
    }
    sendto(g_sock, (const char*)&p, ev ? sizeof(p) : sizeof(Header), 0,
           (sockaddr*)&peer, sizeof(peer));
}

static void ProcessAck(uint32_t ack, uint32_t bits)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        uint32_t s = it->first;
        bool acked = (s == ack) || (s < ack && ((ack - s) > 32)) ||
                     (s < ack && (bits & (1u << (ack - s - 1))));
        if (acked) { g_lastSent.erase(s); it = g_pending.erase(it); }
        else { ++it; }
    }
}

// accumulate chunks; hand the callback a whole line once the last one lands.
// stream is reliable+ordered, so chunks arrive contiguously per line.
static void Reassemble(const NetEvent& ev)
{
    if (!g_deliver) return;
    size_t n = strnlen(ev.text, NET_CHUNK_TEXT);
    if (ev.chunkIdx == 0) g_rxAccum.clear();
    g_rxAccum.append(ev.text, n);
    if (ev.chunkIdx + 1 >= ev.chunkCount) {
        g_deliver(g_rxAccum.c_str());
        g_rxAccum.clear();
    }
}

static void DeliverInOrder(const Packet& p)
{
    uint32_t s = p.h.seq;
    if (s < g_expectedSeq) return;              // duplicate
    if (s > g_expectedSeq) {                    // early: stash
        if (s - g_expectedSeq < 64) g_early[s] = p;
        return;
    }
    if (p.h.type == 1) Reassemble(p.ev);
    g_lastReceivedSeq = s;
    g_receivedBits = (g_receivedBits << 1) | 1;
    g_expectedSeq++;
    for (;;) {
        auto it = g_early.find(g_expectedSeq);
        if (it == g_early.end()) break;
        if (it->second.h.type == 1) Reassemble(it->second.ev);
        g_lastReceivedSeq = g_expectedSeq;
        g_receivedBits = (g_receivedBits << 1) | 1;
        g_early.erase(it);
        g_expectedSeq++;
    }
}

static DWORD WINAPI NetThread(LPVOID)
{
    timeval tv{ 0, 20000 };  // 20ms recv slices
    while (g_running) {
        // 1. flush outbound queue
        for (;;) {
            NetEvent ev;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (g_outQueue.empty()) break;
                ev = g_outQueue.front();
                g_outQueue.pop();
            }
            uint32_t seq = g_nextSeq++;
            Packet p{};
            p.h.magic = MAGIC; p.h.seq = seq; p.h.type = 1; p.ev = ev;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_pending[seq] = p;
                g_lastSent[seq] = GetTickCount64();
            }
            SendRaw(seq, 1, &ev);
        }
        // 2. resend unacked (sent-time tracked alongside)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            // If the peer stops acking, pending grows without limit. We cannot
            // drop individual entries -- delivery is strictly ordered, so a hole
            // would stall the receiver forever -- so treat this as "peer gone"
            // and discard the whole backlog.
            if (g_pending.size() > MAX_PENDING) {
                g_droppedOverflow += g_pending.size();
                g_pending.clear();
                g_lastSent.clear();
                g_peerEverSeen = false;   // force rediscovery via keepalives
            }
            for (auto& kv : g_pending) {
                uint64_t& last = g_lastSent[kv.first];
                if ((int)(GetTickCount64() - last) > RESEND_MS) {
                    last = GetTickCount64();
                    SendRaw(kv.first, 1, &kv.second.ev);
                }
            }
        }

        // 2b. keepalive: type 0 carries acks and proves we exist. Both sides
        // send them, which is what lets each discover the other before either
        // has any data to send.
        {
            static uint64_t lastKeepalive = 0;
            uint64_t now = GetTickCount64();
            if (now - lastKeepalive >= KEEPALIVE_MS) {
                lastKeepalive = now;
                std::lock_guard<std::mutex> lk(g_mtx);
                SendRaw(g_nextSeq, 0, nullptr);
            }
        }
        // 3. receive
        fd_set fds; FD_ZERO(&fds); FD_SET(g_sock, &fds);
        int n = select(0, &fds, nullptr, nullptr, &tv);
        if (n > 0 && FD_ISSET(g_sock, &fds)) {
            Packet p{};
            sockaddr_in from{}; int fromLen = sizeof(from);
            int got = recvfrom(g_sock, (char*)&p, sizeof(p), 0,
                               (sockaddr*)&from, &fromLen);
            if (got >= (int)sizeof(Header) && p.h.magic == MAGIC) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_lastRecvMs = GetTickCount64();
                    g_peerEverSeen = true;
                }
                if (p.h.session != g_peerSession) {
                    // new sender session: reset receiver state so a peer that
                    // restarted (seq back to 0) isn't dropped as duplicates
                    g_peerSession = p.h.session;
                    g_expectedSeq = p.h.seq;
                    g_lastReceivedSeq = p.h.seq ? p.h.seq - 1 : 0;
                    g_receivedBits = 0;
                    g_early.clear();
                    g_rxAccum.clear();   // drop any half-reassembled line
                }
                ProcessAck(p.h.ack, p.h.ackBits);
                if (p.h.type == 1) DeliverInOrder(p);
            }
        }
    }
    return 0;
}

static bool g_wsaUp = false;

static bool EnsureWsa()
{
    if (g_wsaUp) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_wsaUp = true;
    return true;
}

bool Net_PortAvailable(uint16_t port)
{
    if (!EnsureWsa()) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

bool Net_Init(uint16_t localPort, const char* peerIp, uint16_t peerPort,
              void (*deliverCb)(const char*))
{
    if (!EnsureWsa()) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(localPort);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
        // leave nothing behind, so a caller can retry on another port
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    {
        // ask the socket rather than trusting the argument: this is what the
        // lobby has to send to, so it must be the port that really got bound
        sockaddr_in bound{}; int boundLen = sizeof(bound);
        if (getsockname(g_sock, (sockaddr*)&bound, &boundLen) == 0)
            g_localPort = ntohs(bound.sin_port);
        else
            g_localPort = localPort;
    }

    {
        std::lock_guard<std::mutex> lk(g_peerMtx);
        g_peer = sockaddr_in{};
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons(peerPort);
        inet_pton(AF_INET, peerIp, &g_peer.sin_addr);
        g_peerSet = true;
    }
    g_deliver = deliverCb;
    g_session = (uint32_t)(GetTickCount64() ^ (uintptr_t)&g_session);
    g_running = true;
    g_thread = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

// split a line of any length into chunk events. Queued under one lock so a
// line's chunks stay contiguous in the stream even with concurrent callers.
void Net_QueueLine(const char* line)
{
    size_t len = strlen(line);
    size_t chunks = (len / (NET_CHUNK_TEXT - 1)) + 1;   // >=1, even for ""
    if (chunks > 0xFFFF) chunks = 0xFFFF;               // absurd line: clamp
    std::lock_guard<std::mutex> lk(g_mtx);
    // Nobody is listening: drop rather than queue forever. A joiner that
    // connects later starts from a transferred save, so a replay of everything
    // that happened before it existed would be wrong as well as expensive.
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
        g_outQueue.push(ev);
    }
}

bool Net_SetPeer(const char* ip, int port)
{
    if (!ip || port <= 0 || port > 0xFFFF) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return false;
    {
        std::lock_guard<std::mutex> lk(g_peerMtx);
        g_peer = a;
        g_peerSet = true;
    }
    {
        // Same reasoning as the overflow path in NetThread: the backlog was
        // addressed to the old peer, and a hole is not allowed in the ordered
        // stream, so the whole thing goes. Keepalives rediscover the new peer.
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pending.clear();
        g_lastSent.clear();
        g_peerEverSeen = false;
    }
    return true;
}

uint16_t Net_LocalPort()
{
    return g_localPort;
}

void Net_Shutdown()
{
    g_running = false;
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); }
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
}
