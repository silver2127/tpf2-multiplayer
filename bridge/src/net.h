// Minimal reliable-ordered UDP transport (our own, no dependencies).
// v2: payload is a single text line (the event schema lives in Lua land).
// v3: lines longer than one datagram are split into chunks and reassembled
//     on the receiving side, so the callback always sees a whole line.
//     Delivery is already reliable+ordered, so reassembly is just accumulation.
#pragma once
#include <cstdint>
#include <cstddef>

#define NET_CHUNK_TEXT 1024

#pragma pack(push, 1)
struct NetEvent {
    uint8_t  type;        // 1 = EVENT (text chunk)
    uint16_t chunkIdx;    // 0-based index of this chunk
    uint16_t chunkCount;  // total chunks in this line (1 = not split)
    char     text[NET_CHUNK_TEXT];   // chunk payload, NUL-terminated
};
#pragma pack(pop)

// True if nothing currently holds this UDP port. Used to work out which
// instance we are when both games load the same proxy: first one up takes the
// host port. Inherently racy, but the two games are started seconds apart and
// the real bind afterwards still fails loudly if we lose.
bool Net_PortAvailable(uint16_t port);

// deliverCb is called (from the net thread) once per fully reassembled line.
bool Net_Init(uint16_t localPort, const char* peerIp, uint16_t peerPort,
              void (*deliverCb)(const char* line));
void Net_QueueLine(const char* line);      // thread-safe; chunks as needed
void Net_Shutdown();

// Repoint the peer address without touching the socket or the net thread
// (the lobby decides who we talk to after the game is already running).
// Thread-safe. Liveness is reset and the unacked backlog dropped: those
// packets were for the old peer and the new one starts from a transferred
// save anyway. Returns false (and changes nothing) if `ip` is not a dotted
// IPv4 address or the port is out of range.
bool Net_SetPeer(const char* ip, int port);

// The UDP port the socket actually bound (queried from the socket, so it is
// right even after a retry on another port). 0 until Net_Init has succeeded.
uint16_t Net_LocalPort();

// Diagnostics: lines discarded because no peer was alive, lines discarded
// because the peer stopped acking, packets awaiting ack, current liveness.
void Net_Stats(uint64_t* droppedNoPeer, uint64_t* droppedOverflow,
               size_t* pending, bool* peerAlive);
