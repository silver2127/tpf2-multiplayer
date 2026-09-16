"""The lobby's mode assigns companies (lobby.py, 2026-09-16).

Runs the real run_host on a loopback socket with raw UDP joiners. Co-op puts
everyone on company 1; separate companies gives the host 1 and each joiner the
lowest free id in join order, for a mode change and for later joiners alike; a
chip click still overrides one player; a joiner cannot set the mode of a
player-hosted lobby; --companies starts a lobby in that mode; the roster event
and the state file carry the mode for the panels.

    python tools/lobby_mode_test.py
"""
import json
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
import lobby  # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


class Host:
    def __init__(self, name, companies_mode=False):
        self.dir = tempfile.mkdtemp(prefix="lobbymode-")
        self.io = lobby.LobbyIO(self.dir)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.addr = self.sock.getsockname()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=lobby.run_host, args=(self.sock, name, self.io),
                                       kwargs=dict(stop=self.stop, log=lambda *_: None, companies_mode=companies_mode),
                                       daemon=True)
        self.thread.start()
        self.joiners = []

    def command(self, **cmd):
        with open(self.io.in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(cmd) + "\n")

    def state(self):
        try:
            return json.loads(open(self.io.state_path, encoding="utf-8").read())
        except (OSError, ValueError):
            return {}

    def wait(self, pred, seconds=6.0):
        deadline = time.time() + seconds
        while time.time() < deadline:
            st = self.state()
            if pred(st):
                return st
            time.sleep(0.05)
        return self.state()

    def join(self, name):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        s.settimeout(0.2)
        lobby._send_data(s, self.addr, {"t": "join", "version": lobby.LOBBY_VERSION, "name": name})
        j = {"sock": s, "name": name, "alive": True}
        self.joiners.append(j)
        threading.Thread(target=self._keepalive, args=(j,), daemon=True).start()
        return j

    def _keepalive(self, j):
        while j["alive"] and not self.stop.is_set():
            try:
                j["sock"].recv(65536)
            except (socket.timeout, OSError):
                pass
            lobby._send_data(j["sock"], self.addr, {"t": "ping"})
            time.sleep(0.3)

    def events(self):
        try:
            lines = open(self.io.out_path, encoding="utf-8").read().splitlines()
        except OSError:
            return []
        out = []
        for line in lines:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
        return out

    def close(self):
        self.stop.set()
        for j in self.joiners:
            j["alive"] = False
        self.thread.join(3)
        self.sock.close()


def companies(st):
    return {k: int(v) for k, v in (st.get("companies") or {}).items()}


def main():
    h = Host("Ada")
    try:
        for n in ("bob", "cid", "dan"):
            h.join(n)
        st = h.wait(lambda s: len(s.get("players") or []) == 4)
        check("four players in the lobby", len(st.get("players") or []) == 4, st.get("players"))
        check("co-op by default: everyone on company 1", companies(st) == {"Ada": 1, "bob": 1, "cid": 1, "dan": 1}, companies(st))
        check("the state file says coop", st.get("mode") == "coop", st.get("mode"))

        h.command(cmd="mode", mode="companies")
        st = h.wait(lambda s: s.get("mode") == "companies" and len(set(companies(s).values())) == 4)
        check("separate companies: host 1, joiners 2, 3, 4 in join order",
              companies(st) == {"Ada": 1, "bob": 2, "cid": 3, "dan": 4}, companies(st))
        h.join("eve")
        st = h.wait(lambda s: "eve" in companies(s))
        check("a later joiner gets the next free company", companies(st).get("eve") == 5, companies(st))

        h.command(cmd="company", player="cid", id=2)
        st = h.wait(lambda s: companies(s).get("cid") == 2)
        check("a chip click still overrides one player (cid joins bob's company 2)", companies(st).get("cid") == 2, companies(st))

        h.command(cmd="mode", mode="coop")
        st = h.wait(lambda s: s.get("mode") == "coop" and set(companies(s).values()) == {1})
        check("back to co-op: everyone on 1 again", set(companies(st).values()) == {1} and len(companies(st)) == 5, companies(st))
        h.join("fay")
        st = h.wait(lambda s: "fay" in companies(s))
        check("a joiner in co-op lands on 1", companies(st).get("fay") == 1, companies(st))

        h.command(cmd="mode", mode="companies")
        st = h.wait(lambda s: s.get("mode") == "companies" and len(set(companies(s).values())) == 6)
        check("separate again: ids follow join order, the override is gone",
              companies(st) == {"Ada": 1, "bob": 2, "cid": 3, "dan": 4, "eve": 5, "fay": 6}, companies(st))

        # a joiner may not set the mode of a player-hosted lobby
        lobby._send_data(h.joiners[0]["sock"], h.addr, {"t": "mode", "mode": "coop"})
        time.sleep(0.8)
        st = h.state()
        check("a joiner's mode message is ignored on a player-hosted lobby", st.get("mode") == "companies" and len(set(companies(st).values())) == 6, st.get("mode"))

        rosters = [e for e in h.events() if e.get("type") == "roster"]
        check("roster events carry the mode", rosters and all("mode" in e for e in rosters) and rosters[-1]["mode"] == "companies",
              rosters[-1].get("mode") if rosters else None)
        h.command(cmd="mode", mode="pvp-or-whatever")
        st = h.wait(lambda s: s.get("mode") == "coop")
        check("an unknown mode value reads as coop", st.get("mode") == "coop", st.get("mode"))
    finally:
        h.close()

    h2 = Host("Zed", companies_mode=True)
    try:
        h2.join("amy")
        h2.join("ben")
        st = h2.wait(lambda s: len(s.get("players") or []) == 3)
        check("--companies: a lobby started in separate mode assigns 1, 2, 3 from the first join",
              st.get("mode") == "companies" and companies(st) == {"Zed": 1, "amy": 2, "ben": 3}, (st.get("mode"), companies(st)))
    finally:
        h2.close()

    print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
