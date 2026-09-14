"""Exercise real host admission and client rejection without launching the game."""
import collections
import json
import pathlib
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "netpunch"))
import lobby


class VersionGateTest(unittest.TestCase):
    def test_host_admission(self):
        for relay_only in (False, True):
            with self.subTest(relay_only=relay_only), tempfile.TemporaryDirectory() as root:
                io = lobby.LobbyIO(root)
                stop = threading.Event()
                sock = lobby.open_socket(0, socket.AF_INET)
                port = sock.getsockname()[1]
                worker = threading.Thread(target=lobby.run_host,
                    args=(sock, "host", io), kwargs={"stop": stop, "relay_only": relay_only,
                                                    "log": lambda _: None})
                worker.start()
                conn = None
                try:
                    conn = lobby._dial_loopback(0, port, 5)
                    self.assertIsNotNone(conn)
                    for version in (None, "0.4.22", "99.0.0", 23, {}, lobby.LOBBY_VERSION):
                        msg = {"t": "join", "name": "tester"}
                        if version is not None:
                            msg["version"] = version
                        conn.send(json.dumps(msg).encode())
                        deadline = time.monotonic() + 3
                        response = None
                        while time.monotonic() < deadline:
                            raw = conn.recv(timeout=0.1)
                            if raw:
                                event = json.loads(raw)
                                if event.get("t") in ("welcome", "reject"):
                                    response = event
                                    break
                        self.assertIsNotNone(response, version)
                        if version == lobby.LOBBY_VERSION:
                            self.assertEqual(response["t"], "welcome")
                            self.assertEqual(response["version"], lobby.LOBBY_VERSION)
                        else:
                            self.assertEqual(response["t"], "reject")
                            self.assertIn("version mismatch", response["reason"])
                            roster = lobby._latest_roster(io.out_path)
                            self.assertNotIn("tester", (roster or {}).get("players", []))
                finally:
                    if conn:
                        conn.close()
                    stop.set()
                    worker.join(5)
                    self.assertFalse(worker.is_alive())

    def test_client_rejects_before_processing_game_or_save(self):
        for kind in ("welcome", "roster"):
            for version in (None, "0.4.22", "99.0.0", [], False):
                with self.subTest(kind=kind, version=version), tempfile.TemporaryDirectory() as root:
                    greeting = {"t": kind, "version": version, "host": "host",
                                "you": "tester", "players": ["host", "tester"], "started": True}
                    class Connection:
                        def __init__(self):
                            self.messages = collections.deque(json.dumps(m).encode() for m in (
                                {"t": "fbegin"}, {"t": "start", "save": False}, greeting,
                                {"t": "start", "save": False}))
                            self.sent = []
                            self.closed = False
                        def send(self, raw): self.sent.append(json.loads(raw))
                        def recv(self, timeout=0): return self.messages.popleft() if self.messages else None
                        def last_seen_age(self): return 0
                        def close(self): self.closed = True
                    class Receiver:
                        def __init__(self, *args): pass
                        def active(self): return False
                        def on_begin(self, msg): raise AssertionError("save accepted before version check")
                    conn = Connection()
                    io = lobby.LobbyIO(root)
                    lobby.run_client(conn, "tester", io, receiver_cls=Receiver, log=lambda _: None)
                    events = lobby._read_events(io.out_path)
                    self.assertFalse(any(e["type"] in ("start", "roster", "save_ready") for e in events))
                    self.assertFalse(any(e.get("state") == "connected" for e in events))
                    self.assertIn("version mismatch", events[-1]["detail"])
                    self.assertEqual(json.loads(pathlib.Path(io.state_path).read_text())["state"], "failed")
                    self.assertTrue(conn.closed)
                    self.assertTrue(any(m.get("t") == "leave" for m in conn.sent))
                    self.assertEqual(conn.sent[0]["version"], lobby.LOBBY_VERSION)


if __name__ == "__main__":
    unittest.main()
