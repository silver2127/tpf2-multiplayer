"""Offline checks for the load gate and the catch-up feed (pacing.lua, Lua 5.2): a game that
finishes loading after the session moved on takes the hot-join path and is never left behind.

Drives the real pacing.lua (and the clock helpers from lockstep.lua, as tools/pacing_sim.py
does) for one joiner against a stub engine:
  - holds at the loaded save, at speed 0, until the leader is heard -- for as long as that
    takes (the gate used to give up after 900 ticks)
  - once the leader is in, asks for every command stamped after the SAVE'S stamp
    (LSNEED t=<savedAt> o=b save=1), not after a clock read some ticks into play
  - re-asks whenever the feed stalls (K.HIST_STALL_TICKS), and releases only when the end
    marker is in and no gap is left; then the save's own speed is restored
  - its heartbeat says cu=1 from the first tick until then
  - the override is two play presses (the slice's SPEEDBTN or the lever): the first is put
    back with a warning, the second starts and logs what lands out of step
  - a roster of one waits for nothing; an older build's save (no stamp) uses the first clock
  - the catch-up feed of a live game far behind re-asks on a stall instead of running on

    python tools/late_loader_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from pacing_sim import helpers, sources  # noqa: E402  (the same helper slice of lockstep.lua the sim uses)

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


PRELUDE = r'''
SIM = { fs = {}, sent = {}, logs = {}, lever = 2, gaps = 0 }
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
game = { interface = { getGameSpeed = function() return SIM.lever end } }
api = { cmd = { make = { setGameSpeed = function(v) return { speed = v } end },
                sendCommand = function(c) SIM.lever = c.speed end } }

function newInst(spec)
  local CM, K = {}, {}
  K.INSTANCE = "b"
  K.BASE = "mem://b/"
  K.SIM_STEP = 0.2
  K.PEER_STALE_TICKS = 25
  K.HEARTBEAT_EVERY = 2
  K.EXEC_DELAY = 0.4
  K.BARRIER_AHEAD = 8.0
  K.GAP_GRACE_TICKS = 3
  CM.peers, CM.ticks, CM.leader, CM.seqNo, CM.cfgCache, CM.queue = {}, 0, "a", 0, {}, {}
  CM.cfgFlag = function(key, default) return default end
  CM.broadcast = function(line) SIM.sent[#SIM.sent + 1] = line end
  CM.rxGaps = function() return SIM.gaps, 0, nil, nil end
  SIM.T = spec.T0
  CM.gameTime = function() return SIM.T end
  CM.stepOf = function(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
  CM.clearFile = function(path) SIM.fs[path] = nil end
  CM.scheduleLocal = function() end
  CM.savedAt = spec.savedAt
  SIM.fs["mem://b/tpf2_bridge_ctl.txt"] = spec.ctl or "players=3\nleader=a\n"
  SIM.lever, SIM.sent, SIM.logs, SIM.gaps = spec.lever or 2, {}, {}, 0
  local function log(msg) SIM.logs[#SIM.logs + 1] = string.format("%5d b: %s", CM.ticks, tostring(msg)) end
  HELPERS(CM, K)
  FACTORY(CM, K, log)
  for _, nm in ipairs({ "hostUnpause", "syncBegin", "syncEnd" }) do if not CM[nm] then CM[nm] = function() end end end
  CM.lastSetSpeed, CM.paceApplied, CM.paceSetTick = nil, nil, nil
  SIM.CM, SIM.K = CM, K
  return CM
end

-- one update() tick as lockstep.lua runs it: the clock advances at the lever, then pacing, then the gate
function tick(n, leaderAt)
  local CM = SIM.CM
  for _ = 1, (n or 1) do
    CM.ticks = CM.ticks + 1
    SIM.T = SIM.T + SIM.lever / 5.4
    if leaderAt then
      local pr = CM.peers.a or {}
      CM.peers.a = pr
      pr.time, pr.at, pr.step = math.floor(leaderAt), CM.ticks, CM.stepOf(leaderAt)
      CM.peerSeen = true
    end
    SIM.cuNow = CM.heartbeatCu(SIM.T)
    CM.paceTick(SIM.T)
    CM.ensureRunning()
  end
end
function sentMatching(prefix)
  local out = {}
  for _, l in ipairs(SIM.sent) do if l:sub(1, #prefix) == prefix then out[#out + 1] = l end end
  return out
end
function logged(txt)
  for _, l in ipairs(SIM.logs) do if l:find(txt, 1, true) then return true end end
  return false
end
'''


def make():
    pacing, lockstep = sources('work')
    rt = lupa.LuaRuntime(unpack_returned_tuples=True)
    load = rt.eval(b'function(src, name) local f, e = load(src, name); if not f then error(e) end; return f() end')
    rt.globals().FACTORY = load(pacing.encode(), b'@pacing.lua')
    rt.globals().HELPERS = load(helpers(lockstep).encode(), b'@lockstep_helpers.lua')
    rt.execute(PRELUDE.encode())
    return rt


def lua_list(t):
    return [t[i] for i in range(1, len(t) + 1)]


# ---- a slow loader: the leader ran on for minutes ----
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = 1000.0, lever = 2 })')
tick, sent, logged = rt.globals().tick, rt.globals().sentMatching, rt.globals().logged
SIM = rt.globals().SIM
tick(1)
check("the gate pauses on the first tick (was the 5th)", SIM.lever == 0 and CM.lgHolding is True and CM.lgHeld is True)
check("the roster is read: 3 players", CM.rosterPlayers == 3)
check("cu=1 on the heartbeat from the first tick", SIM.cuNow is True)
tick(1200)
check("1,200 ticks without the leader: still holding (the gate used to give up at 900)", CM.lgHolding is True and SIM.lever == 0)
check("...saying who is missing", logged("the leader (a) has not been heard"))
check("no history was asked for without the leader", len(lua_list(sent("LSNEED"))) == 0 and CM.lgFetch == "wait")
tick(1, 1230.0)                       # the leader is heard, 230 units ahead
asks = lua_list(sent("LSNEED"))
check("the leader is heard: asks for every command after the SAVE's stamp", asks == ["LSNEED t=1000.0000 o=b save=1"], str(asks))
check("...and still holds", CM.lgFetch == "fetch" and CM.lgHolding is True and SIM.lever == 0)
tick(int(SIM.K.HIST_STALL_TICKS) + 2, 1240.0)
asks = lua_list(sent("LSNEED"))
check("no history line for ~5 s: asked again, from the same stamp", asks == ["LSNEED t=1000.0000 o=b save=1"] * 2, str(asks))
CM.histProgressAt = CM.ticks          # lines arriving
tick(int(SIM.K.HIST_STALL_TICKS) - 5, 1250.0)
check("while lines arrive it does not re-ask", len(lua_list(sent("LSNEED"))) == 2)
CM.histEndSeen, SIM.gaps = True, 1
tick(1, 1251.0)
check("the end marker with a gap left: still holding", CM.lgHolding is True and CM.lgFetch == "fetch")
SIM.gaps = 0
tick(1, 1252.0)
check("history complete: released at the save's own speed", CM.lgFetch == "done" and CM.lgHolding is False and SIM.lever == 2, f"lever={SIM.lever}")
check("...and logged", logged("the command history since our save is complete") and logged("-- releasing"))
check("nothing was ever given up on", not logged("giving up") and not logged("running anyway"))

# ---- the override: two presses (SPEEDBTN) ----
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = 1000.0, lever = 2 })')
tick, sent, logged, SIM = rt.globals().tick, rt.globals().sentMatching, rt.globals().logged, rt.globals().SIM
tick(3)
CM.speedButton(2, "button")
tick(1)
check("first play press while the leader is missing: held, with the reason", CM.lgHolding is True and SIM.lever == 0
      and logged("play pressed while the leader (a) has not been heard -- held. Press play again"))
tick(20)
check("...and it stays held", CM.lgHolding is True)
CM.speedButton(2, "button")
tick(1)
check("second press: started, loudly", CM.lgHolding is False and CM.lgFetch == "done" and SIM.lever == 2
      and logged("!! LOADGATE: started manually at speed 2 while the leader (a) has not been heard"))

# ---- the override through the lever ----
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = 1000.0, lever = 2 })')
tick, sent, logged, SIM = rt.globals().tick, rt.globals().sentMatching, rt.globals().logged, rt.globals().SIM
tick(3)
SIM.lever = 1                          # the player moved the lever
tick(1)
check("lever moved once: put back to 0 with the warning", SIM.lever == 0 and CM.lgHolding is True and logged("Press play again"))
tick(2)
SIM.lever = 1
tick(1)
check("lever moved again: the player's lever wins", CM.lgHolding is False and SIM.lever == 1 and logged("!! LOADGATE: started manually at speed 1"))

# ---- a roster of one, and an old save without a stamp ----
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = 1000.0, lever = 2, ctl = "players=1\\nleader=a\\n" })')
tick, SIM = rt.globals().tick, rt.globals().SIM
tick(2)
check("a roster of one waits for nothing", CM.lgFetch == "done" and CM.lgHolding is not True and SIM.lever == 2)
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = nil, lever = 2 })')
tick, sent, logged = rt.globals().tick, rt.globals().sentMatching, rt.globals().logged
tick(1)
tick(1, 1050.0)
asks = lua_list(sent("LSNEED"))
# the stub clock ran one tick at the save's speed before the first pacing tick: the first
# clock read is a step or two past the save, which is exactly why the stamp rides in the save
check("no stamp in the save: asks after the first clock it read (approximate, and says so)",
      len(asks) == 1 and asks[0].startswith("LSNEED t=1000.37") and asks[0].endswith(" o=b save=1")
      and logged("an older build's save carries no stamp"), str(asks))

# ---- the catch-up feed of a live game far behind: bound by progress, never by a clock ----
rt = make()
CM = rt.eval('newInst({ T0 = 1000.0, savedAt = 1000.0, lever = 2 })')
tick, sent, logged, SIM = rt.globals().tick, rt.globals().sentMatching, rt.globals().logged, rt.globals().SIM
CM.lgFetch = "done"
tick(1, 1040.0)                        # paceV2 runs the catch-up: 40 behind the leader
asks = lua_list(sent("LSNEED"))
r = CM.catchUpTick(SIM.T, 2)
check("40 behind: holds and asks after its clock (no save=1)", r == 0 and len(asks) == 1
      and asks[0].startswith("LSNEED t=1000.") and asks[0].endswith(" o=b"), str(asks))
for _ in range(200):
    CM.ticks = CM.ticks + 1
    CM.peers.a.at = CM.ticks
    r = CM.catchUpTick(SIM.T, 2)
asks = lua_list(sent("LSNEED"))
check("200 ticks without a line: still holding (it used to run on after 160), re-asking every ~5 s",
      r == 0 and CM.cuPhase == "fetch" and 6 <= len(asks) <= 8, f"{len(asks)} asks, phase {CM.cuPhase}")
check("...loudly", logged("no history line for ~5 s") and logged("asked the host again"))
CM.histEndSeen = True
r = CM.catchUpTick(SIM.T, 2)
check("the end marker with no gaps: runs at catch-up speed", r == 4 and CM.cuPhase == "run", f"r={r}")

print()
print("ALL OK" if not fails else f"{len(fails)} FAILED")
sys.exit(1 if fails else 0)
