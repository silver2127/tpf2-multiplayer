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
  - the ack keeps advancing across the NACK scan (CM.rxAdvance moves firstSeq and drops
    seen[] behind it; the ack used to stall there for good)
  - a rejoined origin restarts at seq 1: both lives are kept (keyed by stamp|origin|seq),
    served, and announced as separate runs; a NACK gets the newest
  - one feed per requester: a second LSNEED while a feed is in flight does not abandon it,
    a re-ask for the same stamp continues the feed, the tick budget is shared
  - the requester merges announced runs into what it tracks, in any arrival order (no reset
    on a re-ask; the seqs between two runs are not owed until a run covers them; every seq
    of a run is owed from its announcement, so a lost tail is NACKed even from a gone origin)
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
check("the feed is queued (3,000 lines)", CM.histFeeds.c is not None and len(CM.histFeeds.c.lines) == 3000)
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
h.clearSent()                          # d's request above was already refused loudly; its feed is pumped here
h.feed("LSNEED t=100.0 o=d save=1")
for _ in range(120):                   # b's catch-up feed above shares the budget until it ends
    CM.histPump()
ends = [l for l in lua_list(h.sent()) if l.startswith("LSHISTEND for=d")]
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

# ---- the ack keeps advancing across the NACK scan ----
L, h = runtime("c", False)
CM = h.CM
for seq in (1, 2, 3):
    h.feed(f"LSCMD op=T at=60.0000 origin=b seq={seq} x=1")
check("b:3 after 1..3", CM.ackReport() == " ak=b:3", repr(CM.ackReport()))
for seq in (4, 5, 6):
    h.feed(f"LSCMD op=T at=60.0000 origin=b seq={seq} x=1")
CM.nackScan()          # rxAdvance: firstSeq moves to 6 and seen[] behind it is dropped
check("...b:6 after a NACK scan advanced past them (the ack used to stall at 3 for good)",
      CM.ackReport() == " ak=b:6", f"{CM.ackReport()!r} firstSeq={CM.rx.b.firstSeq}")
for seq in range(7, 40):
    h.feed(f"LSCMD op=T at=60.0000 origin=b seq={seq} x=1")
check("...b:39 after more", CM.ackReport() == " ak=b:39", repr(CM.ackReport()))
h.feed("LSCMD op=T at=60.0000 origin=b seq=41 x=1")
CM.nackScan()
check("a gap (40) holds it at 39, through a scan", CM.ackReport() == " ak=b:39", repr(CM.ackReport()))
h.feed("LSCMD op=T at=60.0000 origin=b seq=40 x=1")
check("...filled: b:41", CM.ackReport() == " ak=b:41", repr(CM.ackReport()))

# ---- a rejoined origin restarts its sequence: both lives are kept and served ----
L, h = runtime()
CM = h.CM
h.pushMany("b", 5000, 0)                                   # b's first life: seq 1..5000, stamped 0.2..1000
h.clearLogs()
h.feed("LSCMD op=T at=1500.0000 origin=b seq=1 x=2")       # b crashed, rejoined, and starts again at 1
h.feed("LSCMD op=T at=1500.2000 origin=b seq=2 x=2")
check("the restarted seqs are kept beside the first life's (5,002 lines, not 5,000)", h.count() == 5002, str(h.count()))
check("...logged once, naming the origin", "b's seq 1 seen again with a new stamp (1500.0, was 0.2)" in h.logs())
check("a NACK for b:1 is answered with the newest life's line", CM.histFind("b", 1) == "LSCMD op=T at=1500.0000 origin=b seq=1 x=2")
check("a resend (same stamp) is still not kept twice", (h.feed("LSCMD op=T at=1500.0000 origin=b seq=1 x=2"), h.count())[1] == 5002)
h.clearSent(); h.clearLogs()
h.feed("LSNEED t=999.0 o=c save=1")                        # c needs everything after 999: b 4996..5000 and the new 1..2
ranges = [l for l in lua_list(h.sent()) if l.startswith("LSHIST for=c")]
check("the feed announces the two runs", ranges == ["LSHIST for=c o=b from=1 to=2", "LSHIST for=c o=b from=4996 to=5000"], str(ranges))
for _ in range(10):
    CM.histPump()
fed = [l for l in lua_list(h.sent()) if l.startswith("LSCMD") and l.endswith(" hfor=c")]
check("...and feeds all 7, no hole", len(fed) == 7 and any("at=1500.0000 origin=b seq=1 " in l for l in fed)
      and [l for l in lua_list(h.sent()) if l.startswith("LSHISTEND")] == ["LSHISTEND for=c n=7"], str(len(fed)))
CM.histFloor, CM.rosterPlayers = 1000.0, 2
h.peer("b", True)
CM.histPrune()
check("a prune keeps the index pointing at what is left", h.count() == 2 and CM.histFind("b", 1) is not None
      and CM.histFind("b", 3) is None, f"{h.count()} {CM.histFind('b', 3)}")

# ---- one feed per requester ----
L, h = runtime()
CM = h.CM
h.pushMany("b", 3000, 0)
h.clearSent(); h.clearLogs()
CM.histFloor, CM.rosterPlayers = 1.0, 1        # so the hold's only reason can be the feeds
CM.histServe(0.0, "c")                          # (served directly: h.feed would pump once first)
CM.histPump()
CM.histServe(0.0, "d")                          # d asks while c's feed is going out
check("d's request does not abandon c's feed (c at 41, d at 1)",
      CM.histFeeds.c is not None and CM.histFeeds.c.i == 41 and CM.histFeeds.d is not None and CM.histFeeds.d.i == 1)
check("...and the hold says a feed is in flight", CM.histHold() == "a history feed is in flight", str(CM.histHold()))
CM.histPump()
check("the tick budget is shared: 20 lines each (c at 61, d at 21)", CM.histFeeds.c.i == 61 and CM.histFeeds.d.i == 21,
      f"c={CM.histFeeds.c.i} d={CM.histFeeds.d.i}")
h.clearLogs()
CM.histServe(0.0, "c")                          # c saw nothing for a while and asks again
check("a re-ask for the same stamp continues c's feed (still at 61), re-sending its ranges",
      CM.histFeeds.c.i == 61 and "asked again for everything after 0.0 while its feed is at 60 of 3000 -- continuing" in h.logs()
      and lua_list(h.sent()).count("LSHIST for=c o=b from=1 to=3000") == 2, str(CM.histFeeds.c.i))
for _ in range(400):
    CM.histPump()
sent = lua_list(h.sent())
check("both feeds run to their end", sorted(l for l in sent if l.startswith("LSHISTEND")) == ["LSHISTEND for=c n=3000", "LSHISTEND for=d n=3000"])
check("...each requester got every line once", sum(1 for l in sent if l.endswith(" hfor=c")) == 3000 and sum(1 for l in sent if l.endswith(" hfor=d")) == 3000)
check("...and the hold no longer names a feed", CM.histHold() is None, str(CM.histHold()))
h.clearLogs()
CM.histServe(0.0, "c")                          # c lost the end marker: a fresh feed
check("after its feed ended a re-ask is a fresh feed", CM.histFeeds.c is not None and CM.histFeeds.c.i == 1 and "a fresh feed: its last one, 3000 line(s), ended" in h.logs())

# ---- the requester merges announced runs into what it tracks ----
L, h = runtime("c", False)
CM = h.CM
h.feed("LSTICK t=50 o=b s=250 hi=5000 ms=1")               # b's heartbeat first: first contact at 5000, nothing owed below
h.feed("LSHIST for=c o=b from=1 to=2")
h.feed("LSHIST for=c o=b from=4996 to=5000")
check("the runs are tracked from their first seq", CM.rx.b.firstSeq == 0, str(CM.rx.b.firstSeq))
check("the seqs between the runs are not owed: 7 gaps, not 5,000", CM.rxGaps()[0] == 7, str(CM.rxGaps()[0]))
check("...and logged", "b seq 3..4995 are not in the history (its sequence restarted) -- not owed" in h.logs())
for seq in (1, 2, 4996, 4997, 4998, 4999):
    h.feed(f"LSCMD op=T at=999.2000 origin=b seq={seq} x=1 hist=1 hfor=c")
check("six of seven arrived: one gap (the tail, 5000)", CM.rxGaps()[0] == 1 and CM.rxGaps()[3] == 5000, str(CM.rxGaps()))
h.feed("LSHIST for=c o=b from=4996 to=5000")               # the range again (a re-ask): no reset
check("a repeated range keeps what is held (still one gap)", CM.rxGaps()[0] == 1, str(CM.rxGaps()[0]))
h.clearSent()
CM.ticks = CM.ticks + 20
CM.nackScan()
check("the known, missing tail is NACKed", "LSNACK o=b seq=5000 by=c" in lua_list(h.sent()), str(lua_list(h.sent())))
h.feed("LSCMD op=T at=999.4000 origin=b seq=5000 x=1 hist=1 hfor=c")
check("complete: no gap, ack b:5000", CM.rxGaps()[0] == 0 and CM.ackReport() == " ak=b:5000", CM.ackReport())
# runs in any arrival order, and a later run that covers a "not owed" seq
L, h = runtime("c", False)
CM = h.CM
h.feed("LSHIST for=c o=b from=30 to=40")               # the higher run first (reordered on the wire)
h.feed("LSHIST for=c o=b from=5 to=6")
check("runs arriving out of order: 7..29 are not owed, 13 gaps", CM.rxGaps()[0] == 13 and CM.rx.b.firstSeq == 4, f"{CM.rxGaps()[0]} first={CM.rx.b.firstSeq}")
h.feed("LSHIST for=c o=b from=7 to=29")                # a later feed (another stamp) holds the middle after all
check("a run covering them makes 7..29 owed again: 36 gaps", CM.rxGaps()[0] == 36, str(CM.rxGaps()[0]))
# the host announces runs by origin then seq, numerically
L, h = runtime()
CM = h.CM
for seq in (5, 6, 30, 31):
    h.feed(f"LSCMD op=T at=60.0000 origin=b seq={seq} x=1")
h.feed("LSCMD op=T at=60.0000 origin=a seq=9 x=1")
h.clearSent()
CM.histServe(0.0, "c")
runs = [l for l in lua_list(h.sent()) if l.startswith("LSHIST")]
check("runs are announced by origin then seq, numerically (5..6 before 30..31)",
      runs == ["LSHIST for=c o=b from=5 to=6", "LSHIST for=c o=b from=30 to=31"], str(runs))

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
