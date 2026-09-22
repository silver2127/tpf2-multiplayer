// Exercise the actual adapter with a fake Steam boundary. No game/Steam init.
#include "../native/src/steam_tunnel.cpp"
#include <cassert>
#include <cstdarg>

static std::string statusLog;
static void CaptureLog(const char* format, ...) {
    char text[1024];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    statusLog = text;
}

struct TestMessage : SteamNetworkingMessage_t {
    TestMessage() { memset(static_cast<SteamNetworkingMessage_t*>(this), 0, sizeof(SteamNetworkingMessage_t)); }
};
static int released = 0, flagsSeen = -1, resultCode = 1;
static SteamNetworkingMessage_t* incoming = nullptr;
static void ReleaseTest(SteamNetworkingMessage_t* msg) { ++released; delete static_cast<TestMessage*>(msg); }
static int SendTest(void*, const SteamNetworkingIdentity* peer, const void*, uint32_t len, int flags, int channel) {
    assert(peer->GetSteamID64() == 123); assert(len == 3); assert(channel == CH_DATA);
    flagsSeen = flags; return resultCode;
}
static int ReceiveTest(void*, int, SteamNetworkingMessage_t** out, int) {
    if (!incoming) return 0;
    *out = incoming; incoming = nullptr; return 1;
}
static bool AcceptTest(void*, const SteamNetworkingIdentity* peer) { return peer->GetSteamID64() == 123; }
static int StateTest(void*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t* info, SteamNetConnectionRealTimeStatus_t* status) {
    info->m_nFlags = k_nSteamNetworkConnectionInfoFlags_Relayed;
    status->m_cbPendingReliable = 100; status->m_cbPendingUnreliable = 20;
    status->m_cbSentUnackedReliable = 50;
    status->m_flConnectionQualityLocal = 0.875f;
    status->m_flConnectionQualityRemote = 0.625f;
    return k_ESteamNetworkingConnectionState_Connected;
}
static void Enqueue(int size, uint64_t peer = 123) {
    auto* msg = new TestMessage{};
    msg->m_pData = const_cast<char*>("abc"); msg->m_cbSize = size;
    msg->m_identityPeer = MessageIdentity(peer); msg->m_pfnRelease = ReleaseTest;
    incoming = msg;
}
int main() {
    static_assert(sizeof(SteamNetworkingIdentity) == 136, "Steam identity ABI");
    static_assert(offsetof(SteamNetworkingMessage_t, m_pfnRelease) == 184, "x64 message ABI");
    g_messages.send = SendTest; g_messages.receive = ReceiveTest;
    g_messages.accept = AcceptTest; g_messages.close = AcceptTest; g_messages.info = StateTest;
    assert(MessagesSend(nullptr, 123, "abc", 3, SEND_RELIABLE, CH_DATA));
    assert(flagsSeen == k_nSteamNetworkingSend_Reliable);
    assert(MessagesSend(nullptr, 123, "abc", 3, SEND_UNRELIABLE, CH_DATA));
    assert(flagsSeen == k_nSteamNetworkingSend_Unreliable);
    resultCode = 25; assert(!MessagesSend(nullptr, 123, "abc", 3, SEND_RELIABLE, CH_DATA));
    uint32_t size = 0; uint64_t peer = 0; char data[4]{};
    assert(!MessagesAvailable(nullptr, &size, CH_DATA));
    Enqueue(3);
    assert(MessagesAvailable(nullptr, &size, CH_DATA) && size == 3);
    assert(MessagesAvailable(nullptr, &size, CH_DATA)); // does not consume twice
    assert(MessagesRead(nullptr, data, sizeof(data), &size, &peer, CH_DATA));
    assert(peer == 123 && size == 3 && memcmp(data, "abc", 3) == 0 && released == 1);
    assert(!MessagesRead(nullptr, data, sizeof(data), &size, &peer, CH_DATA));
    Enqueue(999); assert(MessagesAvailable(nullptr, &size, CH_CTL));
    assert(!MessagesRead(nullptr, data, sizeof(data), &size, &peer, CH_CTL) && released == 2);
    Enqueue(3, 0); assert(MessagesAvailable(nullptr, &size, CH_DATA));
    assert(!MessagesRead(nullptr, data, sizeof(data), &size, &peer, CH_DATA) && released == 3);
    auto id = MessageIdentity(123); g_messagesRequest.Run(&id);
    assert(g_cbQueue.size() == 1 && g_cbQueue[0].first == 123 && g_cbQueue[0].second == 0);
    SteamNetConnectionInfo_t failure{}; failure.m_identityRemote = id; failure.m_eEndReason = 5003;
    g_messagesFail.Run(&failure); assert(g_cbQueue.back().second == 5004);
    g_log = CaptureLog;
    P2PSessionState state{}; assert(MessagesState(nullptr, 123, &state));
    assert(state.active == 1 && state.usingRelay == 1 && state.bytesQueued == 120 && state.packetsQueued == -1);
    assert(statusLog.find("quality_local=0.875 quality_remote=0.625") != std::string::npos);
    assert(MessagesClose(nullptr, 123));
    puts("PASS: reliable flags, API failures, receive ownership/bounds, callbacks, modern status");
}
