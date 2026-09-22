// steam_tunnel.cpp -- Steam P2P as a lobby transport. See steam_tunnel.h.
//
// Everything Steam-side goes through the flat C API that the game's own
// steam_api64.dll exports (SteamAPI_ISteamNetworking_*), resolved with
// GetProcAddress: no SDK headers, no import library, no second SteamAPI_Init.
// The game initialised Steam long before this thread runs; the accessors
// return null until then, so the thread simply waits.
//
// Threads: this file's own thread owns every socket and the endpoint table.
// The two Steam callbacks (session request, connect failure) run on whichever
// thread the game runs SteamAPI_RunCallbacks on; they only call the accept
// function (the API is thread-safe) and push to a queue for the log.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <share.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cerrno>
#include <chrono>
#include <thread>
#endif
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "steam_tunnel.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

#ifdef _WIN32
using SocketLength = int;
#else
using SOCKET = int;
using SocketLength = socklen_t;
using DWORD = uint32_t;
constexpr SOCKET INVALID_SOCKET = -1;
static int closesocket(SOCKET s) { return close(s); }
static int WSAGetLastError() { return errno; }
static uint64_t GetTickCount64() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
static void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

typedef void*    (*FnAccessor)();
typedef uint64_t (*FnGetSteamID)(void* user);
typedef bool     (*FnSendP2P)(void* net, uint64_t id, const void* data, uint32_t len, int sendType, int channel);
typedef bool     (*FnAvailP2P)(void* net, uint32_t* size, int channel);
typedef bool     (*FnReadP2P)(void* net, void* dest, uint32_t cap, uint32_t* size, uint64_t* from, int channel);
typedef bool     (*FnIdP2P)(void* net, uint64_t id);
typedef bool     (*FnAllowRelay)(void* net, bool allow);
typedef bool     (*FnSessionState)(void* net, uint64_t id, void* state);
typedef void     (*FnRegisterCallback)(void* cb, int id);
typedef void     (*FnUnregisterCallback)(void* cb);
typedef const char* (*FnPersonaName)(void* friends);
typedef bool     (*FnSetConfig)(void* utils, int value, int scope, intptr_t obj, int dataType, const void* arg);
typedef int      (*FnGetConfig)(void* utils, int value, int scope, intptr_t obj, int* dataType, void* result, size_t* size);

struct Api {
    FnAccessor networking = nullptr, user = nullptr, friends = nullptr;
    FnGetSteamID getSteamId = nullptr;
    FnSendP2P send = nullptr;
    FnAvailP2P avail = nullptr;
    FnReadP2P read = nullptr;
    FnIdP2P accept = nullptr, closeSession = nullptr;
    FnAllowRelay allowRelay = nullptr;
    FnSessionState sessionState = nullptr;
    FnRegisterCallback registerCb = nullptr;
    FnUnregisterCallback unregisterCb = nullptr;
    FnPersonaName personaName = nullptr;
    FnAccessor utils = nullptr;
    FnSetConfig setConfig = nullptr;
    FnGetConfig getConfig = nullptr;
    void* net = nullptr;
};

// Steam callbacks use 4-byte packing on Linux, 8-byte packing on Windows.
#ifdef _WIN32
#pragma pack(push, 8)
#else
#pragma pack(push, 4)
#endif
struct P2PSessionState { uint8_t active, connecting, error, usingRelay; int32_t bytesQueued, packetsQueued; uint32_t remoteIp; uint16_t remotePort; };
struct P2PSessionRequest { uint64_t steamId; };
struct P2PSessionConnectFail { uint64_t steamId; uint8_t error; };
#pragma pack(pop)
static_assert(sizeof(P2PSessionState) == 20, "Steam session state ABI");
#ifndef _WIN32
static_assert(sizeof(P2PSessionConnectFail) == 12, "Linux Steam callback ABI");
#endif

const int CB_SESSION_REQUEST = 1202;   // k_iSteamNetworkingCallbacks + 2
const int CB_CONNECT_FAIL    = 1203;   // + 3
const int SEND_UNRELIABLE = 0, SEND_RELIABLE = 2;
const uint32_t UNRELIABLE_MAX = 1200;  // the legacy API's unreliable packet limit
const int CH_DATA = 0, CH_CTL = 1;
// Endpoints are 127.0.0.1 sockets on a reserved port range: that range is how the
// lobby tells a Steam peer from a real loopback one (steamtunnel.py). A loopback
// ALIAS (127.0.0.77) was the first design: Windows lets a socket bind to it and
// then silently drops every datagram it sends (measured 2026-09-21).
const char* TUNNEL_IP = "127.0.0.1";
const uint16_t TUNNEL_PORT_LO = 62100, TUNNEL_PORT_HI = 62199;
const DWORD EP_IDLE_MS = 180000;       // an endpoint nobody has used for 3 min closes its session

// CCallbackBase's layout: a vtable of Run(void*), Run(void*, bool, uint64),
// GetCallbackSizeBytes(), then uint8 flags and int id. SteamAPI_RegisterCallback
// fills the last two. Declared here so no SDK header is needed.
struct CallbackBase {
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int  GetCallbackSizeBytes() = 0;
    uint8_t flags = 0;
    int id = 0;
};

TunnelLogFn g_log = nullptr;
#ifdef _WIN32
using TunnelPath = std::wstring;
HANDLE g_thread = nullptr;
#else
using TunnelPath = std::string;
std::thread* g_thread = nullptr;
#endif
// Process-lifetime storage: shutdown can race the game's callback thread.
TunnelPath& g_dataDir = *new TunnelPath;
std::atomic<bool> g_stop{false};
Api g_api;
uint64_t g_myId = 0;
std::mutex& g_cbMtx = *new std::mutex;
auto& g_cbQueue = *new std::vector<std::pair<uint64_t, int>>; // (steamid, kind)

struct SessionRequestCb : CallbackBase {
    void Run(void* p) override {
        uint64_t id = ((P2PSessionRequest*)p)->steamId;
        if (g_api.accept && g_api.net) g_api.accept(g_api.net, id);   // thread-safe; anyone may talk, the seal decides
        std::lock_guard<std::mutex> lk(g_cbMtx);
        g_cbQueue.emplace_back(id, 0);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int GetCallbackSizeBytes() override { return (int)sizeof(P2PSessionRequest); }
};
struct ConnectFailCb : CallbackBase {
    void Run(void* p) override {
        auto* f = (P2PSessionConnectFail*)p;
        std::lock_guard<std::mutex> lk(g_cbMtx);
        g_cbQueue.emplace_back(f->steamId, 1 + (int)f->error);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int GetCallbackSizeBytes() override { return (int)sizeof(P2PSessionConnectFail); }
};
SessionRequestCb g_reqCb;
ConnectFailCb g_failCb;

struct Endpoint {
    SOCKET sock = INVALID_SOCKET;
    uint16_t port = 0;
    DWORD lastSeen = 0;
    uint64_t in = 0, out = 0, reliable = 0;
};

bool ResolveApi()
{
#ifdef _WIN32
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) return false;
    auto get = [&](const char* name) { return GetProcAddress(m, name); };
#else
    // The game owns loading and initializing Steam. Never load a second copy.
    auto get = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
#endif
    g_api.networking   = (FnAccessor)get("SteamAPI_SteamNetworking_v006");
    g_api.user         = (FnAccessor)get("SteamAPI_SteamUser_v021");
    g_api.friends      = (FnAccessor)get("SteamAPI_SteamFriends_v017");
    g_api.getSteamId   = (FnGetSteamID)get("SteamAPI_ISteamUser_GetSteamID");
    g_api.send         = (FnSendP2P)get("SteamAPI_ISteamNetworking_SendP2PPacket");
    g_api.avail        = (FnAvailP2P)get("SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    g_api.read         = (FnReadP2P)get("SteamAPI_ISteamNetworking_ReadP2PPacket");
    g_api.accept       = (FnIdP2P)get("SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    g_api.closeSession = (FnIdP2P)get("SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    g_api.allowRelay   = (FnAllowRelay)get("SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    g_api.sessionState = (FnSessionState)get("SteamAPI_ISteamNetworking_GetP2PSessionState");
    g_api.registerCb   = (FnRegisterCallback)get("SteamAPI_RegisterCallback");
    g_api.unregisterCb = (FnUnregisterCallback)get("SteamAPI_UnregisterCallback");
    g_api.personaName  = (FnPersonaName)get("SteamAPI_ISteamFriends_GetPersonaName");
    g_api.utils        = (FnAccessor)get("SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    g_api.setConfig    = (FnSetConfig)get("SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    g_api.getConfig    = (FnGetConfig)get("SteamAPI_ISteamNetworkingUtils_GetConfigValue");
    return g_api.networking && g_api.user && g_api.getSteamId && g_api.send && g_api.avail && g_api.read
        && g_api.accept && g_api.closeSession && g_api.allowRelay && g_api.registerCb;
}

// ip:port, or an ephemeral port when port is 0.
SOCKET BindLoopback(const char* ip, uint16_t port, uint16_t* portOut)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    SocketLength len = sizeof(a);
    getsockname(s, (sockaddr*)&a, &len);
    *portOut = ntohs(a.sin_port);
#ifdef _WIN32
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb)) { closesocket(s); return INVALID_SOCKET; }
#else
    if (s >= FD_SETSIZE || fcntl(s, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(s, F_SETFD, FD_CLOEXEC) < 0) { closesocket(s); return INVALID_SOCKET; }
#endif
    return s;
}

void WriteIdentity(uint64_t id, uint16_t port, const char* name)
{
#ifdef _WIN32
    std::wstring p = g_dataDir + L"tpf2_steam.txt";
    FILE* f = _wfsopen(p.c_str(), L"w", _SH_DENYNO);
#else
    FILE* f = fopen((g_dataDir + "tpf2_steam.txt").c_str(), "w");
#endif
    if (!f) return;
    if (id) fprintf(f, "id=%llu\nport=%u\nname=%s\n", (unsigned long long)id, (unsigned)port, name ? name : "");
    fclose(f);
}

bool KillSwitch()
{
#ifdef _WIN32
    return GetFileAttributesW((g_dataDir + L"tpf2mp_steam_off.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access((g_dataDir + "tpf2mp_steam_off.txt").c_str(), F_OK) == 0;
#endif
}

#ifdef _WIN32
DWORD WINAPI TunnelThread(LPVOID)
#else
unsigned TunnelThread(void*)
#endif
{
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // Steam comes up on the game's schedule: wait for it (2 minutes, then give up quietly)
    DWORD t0 = GetTickCount();
    void* user = nullptr;
    while (!g_stop) {
        if (ResolveApi()) {
            user = g_api.user();
            g_api.net = g_api.networking();
            if (user && g_api.net) {
                g_myId = g_api.getSteamId(user);
                if (g_myId) break;
            }
        }
        if (DWORD(GetTickCount() - t0) > 120000) {
            g_log("[steam] no Steam identity after 120 s -- the Steam transport stays off this session\n");
            return 0;
        }
        Sleep(1000);
    }
    if (g_stop) return 0;
    std::string persona;
    if (g_api.friends && g_api.personaName) { void* fr = g_api.friends(); const char* n = fr ? g_api.personaName(fr) : nullptr; if (n) persona = n; }
    for (auto& c : persona) if (c == '\n' || c == '\r') c = ' ';

    g_api.allowRelay(g_api.net, true);
    // THE SEND-RATE CAP (2026-09-21). In today's Steam client the legacy P2P API
    // rides on the SteamNetworkingSockets stack, whose per-connection send rate
    // defaults to 1 MB/s (k_ESteamNetworkingConfig_SendRateMax) with 512 KB
    // buffers: a 104 MB save moved at 1 MB/s while the lobby offered 1.6, and the
    // rest was dropped as over-rate. Raised globally: the lobby's own window is
    // the pacing after that. Values from steamnetworkingtypes.h; Int32 = 1,
    // scope Global = 1. Each set is logged with its result; a client whose
    // legacy path ignores them loses nothing.
    if (g_api.utils && g_api.setConfig) {
        void* utils = g_api.utils();
        struct { const char* name; int id; int32_t value; } cfg[] = {
            { "SendRateMin",    23,  1 * 1024 * 1024 },
            { "SendRateMax",    24, 16 * 1024 * 1024 },
            { "SendBufferSize",  9,  8 * 1024 * 1024 },
            { "RecvBufferSize", 47,  8 * 1024 * 1024 },
        };
        for (auto& c : cfg) {
            int32_t before = -1; int dt = 0; size_t sz = sizeof(before);
            if (g_api.getConfig && utils) g_api.getConfig(utils, c.id, 1, 0, &dt, &before, &sz);
            bool ok = utils && g_api.setConfig(utils, c.id, 1, 0, 1, &c.value);
            g_log("[steam] %s: %d -> %d (%s)\n", c.name, before, c.value, ok ? "set" : "REFUSED");
        }
    } else {
        g_log("[steam] no SteamNetworkingUtils in this steam_api64.dll -- the send-rate cap stays at Steam's default\n");
    }
    uint16_t ctlPort = 0;
    SOCKET ctl = BindLoopback("127.0.0.1", 0, &ctlPort);
    if (ctl == INVALID_SOCKET) { g_log("[steam] control socket failed (%d) -- transport off\n", WSAGetLastError()); return 0; }
    g_api.registerCb(&g_reqCb, CB_SESSION_REQUEST);
    g_api.registerCb(&g_failCb, CB_CONNECT_FAIL);
    WriteIdentity(g_myId, ctlPort, persona.c_str());
    g_log("[steam] up: id=%llu (%s), control 127.0.0.1:%u, endpoints on %s:%u-%u, relay allowed\n",
          (unsigned long long)g_myId, persona.c_str(), (unsigned)ctlPort, TUNNEL_IP, (unsigned)TUNNEL_PORT_LO, (unsigned)TUNNEL_PORT_HI);

    std::map<uint64_t, Endpoint> eps;
    sockaddr_in lobby = {}; lobby.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &lobby.sin_addr); lobby.sin_port = 0;
    std::vector<char> buf(65536);
    DWORD lastSweep = GetTickCount();
    uint64_t dropNoLobby = 0, sendFail = 0;

    auto endpointFor = [&](uint64_t id) -> Endpoint* {
        auto it = eps.find(id);
        if (it != eps.end()) { it->second.lastSeen = GetTickCount(); return &it->second; }
        if (eps.size() >= 64) { g_log("[steam] 64 endpoints already -- refusing %llu\n", (unsigned long long)id); return nullptr; }
        Endpoint e;
        for (uint16_t p = TUNNEL_PORT_LO; p <= TUNNEL_PORT_HI && e.sock == INVALID_SOCKET; p++)
            e.sock = BindLoopback(TUNNEL_IP, p, &e.port);   // the first free port of the range
        if (e.sock == INVALID_SOCKET) { g_log("[steam] no free endpoint port in %u-%u\n", (unsigned)TUNNEL_PORT_LO, (unsigned)TUNNEL_PORT_HI); return nullptr; }
        e.lastSeen = GetTickCount();
        auto r = eps.emplace(id, e);
        g_log("[steam] endpoint %s:%u <-> %llu\n", TUNNEL_IP, (unsigned)e.port, (unsigned long long)id);
        return &r.first->second;
    };
    auto closeEndpoint = [&](uint64_t id, const char* why) {
        auto it = eps.find(id);
        if (it == eps.end()) return;
        closesocket(it->second.sock);
        g_api.closeSession(g_api.net, id);
        g_log("[steam] endpoint for %llu closed (%s): in=%llu out=%llu reliable=%llu\n", (unsigned long long)id, why,
              (unsigned long long)it->second.in, (unsigned long long)it->second.out, (unsigned long long)it->second.reliable);
        eps.erase(it);
    };
    auto sendP2P = [&](uint64_t id, const char* data, int len, int channel, Endpoint* e) {
        const int type = (len > (int)UNRELIABLE_MAX) ? SEND_RELIABLE : SEND_UNRELIABLE;
        if (!g_api.send(g_api.net, id, data, (uint32_t)len, type, channel)) { sendFail++; return; }
        if (e) { e->out++; if (type == SEND_RELIABLE) e->reliable++; }
    };

    while (!g_stop) {
        // ---- the callbacks' news
        {
            std::vector<std::pair<uint64_t, int>> q;
            { std::lock_guard<std::mutex> lk(g_cbMtx); q.swap(g_cbQueue); }
            for (auto& kv1 : q) { auto& id = kv1.first; auto& kind = kv1.second;
                if (kind == 0) { g_log("[steam] session request from %llu -- accepted\n", (unsigned long long)id); endpointFor(id); }
                else g_log("[steam] session with %llu failed (error %d)\n", (unsigned long long)id, kind - 1);
            }
        }
        // ---- inbound P2P: data to the lobby, control handled here
        for (int ch = CH_CTL; ch >= CH_DATA; ch--) {
            for (int n = 0; n < 256; n++) {
                uint32_t size = 0;
                if (!g_api.avail(g_api.net, &size, ch)) break;
                uint64_t from = 0; uint32_t got = 0;
                if (!g_api.read(g_api.net, buf.data(), (uint32_t)buf.size(), &got, &from, ch)) break;
                Endpoint* e = endpointFor(from);
                if (!e) continue;
                if (ch == CH_CTL) continue;                       // OPEN: the endpoint now exists, nothing to forward
                e->in++;
                if (!lobby.sin_port) { dropNoLobby++; continue; }
                sendto(e->sock, buf.data(), (int)got, 0, (sockaddr*)&lobby, sizeof(lobby));
            }
        }
        // ---- outbound: the lobby's datagrams on each endpoint socket, and control
        fd_set rs; FD_ZERO(&rs);
        FD_SET(ctl, &rs);
        SOCKET maxSocket = ctl;
        for (auto& kv2 : eps) {
            FD_SET(kv2.second.sock, &rs);
            if (kv2.second.sock > maxSocket) maxSocket = kv2.second.sock;
        }
        timeval tv = { 0, 2000 };
        int r = select(static_cast<int>(maxSocket) + 1, &rs, nullptr, nullptr, &tv);
        if (r > 0) {
            for (auto& kv3 : eps) { auto& id = kv3.first; auto& e = kv3.second;
                if (!FD_ISSET(e.sock, &rs)) continue;
                for (int n = 0; n < 256; n++) {
                    sockaddr_in from = {}; SocketLength fl = sizeof(from);
                    int got = recvfrom(e.sock, buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
                    if (got <= 0) break;
                    e.lastSeen = GetTickCount();
                    sendP2P(id, buf.data(), got, CH_DATA, &e);
                }
            }
            if (FD_ISSET(ctl, &rs)) {
                for (int n = 0; n < 32; n++) {
                    sockaddr_in from = {}; SocketLength fl = sizeof(from);
                    int got = recvfrom(ctl, buf.data(), (int)buf.size() - 1, 0, (sockaddr*)&from, &fl);
                    if (got <= 0) break;
                    buf[got] = 0;
                    std::string line(buf.data());
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    std::string reply;
                    unsigned long long id = 0; unsigned port = 0;
                    if (sscanf(line.c_str(), "LOBBY %u", &port) == 1 && port > 0 && port < 65536) {
                        lobby.sin_port = htons((uint16_t)port);
                        reply = "OK";
                        g_log("[steam] the lobby listens on 127.0.0.1:%u\n", port);
                    } else if (sscanf(line.c_str(), "DIAL %llu", &id) == 1 && id) {
                        if (id == g_myId) { reply = "ERR self"; }
                        else {
                            Endpoint* e = endpointFor(id);
                            if (!e) reply = "ERR full";
                            else {
                                // an OPEN on the control channel: implicitly accepts the peer's
                                // session on our side and opens ours on theirs
                                sendP2P(id, "O", 1, CH_CTL, nullptr);
                                char out[96]; snprintf(out, sizeof(out), "EP %llu %s %u", id, TUNNEL_IP, (unsigned)e->port);
                                reply = out;
                            }
                        }
                    } else if (sscanf(line.c_str(), "CLOSE %llu", &id) == 1 && id) {
                        closeEndpoint(id, "closed by the lobby"); reply = "OK";
                    } else if (line == "STATUS") {
                        char head[160]; snprintf(head, sizeof(head), "id=%llu endpoints=%zu no_lobby_drops=%llu send_failures=%llu\n",
                                                    (unsigned long long)g_myId, eps.size(), (unsigned long long)dropNoLobby, (unsigned long long)sendFail);
                        reply = head;
                        for (auto& kv4 : eps) { auto& pid = kv4.first; auto& e = kv4.second;
                            P2PSessionState st = {};
                            bool have = g_api.sessionState && g_api.sessionState(g_api.net, pid, &st);
                            char l[200]; snprintf(l, sizeof(l), "%llu %s:%u in=%llu out=%llu reliable=%llu active=%d connecting=%d relay=%d error=%d queued=%d\n",
                                                     (unsigned long long)pid, TUNNEL_IP, (unsigned)e.port, (unsigned long long)e.in, (unsigned long long)e.out,
                                                     (unsigned long long)e.reliable, have ? st.active : -1, have ? st.connecting : -1, have ? st.usingRelay : -1,
                                                     have ? st.error : -1, have ? st.bytesQueued : -1);
                            reply += l;
                        }
                    } else {
                        reply = "ERR unknown";
                    }
                    sendto(ctl, reply.data(), (int)reply.size(), 0, (sockaddr*)&from, fl);
                }
            }
        }
        // ---- idle endpoints
        DWORD now = GetTickCount();
        if (now - lastSweep > 10000) {
            lastSweep = now;
            std::vector<uint64_t> idle;
            for (auto& kv5 : eps) if (now - kv5.second.lastSeen > EP_IDLE_MS) idle.push_back(kv5.first);
            for (uint64_t id : idle) closeEndpoint(id, "idle");
        }
    }
    for (auto& kv6 : eps) { closesocket(kv6.second.sock); g_api.closeSession(g_api.net, kv6.first); }
    closesocket(ctl);
    if (g_api.unregisterCb) { g_api.unregisterCb(&g_reqCb); g_api.unregisterCb(&g_failCb); }
    WriteIdentity(0, 0, nullptr);
    return 0;
}

} // namespace

bool SteamTunnel_Start(const TunnelPath& dataDir, TunnelLogFn log)
{
    if (g_thread) return false;
    g_stop = false;
    g_dataDir = dataDir; g_log = log;
    if (KillSwitch()) { log("[steam] tpf2mp_steam_off.txt present -- the Steam transport stays off\n"); WriteIdentity(0, 0, nullptr); return false; }
    WriteIdentity(0, 0, nullptr);   // no stale identity from a previous run
#ifdef _WIN32
    g_thread = CreateThread(nullptr, 0, TunnelThread, nullptr, 0, nullptr);
#else
    g_thread = new std::thread([] { TunnelThread(nullptr); });
#endif
    return g_thread != nullptr;
}

void SteamTunnel_Stop()
{
    g_stop = true;
#ifdef _WIN32
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr; }
#else
    if (g_thread) { g_thread->join(); delete g_thread; g_thread = nullptr; }
#endif
}

void SteamTunnel_SignalShutdown() { g_stop = true; }
