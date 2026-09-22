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
static std::vector<std::pair<int, int>> configWrites;
static bool refuseSecond = false;
static void* UtilsTest() { return reinterpret_cast<void*>(1); }
static bool ConfigTest(void*, int key, int scope, intptr_t object, int type, const void* value) {
    assert(scope == 1 && object == 0 && type == 1);
    configWrites.emplace_back(key, *static_cast<const int*>(value));
    return !(refuseSecond && configWrites.size() == 2);
}
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
static int testPending = 100;
static int StateTest(void*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t* info, SteamNetConnectionRealTimeStatus_t* status) {
    info->m_nFlags = k_nSteamNetworkConnectionInfoFlags_Relayed;
    status->m_cbPendingReliable = testPending; status->m_cbPendingUnreliable = 20;
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
    assert(!g_messageRate.active); // low-volume feedback must not govern the rate
    testPending = 65536;
    assert(MessagesState(nullptr, 123, &state));
    assert(g_messageRate.active && g_messageRate.queued);
    assert(g_messageRate.Evaluate() == SteamRateController::Initial / 2);
    assert(MessagesClose(nullptr, 123));
    SteamRateController rate;
    for (int i = 0; i < 2; ++i) {
        rate.Sample(1.0f, true, true);
        assert(rate.Evaluate() == SteamRateController::Initial);
    }
    rate.Sample(1.0f, true, true);
    rate.rate = rate.Evaluate();
    assert(rate.rate == 1280 * 1024);
    // A bad peer wins even when another peer is healthy.
    rate.Sample(1.0f, true, true); rate.Sample(0.15f, true, true);
    rate.rate = rate.Evaluate();
    assert(rate.rate == 640 * 1024 && rate.ceiling == 960 * 1024);
    for (int i = 0; i < 30; ++i) {
        rate.Sample(1.0f, true, true); rate.rate = rate.Evaluate();
    }
    assert(rate.rate == 960 * 1024); // does not repeatedly probe failed rate
    for (int i = 0; i < 12; ++i) {
        rate.Sample(0.1f, true, true); rate.rate = rate.Evaluate();
    }
    assert(rate.rate == SteamRateController::Minimum);
    SteamRateController unknown;
    for (int i = 0; i < 12; ++i) {
        unknown.Sample(-1.0f, true, true); assert(unknown.Evaluate() == unknown.Initial);
        unknown.Sample(1.0f, false, true); assert(unknown.Evaluate() == unknown.Initial);
        unknown.Sample(1.0f, true, false); assert(unknown.Evaluate() == unknown.Initial);
    }
    g_api.utils = UtilsTest; g_api.setConfig = ConfigTest;
    g_messageRate = SteamRateController{}; g_messageRateAt = 1000;
    g_messageRate.Sample(0.15f, true, true);
    UpdateMessagesRate(5999); assert(configWrites.empty());
    UpdateMessagesRate(6000);
    assert(g_messageRate.rate == 512 * 1024);
    assert(configWrites.size() == 2 && configWrites[0].first == 10 && configWrites[1].first == 11);
    assert(configWrites[0].second == 512 * 1024 && configWrites[1].second == 512 * 1024);
    configWrites.clear(); refuseSecond = true;
    g_messageRate.Sample(0.15f, true, true); UpdateMessagesRate(11000);
    assert(g_messageRate.rate == 512 * 1024 && configWrites.size() == 4);
    assert(configWrites[2].second == 512 * 1024 && configWrites[3].second == 512 * 1024);
    configWrites.clear(); refuseSecond = false;
    g_messageRate = SteamRateController{}; g_messageRateAt = 20000;
    for (DWORD now = 25000; now <= 35000; now += 5000) {
        g_messageRate.Sample(1.0f, true, true); UpdateMessagesRate(now);
    }
    assert(g_messageRate.rate == 1280 * 1024 && configWrites.size() == 2);
    assert(configWrites[0].first == 11 && configWrites[1].first == 10);
    assert(configWrites[0].second == 1280 * 1024 && configWrites[1].second == 1280 * 1024);
    puts("PASS: adaptive backoff, slow growth, learned ceiling, unknown/idle guards, multi-peer loss, setter rollback");
    puts("PASS: reliable flags, API failures, receive ownership/bounds, callbacks, modern status");
}
