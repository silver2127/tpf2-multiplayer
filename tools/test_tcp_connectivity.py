"""Real IPv4/IPv6 streams and independent router-mapping regressions.

Run: python tools/test_tcp_connectivity.py
Router calls are mocked; this test never changes a real router.
"""
import errno
import hashlib
import os
import queue
from pathlib import Path
import socket
import sys
import threading
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
import bulk_tcp
import observe
import lobby


class TcpConnectivity(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux bound dial contract")
    def test_listener_allows_bound_dial_port(self):
        listener = bulk_tcp.BulkListener(0)
        self.addCleanup(listener.close)
        for endpoint in listener.sockets:
            self.assertEqual(endpoint.getsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT), 1)
            dial = socket.socket(endpoint.family, socket.SOCK_STREAM)
            self.addCleanup(dial.close)
            dial.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            dial.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            if endpoint.family == socket.AF_INET6:
                dial.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            dial.bind(endpoint.getsockname())

    def test_receiver_uses_senders_stream_when_both_ends_dial(self):
        receiver = lobby._ClientSaveReceiver.__new__(lobby._ClientSaveReceiver)
        receiver.sid, receiver.kind, receiver.total_bytes = 7, "save", 6
        receiver._progress_lock = threading.Lock()
        receiver._tcp_q = queue.Queue()
        receiver.log = lambda *args: None
        receiver.io = types.SimpleNamespace(emit=lambda event: None)
        losing, rejected_sender = socket.socketpair()
        winning, chosen_sender = socket.socketpair()
        entered = threading.Event()

        class ObservedSocket:
            def __getattr__(self, key):
                return getattr(losing, key)

            def recv(self, size):
                entered.set()
                return losing.recv(size)

        for s in (losing, rejected_sender, winning, chosen_sender):
            self.addCleanup(s.close)
        worker = threading.Thread(target=receiver._tcp_read, args=(ObservedSocket(), 7), daemon=True)
        worker.start()
        self.assertTrue(entered.wait(2))
        chosen_sender.sendall(b"ABCDEF")
        chosen_sender.close()
        receiver._tcp_read(winning, 7)
        rejected_sender.close()
        worker.join(2)
        self.assertFalse(worker.is_alive())
        chunks = []
        while not receiver._tcp_q.empty():
            sid, chunk = receiver._tcp_q.get_nowait()
            if chunk is not None:
                chunks.append(chunk)
        self.assertEqual(b"".join(chunks), b"ABCDEF")

    def _begin_through(self, peer, tcp):
        """on_begin of a small save from `peer`; returns the addresses _tcp_pull dialled."""
        dialled = []
        conn = types.SimpleNamespace(peer=peer, send=lambda *a, **k: None, sock=None)
        receiver = lobby._ClientSaveReceiver(conn, types.SimpleNamespace(emit=lambda event: None, write_state=lambda **k: None),
                                             lambda *args: None)
        receiver._send = lambda msg: None
        receiver._tcp_pull = lambda ip, *rest: dialled.append(ip)
        msg = {"t": "fbegin", "sid": 5, "kind": "save", "total_bytes": 6, "total_chunks": 1, "chunk": 1350,
               "files": [{"name": "incoming_save.sav", "size": 6}], "mods": [], "tcp": tcp}
        with patch.object(lobby, "BULK_TCP", [True]), \
             patch.object(threading, "Thread", lambda target, args, **k: types.SimpleNamespace(start=lambda: target(*args))):
            receiver.on_begin(msg)
        return dialled

    def test_relayed_join_dials_the_host_never_the_relay(self):
        # 2026-09-26: a joiner in through the master's UDP relay dialled TCP at the
        # relay's address with the host's port, three times, before taking UDP
        relay = ("76.13.109.1", 29600)
        with patch.object(lobby, "MASTER_RELAY_ADDRS", {relay}):
            self.assertTrue(lobby._is_master_relay(relay))
            self.assertFalse(lobby._is_master_relay(("76.13.109.1", 29471)))
            named = self._begin_through(relay, {"port": 29471, "token": "t", "addrs": ["203.0.113.5"]})
            self.assertEqual(named, [["203.0.113.5"]])          # the host's own address, not the relay
            self.assertEqual(self._begin_through(relay, {"port": 29471, "token": "t"}), [])   # none named: UDP at once
        # a direct join still dials the address the lobby came from
        self.assertEqual(self._begin_through(("198.51.100.7", 29471), {"port": 29471, "token": "t"}), ["198.51.100.7"])

    def test_host_advertises_tcp_addresses_to_relayed_targets(self):
        relay = ("192.0.2.1", 29600)
        direct = ("198.51.100.7", 29471)
        addresses = ["203.0.113.5", "2001:db8::5"]
        listener = types.SimpleNamespace(port=29471, expect=lambda *args: None)
        with patch.object(lobby, "MASTER_RELAY_ADDRS", {relay}), \
             patch.object(lobby, "BULK_TCP", [True]), \
             patch.object(lobby, "BULK", [listener]), \
             patch.object(lobby, "MY_TCP_ADDRS", [addresses]):
            for peers, advertised in (([relay], True), ([direct, relay], True), ([direct], False)):
                with self.subTest(peers=peers):
                    transfer = lobby._HostSaveTransfer(
                        None, 5, b"ABCDEF", [{"name": "incoming_save.sav", "size": 6}],
                        [(peer, str(i)) for i, peer in enumerate(peers)], None, lambda *args: None)
                    tcp = transfer.begin_msg["tcp"]
                    self.assertEqual(tcp["port"], listener.port)
                    self.assertTrue(tcp["token"])
                    if advertised:
                        self.assertEqual(tcp["addrs"], addresses)
                        self.assertIsNot(tcp["addrs"], addresses)
                    else:
                        self.assertNotIn("addrs", tcp)

    def test_master_pipe_pairs_by_id_and_role(self):
        import masterserver
        port = masterserver.start_pipe(0, "127.0.0.1")
        got = {}

        def end(pair, role, key):
            got[key] = bulk_tcp.pipe_connect("127.0.0.1", port, pair, role, wait=5)
        a, b = "a" * 32, "b" * 32
        ts = [threading.Thread(target=end, args=args) for args in ((a, "H", "ah"), (a, "J", "aj"), (b, "H", "bh"))]
        for t in ts:
            t.start()
        for t in ts:
            t.join(8)
        self.assertIsNotNone(got["ah"])
        self.assertIsNotNone(got["aj"])
        self.assertIsNone(got["bh"])                     # its joiner never came: no pairing with anyone else
        for s in (got["ah"], got["aj"]):
            self.addCleanup(s.close)
        got["ah"].sendall(b"host->joiner")
        got["aj"].sendall(b"joiner->host")
        self.assertEqual(bulk_tcp._read_exact(got["aj"], 12), b"host->joiner")
        self.assertEqual(bulk_tcp._read_exact(got["ah"], 12), b"joiner->host")

    def test_master_pipe_outlives_its_idle_limit_while_bytes_flow_one_way(self):
        # 2026-09-27: 15 pipe transfers ended at exactly 120 s (PIPE_IDLE) and none
        # ran longer -- 206, 195 and 529 MB saves among them. A save streams host ->
        # joiner only; the joiner's silent direction timed out 120 s after its hello
        # and, as the first direction to end, cut the busy one. Idle means no byte
        # EITHER way.
        import masterserver
        port = masterserver.start_pipe(0, "127.0.0.1")
        got = {}

        def end(role):
            got[role] = bulk_tcp.pipe_connect("127.0.0.1", port, "c" * 32, role, wait=5)
        with patch.object(masterserver, "PIPE_IDLE", 0.6):
            ts = [threading.Thread(target=end, args=(r,)) for r in ("H", "J")]
            for t in ts:
                t.start()
            for t in ts:
                t.join(8)
            h, j = got["H"], got["J"]
            self.assertIsNotNone(h)
            self.assertIsNotNone(j)
            for s in (h, j):
                self.addCleanup(s.close)
            chunk, n = b"x" * 1024, 12
            received = []

            def pull():
                j.settimeout(5)
                try:
                    received.append(bulk_tcp._read_exact(j, len(chunk) * n))
                except OSError as e:
                    received.append(e)
            reader = threading.Thread(target=pull)
            reader.start()
            for _ in range(n):                           # 12 x 0.25 s = 3 s, five idle limits
                h.sendall(chunk)
                threading.Event().wait(0.25)
            reader.join(8)
            self.assertEqual(received, [chunk * n])
            # and a pair that goes quiet both ways is still cut
            j.settimeout(3)
            self.assertEqual(j.recv(1), b"")

    def test_slow_transfer_streams_through_the_master_pipe(self):
        import masterserver
        port = masterserver.start_pipe(0, "127.0.0.1")
        blob = os.urandom(3 * 1024 * 1024 + 17)
        logs = []
        # the host's side: the sender with only what the pipe path touches
        host = lobby._HostSaveTransfer.__new__(lobby._HostSaveTransfer)
        host.sid, host.tcp_token, host.blob, host.kind = 9, "f" * 32, blob, "save"
        host.total_bytes, host.tcp_bytes, host._tcp_lock = len(blob), 0, threading.Lock()
        host.log, host.io = logs.append, types.SimpleNamespace(emit=lambda event: None)
        p = {"name": "Joiny", "state": "active", "tcp": False, "ready": True}
        # the joiner's side: a real receiver on the same transfer
        rx = lobby._ClientSaveReceiver(types.SimpleNamespace(peer=("76.13.109.1", 29600)),
                                       types.SimpleNamespace(emit=lambda event: None), logs.append)
        rx.sid, rx.kind, rx.total_bytes, rx._tcp_token, rx.my_name = 9, "save", len(blob), host.tcp_token, "Joiny"
        pair = os.urandom(16).hex()
        h = threading.Thread(target=host._pipe_serve, args=(p, pair, ("127.0.0.1", port)), daemon=True)
        h.start()
        # TCP recv sizes vary by platform: even 3 MiB can fill the bounded
        # queue. Consume while reading, as the real lobby loop does.
        j = threading.Thread(target=rx._pipe_pull,
                             args=("127.0.0.1", port, pair, 9, host.tcp_token), daemon=True)
        j.start()
        chunks = []
        while True:
            try:
                sid, chunk = rx._tcp_q.get(timeout=10)
            except queue.Empty:
                self.fail("pipe stopped producing chunks: " + "\n".join(map(str, logs)))
            self.assertEqual(sid, 9)
            if chunk is None:
                break
            self.assertNotEqual(chunk, b"", "pipe stream failed")
            chunks.append(chunk)
        h.join(10)
        j.join(10)
        self.assertFalse(h.is_alive() or j.is_alive(), "pipe workers did not finish")
        self.assertEqual(b"".join(chunks), blob, "\n".join(map(str, logs)))
        self.assertTrue(p["tcp"] and rx.tcp_active)

    def test_pipe_only_for_a_slow_transfer_and_once(self):
        sent, started = [], []
        host = lobby._HostSaveTransfer.__new__(lobby._HostSaveTransfer)
        host.sid, host.tcp_token, host.chunk, host.total_bytes, host.sock = 3, "t", 1350, 200 << 20, None
        host.log = lambda *a: None
        now = 1000.0

        def peer(done_bytes, ready_for):
            return {"name": "J", "tcp": False, "base": done_bytes // 1350, "ready_at": now - ready_for}
        with patch.object(lobby, "MASTER_PIPE", [("198.51.100.1", 29700)]), \
             patch.object(lobby, "_send_data", lambda sock, addr, msg: sent.append(msg)), \
             patch.object(threading, "Thread", lambda target, args, **k: types.SimpleNamespace(start=lambda: started.append(args))):
            slow = peer(6 << 20, 20)                   # 0.3 MB/s: ~11 min to go
            host._maybe_pipe(("198.51.100.9", 1), slow, now)
            host._maybe_pipe(("198.51.100.9", 1), slow, now + 5)        # once
            host._maybe_pipe(("198.51.100.9", 1), peer(180 << 20, 20), now)   # 9 MB/s: done in seconds
            host._maybe_pipe(("198.51.100.9", 1), peer(1 << 20, 5), now)      # too early to judge
            host.total_bytes = 8 << 20
            host._maybe_pipe(("198.51.100.9", 1), peer(1 << 20, 30), now)     # small: not worth a pipe
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0]["t"], "bulk_pipe")
        self.assertEqual((sent[0]["ip"], sent[0]["port"]), ("198.51.100.1", 29700))
        self.assertEqual(len(sent[0]["pair"]), 32)
        self.assertEqual(len(started), 1)
        with patch.object(lobby, "MASTER_PIPE", [None]):      # no pipe offered: nothing happens
            host.total_bytes = 200 << 20
            host._maybe_pipe(("198.51.100.9", 1), peer(6 << 20, 20), now)
        self.assertEqual(len(sent), 1)

    def test_both_families_both_directions_and_auth(self):
        listener = bulk_tcp.BulkListener(0)
        self.addCleanup(listener.close)
        # CI and the affected Windows machine have IPv6. Missing support must
        # fail this test, rather than silently claiming IPv6 was verified.
        self.assertEqual({s.family for s in listener.sockets}, {socket.AF_INET, socket.AF_INET6})
        payload = os.urandom(4 * 1024 * 1024 + 17)
        expected = hashlib.sha256(payload).digest()
        for host in ("127.0.0.1", "::1"):
            with self.subTest(host=host):
                listener.expect(1, "recv", "secret", lambda c, a, n: bulk_tcp.stream_send(c, payload))
                self.assertIsNone(bulk_tcp.bulk_connect(host, listener.port, "recv", 1, "wrong"))
                c = bulk_tcp.bulk_connect(host, listener.port, "recv", 1, "secret")
                self.assertIsNotNone(c)
                received = bytearray()
                self.assertTrue(bulk_tcp.stream_recv(c, len(payload), received.extend))
                self.assertEqual(hashlib.sha256(received).digest(), expected)
                done, received, results = threading.Event(), bytearray(), []

                def receive(c, addr, name):
                    results.append(bulk_tcp.stream_recv(c, len(payload), received.extend))
                    done.set()

                listener.expect(2, "send", "secret", receive)
                c = bulk_tcp.bulk_connect(host, listener.port, "send", 2, "secret")
                self.assertIsNotNone(c)
                self.assertTrue(bulk_tcp.stream_send(c, payload))
                self.assertTrue(done.wait(5))
                self.assertEqual(results, [True])
                self.assertEqual(hashlib.sha256(received).digest(), expected)
        listener.close()
        self.assertTrue(all(s.fileno() == -1 for s in listener.sockets))

    def test_ipv6_unavailable_keeps_ipv4(self):
        original = bulk_tcp.BulkListener._listen

        def no_v6(family, bind, port):
            if family == socket.AF_INET6:
                raise OSError(errno.EAFNOSUPPORT, "IPv6 disabled")
            return original(family, bind, port)

        logs = []
        with patch.object(bulk_tcp.BulkListener, "_listen", staticmethod(no_v6)):
            listener = bulk_tcp.BulkListener(0, logs.append)
        self.addCleanup(listener.close)
        self.assertEqual(len(listener.sockets), 1)
        listener.expect(1, "recv", "token", lambda c, a, n: c.close())
        c = bulk_tcp.bulk_connect("127.0.0.1", listener.port, "recv", 1, "token")
        self.assertIsNotNone(c)
        c.close()
        self.assertTrue(any("IPv4 remains available" in line for line in logs))

    def test_explicit_bind_is_not_widened(self):
        listener = bulk_tcp.BulkListener(0, bind="127.0.0.1")
        self.addCleanup(listener.close)
        self.assertEqual(len(listener.sockets), 1)
        self.assertEqual(listener.sock.getsockname()[0], "127.0.0.1")


class RouterMapping(unittest.TestCase):
    def test_cli_tcp_mapping_and_cleanup_use_linux_wrapper(self):
        calls = []
        def run(exe, args):
            calls.append(args)
            return types.SimpleNamespace(returncode=0, stdout="local lan ip address: 192.0.2.2\n")
        with patch.dict(sys.modules, {"miniupnpc": None}), patch.object(observe.shutil, "which", return_value="upnpc"), patch.object(observe, "_upnpc_run", side_effect=run):
            self.assertTrue(observe.upnp_map(23456, keep=True)["tcp_open"])
            self.assertTrue(observe.upnp_unmap(23456))
        self.assertEqual([a[-1] for a in calls], ["-l", "UDP", "TCP", "TCP", "UDP"])

    def mapping(self, udp, tcp, keep=True):
        calls = []

        def add(port, protocol, *args):
            calls.append(protocol)
            result = udp if protocol == "UDP" else tcp
            if isinstance(result, Exception):
                raise result
            return result

        router = types.SimpleNamespace(
            lanaddr="192.0.2.2", discover=lambda: 1, selectigd=lambda: None,
            externalipaddress=lambda: "198.51.100.1", addportmapping=add,
            deleteportmapping=lambda *args: None)
        with patch.dict(sys.modules, {"miniupnpc": types.SimpleNamespace(UPnP=lambda: router)}):
            return observe.upnp_map(23456, keep=keep), calls

    def test_tcp_attempted_when_udp_rejected_or_raises(self):
        for failure in (False, RuntimeError("UDP rejected")):
            result, calls = self.mapping(failure, True)
            self.assertEqual(calls, ["UDP", "TCP"])
            self.assertFalse(result["open"])
            self.assertTrue(result["tcp_open"])

    def test_udp_success_does_not_hide_tcp_failure(self):
        for failure in (False, RuntimeError("Invalid Action")):
            result, calls = self.mapping(True, failure)
            self.assertTrue(result["open"])
            self.assertFalse(result["tcp_open"])
            self.assertTrue(result["tcp_detail"])

    def test_temporary_udp_probe_does_not_leave_tcp_mapping(self):
        result, calls = self.mapping(True, True, keep=False)
        self.assertEqual(calls, ["UDP"])
        self.assertFalse(result["tcp_open"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
