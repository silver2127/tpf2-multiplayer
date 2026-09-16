"""Speed votes, no game: the real pacing.lua in the game's Lua (5.2) against a stub engine.

The session speed is the mean of the players' votes (pacing.lua, THE SESSION SPEED IS THE
PLAYERS' VOTE). This checks the rules that do not show up in a closed-loop run: how each
speed control is classified, the order votes win in, who the leader counts, and the /speed
override. tools/pacing_sim.py speed_votes runs the same code closed-loop.

    python tools/speed_vote_test.py
"""
from pathlib import Path

from lupa.lua52 import LuaRuntime

REPO = Path(__file__).resolve().parents[1]
SOURCE = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/pacing.lua').read_text(encoding='utf-8')

lua = LuaRuntime(unpack_returned_tuples=True)
lua.execute('assert(load(...))', SOURCE)   # compiles under 5.2, the chunk's local limit included
lua.execute(r'''
FACTORY = assert(load(...))()
FS, LOGS, SENT, CMDS = {}, {}, {}, {}
SPEED, NOW = 4, 1000
local real_open = io.open
io.open = function(path, mode)
  if type(path) == "string" and path:sub(1, 6) == "mem://" then
    if (mode or "r"):find("w") then
      local buf, f = {}, {}
      function f:write(...) for _, v in ipairs({...}) do buf[#buf + 1] = tostring(v) end return self end
      function f:close() FS[path] = table.concat(buf) end
      return f
    end
    local body = FS[path]
    if not body then return nil end
    local f = {}
    function f:read(fmt) if fmt == "*l" then return body:match("^[^\n]*") end return body end
    function f:close() end
    return f
  end
  return real_open(path, mode)
end
game = { interface = { getGameSpeed = function() return SPEED end } }
api = { cmd = { make = { setGameSpeed = function(v) return { speed = v } end },
                sendCommand = function(c) SPEED = c.speed end } }

function newCM(letter)
  local K = { INSTANCE = letter, BASE = "mem://" .. letter .. "/", SIM_STEP = 0.2, PEER_STALE_TICKS = 25, EXEC_DELAY = 0.4 }
  local CM = { peers = {}, ticks = 0, leader = "a", seqNo = 0, cfgCache = {} }
  CM.isLeader = function() return K.INSTANCE == (CM.leader or "a") end
  CM.leaderPrecise = function() return nil end
  CM.peerFastPrecise = function() return nil end
  CM.stepOf = function(t) return math.floor(t / 0.2 + 0.5) end
  CM.clearFile = function(path) FS[path] = nil end
  CM.cfgFlag = function(_, default) return default end
  CM.gameTime = function() return NOW end
  CM.broadcast = function(line) SENT[#SENT + 1] = line end
  CM.scheduleLocal = function(op, args)
    CM.seqNo = CM.seqNo + 1
    local c = { op = op, origin = letter, seq = CM.seqNo, at = NOW + 0.4 }
    for k, v in pairs(args) do c[k] = v end
    CMDS[#CMDS + 1] = c
  end
  FACTORY(CM, K, function(msg) LOGS[#LOGS + 1] = letter .. ": " .. tostring(msg) end)
  return CM, K
end
function heard(CM, letter) CM.peers[letter] = { at = CM.ticks } end
function vote(CM, origin, v, ct, seq) CM.execSpeedVote({ op = "SPEEDVOTE", origin = origin, v = v, ct = ct, seq = seq, at = ct + 0.4 }) end
function lastSent() return SENT[#SENT] end
function lastLog() return LOGS[#LOGS] end
''', SOURCE)
G = lua.globals()
failures = 0


def check(label, ok):
    global failures
    failures += 0 if ok else 1
    print('  %s  %s' % ('OK  ' if ok else 'FAIL', label))


def run(chunk):
    return lua.execute(chunk)


print('== a vote as a number')
b, _ = G.newCM('b')
check('snapped to quarters', b.voteValue(2.6) == 2.5 and b.voteValue(2.63) == 2.75)
check('clamped to 0.25..8', b.voteValue(0) == 0.25 and b.voteValue(64) == 8)
check('not a number is no vote', b.voteValue('x') is None)

print('== a vote at its stamp: newer by the click, not the stamp')
G.vote(b, 'c', 2, 100.0, 5)
check('recorded', b.speedVotes.c.v == 2)
G.vote(b, 'c', 4, 100.2, 6)
check('a later click replaces it', b.speedVotes.c.v == 4)
G.vote(b, 'c', 1, 100.0, 7)
check('an earlier click stamped late does not', b.speedVotes.c.v == 4 and 'that one stands' in G.lastLog())
G.vote(b, 'c', 3, 100.2, 8)
check('the same tick: the later seq wins', b.speedVotes.c.v == 3)
G.vote(b, 'c', 1.5, 250.0, 1)
check('a restarted game (seq from 1 again) still counts: its click is later', b.speedVotes.c.v == 1.5)
G.vote(b, 'c', 'x', 260.0, 2)
check('a bad vote is not counted', b.speedVotes.c.v == 1.5 and 'not counted' in G.lastLog())
run('''
  local CMs = {}
  for _, l in ipairs({ "a", "b", "c" }) do CMs[l] = newCM(l) end
  -- one command stream, applied in stamp order on three games: the older click is stamped later
  local stream = {
    { origin = "b", v = 1, ct = 50.0, seq = 1, at = 50.6 },
    { origin = "c", v = 2, ct = 50.2, seq = 1, at = 50.6 },
    { origin = "b", v = 4, ct = 50.4, seq = 2, at = 50.8 },
    { origin = "b", v = 2, ct = 50.2, seq = 3, at = 51.0 },
  }
  for _, c in ipairs(stream) do
    c.op = "SPEEDVOTE"
    for _, l in ipairs({ "a", "b", "c" }) do CMs[l].execSpeedVote(c) end
  end
  SAME = CMs.a.speedVotes.b.v == 4 and CMs.b.speedVotes.b.v == 4 and CMs.c.speedVotes.b.v == 4
     and CMs.a.speedVotes.c.v == 2 and CMs.b.speedVotes.c.v == 2 and CMs.c.speedVotes.c.v == 2
''')
check('every game applying the same stream holds the same votes', G.SAME)

print('== the leader counts')
a, _ = G.newCM('a')
a.myCeiling = 4
avg, vt, n = a.voteSpeed()
check('before anyone votes: its own speed, marked', avg == 4 and vt == 'a:4*' and n == 1)
G.heard(a, 'b'); G.heard(a, 'c')
G.vote(a, 'b', 1, 10.0, 1)
avg, vt, n = a.voteSpeed()
check('b votes 1 against the host\'s own 4: 2.5', avg == 2.5 and vt == 'a:4*,b:1')
G.vote(a, 'c', 2, 11.0, 1)
avg, vt, n = a.voteSpeed()
check('mean of 4, 1, 2 rounds to 2.35 (and prints as 2.35)', avg == 2.35 and a.lseffLine(avg, vt) == 'LSEFF v=2.35 vt=a:4*,b:1,c:2')
a.ticks = a.peers.c.at + 160
a.peers.b.at = a.ticks
check('a player silent for 160 ticks still counts', a.voteSpeed()[0] == 2.35)
a.ticks = a.ticks + 1
a.peers.b.at = a.ticks
check('one tick more and it stops counting', a.voteSpeed()[1] == 'a:4*,b:1')
check('a vote from a player never heard does not count', (G.vote(a, 'd', 8, 12.0, 1), a.voteSpeed()[1])[1] == 'a:4*,b:1')
G.CMDS = lua.table()
a.peerSeen = True
a.castSpeedVote(2, 'test')
avg, vt, n = a.voteSpeed()
check('the host\'s own click counts on the host at once, before its stamp', avg == 1.5 and vt == 'a:2,b:1')
check('and goes out as a SPEEDVOTE command', len(G.CMDS) == 1 and G.CMDS[1].op == 'SPEEDVOTE' and G.CMDS[1].v == 2 and G.CMDS[1].ct == 1000)
check('in words', a.voteWords('a:4*,b:1,c:2.5') == 'A 4x (own speed), B 1x, C 2.5x')
check('LSEFF without votes is the old line', a.lseffLine(4, '') == 'LSEFF v=4')

print('== a joiner\'s speed controls')
G.CMDS = lua.table()
G.SPEED = 3
b, _ = G.newCM('b')
b.peerSeen, b.effSpeed = True, 2.5
b.speedButton(2, 'button')
check('a speed button is its vote', len(G.CMDS) == 1 and G.CMDS[1].origin == 'b' and G.CMDS[1].v == 2)
check('and moves no lever', G.SPEED == 3 and b.effSpeed == 2.5)
b.speedButton(0, 'toggle')
b.speedButton(0, 'button')
check('its pause does nothing', len(G.CMDS) == 1 and 'only the host pauses' in G.lastLog())
b.speedButton(2, 'toggle')
check('its toggle back does nothing', len(G.CMDS) == 1 and 'only the host resumes' in G.lastLog())
G.SPEED = 0
b.speedButton(4)
check('older slice, game standing at 0: may be the toggle, no vote', len(G.CMDS) == 1)
G.SPEED = 3
b.speedButton(4)
check('older slice, game running: a speed can only be a button, a vote', len(G.CMDS) == 2 and G.CMDS[2].v == 4)
b.guiSpeedSet(3.3)
check('the window\'s speed row is its vote, in quarters', len(G.CMDS) == 3 and G.CMDS[3].v == 3.25)
b.resyncHold = True
check('no vote while a resync holds the game', b.castSpeedVote(2, 'test') is False and len(G.CMDS) == 3)
b.resyncHold = None
b.lgHolding = True
b.speedButton(2, 'button')
check('while the load gate holds, a press is the start override, no vote', b.lgPress == 2 and len(G.CMDS) == 3)
b.lgHolding = None
solo, _ = G.newCM('c')
solo.speedButton(2, 'button')
check('nobody else in the session: applied here, no vote', G.SPEED == 2 and len(G.CMDS) == 3)

print('== the host\'s pause and resume')
G.CMDS = lua.table()
a, _ = G.newCM('a')
a.peerSeen, a.myCeiling, a.effSpeed = True, 4, 2.5
G.heard(a, 'b')
G.vote(a, 'b', 1, 10.0, 1)
a.speedButton(0, 'toggle')
check('its pause: ceiling 0, its own speed kept, no vote', a.myCeiling == 0 and a.ceilBeforePause == 4 and len(G.CMDS) == 0)
avg, vt, n = a.voteSpeed()
check('the votes still show while paused', vt == 'a:4*,b:1')
a.effSpeed = 0
a.speedButton(2, 'toggle')
check('its toggle resumes at the votes\' speed, with its own speed from before the pause, no vote',
      a.myCeiling == 4 and a.effSpeed == 2.5 and G.lastSent() == 'LSEFF v=2.5 vt=a:4*,b:1' and len(G.CMDS) == 0)
a.speedButton(0, 'button')
a.effSpeed = 0
a.speedButton(2, 'button')
check('a speed button while paused: its vote, and the resume counts it at once',
      len(G.CMDS) == 1 and G.CMDS[1].v == 2 and a.effSpeed == 1.5 and G.lastSent() == 'LSEFF v=1.5 vt=a:2,b:1')
a.speedButton(2, 'toggle')
check('a toggle with a speed while running does nothing', len(G.CMDS) == 1 and 'not paused' in G.lastLog())

print('== the controller on the leader: the mean, and /speed until the next vote')
G.CMDS, G.SENT = lua.table(), lua.table()
G.SPEED = 4
a, _ = G.newCM('a')
a.peerSeen, a.myCeiling, a.lastSetSpeed, a.paceApplied = True, 4, 4, True
a.ticks = 100
G.heard(a, 'b'); G.heard(a, 'c')
G.vote(a, 'b', 1, 10.0, 1)
a.paceV2(G.NOW)
check('session speed 2.5, broadcast with its votes', a.effSpeed == 2.5 and G.lastSent() == 'LSEFF v=2.5 vt=a:4*,b:1')
check('lever 3, the fraction in the speed file', G.SPEED == 3 and G.FS['mem://a/tpf2_speed.txt'] == '2.5000\n')
G.FS['mem://a/tpf2_bridge_ctl.txt'] = 'speed=3\n'
a.ticks, a.spdReqAt = a.ticks + 1, None
a.paceV2(G.NOW)
check('/speed 3 in the chat overrides the votes', a.effSpeed == 3 and a.spdReqInForce)
a.ticks = a.ticks + 1
G.vote(a, 'c', 2, 11.0, 1)
a.paceV2(G.NOW)
check('the next vote hands the session back to the votes: 2.35', a.effSpeed == 2.35 and not a.spdReqInForce)
a.speedButton(0, 'toggle')
a.ticks = a.ticks + 1
a.paceV2(G.NOW)
check('the host\'s pause: session speed 0, the votes still listed', a.effSpeed == 0 and G.lastSent() == 'LSEFF v=0 vt=a:4*,b:1,c:2')

print('SPEED VOTES:', 'all checks passed' if failures == 0 else '%d check(s) failed' % failures)
raise SystemExit(1 if failures else 0)
