"""TCP registration ownership regressions; stdlib only, loopback sockets.

Run: python3 tools/test_bulk_lifetime.py
"""
import gc
from pathlib import Path
import socket
import sys
import threading
import unittest
import weakref

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
import bulk_tcp


class Transfer:
    def __init__(self):
        self.blob = b"payload" * 150000

    def serve(self, conn, addr, name):
        bulk_tcp.stream_send(conn, self.blob)


class BulkLifetime(unittest.TestCase):
    def setUp(self):
        self.listener = bulk_tcp.BulkListener(0, bind="127.0.0.1")
        self.addCleanup(self.listener.close)

    def test_dropped_transfer_rejected_and_pruned(self):
        transfer = Transfer()
        self.listener.expect(1, "recv", "secret", transfer.serve)
        gone = weakref.ref(transfer)
        del transfer
        gc.collect()
        self.assertIsNone(gone(), "registration retains the file's owner")
        self.assertIsNone(bulk_tcp.bulk_connect(
            "127.0.0.1", self.listener.port, "recv", 1, "secret"))
        self.listener.expect(2, "recv", "next", lambda c, a, n: c.close())
        self.assertNotIn((1, "recv"), self.listener._expect)

    def test_plain_function_retained_and_streams(self):
        payload = b"plain function payload"

        def serve(conn, addr, name):
            bulk_tcp.stream_send(conn, payload)

        held = weakref.ref(serve)
        self.listener.expect(1, "recv", "secret", serve)
        del serve
        gc.collect()
        self.assertIsNotNone(held())
        self.assertIsNone(bulk_tcp.bulk_connect(
            "127.0.0.1", self.listener.port, "recv", 1, "wrong"))
        conn = bulk_tcp.bulk_connect("127.0.0.1", self.listener.port, "recv", 1, "secret")
        self.assertIsNotNone(conn)
        self.addCleanup(conn.close)
        conn.settimeout(5)
        got = bytearray()
        self.assertTrue(bulk_tcp.stream_recv(conn, len(payload), got.extend))
        self.assertEqual(got, payload)

    def test_accepted_handler_keeps_owner_until_return(self):
        entered, release = threading.Event(), threading.Event()

        class PausedTransfer(Transfer):
            def serve(self, conn, addr, name):
                entered.set()
                if release.wait(5):
                    super().serve(conn, addr, name)
                else:
                    conn.close()

        transfer = PausedTransfer()
        expected = transfer.blob
        gone = weakref.ref(transfer)
        self.listener.expect(1, "recv", "secret", transfer.serve)
        client, server = socket.socketpair()
        self.addCleanup(client.close)
        self.addCleanup(server.close)
        client.settimeout(5)
        # Run the real handshake on a joinable worker so completion, and thus
        # release of its local bound method, is deterministic.
        with self.listener._lock:
            self.listener._pending += 1
        worker = threading.Thread(target=self.listener._hello,
                                  args=(server, ("127.0.0.1", 0)), daemon=True)
        worker.start()
        try:
            client.sendall(b"TPF2BULK1 recv 1 secret peer\n")
            self.assertEqual(client.recv(3), b"OK\n")
            self.assertTrue(entered.wait(5))
            del transfer
            gc.collect()
            self.assertIsNotNone(gone(), "active handler lost its owner")
            release.set()
            got = bytearray()
            self.assertTrue(bulk_tcp.stream_recv(client, len(expected), got.extend))
            self.assertEqual(got, expected)
        finally:
            release.set()
            client.close()
            worker.join(5)
        self.assertFalse(worker.is_alive())
        gc.collect()
        self.assertIsNone(gone(), "completed handler retains its owner")


if __name__ == "__main__":
    unittest.main(verbosity=2)
