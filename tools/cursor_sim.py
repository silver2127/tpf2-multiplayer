"""Cursor-circle playback simulation, no game needed.

Runs the real mod/.../scripts/mp/cursors.lua as four module instances in one Lua
runtime (lupa) -- the sender's GUI and script states and the receiver's script and
GUI states -- with in-memory files, two process clocks on different bases, script
ticks about five a second with jitter, GUI frames at 60 fps, and 30-90 ms of network
latency. The receiver's setZone calls are recorded and the drawn circle is compared
with the sender's real cursor path:

  stutter  coefficient of variation of the circle's per-frame movement while the
           cursor sweeps at constant speed (0 = perfectly even)
  stalls   frames in which the circle did not move although the cursor did
  lag      how far behind the real cursor the circle is played, and the mean
           error at that lag

plus a pause (the circle must hold, not glide across it), a jump (no glide across
the map), a cursor that leaves the map (the circle comes down) and a still cursor
(the circle stays up). Each scenario runs on the working tree (checked) and on a
git ref (printed for comparison, default HEAD). Built 2026-09-10 when the circles
were still jumpy after the second pass.

    python tools/cursor_sim.py               # working tree vs HEAD, Lua 5.2 and 5.1
    python tools/cursor_sim.py --ref 11e764d # vs an older build
    python tools/cursor_sim.py --lua 5.2
"""
import argparse
import importlib
import math
import os
import random
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CURSORS = 'mod/mp_lockstep_1/res/scripts/mp/cursors.lua'
FRAME = 1.0 / 60

HARNESS = rb'''
FILES = {}
local fakeio = {}
function fakeio.open(path, mode)
  if mode == "r" then
    local c = FILES[path]
    if not c then return nil end
    local pos = 1
    return {
      read = function(self, what)
        if what == "*a" then local r = c:sub(pos); pos = #c + 1; return r end
        if pos > #c then return nil end
        local e = c:find("\n", pos, true) or (#c + 1)
        local r = c:sub(pos, e - 1); pos = e + 1; return r
      end,
      close = function() end }
  end
  local buf = {}
  return { write = function(self, s) buf[#buf + 1] = s end,
           close = function() FILES[path] = table.concat(buf) end }
end
SIM_T, CLOCK_BASE = 0, 0
io = fakeio
os = { clock = function() return SIM_T + CLOCK_BASE end,
       time = function() return math.floor(SIM_T + CLOCK_BASE) end }
print = function() end
MOUSE_X, MOUSE_Y = nil, nil
ZONE = nil
local function gui(getPos)
  return { getTerrainPos = getPos, getCamera = function() return { 0, 0, 230 } end,
           getMousePos = function() return { 0, 0 } end }
end
SENDER_GAME = { gui = gui(function() if MOUSE_X then return { MOUSE_X, MOUSE_Y, 100 } end return nil end),
                interface = { setZone = function() end } }
RECEIVER_GAME = { gui = gui(function() return nil end),
                  interface = { setZone = function(id, z)
                    if id ~= "mpcursor_a" then return end
                    if z == nil then ZONE = nil; return end
                    local sx, sy = 0, 0
                    for _, p in ipairs(z.polygon) do sx = sx + p[1]; sy = sy + p[2] end
                    ZONE = { sx / #z.polygon, sy / #z.polygon }
                  end } }
OUTBOX = {}
function makeSide(src, letter, base)
  local CM = {}
  CM.cmCompanyColor = function() return 1, 0, 0 end
  CM.detectInstance = function() end
  CM.broadcast = function(line) OUTBOX[#OUTBOX + 1] = line end
  local K = { INSTANCE = letter, BASE = base }
  assert((loadstring or load)(src, "@cursors.lua"))()(CM, K, function() end)
  return CM
end
function senderGui() game = SENDER_GAME; SG.cursorGuiTick() end
function senderTick() game = SENDER_GAME; SS.cursorTick(); local o = OUTBOX; OUTBOX = {}; return o end
function receiverRecv(line) RS.cursorRecv(line) end
function receiverTick() game = RECEIVER_GAME; RS.cursorTick(); OUTBOX = {} end
function receiverGui() game = RECEIVER_GAME; RG.cursorGuiTick(); if ZONE then return ZONE[1], ZONE[2] end return nil end
'''


def source(which):
    if which == 'work':
        return open(os.path.join(REPO, CURSORS), 'rb').read()
    return subprocess.check_output(['git', '-C', REPO, 'show', which + ':' + CURSORS])


# ---- cursor paths (metres; the sender's real mouse on the ground) ----
def orbit(t):      # a quick sweep: a 150 m circle at 1.2 rad/s, 180 m/s
    return (1000 + 150 * math.cos(1.2 * t), 2000 + 150 * math.sin(1.2 * t))


def stop_go(t):    # 60 m/s east for 3 s, still for 2 s, then 60 m/s north
    if t < 3:
        return (1000 + 60 * t, 2000)
    if t < 5:
        return (1180, 2000)
    return (1180, 2000 + 60 * (t - 5))


def jump(t):       # east at 40 m/s, then 4 km further on in one frame
    return (1000 + 40 * t, 2000) if t < 3 else (5000 + 40 * (t - 3), 2000)


def leave(t):      # moving, then over a window (no ground point) from t=3
    return (1000 + 40 * t, 2000) if t < 3 else None


def still(t):
    return (1000, 2000)


def simulate(L, src, path, duration, seed):
    rnd = random.Random(seed)
    rt = L.LuaRuntime(encoding=None)
    rt.execute(HARNESS)
    g = rt.globals()
    g.SRC = src
    rt.execute(b'SG = makeSide(SRC, "a", "A/"); SS = makeSide(SRC, "a", "A/"); '
               b'RS = makeSide(SRC, "b", "B/"); RG = makeSide(SRC, "b", "B/")')
    next_send, next_recv = rnd.uniform(0, 0.2), rnd.uniform(0, 0.2)
    inflight, frames = [], []
    for k in range(int(duration / FRAME)):
        t = k * FRAME
        g.SIM_T = t
        m = path(t)
        g.MOUSE_X, g.MOUSE_Y = (m if m else (None, None))
        g.CLOCK_BASE = 1000.0          # the sender's process clock
        g.senderGui()
        if t >= next_send:
            for line in list(g.senderTick().values()):
                inflight.append((t + rnd.uniform(0.03, 0.09), line))
            next_send += 0.2 + rnd.uniform(-0.03, 0.03)
        g.CLOCK_BASE = 50.0            # the receiver's
        if t >= next_recv:
            due = sorted([x for x in inflight if x[0] <= t], key=lambda x: x[0])
            inflight = [x for x in inflight if x[0] > t]
            for _, line in due:
                g.receiverRecv(line)
            g.receiverTick()
            next_recv += 0.2 + rnd.uniform(-0.03, 0.03)
        r = g.receiverGui()
        frames.append((t, tuple(r) if isinstance(r, tuple) else None))
    return frames


def dist(p, q):
    return math.hypot(p[0] - q[0], p[1] - q[1])


def best_lag(frames, path, t0, t1):
    best = None
    for lag_ms in range(0, 1001, 5):
        lag = lag_ms / 1000.0
        errs = [dist(d, path(t - lag)) for t, d in frames if t0 <= t <= t1 and d and path(t - lag)]
        if errs:
            e = sum(errs) / len(errs)
            if best is None or e < best[1]:
                best = (lag, e)
    return best


def stutter(frames, t0, t1, still_m):
    steps, prev = [], None
    for t, d in frames:
        if t0 <= t <= t1 and d and prev:
            steps.append(dist(d, prev))
        prev = d
    if not steps:
        return float('nan'), 1.0
    mean = sum(steps) / len(steps)
    sd = math.sqrt(sum((s - mean) ** 2 for s in steps) / len(steps))
    return sd / mean if mean > 0 else float('nan'), sum(1 for s in steps if s < still_m) / len(steps)


def run(L, src, seed):
    out = {}
    fr = simulate(L, src, orbit, 12.0, seed)
    out['orbit_cv'], out['orbit_stall'] = stutter(fr, 3.0, 12.0, 0.3)   # 3 m a frame when even
    out['orbit_lag'], out['orbit_err'] = best_lag(fr, orbit, 8.0, 12.0)   # once the delay has settled
    _, out['start_stall'] = stutter(fr, 1.0, 4.0, 0.3)                    # while it settles

    fr = simulate(L, src, stop_go, 9.0, seed + 1)
    # the longest unbroken stay within 0.5 m of where the cursor stopped: a circle that
    # glides across the 2 s pause, or wanders during it, cannot stay put that long.
    # (A window placed from a lag measured before the pause misreads a delay that is
    # still easing down.)
    run_, best, start = 0, 0, None
    for t, d in fr:
        if d and dist(d, (1180, 2000)) <= 0.5:
            run_ += 1
            best = max(best, run_)
        else:
            run_ = 0
    out['held_s'] = best * FRAME
    out['resumed'] = bool(fr[-1][1]) and fr[-1][1][1] > 2010

    fr = simulate(L, src, jump, 6.0, seed + 2)

    def off_path(d):
        a = abs(d[1] - 2000) if 1000 - 5 <= d[0] <= 1130 else float('inf')
        b = abs(d[1] - 2000) if d[0] >= 5000 - 5 else float('inf')
        return min(a, b)
    out['jump_worst'] = max((off_path(d) for t, d in fr if d), default=0.0)

    fr = simulate(L, src, leave, 6.0, seed + 3)
    out['down_after'] = next((t - 3 for t, d in fr if t >= 3 and d is None), float('inf'))
    out['stayed_down'] = all(d is None for t, d in fr if t >= 3 + out['down_after'])

    fr = simulate(L, src, still, 14.0, seed + 4)
    out['still_up'] = fr[-1][1] is not None
    return out


def main():
    ap = argparse.ArgumentParser(description='cursor circle playback simulation')
    ap.add_argument('--ref', default='HEAD', help='git ref to compare against (default HEAD)')
    ap.add_argument('--lua', choices=['5.2', '5.1'], help='one Lua version only')
    args = ap.parse_args()
    fails = 0
    for ver in ([args.lua] if args.lua else ['5.2', '5.1']):
        L = importlib.import_module('lupa.lua' + ver.replace('.', ''))
        print('== Lua', ver)
        ref = run(L, source(args.ref), 7)
        work = run(L, source('work'), 7)
        for name, r in ((args.ref, ref), ('working tree', work)):
            print('  %-13s stutter %.2f  stalls %4.1f%% (first 4 s %4.1f%%)  lag %.2f s  err %5.1f m | held through the 2 s pause %s s  jump off-path %.0f m  down after %.2f s  still up %s'
                  % (name, r['orbit_cv'], 100 * r['orbit_stall'], 100 * r['start_stall'], r['orbit_lag'], r['orbit_err'],
                     '%.2f' % r['held_s'],
                     r['jump_worst'], r['down_after'], r['still_up']))
        checks = [
            ('an even sweep draws evenly (stutter <= 0.25)', work['orbit_cv'] <= 0.25),
            ('the circle almost never stalls while the cursor moves (<= 1% of frames)', work['orbit_stall'] <= 0.01),
            ('smoother than %s (stutter under half)' % args.ref, work['orbit_cv'] < 0.5 * ref['orbit_cv']),
            ('the circle trails by at most 0.6 s', work['orbit_lag'] <= 0.6),
            ('and follows the path at that lag once settled (mean error <= 3 m)', work['orbit_err'] <= 3.0),
            ('no stalls while a new circle settles in (<= 2% of frames, 1-4 s)', work['start_stall'] <= 0.02),
            ('through a 2 s pause it holds where the cursor stopped (>= 1.9 s within 0.5 m)', work['held_s'] >= 1.9),
            ('and moves on after it', work['resumed']),
            ('a 4 km jump is a jump, never a glide across the map (<= 30 m off the path)', work['jump_worst'] <= 30),
            ('a cursor that leaves the map comes down within 1 s, and stays down', work['down_after'] <= 1.0 and work['stayed_down']),
            ('a still cursor stays up', work['still_up']),
        ]
        for label, ok in checks:
            print('  %s %s' % ('OK  ' if ok else 'FAIL', label))
            fails += 0 if ok else 1
    print('SIM:', 'all checks passed' if fails == 0 else '%d check(s) failed' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
