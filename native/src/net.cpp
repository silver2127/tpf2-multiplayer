#include "net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>

#pragma comment(lib, "ws2_32.lib")

// protocol 4: process + world-scoped ACKs. Protocol 5 (2026-09-15): an event
// datagram is only as long as its text ("FPT5"); every one used to be a full
// 1,086 bytes. A protocol-4 bridge would drop the short events by size, so the
// magic moves with it and a stale DLL fails loudly instead.
static const uint32_t MAGIC = 0x35545046;
static const int RESEND_MS = 250;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    char world[32];     // exact operation epoch, checked BEFORE liveness or ACKs
    uint32_t session;   // sender instance id, selects its independent stream
    uint32_t ackSession; // an ACK is valid only for this sender epoch
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
static volatile bool g_loopbackOnly = true;
static uint64_t g_droppedStrangers = 0;
static uint16_t g_localPort = 0;   // set once in Net_Init, after a successful bind
static void (*g_deliver)(const char*) = nullptr;

static HANDLE g_thread = nullptr;
static volatile bool g_running = false;

static uint32_t g_nextSeq = 0;
static uint32_t g_session = 0;          // our id (set at init)
static std::map<uint32_t, Packet> g_pending;        // sent, awaiting ack
static std::map<uint32_t, uint64_t> g_lastSent;     // seq -> last send time
static std::queue<NetEvent> g_outQueue;
static std::mutex g_mtx;
static std::mutex g_epochMtx; // outer lock: net iteration vs coordinated reset
static std::string g_worldEpoch(32, '0');
static std::string g_lobbyEpoch;
// Zero is a real data sequence, not "nothing received". An initial keepalive
// must not acknowledge and discard packet 0 while it is still in flight.
static const uint32_t NO_ACK = UINT32_MAX;
// Relay frames retain the original process session. Each sender has an
// independent sequence space, ACK bitmap and partially assembled line.
struct PeerStream {
    bool sequenceReady = false; // initial join waits for a sender-advertised floor
    bool assembling = false;
    uint32_t expectedSeq = 0;
    uint32_t lastReceivedSeq = NO_ACK;
    uint32_t receivedBits = 0;
    uint64_t lastRecvMs = 0;    // its last admitted packet; silence this long evicts it
    std::map<uint32_t, Packet> early;
    std::string rxAccum;
};
static std::map<uint32_t, PeerStream> g_streams; // protected by g_epochMtx
// A broadcast remains pending until EVERY recipient present at send time ACKs.
// A recipient leaves that set only when it leaves the cohort (see the eviction
// pass in NetThread): silence for the peer timeout, i.e. not even a keepalive.
static std::map<uint32_t, std::set<uint32_t>> g_awaiting; // under g_mtx

// --- peer liveness -----------------------------------------------------------
// Without this, a host with no joiner queued every event forever: g_pending grew
// without bound, each entry was resent every 250 ms, and the moment a peer
// appeared it received the entire backlog at once. Events from before a joiner
// existed are meaningless to it anyway -- it starts from a transferred save --
// so the right behaviour is to drop them.
//
// The same silence takes a member OUT of the cohort. Every broadcast waits for
// every member it was sent to, and the send window (32 packets) then holds
// everything behind the oldest unacknowledged one. A member whose process died
// -- or a joiner's OLD process after its game restarted -- therefore stalled
// the host for every other player, forever: "pending=33" in the field. A
// variable, not a constant, so the transport tests can shorten it.
static uint32_t g_peerTimeoutMs         = 10000;
static const uint32_t KEEPALIVE_MS      = 500;
static uint64_t g_lastRecvMs   = 0;
static bool     g_peerEverSeen = false;
static uint64_t g_droppedNoPeer = 0;
static uint64_t g_droppedOverflow = 0;
static uint64_t g_droppedOversize = 0;
static uint64_t g_droppedWorld = 0;     // datagrams from another world epoch

// The bridge's log, when it gives us one. Every drop used to be silent, and a
// joiner that could never hear the host showed nothing but "peer=DOWN".
static void (*g_logger)(const char*) = nullptr;
static void NetLog(const char* fmt, ...)
{
    if (!g_logger) return;
    char buf[400];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_logger(buf);
}
void Net_SetLogger(void (*log)(const char* line)) { g_logger = log; }

// A sender in another world is logged once per world it claims, not per
// datagram (a stuck member resends every 250 ms). Under g_epochMtx.
static std::map<uint32_t, std::string> g_worldNoted;

static bool PeerAlive()
{
    return g_peerEverSeen && (GetTickCount64() - g_lastRecvMs) < g_peerTimeoutMs;
}

void Net_Stats(uint64_t* droppedNoPeer, uint64_t* droppedOverflow,
               size_t* pending, bool* peerAlive, uint64_t* droppedOversize,
               size_t* members, uint64_t* droppedWorld)
{
    std::lock_guard<std::mutex> epochLock(g_epochMtx);   // g_streams; outer lock first
    std::lock_guard<std::mutex> lk(g_mtx);
    if (droppedNoPeer)   *droppedNoPeer = g_droppedNoPeer;
    if (droppedOverflow) *droppedOverflow = g_droppedOverflow;
    if (pending)         *pending = g_pending.size();
    if (peerAlive)       *peerAlive = PeerAlive();
    if (droppedOversize) *droppedOversize = g_droppedOversize;
    if (members)         *members = g_streams.size();
    if (droppedWorld)    *droppedWorld = g_droppedWorld;
}

static void SendRaw(uint32_t seq, uint32_t type, const NetEvent* ev,
                    uint32_t ackSession = 0, const PeerStream* stream = nullptr)
{
    Packet p{};
    p.h.magic = MAGIC;
    memcpy(p.h.world,g_worldEpoch.data(),32);
    p.h.session = g_session;
    p.h.ackSession = ackSession;
    p.h.seq = seq;
    p.h.ack = stream ? stream->lastReceivedSeq : NO_ACK;
    p.h.ackBits = stream ? stream->receivedBits : 0;
    p.h.type = (uint8_t)type;
    if (ev) p.ev = *ev;
    sockaddr_in peer;
    {
        std::lock_guard<std::mutex> lk(g_peerMtx);
        peer = g_peer;
    }
    // A keepalive is the header alone. An event is its chunk header and its text
    // up to and including the NUL, not the whole 1,024-byte buffer: a heartbeat
    // line is ~200 bytes on the wire, not 1,086. A text with no NUL (never from
    // Net_QueueLine) goes out whole, and the receiver refuses it.
    size_t size = sizeof(Header);
    if (ev) {
        size_t n = strnlen(ev->text, NET_CHUNK_TEXT);
        size += offsetof(NetEvent, text) + (n < NET_CHUNK_TEXT ? n + 1 : NET_CHUNK_TEXT);
    }
    sendto(g_sock, (const char*)&p, (int)size, 0, (sockaddr*)&peer, sizeof(peer));
}

static void ProcessAck(uint32_t sender, uint32_t ack, uint32_t bits)
{
    if (ack == NO_ACK) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        uint32_t s = it->first;
        bool acked = (s == ack) ||
                     (s < ack && ack - s <= 32 && (bits & (1u << (ack - s - 1))));
        // find, never operator[]: a pending packet without an awaiting set
        // must not be released by whoever happens to ACK something else.
        auto waiting = g_awaiting.find(s);
        if (acked && waiting != g_awaiting.end()) waiting->second.erase(sender);
        if (waiting != g_awaiting.end() && waiting->second.empty()) {
            g_awaiting.erase(waiting);
            g_lastSent.erase(s);
            it = g_pending.erase(it);
        } else { ++it; }
    }
}

// accumulate chunks; hand the callback a whole line once the last one lands.
// stream is reliable+ordered, so chunks arrive contiguously per line.
static void Reassemble(PeerStream& stream, const NetEvent& ev)
{
    if (!g_deliver) return;
    size_t n = strnlen(ev.text, NET_CHUNK_TEXT);
    if (ev.chunkIdx == 0) { stream.rxAccum.clear(); stream.assembling = true; }
    // Joining may start inside an old fragmented line; never deliver its suffix.
    if (!stream.assembling) return;
    stream.rxAccum.append(ev.text, n);
    if (ev.chunkIdx + 1 >= ev.chunkCount) {
        g_deliver(stream.rxAccum.c_str());
        stream.rxAccum.clear();
        stream.assembling = false;
    }
}

// How far past the next expected seq an out-of-order packet may be stashed.
//
// Bound reordering to the selective-ACK window. Missing packets beyond
// that bitmap are never implicitly acknowledged; sends also retain a window
// behind the oldest outstanding sequence.
static const uint32_t EARLY_WINDOW = 32;

// Record that `s` arrived, in a bitmap the sender can actually read.
//
// This used to be `stream.receivedBits = (stream.receivedBits << 1) | 1` on each in-order
// delivery -- i.e. permanently all-ones -- and it was not touched at all for a
// packet stashed in stream.early. An out-of-order packet was therefore never
// acknowledged, so it stayed in the sender's g_pending and the WHOLE stash was
// retransmitted every RESEND_MS until the hole finally filled.
//
// Bit k of ackBits means "seq (ack - k - 1) was received", which is the layout
// ProcessAck has always decoded.
static void NoteReceived(PeerStream& stream, uint32_t s)
{
    if (stream.lastReceivedSeq == NO_ACK) {
        stream.lastReceivedSeq = s;
        stream.receivedBits = 0;
        return;
    }
    if (s == stream.lastReceivedSeq) return;
    if (s > stream.lastReceivedSeq) {
        uint32_t shift = s - stream.lastReceivedSeq;
        // the old high-water mark moves down to bit (shift-1); anything that
        // falls past bit 31 leaves the window and is simply no longer reported
        if (shift > 32)       stream.receivedBits = 0;
        else if (shift == 32) stream.receivedBits = 1u << 31;   // 1u<<32 is UB
        else                  stream.receivedBits = (stream.receivedBits << shift) | (1u << (shift - 1));
        stream.lastReceivedSeq = s;
    } else {
        uint32_t back = stream.lastReceivedSeq - s;             // >= 1
        if (back <= 32) stream.receivedBits |= (1u << (back - 1));
    }
}

// deliver whatever the stash holds from expectedSeq onwards
static void DrainEarly(PeerStream& stream)
{
    for (;;) {
        auto it = stream.early.find(stream.expectedSeq);
        if (it == stream.early.end()) break;
        if (it->second.h.type == 1) Reassemble(stream, it->second.ev);
        NoteReceived(stream, stream.expectedSeq);            // already acked when stashed; idempotent
        stream.early.erase(it);
        stream.expectedSeq++;
    }
}

static void DeliverInOrder(PeerStream& stream, const Packet& p)
{
    uint32_t s = p.h.seq;
    if (s < stream.expectedSeq) {                    // duplicate
        NoteReceived(stream, s);                        // our ack was lost; say so again
        return;
    }
    if (s > stream.expectedSeq) {                    // early: stash
        // Only note what we actually KEEP. Acking a packet we then discarded
        // would tell the sender to stop resending something we never had.
        if (s - stream.expectedSeq < EARLY_WINDOW) {
            stream.early[s] = p;
            NoteReceived(stream, s);
        }
        return;
    }
    if (p.h.type == 1) Reassemble(stream, p.ev);
    NoteReceived(stream, s);
    stream.expectedSeq++;
    DrainEarly(stream);
}

// The sender's oldest retained packet (every keepalive advertises it) is past
// the one we are waiting for. It only drops a packet its whole cohort has
// acknowledged, so the holes below the floor went by while we were not in
// that cohort: evicted for silence, or admitted with a floor the others had
// already cleared. Waiting would be forever -- the sender has nothing to
// resend -- and behind a stuck stream nothing from this sender ever reaches
// Lua again. Skip to the floor.
//
// What we STASHED below the floor is delivered first, in order: we
// acknowledged those packets and the sender erased them on the strength of
// that ACK, so nobody will ever send them again (found in review 2026-09-16:
// erasing the stash lost every line sent between an admission and the first
// packet the newcomer was awaited for). A line cut by a hole is abandoned,
// never delivered short. The Lua layer recovers a command gap (NACK + resend).
static void SkipToFloor(PeerStream& stream, uint32_t floor, uint32_t session)
{
    uint32_t lost = 0, delivered = 0, next = stream.expectedSeq;
    stream.assembling = false;                  // expectedSeq itself is a hole
    stream.rxAccum.clear();
    for (auto it = stream.early.begin(); it != stream.early.end() && it->first < floor;) {
        if (it->first != next) {                // a hole before this one
            stream.assembling = false; stream.rxAccum.clear();
            lost += it->first - next;
        }
        if (it->second.h.type == 1) Reassemble(stream, it->second.ev);
        ++delivered;
        next = it->first + 1;
        it = stream.early.erase(it);
    }
    if (next != floor) {                        // a hole up to the floor
        stream.assembling = false; stream.rxAccum.clear();
        lost += floor - next;
    }
    NetLog("[net] session %08x retains nothing before seq %u and we waited for %u: "
           "%u stashed packet(s) delivered, %u went by while we were out of its cohort -- skipping ahead\n",
           session, floor, stream.expectedSeq, delivered, lost);
    stream.expectedSeq = floor;
    DrainEarly(stream);
}

static DWORD WINAPI NetThread(LPVOID)
{
    timeval tv{ 0, 20000 };  // 20ms recv slices
    while (g_running) {
        // Net_SignalShutdown closes the socket and publishes INVALID_SOCKET to
        // break us out of select(); re-reading it here keeps FD_SET off a
        // handle that has already been closed.
        SOCKET sock = g_sock;
        if (sock == INVALID_SOCKET) break;
        {
        std::lock_guard<std::mutex> epochLock(g_epochMtx);
        // 1. flush outbound queue
        for (;;) {
            NetEvent ev;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (g_outQueue.empty()) break;
                // Nobody to send to: a packet with an empty awaiting set could
                // never be released and would hold the window for good.
                if (g_streams.empty()) {
                    g_droppedNoPeer += g_outQueue.size();
                    while (!g_outQueue.empty()) g_outQueue.pop();
                    break;
                }
                // Selective acknowledgements cover only 32 earlier packets.
                // Never advance beyond an unacknowledged gap's visibility.
                if (!g_pending.empty() && g_nextSeq - g_pending.begin()->first > 32) break;
                ev = g_outQueue.front();
                g_outQueue.pop();
            uint32_t seq = g_nextSeq++;
            Packet p{};
            p.h.magic = MAGIC; p.h.seq = seq; p.h.type = 1; p.ev = ev;
                g_pending[seq] = p;
                for (const auto& peer : g_streams) g_awaiting[seq].insert(peer.first);
                g_lastSent[seq] = GetTickCount64();
            SendRaw(seq, 1, &ev);
            }
        }
        // 2. resend unacked (sent-time tracked alongside)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            // Outstanding packets survive timeouts; the flush window above
            // bounds retransmissions without making holes in the stream.
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
                if (g_streams.empty()) SendRaw(g_pending.empty() ? g_nextSeq : g_pending.begin()->first, 0, nullptr);
                for (const auto& peer : g_streams)
                    SendRaw(g_pending.empty() ? g_nextSeq : g_pending.begin()->first, 0, nullptr, peer.first, &peer.second);
            }
        }

        // 2c. eviction: a member that has sent nothing for the peer timeout --
        // not even the keepalive it owes every 500 ms -- is out of the cohort.
        // Its process is gone (a crash, or a game restart that came back under
        // a new session id) or its link is. Every broadcast that still waits
        // for it is released to the members that did acknowledge; if it speaks
        // again it is admitted afresh and told the current floor.
        {
            static uint64_t lastPass = 0;
            uint64_t now = GetTickCount64();
            std::lock_guard<std::mutex> lk(g_mtx);
            // Silence this thread did not observe is not the members' silence:
            // after standby, or a stall under a world reset, their keepalives
            // are sitting in the socket buffer. Restart every clock instead of
            // evicting the whole cohort on the first pass back.
            if (lastPass && now - lastPass > g_peerTimeoutMs / 2) {
                for (auto& s : g_streams) s.second.lastRecvMs = now;
                NetLog("[net] net thread was away for %llu ms -- members' silence clocks restarted\n",
                       (unsigned long long)(now - lastPass));
            }
            lastPass = now;
            bool evicted = false;
            size_t releasedAll = 0;
            for (auto it = g_streams.begin(); it != g_streams.end();) {
                if (now - it->second.lastRecvMs <= g_peerTimeoutMs) { ++it; continue; }
                const uint32_t gone = it->first;
                size_t released = 0;
                for (auto a = g_awaiting.begin(); a != g_awaiting.end();) {
                    a->second.erase(gone);
                    if (a->second.empty()) {
                        g_pending.erase(a->first); g_lastSent.erase(a->first);
                        a = g_awaiting.erase(a); ++released;
                    } else ++a;
                }
                it = g_streams.erase(it);
                evicted = true;
                releasedAll += released;
                NetLog("[net] session %08x silent for %u ms -- out of the cohort; %zu packet(s) "
                       "stop waiting for it, %zu member(s) remain\n",
                       gone, g_peerTimeoutMs, released, g_streams.size());
            }
            // The last one gone: back to "nobody is listening". Lines are
            // dropped at the queue again, and nothing is kept for whoever
            // comes next -- it starts from a transferred save, and a backlog
            // delivered in one burst is exactly what that rule exists against.
            if (evicted && g_streams.empty()) {
                const size_t dropped = releasedAll + g_pending.size() + g_outQueue.size();
                g_pending.clear(); g_lastSent.clear(); g_awaiting.clear();
                while (!g_outQueue.empty()) g_outQueue.pop();
                g_droppedNoPeer += dropped;
                g_peerEverSeen = false; g_lastRecvMs = 0;
                NetLog("[net] cohort empty -- %zu pending/queued line(s) dropped, nobody is listening\n", dropped);
            }
        }
        }
        // Never hold the reset lock across select: an idle receive loop must
        // not starve the control/tail threads trying to switch worlds.
        // 3. receive
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        int n = select(0, &fds, nullptr, nullptr, &tv);
        if (n > 0 && FD_ISSET(sock, &fds)) {
            Packet p{};
            sockaddr_in from{}; int fromLen = sizeof(from);
            int got = recvfrom(sock, (char*)&p, sizeof(p), 0,
                               (sockaddr*)&from, &fromLen);
            std::lock_guard<std::mutex> epochLock(g_epochMtx);
            in_addr peerAddr;
            { std::lock_guard<std::mutex> lk(g_peerMtx); peerAddr = g_peer.sin_addr; }
            if (got >= 0 && from.sin_addr.s_addr != peerAddr.s_addr) {
                std::lock_guard<std::mutex> lk(g_mtx);
                ++g_droppedStrangers;
                continue;
            }
            if (got >= (int)sizeof(Header) && p.h.magic == MAGIC) {
                // Old data and old ACK-only packets are equally inadmissible.
                if (memcmp(p.h.world, g_worldEpoch.data(), 32) != 0) {
                    { std::lock_guard<std::mutex> lk(g_mtx); ++g_droppedWorld; }
                    const std::string theirs(p.h.world, 32);
                    auto noted = g_worldNoted.find(p.h.session);
                    if (noted == g_worldNoted.end() || noted->second != theirs) {
                        if (g_worldNoted.size() >= 64) g_worldNoted.clear();
                        g_worldNoted[p.h.session] = theirs;
                        NetLog("[net] dropped: session %08x is in world %.8s.., we are in %.8s.. -- "
                               "a member the lobby has not moved to this epoch yet, or a process "
                               "from an earlier one (logged once per world it claims)\n",
                               p.h.session, theirs.c_str(), g_worldEpoch.c_str());
                    }
                    continue;
                }
                if (p.h.session == 0 || (p.h.type != 0 && p.h.type != 1)) continue;
                // An event holds its chunk header and a text that ENDS inside the
                // datagram (p is zeroed, so nothing past `got` can supply the NUL).
                // A truncated one is dropped, never delivered as a shorter line; a
                // full-size one still reads.
                if (p.h.type == 1) {
                    const int head = (int)(sizeof(Header) + offsetof(NetEvent, text));
                    if (got <= head || !memchr(p.ev.text, 0, (size_t)(got - head))) continue;
                }
                if (p.h.session == g_session) continue;
                auto found = g_streams.find(p.h.session);
                if (found == g_streams.end()) {
                    // The world epoch is the credential. A process has it only
                    // from the lobby's own control path (lobby= at a join, epoch=
                    // from a sync operation it took part in), so whatever process
                    // id it now runs under, this sender is a member of THIS world.
                    // Until 2026-09-15 the roster froze at every world change and
                    // a member whose game had restarted -- a new random session --
                    // was refused for the rest of the lobby's life, both ways: its
                    // datagrams died here and the host's died on its side, while
                    // every broadcast waited for its dead session. It sat in the
                    // load gate with "0 peer(s) in" and the host read pending=33.
                    found = g_streams.emplace(p.h.session, PeerStream{}).first;
                    NetLog("[net] session %08x joins world %.8s.. -- %zu member(s) in the cohort\n",
                           p.h.session, g_worldEpoch.c_str(), g_streams.size());
                }
                found->second.lastRecvMs = GetTickCount64();
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_lastRecvMs = found->second.lastRecvMs;
                    g_peerEverSeen = true;
                }
                if (p.h.ackSession == g_session) ProcessAck(p.h.session, p.h.ack, p.h.ackBits);
                // Only an ACK/keepalive advertises the oldest retained packet.
                // A reordered data packet is not a safe initial sequence floor.
                // Do not ACK data before discovery: the sender must retry it.
                if (!found->second.sequenceReady) {
                    if (p.h.type != 0) continue;
                    found->second.expectedSeq = p.h.seq;
                    found->second.sequenceReady = true;
                } else if (p.h.type == 0 && p.h.seq > found->second.expectedSeq) {
                    SkipToFloor(found->second, p.h.seq, p.h.session);
                }
                if (p.h.type == 1) {
                    DeliverInOrder(found->second, p);
                    std::lock_guard<std::mutex> lk(g_mtx);
                    SendRaw(g_pending.empty() ? g_nextSeq : g_pending.begin()->first,
                            0, nullptr, found->first, &found->second);
                }
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

static bool IsLoopback(const in_addr& a)
{
    return (ntohl(a.s_addr) >> 24) == 127;
}

// True if no socket holds `port` on any local address. An exclusive bind on the
// wildcard address conflicts with every existing binding of the port, whatever
// address and options it used. The old probe, a plain wildcard bind, succeeds
// beside a socket bound to 127.0.0.1 alone -- and a loopback bind succeeds beside
// an older bridge's plain wildcard socket (both measured on Windows 11).
static bool PortFree(uint16_t port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

bool Net_PortAvailable(uint16_t port)
{
    if (!EnsureWsa()) return false;
    return PortFree(port);
}

bool Net_Init(uint16_t localPort, const char* peerIp, uint16_t peerPort,
              void (*deliverCb)(const char*))
{
    if (!EnsureWsa()) return false;
    // An unreadable peer_ip used to leave the peer at 0.0.0.0, which nothing
    // answers; this PC at least exists.
    in_addr peerAddr{};
    if (!peerIp || inet_pton(AF_INET, peerIp, &peerAddr) != 1)
        inet_pton(AF_INET, "127.0.0.1", &peerAddr);
    // The socket only listens where its peer is. Every lobby session (and two
    // games on one PC) has a loopback peer, and then nothing off this PC can
    // reach the socket at all. It used to take commands from any address.
    const bool loopback = IsLoopback(peerAddr);
    // Checked on every address first: a loopback bind succeeds beside an older
    // bridge's all-interfaces socket and would take that game's local traffic.
    if (!PortFree(localPort)) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(g_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
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
        g_peer.sin_addr = peerAddr;
        g_peerSet = true;
    }
    g_loopbackOnly = loopback;
    g_deliver = deliverCb;
    g_worldEpoch.assign(32, '0');
    g_lobbyEpoch.clear();
    g_nextSeq = 0;
    g_streams.clear();
    g_awaiting.clear();
    g_worldNoted.clear();
    g_session = (uint32_t)(GetTickCount64() ^ (uintptr_t)&g_session);
    if (g_session == 0) g_session = 1;
    g_running = true;
    g_thread = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

// split a line of any length into chunk events. Queued under one lock so a
// line's chunks stay contiguous in the stream even with concurrent callers.
void Net_QueueLine(const char* line, const char* expectedWorld)
{
    std::lock_guard<std::mutex> epochLock(g_epochMtx);
    // A tail read begun before reset must not enter the new world's queue.
    if(expectedWorld && g_worldEpoch!=expectedWorld) return;
    size_t len = strlen(line);
    size_t chunks = (len / (NET_CHUNK_TEXT - 1)) + 1;   // >=1, even for ""
    std::lock_guard<std::mutex> lk(g_mtx);
    // REFUSE, do not clamp.
    //
    // This used to be `if (chunks > 0xFFFF) chunks = 0xFFFF`, which cut the
    // line short but still shipped chunkCount = 0xFFFF -- so the receiver
    // reassembled the truncated text, saw the last chunk arrive, and handed it
    // to Lua as a complete line. A silently shortened command is far worse than
    // a missing one: it parses, and it replays as something the player never
    // did. The cap is ~67 MB of text, so hitting it means a bug upstream, not a
    // big station.
    if (chunks > 0xFFFF) { g_droppedOversize++; return; }
    // Nobody is listening: drop rather than queue forever. A joiner that
    // connects later starts from a transferred save, so a replay of everything
    // that happened before it existed would be wrong as well as expensive.
    if (!g_peerEverSeen) { g_droppedNoPeer++; return; }
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

std::string Net_WorldEpoch() {
    std::lock_guard<std::mutex> epochLock(g_epochMtx);
    return g_worldEpoch;
}
bool Net_SetWorldEpoch(const char* epoch,void (*resetLocal)(const char*)) {
    if(!epoch || strlen(epoch)!=32 || strspn(epoch,"0123456789abcdef")!=32 ||
       strspn(epoch,"0")==32) return false;
    std::lock_guard<std::mutex> epochLock(g_epochMtx);
    if(g_worldEpoch==epoch) return true;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_worldEpoch=epoch;
        g_nextSeq=0;
        const uint64_t now = GetTickCount64();
        for (auto& peer : g_streams) {
            peer.second = PeerStream{};
            // Coordinated resets really do start at zero, even if the first
            // keepalive arrives ahead of a missing/reordered packet zero.
            peer.second.sequenceReady = true;
            peer.second.lastRecvMs = now;   // the silence clock restarts with the world
        }
        g_pending.clear(); g_lastSent.clear(); g_awaiting.clear();
        while(!g_outQueue.empty()) g_outQueue.pop();
        g_lastRecvMs=0; g_peerEverSeen=false;
        g_worldNoted.clear();
        // The established members keep their streams (they start at zero
        // together). Anyone else who presents this epoch is admitted on
        // arrival: it can only have it from the same operation. A member that
        // never speaks in the new world leaves the cohort by the timeout.
    }
    NetLog("[net] world %.8s.. -- %zu member(s) carried over\n", epoch, g_streams.size());
    if(resetLocal) resetLocal(epoch);
    return true;
}

// A new lobby is a new recipient cohort, unlike a route change or resync.
// Its shared nonce also rejects delayed packets and ACKs from the old lobby.
bool Net_BeginLobby(const char* epoch, const char* ip, int port,
                    void (*resetLocal)(const char*)) {
    if(!epoch || strlen(epoch)!=32 || strspn(epoch,"0123456789abcdef")!=32 ||
       strspn(epoch,"0")==32) return false;
    std::lock_guard<std::mutex> epochLock(g_epochMtx);
    if(!Net_SetPeer(ip,port)) return false;
    if(g_lobbyEpoch==epoch) return true;
    // The lobby's nonce has caught up with the world we already run: after a
    // resync the host lobby advertises the resync epoch, so a player who joins
    // later starts in it instead of in a lobby nonce nobody else is in any more.
    // The members that did the resync are here already; keep every stream.
    if(g_worldEpoch==epoch) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_lobbyEpoch=epoch;
        NetLog("[net] lobby nonce now names our world %.8s.. -- nothing resets\n", epoch);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_lobbyEpoch=epoch;
        g_worldEpoch=epoch;
        if(++g_session==0) ++g_session;
        g_nextSeq=0;
        g_streams.clear(); g_awaiting.clear();
        g_pending.clear(); g_lastSent.clear();
        while(!g_outQueue.empty()) g_outQueue.pop();
        g_lastRecvMs=0; g_peerEverSeen=false;
        g_worldNoted.clear();
    }
    NetLog("[net] lobby %.8s.. -- new cohort, our session is now %08x\n", epoch, g_session);
    if(resetLocal) resetLocal(epoch);
    return true;
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
        std::lock_guard<std::mutex> lk(g_peerMtx);
        g_peer = a;
        g_peerSet = true;
    }
    // Preserve the reliable stream and pending sequences across route changes.
    // World changes use Net_SetWorldEpoch to reset all participant streams.
    return true;
}

uint16_t Net_LocalPort()
{
    return g_localPort;
}

// Loader-lock-safe half of Net_Shutdown: signal and return, never block.
//
// The blocking version was being called straight from DllMain(PROCESS_DETACH),
// where WaitForSingleObject waits on a thread whose own exit needs the loader
// lock we are holding for every other DLL's DLL_THREAD_DETACH -- the classic
// hang-on-exit. Closing the socket is what actually stops the thread: it makes
// the 20 ms select() return immediately instead of at the end of its slice.
void Net_SignalShutdown()
{
    g_running = false;
    SOCKET s = g_sock;
    if (s != INVALID_SOCKET) {
        // publish INVALID first so NetThread's next pass bails out rather than
        // calling select() on a handle we are about to close
        g_sock = INVALID_SOCKET;
        closesocket(s);
    }
}

void Net_Shutdown()
{
    Net_SignalShutdown();
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    // Reset BOTH, or a later Net_Init lies about succeeding: g_sock kept a
    // stale non-INVALID value, and g_wsaUp stayed true across WSACleanup, so
    // EnsureWsa() short-circuited and every socket call came back
    // WSANOTINITIALISED on a transport that reported itself up.
    g_sock = INVALID_SOCKET;
    if (g_wsaUp) { WSACleanup(); g_wsaUp = false; }
    g_deliver = nullptr;
    g_localPort = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pending.clear();
        g_lastSent.clear();
        g_streams.clear();
        g_awaiting.clear();
        g_worldNoted.clear();
        g_outQueue = std::queue<NetEvent>{};
        g_peerEverSeen = false;
    }
}

uint64_t Net_DroppedStrangers()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_droppedStrangers;
}
