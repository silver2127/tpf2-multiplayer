"""The train-name hash lane, on Lua 5.2, no game.

The native reservation-order patch (native/src/slice_hook.cpp, "TRAIN RESERVATION
ORDER") ranks trains by NAME to decide which one reserves a junction first, so a
name that differs between two peers is a divergence of the simulation and not of
the vehicle window. The geometry lanes of the world hash cannot see it. This runs
the real hash.lua worldHash, and the real comparison cut out of net.lua, against a
stub engine:

  - the r lane counts the rail vehicles and carries two hashes: the names in
    entity-id order, and the names sorted
  - neither depends on the order the engine enumerated the vehicles in
  - a rename changes the sorted hash; the same names under swapped ids change
    only the by-id one, and the log says which happened
  - road vehicles are left out when the build reports a carrier, and when it
    reports none the lane covers everything and says so once
  - two stamps of differing names in a row is a desync; one is not

    python tools/train_name_lane_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
HASH = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/hash.lua').read_text(encoding='utf-8')
NET = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/net.lua').read_text(encoding='utf-8').replace('\r\n', '\n')


def cut(pattern, what):
    m = re.search(pattern, NET, re.S | re.M)
    if not m:
        sys.exit('could not find %s in net.lua' % what)
    return m.group(1)


LANEDIFF = cut(r'^(local function logLaneDiff\(dm, dt\)\n.*?\n^end)\n', 'logLaneDiff')
# the every-stamp train-name comparison, lifted out of CM.compareOne
NAMECHECK = cut(r'^\t(-- TRAIN NAMES AS A DESYNC OF THEIR OWN\..*?\n\tend)\n\tif mine == theirs then',
                'the train-name comparison')

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.HASH_SRC, G.LANEDIFF_SRC, G.NAMECHECK_SRC = HASH, LANEDIFF, NAMECHECK
L.execute(r'''
local function auto()
  return setmetatable({}, { __index = function(t, k) local v = auto(); rawset(t, k, v); return v end,
                            __call = function() return {} end })
end
api = auto()
-- the vehicles this stub hands back, in the order it hands them back
FLEET = {}
api.engine.getComponent = function(id, _)
  for i = 1, #FLEET do if FLEET[i].id == id then return { name = FLEET[i].name } end end
  return {}
end
game = { interface = {
  getEntities = function(_, filter)
    local out = {}
    if filter and filter.type == "VEHICLE" then
      for i = 1, #FLEET do
        -- an ARRAY, so the enumeration order is the one the test asked for
        out[#out + 1] = { id = FLEET[i].id, carrier = FLEET[i].carrier,
                          position = { i * 10.0, 5.0, 0.0 } }
      end
    end
    return out
  end,
  getEntity = function() return nil end,
} }

function setFleet(list)
  FLEET = {}
  for i = 1, #list do FLEET[i] = { id = list[i][1], name = list[i][2], carrier = list[i][3] } end
end

function newGame(letter)
  local g = { logs = {} }
  local CM = { peers = {}, ticks = 0, desyncs = 0, myDetails = {} }
  local K = { INSTANCE = letter, BASE = "mem://" .. letter .. "/" }
  CM.cfgFlag = function(_, default) return default end
  CM.hashEveryFor = function() return 12 end
  CM.broadcast = function() end
  CM.noteDesync = function(why) CM.desyncs = CM.desyncs + 1; CM.lastWhy = why end
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  g.CM, g.K, g.log = CM, K, log
  g.hash = assert(load(HASH_SRC, "@hash.lua"))()(CM, K, log)
  g.laneDiff = assert(load("local log = ...\n" .. LANEDIFF_SRC .. "\nreturn logLaneDiff", "@lanediff"))(log)
  g.nameCheck = assert(load("local CM, log, stamp, origin, dt = ...\n" .. NAMECHECK_SRC, "@namecheck"))
  return g
end

-- the detail line for the current fleet
function detailFor(g, now)
  local ok, _, d = pcall(g.hash.worldHash, now)
  if not ok then return nil end
  return d
end
function rLane(d) return d and d:match("(r%d+:[^,]+)") or nil end
function logged(g, text) for _, l in ipairs(g.logs) do if l:find(text, 1, true) then return true end end; return false end
function nlogged(g, text) local n = 0; for _, l in ipairs(g.logs) do if l:find(text, 1, true) then n = n + 1 end end; return n end
function clearLogs(g) g.logs = {} end
function runCheck(g, stamp, mineDetail, theirDetail, origin)
  g.CM.myDetails[stamp] = mineDetail
  g.nameCheck(g.CM, g.log, stamp, origin, theirDetail)
end
''')

fails = []


def check(name, cond, extra=''):
    print(('ok   ' if cond else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not cond:
        fails.append(name)


def fleet(pairs_):
    G.setFleet(L.table_from([L.table_from(list(p)) for p in pairs_]))


RAIL = 'RAIL'
print('== the lane')
a = G.newGame('a')
fleet([(101, 'Coal Express', RAIL), (102, 'ore 2', RAIL), (103, 'Alpha', RAIL)])
d = G.detailFor(a, 12.0)
lane = G.rLane(d)
check('the detail carries an r lane', lane is not None, str(lane))
check('it counts the rail vehicles', lane.startswith('r3:'), lane)
by_id, sorted_h = lane[3:].split('/')
check('and carries two hashes', by_id != sorted_h and '-' in by_id and '-' in sorted_h, lane)

print('== the order the engine enumerates them in does not reach the lane')
b = G.newGame('b')
fleet([(103, 'Alpha', RAIL), (101, 'Coal Express', RAIL), (102, 'ore 2', RAIL)])
lane2 = G.rLane(G.detailFor(b, 12.0))
check('a different enumeration order gives the same lane', lane == lane2, '%s vs %s' % (lane, lane2))

print('== what each hash reacts to')
c = G.newGame('c')
fleet([(101, 'Coal Express', RAIL), (102, 'ore 2 RENAMED', RAIL), (103, 'Alpha', RAIL)])
lane3 = G.rLane(G.detailFor(c, 12.0))
check('a rename changes the sorted hash', lane3.split('/')[1] != sorted_h)
check('...and the by-id hash too', lane3[3:].split('/')[0] != by_id)
fleet([(101, 'ore 2', RAIL), (102, 'Coal Express', RAIL), (103, 'Alpha', RAIL)])
lane4 = G.rLane(G.detailFor(c, 12.0))
check('the same names under swapped ids keep the sorted hash', lane4.split('/')[1] == sorted_h)
check('...but change the by-id hash', lane4[3:].split('/')[0] != by_id, lane4)

print('== which vehicles count')
e = G.newGame('e')
fleet([(101, 'Coal Express', RAIL), (102, 'ore 2', RAIL), (103, 'Alpha', RAIL),
       (201, 'Bus 1', 'ROAD'), (202, 'Truck', 'ROAD')])
laneRail = G.rLane(G.detailFor(e, 12.0))
check('road vehicles are left out of the lane', laneRail == lane, '%s vs %s' % (laneRail, lane))
check('the vehicle count lane still sees all five', G.detailFor(e, 12.0).startswith('v5,'))
check('no carrier warning when the build reports one', not G.logged(e, 'no vehicle reported a carrier'))

f = G.newGame('f')
fleet([(101, 'Coal Express', None), (102, 'ore 2', None), (103, 'Alpha', None), (201, 'Bus 1', None)])
laneAll = G.rLane(G.detailFor(f, 12.0))
check('with no carrier field the lane covers every vehicle', laneAll.startswith('r4:'), laneAll)
G.detailFor(f, 24.0)
check('and says so exactly once', G.nlogged(f, 'no vehicle reported a carrier') == 1)

print('== the comparison')
g = G.newGame('g')
mine = 'v3,c0:h,e0:h,z:h,p3@12.0:h,' + lane + ',m:1,l:0,t:5,n:9'
same = mine
renamed = 'v3,c0:h,e0:h,z:h,p3@12.0:h,' + lane3 + ',m:1,l:0,t:5,n:9'
swapped = 'v3,c0:h,e0:h,z:h,p3@12.0:h,' + lane4 + ',m:1,l:0,t:5,n:9'
G.runCheck(g, 12, mine, same, 'b')
check('identical names: nothing logged, no desync', len(g.logs) == 0 and g.CM.desyncs == 0)
G.runCheck(g, 16, mine, renamed, 'b')
check('one differing stamp waits', G.logged(g, '~~ train names differ') and g.CM.desyncs == 0)
G.runCheck(g, 20, mine, renamed, 'b')
check('two in a row is a desync', g.CM.desyncs == 1 and G.logged(g, '!! DESYNC (train names)'))
check('...and the dash says which', str(g.CM.dashVerdict) == 'DESYNC train names vs b', str(g.CM.dashVerdict))
G.runCheck(g, 24, mine, same, 'b')
check('agreeing again clears the streak', g.CM.trainNameStreak.b == 0 and g.CM.desyncs == 1)
G.runCheck(g, 28, mine, swapped, 'b')
check('the same names in another id order is NOT a name difference',
      g.CM.desyncs == 1 and g.CM.trainNameStreak.b == 0)

print('== the lane diff message')
h = G.newGame('h')
h.laneDiff(mine, renamed)
check('a real name difference is named as one', G.logged(h, 'train names differ'))
check('...and says what it costs', G.logged(h, 'different orders'))
i = G.newGame('i')
i.laneDiff(mine, swapped)
check('an id-order difference is called out separately',
      G.logged(i, 'train names MATCH') and G.logged(i, 'different'))
j = G.newGame('j')
j.laneDiff(mine, mine)
check('identical details log nothing', len(j.logs) == 0)

print('== an older peer without the lane')
k = G.newGame('k')
old = 'v3,c0:h,e0:h,z:h,p3@12.0:h,m:1,l:0,t:5,n:9'
G.runCheck(k, 12, mine, old, 'b')
G.runCheck(k, 16, mine, old, 'b')
check('no lane in their detail: skipped, never a desync', k.CM.desyncs == 0 and len(k.logs) == 0)
k.laneDiff(mine, old)
check('and the lane diff does not invent one', not G.logged(k, 'train names'))

print()
if fails:
    print('FAILED: ' + ', '.join(fails))
    sys.exit(1)
print('ALL PASS')
