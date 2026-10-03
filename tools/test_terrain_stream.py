"""The terrain stream (bigmap/docs/terrain-stream.md): after START the host
sends the save's terrain sidecar to joiners that take it, and each joiner
writes it IN ORDER into <data>/terrain_stream/ while its game loads (the
big-map plugin reads the file as it grows). A real _HostSaveTransfer and a
_TerrReceiver on a simulated wire that reorders and drops chunks.

  python tools/test_terrain_stream.py
"""
import hashlib
import json
import os
import random
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "netpunch"))
_TMP = tempfile.TemporaryDirectory()
os.environ["TPF2MP_DATADIR"] = _TMP.name
os.environ["XDG_DATA_HOME"] = _TMP.name
import lobby  # noqa: E402
from punch import _unpack  # noqa: E402

lobby.BULK_TCP[0] = False
HOST, JOINER = ("10.0.0.1", 1), ("10.0.0.2", 2)


def sidecar(nbytes, seed=1):
    rnd = random.Random(seed)
    return lobby.TERR_MAGIC + bytes(rnd.getrandbits(8) for _ in range(nbytes - len(lobby.TERR_MAGIC)))


class Wire:
    """The host's socket and the joiner's connection, with loss and reorder."""

    def __init__(self, loss=0.0, seed=7):
        self.to_joiner, self.to_host = [], []
        self.rnd = random.Random(seed)
        self.loss = loss
        wire = self

        class Sock:
            def sendto(self, frame, addr):
                assert addr == JOINER
                wire.to_joiner.append(frame)

        class Conn:
            peer = HOST

            def send(self, piece):
                wire.to_host.append(piece)

        self.sock, self.conn = Sock(), Conn()


def run(blob, receiver_cb=None, loss=0.0, sha=None, steps=20000):
    """Stream `blob` host -> joiner; returns (host transfer, receiver, stream path)."""
    wire = Wire(loss)
    log = []
    host = lobby._HostSaveTransfer(wire.sock, 4242, blob,
                                   [{"name": lobby.TERR_NAME, "size": len(blob), "sha256": sha or hashlib.sha256(blob).hexdigest()}],
                                   [(JOINER, "joiner")], lobby._QuietIO(type("IO", (), {"dir": _TMP.name})()), log.append,
                                   kind="terr", overall_sha=sha)
    rx = lobby._TerrReceiver(wire.conn, type("IO", (), {"dir": _TMP.name, "emit": lambda s, e: None})(), log.append)
    now = time.time()
    for _ in range(steps):
        now += 0.05
        host.pump(now)
        frames, wire.to_joiner = wire.to_joiner, []
        wire.rnd.shuffle(frames)
        for frame in frames:
            _, payload = _unpack(frame)
            if payload[:4] == lobby.CHUNK_MAGIC:
                if wire.rnd.random() < wire.loss:
                    continue
                sid, seq = lobby.struct.unpack("!II", payload[4:12])
                rx.on_chunk(sid, seq, payload[12:])
            else:
                msg = json.loads(payload.decode("utf-8"))
                if msg.get("t") == "fbegin":
                    rx.on_begin(msg)
        rx.tick(now)
        if receiver_cb:
            receiver_cb(rx)
        replies, wire.to_host = wire.to_host, []
        for piece in replies:
            msg = json.loads(piece.decode("utf-8"))
            {"fbegin_ack": host.on_begin_ack, "fack": host.on_fack, "fdone": host.on_fdone}[msg["t"]](JOINER, msg)
        if host.all_resolved():
            break
    return host, rx, rx._path, log


class TerrainStream(unittest.TestCase):
    def test_stream_lookup_checks_every_stream_and_only_active_peers(self):
        other = ("127.0.0.1", 40002)
        key = ("/saves/world.terr", 100, 123)
        streams = [
            SimpleNamespace(terr_key=("/saves/old.terr", 100, 123),
                            peers={JOINER: {"state": "active"}}),
            SimpleNamespace(terr_key=key, peers={other: {"state": "active"}}),
            SimpleNamespace(terr_key=key, peers={JOINER: {"state": "active"}}),
        ]
        self.assertTrue(lobby._terr_streaming(streams, key, JOINER))
        for state in ("done", "failed", "dropped"):
            with self.subTest(state=state):
                streams[-1].peers[JOINER]["state"] = state
                self.assertFalse(lobby._terr_streaming(streams, key, JOINER))
                self.assertTrue(lobby._terr_streaming(streams, key, other))

    def test_sidecar_key_detects_same_size_rewrite_and_normalizes_path(self):
        d = Path(_TMP.name) / "key-test"
        d.mkdir(exist_ok=True)
        terr = d / "world.terr"
        terr.write_bytes(sidecar(100))
        key = lobby._terr_key(str(terr))
        alias = os.path.join(str(d), "..", d.name, terr.name)
        self.assertEqual(lobby._terr_key(alias), key)
        terr.write_bytes(sidecar(100, seed=2))
        # Set a distinct timestamp explicitly: no filesystem-resolution sleep.
        st = terr.stat()
        os.utime(terr, ns=(st.st_atime_ns, key[2] + 2_000_000_000))
        changed = lobby._terr_key(str(terr))
        self.assertEqual(changed[:2], key[:2])
        self.assertNotEqual(changed, key)

    def test_streams_in_order_under_loss_and_reorder(self):
        blob = sidecar(700_000)
        prefixes = []

        def watch(rx):
            if rx._path and os.path.exists(rx._path):
                data = Path(rx._path).read_bytes()
                prefixes.append(len(data))
                self.assertEqual(data, blob[:len(data)], "only the in-order prefix is ever on disk")

        host, rx, path, _ = run(blob, watch, loss=0.1)
        self.assertTrue(host.all_resolved() and host.done_count() == 1)
        self.assertTrue(rx.complete and not rx.failed)
        self.assertEqual(Path(path).read_bytes(), blob)
        self.assertEqual(Path(path).parent, Path(_TMP.name) / lobby.TERR_DIRNAME)
        self.assertTrue(any(0 < n < len(blob) for n in prefixes), "the file grows while it arrives")
        self.assertIsNone(rx.buf, "never held whole in memory")

    def test_hash_mismatch_drops_the_file(self):
        blob = sidecar(50_000, seed=3)
        host, rx, path, _ = run(blob, sha="0" * 64)
        self.assertTrue(rx.failed and not rx.complete)
        self.assertFalse(os.path.exists(path))
        self.assertEqual(host.done_count(), 0)
        self.assertTrue(host.all_resolved())

    def test_the_load_done_marker_stops_the_rest(self):
        blob = sidecar(6_000_000, seed=5)                   # several pump steps

        def plugin(rx):
            if rx._path and rx.base > 20 and not os.path.exists(rx._path + ".done"):
                Path(rx._path + ".done").write_bytes(b"")
                rx._done_look = 0.0                  # its next tick looks (the lobby looks once a second)

        host, rx, path, log = run(blob, plugin)
        self.assertTrue(rx.failed and not rx.complete)
        self.assertTrue(host.all_resolved() and host.done_count() == 0)
        self.assertTrue(any("the load finished with" in line for line in log))

    def test_refusals(self):
        rx = lobby._TerrReceiver(Wire().conn, type("IO", (), {"dir": _TMP.name})(), lambda s: None)
        sent = []
        rx._send = sent.append
        base = {"t": "fbegin", "kind": "terr", "sid": 9, "total_bytes": 1000, "chunk": 1200, "total_chunks": 1, "sha256": "x"}
        rx.on_begin(dict(base, files=[{"name": "incoming_save.sav", "size": 1000, "sha256": "x"}]))
        self.assertTrue(rx.failed)
        self.assertEqual(sent[-1], {"t": "fdone", "sid": 9, "ok": False, "final": True})
        rx2 = lobby._TerrReceiver(Wire().conn, type("IO", (), {"dir": _TMP.name})(), lambda s: None)
        rx2._send = sent.append
        rx2.on_begin(dict(base, sid=10, total_bytes=lobby.TERR_MAX_BYTES + 1, files=[{"name": lobby.TERR_NAME}]))
        self.assertEqual(sent[-1]["ok"], False)
        rx3 = lobby._TerrReceiver(Wire().conn, type("IO", (), {"dir": _TMP.name})(), lambda s: None)
        rx3.on_begin(dict(base, kind="save", files=[{"name": lobby.TERR_NAME}]))
        self.assertIsNone(rx3.sid, "only a terr fbegin is taken")

    def test_host_sends_only_a_current_sidecar(self):
        d = Path(_TMP.name) / "saves"
        d.mkdir(exist_ok=True)
        sav, terr = d / "w.sav", d / "w.terr"
        sav.write_bytes(b"save")
        self.assertIsNone(lobby._terr_for_save(str(sav)))                 # none
        terr.write_bytes(sidecar(100))
        self.assertEqual(lobby._terr_for_save(str(sav)), str(terr))
        old = time.time() - 3600
        os.utime(terr, (old, old))
        self.assertIsNone(lobby._terr_for_save(str(sav)), "older than its save: an earlier version's")
        terr.write_bytes(b"NOPE" + bytes(100))
        self.assertIsNone(lobby._terr_for_save(str(sav)), "not a sidecar")
        self.assertIsNone(lobby._terr_for_save(str(d / "w.lua")))

    def test_a_second_start_leaves_a_running_stream_alone(self):
        """A START for a joiner already receiving this sidecar sends nothing new:
        a replacement left the joiner's game reading a stream that stopped half
        way, and it computed the terrain itself (2026-09-28)."""
        d = Path(_TMP.name) / "saves2"
        d.mkdir(exist_ok=True)
        terr = d / "w.terr"
        terr.write_bytes(sidecar(100))
        key = lobby._terr_key(str(terr))
        other = ("127.0.0.1", 40002)
        tx = lobby._HostSaveTransfer(Wire().sock, 77, b"x" * 100, [{"name": lobby.TERR_NAME, "size": 100, "sha256": "0" * 64}],
                                     [(JOINER, "joiner")], lobby._QuietIO(type("IO", (), {"dir": _TMP.name})()), lambda s: None,
                                     kind="terr", overall_sha="0" * 64)
        tx.terr_key = key
        self.assertTrue(lobby._terr_streaming([tx], key, JOINER), "same file, still sending: leave it")
        self.assertFalse(lobby._terr_streaming([tx], key, other), "a joiner not in it gets the file")
        time.sleep(0.02)
        terr.write_bytes(sidecar(120))                       # saved again under the same name
        self.assertNotEqual(lobby._terr_key(str(terr)), key)
        self.assertFalse(lobby._terr_streaming([tx], lobby._terr_key(str(terr)), JOINER), "a new file is a new load")
        tx.peers[JOINER]["state"] = "done"
        self.assertFalse(lobby._terr_streaming([tx], key, JOINER), "a finished stream is not held for")


if __name__ == "__main__":
    unittest.main(verbosity=2)
