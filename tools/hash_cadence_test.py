"""The world hash's interval follows its cost, on one grid for every game, on Lua 5.2, no game.

A stamp is floor(now / interval) * interval, so games on different intervals never compare a
single hash. Since 2026-09-15 each game times its stamps and reports the median on its heartbeat
(hc=), and the leader moves every game to the ladder interval that keeps the slowest one within
K.HASH_MS_PER_UNIT ms of hash per game unit, with a stamped HASHEVERY command (hash.lua).

This runs the real hash.lua cadence code and the real checkHash cut out of lockstep.lua for
three games that sample at different sim times, a command bus that applies HASHEVERY at its
stamp, and a hot joiner that takes the grid from the save:
  - the ladder: the smallest maps keep 12 units, an 8,000-edge map's ~600 ms goes to 96
  - the median of five: one slow stamp does not move the interval
  - the leader follows the SLOWEST fresh game; a stale one and a follower never decide
  - every game hashes exactly the same stamps through three switches (up, up, down)
  - down only with headroom, and at most once per K.HASH_CADENCE_MIN_TICKS
  - a bad HASHEVERY is refused; the grid survives save/load, and a hot joiner that loads it
    hashes the same stamps (one that does not, does not -- the check can fail)

    python tools/hash_cadence_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
HASH = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/hash.lua').read_text(encoding='utf-8')
LOCKSTEP = (REPO / 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(encoding='utf-8').replace('\r\n', '\n')
NET = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/net.lua').read_text(encoding='utf-8').replace('\r\n', '\n')

m = re.search(r'^(local function checkHash\(now\)\n.*?\n^end)\n', LOCKSTEP, re.S | re.M)
if not m:
    sys.exit('could not find checkHash in lockstep.lua')
CHECK = m.group(1)

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.HASH_SRC, G.CHECK_SRC = HASH, CHECK
L.execute(r'''
BUS = {}
function newGame(letter, leader, phase)
  local g = { letter = letter, logs = {}, stamps = {}, costMs = 50, now = 0, clock = 0, phase = phase or 0, applied = {} }
  local CM = { peers = {}, ticks = 0, leader = leader, seqNo = 0 }
  local K = { INSTANCE = letter, BASE = "mem://" .. letter .. "/", SIM_STEP = 0.2, PEER_STALE_TICKS = 25,
              HASH_EVERY_GAMETIME = 12, STAMP_KEEP = 64, EXEC_DELAY = 0.4 }
  CM.isLeader = function() return K.INSTANCE == CM.leader end
  CM.cfgFlag = function(_, d) return d end
  CM.broadcast = function() end
  CM.noteDesync = function() end
  CM.mapTooBigToHash = function() return false end
  CM.pruneOldest = function() end
  CM.vposShip = function() end
  CM.compareAt = function(stamp) g.stamps[#g.stamps + 1] = stamp end
  CM.myHashes, CM.myDetails, CM.comparedAt, CM.vposDone = {}, {}, {}, {}
  CM.scheduleLocal = function(op, args)
    CM.seqNo = CM.seqNo + 1
    local at = tonumber(string.format("%.4f", math.ceil((g.now + K.EXEC_DELAY) / K.SIM_STEP - 1e-6) * K.SIM_STEP))
    local c = { op = op, at = at, origin = letter, seq = CM.seqNo, tick = CM.ticks }
    for k, v in pairs(args) do c[k] = v end
    BUS[#BUS + 1] = c
  end
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  g.hash = assert(load(HASH_SRC, "@hash.lua"))()(CM, K, log)
  local fakeOs = { clock = function() return g.clock end }
  local function worldHash() g.clock = g.clock + g.costMs / 1000; return "h", "d" end
  g.checkHash = assert(load("local CM, K, log, worldHash, os = ...\nlocal lastHashAt = nil\n" .. CHECK_SRC .. "\nreturn checkHash",
    "@checkHash"))(CM, K, log, worldHash, fakeOs)
  g.CM, g.K = CM, K
  return g
end

-- every command due by this game's clock, in the order the leader issued them
local function applyDue(g)
  for _, c in ipairs(BUS) do
    local key = c.origin .. ":" .. c.seq
    if not g.applied[key] and c.at <= g.now + 1e-9 then
      g.applied[key] = true
      if c.op == "HASHEVERY" then g.CM.execHashEvery(c) end
    end
  end
end

-- games advance `rate` game units a tick, each `phase` behind the leader's clock
function run(games, ticks, rate, costs)
  for _ = 1, ticks do
    for _, g in ipairs(games) do
      if costs and costs[g.letter] then g.costMs = costs[g.letter] end
      g.CM.ticks = g.CM.ticks + 1
      g.now = g.now + rate
      applyDue(g)
      g.checkHash(g.now)
    end
    for _, g in ipairs(games) do   -- heartbeats: hc= as the real LSTICK carries it
      local hc = tonumber(g.CM.hashCostReport():match("hc=(%d+)"))
      for _, h in ipairs(games) do
        if h ~= g then
          h.CM.peers[g.letter] = h.CM.peers[g.letter] or {}
          h.CM.peers[g.letter].at = h.CM.ticks
          h.CM.peers[g.letter].hashMs = hc
        end
      end
    end
  end
end

-- the stamps two games both hashed, over the range both covered, as strings
function common(g, h, from)
  local lo = math.max(g.stamps[1], h.stamps[1], from or -math.huge)
  local hi = math.min(g.stamps[#g.stamps], h.stamps[#h.stamps])
  local a, b = {}, {}
  for _, s in ipairs(g.stamps) do if s >= lo and s <= hi then a[#a + 1] = tostring(s) end end
  for _, s in ipairs(h.stamps) do if s >= lo and s <= hi then b[#b + 1] = tostring(s) end end
  return table.concat(a, ","), table.concat(b, ","), #a
end
function logged(g, text) for _, l in ipairs(g.logs) do if l:find(text, 1, true) then return true end end; return false end
''')

fails = []


def check(name, cond, extra=''):
    print(('ok   ' if cond else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not cond:
        fails.append(name)


print('== the ladder: 8 ms of hash per game unit, rounded up')
x = G.newGame('x', 'x')
# 10000 ms reached the ladder's top rung of 768 until 2026-09-15. Big maps are no longer
# switched off (they were the ones hashing for seconds), so the ladder gained 1152 and 1536
# to carry them: 10000 / 8 = 1250 now rounds up to 1536 instead of clamping at 768.
want = {58: 12, 70: 12, 95: 12, 118: 24, 165: 24, 390: 72, 590: 96, 700: 96, 1200: 192, 6000: 768, 10000: 1536}
got = {ms: x.CM.hashEveryForCost(ms) for ms in want}
check('rungs as the logs of 2026-09-13 would get them', got == want, str(got))

print('== the measured cost')
check('no report before two stamps', x.CM.hashCostReport() == '' and (x.CM.hashCostNote(590), x.CM.hashCostReport())[1] == '')
for ms in (590, 5000, 590, 590):
    x.CM.hashCostNote(ms)
check('the median of five: one 5 s stamp does not count', x.CM.hashCostMs == 590 and x.CM.hashCostReport() == ' hc=590')
for ms in (600, 610, 620, 630, 640, 650):
    x.CM.hashCostNote(ms)
check('only the last five are kept', len(x.CM.hashCostSamples) == 5 and x.CM.hashCostMs == 630)
x.CM.ticks = 100
x.CM.peers.y = L.eval('{ at = 70, hashMs = 5000 }')
x.CM.peers.z = L.eval('{ at = 90, hashMs = 900 }')
worst = x.CM.hashCostSlowest()
check("the slowest fresh game counts; one silent past PEER_STALE_TICKS does not", worst == (900, 'z'), str(worst))
f = G.newGame('f', 'x')
for ms in (2000, 2000):
    f.CM.hashCostNote(ms)
f.CM.hashCadenceTick(1000)
check('a follower never decides', len(G.BUS) == 0)
bad = G.newGame('q', 'q')
bad.CM.execHashEvery(L.eval('{ op = "HASHEVERY", every = 100, prev = 60, at = 5000, origin = "q", seq = 1 }'))
bad.CM.execHashEvery(L.eval('{ op = "HASHEVERY", every = 6, prev = 60, at = 5000, origin = "q", seq = 2 }'))
check('a HASHEVERY off the 12-unit grid is refused', bad.CM.hashGrid is None and G.logged(bad, 'bad interval every=100'))

print('== three games through three switches')
a, b, c = G.newGame('a', 'a', 0), G.newGame('b', 'a', 0.2), G.newGame('c', 'a', 0.4)
for g in (a, b, c):
    g.CM.hashEvery = 60            # an 8,000-edge map's edge-count interval
    g.now = 1000 - g.phase
games = L.table(a, b, c)
G.run(games, 600, 2.0, L.table_from({'a': 590, 'b': 450, 'c': 1200}))
G.run(games, 1600, 2.0, L.table_from({'a': 300, 'b': 300, 'c': 300}))
cmds = [G.BUS[i] for i in range(1, len(G.BUS) + 1)]
seq = [int(cc.every) for cc in cmds]
check('the leader issued 96 (its own 590 ms), then 192 (C at 1200 ms), then 48 once all fell to 300 ms', seq == [96, 192, 48], str(seq))
check('every command came from the leader', all(cc.origin == 'a' for cc in cmds))
gaps = [cmds[i].tick - cmds[i - 1].tick for i in range(1, len(cmds))]
check('at least K.HASH_CADENCE_MIN_TICKS between changes', all(gp >= 300 for gp in gaps), str(gaps))
grids = [(g.CM.hashGrid.every, g.CM.hashGrid.prev, g.CM.hashGrid['from']) for g in (a, b, c)]
check('every game ends on the same grid', len(set(grids)) == 1 and grids[0][0] == 48, str(grids))
for g, h in ((a, b), (a, c)):
    sa, sb, n = G.common(g, h)
    check('%s and %s hashed exactly the same stamps (%d of them)' % (g.letter.upper(), h.letter.upper(), n), sa == sb and n > 20)
stamps = [int(s) for s in G.common(a, b)[0].split(',')]
froms = []
for cc in cmds:
    every = int(cc.every)
    froms.append(int(-(-float(cc.at) // every) * every))
check('stamps before the first switch are on 60, after the last on 48',
      all(s % 60 == 0 for s in stamps if s < froms[0]) and all(s % 48 == 0 for s in stamps if s >= froms[2]))
check('each game logged every switch at the same stamp',
      all(G.logged(g, 'from stamp %d' % fr) for g in (a, b, c) for fr in froms), str(froms))

print('== the grid in the save')
saved = a.CM.hashGridSave()
d, e = G.newGame('d', 'a', 0.3), G.newGame('e', 'a', 0.3)
d.CM.hashGridLoad(saved)
d.CM.hashGridLoad(L.eval('{ every = 768, prev = 12, from = 1 }'))
check('load takes the saved grid and never replaces one in place', d.CM.hashGrid.every == 48 and d.CM.hashGrid['from'] == saved['from'])
for g in (d, e):
    g.now = a.now - g.phase
    # their save was taken after every command so far: those arrive through the save, never the
    # history (LSNEED serves only commands stamped after it), so mark them applied
    for i in range(1, len(G.BUS) + 1):
        g.applied['%s:%d' % (G.BUS[i].origin, int(G.BUS[i].seq))] = True
G.run(L.table(a, b, c, d, e), 300, 2.0, L.table_from({'a': 300, 'b': 300, 'c': 300, 'd': 300, 'e': 300}))
sa, sd, n = G.common(a, d)
check('a hot joiner that loaded it hashes the same stamps (%d)' % n, sa == sd and n > 5)
sa, se, n = G.common(a, e)
check('one that did not load it does not (the stamp check can fail)', sa != se)

print('== wiring in the real files')
check('checkHash takes its stamp from the agreed grid and notes the cost', 'CM.hashStampOf(now)' in CHECK
      and 'CM.hashCostNote(' in CHECK and 'CM.hashCadenceTick' in CHECK)
check('the heartbeat carries hc=', 'CM.hashCostReport and CM.hashCostReport()' in LOCKSTEP)
check('HASHEVERY is dispatched', 'elseif c.op == "HASHEVERY" then CM.execHashEvery(c)' in LOCKSTEP)
check('the grid is saved and loaded', 'hashGrid = CM.hashGridSave' in LOCKSTEP and 'CM.hashGridLoad, s.hashGrid' in LOCKSTEP)
check('the heartbeat reader keeps hc=', 'pr.hashMs = tonumber(line:match(" hc=(%d+)"))' in NET)

print('ALL PASS' if not fails else '%d FAILED' % len(fails))
sys.exit(1 if fails else 0)
