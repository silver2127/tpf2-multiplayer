"""THE WORKSHOP FIRST: a joiner subscribes to the host's Workshop mods through Steam.

A joiner's receiver (lobby._ClientSaveReceiver) against a fake Steam Workshop
(steamtunnel.FakeTunnel's UGC SUB / UGC STATE), a fake game that answers each
registry publish with a catalogue receipt, and no host:

  1. the player says yes to three mods (two Workshop, one local): the Workshop
     ones go to Steam and the host is asked for NOTHING while Steam works
  2. Steam installs one; the other it never subscribes to (a hidden item), so
     after UGC_SUBSCRIBE_WAIT that one falls back to the host
  3. what Steam installed is registered with the game (its folder in the
     registry, the receipt lists it); then the host is asked for exactly the
     rest -- the local mod and the item Steam did not deliver
  4. every mod on the Workshop: nothing is asked of the host, mods_ready follows
     the receipt
  5. a download that stops moving falls back after UGC_STALL
  6. no tunnel, or the rig's ignore_steam_workshop flag: the host is asked for
     everything at once, as before

    python tools/test_workshop_subscribe.py
"""
import os
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "netpunch"))

TMP = tempfile.mkdtemp(prefix="ws_sub_")
os.environ["TPF2MP_DATADIR"] = os.path.join(TMP, "data")
os.makedirs(os.environ["TPF2MP_DATADIR"])

import modshare                                   # noqa: E402
import steamtunnel                                # noqa: E402
import lobby                                      # noqa: E402

# nothing of this PC's real game or Steam library may leak in
modshare.workshop_dirs = lambda: []
modshare.game_dir = lambda: None
modshare.userdata_mods_dir = lambda: None
lobby.UGC_SUBSCRIBE_WAIT = 1.0
lobby.UGC_POLL_EVERY = 0.1

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def workshop_folder(wid):
    p = os.path.join(TMP, "steam", "workshop", "content", "1066780", wid)
    os.makedirs(p, exist_ok=True)
    with open(os.path.join(p, "mod.lua"), "w") as f:
        f.write("function data() return {} end\n")
    return p


def fake_game():
    """The game's catalogue refresh: a receipt for the registry's token listing its rows."""
    token, rows = modshare.read_registry()
    if token and modshare.catalogue()[0] != token:
        with open(os.path.join(modshare.data_dir(), "mods_catalogue.txt"), "w", encoding="utf-8") as f:
            f.write(token + "\n" + "".join(f"*{wid}\t1\n" for wid in rows))


class Conn:
    peer = ("127.0.0.1", 1)

    def send(self, piece):
        pass


def receiver(name):
    io = lobby.LobbyIO(os.path.join(TMP, name))
    r = lobby._ClientSaveReceiver(Conn(), io, lambda s: None)
    sent = []
    r._send = sent.append
    return r, io, sent


def requests(sent):
    return [m.get("need") for m in sent if m.get("t") == "mods_request"]


def run(r, secs, until=None):
    end = time.time() + secs
    while time.time() < end:
        fake_game()
        r.tick(time.time())
        if until and until():
            return True
        time.sleep(0.05)
    return bool(until and until())


def reset_data():
    for n in ("mods_registry.txt", "mods_catalogue.txt", "tpf2mp_modtest.txt"):
        try:
            os.remove(os.path.join(modshare.data_dir(), n))
        except OSError:
            pass


steam_dir = os.path.join(TMP, "steamdata")
fake = steamtunnel.FakeTunnel(steam_dir, 3003, "joiner")
fake.workshop = {"111": workshop_folder("111"), "222": None, "333": workshop_folder("333")}
lobby._UGC[0] = steamtunnel.SteamTunnel(steam_dir)
try:
    # ---- 1-3. two Workshop mods and a local one
    reset_data()
    r, io, sent = receiver("a")
    r.on_manifest([["*111", 1], ["*222", 1], ["localmod", 1]])
    check("the player is asked about all three", r.ask and sorted(r.offered) == ["*111_1", "*222_1", "localmod_1"], str(r.offered))
    r.answer_mods(True, r.consent_id)
    check("the Workshop mods go to Steam", sorted(r.steam_items) == ["111", "222"], str(r.steam_items))
    run(r, 0.5)
    check("the host is asked for nothing while Steam works", requests(sent) == [], str(requests(sent)))
    got = run(r, 6, until=lambda: requests(sent))
    check("then the host is asked for exactly the rest", got and sorted(requests(sent)[0]) == ["*222_1", "localmod_1"], str(requests(sent)))
    token, rows = modshare.read_registry()
    check("what Steam installed is in the registry", rows.get("111") == os.path.abspath(fake.workshop["111"]), str(rows))
    check("... and the game catalogued it", ("*111", "1") in modshare.catalogue()[1])
    check("the save is not ready while mods are still to come", not r.mods_satisfied and not r.cancelled)
    check("Steam's subscription was asked for", "111" in fake.subscribed)

    # ---- 4. every mod on the Workshop
    reset_data()
    r, io, sent = receiver("b")
    r.on_manifest([["*111", 1], ["*333", 1]])
    r.answer_mods(True, r.consent_id)
    ok = run(r, 6, until=lambda: r.mods_satisfied)
    check("all on the Workshop: satisfied after the receipt", ok and not r.cancelled and not r.failed)
    check("... and the host was asked for nothing", requests(sent) == [], str(requests(sent)))
    ready = [e for e in lobby._read_events(io.out_path) if e.get("type") == "mods_ready"]
    check("... and mods_ready was said", bool(ready))

    # ---- 5. a stalled download
    reset_data()
    lobby.UGC_STALL, fake.ugc_delay, fake.ugc_size = 0.5, 1e9, 0
    r, io, sent = receiver("c")
    r.on_manifest([["*333", 1]])
    r.answer_mods(True, r.consent_id)
    got = run(r, 5, until=lambda: requests(sent))
    check("a download that stops moving falls back to the host", got and requests(sent)[0] == ["*333_1"], str(requests(sent)))
    fake.ugc_delay, fake.ugc_size = 0.3, 1000000

    # ---- 6a. the rig flag
    reset_data()
    with open(os.path.join(modshare.data_dir(), "tpf2mp_modtest.txt"), "w") as f:
        f.write("ignore_steam_workshop\n")
    r, io, sent = receiver("d")
    r.on_manifest([["*111", 1], ["localmod", 1]])
    r.answer_mods(True, r.consent_id)
    check("ignore_steam_workshop: no Steam, the host is asked for everything",
          not r.steam_items and sorted(requests(sent)[0]) == ["*111_1", "localmod_1"], str(requests(sent)))
finally:
    fake.close()

# ---- 6b. no tunnel
reset_data()
lobby._UGC[0] = None
os.environ["TPF2MP_DATADIR"] = os.path.join(TMP, "data")     # no identity file there: no tunnel
r, io, sent = receiver("e")
r.on_manifest([["*111", 1], ["localmod", 1]])
r.answer_mods(True, r.consent_id)
check("no Steam tunnel: the host is asked for everything at once",
      not r.steam_items and sorted(requests(sent)[0]) == ["*111_1", "localmod_1"], str(requests(sent)))

print()
if fails:
    print(f"{len(fails)} FAILED")
    for f in fails:
        print("  - " + f)
    raise SystemExit(1)
print("PASS: the Workshop first -- Steam subscribes, installs and registers; the host sends only the rest; fallbacks for a hidden item, a stall, the rig flag and no tunnel")
