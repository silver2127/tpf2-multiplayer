"""Offline checks for command retention in net.lua (Lua 5.2): no rings, no counts.

The command history used to be a 4,096-line ring and a sender's own lines a 256-deep ring:
a joiner whose save predated the oldest retained command got a hole (a desync at the
moment it joined) and a NACK for an old command found nothing. Now:
  - CM.hist keeps every command until a joiner reports the stamp of the save it loaded
    (LSNEED ... save=1) AND the whole lobby roster is heard AND nobody is catching up;
    then everything stamped at or before that stamp is pruned (every later save holds it)
  - a request for pruned history is refused loudly: LSHISTEND carries hole=<stamp>, both
    ends log it
  - a plain catch-up LSNEED (a live clock, not a save) moves no floor
  - CM.sentRing keeps a line until every live peer acknowledges past it (ak= on the
    heartbeat, CM.ackReport); a live peer that has not reported keeps everything; a NACK
    for a pruned line is answered from the history, else logged loudly
  - the heartbeat and the LSNEED/LSHISTEND readers are wired (text checks on lockstep.lua)

    python tools/hist_retention_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "net.lua")
LOCKSTEP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
PACING = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "pacing.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def lua_list(t):
    return [t[i] for i in range(1, len(t) + 1)]


def runtime(instance="a", leader=True):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().NET_SRC = open(NET, encoding="utf-8").read()
    L.globals().EVENTS = os.path.join(tempfile.mkdtemp(), "events.txt").replace("\\", "/")
    L.globals().INSTANCE = instance
    L.globals().LEADER = leader
    h = L.execute(r'''
CLK = 100.0
os.clock = function() return CLK end
local logs, sent = {}, {}
local K = {
  INSTANCE = INSTANCE, SIM_STEP = 0.2, EXEC_DELAY = 0.4, EXEC_DELAY_MIN = 0.4, EXEC_DELAY_MAX = 3.0,
  DELAY_SLACK_MS = 50, DELAY_DEV_MULT = 1, DELAY_DOWN_TICKS = 25, RTT_MIN_SAMPLES = 8, PEER_STALE_TICKS = 25,
  GAP_HOLD_GRACE_TICKS = 1, GAP_HOLD_ENGAGE_TICKS = 3, GAP_HOLD_MAX_TICKS = 55,
  NACK_MAX = 10, NACK_GRACE = 15, NACK_EVERY = 30, NACK_PER_SCAN = 12, RESEND_MIN_GAP = 5, EVENTS_FILE = EVENTS,
}
local CM = { ticks = 1000, peers = {}, seqNo = 0, queue = {}, MAX_LEAD = 15, execDelayAuto = false }
local now = 50.0
function CM.gameTime() return now end
function CM.stepOf(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
function CM.peerFor(o) local pr = CM.peers[o]; if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end; return pr end
function CM.peerBounds() return nil, nil end
function CM.broadcast(line) sent[#sent + 1] = line end
function CM.cmEnsure() end
function CM.isLeader() return LEADER end
function CM.livePeers()
  local n = 0
  for _, pr in pairs(CM.peers) do if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then n = n + 1 end end
  return n
end
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
function H.peer(o, fresh, cu)
  local pr = CM.peerFor(o)
  pr.at = fresh and CM.ticks or (CM.ticks - 100)
  pr.cu = cu or nil
  return pr
end
function H.pushMany(o, n, from)
  for i = 1, n do
    CM.histPush(string.format("LSCMD op=T at=%.4f origin=%s seq=%d x=1", from + i * 0.2, o, i), from + i * 0.2)
  end
end
function H.count() return #CM.hist end
function H.oldestAt() return CM.hist[1] and CM.hist[1].at or -1 end
return H
''')
    return L, h


# ---- the history has no count cap ----
L, h = runtime()
CM = h.CM
h.pushMany("b", 5000, 0)
check("5,000 commands retained (the ring held 4,096)", h.count() == 5000, str(h.count()))
check("the oldest is still found for a NACK", CM.histFind("b", 1) is not None)
check("its size is logged at 4,096 lines, with why it is kept",
      "HIST: 4096 command(s) retained" in h.logs() and "no joiner has loaded a save yet" in h.logs())
check("a resend of a held line is not kept twice", (CM.histPush("LSCMD op=T at=0.2000 origin=b seq=1 x=1", 0.2), h.count())[1] == 5000)

# ---- the floor: a joiner's save stamp, and when it may prune ----
h.clearLogs(); h.clearSent()
h.peer("b", True)
h.feed("LSNEED t=400.0 o=c save=1")
check("LSNEED ... save=1 sets the floor at the save's stamp", CM.histFloor == 400.0, str(CM.histFloor))
served = [l for l in lua_list(h.sent()) if l.startswith("LSHIST for=c")]
check("the leader serves c the commands stamped after 400 (b seq 2001..5000)", served == ["LSHIST for=c o=b from=2001 to=5000"], str(served))
check("the feed is queued (3,000 lines)", CM.histSend is not None and len(CM.histSend.lines) == 3000)
for _ in range(80):
    CM.histPump()
ends = [l for l in lua_list(h.sent()) if l.startswith("LSHISTEND")]
check("...and closed with LSHISTEND, no hole", ends == ["LSHISTEND for=c n=3000"], str(ends))
CM.rosterPlayers = None
CM.histPrune()
check("roster unknown: nothing pruned", h.count() == 5000 and "roster size is unknown" in CM.histHold())
CM.rosterPlayers = 3
CM.histPrune()
check("one roster member not heard (still loading?): nothing pruned", h.count() == 5000 and "not heard" in CM.histHold(), CM.histHold())
h.peer("c", True, True)
CM.histPrune()
check("c heard but catching up (cu=1): nothing pruned", h.count() == 5000 and "catching up" in CM.histHold(), CM.histHold())
h.peer("c", True, False)
check("everyone in, nobody catching up: prunable", CM.histHold() is None, str(CM.histHold()))
CM.histPrune()
check("pruned: only commands stamped after 400 remain", h.count() == 3000 and h.oldestAt() > 400.0, f"{h.count()} oldest {h.oldestAt()}")
check("the prune is logged with the stamp and the count", "HIST: pruned 2000 command(s) stamped at or before 400.0" in h.logs())
check("a pruned line is no longer found", CM.histFind("b", 2001) is not None and CM.histFind("b", 2000) is None)
check("a plain catch-up LSNEED (a live clock, not a save) moves no floor",
      (h.feed("LSNEED t=900.0 o=b"), CM.histFloor)[1] == 400.0, str(CM.histFloor))
check("an older save's stamp does not lower the floor", (h.feed("LSNEED t=100.0 o=d save=1"), CM.histFloor)[1] == 400.0)

# ---- a request below the prune is refused loudly ----
h.clearLogs(); h.clearSent()
h.feed("LSNEED t=100.0 o=d save=1")
for _ in range(80):
    CM.histPump()
ends = [l for l in lua_list(h.sent()) if l.startswith("LSHISTEND")]
check("the end marker names the hole", ends == ["LSHISTEND for=d n=3000 hole=400.0000"], str(ends))
check("...and the host logs it loudly", "!! HIST: d needs every command after 100.0 but everything at or before 400.0 was pruned" in h.logs())
h.clearLogs()
h.feed("LSHISTEND for=a n=3 hole=400.0000")
check("the requester logs a hole loudly", CM.histHole == 400.0 and "THIS GAME IS FORKED" in h.logs())

# ---- own lines: kept until every live peer acknowledges ----
L, h = runtime()
CM = h.CM
h.clearSent()
for i in range(300):
    CM.scheduleLocal("T", L.table_from({"x": i}))
check("300 own lines kept (the ring held 256)", CM.sentRing[1] is not None and CM.sentRing[300] is not None and CM.sentLo == 1)
h.peer("b", True)
CM.sentPrune()
check("a live peer that has not reported about us: nothing dropped", CM.sentRing[1] is not None)
h.feed("LSTICK t=50 o=b s=250 hi=0 ms=1 ak=a:120")
check("ak= on the heartbeat is read", CM.peers.b.ackMine == 120)
h.peer("c", True)
CM.sentPrune()
check("a second live peer without an ack still holds everything", CM.sentRing[1] is not None)
h.feed("LSTICK t=50 o=c s=250 hi=0 ms=1 ak=a:200,b:5")
CM.sentPrune()
check("dropped through the lowest live ack (120): 121 is the first kept", CM.sentRing[120] is None and CM.sentRing[121] is not None and CM.sentLo == 121,
      f"lo={CM.sentLo}")
h.peer("c", False)
CM.sentPrune()
check("a stale peer's ack no longer counts (b alone at 120: nothing more)", CM.sentLo == 121)
h.feed("LSTICK t=50 o=b s=250 hi=0 ms=1 ak=a:250")
CM.sentPrune()
check("...until it reports further: 251 is the first kept", CM.sentRing[250] is None and CM.sentRing[251] is not None)
h.clearSent(); h.clearLogs()
CM.onNack("a", 5)
check("a NACK for a dropped line is answered from the history", any(l.startswith("LSCMD op=T") and "seq=5 " in l for l in lua_list(h.sent()))
      and "RESEND seq=5" in h.logs())
CM.histFloor, CM.rosterPlayers, CM.histPrunedTo = 999.0, 2, None
h.peer("b", True)
CM.histPrune()
h.clearSent(); h.clearLogs()
CM.onNack("a", 5)
check("a NACK for a line pruned everywhere is refused loudly", not lua_list(h.sent()) and "!! NACK for our seq=5 but it is no longer kept" in h.logs())
h.clearSent()
h.feed("LSTICK t=50 o=b s=250 hi=0 ms=1 ak=a:255")
CM.ticks = 1024
CM.pollEvents()
check("the prune runs from pollEvents (every 32 ticks)", CM.sentLo == 256, f"lo={CM.sentLo}")

# ---- our own ack report ----
L, h = runtime("c", False)
CM = h.CM
for seq in (1, 2, 3, 5):
    h.feed(f"LSCMD op=T at=60.0000 origin=b seq={seq} x=1")
h.feed("LSCMD op=T at=60.0000 origin=a seq=7 x=1")
check("ak= reports the contiguous high-water per origin (b:3, a:7)", CM.ackReport() == " ak=a:7,b:3", repr(CM.ackReport()))
h.feed("LSCMD op=T at=60.0000 origin=b seq=4 x=1")
check("...advancing once the gap fills", CM.ackReport() == " ak=a:7,b:5", repr(CM.ackReport()))
check("nothing heard: no ak= field", runtime("d", False)[1].CM.ackReport() == "")

# ---- wiring ----
lockstep = open(LOCKSTEP, encoding="utf-8").read()
net = open(NET, encoding="utf-8").read()
pacing = open(PACING, encoding="utf-8").read()
check("the heartbeat carries ak=", "CM.ackReport and CM.ackReport() or \"\"" in lockstep)
check("no command ring constant is left", "K.CMD_RING =" not in lockstep and "K.HIST_RING" not in net)
check("the save carries its stamp and the loader reads it", "savedAt = CM.gameTime" in lockstep and "CM.savedAt = tonumber(s.savedAt)" in lockstep)
check("the load gate asks with save=1", 'o=%s save=1", S, K.INSTANCE' in pacing)
check("the heartbeat says cu=1 until the load gate has its history", 'CM.lgFetch ~= "done"' in lockstep)

print()
print("ALL OK" if not fails else f"{len(fails)} FAILED")
sys.exit(1 if fails else 0)
