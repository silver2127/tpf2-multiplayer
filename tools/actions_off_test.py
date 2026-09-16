"""A game far behind the others turns its player's actions off, on Lua 5.2, no game.

A command's stamp pays at most CM.MAX_LEAD (15 units) of lead over the fastest game, so a game
further behind than that would stamp its player's actions into the other games' past. Since
2026-09-15 inject.lua drops those actions until the game is back within 2 units.

This runs the real inject.lua (CM.actionsBlockTick and CM.pollInject) and the real
CM.fastestPeerClock cut out of net.lua against stubbed captures:
  - off only past 15 units behind the fastest game, back on only within 2 (and never with no peer)
  - dropped while off: captures the slice cancelled (ARMED 1, CONXP/CONUP/CDEMO) and the calendar
  - still shipped while off: what already ran natively (ARMED 0) and speed buttons
  - a dropped ROADE takes its STREETP bus/tram values with it
  - a line creation is held, then replayed behind ARMED 1 once caught up -- with or without new
    records in that read, after a VBUY that waits for its VBUYLINE -- and the ARMED state is restored

    python tools/actions_off_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
INJECT = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/inject.lua').read_text(encoding='utf-8')
NET = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/net.lua').read_text(encoding='utf-8').replace('\r\n', '\n')
LOCKSTEP = (REPO / 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(encoding='utf-8').replace('\r\n', '\n')

m = re.search(r'^(function CM\.fastestPeerClock\(\)\n.*?\n^end)\n', NET, re.S | re.M)
if not m:
    sys.exit('could not find CM.fastestPeerClock in net.lua')
FASTEST = m.group(1)

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.INJECT_SRC, G.FASTEST_SRC = INJECT, FASTEST
L.execute(r'''
LOGS, SCHED, SPEED, DATA, FAST = {}, {}, {}, nil, nil
CM = { ticks = 100, peerSeen = true, MAX_LEAD = 15.0, pendingLineCreates = {}, injectOffset = 0 }
K = { INSTANCE = "b", INJECT_FILE = "mem://inject", SIM_STEP = 0.2 }
CM.readFrom = function(_, off) local d = DATA; DATA = nil; return d, off end
CM.scheduleLocal = function(op, args) SCHED[#SCHED + 1] = { op = op, args = args } end
CM.gameTime = function() return 1000 end
CM.speedButton = function(v) SPEED[#SPEED + 1] = v end
CM.vehKeyFor = function(id) return "s:" .. tostring(id) end
CM.stationGroupPos = function() return 10, 20 end
CM.stationPosInGroup = function() return nil end
CM.unescName = function(s) return s end
CM.fastestPeerClock = function() return FAST end
assert(load(INJECT_SRC, "@inject.lua"))()(CM, K, function(msg) LOGS[#LOGS + 1] = tostring(msg) end)

function feed(text) DATA = text; CM.pollInject() end
function tick(now, fast) FAST = fast; CM.actionsBlockTick(now) end
function logged(t) for _, l in ipairs(LOGS) do if l:find(t, 1, true) then return true end end; return false end
function ops() local o = {}; for i, s in ipairs(SCHED) do o[i] = s.op end; return table.concat(o, ",") end
function reset() SCHED, SPEED, LOGS = {}, {}, {} end

-- the real fastestPeerClock against stubbed peer readings
function fastest(bounds, proj)
  local C, KK = {}, { SIM_STEP = 0.2 }
  C.peerBounds = function() return nil, bounds end
  C.projectedPeerMax = proj and function() return proj end or nil
  assert(load("local CM, K = ...\n" .. FASTEST_SRC .. "\nreturn CM.fastestPeerClock", "@fastestPeerClock"))(C, KK)
  return C.fastestPeerClock()
end
''')

fails = []


def check(name, cond, extra=''):
    print(('ok   ' if cond else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not cond:
        fails.append(name)


print('== the fastest clock a stamp has to clear')
check('the projected peer, plus a step of margin', abs(G.fastest(100, 101) - 101.2) < 1e-9)
check('the heartbeat reading when projection is behind it', G.fastest(100, 99) == 100)
check('no fresh peer: none', G.fastest(None, None) is None)

print('== in step: actions replicate')
G.tick(1000, 1001)
G.feed('SETDATE 2460000\nARMED 1\nVREV 7\n')
check('a calendar change and a cancelled reverse are scheduled', not G.CM.actionsOff and G.ops() == 'SETDATE,VREV', G.ops())

print('== far behind: off')
G.reset()
G.tick(1000, 1015)
check('exactly 15 units behind is not past the limit', not G.CM.actionsOff)
G.tick(1000, 1015.5)
check('15.5 behind turns actions off, and says so', G.CM.actionsOff and G.logged('ACTIONS OFF: this game is 15.5 game units behind'))
G.feed('SETDATE 2460000\nARMED 1\nVREV 7\nARMED 0\nVREV 8\nCONXP station/x.con t=1,0,0 params={}\nCDEMO 1 5\nSPEEDBTN 2\n')
check('only the native reverse (ARMED 0) is scheduled', G.ops() == 'VREV' and G.SCHED[1].args.key == 's:8', G.ops())
check('the calendar, the cancelled reverse, the placement and the demolish are dropped',
      all(G.logged('ACTIONS OFF: %s dropped' % op) for op in ('SETDATE', 'VREV', 'CONXP', 'CDEMO')))
check('speed buttons still work', len(G.SPEED) == 1 and G.SPEED[1] == 2)
G.feed('STREETP 1 2\nARMED 1\nROADE 0 0 16 1 0 1 0 0\n')
check("a dropped road takes its bus lane and tram with it", G.CM.lastStreetBus is None and G.CM.lastStreetTram is None
      and G.logged('ACTIONS OFF: ROADE dropped'))
G.feed('ARMED 1\nLCREATEX 0.1 0.2 0.3 180 1 555 0 0 0 0 180 0 name=Held1\n')
check('a line creation is held, not scheduled', len(G.CM.actionsHeld) == 1 and G.ops() == 'VREV'
      and G.logged('a line creation is held'))
G.tick(1000, 1010)
check('10 behind is still off (back on only within 2)', G.CM.actionsOff)
G.feed('ARMED 1\nVLINE 7 9 0\n')
check('a cancelled line assignment is dropped', G.logged('ACTIONS OFF: VLINE dropped') and G.ops() == 'VREV')

print('== caught up: on again, held lines go out')
G.reset()
G.tick(1000, 1001.5)
check('within 2 turns actions back on, and says the held line goes out', not G.CM.actionsOff
      and G.logged('1 held line creation(s) go out now'))
G.feed('ARMED 0\n')
check('the held line is created behind ARMED 1', G.ops() == 'LCREATE' and G.SCHED[1].args.armed == 1
      and G.SCHED[1].args.name == 'Held1', G.ops())
check('the ARMED state after it is what this read left (0)', G.CM.lastArmed == 0 and len(G.CM.actionsHeld) == 0)

G.reset()
G.tick(1000, 1020)
G.feed('ARMED 1\nLCREATEX 0.1 0.2 0.3 180 1 555 0 0 0 0 180 0 name=Held2\n')
G.CM.lastArmed = 0
G.tick(1000, 1000.5)
G.feed(None)
check('released on a poll with nothing new to read', G.ops() == 'LCREATE' and G.SCHED[1].args.name == 'Held2'
      and G.CM.lastArmed == 0, G.ops())

G.reset()
G.tick(1000, 1020)
G.feed('ARMED 1\nLCREATEX 0.1 0.2 0.3 180 1 555 0 0 0 0 180 0 name=Held3\n')
G.tick(1000, 1000.5)
G.feed('ARMED 0\nVBUY 42 1\n')
check('after a VBUY that waits for its VBUYLINE: the VBUY is still carried, the held line goes out',
      str(G.CM.injectCarry).startswith('VBUY') and G.ops() == 'LCREATE' and G.SCHED[1].args.name == 'Held3', G.ops())
check('and the carried VBUY will read its own ARMED 0', G.CM.lastArmed == 0)

print('== no peer: never behind')
G.tick(1000, 1020)
G.tick(1000, None)
check('a game that hears nobody turns actions back on', not G.CM.actionsOff)

print('== wiring in the real files')
check('checked every tick, just before the inject read', 'pcall(CM.actionsBlockTick, now)\n\t\t\tCM.pollInject()' in LOCKSTEP)
check('scheduleLocal stamps past the same clock', 'local fastT = CM.fastestPeerClock()' in NET)
check('the dash file carries it and the window shows it', 'f:write("actionsoff="' in LOCKSTEP and 'D.alertText:setText(text)' in LOCKSTEP)

print('ALL PASS' if not fails else '%d FAILED' % len(fails))
sys.exit(1 if fails else 0)
