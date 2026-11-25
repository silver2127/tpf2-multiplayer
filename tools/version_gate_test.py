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
    def test_0717_keeps_the_0715_lobby(self):
        # 0.7.1.6 and 0.7.1.7 changed nothing on the wire or in the world, so they keep
        # the 0.7.1.5 lobby version: a 0.7.1.6 dedicated server or native Linux player
        # (0.7.1.7 has no Linux build) and 0.7.1.7 players play together. The "release"
        # field 0.7.1.7 adds is informational. Bump LOBBY_VERSION with the next protocol change.
        self.assertEqual(lobby.LOBBY_VERSION, "0.7.1.5")
        self.assertIsNone(lobby.version_rejection("0.7.1.5"))
        self.assertIsNone(lobby.version_rejection("0.7.1.5", "0.7.1.6"))
        self.assertIsNone(lobby.version_rejection("0.7.1.5", "0.7.1.7"))

    def test_release_version_is_the_installer_version(self):
        # The mismatch message names this; a release bump that forgets it fails here.
        installer = pathlib.Path(__file__).resolve().parents[1] / "installer" / "VERSION"
        self.assertEqual(lobby.RELEASE_VERSION, installer.read_text().strip())

    def test_mismatch_names_releases(self):
        # A 0.7.1.6 player was told "you have 0.7.1.5" and took the message for the bug.
        reason = lobby.version_rejection("0.7.1.4")
        self.assertIn(f"you have {lobby.RELEASE_VERSION};", reason)
        self.assertIn("the other side has 0.7.1.4.", reason)
        self.assertNotIn(f"you have {lobby.LOBBY_VERSION};", reason)
        # a newer peer names its release, which can differ from its lobby version
        self.assertIn("the other side has 0.7.2.1.", lobby.version_rejection("0.7.2", "0.7.2.1"))
        self.assertIn("the other side has unknown (older build).", lobby.version_rejection(None))
        self.assertIn("the other side has 0.7.1.3.", lobby.version_rejection("0.7.1.3", 7))
        # 0.7.1.6 sends lobby 0.7.1.5 and no release; 0.7.1.5 was never published
        self.assertEqual(lobby.version_label("0.7.1.5"), "0.7.1.6")
        self.assertEqual(lobby.version_label("0.7.1.5", "0.7.1.5"), "0.7.1.5")

    def test_old_hosts_reject_names_this_release(self):
        # The exact text a 0.7.1.4 host sends a joiner whose lobby version is 0.7.1.5.
        old = ("Multiplayer version mismatch: you have 0.7.1.5; the host has "
               "0.7.1.4. Install the same multiplayer version on both sides.")
        self.assertEqual(lobby.own_release_in_reject(old),
                         old.replace("you have 0.7.1.5;", f"you have {lobby.RELEASE_VERSION};"))
        for other in ("wrong password", "lobby full", None, 5):
            self.assertEqual(lobby.own_release_in_reject(other), other)

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
                    for version in (None, "0.4.22", "0.7.0.3", "0.7.0.4", "0.7.0.5", "0.7.0.6", "0.7.0.7", "0.7.1", "0.7.1.1", "0.7.1.2", "0.7.1.3", "0.7.1.4", "0.7.1.6", "99.0.0", 23, {}, lobby.LOBBY_VERSION):
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
                            self.assertEqual(response["release"], lobby.RELEASE_VERSION)
                        else:
                            self.assertEqual(response["t"], "reject")
                            self.assertIn("version mismatch", response["reason"])
                            self.assertIn(f"the host has {lobby.RELEASE_VERSION}.", response["reason"])
                            if isinstance(version, str):
                                self.assertIn(f"you have {version};", response["reason"])
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
            for version in (None, "0.4.22", "0.7.0.4", "0.7.0.5", "0.7.0.6", "0.7.0.7", "0.7.1", "0.7.1.1", "0.7.1.2", "0.7.1.3", "0.7.1.4", "0.7.1.6", "99.0.0", [], False):
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
                    self.assertIn(f"you have {lobby.RELEASE_VERSION};", events[-1]["detail"])
                    self.assertEqual(json.loads(pathlib.Path(io.state_path).read_text())["state"], "failed")
                    self.assertTrue(conn.closed)
                    self.assertTrue(any(m.get("t") == "leave" for m in conn.sent))
                    self.assertEqual(conn.sent[0]["version"], lobby.LOBBY_VERSION)
                    self.assertEqual(conn.sent[0]["release"], lobby.RELEASE_VERSION)

    def test_client_shows_its_release_in_an_old_hosts_reject(self):
        old = ("Multiplayer version mismatch: you have 0.7.1.5; the host has "
               "0.7.1.4. Install the same multiplayer version on both sides.")
        with tempfile.TemporaryDirectory() as root:
            class Connection:
                def __init__(self):
                    self.messages = collections.deque([json.dumps({"t": "reject", "reason": old}).encode()])
                    self.sent = []
                def send(self, raw): self.sent.append(json.loads(raw))
                def recv(self, timeout=0): return self.messages.popleft() if self.messages else None
                def last_seen_age(self): return 0
                def close(self): pass
            io = lobby.LobbyIO(root)
            lobby.run_client(Connection(), "tester", io, log=lambda _: None)
            detail = lobby._read_events(io.out_path)[-1]["detail"]
            self.assertIn(f"you have {lobby.RELEASE_VERSION}; the host has 0.7.1.4.", detail)
            self.assertEqual(json.loads(pathlib.Path(io.state_path).read_text())["detail"], detail)


if __name__ == "__main__":
    unittest.main()
