#include "../native/src/net.cpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    printf("[TEST] Starting net_rto_resilience_test...\n");

    // 1. Test RttEstimator algorithm directly
    {
        RttEstimator rtt;
        assert(rtt.rto == INITIAL_RESEND_MS);

        // Feed samples of 20ms (LAN)
        for (int i = 0; i < 20; ++i) {
            rtt.NoteSample(20);
        }
        printf("  LAN RTO: srtt=%.1f rttvar=%.1f rto=%u\n", rtt.srtt, rtt.rttvar, rtt.rto);
        assert(rtt.rto >= MIN_RESEND_MS && rtt.rto <= 100); // Must settle low for fast recovery

        // Feed samples of 300ms (Trans-oceanic)
        for (int i = 0; i < 30; ++i) {
            rtt.NoteSample(300);
        }
        printf("  WAN RTO: srtt=%.1f rttvar=%.1f rto=%u\n", rtt.srtt, rtt.rttvar, rtt.rto);
        assert(rtt.rto > 250); // Must be > 250ms so packets are not prematurely resent

        // Test exponential backoff
        uint32_t prevRto = rtt.rto;
        rtt.Backoff();
        assert(rtt.rto >= prevRto * 2 || rtt.rto == MAX_RESEND_MS * 2);
        printf("  Backoff RTO: %u\n", rtt.rto);
    }

    // 2. A packet a quiet (not yet evicted) peer still owes stays pending: it is
    //    resent until the peer answers or is evicted (follow-up to PR #12)
    {
        g_pending.clear();
        g_awaiting.clear();
        g_lastSent.clear();
        g_firstSentTime.clear();
        g_sendCount.clear();
        g_streams.clear();

        // 3 peers: 101, 102, 103
        g_streams[101] = PeerStream{};
        g_streams[102] = PeerStream{};
        g_streams[103] = PeerStream{};

        uint64_t now = GetTickCount64();
        g_streams[101].lastRecvMs = now;
        g_streams[102].lastRecvMs = now;
        g_streams[103].lastRecvMs = now - 2500; // 103 has been quiet for 2.5 s

        // Queue packet 1 awaiting all 3 peers
        g_pending[1] = Packet{};
        g_awaiting[1] = {101, 102, 103};

        // Healthy peers 101 and 102 ACK packet 1
        ProcessAck(101, 1, 1, 0);
        ProcessAck(102, 1, 1, 0);

        // Only lagged peer 103 remains
        assert(g_awaiting[1].size() == 1 && g_awaiting[1].count(103));
        assert(g_pending.count(1)); // Still pending before lag sweep

        // Nothing releases it before eviction: the command must reach 103 too
        assert(g_pending.count(1) == 1);
        assert(g_awaiting.count(1) == 1);
        printf("  Quiet peer: its packet stays pending until it answers or is evicted\n");
    }

    // 3. Test Karn's algorithm RTT sampling in ProcessAck
    {
        g_pending.clear();
        g_awaiting.clear();
        g_lastSent.clear();
        g_firstSentTime.clear();
        g_sendCount.clear();
        g_streams.clear();

        g_streams[201] = PeerStream{};
        g_streams[201].lastRecvMs = GetTickCount64();
        g_streams[201].rtt.srtt = 100.0f;

        // Packet 10: sent once at now - 40ms
        uint64_t now = GetTickCount64();
        g_pending[10] = Packet{};
        g_awaiting[10] = {201};
        g_firstSentTime[10] = now - 40;
        g_sendCount[10] = 1; // Exactly 1 transmission

        // Packet 11: retransmitted (sendCount = 2) at now - 300ms
        g_pending[11] = Packet{};
        g_awaiting[11] = {201};
        g_firstSentTime[11] = now - 300;
        g_sendCount[11] = 2; // Retransmitted! Should NOT be sampled by Karn's rule

        // ACK packet 10
        ProcessAck(201, 10, 10, 0);
        // SRTT should have moved towards 40ms
        assert(g_streams[201].rtt.srtt < 100.0f);
        float srttAfterPkt10 = g_streams[201].rtt.srtt;

        // ACK packet 11 (retransmitted)
        ProcessAck(201, 11, 11, 0);
        // SRTT should NOT have changed because sendCount was 2!
        assert(g_streams[201].rtt.srtt == srttAfterPkt10);
        printf("  Karn's algorithm check: successfully ignored retransmitted packet sample!\n");
    }

    // 4. Test Cumulative ACK (ACK-02) and 64-bit SACK bitmap
    {
        g_pending.clear();
        g_awaiting.clear();
        g_lastSent.clear();
        g_firstSentTime.clear();
        g_sendCount.clear();
        g_streams.clear();

        g_streams[301] = PeerStream{};
        g_streams[301].lastRecvMs = GetTickCount64();

        // Populate packets 1 through 60 in flight
        for (uint32_t s = 1; s <= 60; ++s) {
            g_pending[s] = Packet{};
            g_awaiting[s] = {301};
            g_firstSentTime[s] = GetTickCount64();
            g_sendCount[s] = 1;
        }

        // Test Cumulative ACK: cumAck = 20 acknowledges 1..20 all at once
        ProcessAck(301, NO_ACK, 20, 0);
        for (uint32_t s = 1; s <= 20; ++s) {
            assert(g_pending.count(s) == 0);
        }
        assert(g_pending.count(21) == 1);
        assert(g_pending.size() == 40); // 21..60 remaining
        printf("  Cumulative ACK: successfully cleared range 1..20 via cumAck!\n");

        // Test 64-bit Selective ACK: ack = 60, bitmask acknowledges packet (60 - 45) = 15
        // which is 45 packets behind (previously impossible with 32-bit mask!)
        // Bit index for packet 15 relative to ack 60: (60 - 15 - 1) = 44 -> (1ULL << 44)
        uint32_t ackSeq = 60;
        uint32_t targetSeq = 25; // 60 - 25 = 35 positions back (> 32)
        uint64_t sackBits = (1ULL << (ackSeq - targetSeq - 1)); // bit 34

        ProcessAck(301, ackSeq, 20, sackBits);
        assert(g_pending.count(60) == 0); // ackSeq itself is acknowledged
        assert(g_pending.count(targetSeq) == 0); // targetSeq (35 pkts back) acknowledged by 64-bit SACK!
        assert(g_pending.count(24) == 1); // 24 was not in SACK, remains pending
        assert(g_pending.count(26) == 1); // 26 was not in SACK, remains pending
        printf("  64-bit SACK check: successfully acknowledged packet 35 positions behind ack!\n");
    }

    // 5. Test Outbound Queue Hard Limit (NET-02)
    {
        while (!g_outQueue.empty()) g_outQueue.pop();
        g_peerEverSeen = true;
        g_droppedOverflow = 0;

        // Fill outbound queue up to MAX_OUT_QUEUE
        for (size_t i = 0; i < MAX_OUT_QUEUE; ++i) {
            NetEvent ev{};
            ev.type = 1;
            g_outQueue.push(ev);
        }
        assert(g_outQueue.size() == MAX_OUT_QUEUE);

        // Past the limit a line is still queued, whole: dropping it is a lost command
        Net_QueueLine("overflow_test_line");
        assert(g_outQueue.size() == MAX_OUT_QUEUE + 1);
        assert(g_droppedOverflow == 0);
        printf("  Outbound queue past %zu: the line is still queued, nothing dropped\n", MAX_OUT_QUEUE);
        while (!g_outQueue.empty()) g_outQueue.pop();
    }

    // 6. Test Lock Deadlock Prevention: Net_SetWorldEpoch and Net_BeginLobby
    {
        assert(EnsureWsa());
        bool callbackCalled = false;
        auto dummyReset = [](const char* ep) {
            // Emulate what ResetWorldFiles does: verify that calling this doesn't deadlock
            printf("  Dummy resetLocal called for epoch %.8s..\n", ep);
        };

        const char* testEpoch1 = "0123456789abcdef0123456789abcdef";
        assert(Net_SetWorldEpoch(testEpoch1, dummyReset));
        assert(Net_WorldEpoch() == testEpoch1);

        const char* testEpoch2 = "abcdef0123456789abcdef0123456789";
        assert(Net_BeginLobby(testEpoch2, "127.0.0.1", 7771, dummyReset));
        assert(Net_WorldEpoch() == testEpoch2);

        WSACleanup();
        printf("  Lock order / reset callback test passed cleanly!\n");
    }

    printf("\nALL MILESTONE 1 & 2 RESILIENCE TESTS PASSED!\n");
    return 0;
}
