"""The auto command delay against freezes and a real latency change, no game.

Runs the real net.lua -- the working tree, and a git ref for comparison -- on one game (a) that
exchanges heartbeats with a simulated peer (b) over a link with a set one-way latency. Both
games tick at a fixed rate; a frozen game ticks at nothing until its freeze ends, so pings and
echoes wait in flight exactly as they would behind an autosave. Heartbeats go through the real
CM.pollEvents, and CM.execDelayTick runs every tick.

  autosaves_4x   the players' session of 2026-09-13 (1,745 delay changes): a ~390 ms round trip
                 at 4x, a ~1 s autosave on both games every 5 minutes, a short hash freeze every
                 15 s, the sim rate wandering 3.92-4.12 u/s
  latency_step   the same link without freezes; the round trip rises to ~690 ms for good at 5 min
  one_pc_1x      speed 1, a 1 ms network: the round trip is the two games' tick waits

    python tools/delay_noise_sim.py                # working tree vs HEAD
    python tools/delay_noise_sim.py --ref <commit>
"""
import argparse
import os
import subprocess
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = 'mod/mp_lockstep_1/res/scripts/mp/net.lua'


def source(which):
    if which == 'work':
        return open(os.path.join(REPO, NET), encoding='utf-8').read()
    return subprocess.check_output(['git', '-C', REPO, 'show', which + ':' + NET]).decode('utf-8')


HARNESS = r'''
CLK = 0.0
os.clock = function() return CLK end
LOGS = {}
K = {
  INSTANCE = "a", SIM_STEP = 0.2, EXEC_DELAY = 0.4, EXEC_DELAY_MIN = 0.4, EXEC_DELAY_MAX = 3.0,
  DELAY_SLACK_MS = 50, DELAY_DEV_MULT = 1, DELAY_DOWN_TICKS = 25, RTT_MIN_SAMPLES = 8, PEER_STALE_TICKS = 25,
  GAP_HOLD_GRACE_TICKS = 1, GAP_HOLD_ENGAGE_TICKS = 3, GAP_HOLD_MAX_TICKS = 55,
  NACK_MAX = 10, CMD_RING = 256, HIST_RING = 64, EVENTS_FILE = EVENTS,
}
CM = { ticks = 1000, peers = {}, seqNo = 0, queue = {}, MAX_LEAD = 15, execDelayAuto = true }
function CM.gameTime() return CLK * 4 end
function CM.stepOf(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
function CM.peerFor(o) local pr = CM.peers[o]; if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end; return pr end
function CM.peerBounds() return nil, nil end
function CM.broadcast() end
function CM.cmEnsure() end
function CM.readFrom(path, offset)
  local f = io.open(path, "r")
  if not f then return nil, offset end
  local size = f:seek("end")
  if offset >= size then f:close(); return nil, offset end
  f:seek("set", offset)
  local data = f:read("*a") or ""
  f:close()
  return data, offset + #data
end
assert(load(NET_SRC, "@net.lua"))()(CM, K, function(s) LOGS[#LOGS + 1] = s end)
CM.eventsOffset = 0

local seed = 12345
local function rnd() seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648 end
local function frozenUntil(list, t)
  for _, f in ipairs(list) do if t >= f[1] and t < f[1] + f[2] then return f[1] + f[2] end end
  return nil
end

function RUN(sc)
  CM.effSpeed = sc.speed
  local rate, dt = sc.rate, sc.tick
  local toPeer, toUs = {}, {}
  local usTick, peerTick, nextUs, nextPeer = 0, 0, 0, dt / 2
  local lastMs, lastMsAt
  local st = { n = 0, sum = 0, peak = 0, above = 0, series = {} }
  local ev = io.open(K.EVENTS_FILE, "w"); ev:close()
  while math.min(nextUs, nextPeer) < sc.duration do
    if nextUs <= nextPeer then
      local t = nextUs
      local fz = frozenUntil(sc.usFreeze, t)
      if fz then nextUs = fz else
        CLK = t
        local keep, arrived = {}, {}
        for _, b in ipairs(toUs) do if b.at <= t then arrived[#arrived + 1] = b.line else keep[#keep + 1] = b end end
        toUs = keep
        if #arrived > 0 then
          local f = io.open(K.EVENTS_FILE, "a"); f:write(table.concat(arrived, "\n"), "\n"); f:close()
          CM.pollEvents()
        end
        usTick, CM.ticks = usTick + 1, CM.ticks + 1
        rate = math.max(sc.rateLo, math.min(sc.rateHi, rate + (rnd() - 0.5) * sc.rateStep))
        CM.simRate = rate
        CM.execDelayTick()
        if usTick % 2 == 0 then toPeer[#toPeer + 1] = { at = t + sc.latency(t) + rnd() * 0.004, ms = math.floor(t * 1000) } end
        local d = CM.execDelayCur or 0
        st.n, st.sum = st.n + 1, st.sum + d
        if d > st.peak then st.peak = d end
        if d > sc.steady + 1e-9 then st.above = st.above + 1 end
        st.series[#st.series + 1] = { t, d }
        nextUs = t + dt
      end
    else
      local t = nextPeer
      local fz = frozenUntil(sc.peerFreeze, t)
      if fz then nextPeer = fz else
        local keep = {}
        for _, p in ipairs(toPeer) do if p.at <= t then lastMs, lastMsAt = p.ms, t else keep[#keep + 1] = p end end
        toPeer = keep
        peerTick = peerTick + 1
        if peerTick % 2 == 0 then
          local line = string.format("LSTICK t=%d o=b s=%d hi=0 ms=%d", math.floor(t * 4), math.floor(t * 20), math.floor(t * 1000))
          if lastMs then line = line .. string.format(" e=a:%d:%d", lastMs, math.floor((t - lastMsAt) * 1000 + 0.5)) end
          toUs[#toUs + 1] = { at = t + sc.latency(t) + rnd() * 0.004, line = line }
        end
        nextPeer = t + dt
      end
    end
  end
  local changes = 0
  for _, l in ipairs(LOGS) do if l:find("EXEC_DELAY auto:", 1, true) then changes = changes + 1 end end
  st.changes = changes
  return st
end
'''


def every(period, length, until, offset=0.0):
    """(start, length) freezes every `period` seconds, the first at `period` + `offset`."""
    out, t = [], period
    while t < until:
        out.append((t + offset, length))
        t += period
    return out


def lua_spans(spans):
    return '{' + ', '.join('{%g, %g}' % s for s in spans) + '}'


def scenarios():
    d = 1500
    # an autosave every 5 minutes on both games (b's a little longer), a 70 ms hash every 15 s
    us = lua_spans(every(300, 0.95, d) + every(15, 0.07, d, 2))
    peer = lua_spans(every(300, 1.0, d) + every(15, 0.07, d, 7))
    return {
        'autosaves_4x': '''{ duration = %d, tick = 0.05, speed = 4, rate = 4.0, rateLo = 3.92, rateHi = 4.12, rateStep = 0.02,
            steady = 1.2, latency = function() return 0.17 end, usFreeze = %s, peerFreeze = %s }''' % (d, us, peer),
        'latency_step': '''{ duration = 600, tick = 0.05, speed = 4, rate = 4.0, rateLo = 3.92, rateHi = 4.12, rateStep = 0.02,
            steady = 1.6, latency = function(t) if t < 300 then return 0.17 end return 0.32 end, usFreeze = {}, peerFreeze = {} }''',
        'one_pc_1x': '''{ duration = 300, tick = 0.185, speed = 1, rate = 0.9, rateLo = 0.88, rateHi = 0.92, rateStep = 0.004,
            steady = 0.4, latency = function() return 0.001 end, usFreeze = {}, peerFreeze = {} }''',
    }


def run(which, name, sc):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    G = L.globals()
    G.NET_SRC = source(which)
    G.EVENTS = os.path.join(tempfile.mkdtemp(), 'events.txt').replace('\\', '/')
    L.execute(HARNESS)
    st = G.RUN(L.eval(sc))
    series = [(st.series[i][1], st.series[i][2]) for i in range(1, len(st.series) + 1)]
    return {'changes': st.changes, 'peak': st.peak, 'mean': st.sum / max(1, st.n), 'above': st.above / max(1, st.n),
            'series': series}


def main():
    ap = argparse.ArgumentParser(description='auto command delay vs freezes, offline')
    ap.add_argument('--ref', default='HEAD')
    ap.add_argument('--only', choices=sorted(scenarios()))
    args = ap.parse_args()
    fails = 0
    for name, sc in scenarios().items():
        if args.only and name != args.only:
            continue
        print('=' * 20, name)
        res = {}
        for which in (args.ref, 'work'):
            r = run(which, name, sc)
            res[which] = r
            extra = ''
            if name == 'latency_step':
                final = r['series'][-1][1]
                react = next((t for t, dly in r['series'] if t >= 300 and dly >= final - 1e-9), None)
                r['react'] = (react - 300) if react is not None else None
                extra = '  reaches its new %.1f %.1f s after the step' % (final, r['react'] if react is not None else -1)
            print('  %-8s delay changes %4d  peak %.1f  mean %.2f  time above the steady value %5.1f%%%s'
                  % (which, r['changes'], r['peak'], r['mean'], 100 * r['above'], extra))
        w, ref = res['work'], res[args.ref]
        checks = []
        if name == 'autosaves_4x':
            checks = [('no autosave or hash freeze lifts the delay (peak at most 1.2)', w['peak'] <= 1.2 + 1e-9),
                      ('it settles instead of flapping (at most 4 changes in 25 minutes)', w['changes'] <= 4),
                      ('fewer changes than %s' % args.ref, w['changes'] < ref['changes'] or ref['changes'] <= 4)]
        elif name == 'latency_step':
            # ~690 ms round trip: 345 + deviation + 50 ms one way at ~4 u/s = 1.6 units (from 1.2)
            checks = [('a real latency rise still lifts it (from 1.2 to 1.6)', w['series'][-1][1] >= 1.6 - 1e-9),
                      ('and about as fast as before (within 1 s of %s, at most 5 s)' % args.ref,
                       w['react'] is not None and w['react'] <= 5 and (ref.get('react') is None or w['react'] <= ref['react'] + 1))]
        elif name == 'one_pc_1x':
            checks = [('one PC stays at the 0.4 floor', w['peak'] <= 0.4 + 1e-9)]
        for label, ok in checks:
            fails += 0 if ok else 1
            print('  %s  %s' % ('OK  ' if ok else 'FAIL', label))
    print('DELAY SIM:', 'all checks passed' if fails == 0 else '%d check(s) failed' % fails)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
