// Minimal reliable-ordered UDP transport (our own, no dependencies).
// v2: payload is a single text line (the event schema lives in Lua land).
// v3: lines longer than one datagram are split into chunks and reassembled
//     on the receiving side, so the callback always sees a whole line.
//     Delivery is already reliable+ordered, so reassembly is just accumulation.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

#define NET_CHUNK_TEXT 1024

#pragma pack(push, 1)
struct NetEvent {
    uint8_t  type;        // 1 = EVENT (text chunk)
    uint16_t chunkIdx;    // 0-based index of this chunk
    uint16_t chunkCount;  // total chunks in this line (1 = not split)
    char     text[NET_CHUNK_TEXT];   // chunk payload, NUL-terminated
};
#pragma pack(pop)

// True if no socket holds this UDP port on any local address. Used to work out
// which instance we are when both games load the same proxy: first one up takes
// the host port. Inherently racy, but the two games are started seconds apart
// and the real bind afterwards still fails loudly if we lose.
bool Net_PortAvailable(uint16_t port);

// deliverCb is called (from the net thread) once per fully reassembled line.
//
// The socket only listens to its peer. With a loopback peer (every lobby
// session, and two games on one PC) it is bound to 127.0.0.1, so nothing off
// this PC can reach it; any other peer address binds all interfaces, for a
// direct link to that machine. Either way a datagram from any address but the
// peer's is dropped (Net_DroppedStrangers). The bind is exclusive, and refused
// if anything already holds the port on any address.
bool Net_Init(uint16_t localPort, const char* peerIp, uint16_t peerPort,
              void (*deliverCb)(const char* line));
// Thread-safe; chunks as needed. A line too long to describe with a 16-bit
// chunk count is REFUSED (counted in Net_Stats' droppedOversize), never
// truncated -- a shortened command still parses on the far side.
void Net_QueueLine(const char* line, const char* expectedWorld = nullptr);
std::string Net_WorldEpoch();
bool Net_SetWorldEpoch(const char* epoch, void (*resetLocal)(const char*) = nullptr);

// Orderly shutdown: stops the net thread, WAITS for it, closes the socket and
// releases Winsock, leaving the module reinitialisable by Net_Init.
//
// MUST NOT be called from DllMain. It waits on a thread whose exit needs the
// loader lock that DllMain already holds; use Net_SignalShutdown there.
void Net_Shutdown();

// Non-blocking: clears the run flag and closes the socket so the net thread
// falls out of select() and returns on its own. Safe under the loader lock.
// Does not join the thread and does not call WSACleanup.
void Net_SignalShutdown();

// Repoint the peer address without touching the socket or the net thread
// (the lobby decides who we talk to after the game is already running).
// Thread-safe. Route changes preserve the reliable stream; a coordinated
// world reset clears it. Returns false (and changes nothing) if `ip` is not a dotted
// IPv4 address, the port is out of range, or the socket is bound to 127.0.0.1
// and `ip` is not a loopback address.
bool Net_SetPeer(const char* ip, int port);
// Explicit new-lobby boundary: discard the old cohort, queues and world.
// Repeating the same nonce is idempotent, including after a resync.
bool Net_BeginLobby(const char* epoch, const char* ip, int port,
                    void (*resetLocal)(const char*) = nullptr);

// The UDP port the socket actually bound (queried from the socket, so it is
// right even after a retry on another port). 0 until Net_Init has succeeded.
uint16_t Net_LocalPort();

// Diagnostics: lines discarded because no peer was alive, lines discarded
// because the peer stopped acking, packets awaiting ack, current liveness,
// lines refused for exceeding the 16-bit chunk count, sessions in the cohort
// and datagrams dropped for carrying another world epoch. Every pointer is
// optional. droppedOversize is never expected to move; if it does, something
// upstream is generating a multi-megabyte line.
void Net_Stats(uint64_t* droppedNoPeer, uint64_t* droppedOverflow,
               size_t* pending, bool* peerAlive, uint64_t* droppedOversize,
               size_t* members = nullptr, uint64_t* droppedWorld = nullptr);

// Where the transport writes its own log lines (admissions, evictions, the
// drops that used to be silent). One line per call, newline included. Optional.
void Net_SetLogger(void (*log)(const char* line));

// Datagrams dropped because their source address was not the peer's.
uint64_t Net_DroppedStrangers();
