// Adapter for the flat SteamNetworkingMessages v002 API. Included inside the
// tunnel namespace. Both peers must choose the same transport before starting.
struct MessagesApi {
    void* instance = nullptr;
    int (*send)(void*, const SteamNetworkingIdentity*, const void*, uint32_t, int, int) = nullptr;
    int (*receive)(void*, int, SteamNetworkingMessage_t**, int) = nullptr;
    bool (*accept)(void*, const SteamNetworkingIdentity*) = nullptr;
    bool (*close)(void*, const SteamNetworkingIdentity*) = nullptr;
    int (*info)(void*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t*, SteamNetConnectionRealTimeStatus_t*) = nullptr;
    SteamNetworkingMessage_t* pending[2] = {};
} g_messages;
bool g_useMessages = false;

SteamNetworkingIdentity MessageIdentity(uint64_t id) {
    SteamNetworkingIdentity result{};
    result.SetSteamID64(id);
    return result;
}
bool MessagesSend(void*, uint64_t id, const void* data, uint32_t len, int type, int channel) {
    auto identity = MessageIdentity(id);
    // The legacy enum value 2 is NOT the modern reliable flag (8).
    int flags = type == SEND_RELIABLE ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    return g_messages.send(g_messages.instance, &identity, data, len, flags, channel) == 1;
}
bool MessagesAvailable(void*, uint32_t* size, int channel) {
    if (channel < 0 || channel > 1) return false;
    auto*& msg = g_messages.pending[channel];
    if (!msg && g_messages.receive(g_messages.instance, channel, &msg, 1) != 1) return false;
    *size = msg->m_cbSize;
    return true;
}
bool MessagesRead(void*, void* dest, uint32_t cap, uint32_t* size, uint64_t* from, int channel) {
    if (channel < 0 || channel > 1) return false;
    auto*& slot = g_messages.pending[channel];
    auto* msg = slot;
    if (!msg) return false;
    slot = nullptr;
    bool valid = msg->m_cbSize > 0 && uint32_t(msg->m_cbSize) <= cap && msg->m_identityPeer.GetSteamID64();
    if (valid) {
        *from = msg->m_identityPeer.GetSteamID64();
        *size = uint32_t(msg->m_cbSize);
        memcpy(dest, msg->m_pData, *size);
    }
    msg->Release();
    return valid;
}
bool MessagesAccept(void*, uint64_t id) {
    auto identity = MessageIdentity(id);
    return g_messages.accept(g_messages.instance, &identity);
}
bool MessagesClose(void*, uint64_t id) {
    auto identity = MessageIdentity(id);
    return g_messages.close(g_messages.instance, &identity);
}
bool MessagesState(void*, uint64_t id, void* result) {
    auto identity = MessageIdentity(id);
    SteamNetConnectionInfo_t info{};
    SteamNetConnectionRealTimeStatus_t status{};
    int state = g_messages.info(g_messages.instance, &identity, &info, &status);
    auto& out = *static_cast<P2PSessionState*>(result);
    out = {};
    out.active = state == k_ESteamNetworkingConnectionState_Connected;
    out.connecting = state == k_ESteamNetworkingConnectionState_Connecting || state == k_ESteamNetworkingConnectionState_FindingRoute;
    out.usingRelay = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
    out.bytesQueued = status.m_cbPendingReliable + status.m_cbPendingUnreliable;
    out.packetsQueued = -1; // Modern API reports bytes, not a packet count.
    if (g_log) g_log("[steam-messages] state=%d ping=%dms capacity=%dB/s wire_out=%.0fB/s wire_in=%.0fB/s pending=%dB unacked=%dB queue_us=%lld end=%d\n",
        state, status.m_nPing, status.m_nSendRateBytesPerSecond, status.m_flOutBytesPerSec,
        status.m_flInBytesPerSec, out.bytesQueued, status.m_cbSentUnackedReliable,
        (long long)status.m_usecQueueTime, info.m_eEndReason);
    return state != k_ESteamNetworkingConnectionState_None;
}
struct MessagesRequestCb : CallbackBase {
    void Run(void* p) override {
        auto id = static_cast<SteamNetworkingIdentity*>(p)->GetSteamID64();
        if (!id || !MessagesAccept(nullptr, id)) return;
        std::lock_guard<std::mutex> lk(g_cbMtx);
        g_cbQueue.emplace_back(id, 0);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int GetCallbackSizeBytes() override { return sizeof(SteamNetworkingIdentity); }
} g_messagesRequest;
struct MessagesFailCb : CallbackBase {
    void Run(void* p) override {
        auto& info = *static_cast<SteamNetConnectionInfo_t*>(p);
        std::lock_guard<std::mutex> lk(g_cbMtx);
        g_cbQueue.emplace_back(info.m_identityRemote.GetSteamID64(), 1 + info.m_eEndReason);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int GetCallbackSizeBytes() override { return sizeof(SteamNetConnectionInfo_t); }
} g_messagesFail;

bool StartMessages() {
#ifdef _WIN32
    auto module = GetModuleHandleW(L"steam_api64.dll");
    if (!module) return false;
    auto get = [&](const char* name) { return GetProcAddress(module, name); };
#else
    // Resolve only the game's loaded Steam API; no dlopen or SteamAPI_Init.
    auto get = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
#endif
    auto accessor = reinterpret_cast<FnAccessor>(get("SteamAPI_SteamNetworkingMessages_SteamAPI_v002"));
    #define MESSAGE_API(field, name) g_messages.field = reinterpret_cast<decltype(g_messages.field)>(get("SteamAPI_ISteamNetworkingMessages_" name))
    MESSAGE_API(send, "SendMessageToUser");
    MESSAGE_API(receive, "ReceiveMessagesOnChannel");
    MESSAGE_API(accept, "AcceptSessionWithUser");
    MESSAGE_API(close, "CloseSessionWithUser");
    MESSAGE_API(info, "GetSessionConnectionInfo");
    #undef MESSAGE_API
    if (!accessor || !g_messages.send || !g_messages.receive || !g_messages.accept || !g_messages.close || !g_messages.info) return false;
    g_messages.instance = accessor();
    if (!g_messages.instance) return false;
    g_api.send = MessagesSend; g_api.avail = MessagesAvailable; g_api.read = MessagesRead;
    g_api.accept = MessagesAccept; g_api.closeSession = MessagesClose; g_api.sessionState = MessagesState;
    g_useMessages = true;
    return true;
}
