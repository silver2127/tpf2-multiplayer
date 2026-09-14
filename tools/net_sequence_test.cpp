#include "../native/src/net.cpp"
#include <cassert>

int main() {
    // A selective ACK for packet 100 does not acknowledge missing packet 1.
    g_pending[1] = Packet{};
    g_pending[99] = Packet{};
    g_pending[100] = Packet{};
    for (auto seq : {1u, 99u, 100u}) g_awaiting[seq] = {77, 88};
    ProcessAck(77, 100, 0);
    assert(g_pending.count(100)); // the second recipient still needs it
    ProcessAck(88, 100, 0);
    assert(g_pending.count(1) && g_pending.count(99) && !g_pending.count(100));
    ProcessAck(77, 100, 1);
    ProcessAck(88, 100, 1);
    assert(g_pending.count(1) && !g_pending.count(99));

    // Route migration preserves the stream and its unacknowledged gap.
    // World epoch changes, covered separately, reset all peers together.
    assert(EnsureWsa());
    g_session = 123;
    g_nextSeq = 101;
    g_outQueue.push(NetEvent{});
    assert(Net_SetPeer("127.0.0.1", 7773));
    assert(g_session == 123 && g_nextSeq == 101);
    assert(g_pending.count(1) && !g_outQueue.empty());
    assert(Net_SetPeer("127.0.0.1", 7773));
    assert(g_session == 123); // polling the same address must not reset it
    assert(!Net_SetPeer("bad-address", 7773));
    assert(g_session == 123);
    WSACleanup();
    puts("PASS: selective ACK gaps and relay stream preservation");
}
