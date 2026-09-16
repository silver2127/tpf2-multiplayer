"""The vehicle drift check stops for good past 200 vehicles, on Lua 5.2, no game.

CM.vposCompare (hash.lua) pairs every vehicle with every vehicle, once per other player per
hash stamp, on the sim thread: 108 ms per player at 2,000 vehicles. Since 2026-09-15 the
first stamp that counts more than K.VPOS_MAX_VEHICLES (200), in our world or in a peer's
LSVPOS n=, turns the check off for good, and the switch rides in the save.

This runs the real hash.lua (worldHash, vposRecv, vposCompare, the cap) and the real
CM.vposShip and save/load text cut out of lockstep.lua, against a stub engine:
  - at 200 vehicles positions are collected, shipped and compared
  - at 201 the check turns off: nothing collected, shipped, received or compared, history cleared
  - selling back under the cap does not turn it on again
  - a peer's n= past the cap turns it off here too
  - the switch survives save/load, says so once in the log, and an old save leaves it on

    python tools/vpos_cap_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
HASH = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/hash.lua').read_text(encoding='utf-8')
LOCKSTEP = (REPO / 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(encoding='utf-8').replace('\r\n', '\n')


def cut(pattern, what):
    m = re.search(pattern, LOCKSTEP, re.S | re.M)
    if not m:
        sys.exit('could not find %s in lockstep.lua' % what)
    return m.group(1)


SHIP = cut(r'^(function CM\.vposShip\(stamp\)\n.*?\n^end)\n', 'CM.vposShip')
SAVELOAD = cut(r'^(\t\tsave = function\(\)\n.*?\n\t\tend,\n\t\tload = function\(s\)\n.*?\n\t\tend,)\n', 'the save/load pair')

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.HASH_SRC, G.SHIP_SRC, G.SAVELOAD_SRC = HASH, SHIP, SAVELOAD
L.execute(r'''
-- an engine that answers anything with an empty table, and N vehicles in a row
local function auto()
  return setmetatable({}, { __index = function(t, k) local v = auto(); rawset(t, k, v); return v end,
                            __call = function() return {} end })
end
api = auto()
VEHICLES = 0
game = { interface = {
  getEntities = function(_, filter)
    local out = {}
    if filter and filter.type == "VEHICLE" then
      for i = 1, VEHICLES do out[1000 + i] = { position = { i * 10.0, 5.0, 0.0 } } end
    end
    return out
  end,
  getEntity = function() return nil end,
} }

function newGame(letter)
  local g = { logs = {}, sent = {} }
  local CM = { peers = {}, ticks = 0, desyncs = 0 }
  local K = { INSTANCE = letter, BASE = "mem://" .. letter .. "/" }
  CM.cfgFlag = function(_, default) return default end
  CM.hashEveryFor = function() return 12 end
  CM.broadcast = function(line) g.sent[#g.sent + 1] = line end
  CM.noteDesync = function() CM.desyncs = CM.desyncs + 1 end
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  local mod = assert(load(HASH_SRC, "@hash.lua"))()(CM, K, log)
  assert(load("local CM, K, log, vposPrune = ...\n" .. SHIP_SRC, "@vposShip"))(CM, K, log, mod.vposPrune)
  g.data = assert(load("local CM = ...\nreturn {\n" .. SAVELOAD_SRC .. "\n}", "@saveload"))(CM)
  g.CM, g.K, g.hash = CM, K, mod
  return g
end
-- true and the hash's detail line, or false and the error
function stampHash(g, n, now)
  VEHICLES = n
  local ok, a, b = pcall(g.hash.worldHash, now)
  if ok then return true, b end
  return false, a
end
function count(t) local n = 0; for _ in pairs(t or {}) do n = n + 1 end; return n end
function logged(g, text) for _, l in ipairs(g.logs) do if l:find(text, 1, true) then return true end end; return false end
function nlogged(g, text) local n = 0; for _, l in ipairs(g.logs) do if l:find(text, 1, true) then n = n + 1 end end; return n end
-- our own LSVPOS lines as if player b had sent them
function asPeer(g, from, to)
  local out = {}
  for i = from, to do out[#out + 1] = (g.sent[i]:gsub(" o=a ", " o=b ")) end
  return out
end
''')

fails = []


def check(name, cond, extra=''):
    print(('ok   ' if cond else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not cond:
        fails.append(name)


print('== at the cap: the check runs')
a = G.newGame('a')
ok, err = G.stampHash(a, 200, 12.0)
check('worldHash runs to the end on the stub engine', ok, '' if ok else str(err))
check('200 vehicles: positions collected, check still on', a.CM.vposOff is None and G.count(a.CM.lastVposRaw) == 200)
a.CM.vposShip(12)
ship1 = len(a.sent)
check('200 vehicles: shipped in 7 LSVPOS parts', ship1 == 7 and all(l.startswith('LSVPOS t=12 ') for l in a.sent.values()), '%d lines' % ship1)
for line in G.asPeer(a, 1, ship1).values():
    a.CM.vposRecv(line)
check("a peer's 200 identical positions are compared (mean 0)", a.CM.vposLast is not None and a.CM.vposLast.b is not None
      and a.CM.vposLast.b.mean == 0 and a.CM.vposLast.b.n == 200)

print('== past the cap: off for good')
ok, _ = G.stampHash(a, 201, 24.0)
check('201 vehicles: the check turns off, at that count', a.CM.vposOff == 201)
check('says why, once', G.nlogged(a, 'over the cap of 200 -- the vehicle drift check is off for good') == 1)
check('nothing collected, the drift history dropped', a.CM.lastVposRaw is None and a.CM.vposLast is None
      and G.count(a.CM.vposMine) == 0 and G.count(a.CM.vposPeer) == 0 and G.count(a.CM.vposHist) == 0)
check('the world hash still carries the positions in its p lane', ok and 'p201@24.0:' in (G.stampHash(a, 201, 24.0)[1] or ''))
a.CM.vposShip(24)
check('nothing shipped', len(a.sent) == ship1)
check('the off notice is not repeated at the next stamp', G.nlogged(a, 'vehicle drift check is off') == 1)
for line in G.asPeer(a, 1, ship1).values():
    a.CM.vposRecv(line.replace('t=12 ', 't=24 ').replace('s=12.0 ', 's=24.0 '))
check("a peer's parts are ignored", G.count(a.CM.vposPeer) == 0 and a.CM.desyncs == 0)
a.CM.vposMine[36] = L.eval('{ s = 36.0, pts = { { 1, 1 } } }')
a.CM.vposCompare(36, 'b')
check('a direct compare does nothing', a.CM.vposLast is None)
G.stampHash(a, 150, 36.0)
a.CM.vposShip(36)
check('back under the cap (150 vehicles): still off, nothing collected or shipped',
      a.CM.vposOff == 201 and a.CM.lastVposRaw is None and len(a.sent) == ship1)

print("== a peer's world past the cap")
c = G.newGame('c')
G.stampHash(c, 50, 12.0)
c.CM.vposRecv('LSVPOS t=12 s=12.0 o=b i=1 m=9 n=250 d=10.0,5.0')
check("a peer's n=250 turns it off here too, naming the peer", c.CM.vposOff == 250 and G.logged(c, "250 vehicles in b's game"))
check('and nothing of it is kept', G.count(c.CM.vposPeer) == 0)

print('== saved with the world')
saved = a.data.save()
check('save carries the switch', saved.vposOff == 201)
d = G.newGame('a')
d.data.load(saved)
check('load restores it, silently (load is the GUI state per-frame sync too)', d.CM.vposOff == 201 and len(d.logs) == 0)
G.stampHash(d, 120, 12.0)
d.CM.vposShip(12)
check('a reloaded world under the cap stays off: nothing collected or shipped', d.CM.lastVposRaw is None and len(d.sent) == 0)
check('the log says so once', G.nlogged(d, 'the vehicle drift check is off in this world -- it had 201 vehicles') == 1)
d.CM.vposShip(24)
check('only once', G.nlogged(d, 'the vehicle drift check is off in this world') == 1)
e = G.newGame('a')
e.data.load(L.eval('{ cm = nil }'))
G.stampHash(e, 120, 12.0)
check('an older save without the switch leaves the check on', e.CM.vposOff is None and G.count(e.CM.lastVposRaw) == 120)
check('a running check saves no switch', e.data.save().vposOff is None)

print('ALL PASS' if not fails else '%d FAILED' % len(fails))
sys.exit(1 if fails else 0)
