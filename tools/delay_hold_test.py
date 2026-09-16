"""Offline checks for the measured command delay and the gap hold (net.lua), on Lua 5.2.

Loads the real mod/.../scripts/mp/net.lua into a lupa.lua52 runtime with a stub CM/K
and a settable os.clock, then drives:
  - CM.rttNote: RFC 6298 smoothing, absurd samples rejected
  - CM.execDelayTick: the delay from the worst fresh peer, the step grid, min/max,
    rising at once and falling one step after K.DELAY_DOWN_TICKS, pinned mode
  - LSTICK parsing through CM.pollEvents: ms= stored, our echo e= becomes a round trip
  - CM.scheduleLocal: the stamp uses the current delay and an LSHI follows the LSCMD
  - CM.gapHoldNeed / CM.gapHoldTick: hold for a missing command due soon, not for one
    far away, already past, inside the grace, or out of NACKs; release when it arrives;
    give up after K.GAP_HOLD_MAX_TICKS

    python tools/delay_hold_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "net.lua")

fails = []


def lua_list(t):
    return [t[i] for i in range(1, len(t) + 1)]


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = os.path.join(REPO, "mod/mp_lockstep_1/res/scripts/?.lua").replace("\\", "/") + ";" + L.globals().package.path
    L.globals().NET_SRC = open(NET, encoding="utf-8").read()
    L.globals().EVENTS = os.path.join(tempfile.mkdtemp(), "events.txt").replace("\\", "/")
    h = L.execute(r'''
CLK = 100.0
os.clock = function() return CLK end
local logs, sent = {}, {}
local K = {
  INSTANCE = "a", SIM_STEP = 0.2, EXEC_DELAY = 0.4, EXEC_DELAY_MIN = 0.4, EXEC_DELAY_MAX = 3.0,
  DELAY_SLACK_MS = 50, DELAY_DEV_MULT = 1, DELAY_DOWN_TICKS = 25, RTT_MIN_SAMPLES = 8, PEER_STALE_TICKS = 25,
  GAP_HOLD_GRACE_TICKS = 1, GAP_HOLD_ENGAGE_TICKS = 3, GAP_HOLD_MAX_TICKS = 55,
  NACK_MAX = 10, CMD_RING = 256, HIST_RING = 64, EVENTS_FILE = EVENTS,
}
local CM = { ticks = 1000, peers = {}, seqNo = 0, queue = {}, MAX_LEAD = 15, execDelayAuto = true }
local now = 50.0
function CM.gameTime() return now end
function CM.stepOf(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
function CM.peerFor(o) local pr = CM.peers[o]; if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end; return pr end
function CM.peerBounds() return nil, nil end
function CM.broadcast(line) sent[#sent + 1] = line end
function CM.cmEnsure() end
-- io.lua's reader (whole lines from an offset), which pollEvents uses
function CM.readFrom(path, offset)
  local f = io.open(path, "r")
  if not f then return nil, offset end
  local size = f:seek("end")
  if offset >= size then f:close(); return nil, offset end
  f:seek("set", offset)
  local data = f:read("*a") or ""
  f:close()
  local last = #data
  while last > 0 and data:byte(last) ~= 10 do last = last - 1 end
  if last == 0 then return nil, offset end
  data = data:sub(1, last)
  return data, offset + #data
end
local log = function(s) logs[#logs + 1] = s end
local ok, err = pcall(function() assert(load(NET_SRC, "@net.lua"))()(CM, K, log) end)
if not ok then error("net.lua did not load: " .. tostring(err)) end
local H = { CM = CM, K = K }
function H.setNow(t) now = t end
function H.logs() return table.concat(logs, "\n") end
function H.clearLogs() logs = {} end
function H.sent() return sent end
function H.clearSent() sent = {} end
function H.feed(line)
  local f = io.open(EVENTS, "a"); f:write(line .. "\n"); f:close()
  CM.eventsOffset = CM.eventsOffset or 0
  CM.pollEvents()
end
return H
''')
    return L, h


# ---- round trip smoothing ----
L, h = runtime()
CM = h.CM
L.globals().CLK = 100.0
CM.rttNote("b", 100.0 * 1000 - 100, 0)
pr = CM.peers["b"]
check("first sample: srtt=r, rttvar=r/2", abs(pr.srtt - 100) < 1e-6 and abs(pr.rttvar - 50) < 1e-6, f"{pr.srtt} {pr.rttvar}")
CM.rttNote("b", 100.0 * 1000 - 140, 20)          # r = 120
check("second sample smoothed", abs(pr.srtt - (0.875 * 100 + 0.125 * 120)) < 1e-6 and abs(pr.rttvar - (0.75 * 50 + 0.25 * 20)) < 1e-6,
      f"{pr.srtt:.2f} {pr.rttvar:.2f}")
n_before = pr.rttN
CM.rttNote("b", 100.0 * 1000 + 500, 0)          # negative: clock from another run
CM.rttNote("b", 100.0 * 1000 - 20000, 0)        # 20 s: absurd
check("negative and absurd samples rejected", pr.rttN == n_before)


# ---- the delay ----
def peer(h, o, srtt, var, n=10, fresh=True):
    p = h.CM.peerFor(o)
    p.srtt, p.rttvar, p.rttN = srtt, var, n
    p.at = h.CM.ticks if fresh else h.CM.ticks - 100


def tick(h, n=1):
    for _ in range(n):
        h.CM.ticks = h.CM.ticks + 1
        for o, p in h.CM.peers.items():
            if p.at is not None and p.at >= h.CM.ticks - 2:
                p.at = h.CM.ticks
        h.CM.execDelayTick()


L, h = runtime()
h.CM.simRate = 0.9
tick(h)
check("nothing measured: the starting 0.4", abs(h.CM.execDelayCur - 0.4) < 1e-9, h.CM.execDelayCur)
peer(h, "b", 60, 10)
tick(h, 60)
check("a fast peer never takes it below EXEC_DELAY_MIN (0.4)", abs(h.CM.execDelayCur - 0.4) < 1e-9, h.CM.execDelayCur)
peer(h, "c", 1400, 100)
tick(h)
check("a slower peer raises it at once (1400+-100 ms -> 0.8 at 0.9 u/s)", abs(h.CM.execDelayCur - 0.8) < 1e-9, h.CM.execDelayCur)
peer(h, "c", 500, 20)
tick(h)
check("it does not fall at once", abs(h.CM.execDelayCur - 0.8) < 1e-9, h.CM.execDelayCur)
tick(h, 26)
check("after DELAY_DOWN_TICKS it drops one step (0.6)", abs(h.CM.execDelayCur - 0.6) < 1e-9, h.CM.execDelayCur)
peer(h, "c", 1800, 100)
tick(h)
check("a slow peer raises it at once (1800+-100 ms -> 1.0 at 0.9 u/s)", abs(h.CM.execDelayCur - 1.0) < 1e-9, h.CM.execDelayCur)
check("the raise is logged with the worst peer", "worst peer c" in h.logs(), h.logs().splitlines()[-1] if h.logs() else "")
h.CM.peers["c"].at = h.CM.ticks - 100          # c goes stale
tick(h, 100)
check("a stale peer stops counting (back down to 0.4 step by step)", abs(h.CM.execDelayCur - 0.4) < 1e-9, h.CM.execDelayCur)
h.CM.simRate, h.CM.effSpeed = 3.6, 4
peer(h, "b", 300, 40)
tick(h)
# one way 300/2 + 40 + 50 = 240 ms at 3.6 u/s = 0.864 -> 1.0
check("speed 4: 300+-40 ms -> 1.0 units", abs(h.CM.execDelayCur - 1.0) < 1e-9, h.CM.execDelayCur)
h.CM.simRate, h.CM.effSpeed = 0.0, 1          # held or paused: rate reads 0
peer(h, "b", 1500, 400)
tick(h)
# one way 1500/2 + 400 + 50 = 1200 ms at the session's 0.9 u/s = 1.08 -> 1.2, not 1200 ms at rate 0
check("a paused game (rate 0) still uses the session speed (1500+-400 ms -> 1.2)", abs(h.CM.execDelayCur - 1.2) < 1e-9, h.CM.execDelayCur)
# the first rig run's samples (A and B on one PC): all 0.4 with one deviation
for srtt, var in ((200, 60), (273, 132), (226, 28), (319, 159), (216, 63), (304, 123)):
    Lr, hr = runtime()
    hr.CM.simRate = 1.0
    peer(hr, "b", srtt, var)
    tick(hr, 30)
    check(f"one PC, measured {srtt}+-{var} ms -> 0.4", abs(hr.CM.execDelayCur - 0.4) < 1e-9, hr.CM.execDelayCur)
peer(h, "b", 20000, 5000)
tick(h)
check("never above EXEC_DELAY_MAX", h.CM.execDelayCur <= 3.0 + 1e-9, h.CM.execDelayCur)
h.CM.execDelayAuto = False
h.K.EXEC_DELAY = 0.8
tick(h)
check("pinned: exactly K.EXEC_DELAY", abs(h.CM.execDelayCur - 0.8) < 1e-9, h.CM.execDelayCur)
peer(h, "d", 60, 10, n=2)
h.CM.execDelayAuto = True
h.K.EXEC_DELAY = 0.4
h.CM.peers["b"].at = h.CM.ticks - 100
h.CM.execDelayCur = 0.4
tick(h, 40)
check("a peer with fewer than RTT_MIN_SAMPLES is not trusted yet", abs(h.CM.execDelayCur - 0.4) < 1e-9, h.CM.execDelayCur)

# a requirement on a step boundary: 392+-3 ms is 249 ms one way, 1.02 units at 4.10 u/s and 0.99 at 3.99
L, h = runtime()
h.CM.effSpeed = 4
peer(h, "b", 392, 3)
h.CM.simRate = 4.10
tick(h)
check("392+-3 ms at 4.10 u/s -> 1.2", abs(h.CM.execDelayCur - 1.2) < 1e-9, h.CM.execDelayCur)
flips = 0
for i in range(400):
    h.CM.simRate = 3.99 if i % 40 < 30 else 4.10    # 30 ticks just under the 1.0 step, 10 just over
    before = h.CM.execDelayCur
    tick(h)
    flips += abs(h.CM.execDelayCur - before) > 1e-9
check("a requirement on the 1.0/1.2 boundary does not step down and back up", flips == 0, flips)
peer(h, "b", 300, 3)                                 # 203 ms one way at 4.0 u/s = 0.81, clear of 1.0
h.CM.simRate = 4.0
tick(h, 26)
check("a clearly lower one still steps down after DELAY_DOWN_TICKS (1.0)", abs(h.CM.execDelayCur - 1.0) < 1e-9, h.CM.execDelayCur)


# ---- heartbeat parse and echo ----
L, h = runtime()
L.globals().CLK = 200.0
h.feed("LSTICK t=50 o=b s=250 hi=0 ms=77000")
pb = h.CM.peers["b"]
check("ms= stored with the arrival clock", pb.ms == 77000 and abs(pb.msClk - 200.0) < 1e-9, f"{pb.ms} {pb.msClk}")
L.globals().CLK = 200.25
echo = h.CM.heartbeatEcho()
check("our heartbeat echoes b's clock and 250 ms held", echo == " e=b:77000:250", echo)
L.globals().CLK = 300.0
# b echoes our ms=299800 (sent 200 ms ago) after holding it 40 ms -> 160 ms round trip
h.feed("LSTICK t=51 o=b s=255 hi=0 ms=77400 e=a:299800:40,c:1:1")
check("our echo becomes a 160 ms round trip", pb.srtt is not None and abs(pb.srtt - 160) < 1e-6, pb.srtt)
h.feed("LSTICK t=52 o=b s=260 hi=4 ha=53.2000 ms=77800")
rb = h.CM.rx["b"]
check("hi=/ha= record the stamp of the high-water command", rb is not None and rb.stamp is not None and abs(rb.stamp[4] - 53.2) < 1e-9)


# ---- round trips across a freeze ----
L, h = runtime()


def beat(clk, i, ping_at):
    L.globals().CLK = clk
    h.feed("LSTICK t=%d o=b s=%d hi=0 ms=%d e=a:%d:0" % (60 + i, 300 + i, 90000 + i * 370, int(ping_at * 1000)))


for i in range(12):                                    # b's heartbeat every 0.37 s, each timing a 390 ms round trip
    beat(500.0 + i * 0.37, i, 500.0 + i * 0.37 - 0.39)
pb = h.CM.peers["b"]
n0, s0 = pb.rttN, pb.srtt
check("steady 390 ms round trips through LSTICK are samples", n0 == 12 and abs(s0 - 390) < 1.0, f"{n0} {s0:.1f}")
end0 = 500.0 + 11 * 0.37 + 1.3                         # 1.3 s of silence (an autosave)
beat(end0, 12, end0 - 1.1)                            # its echo of a ping sent before the silence: 1,100 ms
check("a round trip that waited out a 1.3 s silence is not a sample", pb.rttN == n0 and abs(pb.srtt - s0) < 1e-9,
      f"{pb.rttN} {pb.srtt:.1f}")
check("the silence is logged", "b was silent for 1.3 s" in h.logs(), h.logs().splitlines()[-1] if h.logs() else "")
beat(end0 + 0.37, 13, end0 - 0.02)                    # a ping sent while b was still silent: not a sample either
check("nor one whose ping went out before the silence ended", pb.rttN == n0)
beat(end0 + 0.74, 14, end0 + 0.35)                    # sent after it: a sample again
check("a ping sent after the silence is a sample again", pb.rttN == n0 + 1, pb.rttN)
beat(end0 + 0.74 + 0.55, 15, end0 + 0.74 + 0.16)      # 0.55 s against a usual 0.37: ordinary jitter
check("0.55 s between heartbeats (usual 0.37) is no silence", pb.rttN == n0 + 2, pb.rttN)


# ---- scheduleLocal ----
L, h = runtime()
h.CM.execDelayCur = 0.8
h.setNow(50.0)
h.CM.scheduleLocal("ROAD", L.table_from({"x": 1}))
s = list(h.sent().values())
cmd = [x for x in s if x.startswith("LSCMD")]
shi = [x for x in s if x.startswith("LSHI")]
check("the stamp uses the current delay (50 + 0.8)", cmd and "at=50.8000" in cmd[0], cmd[:1])
check("an LSHI with the same seq and stamp follows the LSCMD", shi and shi[0] == "LSHI o=a s=1 at=50.8000" and s.index(shi[0]) > s.index(cmd[0]), shi[:1])
check("lastSchedAt is the stamp", abs(h.CM.lastSchedAt - 50.8) < 1e-9)
h.clearSent()
h.CM.dropNextCmd = True
h.CM.scheduleLocal("ROAD", L.table_from({"x": 2}))
s = list(h.sent().values())
check("DROPNEXT: the LSCMD is not sent, the LSHI is", not any(x.startswith("LSCMD") for x in s) and any(x.startswith("LSHI o=a s=2") for x in s), s)
check("DROPNEXT: kept for resend and cleared after one command", h.CM.sentRing[2] is not None and h.CM.dropNextCmd is None)


# ---- gap hold ----
def rx(h, missing_age=2, stamp2=None, stamp3=None, nack=0):
    CMh = h.CM
    t = CMh.ticks
    r = h_lua.table_from({
        "seen": h_lua.table_from({1: True, 3: True}), "maxSeq": 3, "firstSeq": 0, "advMax": 3,
        "missSince": h_lua.table_from({2: t - missing_age}), "nackAt": h_lua.table_from({}),
        "nackN": h_lua.table_from({2: nack}), "stamp": h_lua.table_from({}),
    })
    if stamp2 is not None:
        r.stamp[2] = stamp2
    if stamp3 is not None:
        r.stamp[3] = stamp3
    CMh.rx["b"] = r
    return r


L, h = runtime()
h_lua = L
h.CM.simRate, h.CM.tickSec = 0.9, 0.19
h.setNow(50.0)
r = rx(h, stamp2=50.4, stamp3=50.6)
need = h.CM.gapHoldNeed(50.0)
check("missing seq 2 due in 0.4: hold", need is not None and need.seq == 2 and abs(need.at - 50.4) < 1e-9)
check("gapHoldTick engages and logs", h.CM.gapHoldTick(50.0) is True and "HOLD: b's command seq=2" in h.logs())
h.CM.gapHold = None   # the cases below are about gapHoldNeed with no hold in progress (a held command stays held, 2026-09-16)
r.stamp[2] = 55.0
check("due in 5 units: no hold yet", h.CM.gapHoldNeed(50.0) is None)
r.stamp[2] = 49.6
check("stamp already behind us: no hold (it applies late)", h.CM.gapHoldNeed(50.0) is None)
r.stamp[2] = None
check("stamp unknown but newer commands still due: hold", h.CM.gapHoldNeed(50.0) is not None)
r.stamp[3] = 49.0
check("stamp unknown and even the newest is past: no hold", h.CM.gapHoldNeed(50.0) is None)
r = rx(h, missing_age=0, stamp2=50.4)
check("inside the grace but its own stamp is within the engage window: hold now (2026-09-16)", h.CM.gapHoldNeed(50.0) is not None)
r = rx(h, missing_age=0, stamp2=55.0)
check("inside the grace and its own stamp is far off: wait out the grace", h.CM.gapHoldNeed(50.0) is None)
r = rx(h, stamp2=50.4, nack=10)
check("out of NACKs: no hold", h.CM.gapHoldNeed(50.0) is None)
# the relay case: 53 applied, 55 held and due next step, 54 missing with no stamp, gap just seen
r = rx(h, missing_age=0, stamp3=50.2)
check("inside the grace but the command above it is due next step: hold now", h.CM.gapHoldNeed(50.0) is not None)
r = rx(h, missing_age=0, stamp3=51.0)
check("inside the grace and the command above it is not due yet: wait out the grace", h.CM.gapHoldNeed(50.0) is None)

L, h = runtime()
h_lua = L
h.CM.simRate, h.CM.tickSec = 0.9, 0.19
r = rx(h, stamp2=50.4, stamp3=50.6)
h.clearSent()
check("hold engages", h.CM.gapHoldTick(50.0) is True)
check("and NACKs the held command at once, not after the scan's grace", any(x == "LSNACK o=b seq=2 by=a" for x in lua_list(h.sent())) and "NACK b seq=2 (holding for it)" in h.logs(), lua_list(h.sent()))
h.clearSent()
h.CM.gapHoldTick(50.0)
check("not again on the very next tick", not any(x.startswith("LSNACK") for x in lua_list(h.sent())))
h.CM.ticks = h.CM.ticks + 3
h.CM.gapHoldTick(50.0)
check("again HOLD_NACK_EVERY ticks later", any(x == "LSNACK o=b seq=2 by=a" for x in lua_list(h.sent())))
check("only the first hold NACK counts against NACK_MAX", r.nackN[2] == 1, r.nackN[2])
# the batch in flight ran past the stamp while holding: still held (2026-09-16)
h.setNow(50.8)
need = h.CM.gapHoldNeed(50.8)
check("once held, held: the stamp fell behind and the hold stays on it", need is not None and need.seq == 2, need and need.seq)
check("gapHoldTick keeps holding", h.CM.gapHoldTick(50.8) is True and h.CM.gapHold is not None)
h.setNow(50.0)
r.seen[2] = True
h.clearLogs()
check("the command arrives: released", h.CM.gapHoldTick(50.0) is False and "HOLD: released" in h.logs() and h.CM.gapHold is None)
check("the release says it arrived", "seq=2 arrived" in h.logs())

L, h = runtime()
h_lua = L
h.CM.simRate, h.CM.tickSec = 0.9, 0.19
r = rx(h, stamp2=50.4, stamp3=50.6)
h.CM.gapHoldTick(50.0)
h.CM.ticks = h.CM.ticks + 56
h.clearLogs()
check("past GAP_HOLD_MAX_TICKS: gives up and runs on", h.CM.gapHoldTick(50.0) is False and "gave up" in h.logs())
check("and does not hold for that command again", h.CM.gapHoldNeed(50.0) is None and r.holdDone[2] is True)

# ---- a command is sent more than once (2026-09-16) ----
L, h = runtime()
h.K.CMD_SEND_COPIES, h.K.CMD_REPEATS = 2, 1
h.setNow(50.0)
h.clearSent()
h.CM.scheduleLocal("ROAD", L.table_from({"x": 7}))
s = lua_list(h.sent())
cmds = [x for x in s if x.startswith("LSCMD")]
check("two copies go out back to back, then the LSHI", len(cmds) == 2 and cmds[0] == cmds[1] and s[-1].startswith("LSHI o=a s=1"), s)
h.clearSent()
h.CM.txRepeatTick()
check("nothing more on the same tick", not lua_list(h.sent()))
h.CM.ticks = h.CM.ticks + 1
h.CM.txRepeatTick()
s = lua_list(h.sent())
check("the next tick repeats the command with its LSHI", len(s) == 2 and s[0] == cmds[0] and s[1].startswith("LSHI o=a s=1"), s)
h.clearSent()
h.CM.ticks = h.CM.ticks + 1
h.CM.txRepeatTick()
check("and then it is done", not lua_list(h.sent()) and len(h.CM.txRepeat) == 0)
h.CM.dropNextCmd = True
h.clearSent()
h.CM.scheduleLocal("ROAD", L.table_from({"x": 8}))
h.CM.ticks = h.CM.ticks + 1
h.CM.txRepeatTick()
check("DROPNEXT drops the copies and the repeat too", not any(x.startswith("LSCMD") for x in lua_list(h.sent())))

# ---- the delay pays for the repeat ----
L, h = runtime()
h.K.DELAY_REPEAT_TICKS = 1
h.CM.simRate, h.CM.effSpeed, h.CM.tickSec = 3.6, 4, 0.19
peer(h, "b", 300, 40)
tick(h)
# one way 240 ms + one tick 190 ms = 430 ms at 3.6 u/s = 1.548 -> 1.6 (was 1.0 without the repeat)
check("speed 4: 300+-40 ms plus one tick for the repeat -> 1.6 units", abs(h.CM.execDelayCur - 1.6) < 1e-9, h.CM.execDelayCur)

print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
