EXPERIMENTAL: bounded Steam save-transfer retries and throughput diagnostics

This is a test build and a pre-release. Everyone in a session needs 0.6.1.25. The stable release remains unchanged. Real two-computer Steam validation is still needed; successful automated tests do not establish your internet transfer speed.

What changed

- The reliable 32 KB Steam path starts with 512 KB in flight and grows only as the receiver acknowledges delivery, up to about 4 MB. It no longer requeues an entire window simply because Steam has not delivered it yet.
- A stalled transfer probes the first missing piece. If later pieces have already arrived, it repairs a small group of holes. Repeated stalls back off instead of filling Steam's queue with copies.
- This experimental release ENABLES the corrected large-piece path for Steam-only transfers without a local test file. Mixed Steam/CROSS-PLAY sessions keep small pieces. Direct TCP from 0.6.1.24 is still preferred when reachable.
- Logs now show acknowledged/unique bytes, rate, duplicates, recovery probes, Steam queue size and send failures. A displayed 0% alone is not a byte-level diagnosis: the existing UI updates in 10% steps.

Still included

The 0.6.1.24 direct-TCP address exchange and optional live join, Steam ID joining, Workshop downloads, title/dashboard redesign and prior game-speed fixes. Live join remains opt-in; this build does not enable it automatically.

Automated validation

- A finite 8 MiB queue at 256 KiB/s, 1 MiB/s and 16 MiB/s; exact file hashes, local loss, lost feedback, rewind and stalled-peer timeout.
- In the simulated 1 MiB/s case, the same 12 MiB file took about 12 seconds instead of 71 seconds with the previous retry logic. This is a simulation, not a claimed live Steam result.
- Two-receiver transfers with 0%, 8% and 15% injected datagram loss; both direct TCP directions and fallback; normal transfer/failure/retry regression; native bridge build.

How to test

Close the game normally on both machines, install this release's MSI, and retry the same save. Record its size and elapsed transfer time. OPEN LOGS collects the evidence: note whether it says `taking the save over TCP` or uses Steam, and retain both host and joiner logs. Avoid original-save modifications during the transfer test.

Known limitations

The previous large-piece Steam builds stalled in live sessions. This candidate addresses reproduced retry amplification but is not yet a confirmed fix for every live stall. A direct TCP transfer does not test the reliable Steam fallback. Existing optional live-join and unrelated gameplay issues are outside this fix.
