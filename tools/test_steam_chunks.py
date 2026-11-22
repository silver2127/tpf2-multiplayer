"""BIG CHUNKS OVER STEAM: a full host + 2-joiner save transfer at CHUNK_STEAM / SEND_WINDOW_STEAM.

The Steam tunnel sends a 32 KB chunk with Steam's reliable P2P send, so on the
real path chunks are not lost; here the same transfer runs over lossy loopback
UDP instead (lobby._run_transfer_once with the chunk choice forced to
CHUNK_STEAM), which is the harder case: the NACK/rewind recovery has to carry
32 KB chunks through a 128-chunk window without stalling.

    python tools/test_steam_chunks.py
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "netpunch"))

import lobby    # noqa: E402

lobby._HostSaveTransfer._pick_chunk = staticmethod(lambda targets: lobby.CHUNK_STEAM)
ok = True
for tag, loss, size in (("steam-clean-48MB", 0.0, 48 * 1024 * 1024),
                        ("steam-lossy-8pct", 0.08, 16 * 1024 * 1024),
                        ("steam-lossy-15pct", 0.15, 12 * 1024 * 1024)):
    t0 = time.time()
    r = lobby._run_transfer_once(loss, size, tag)
    print(f"{'ok  ' if r else 'FAIL'} {tag}: {size / 1e6:.0f} MB in {time.time() - t0:.1f} s")
    ok = ok and r
print("PASS: CHUNK_STEAM transfers complete, clean and lossy" if ok else "FAIL")
sys.exit(0 if ok else 1)
