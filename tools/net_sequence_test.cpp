#include "../native/src/net.cpp"
#include <cassert>

int main() {
    // A selective ACK for packet 100 does not acknowledge missing packet 1.
    g_pending[1] = Packet{};
    g_pending[99] = Packet{};
    g_pending[100] = Packet{};
    ProcessAck(100, 0);
    assert(g_pending.count(1) && g_pending.count(99) && !g_pending.count(100));
    ProcessAck(100, 1);
    assert(g_pending.count(1) && !g_pending.count(99));

    // Switching from direct loopback to a relay invalidates the old stream,
    // including queued fragments, even if the bridge process stays alive.
    assert(EnsureWsa());
    g_session = 123;
    g_nextSeq = 101;
    g_outQueue.push(NetEvent{});
    assert(Net_SetPeer("127.0.0.1", 7773));
    assert(g_session == 124 && g_nextSeq == 101);
    assert(g_pending.empty() && g_outQueue.empty());
    assert(Net_SetPeer("127.0.0.1", 7773));
    assert(g_session == 124); // polling the same address must not reset it
    assert(!Net_SetPeer("bad-address", 7773));
    assert(g_session == 124);
    WSACleanup();
    puts("PASS: selective ACK gaps and relay stream reset");
}
