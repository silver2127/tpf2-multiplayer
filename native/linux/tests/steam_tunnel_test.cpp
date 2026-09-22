// Exercise the shared transport against a fake Steam flat API and real UDP.
#include "../../src/steam_tunnel.cpp"
#include <cassert>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <sstream>

struct Packet { std::vector<char> bytes; int channel; };
static std::mutex packetsMutex;
static std::deque<Packet> packets;
static int registrations = 0, removals = 0;
static std::atomic<int> reliablePackets{0}, accepted{0};
#define API extern "C" __attribute__((visibility("default")))
static int subscribed=0, downloaded=0;
static std::map<int,int32_t> configValues;
API void* SteamAPI_SteamNetworkingUtils_SteamAPI_v004() { return reinterpret_cast<void*>(4); }
API bool SteamAPI_ISteamNetworkingUtils_SetConfigValue(void* utils,int id,int scope,intptr_t object,int type,const void* value) {
    assert(utils==reinterpret_cast<void*>(4) && scope==1 && object==0 && type==1);
    assert(id==10 || id==11 || id==9 || id==47);
    configValues[id]=*static_cast<const int32_t*>(value);return true;
}
API void* SteamAPI_SteamUGC_v016() { return reinterpret_cast<void*>(3); }
API uint64_t SteamAPI_ISteamUGC_SubscribeItem(void*, uint64_t id) { assert(id==123);++subscribed;return 7; }
API bool SteamAPI_ISteamUGC_DownloadItem(void*, uint64_t id, bool high) { assert(id==123 && high);++downloaded;return true; }
API uint32_t SteamAPI_ISteamUGC_GetItemState(void*, uint64_t id) { assert(id==123);return 5; }
API bool SteamAPI_ISteamUGC_GetItemDownloadInfo(void*, uint64_t, uint64_t* done, uint64_t* total) { *done=42;*total=42;return true; }
API bool SteamAPI_ISteamUGC_GetItemInstallInfo(void*, uint64_t, uint64_t* size, char* folder, uint32_t cap, uint32_t* ts) {
    *size=42;*ts=1;snprintf(folder,cap,"/workshop/path with spaces");return true;
}
API void* SteamAPI_SteamNetworking_v006() { return reinterpret_cast<void*>(1); }
API void* SteamAPI_SteamUser_v021() { return reinterpret_cast<void*>(2); }
API uint64_t SteamAPI_ISteamUser_GetSteamID(void*) { return 1001; }
API bool SteamAPI_ISteamNetworking_SendP2PPacket(void*, uint64_t id, const void* p, uint32_t n, int mode, int channel) {
    assert(id == 2002);
    assert(mode == (n > 1200 ? 2 : 0));
    if (mode == 2) ++reliablePackets;
    std::lock_guard<std::mutex> lock(packetsMutex);
    packets.push_back({std::vector<char>((const char*)p, (const char*)p+n), channel});
    return true;
}
API bool SteamAPI_ISteamNetworking_IsP2PPacketAvailable(void*, uint32_t* n, int channel) {
    std::lock_guard<std::mutex> lock(packetsMutex);
    for (const auto& p : packets) if (p.channel == channel) { *n = p.bytes.size(); return true; }
    return false;
}
API bool SteamAPI_ISteamNetworking_ReadP2PPacket(void*, void* dest, uint32_t cap, uint32_t* n, uint64_t* from, int channel) {
    std::lock_guard<std::mutex> lock(packetsMutex);
    for (auto i = packets.begin(); i != packets.end(); ++i) if (i->channel == channel) {
        assert(i->bytes.size() <= cap); *n = i->bytes.size(); *from = 2002;
        memcpy(dest, i->bytes.data(), *n); packets.erase(i); return true;
    }
    return false;
}
API bool SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(void*, uint64_t id) { assert(id == 2002); ++accepted; return true; }
API bool SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(void*, uint64_t id) { assert(id == 2002); return true; }
API bool SteamAPI_ISteamNetworking_AllowP2PPacketRelay(void*, bool allow) { assert(allow); return true; }
API void SteamAPI_RegisterCallback(void* p, int id) {
    auto* cb = static_cast<CallbackBase*>(p); cb->id = id; cb->flags = 1; ++registrations;
    if (id == 1251) {
        assert(cb->GetCallbackSizeBytes() == sizeof(SteamNetworkingIdentity));
        auto identity = MessageIdentity(2002); cb->Run(&identity);
    } else if (id == 1252) {
        assert(cb->GetCallbackSizeBytes() == sizeof(SteamNetConnectionInfo_t));
        SteamNetConnectionInfo_t info{}; info.m_identityRemote = MessageIdentity(2002);
        info.m_eEndReason = 5003; cb->Run(&info);
    } else if (id == 1202) {
        assert(cb->GetCallbackSizeBytes() == 8);
        P2PSessionRequest request{2002}; cb->Run(&request, false, 0);
    } else {
        assert(id == 1203 && cb->GetCallbackSizeBytes() == 12);
        P2PSessionConnectFail failure{2002, 4}; cb->Run(&failure);
    }
}
API void SteamAPI_UnregisterCallback(void* p) { static_cast<CallbackBase*>(p)->flags = 0; ++removals; }
static bool messagesMode = false, messagesUnavailable = false;
API void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002() {
    return messagesUnavailable ? nullptr : reinterpret_cast<void*>(5);
}
API int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void*, const SteamNetworkingIdentity* id, const void* data, uint32_t len, int flags, int channel) {
    assert(flags == (len > 1200 ? 8 : 0));
    return SteamAPI_ISteamNetworking_SendP2PPacket(nullptr, id->GetSteamID64(), data, len, flags == 8 ? 2 : 0, channel) ? 1 : 2;
}
struct TunnelMessage : SteamNetworkingMessage_t {};
static void ReleaseMessage(SteamNetworkingMessage_t* msg) { delete[] static_cast<char*>(msg->m_pData); delete static_cast<TunnelMessage*>(msg); }
API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void*, int channel, SteamNetworkingMessage_t** out, int count) {
    assert(count == 1);
    uint32_t size = 0;
    if (!SteamAPI_ISteamNetworking_IsP2PPacketAvailable(nullptr, &size, channel)) return 0;
    auto* msg = new TunnelMessage{};
    msg->m_pData = new char[size]; uint64_t peer = 0;
    assert(SteamAPI_ISteamNetworking_ReadP2PPacket(nullptr, msg->m_pData, size, &size, &peer, channel));
    msg->m_cbSize = size; msg->m_identityPeer = MessageIdentity(peer); msg->m_pfnRelease = ReleaseMessage;
    *out = msg; return 1;
}
API bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void*, const SteamNetworkingIdentity* id) {
    return SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(nullptr, id->GetSteamID64());
}
API bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void*, const SteamNetworkingIdentity* id) {
    return SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(nullptr, id->GetSteamID64());
}
API int SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(void*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t*, SteamNetConnectionRealTimeStatus_t*) {
    return k_ESteamNetworkingConnectionState_Connected;
}
static void LogTest(const char*, ...) {}
static sockaddr_in Address(uint16_t port) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); return a;
}
static std::string Receive(int sock, uint16_t* fromPort = nullptr) {
    fd_set rs; FD_ZERO(&rs); FD_SET(sock, &rs); timeval tv{3, 0};
    assert(select(sock+1, &rs, nullptr, nullptr, &tv) == 1);
    char data[4096]; sockaddr_in a{}; socklen_t size = sizeof(a);
    const auto n = recvfrom(sock, data, sizeof(data), 0, (sockaddr*)&a, &size);
    assert(n >= 0); if (fromPort) *fromPort = ntohs(a.sin_port);
    return {data, static_cast<size_t>(n)};
}
static std::string Control(int sock, uint16_t port, const std::string& text) {
    const auto a = Address(port);
    assert(sendto(sock, text.data(), text.size(), 0, (sockaddr*)&a, sizeof(a)) == (ssize_t)text.size());
    return Receive(sock);
}
int main(int argc, char**) {
    messagesMode = argc > 1;
    assert(ResolveApi());
    g_log=LogTest;
    assert(UgcCommand("UGC SUB 123")=="OK 1" && subscribed==1 && downloaded==1);
    assert(UgcCommand("UGC STATE 123")=="STATE\n123 5 42 42 /workshop/path with spaces\n");
    g_api.ugc=nullptr;assert(UgcCommand("UGC SUB 123")=="ERR nougc");

    char temporary[] = "/tmp/tpf2mp-steam.XXXXXX"; assert(mkdtemp(temporary));
    const std::string dir = std::string(temporary) + "/";
    if (!messagesMode) { std::ofstream legacy(dir + "tpf2mp_steam_legacy.txt"); legacy << "1\n"; }
    uint16_t occupiedPort = 0;
    const int occupied = BindLoopback(TUNNEL_IP, TUNNEL_PORT_LO, &occupiedPort);
    assert(occupied >= 0); // The endpoint must skip a port used by another game.
    assert(SteamTunnel_Start(dir, LogTest));
    uint16_t control = 0;
    for (int n = 0; n < 300 && !control; ++n) {
        std::ifstream f(dir + "tpf2_steam.txt"); std::string line;
        while (std::getline(f, line)) if (line.rfind("port=", 0) == 0) control = std::stoi(line.substr(5));
        if (!control) Sleep(10);
    }
    assert(control && accepted == 1);
    assert(g_useMessages == messagesMode);
    uint16_t callerPort = 0, lobbyPort = 0;
    const int caller = BindLoopback(TUNNEL_IP, 0, &callerPort);
    const int lobby = BindLoopback(TUNNEL_IP, 0, &lobbyPort);
    assert(Control(caller, control, "DIAL 1001") == "ERR self");
    assert(Control(caller, control, "LOBBY 0") == "ERR unknown");
    assert(Control(caller, control, "LOBBY " + std::to_string(lobbyPort)) == "OK");
    const auto reply = Control(caller, control, "DIAL 2002");
    unsigned ep = 0; assert(sscanf(reply.c_str(), "EP 2002 127.0.0.1 %u", &ep) == 1);
    assert(ep > occupiedPort && ep <= TUNNEL_PORT_HI);
    for (unsigned n : {24u, 1200u, 1400u}) {
        std::string bytes(n, '\0'); for (unsigned i = 0; i < n; ++i) bytes[i] = char(i);
        const auto a = Address(ep);
        assert(sendto(lobby, bytes.data(), bytes.size(), 0, (sockaddr*)&a, sizeof(a)) == (ssize_t)n);
        uint16_t from = 0; assert(Receive(lobby, &from) == bytes && from == ep);
    }
    assert(reliablePackets == 1);
    assert(Control(caller, control, "STATUS").find("endpoints=1") != std::string::npos);
    assert(Control(caller, control, "CLOSE 2002") == "OK");
    assert(Control(caller, control, "STATUS").find("endpoints=0") != std::string::npos);
    SteamTunnel_Stop(); assert(registrations == 2 && removals == 2);
    assert(configValues.size()==4 && configValues[10]==1024*1024 && configValues[11]==16*1024*1024);
    assert(configValues[9]==8*1024*1024 && configValues[47]==8*1024*1024);
    std::ifstream identity(dir + "tpf2_steam.txt"); assert(identity.peek() == EOF);
    // Restart without the switch and with an unavailable accessor: stay off,
    // publish no identity and register no callbacks (no implicit Legacy).
    unlink((dir + "tpf2mp_steam_legacy.txt").c_str());
    messagesUnavailable = true;
    assert(SteamTunnel_Start(dir, LogTest));
    Sleep(50); SteamTunnel_Stop();
    assert(registrations == 2 && removals == 2 && !g_useMessages);
    std::ifstream unavailable(dir + "tpf2_steam.txt"); assert(unavailable.peek() == EOF);
    { std::ofstream disabled(dir + "tpf2mp_steam_off.txt"); disabled << "1\n"; }
    assert(!SteamTunnel_Start(dir, LogTest));
    close(caller); close(lobby); close(occupied);
    unlink((dir + "tpf2_steam.txt").c_str()); unlink((dir + "tpf2mp_steam_off.txt").c_str()); unlink((dir + "tpf2mp_steam_legacy.txt").c_str()); rmdir(temporary);
    puts("PASS: native Steam callbacks, UDP endpoints, binary packets, reliability boundary and shutdown");
}
