"""Closed-loop pacing simulation, no game needed.

Runs the real mod/.../scripts/mp/pacing.lua and the clock helpers from
lockstep.lua for several instances in one Lua 5.1 runtime (lupa), against a toy
engine: the speed lever, the fractional-speed dither clamped to 0.25-2x of the
lever as native/src/speedhook.cpp does, a 4x hardware cap, one tick of command
latency and one tick of network latency. The leader answers LSNEED; nobody
answers the leader's own. Not modelled: the load gate, commands, real frame
timing (ticks are a fixed 1/5.4 s, 1x is 1 game unit per second).

Each scenario runs on the working tree (checked) and on a git ref (printed for
comparison, default HEAD). Built 2026-09-10 to reproduce the live failure where
joiners raced ahead and the leader held itself at speed 0 "catching up".

    python tools/pacing_sim.py                 # working tree vs HEAD
    python tools/pacing_sim.py --ref 10a1d32   # vs an older build
    python tools/pacing_sim.py --only speed_drop

Per-run logs (every PACE/PID/CATCHUP line) go to %TEMP%/pacing_sim/.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import lupa.lua51 as L51

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.join(tempfile.gettempdir(), 'pacing_sim')
PACING = 'mod/mp_lockstep_1/res/scripts/mp/pacing.lua'
LOCKSTEP = 'mod/mp_lockstep_1/res/config/game_script/lockstep.lua'


def sources(which):
    if which == 'work':
        p = open(os.path.join(REPO, PACING), 'rb').read()
        l = open(os.path.join(REPO, LOCKSTEP), 'rb').read()
    else:
        p = subprocess.check_output(['git', '-C', REPO, 'show', which + ':' + PACING])
        l = subprocess.check_output(['git', '-C', REPO, 'show', which + ':' + LOCKSTEP])
    return p.decode('utf-8').replace('\r\n', '\n'), l.decode('utf-8').replace('\r\n', '\n')


def helpers(lockstep):
    out = []
    for nm in ['isLeader', 'livePeers', 'peerSlowPrecise', 'peerFastPrecise', 'peerBounds', 'leaderPrecise', 'heartbeatCu']:
        i = lockstep.find('\nfunction CM.' + nm + '(')
        if i < 0:
            continue
        i += 1
        eol = lockstep.find('\n', i)
        first = lockstep[i:eol]
        if first.rstrip().endswith(' end'):
            out.append(first)
            continue
        j = lockstep.find('\nend\n', i)
        out.append(lockstep[i:j + 4])
    return 'return function(CM, K)\n' + '\n'.join(out) + '\nend\n'


PRELUDE = r'''
SIM = { tick = 0, logs = {}, fs = {}, outbox = {}, histDue = {}, byLetter = {}, cfg = {}, series = {} }
local real_open = io.open
io.open = function(path, mode)
  mode = mode or "r"
  if type(path) == "string" and path:sub(1, 6) == "mem://" then
    if mode:find("w") then
      local buf, f = {}, {}
      function f:write(...) for _, v in ipairs({...}) do buf[#buf + 1] = tostring(v) end return self end
      function f:close() SIM.fs[path] = table.concat(buf) end
      return f
    end
    local body = SIM.fs[path]
    if not body then return nil end
    local f = {}
    function f:read(fmt) if fmt == "*l" then return body:match("^[^\n]*") end return body end
    function f:close() end
    return f
  end
  return real_open(path, mode)
end
os.remove = function(path) SIM.fs[path] = nil return true end
game = { interface = { getGameSpeed = function() return SIM.cur.lever end } }
api = { cmd = { make = { setGameSpeed = function(v) return { speed = v } end },
                sendCommand = function(c) SIM.cur.pendingLever = c.speed; SIM.cur.pendingAt = SIM.tick + 1 end } }

function newInst(spec)
  local I = { letter = spec.letter, T = spec.T0, lever = spec.lever or 4, startTick = spec.start or 1 }
  local CM, K = {}, {}
  I.CM, I.K = CM, K
  K.INSTANCE = spec.letter
  K.BASE = "mem://" .. spec.letter .. "/"
  K.SIM_STEP = 0.2
  K.PEER_STALE_TICKS = 25
  K.HEARTBEAT_EVERY = 2
  K.EXEC_DELAY = 0.6
  K.BARRIER_AHEAD = 8.0
  K.GAP_GRACE_TICKS = 3
  CM.peers, CM.ticks, CM.leader, CM.seqNo, CM.cfgCache = {}, 0, "a", 0, {}
  CM.cfgFlag = function(key, default)
    local v = SIM.cfg[key]
    if v == nil then return default end
    CM.cfgCache[key] = v
    return v ~= "0"
  end
  CM.broadcast = function(line) SIM.outbox[#SIM.outbox + 1] = { from = spec.letter, line = line } end
  CM.rxGaps = function() return 0, 0, nil, nil end
  CM.gameTime = function() return I.T end
  CM.stepOf = function(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
  local function log(msg) SIM.logs[#SIM.logs + 1] = string.format("%5d %s: %s", SIM.tick, spec.letter, tostring(msg)) end
  HELPERS(CM, K)
  if not CM.heartbeatCu then
    -- the build before 2026-09-10: measured against the fastest peer, on every instance
    CM.heartbeatCu = function(now)
      local fp = CM.peerFastPrecise()
      CM.farBehind = (fp ~= nil) and (fp - now) > K.CATCHUP_MIN
      return (CM.catchingUp2 or CM.farBehind) and true or false
    end
  end
  SIM.cur = I
  FACTORY(CM, K, log)
  for _, nm in ipairs({ "hostUnpause", "syncBegin", "syncEnd" }) do
    if not CM[nm] then CM[nm] = function() end; SIM.stubbed = (SIM.stubbed or "") .. nm .. " " end
  end
  CM.lastSetSpeed, CM.paceApplied, CM.paceSetTick = I.lever, true, -100
  if spec.ceil then CM.myCeiling = spec.ceil end
  SIM.series[spec.letter] = {}
  return I
end

local function advance(I, dt)
  if I.pendingLever and SIM.tick >= I.pendingAt then I.lever = I.pendingLever; I.pendingLever = nil end
  local L = I.lever
  if L <= 0 then return 0 end
  local rate = L
  local d = tonumber(SIM.fs["mem://" .. I.letter .. "/tpf2_speed.txt"] or "")
  if d and d > 0 then
    local m = d / L
    if m < 0.25 then m = 0.25 elseif m > 2.0 then m = 2.0 end
    rate = L * m
  end
  if rate > SIM.hwMax then rate = SIM.hwMax end
  I.T = I.T + rate * dt
  return rate
end

local function deliver(msg, insts, tick)
  local op = msg.line:match("^(%u+)")
  for _, R in ipairs(insts) do
    if R.letter ~= msg.from and tick >= R.startTick then
      local CM = R.CM
      if op == "LSTICK" then
        local pr = CM.peers[msg.from] or {}
        CM.peers[msg.from] = pr
        pr.time = tonumber(msg.line:match("t=([%d%.%-]+)")); pr.at = CM.ticks; pr.clk = os.clock()
        pr.step = tonumber(msg.line:match(" s=(%-?%d+)"))
        pr.ceil = tonumber(msg.line:match(" ceil=(%d+)"))
        pr.cu = (msg.line:find(" cu=1", 1, true) ~= nil)
        CM.peerSeen = true
      elseif op == "LSEFF" then
        if not CM.lgHolding then
          local v = tonumber(msg.line:match("v=([%d%.]+)"))
          if v then
            CM.effSpeed = v; CM.baseSpeed = v
            if v > 0 and CM.myCeiling == 0 then CM.myCeiling = v end
          end
        end
      elseif op == "LSNEED" then
        -- only the leader serves the history ring; nobody answers the leader's own request
        if CM.isLeader() then SIM.histDue[#SIM.histDue + 1] = { to = msg.from, at = tick + 2 } end
      end
    end
  end
end

function SIM.run(sc)
  SIM.cfg = sc.cfg or {}
  SIM.hwMax = sc.hwMax or 4
  local dt = 1 / 5.4
  local insts = {}
  for _, spec in ipairs(sc.insts) do
    local I = newInst(spec)
    insts[#insts + 1] = I
    SIM.byLetter[spec.letter] = I
  end
  local A = SIM.byLetter.a
  local M = { leaderZero = 0, aCatchup = 0, ratesAbove = 0 }
  for tick = 1, sc.ticks do
    SIM.tick = tick
    for _, act in ipairs(sc.actions or {}) do
      if act.tick == tick then
        local I = SIM.byLetter[act.who]
        if act.kind == "lever" then I.lever = act.value
        elseif act.kind == "req" then SIM.fs["mem://" .. act.who .. "/tpf2_bridge_ctl.txt"] = "speed=" .. tostring(act.value) .. "\n" end
      end
    end
    for _, I in ipairs(insts) do
      if tick >= I.startTick then
        SIM.cur = I
        I.rate = advance(I, dt)
        local CM = I.CM
        CM.ticks = CM.ticks + 1
        local now = I.T
        if CM.ticks % 2 == 0 then
          CM.broadcast(string.format("LSTICK t=%d o=%s s=%d hi=0 ceil=%d%s", math.floor(now), I.letter, CM.stepOf(now),
            CM.myCeiling or 4, CM.heartbeatCu(now) and " cu=1" or ""))
        end
        CM.lgHolding = false
        local paceTick = CM.paceTick or CM.applyBarrier   -- applyBarrier in builds before remove-legacy
        paceTick(now)
      end
    end
    local box = SIM.outbox
    SIM.outbox = {}
    for _, msg in ipairs(box) do deliver(msg, insts, tick) end
    for idx = #SIM.histDue, 1, -1 do
      local h = SIM.histDue[idx]
      if tick >= h.at then SIM.byLetter[h.to].CM.histEndSeen = true; table.remove(SIM.histDue, idx) end
    end
    if A.lever == 0 then M.leaderZero = M.leaderZero + 1 end
    for _, I in ipairs(insts) do
      if I ~= A then
        SIM.series[I.letter][tick] = (tick >= I.startTick) and (I.T - A.T) or 0
        -- a joiner AHEAD of the leader by more than a unit that still runs faster than it
        if tick >= I.startTick and I.T - A.T > 1.0 and (I.rate or 0) > (A.rate or 0) + 1e-9 then M.ratesAbove = M.ratesAbove + 1 end
      end
    end
  end
  for _, l in ipairs(SIM.logs) do if l:find(" a: CATCHUP", 1, true) then M.aCatchup = M.aCatchup + 1 end end
  return M
end
'''

SCENARIOS = {
    # the live start of 2026-09-10: the leader starts first at 4x, joiners load 8-17 units behind,
    # then the host player drops to 1x, back to 4x, then 2x
    'live_start': '''{ ticks = 1400,
        insts = { {letter="a", T0=3563.8, lever=4, start=1}, {letter="b", T0=3563.2, lever=4, start=12},
                  {letter="c", T0=3563.2, lever=4, start=20}, {letter="d", T0=3563.2, lever=4, start=24} },
        actions = { {tick=150, who="a", kind="lever", value=1}, {tick=600, who="a", kind="lever", value=4},
                    {tick=900, who="a", kind="lever", value=2} } }''',
    # a joiner 50 units AHEAD of a 1x leader
    'far_ahead': '''{ ticks = 1200,
        insts = { {letter="a", T0=1000, lever=1, ceil=1, start=1}, {letter="b", T0=1050, lever=1, start=1} } }''',
    # a joiner in step at 4x when the host drops the session to 1x
    'speed_drop': '''{ ticks = 900,
        insts = { {letter="a", T0=2000, lever=4, start=1}, {letter="b", T0=2000, lever=4, start=1} },
        actions = { {tick=200, who="a", kind="lever", value=1} } }''',
    # a hot joiner 40 units behind a 1x session
    'hot_join_1x': '''{ ticks = 900,
        insts = { {letter="a", T0=1000, lever=1, ceil=1, start=1}, {letter="b", T0=960, lever=1, start=1} } }''',
    # the same at 2x
    'hot_join_2x': '''{ ticks = 900,
        insts = { {letter="a", T0=1000, lever=2, ceil=2, start=1}, {letter="b", T0=960, lever=2, start=1} } }''',
    # the host clicks 1x while its only joiner is still catching up (every peer has cu=1)
    'click_during_catchup': '''{ ticks = 700,
        insts = { {letter="a", T0=1000, lever=4, start=1}, {letter="b", T0=980, lever=4, start=1} },
        actions = { {tick=100, who="a", kind="lever", value=1} } }''',
    # the host pauses while its only joiner is still catching up, and presses play later
    'pause_during_catchup': '''{ ticks = 900,
        insts = { {letter="a", T0=1000, lever=1, ceil=1, start=1}, {letter="b", T0=900, lever=1, start=1} },
        actions = { {tick=60, who="a", kind="lever", value=0}, {tick=300, who="a", kind="lever", value=1} } }''',
}


def lua_to_py(t):
    if L51.lua_type(t) == 'table':
        return {k.decode() if isinstance(k, bytes) else k: lua_to_py(v) for k, v in t.items()}
    return t.decode() if isinstance(t, bytes) else t


def run(which, name):
    pacing, lockstep = sources(which)
    rt = L51.LuaRuntime(encoding=None)
    load = rt.eval(b'function(src, name) local f, e = loadstring(src, name); if not f then error(e) end; return f() end')
    rt.globals().FACTORY = load(pacing.encode(), b'@pacing.lua')
    rt.globals().HELPERS = load(helpers(lockstep).encode(), b'@lockstep_helpers.lua')
    rt.execute(PRELUDE.encode())
    sc = rt.eval(SCENARIOS[name].encode())
    m = lua_to_py(rt.eval(b'SIM.run')(sc))
    series = lua_to_py(rt.eval(b'SIM.series'))
    logs = [l.decode() for l in rt.eval(b'SIM.logs').values()]
    os.makedirs(LOGDIR, exist_ok=True)
    with open(os.path.join(LOGDIR, 'sim_%s_%s.log' % (which.replace('/', '_'), name)), 'w') as f:
        f.write('\n'.join(logs))
    stubbed = rt.eval(b'SIM.stubbed')
    return m, series, logs, stubbed


def summarize(m, series, from_tick=1):
    out = {}
    for letter, s in sorted(series.items()):
        vals = [(t, e) for t, e in s.items() if t >= from_tick]
        if not vals:
            continue   # the leader has no series of its own
        out[letter] = {
            'max_ahead': max(e for _, e in vals),
            'max_behind': -min(e for _, e in vals),
            'end': s[max(s)],
            'last_out': max([t for t, e in vals if abs(e) >= 1.5], default=0),
        }
    return out


def main():
    ap = argparse.ArgumentParser(description='closed-loop pacing simulation')
    ap.add_argument('--ref', default='HEAD', help='git ref to compare against (default HEAD)')
    ap.add_argument('--only', choices=sorted(SCENARIOS), help='run one scenario')
    args = ap.parse_args()
    failures = 0
    for name in SCENARIOS:
        if args.only and name != args.only:
            continue
        print('=' * 20, name)
        res = {}
        for which in (args.ref, 'work'):
            m, series, logs, stubbed = run(which, name)
            res[which] = (m, series, logs)
            if stubbed:
                print('  (stubbed: %s)' % stubbed.decode())
            print('  %-8s leader held at 0 for %d tick(s), leader catch-up lines %d, ahead-and-faster ticks %d'
                  % (which, m['leaderZero'], m['aCatchup'], m['ratesAbove']))
            for letter, st in summarize(m, series).items():
                print('       %s: max ahead %+.1f  max behind %.1f  end %+.2f  last tick off by 1.5+ = %d'
                      % (letter, st['max_ahead'], st['max_behind'], st['end'], st['last_out']))
        m, series, logs = res['work']
        if name.startswith('pause'):   # the host's own pause is supposed to hold it at 0
            checks = [('the leader never enters catch-up', m['aCatchup'] == 0)]
        else:
            checks = [('the leader never holds', m['leaderZero'] == 0 and m['aCatchup'] == 0)]
        if name == 'live_start':
            st = summarize(m, series)
            checks += [('no joiner ever more than 3.5 ahead', all(v['max_ahead'] <= 3.5 for v in st.values())),
                       ('all within 1.5 of the leader by tick 450 and after', all(v['last_out'] <= 450 for v in st.values()))]
        elif name == 'far_ahead':
            st = summarize(m, series)['b']
            post = summarize(m, series, st['last_out'] + 1)['b'] if st['last_out'] < 1200 else None
            checks += [('50 ahead closes by tick 500', st['last_out'] <= 500),
                       ('no overshoot behind after closing (<1.5)', post is not None and post['max_behind'] < 1.5)]
        elif name == 'speed_drop':
            st = summarize(m, series, 200)['b']
            checks += [('no runaway after 4x -> 1x (max ahead < 2)', st['max_ahead'] < 2.0)]
        elif name == 'click_during_catchup':
            reg = [int(l.split()[0]) for l in logs if ' a: SPEED2: player ceiling -> 1' in l]
            st = summarize(m, series, 101)['b']
            checks += [('the host takes its speed click while the joiner catches up (10 ticks)', bool(reg) and reg[0] <= 110),
                       ('the joiner does not overshoot after it (max ahead < 1.0)', st['max_ahead'] < 1.0)]
        elif name == 'pause_during_catchup':
            reg = [int(l.split()[0]) for l in logs if ' a: SPEED2: player ceiling -> 0' in l]
            undone = [l for l in logs if ' a: SPEED2: running at ' in l and 68 < int(l.split()[0]) < 300]
            st = summarize(m, series, 61)['b']
            checks += [('the host pause registers while the joiner catches up (20 ticks)', bool(reg) and reg[0] <= 80),
                       ('the pause holds until the player presses play', not undone and m['leaderZero'] >= 230),
                       ('the joiner stops at the pause point (max ahead < 1.0)', st['max_ahead'] < 1.0)]
        elif name.startswith('hot_join'):
            st = summarize(m, series)['b']
            post = summarize(m, series, st['last_out'] + 1)['b'] if st['last_out'] < 900 else None
            checks += [('catches up (within 1.5 by tick 600)', st['last_out'] <= 600),
                       ('no overshoot past the leader (max ahead < 1.5)', st['max_ahead'] < 1.5),
                       ('stays in step after', post is not None and post['max_behind'] < 1.5)]
        for label, ok in checks:
            failures += 0 if ok else 1
            print('  %s  %s' % ('OK  ' if ok else 'FAIL', label))
    print('SIM:', 'all checks passed' if failures == 0 else '%d check(s) failed' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
