"""The host-capacity cap (pacing.lua CM.hostCapacityCap, 2026-09-18).

The engine runs 5 sim steps a second per unit of session speed and, when it
cannot afford them, simply makes fewer: the lever still reads 4 while the world
runs at 2. Every K.CAP_WINDOW_S wall seconds the leader compares the steps it
made with 5 x the applied speed x the seconds; three short windows in a row cap
the session speed at what the host sustains, a whole number; an unstretched
minute at the cap tries one step up; a pause, a hold or a speed change restarts
the window. Real Lua 5.2 over the sliced functions with a fake wall clock and
step counter, a temp data dir, no engine.

    python tools/host_capacity_test.py
"""
import os
import re
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACING = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "pacing.lua"), encoding="utf-8").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


check("the leader caps eff by the host's capacity before the governor, from the applied speed and the lever read",
      "local capped, capAt = CM.hostCapacityCap(eff, CM.effSpeed, s, now)" in PACING
      and PACING.index("local capped, capAt = CM.hostCapacityCap(eff, CM.effSpeed, s, now)") < PACING.index("local governed = CM.governSpeed(now, eff)"))

m1 = re.search(r"^function CM\.hostPace\(\)\n.*?\n^end\n", PACING, re.S | re.M)
m2 = re.search(r"^function CM\.hostCapacityCap\(eff, applied, s, now\)\n.*?\n^end\n", PACING, re.S | re.M)
consts = "\n".join(l for l in PACING.splitlines() if l.startswith("K.CAP_"))
check("pacing.lua defines CM.hostPace and CM.hostCapacityCap", bool(m1 and m2))

with tempfile.TemporaryDirectory() as td:
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = consts + "\n" + (m1.group(0) if m1 else "") + (m2.group(0) if m2 else "")
    L.globals().BASE = td.replace("\\", "/") + "/"
    T = L.execute(r'''
local T = { log = {}, wall = 1000, now = 0 }
os.time = function() return T.wall end          -- the fake wall clock (whole seconds, as the real one)
local CM, K = { ticks = 0 }, { BASE = BASE, SIM_STEP = 0.2 }
CM.stepOf = function(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
local function log(s) T.log[#T.log + 1] = s end
assert(load("local CM, K, log = ...\n" .. SRC, "@cap"))(CM, K, log)
T.CM, T.K = CM, K
-- run `seconds` of wall time in 1 s passes: the sim makes `rate` x 5 steps a second (rate = achieved speed)
function T.run(seconds, eff, applied, rate, lever)
  local e, at
  for _ = 1, seconds do
    T.wall = T.wall + 1
    T.now = T.now + rate * 5 * K.SIM_STEP
    CM.ticks = CM.ticks + 1
    e, at = CM.hostCapacityCap(eff, applied, lever, T.now)
  end
  return e, at
end
function T.logged(sub) local n = 0; for _, s in ipairs(T.log) do if s:find(sub, 1, true) then n = n + 1 end end; return n end
return T
''')
    W = T.K.CAP_WINDOW_S

    e, at = T.run(60, 4, 4, 4, 4)
    check("keeping up at 4 (20 steps/s): no cap", e == 4 and at is None)
    e, at = T.run(W + 1, 4, 4, 2, 4)
    check("one short window (a hash stamp, an autosave) does nothing", e == 4 and at is None)
    e, at = T.run(2 * W + 2, 4, 4, 2, 4)
    check("three short windows in a row: capped at what the host made (10 of 20 steps/s -> 2x)", e == 2 and at == 2, f"{e} {at}")
    check("  ... logged once, with the numbers", T.logged("keeps up with 2x") == 1)
    # the session now runs at 2 (applied 2) and the host makes 10 steps/s: the cap holds
    e, at = T.run(40, 4, 2, 2, 2)
    check("unstretched at the cap for a short while: holds", e == 2 and at == 2)
    e, at = T.run(T.K.CAP_UP_S + W + 2, 4, 2, 2, 2)
    check("after an unstretched minute it tries one step up", e == 3 and at == 3, f"{e} {at}")
    # at 3 the host makes 2.2x worth: short again -> back to floor(3 x 0.73) = 2
    e, at = T.run(3 * W + 3, 4, 3, 2.2, 3)
    check("short again at 3: back to 2", e == 2 and at == 2, f"{e} {at}")
    # the votes drop to 2: the cap is not below them, nothing bites; it clears on the next climb
    e, at = T.run(T.K.CAP_UP_S + W + 2, 2, 2, 2, 2)
    check("votes at the cap: nothing bites and the climb clears it", e == 2 and at is None and T.CM.hostCap is None, f"{e} {at} {T.CM.hostCap}")
    # a pause (lever 0) or a hold restarts the window: a minute paused is not a short window
    T.CM.hostCap = None; T.CM.capStretched = 0
    e, at = T.run(20, 4, 4, 4, 4)
    e, at = T.run(70, 4, 4, 0, 0)          # paused: no steps, lever 0
    e, at = T.run(20, 4, 4, 4, 4)
    check("a pause restarts the window: no cap from a paused minute", e == 4 and at is None and (T.CM.capStretched or 0) == 0, str(T.CM.capStretched))
    T.CM.resyncHold = True
    e, at = T.run(70, 4, 4, 0.5, 4)        # a world operation's hold: the lever runs, the sim crawls
    T.CM.resyncHold = None
    e, at = T.run(20, 4, 4, 4, 4)
    check("a hold restarts the window too", e == 4 and at is None and (T.CM.capStretched or 0) == 0)
    # a speed change mid-window restarts it: the mix of two speeds is not a reading
    e, at = T.run(10, 4, 4, 4, 4)
    e, at = T.run(10, 4, 2, 2, 2)
    check("a speed change restarts the window", T.CM.capWin is not None and T.CM.capWin["applied"] == 2)
    # the governor's off switch turns the cap off too
    e, at = T.run(3 * W + 3, 4, 4, 1, 4)
    check("(set-up) capped at 1 with the host making 5 steps/s", e == 1 and at == 1, f"{e} {at}")
    T.CM.governorOff = L.eval("function() return true end")
    e, at = T.run(1, 4, 4, 1, 4)
    check("tpf2mp_governor_off.txt: no cap", e == 4 and at is None and T.CM.hostCap is None)
    T.CM.governorOff = None
    e, at = T.run(1, 0, 0, 0, 0)
    check("eff 0 (paused): untouched", e == 0 and at is None)

    # A HOST WHOSE ENGINE STRETCHES EVERY SPEED ALIKE (live, 2026-09-22): 77% of the
    # steps at any lever, 172 of 225 at 3x and 116 of 150 at 2x. Capping only lost speed.
    T.CM.hostCap = None; T.CM.capStretched = 0; T.CM.capWin = None; T.CM.capFrom = None; T.CM.capOffUntil = None
    e, at = T.run(3 * W + 3, 4, 3, 3 * 0.767, 3)
    check("stretched at 3x (2.3x achieved): capped at 2 as before", e == 2 and at == 2, f"{e} {at}")
    e, at = T.run(W + 2, 4, 2, 2 * 0.773, 2)
    check("the 2x cap made the host slower (1.55x vs 2.3x): lifted at the first window, back to the votes",
          e == 4 and at is None and T.CM.hostCap is None, f"{e} {at} {T.CM.hostCap}")
    check("  ... logged once", T.logged("made the host slower") == 1)
    e, at = T.run(5 * W, 4, 4, 4 * 0.77, 4)
    check("stretched at 4x afterwards: no cap for K.CAP_OFF_S", e == 4 and at is None, f"{e} {at}")
    e, at = T.run(T.K.CAP_OFF_S, 4, 4, 4 * 0.77, 4)
    check("after K.CAP_OFF_S the cap may act again (and is lifted again if it costs speed)", at in (None, 3), f"{e} {at}")

if fails:
    print("FAIL:", len(fails), "check(s):", "; ".join(fails))
    raise SystemExit(1)
print("PASS: the session speed is capped at the steps the host actually makes, whole numbers, one step up after an unstretched minute, lifted when it costs speed; pauses, holds and speed changes restart the window")
