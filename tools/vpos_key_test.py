"""The vehicle drift check pairs the SAME vehicle on both sides and names the one that drifts.

Until 2026-09-16 the check paired each of our positions with whichever of the peer's
positions was nearest. A cloned train that sat 10 m behind its original on the host
and 1,150 m away on the joiner therefore read "max 10.8 m" on the host (its clone
paired with the joiner's original) and "max 1,150 m" on the joiner -- and neither
log said WHICH vehicle. Positions now travel with the vehicle's cross-peer key, the
pairing is by key, an unbound vehicle still falls back to the nearest point, and the
log names the worst offenders. Runs the real hash.lua on Lua 5.2, no game.

    python tools/vpos_key_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
HASH = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/hash.lua').read_text(encoding='utf-8')
LOCKSTEP = (REPO / 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(encoding='utf-8').replace('\r\n', '\n')
m = re.search(r'^(function CM\.vposShip\(stamp\)\n.*?\n^end)\n', LOCKSTEP, re.S | re.M)
if not m:
    sys.exit('could not find CM.vposShip in lockstep.lua')
SHIP = m.group(1)

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.HASH_SRC, G.SHIP_SRC = HASH, SHIP
L.execute(r'''
-- a game: its own CM/K, a log, and every LSVPOS line it broadcast
function newGame(letter)
  local g = { letter = letter, logs = {}, sent = {}, desyncs = 0 }
  local CM = { peers = {}, ticks = 0, vehKeyOf = {}, primedVeh = {} }
  local K = { INSTANCE = letter, PEER_STALE_TICKS = 25 }
  CM.broadcast = function(line) g.sent[#g.sent + 1] = line end
  CM.desyncs = 0
  CM.noteDesync = function() g.desyncs = g.desyncs + 1; CM.desyncs = g.desyncs end
  CM.vehIdForKey = function(key) return g.idOf and g.idOf[key] or nil end
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  api = { engine = { getComponent = function(id, kind)   -- the game created last owns the engine stub
      if kind == "TV" and g.lineOf and g.lineOf[id] then return { line = g.lineOf[id] } end
      if kind == "NAME" and g.lineName and g.lineName[id] then return { name = g.lineName[id] } end
      return nil end },
    type = { ComponentType = { TRANSPORT_VEHICLE = "TV", NAME = "NAME" } } }
  assert(load(HASH_SRC, "@hash.lua"))()(CM, K, log)
  assert(load("local CM, K, log = ...\nlocal function vposPrune() end\n" .. SHIP_SRC, "@vposShip"))(CM, K, log)
  g.CM, g.K, g.log = CM, K, log
  return g
end
-- sample `pts` ({x, y, key}) at sim time s on game g and ship stamp t
function sample(g, t, s, pts)
  g.CM.lastVposRaw, g.CM.lastVposT = pts, s
  g.CM.vposShip(t)
end
function deliver(from, to)
  for _, line in ipairs(from.sent) do to.CM.vposRecv(line) end
  from.sent = {}
end
function lastLog(g, needle)
  for i = #g.logs, 1, -1 do if g.logs[i]:find(needle, 1, true) then return g.logs[i] end end
  return nil
end
''')

fails = []
def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)

# --- 1. the field case: the clone is 10 m behind its original on A, 1,150 m off on B
L.execute(r'''
A, B = newGame("a"), newGame("b")
B.idOf, B.lineOf, B.lineName = { ["a:114"] = 761620 }, { [761620] = 751014 }, { [751014] = "Leigh-Woking" }
sample(A, 4608, 4608.0, { { 1000, 1000, "s:1" }, { 1010, 1000, "a:114" }, { 5000, 5000, "s:2" } })
sample(B, 4608, 4608.0, { { 1000, 1000, "s:1" }, { 2150, 1000, "a:114" }, { 5000, 5000, "s:2" } })
deliver(A, B); deliver(B, A)
''')
a_line = L.eval('lastLog(A, "VPOS t=4608 vs b @")')
b_line = L.eval('lastLog(B, "VPOS t=4608 vs a @")')
check('max=1140.00 m' in a_line, 'the host measures the clone against ITS clone, not the nearest train: ' + a_line)
check('max=1140.00 m' in b_line, 'the joiner measures the same: ' + b_line)
check(L.eval('A.desyncs') == 1 and L.eval('B.desyncs') == 1, 'both call it a desync')
named = L.eval('lastLog(B, "drift #1")')
check(named is not None and 'a:114 (same key)' in named and "751014 'Leigh-Woking'" in named and 'off by 1140.0 m' in named,
      'the worst offender is named with its line: ' + str(named))
check(L.eval('lastLog(A, "drift #2")') is None, 'only the drifting vehicle is listed')

# --- 2. an unbound vehicle (no key yet) still pairs with the nearest point
L.execute(r'''
A, B = newGame("a"), newGame("b")
sample(A, 100, 100.0, { { 0, 0, "s:1" }, { 500, 500 } })
sample(B, 100, 100.0, { { 0, 0, "s:1" }, { 500.5, 500 } })
deliver(A, B); deliver(B, A)
''')
line = L.eval('lastLog(A, "VPOS t=100 vs b @")')
check('max=0.50 m' in line and L.eval('A.desyncs') == 0, 'unbound vehicles fall back to nearest: ' + line)
wire = L.eval('A.sent[1] or ""')

# --- 3. the wire carries the key, and "-" for none; a receiver parses both
L.execute(r'''
A, B = newGame("a"), newGame("b")
sample(A, 200, 200.0, { { 1.5, 2.5, "s:7" }, { 3, 4 } })
''')
wire = L.eval('A.sent[1]')
check(' d=1.5,2.5,s:7;3.0,4.0,-' in wire, 'wire format: ' + wire)
L.execute('deliver(A, B)')
check(L.eval('B.CM.vposPeer.a[200].pts[1][3]') == 's:7' and L.eval('B.CM.vposPeer.a[200].pts[2][3]') is None,
      'the receiver keeps the key and drops the placeholder')

# --- 4. the same key on both sides but different sim times is still skipped, never compared
L.execute(r'''
A, B = newGame("a"), newGame("b")
sample(A, 300, 300.0, { { 0, 0, "s:1" } })
sample(B, 300, 300.4, { { 900, 0, "s:1" } })
deliver(A, B)
''')
check(L.eval('lastLog(B, "sampled at different sim times")') is not None and L.eval('B.desyncs') == 0,
      'different sample times are skipped, not judged')

print('ALL PASS' if not fails else f'{len(fails)} FAILED')
sys.exit(1 if fails else 0)
