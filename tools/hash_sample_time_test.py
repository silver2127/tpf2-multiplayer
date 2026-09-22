"""A world hash is a sample at a sim time, not a property of the stamp.

Lua 5.2, no game. Runs the real `checkHash` cut out of lockstep.lua and the real
`CM.compareOne` cut out of net.lua against the numbers measured on the two-instance
rig on 2026-09-16 (%LOCALAPPDATA%\\tpf2mp\\runs\\20260916-155830):

    [ls-a]  mine ... p0@1.8: ... t:8899   (the host, first update after its load)
    [ls-b]  peer ... p0@31.6: ... t:8975   (the joiner, which loaded a save taken at 31.4)
    [ls-b]  !! DESYNC t=0 mine=... peer a=...        <- neither world had done anything wrong

Both samples fell in the same stamp interval (t=0) and both were published under it,
so the detector compared the host's world at sim time 1.8 with the SAME world 30 game
units later and called the 76 extra town buildings a desync. What this pins down:

  - a game that enters a stamp's interval part way through (every game, on the first
    update after a load) takes its hash but does NOT publish it
  - a game that watched its clock cross the stamp publishes as before
  - two details whose sample times differ are not compared at all: no verdict, no
    `$$ TOWN`/`$$ PEOPLE` gap, no town-gap streak, no desync counted
  - two details from the same sim time compare exactly as before, town streak included

Verify the fix BITES: `git stash`-free check is
`git show HEAD:mod/mp_lockstep_1/res/scripts/mp/net.lua` -- the pre-fix source fails
the "different sim times" cases below.

    python tools/hash_sample_time_test.py
"""
import re
import sys
from pathlib import Path

import lupa.lua52 as lupa

REPO = Path(__file__).resolve().parents[1]
HASH = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/hash.lua').read_text(encoding='utf-8')
LOCKSTEP = (REPO / 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua').read_text(
    encoding='utf-8').replace('\r\n', '\n')
NET = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/net.lua').read_text(
    encoding='utf-8').replace('\r\n', '\n')


def cut(src, pattern, what):
    m = re.search(pattern, src, re.S | re.M)
    if not m:
        sys.exit('could not find %s' % what)
    return m.group(1)


CHECK = cut(LOCKSTEP, r'^(local function checkHash\(now\)\n.*?\n^end)\n', 'checkHash in lockstep.lua')
LANEDIFF = cut(NET, r'^(local function logLaneDiff\(dm, dt\)\n.*?\n^end)\n', 'logLaneDiff in net.lua')
COMPARE = cut(NET, r'^(function CM\.compareOne\(stamp, origin, theirs, dt\)\n.*?\n^end)\n',
              'CM.compareOne in net.lua')

L = lupa.LuaRuntime(unpack_returned_tuples=True)
G = L.globals()
G.HASH_SRC, G.CHECK_SRC, G.LANEDIFF_SRC, G.COMPARE_SRC = HASH, CHECK, LANEDIFF, COMPARE
L.execute(r'''
-- a game running the real checkHash over a stub world hash
function newGame(letter, startClock)
  local g = { letter = letter, logs = {}, sent = {}, compared = {}, now = startClock, clock = 0, n = 0 }
  local CM = { peers = {}, ticks = 0, leader = "a", seqNo = 0, rosterPlayers = 2 }
  local K = { INSTANCE = letter, BASE = "mem://" .. letter .. "/", SIM_STEP = 0.2,
              PEER_STALE_TICKS = 25, HASH_EVERY_GAMETIME = 12, STAMP_KEEP = 64 }
  CM.isLeader = function() return K.INSTANCE == CM.leader end
  CM.cfgFlag = function(_, d) return d end
  CM.noteDesync = function() end
  CM.pruneOldest = function() end
  CM.vposShip = function(stamp) g.sent[#g.sent + 1] = "vpos:" .. stamp end
  CM.scheduleLocal = function() end
  CM.hashCadenceTick = function() end
  CM.broadcast = function(line) g.sent[#g.sent + 1] = line end
  CM.compareAt = function(stamp) g.compared[#g.compared + 1] = stamp end
  CM.myHashes, CM.myDetails, CM.comparedAt, CM.vposDone = {}, {}, {}, {}
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  g.hash = assert(load(HASH_SRC, "@hash.lua"))()(CM, K, log)
  local fakeOs = { clock = function() return g.clock end }
  -- a distinct hash per sample, so a published one can be told from another's
  local function worldHash(now)
    g.clock = g.clock + 0.05
    g.n = g.n + 1
    return string.format("h%s@%.1f", letter, now), string.format("v0,c0:c,e1:e,z:z,p0@%.1f:p,r0:x/y,m:-,l:-,t:%d,n:%d", now, 100, 5)
  end
  g.checkHash = assert(load("local CM, K, log, worldHash, os = ...\nlocal lastHashAt = nil\n" .. CHECK_SRC ..
    "\nreturn checkHash", "@checkHash"))(CM, K, log, worldHash, fakeOs)
  g.CM, g.K = CM, K
  return g
end

-- one update per sim step, exactly as the engine calls it (STEPS: 1 step/update
-- at speed 1 and at speed 4 alike, measured 2026-09-16)
function step(g, ticks)
  for _ = 1, ticks do
    g.CM.ticks = g.CM.ticks + 1
    g.checkHash(g.now)
    g.now = tonumber(string.format("%.1f", g.now + 0.2))
  end
end

-- the solo rule (2026-09-17): a game whose lobby says one player and that hears nobody takes no
-- sample; a peer's heartbeat (or a roster of two) turns the hash on at the next real crossing
function soloStory()
  local g = newGame("s", 0.0)
  g.CM.rosterPlayers = 1
  step(g, 130)                                   -- 26 units alone: stamps 12 and 24 pass unsampled
  local aloneN, alonePub = g.n, published(g)
  g.CM.peers.b = { at = g.CM.ticks }             -- a peer is heard at 26.0, mid-interval...
  for _ = 1, 130 do                              -- ...and keeps its heartbeats coming; to 52.0: crossings at 36 and 48
    g.CM.peers.b.at = g.CM.ticks
    step(g, 1)
  end
  return aloneN, alonePub, g.n, published(g), logged(g, "world hash is off until one joins"), logged(g, "world hash is on")
end

function published(g)
  local out = {}
  for _, line in ipairs(g.sent) do
    local t = line:match("^LSHASH t=(%d+)")
    if t then out[#out + 1] = t end
  end
  return table.concat(out, ",")
end
function logged(g, text)
  for _, l in ipairs(g.logs) do if l:find(text, 1, true) then return true end end
  return false
end
function nlogged(g, text)
  local n = 0
  for _, l in ipairs(g.logs) do if l:find(text, 1, true) then n = n + 1 end end
  return n
end

-- the comparison side: the real CM.compareOne over stub peers
function newCompare(letter)
  local g = { logs = {} }
  local CM = { peers = {}, ticks = 0, desyncs = 0, myHashes = {}, myDetails = {}, comparedAt = {} }
  local K = { INSTANCE = letter }
  CM.peerFor = function(o)
    CM.peers[o] = CM.peers[o] or { hashes = {}, details = {}, streak = 0 }
    return CM.peers[o]
  end
  CM.noteDesync = function(why, stamp) CM.desyncs = CM.desyncs + 1; CM.lastWhy = why; CM.lastT = stamp end
  local function log(msg) g.logs[#g.logs + 1] = tostring(msg) end
  g.compare = assert(load("local CM, K, log = ...\n" .. LANEDIFF_SRC .. "\n" .. COMPARE_SRC ..
    "\nreturn CM.compareOne", "@compareOne"))(CM, K, log)
  g.CM, g.log = CM, log
  return g
end
function detail(sample, town, people, edges)
  return string.format("v0,c0:0018652614-0018652632,e%d:1200945272-1791406417,z:0590944318-2051440049," ..
    "p0@%.1f:0018652614-0018652632,r0:a/b,m:10000000,l:10000000,t:%d,n:%d", edges, sample, town, people)
end
function runCompare(g, stamp, mineHash, mineDetail, theirHash, theirDetail, origin)
  g.CM.myHashes[stamp] = mineHash
  g.CM.myDetails[stamp] = mineDetail
  g.CM.comparedAt[stamp] = g.CM.comparedAt[stamp] or {}
  g.compare(stamp, origin, theirHash, theirDetail)
end
''')

fails = []


def check(name, cond, extra=''):
    print(('ok   ' if cond else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not cond:
        fails.append(name)


print('== a sample from the middle of an interval is taken but never published')
# the host of 2026-09-16: its world starts at 0, its first update is at 0.2, and the
# stamp interval [0,12) began before this game existed in it
host = G.newGame('a', 0.2)
G.step(host, 3)
check('the first, mid-interval sample is not published', G.published(host) == '',
      G.published(host))
check('it is still taken, and said so once', G.nlogged(host, 'not comparable with anyone') == 1)
check('and it still reaches the dash', host.CM.dashLastDetail is not None)
check('nothing is compared for it', len(host.compared) == 0)
G.step(host, 62)   # 0.8 .. 13.0: crosses 12 at 12.0
check('the stamp whose crossing it watched is published', G.published(host) == '12',
      G.published(host))
check('and compared', len(host.compared) == 1 and host.compared[1] == 12, str(host.compared[1]))
check('published at the stamp itself, not a step past it',
      G.logged(host, '') and 'p0@12.0' in host.CM.myDetails[12], host.CM.myDetails[12])

print('== the joiner, whose world starts at the save\'s own clock')
# it loaded a save taken at 31.4; its first update reads 31.6, inside [24,36)
joiner = G.newGame('b', 31.6)
G.step(joiner, 3)
check('its mid-interval sample is not published either', G.published(joiner) == '',
      G.published(joiner))
G.step(joiner, 25)   # 32.2 .. 37.0: crosses 36
check('the first stamp it crosses is published', G.published(joiner) == '36', G.published(joiner))
check('so the two games never publish the same incomparable stamp',
      set(G.published(host).split(',')).isdisjoint(set(G.published(joiner).split(','))))

print('== two samples of one stamp from different sim times are not compared')
c = G.newCompare('a')
# exactly the rig's numbers: host at 1.8 with 8899 town buildings, joiner at 31.6 with 8975
G.runCompare(c, 0, 'mineHash', G.detail(1.8, 8899, 25, 22680),
             'peerHash', G.detail(31.6, 8975, 1130, 22695), 'b')
check('no desync counted', c.CM.desyncs == 0)
check('it says why it skipped', G.logged(c, 'from different sim times (1.8 vs 31.6) -- not comparable, skipped'))
check('no town gap reported', not G.logged(c, '$$ TOWN'))
check('no people gap reported', not G.logged(c, '$$ PEOPLE'))
check('no verdict logged, not even a LAG', not G.logged(c, 'DESYNC') and not G.logged(c, 'SYNC t=')
      and not G.logged(c, '~~ LAG t=0'))
check('the town streak is untouched', c.CM.townGapStreak is None)

print('== the same stamp from the same sim time still compares, exactly as before')
c2 = G.newCompare('a')
G.runCompare(c2, 144, 'same', G.detail(144.0, 9208, 4110, 22754),
             'same', G.detail(144.0, 9208, 4110, 22754), 'b')
check('an agreeing pair is SYNC', G.logged(c2, 'SYNC t=144'))
check('and no desync', c2.CM.desyncs == 0)

c3 = G.newCompare('a')
# the real divergence of the same session: same sim time, town lane 2 then 3 apart
G.runCompare(c3, 1152, 'mineA', G.detail(1152.0, 9515, 4691, 22980),
             'peerB', G.detail(1152.0, 9513, 4743, 22981), 'b')
check('a real mismatch still logs the town gap', G.logged(c3, '$$ TOWN t=1152'))
check('and the lag line', G.logged(c3, '~~ LAG t=1152'))
G.runCompare(c3, 1536, 'mineA2', G.detail(1536.0, 9621, 4693, 22998),
             'peerB2', G.detail(1536.0, 9618, 4747, 23000), 'b')
check('two town gaps in a row are still a desync', G.logged(c3, '!! DESYNC (town buildings) t=1536'))
check('and it is counted', c3.CM.desyncs >= 1, str(c3.CM.desyncs))

print('== a peer that sends no p lane at all is still compared (old builds)')
c4 = G.newCompare('a')
G.runCompare(c4, 288, 'x', G.detail(288.0, 100, 5, 10), 'x', 'v0,c0:c,e10:e,t:100,n:5', 'b')
check('no sample time on one side means compare as before', G.logged(c4, 'SYNC t=288'))

print('== alone, no hash; a peer turns it on at the next real crossing')
aloneN, alonePub, laterN, laterPub, saidOff, saidOn = G.soloStory()
check('26 units alone take no sample and publish nothing', aloneN == 0 and alonePub == '', f'{aloneN} {alonePub!r}')
check('it says so once', saidOff)
check('with a peer heard mid-interval, the next crossings are sampled and published (36, 48)',
      laterPub == '36,48', repr(laterPub))
check('the interval the peer arrived in is not published (entered, not crossed)', '24' not in laterPub.split(','))
check('and it says the hash is on', saidOn)

print()
if fails:
    print('FAILED: ' + ', '.join(fails))
    sys.exit(1)
print('all checks passed')
