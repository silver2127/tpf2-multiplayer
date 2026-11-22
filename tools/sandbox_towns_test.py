"""Sandbox mode's town tool, replicated (mod/.../mp/sandbox.lua, native/src/slice/capture.inl).

Offline, on Lua 5.2 (no game needed):
  * a TOWNC line exactly as the slice writes it becomes the same TownInfo on the
    placing game (its own table) and on a peer (the real wire codec from net.lua);
  * the position survives as the float32 the slice read;
  * cargo needs travel whole, "-" is an empty list, the name "-" leaves it to the engine;
  * malformed fields are refused and nothing is sent;
  * the slice, the Add hook and lockstep.lua carry the wiring the design needs.

    python tools/sandbox_towns_test.py
"""
import os
import re
import struct
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
read = lambda *p: open(os.path.join(REPO, *p), encoding="utf-8", errors="replace").read()
SANDBOX = read("mod", "mp_lockstep_1", "res", "scripts", "mp", "sandbox.lua")
NET = read("mod", "mp_lockstep_1", "res", "scripts", "mp", "net.lua")
CONS = read("mod", "mp_lockstep_1", "res", "scripts", "mp", "cons.lua")
INJECT = read("mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")
LOCKSTEP = read("mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
CAPTURE = read("native", "src", "slice", "capture.inl")
ADDHOOK = read("native", "src", "slice", "add_hook.inl")
SLICE = read("native", "src", "slice_hook.cpp")

failures = []


def check(name, ok, extra=""):
    print(("ok   " if ok else "FAIL ") + name + (f"  ({extra})" if extra and not ok else ""))
    if not ok:
        failures.append(name)


L = lupa.LuaRuntime(unpack_returned_tuples=True)
# the wire codec, cut from net.lua as the other codec tests do (it sets K.LEAD_SANE)
codec = "local K = {}\n" + NET[NET.index("local function encodeCmd(c)"):NET.index("function CM.scheduleLocal(op, args)")]
L.execute("ENC, DEC = (function()\n" + codec + "\nreturn encodeCmd, decodeCmd\nend)()")
unesc = CONS[CONS.index("function CM.unescName(s)"):]
unesc = unesc[:unesc.index("\nend") + 4]
L.execute(r"""
LOGS = {}
SENT = {}
CM = {}
""" + unesc + r"""
local function vec() return {} end
api = { type = { TownInfo = { new = function()
          return { name = "", position = { x = 0, y = 0 }, initialLandUseCapacities = { 0, 0, 0 },
                   landUse2CargoNeeds = { vec(), vec(), vec() } } end } },
        cmd = { make = { createTowns = function(towns) return { towns = towns } end },
                sendCommand = function(cmd, cb) SENT[#SENT + 1] = cmd; if cb then cb(nil, true) end end } }
""")
KT = L.table()
KT.INSTANCE = "a"
L.execute("return function(src) return load(src, 'sandbox.lua') end")(SANDBOX)()(L.eval("CM"), KT, L.eval("function(s) LOGS[#LOGS+1] = s end"))
CM = L.eval("CM")


def inject_fields(line):
    """What inject.lua schedules for a TOWNC line (the same field mapping)."""
    w = line.split()
    num = lambda s: float(s) if re.fullmatch(r"-?\d+(\.\d+)?", s) else -1
    return {"op": "TOWNC", "pos": w[1], "c1": num(w[2]), "c2": num(w[3]), "c3": num(w[4]),
            "n1": w[5], "n2": w[6], "n3": w[7], "name": w[8], "armed": 1}


def to_lua(d):
    t = L.table()
    for k, v in d.items():
        t[k] = v
    return t


def sent_town(i=-1):
    sent = L.eval("SENT")
    cmd = sent[len(sent)] if i == -1 else sent[i]
    return cmd.towns[1]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


# ---- the placing game and a peer build the same TownInfo ------------------ #
# the line the slice wrote for the town placed on 2026-09-27 (Charlotte)
x, y = f32(-1641.91003), f32(-1300.64404)
line = f"TOWNC {x:.9g},{y:.9g} 90 90 90 - 28,26,28 23,24,23 -"
local_cmd = inject_fields(line)
local_cmd.update(at=100.4, origin="a", seq=7)
wire = L.eval("ENC")(to_lua(local_cmd))
peer_cmd = L.eval("DEC")(wire)
check("the wire keeps the position as text", peer_cmd.pos == local_cmd["pos"], f"{peer_cmd.pos!r} vs {local_cmd['pos']!r}")

CM.execTownCreate(to_lua(local_cmd))
mine = sent_town()
CM.execTownCreate(peer_cmd)
theirs = sent_town()


def fields(t):
    return (t.name, f32(t.position.x), f32(t.position.y),
            tuple(t.initialLandUseCapacities[i] for i in (1, 2, 3)),
            tuple(tuple(t.landUse2CargoNeeds[i][k] for k in range(1, len(t.landUse2CargoNeeds[i]) + 1)) for i in (1, 2, 3)))


check("the placing game and the peer build the same TownInfo", fields(mine) == fields(theirs), f"{fields(mine)} vs {fields(theirs)}")
check("the position is the float32 the slice read", (f32(mine.position.x), f32(mine.position.y)) == (x, y),
      f"{mine.position.x}, {mine.position.y}")
check("capacities as captured", fields(mine)[3] == (90, 90, 90), str(fields(mine)[3]))
check("cargo needs whole, '-' is none", fields(mine)[4] == ((), (28, 26, 28), (23, 24, 23)), str(fields(mine)[4]))
check("name '-' leaves the name to the engine", mine.name == "", repr(mine.name))
check("the result is logged with success", any("success=true" in s for s in L.eval("LOGS").values()))

# a named town (a script's, or a future tool option): percent-encoded, a text key
named = inject_fields(f"TOWNC 10,20 1 2 3 5 - - Neckargem%C3%BCnd%20Nord")
named.update(at=1, origin="b", seq=1)
back = L.eval("DEC")(L.eval("ENC")(to_lua(named)))
CM.execTownCreate(back)
check("a name round-trips through the codec and the escape", sent_town().name == "Neckargemünd Nord", repr(sent_town().name))
digits = inject_fields("TOWNC 10,20 1 2 3 - - - 007")
digits.update(at=1, origin="b", seq=2)
back = L.eval("DEC")(L.eval("ENC")(to_lua(digits)))
CM.execTownCreate(back)
check("a name that looks like a number stays text", sent_town().name == "007", repr(sent_town().name))
single = inject_fields("TOWNC 10,20 1 2 3 - 28 - -")
single.update(at=1, origin="b", seq=3)
back = L.eval("DEC")(L.eval("ENC")(to_lua(single)))
CM.execTownCreate(back)
check("a one-cargo list (a number on the wire) still reads", sent_town().landUse2CargoNeeds[2][1] == 28)

# ---- refused, nothing sent -------------------------------------------------- #
for what, bad in [("a position without y", {"pos": "10"}), ("a text position", {"pos": "a,b"}),
                  ("a negative capacity", {"c2": -1}), ("a malformed cargo list", {"n2": "28,x"}),
                  ("a fractional cargo id", {"n3": "2.5"})]:
    before = len(L.eval("SENT"))
    c = inject_fields("TOWNC 10,20 1 2 3 - - - -")
    c.update(bad)
    c.update(at=1, origin="b", seq=9)
    CM.execTownCreate(to_lua(c))
    check(f"{what} is refused and nothing is sent", len(L.eval("SENT")) == before and
          any("refused" in s for s in L.eval("LOGS").values()))

# ---- the wiring -------------------------------------------------------------- #
check("the slice hooks CreateTowns (0x9dd0b0, steal 15, id 19)", '{ 0x9dd0b0, 15, 19, "CreateTowns"' in SLICE)
check("only the town tool's click is captured", "CALLER_TOWNBUILDER = 0x470959" in CAPTURE
      and "if (caller != CALLER_TOWNBUILDER)" in CAPTURE)
check("the Add hook cancels the town tool's CreateTowns",
      "(id == 19 && caller == CALLER_TOWNBUILDER)" in ADDHOOK and "id == 19 || id == 20" in ADDHOOK)
check("nothing is cancelled that did not decode or ship",
      "did not decode -- NOT cancelled" in CAPTURE and "nothing shipped -- NOT cancelled" in CAPTURE
      and "if (armed) WriteArmed(false);" in CAPTURE)
check("the slice writes the position float-exact and 0x90-byte TownInfos",
      'fprintf(f, "TOWNC %.9g,%.9g %d %d %d"' in CAPTURE and "TOWNINFO_SIZE = 0x90" in CAPTURE)
check("no claim file: the placing game's replay needs nothing from the slice", "tclaim" not in SANDBOX)
check("the town tool's callback is fired at the cancel (the tool waits on it: the \"building town\" bar)",
      "InterlockedExchange(&g_pendingNoCb, 0);" in CAPTURE[CAPTURE.index("static void CaptureTowns"):])
check("TOWNC is a strict op (the placer replays it too)", re.search(r"K\.STRICT_OPS = \{[^}]*TOWNC = true", LOCKSTEP) is not None)
check("TOWNC is dispatched", 'elseif c.op == "TOWNC" then CM.execTownCreate(c)' in LOCKSTEP)
check("mp.sandbox boots after mp.cons (CM.unescName)",
      LOCKSTEP.index('CM.boot("mp.cons")') < LOCKSTEP.index('CM.boot("mp.sandbox")'))
check("inject.lua schedules TOWNC and drops it with the other armed actions far behind",
      'CM.scheduleLocal("TOWNC"' in INJECT and re.search(r"K\.ACTIONS_OFF_ARMED = \{[^}]*TOWNC = true", INJECT) is not None)

print()
if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all passed")
