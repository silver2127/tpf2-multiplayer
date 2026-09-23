// steam_tunnel.h -- Steam's own networking as a transport for the lobby (2026-09-21).
//
// The game process is a Steam app with steam_api64.dll initialised, so the
// bridge can send P2P packets keyed by SteamID through ISteamNetworking (the
// "legacy" P2P API, v006): Valve does the NAT traversal and, when a direct
// path cannot be opened, relays through its own servers -- for any app, no
// partner allow-listing (unlike the newer SDR sockets). Nobody forwards a port,
// installs Hamachi or depends on the master server's relay.
//
// The lobby (netpunch, a separate process) keeps its UDP model unchanged: the
// tunnel presents every Steam peer as a loopback UDP endpoint, 127.0.0.1 on a
// reserved port range (62100-62199), which is how the lobby tells it apart.
// A datagram the lobby sends to that endpoint goes out as a P2P packet to that
// SteamID; a P2P packet from that SteamID arrives at the lobby's socket FROM
// that endpoint. Sealing, roster, save transfer and the NACK/resend all work
// as over any UDP path; the TCP side channels are skipped for endpoints.
//
// Control (text datagrams on the tunnel's control port, replies to the sender):
//   LOBBY <port>      where to deliver inbound packets (the lobby's game socket)
//   DIAL <steamid64>  make (or find) the endpoint for that user, send it an OPEN
//                     on channel 1 (which implicitly accepts its session too);
//                     reply "EP <steamid64> 127.0.0.1 <port>"
//   CLOSE <steamid64> drop the endpoint and the P2P session
//   STATUS            one line per endpoint
// The identity file <data dir>\tpf2_steam.txt carries "id=<steamid64>",
// "port=<control port>" and "name=<persona>" once the tunnel is up; the lobby
// reads it. <data dir>\tpf2mp_steam_off.txt keeps the tunnel off.
//
// Packets over 1,200 bytes (the legacy unreliable limit) go reliable; everything
// else unreliable, so the lobby's own reliability sees the same UDP semantics.
#pragma once
#include <string>

typedef void (*TunnelLogFn)(const char* fmt, ...);

// Starts the tunnel thread; returns false when the kill switch is set. The
// thread waits for Steam to come up on its own.
#ifdef _WIN32
bool SteamTunnel_Start(const std::wstring& dataDir, TunnelLogFn log);
#else
bool SteamTunnel_Start(const std::string& dataDir, TunnelLogFn log);
#endif
void SteamTunnel_Stop();
void SteamTunnel_SignalShutdown();
