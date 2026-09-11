"""Offline test of CM.bulldozeAlive (mod/.../scripts/mp/conx.lua) on Lua 5.2.

The survivor-diff and corridor sweeps bulldoze a list gathered up front; an
entry already removed by an earlier bulldoze (a town building takes its own
asset groups with it) must be skipped, because game.interface.bulldoze on a
gone entity is a native crash (2026-09-11, entity 264852).

    python tools/bulldoze_alive_test.py
"""
import os, sys
import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "conx.lua"), encoding="utf-8").read()
a = src.index("function CM.bulldozeAlive(id)")
b = src.index("\nend\n", a) + 5

L = lupa.LuaRuntime()
run = L.execute(r'''
return function(fnsrc)
  local world = { [100] = true, [101] = true, [102] = true }
  local calls = {}
  api = { engine = { entityExists = function(id) calls[#calls + 1] = "exists " .. id; return world[id] == true end } }
  game = { interface = { bulldoze = function(id)
    calls[#calls + 1] = "bulldoze " .. id
    if id == 101 then error("engine refused") end
    world[id] = nil
    if id == 100 then world[102] = nil end   -- a town building takes its asset group with it
  end } }
  local CM = {}
  assert(load("local CM = ... " .. fnsrc))(CM)
  local r = {}
  r[1] = CM.bulldozeAlive(100)
  r[2] = CM.bulldozeAlive(102)   -- removed by the first bulldoze: skipped, no bulldoze call
  r[3] = CM.bulldozeAlive(101)   -- still exists but the engine refuses: false
  r[4] = CM.bulldozeAlive(nil)
  r[5] = CM.bulldozeAlive(-1)
  return r[1], r[2], r[3], r[4], r[5], table.concat(calls, ",")
end
''')
res = run(src[a:b])
got, calls = list(res[:5]), res[5]
want = [True, False, False, False, False]
want_calls = "exists 100,bulldoze 100,exists 102,exists 101,bulldoze 101"
fails = []
for name, g, w in zip(["alive", "already gone", "engine refuses", "nil id", "negative id"], got, want):
    print(("ok   " if g == w else "FAIL ") + f"{name}: {g}")
    if g != w:
        fails.append(name)
print(("ok   " if calls == want_calls else "FAIL ") + "calls: " + calls)
if calls != want_calls:
    fails.append("calls")
print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
