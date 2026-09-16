"""The lobby imposes no content limits: big control messages are fragmented and
reassembled (through sealed frames too), a save's mod list is read whole
(any count, any folder name, any version) and an unreadable one is UNKNOWN
rather than "no mods", a joiner busy verifying a save is neither dropped by
the host nor timed out by the transfer, and the verify/write runs off the
lobby loop so the joiner keeps pinging.

    python tools/test_lobby_limits.py
"""
import hashlib
import inspect
import json
import os
import socket
import struct
import sys
import types
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby                                                 # noqa: E402
import modshare                                              # noqa: E402
from seal import Sealer, derive_key                          # noqa: E402
from sync_lobby import HostRecovery, ClientRecovery          # noqa: E402
from sync_operation import SILENCE                           # noqa: E402


def big_roster(players):
    """A roster like run_host sends: N players, N profiles, N link lists (N^2)."""
    names = [f'player{i:03d}' for i in range(players)]
    return {"t": "roster", "version": lobby.LOBBY_VERSION, "players": names,
            "profiles": {n: "P" + "x" * 90 for n in names},
            "links": {n: [m for m in names if m != n] for n in names},
            "companies": {n: 1 for n in names}}


class Fragmentation(unittest.TestCase):
    def test_small_message_is_sent_as_plain_json(self):
        payload = json.dumps({"t": "ping"}).encode()
        self.assertEqual(lobby._fragments(payload), [payload])
        payload = b"x" * lobby.FRAG_DATA
        self.assertEqual(lobby._fragments(payload), [payload])

    def test_roster_of_a_full_lobby_round_trips(self):
        # 200 players (CAP): profiles + N^2 links. This crossed 64 KB at 60-70
        # players and sendto raised WSAEMSGSIZE, silently.
        msg = big_roster(lobby.CAP)
        payload = json.dumps(msg).encode()
        self.assertGreater(len(payload), 65535)
        pieces = lobby._fragments(payload)
        self.assertGreater(len(pieces), 50)
        self.assertTrue(all(len(p) <= 1 + lobby.FRAG_HEADER.size + lobby.FRAG_DATA for p in pieces))
        self.assertTrue(all(p[:1] == lobby.FRAG_MAGIC for p in pieces))
        r = lobby._Reassembler()
        addr = ("10.0.0.1", 5000)
        out = [r.feed(addr, p) for p in pieces]
        self.assertEqual([o for o in out if o is not None], [payload])
        self.assertEqual(json.loads(payload.decode()), msg)
        self.assertEqual(r.pending, {})

    def test_out_of_order_duplicates_and_interleaving(self):
        a = json.dumps({"t": "a", "pad": "a" * 5000}).encode()
        b = json.dumps({"t": "b", "pad": "b" * 7000}).encode()
        pa, pb = lobby._fragments(a), lobby._fragments(b)
        r = lobby._Reassembler()
        addr = ("10.0.0.1", 5000)
        got = []
        for piece in [pa[2], pb[0], pa[2], pa[0], pb[3], pb[1], pb[2], pa[1], pa[3], pb[4], pb[5]]:
            out = r.feed(addr, piece)
            if out is not None:
                got.append(out)
        self.assertEqual(got, [a, b])

    def test_senders_are_kept_apart_and_stale_pieces_expire(self):
        a = json.dumps({"t": "a", "pad": "a" * 5000}).encode()
        pieces = lobby._fragments(a)
        r = lobby._Reassembler()
        self.assertIsNone(r.feed(("1.1.1.1", 1), pieces[0], now=100.0))
        self.assertIsNone(r.feed(("2.2.2.2", 1), pieces[1], now=100.0))   # another sender, same id
        self.assertIsNone(r.feed(("1.1.1.1", 1), pieces[1], now=101.0))
        r.expire(now=101.0 + lobby.FRAG_TTL + 1)
        self.assertEqual(r.pending, {})                                  # both abandoned
        r.forget(("1.1.1.1", 1))

    def test_garbage_guard_is_loud_and_never_hits_a_real_sender(self):
        lines = []
        r = lobby._Reassembler(log=lines.append)
        addr = ("10.0.0.1", 5000)
        for fid in range(lobby.FRAG_PENDING_PER_ADDR + 1):
            r.feed(addr, lobby.FRAG_MAGIC + lobby.FRAG_HEADER.pack(fid, 0, 2) + b"x", now=float(fid))
        self.assertEqual(len([k for k in r.pending if k[0] == addr]), lobby.FRAG_PENDING_PER_ADDR)
        self.assertTrue(lines and "unfinished messages" in lines[0], lines)
        # malformed pieces are ignored, not crashed on
        self.assertIsNone(r.feed(addr, lobby.FRAG_MAGIC + b"\x00"))
        self.assertIsNone(r.feed(addr, lobby.FRAG_MAGIC + lobby.FRAG_HEADER.pack(1, 5, 2) + b"x"))
        self.assertIsNone(r.feed(addr, lobby.FRAG_MAGIC + lobby.FRAG_HEADER.pack(1, 0, 0) + b"x"))

    def test_fragments_cross_a_sealed_session(self):
        # each fragment is sealed on its own; a second member with the same key opens them
        key = derive_key(b"s" * 12, "pw")
        alice, bob = Sealer(key), Sealer(key)
        msg = big_roster(120)
        payload = json.dumps(msg).encode()
        r = lobby._Reassembler()
        got = None
        for piece in lobby._fragments(payload):
            plain = bob.open(alice.seal(piece))
            self.assertIsNotNone(plain)
            out = r.feed(("h", 1), plain)
            if out is not None:
                got = out
        self.assertEqual(json.loads(got.decode()), msg)

    def test_send_data_fragments_and_logs_failures(self):
        class Sock:
            def __init__(self): self.sent = []; self.fail = None
            def sendto(self, data, addr):
                if self.fail: raise self.fail
                self.sent.append(data)
        s = Sock()
        lobby._send_data(s, ("10.0.0.9", 1), big_roster(100))
        self.assertGreater(len(s.sent), 20)
        self.assertTrue(all(len(d) < 1500 for d in s.sent))
        s.fail = OSError(10040, "WSAEMSGSIZE")
        lines = []
        lobby._send_failures.clear()
        lobby._send_data(s, ("10.0.0.9", 1), {"t": "ping"}, log=lines.append)
        self.assertEqual(len(lines), 1)
        self.assertIn("WSAEMSGSIZE", lines[0])
        lobby._send_data(s, ("10.0.0.9", 1), {"t": "ping"}, log=lines.append)   # rate-limited
        self.assertEqual(len(lines), 1)

    def test_host_handle_data_reassembles_a_joiner_message(self):
        # the host-side entry point, driven the way run_host drives it
        peers = {}
        seen = []
        frags = lobby._Reassembler()
        def handle(addr, payload):
            if payload[:1] == lobby.FRAG_MAGIC:
                payload = frags.feed(addr, payload)
                if payload is None:
                    return
            seen.append(json.loads(payload.decode()))
        need = {"t": "mods_request", "need": [f"Big Mod Pack {i}_{i}" for i in range(2000)]}
        for piece in lobby._fragments(json.dumps(need).encode()):
            handle(("1.2.3.4", 1), piece)
        self.assertEqual(seen, [need])
        self.assertEqual(peers, {})


class ModList(unittest.TestCase):
    def encode(self, mods):
        body = struct.pack("<I", len(mods))
        for m, v in mods:
            raw = m.encode("utf-8")
            body += struct.pack("<I", len(raw)) + raw + struct.pack("<I", v)
        return body + struct.pack("<I", 5) + modshare.SETTINGS_ANCHOR + b"\x09\x00\x00\x00temperate"

    def test_150_mods_with_spaces_in_ids(self):
        mods = [(f"Great Mod {i} (fixed)", 100000 + i) for i in range(150)]
        head = b"\xc7\xa9\xb9" * 2000 + self.encode(mods) + b"\x00" * 100
        self.assertEqual(modshare.parse_mod_list(head), mods)
        for m, v in mods:
            self.assertTrue(modshare.valid_mod(m, v), (m, v))
            self.assertEqual(modshare.parse_mod_zip_name(modshare.mod_zip_name(m, v)), (m, v))
            self.assertTrue(lobby._safe_incoming_name(modshare.mod_zip_name(m, v)))

    def test_unicode_long_and_workshop_ids_and_any_u32_version(self):
        mods = [("Straßenbahn München", 1), ("東京の電車", 42), ("x" * 1000, 0),
                ("*123456789012345678901234", 3), ("plain", modshare.MAX_VERSION)]
        self.assertEqual(modshare.parse_mod_list(b"\x00" * 50 + self.encode(mods)), mods)

    def test_empty_list_is_a_list_not_unknown(self):
        self.assertEqual(modshare.parse_mod_list(b"\x00" * 50 + self.encode([])), [])

    def test_a_list_longer_than_the_old_window(self):
        mods = [("m" * 200 + str(i), i) for i in range(500)]        # ~105 KB, was a 16 KB window
        self.assertEqual(modshare.parse_mod_list(b"\x00" * 50 + self.encode(mods)), mods)

    def test_no_folder_can_be_called_that(self):
        for bad in ("a/b", "a\\b", "c:d", 'q"q', "a<b", "a>b", "a|b", "a?b", "a*b", "*12a", "\x01", "", ".", ".."):
            self.assertFalse(modshare.valid_mod(bad, 1), bad)
        mods = [("ok", 1), ("bad/name", 1)]
        self.assertIsNone(modshare.parse_mod_list(b"\x00" * 50 + self.encode(mods)))

    def test_unreadable_save_is_unknown_never_empty(self):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "x.sav")
            with open(p, "wb") as f:
                f.write(os.urandom(4096))                              # not a zstd stream
            lines = []
            self.assertIsNone(modshare.save_mod_list(p, lines.append))
            self.assertTrue(lines and "cannot read the mod list" in lines[0], lines)
            self.assertIsNone(modshare.save_mod_list(os.path.join(td, "missing.sav"), lines.append))
            self.assertEqual(len(lines), 2)

    def test_real_save_layout_streams_past_six_megabytes(self):
        zstd = None
        try:
            import zstandard as zstd
        except ImportError:
            self.skipTest("zstandard not installed")
        mods = [("Deep List Mod", 9)]
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "deep.sav")
            data = bytes(7 * 1024 * 1024) + self.encode(mods) + bytes(1024)   # anchor 7 MB in
            with open(p, "wb") as f:
                f.write(zstd.ZstdCompressor().compress(data))
            self.assertEqual(modshare.save_mod_list(p), mods)

    def test_host_transfer_tells_receivers_the_list_is_unknown(self):
        class IO:
            def emit(self, e): pass
        t = lobby._HostSaveTransfer(None, 1, b"", [], [], IO(), lambda s: None, mods=None)
        self.assertTrue(t.begin_msg["mods_unknown"])
        self.assertEqual(t.begin_msg["mods"], [])
        t = lobby._HostSaveTransfer(None, 1, b"", [], [], IO(), lambda s: None, mods=[("a", 1)])
        self.assertFalse(t.begin_msg["mods_unknown"])

    def test_receiver_treats_unknown_manifest_as_a_notice_not_as_no_mods(self):
        class Conn:
            def __init__(self): self.sent = []
            def send(self, raw): self.sent.append(json.loads(raw))
        class IO:
            def __init__(self, d): self.dir = d; self.events = []
            def emit(self, e): self.events.append(e)
        with tempfile.TemporaryDirectory() as td:
            r = lobby._ClientSaveReceiver(Conn(), IO(td), lambda s: None)
            r.on_manifest(None)
            self.assertFalse(r.cancelled)
            self.assertTrue(r.manifest_unknown)
            r.on_manifest([], True)
            self.assertEqual(len([e for e in r.io.events if e["type"] == "chat"]), 1)   # said once
            r.on_manifest([[f"Mod {i}", i] for i in range(300)])                        # 300 > the old 128 cap
            self.assertFalse(r.cancelled)
            self.assertEqual(len(r.manifest), 300)
            r.on_manifest("garbage")
            self.assertTrue(r.cancelled)

    def test_settings_anchor_short_of_its_tail_is_not_lost(self):
        """The anchor lands 20 B before the end of a decompressed chunk, so its
        64 B tail is not in yet. The next search must start AT the anchor:
        starting past it never saw it again and decompressed the whole save
        into memory (a repro returned the entire stream, 2026-09-16)."""
        anchor = modshare.SETTINGS_ANCHOR
        plain = bytearray(os.urandom(5000))
        at = 980
        plain[at:at + len(anchor)] = anchor
        class Obj:
            def __init__(self): self.pos = 0
            def decompress(self, chunk):
                out = bytes(plain[self.pos:self.pos + 1000])
                self.pos += 1000
                return out
        fake = types.SimpleNamespace(ZstdDecompressor=lambda: types.SimpleNamespace(decompressobj=Obj),
                                     ZstdError=Exception)
        with tempfile.TemporaryDirectory() as td, patch.dict(sys.modules, {"zstandard": fake}), \
                patch.object(modshare, "DECOMPRESS_CHUNK", 1):
            path = os.path.join(td, "x.sav")
            with open(path, "wb") as f:
                f.write(b"z" * 5)                                   # five 1 B reads -> five 1000 B chunks
            out = modshare._decompress_until(path, anchor)
        self.assertEqual(len(out), at + len(anchor) + 64)
        self.assertEqual(out[at:at + len(anchor)], anchor)

    def test_registry_has_no_count_cap(self):
        with tempfile.TemporaryDirectory() as td, patch.object(modshare, "data_dir", return_value=td):
            for i in range(300):
                d = os.path.join(td, "workshop", str(1000000 + i))
                os.makedirs(d)
                with open(os.path.join(d, "mod.lua"), "w") as f:
                    f.write("x")
            token = modshare.request_catalogue()
            with open(os.path.join(td, "mods_registry.txt"), encoding="utf-8") as f:
                lines = f.read().splitlines()
            self.assertEqual(lines[0], token)
            self.assertEqual(len(lines), 301)


class MidTransfer(unittest.TestCase):
    """A joiner that has every chunk and is verifying/writing is neither
    dropped by the host's keepalive nor timed out by the transfer."""

    class IO:
        def __init__(self, d="."): self.dir = d; self.events = []
        def emit(self, e): self.events.append(e)

    class Sock:
        def __init__(self): self.sent = 0
        def sendto(self, data, addr): self.sent += 1

    def transfer(self, size=200000):
        blob = os.urandom(size)
        meta = [{"name": "incoming_save.sav", "size": size, "sha256": hashlib.sha256(blob).hexdigest()}]
        addr = ("10.0.0.5", 7)
        t = lobby._HostSaveTransfer(self.Sock(), 5, blob, meta, [(addr, "joiner")], self.IO(), lambda s: None)
        return t, addr

    def test_host_drop_defers_to_the_transfer(self):
        t, addr = self.transfer()
        self.assertTrue(lobby._mid_transfer(addr, t))
        self.assertTrue(lobby._mid_transfer(addr, None, t))
        self.assertFalse(lobby._mid_transfer(("10.0.0.6", 7), t))
        self.assertFalse(lobby._mid_transfer(addr, None))
        t.peers[addr]["state"] = "done"
        self.assertFalse(lobby._mid_transfer(addr, t))

    def test_host_eviction_loop_keeps_a_silent_mid_transfer_peer(self):
        """The keepalive sweep run_host runs once a second: a silent peer that
        is mid-transfer (in either transfer slot) is deferred and logged once,
        a silent idle peer is dropped, and the deferral ends with the transfer."""
        t, addr = self.transfer()
        other = ("10.0.0.9", 7)
        peers = {addr: {"name": "joiner", "last": 0.0}, other: {"name": "idle", "last": 0.0}}
        logged = []
        now = lobby.DROP_AFTER + 5
        self.assertEqual(lobby._keepalive_sweep(peers, now, lobby.DROP_AFTER, (t, None), logged.append), [other])
        self.assertTrue(peers[addr]["drop_deferred"])
        self.assertEqual(len(logged), 1)
        self.assertIn("mid-transfer", logged[0])
        # the recovery transfer slot counts the same, and the line is logged once
        self.assertEqual(lobby._keepalive_sweep(peers, now + 1, lobby.DROP_AFTER, (None, t), logged.append), [other])
        self.assertEqual(len(logged), 1)
        # heard again: the deferral clears
        peers[addr]["last"] = now + 1
        self.assertEqual(lobby._keepalive_sweep(peers, now + 1, lobby.DROP_AFTER, (t, None), logged.append), [other])
        self.assertNotIn("drop_deferred", peers[addr])
        # the transfer resolved: a silent peer is an ordinary silent peer
        t.peers[addr]["state"] = "done"
        peers[addr]["last"] = 0.0
        self.assertEqual(sorted(lobby._keepalive_sweep(peers, now + 30, lobby.DROP_AFTER, (t, None), logged.append)),
                         sorted([addr, other]))
        # and run_host's loop is that sweep, over both transfer slots
        self.assertIn("_keepalive_sweep(peers, now, drop_after", inspect.getsource(lobby.run_host))
        self.assertIn("(transfer[0], recovery.transfer if recovery else None)", inspect.getsource(lobby.run_host))

    def test_a_verifier_whose_count_stops_moving_times_out(self):
        """A joiner that has every chunk reports its verify/write count in
        each fack. A moving count keeps it alive however long the work takes;
        a count that stops (a disk hang, a wedged unzip) is no progress and
        the transfer's timeout resolves the peer instead of holding transfer[0]
        -- and every resync -- open for ever."""
        t, addr = self.transfer()
        p = t.peers[addr]
        total = t.total_chunks
        def fack(t_, a, progress):
            t_.on_fack(a, {"t": "fack", "sid": 5, "base": total, "nack": [], "verifying": True, "progress": progress})
        fack(t, addr, 0)
        fack(t, addr, 8 << 20)
        self.assertEqual(p["verify_progress"], 8 << 20)
        p["last_advance"] = time.time() - lobby.PEER_XFER_TIMEOUT - 5
        fack(t, addr, 8 << 20)                            # the worker stopped moving
        t.pump(time.time())
        self.assertEqual(p["state"], "failed")
        t2, addr2 = self.transfer()
        p2 = t2.peers[addr2]
        for i in range(4):                                # a moving count: alive past the timeout, every time
            p2["last_advance"] = time.time() - lobby.PEER_XFER_TIMEOUT - 5
            fack(t2, addr2, i * 4096)
            t2.pump(time.time())
            self.assertEqual(p2["state"], "active")
        # the barrier hears the same, per receiving member
        tokens = dict(t2.progress_tokens())
        self.assertEqual(list(tokens), ["joiner"])
        self.assertEqual(tokens["joiner"], "transfer:%d/%d/active" % (total, 3 * 4096))
        t2.on_fdone(addr2, {"t": "fdone", "sid": 5, "ok": True})
        self.assertEqual(dict(t2.progress_tokens())["joiner"], "transfer:%d/%d/done" % (total, 3 * 4096))

    def test_verifying_facks_are_progress_for_the_transfer_timeout(self):
        t, addr = self.transfer()
        p = t.peers[addr]
        p["last_advance"] = time.time() - lobby.PEER_XFER_TIMEOUT - 5      # would time out now
        t.on_fack(addr, {"t": "fack", "sid": 5, "base": t.total_chunks, "nack": [], "verifying": True})
        self.assertGreater(p["last_advance"], time.time() - 1)
        before = t.progress_at
        time.sleep(0.01)
        t.on_fack(addr, {"t": "fack", "sid": 5, "base": t.total_chunks, "nack": [], "verifying": True})
        self.assertGreater(t.progress_at, before)
        t.pump(time.time())
        self.assertEqual(p["state"], "active")
        # a receiver that is NOT done and NOT advancing still times out
        t2, addr2 = self.transfer()
        p2 = t2.peers[addr2]
        t2.on_fack(addr2, {"t": "fack", "sid": 5, "base": 1, "nack": [2]})
        p2["last_advance"] = time.time() - lobby.PEER_XFER_TIMEOUT - 5
        t2.on_fack(addr2, {"t": "fack", "sid": 5, "base": 1, "nack": [2]})   # same base: no progress
        t2.pump(time.time())
        self.assertEqual(p2["state"], "failed")

    def test_receiver_verifies_off_the_loop_and_keeps_facking(self):
        class Conn:
            def __init__(self): self.sent = []
            def send(self, raw): self.sent.append(json.loads(raw))
        with tempfile.TemporaryDirectory() as td:
            conn = Conn()
            r = lobby._ClientSaveReceiver(conn, self.IO(td), lambda s: None)
            blob = os.urandom(3 * lobby.CHUNK_LOCAL + 17)
            meta = [{"name": "incoming_save.sav", "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}]
            chunks = (len(blob) + lobby.CHUNK_LOCAL - 1) // lobby.CHUNK_LOCAL
            with patch.object(lobby, "_save_has_mp_mod", return_value=True):
                r.on_begin({"sid": 9, "kind": "save", "files": meta, "total_bytes": len(blob),
                            "total_chunks": chunks, "chunk": lobby.CHUNK_LOCAL,
                            "sha256": hashlib.sha256(blob).hexdigest(), "mods": []})
                # hold the worker: the loop must keep running while it works
                gate = {"go": False}
                real = r._finalize_work
                def slow(job):
                    while not gate["go"]:
                        time.sleep(0.005)
                    real(job)
                r._finalize_work = slow
                for seq in range(chunks):
                    r.on_chunk(9, seq, blob[seq * lobby.CHUNK_LOCAL:(seq + 1) * lobby.CHUNK_LOCAL])
                self.assertTrue(r.finalizing)
                self.assertFalse(r.complete)
                self.assertTrue(r.active())
                r.on_chunk(9, 0, blob[:lobby.CHUNK_LOCAL])                  # a retransmit: ignored
                r.tick(time.time() + 1)
                facks = [m for m in conn.sent if m["t"] == "fack"]
                self.assertTrue(facks and facks[-1]["base"] == chunks and facks[-1]["verifying"] is True)
                self.assertEqual(facks[-1]["progress"], 0)                  # the worker has not started
                r.on_begin({"sid": 10, "kind": "save", "files": meta, "total_bytes": 1, "total_chunks": 1,
                            "chunk": 1, "sha256": "0" * 64, "mods": []})   # deferred until the worker is done
                self.assertEqual(r.sid, 9)
                gate["go"] = True
                r.settle()
            self.assertFalse(r.finalizing)
            self.assertTrue(r.complete and r.save_done)
            # the count the facks carry moved through both hashes and the write
            self.assertEqual(r.finalize_progress, 3 * len(blob))
            with open(os.path.join(td, "incoming_save.sav"), "rb") as f:
                self.assertEqual(f.read(), blob)
            self.assertIn({"type": "save_ready", "name": "incoming_save", "dir": os.path.abspath(td),
                           "files": ["incoming_save.sav"]}, r.io.events)
            self.assertTrue(any(m["t"] == "fdone" and m["ok"] for m in conn.sent))

    def test_hash_mismatch_still_retries_from_the_worker(self):
        class Conn:
            def __init__(self): self.sent = []
            def send(self, raw): self.sent.append(json.loads(raw))
        with tempfile.TemporaryDirectory() as td:
            conn = Conn()
            r = lobby._ClientSaveReceiver(conn, self.IO(td), lambda s: None)
            blob = os.urandom(100)
            meta = [{"name": "incoming_save.sav", "size": 100, "sha256": "f" * 64}]
            r.on_begin({"sid": 3, "kind": "save", "files": meta, "total_bytes": 100, "total_chunks": 1,
                        "chunk": 100, "sha256": hashlib.sha256(blob).hexdigest(), "mods": []})
            r.on_chunk(3, 0, blob)
            r.settle()
            self.assertEqual(r.retries, 1)
            self.assertFalse(r.complete or r.failed)
            self.assertEqual(r.base, 0)                                     # re-requested


class SendFailures(unittest.TestCase):
    """A joiner's control message the socket refused is never silent: the
    punch connection raises it and the lobby's send wrappers log it."""

    def test_a_joiner_send_failure_is_raised_and_logged(self):
        import punch
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        conn = punch.Connection(sock, [], listen=True)
        try:
            class Refusing:
                family = socket.AF_INET
                def fileno(self):
                    return sock.fileno()                              # the punch thread's select keeps working
                def sendto(self, data, addr):
                    raise OSError(10040, "WSAEMSGSIZE")
            conn.sock = Refusing()
            conn.peer = ("127.0.0.1", 9)
            conn._send_to(punch.TYPE_HELLO, b"", conn.peer)           # punch traffic: retried, never raised
            with self.assertRaises(OSError):
                conn.send(b'{"t": "join"}')
            logged = []
            r = lobby._ClientSaveReceiver(conn, MidTransfer.IO(), logged.append)
            r._send({"t": "fack", "sid": 1})
            self.assertTrue(any("send of" in line and "failed" in line for line in logged), logged)
        finally:
            conn.sock = sock
            conn.close()


class ProgressPlumbing(unittest.TestCase):
    """The host counts a member's sync_progress and its own transfer's
    progress; a joiner reports progress while a silence-timed phase runs."""

    def test_host_feeds_member_and_transfer_progress_to_the_barrier(self):
        from unittest.mock import Mock
        members = ['host', 'p1']
        io, runtime = Mock(), Mock(state=None)
        runtime._read.return_value = {}
        runtime.progress.return_value = None
        h = HostRecovery(runtime, 'host', io, Mock(), lambda: members, lambda: [], Mock())
        h.command('host', dict(cmd='sync_request', id='r1'))
        self.assertEqual(h.barrier.phase, 'holding')
        h.barrier.phase = 'transferring'
        ids = {k: getattr(h.barrier, k) for k in ('operation', 'revision', 'epoch', 'phase')}
        h.barrier.deadline = 0
        self.assertTrue(h.command('p1', dict(ids, t='sync_progress', progress='recv=1')))
        self.assertGreater(h.barrier.deadline, 0)
        h.barrier.deadline = 0
        h._local_progress('transfer:1', 'p1')               # the transfer's progress, per receiver
        self.assertGreater(h.barrier.deadline, 0)
        h.barrier.deadline = 0
        h._local_progress('transfer:1', 'p1')               # same token: no
        self.assertEqual(h.barrier.deadline, 0)
        h._local_progress(Mock(), 'p1')                     # a non-string never counts
        self.assertEqual(h.barrier.deadline, 0)
        # the host's own engine token counts only until the host has acked
        h._local_progress('transferring:engine:cpu_ui=1')
        self.assertGreater(h.barrier.deadline, 0)
        h.barrier.deadline = 0
        h.barrier.acks['host'] = {}
        h._local_progress('transferring:engine:cpu_ui=2')
        self.assertEqual(h.barrier.deadline, 0)
        h._local_progress('transfer:2', 'p1')               # a receiver still working does
        self.assertGreater(h.barrier.deadline, 0)
        # tick feeds every receiver's transfer token to the barrier under that member's name
        h.barrier.deadline = None
        h.barrier.snapshot = None
        transfer = Mock(progress_tokens=lambda: [('p1', 'transfer:5//active')], failed_names=lambda: [])
        h.transfer, h.transfer_epoch = transfer, h.barrier.epoch
        runtime.accept.return_value = False
        runtime.tick.return_value = None
        h.tick(1000.0)
        self.assertGreater(h.barrier.deadline, 0)
        self.assertEqual(h.barrier.progress_seen.get('p1'), 'transfer:5//active')

    def test_client_reports_progress_once_a_second_until_acked(self):
        from unittest.mock import Mock
        sent = []
        runtime = Mock(state={'operation': 'o', 'revision': 3, 'epoch': 'e', 'phase': 'loading'})
        runtime._read.return_value = {}
        runtime.tick.return_value = None
        runtime.progress.return_value = 'loading:engine:cpu_ui=1'
        receiver = Mock(recv_count=0, finalizing=False, complete=False)
        c = ClientRecovery(runtime, Mock(), sent.append, receiver)
        c.tick(100.0)
        c.tick(100.5)
        c.tick(101.0)
        prog = [m for m in sent if m.get('t') == 'sync_progress']
        self.assertEqual(len(prog), 2)
        self.assertEqual(prog[0]['phase'], 'loading')
        self.assertIn('loading:engine:cpu_ui=1', prog[0]['progress'])
        runtime.state['phase'] = 'holding'                 # a fixed-wait phase: nothing to report
        c.tick(103.0)
        self.assertEqual(len([m for m in sent if m.get('t') == 'sync_progress']), 2)
        self.assertEqual(set(SILENCE), {'saving', 'transferring', 'loading'})


if __name__ == '__main__':
    unittest.main()
