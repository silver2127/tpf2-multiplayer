"""Offline checks that the desync hash is off on maps larger than vanilla allows, on Lua 5.2.

The world hash walks every vehicle, construction and edge on the sim thread. On a 224-tile
map (27k edges) that measured 3.0-3.5 s per stamp -- a freeze every few minutes, even in a
solo game (2026-09-12). lockstep.lua's CM.mapTooBigToHash now turns the hash off for the
whole game when the terrain is bigger than the stock New Game menu builds (Megalomaniac:
at most 96 x 96 = 9,216 tiles, and 192 tiles on an axis for 1:4).

This runs the REAL function text cut out of lockstep.lua, and the real stats.lua, and checks:
  - every vanilla preset shape keeps hashing; anything past the area or the axis does not
  - an unreadable terrain keeps hashing (the safe side), and says so
  - the answer is read once per game, then cached
  - checkHash returns before any hashing when the map is too big
  - the stats panel says the check is OFF instead of "checking" forever

    python tools/hash_bigmap_test.py
"""
import os
import re
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCKSTEP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
STATS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "stats.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def size_check_source():
    src = open(LOCKSTEP, encoding="utf-8").read().replace("\r\n", "\n")
    m = re.search(r"^K\.VANILLA_MAX_TILES\s*=.*?(?=^local function checkHash\(now\))", src, re.S | re.M)
    if not m:
        sys.exit("could not find the CM.mapTooBigToHash block in lockstep.lua")
    early = re.search(r"^local function checkHash\(now\)\n\tif CM\.mapTooBigToHash\(\) then\n"
                      r"\t\tCM\.dashVerdict = \"OFF\"\n\t\treturn\n\tend\n", src, re.M)
    return m.group(0), early is not None


def size_runtime(block, tiles):
    """tiles: (x, y), or None for a terrain read that throws."""
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    g = L.globals()
    g.BLOCK = block
    g.TX, g.TY = (tiles if tiles else (None, None))
    g.BROKEN = tiles is None
    return L.execute(r'''
local logs, reads = {}, 0
api = {
  engine = {
    util = { getWorld = function() return 1 end },
    getComponent = function(e, t)
      reads = reads + 1
      if BROKEN then error("no terrain component") end
      return { size = { x = TX, y = TY } }
    end,
  },
  type = { ComponentType = { TERRAIN = 42 } },
}
local K, CM = {}, {}
local log = function(s) logs[#logs + 1] = s end
assert(load("return function(K, CM, log) " .. BLOCK .. " end", "@lockstep.lua"))()(K, CM, log)
local H = {}
function H.off() return CM.mapTooBigToHash() end
function H.reads() return reads end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def stats_runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = open(STATS, encoding="utf-8").read()
    return L.execute(r'''
local CM = { guiLeader = function() return "a" end }
local K = {}
local log = function() end
assert(load(SRC, "@stats.lua"))()(CM, K, log)
local function view() local v = { text = "" }; function v:setText(s) self.text = s end; return v end
local H = {}
function H.words(v) return (CM.verdictWords(v)) end
function H.status(verdict, npeers) return CM.statusWords({ verdict = verdict, desyncs = "0", t = "100" }, npeers) end
-- the sync column for us (a) and one peer (b), with our verdict as given
function H.cells(verdict)
  local cells = {}
  for _, l in ipairs({ "a", "b" }) do cells[l] = { name = view(), sync = view(), clock = view(), notes = view() } end
  local status = view()
  CM.statsInWords(status, cells, "a", { "a", "b" },
    { a = { verdict = verdict, desyncs = "0", t = "100", speed = "1" } },
    { b = { t = 100, skew = 0, verdict = "-" } })
  return cells.a.sync.text, cells.b.sync.text, status.text
end
return H
''')


def main():
    block, early = size_check_source()
    print("size check, cut from lockstep.lua")
    cases = [
        ((96, 96), False, "Megalomaniac 1:1"),
        ((66, 132), False, "Megalomaniac 1:2"),
        ((54, 162), False, "Megalomaniac 1:3"),
        ((48, 192), False, "Megalomaniac 1:4"),
        ((18, 54), False, "Small 1:3"),
        ((100, 90), False, "under both limits, not a preset"),
        ((98, 96), True, "a little past the area"),
        ((40, 200), True, "past the axis, under the area"),
        ((128, 128), True, "the smallest Big Maps row"),
        ((224, 224), True, "the 57 km map"),
        ((448, 224), True, "a Big Maps rectangle"),
    ]
    for tiles, want, what in cases:
        R = size_runtime(block, tiles)
        got = R.off()
        check(f"{tiles[0]} x {tiles[1]} ({what}): hash {'OFF' if want else 'on'}", got == want,
              f"got {'OFF' if got else 'on'}; log: {R.logs()}")

    R = size_runtime(block, (224, 224))
    R.off(); R.off(); R.off()
    check("the terrain is read once per game", R.reads() == 1, str(R.reads()))
    check("the decision is logged with the size", "224 x 224" in R.logs() and "OFF" in R.logs(), R.logs())

    R = size_runtime(block, None)
    check("an unreadable terrain keeps hashing", R.off() is False)
    check("...and says so", "unreadable" in R.logs(), R.logs())

    check("checkHash returns before hashing when the map is too big", early)

    print("stats panel")
    S = stats_runtime()
    check('verdict "OFF" reads as off', S.words("OFF") == "off", str(S.words("OFF")))
    check('"SYNC" still reads as sync', S.words("SYNC") == "sync")
    check('"-" still reads as checking', S.words("-") == "checking")
    st = S.status("OFF", 1)
    check("status line says the check is OFF and why", "OFF" in st and "larger than vanilla" in st, st)
    check("no peer heard still says so first", S.status("OFF", 0).startswith("No other player heard"))
    own, peer, _ = S.cells("OFF")
    check("our row reads 'not checked'", own.strip() == "not checked", repr(own))
    check("a peer's row reads 'not checked'", peer.strip() == "not checked", repr(peer))
    own, peer, _ = S.cells("SYNC")
    check("a synced game still reads 'match' / 'checking'", own.strip() == "match" and peer.strip() == "checking",
          f"{own!r} {peer!r}")

    print()
    print("ALL PASS" if not fails else f"{len(fails)} FAILURE(S)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
