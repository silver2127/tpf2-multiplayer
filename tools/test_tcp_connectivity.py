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
