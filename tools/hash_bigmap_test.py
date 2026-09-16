"""Offline checks that big maps still run the desync hash, just rarely, on Lua 5.2.

The world hash walks every vehicle, construction and edge on the sim thread. On a 224-tile
map (27k edges) that measured 3.0-3.5 s per stamp, so from 2026-09-12 the hash was switched
OFF for the whole game above vanilla's largest size -- which left exactly the biggest worlds
with NO desync detection.

Since 2026-09-15 the cost-proportional cadence (hash.lua) carries that load instead: the
interval starts from the EDGE COUNT (identical on every instance, read from the same save,
so the stamp grids agree) and the leader then moves everyone with a stamped HASHEVERY. A
huge world hashes seldom rather than never.

This runs the REAL functions cut out of lockstep.lua and hash.lua and checks:
  - the size-based switch-off is GONE: no mapTooBigToHash, no early return in checkHash
  - the starting interval grows with the edge count and caps at the ladder's top rung
  - the ladder is ascending and every rung is a multiple of the base (execHashEvery's rule)
  - a cost picks a rung that keeps it within K.HASH_MS_PER_UNIT ms per game unit
  - a 3.5 s hash on a 224-tile map lands on a rung that is minutes apart, not off
  - the stats panel still renders the verdicts (an older peer can still report OFF)

    python tools/hash_bigmap_test.py
"""
import os
import re
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCKSTEP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
HASH = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "hash.lua")
STATS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "stats.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def read(path):
    return open(path, encoding="utf-8").read().replace("\r\n", "\n")


def cut(src, pattern, what):
    m = re.search(pattern, src, re.S | re.M)
    if not m:
        sys.exit(f"could not cut {what}")
    return m.group(0)


def runtime():
    """The real interval functions, run together in one Lua state."""
    lock, hsh = read(LOCKSTEP), read(HASH)
    block = "\n".join([
        cut(lock, r"^K\.HASH_EVERY_GAMETIME\s*=.*?$", "K.HASH_EVERY_GAMETIME"),
        cut(lock, r"^K\.HASH_EDGES_PER_STEP\b.*?^end$", "CM.hashEveryFor"),
        cut(hsh, r"^K\.HASH_MS_PER_UNIT\s*=.*?$", "K.HASH_MS_PER_UNIT"),
        cut(hsh, r"^K\.HASH_EVERY_LADDER\s*=.*?$", "K.HASH_EVERY_LADDER"),
        cut(hsh, r"^function CM\.hashEveryForCost\(ms\).*?^end$", "CM.hashEveryForCost"),
    ])
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().BLOCK = block
    return L.execute(r'''
local K, CM = {}, {}
assert(load("return function(K, CM) " .. BLOCK .. " end", "@cut"))()(K, CM)
local H = {}
function H.every(edges) return CM.hashEveryFor(edges) end
function H.forCost(ms) return CM.hashEveryForCost(ms) end
function H.base() return K.HASH_EVERY_GAMETIME end
function H.msPerUnit() return K.HASH_MS_PER_UNIT end
function H.ladder() return table.concat(K.HASH_EVERY_LADDER, ",") end
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
local H = {}
function H.words(v) return (CM.verdictWords(v)) end
return H
''')


def main():
    lock = read(LOCKSTEP)

    print("the size-based switch-off is gone")
    check("no CM.mapTooBigToHash in lockstep.lua", "mapTooBigToHash" not in lock)
    check("no vanilla size constants", "VANILLA_MAX_TILES" not in lock)
    early = re.search(r"^local function checkHash\(now\)\n\tif .*?\n\t\tCM\.dashVerdict = \"OFF\"", lock, re.M)
    check("checkHash no longer returns early before hashing", early is None)

    R = runtime()
    base, mspu = R.base(), R.msPerUnit()
    ladder = [int(v) for v in R.ladder().split(",")]

    print("\nthe ladder")
    check("ascending", all(b > a for a, b in zip(ladder, ladder[1:])), R.ladder())
    check(f"every rung is a multiple of the base ({base})", all(v % base == 0 for v in ladder), R.ladder())
    check("starts at the base", ladder[0] == base, str(ladder[0]))
    check("reaches past 768 for worlds far past vanilla", ladder[-1] >= 1152, str(ladder[-1]))

    print("\nthe starting interval follows the edge count")
    cases = [
        (0, base, "an empty world"),
        (1999, base, "under one step"),
        (2000, base * 2, "one step"),
        (27000, base * 14, "a 224-tile map (27k edges)"),
    ]
    for edges, want, what in cases:
        got = int(R.every(edges))
        check(f"{edges} edges ({what}) -> every {want}", got == want, f"got {got}")
    huge = int(R.every(10 ** 7))
    check("an enormous world caps at the ladder's top rung", huge == ladder[-1] or huge == base * 64,
          f"got {huge}")
    check("...and is never switched off (always a positive interval)", huge > 0, str(huge))

    print("\na cost picks a rung that fits the budget")
    for ms in (0, 100, 380, 1000, 3500, 10000):
        rung = int(R.forCost(ms))
        fits = ms / rung <= mspu + 1e-9
        top = rung == ladder[-1]
        check(f"{ms} ms -> every {rung} units ({ms / rung:.1f} ms/unit)", fits or top,
              f"budget is {mspu} ms/unit")

    print("\nthe 224-tile map that caused the switch-off")
    rung = int(R.forCost(3500))
    check("a 3.5 s hash lands on a rung, not off", rung in ladder, str(rung))
    check("...and that rung is minutes apart (>= 288 units)", rung >= 288, str(rung))

    print("\nstats panel")
    S = stats_runtime()
    check('"SYNC" reads as sync', S.words("SYNC") == "sync")
    check('"-" reads as checking', S.words("-") == "checking")
    check('"OFF" still understood (an older peer can report it)', S.words("OFF") == "off")

    print()
    print("ALL PASS" if not fails else f"{len(fails)} FAILURE(S)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
