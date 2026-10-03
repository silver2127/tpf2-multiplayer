"""Exercise the shared pacing factory with Lua 5.2, without a game."""
from pathlib import Path
from lupa.lua52 import LuaRuntime

lua = LuaRuntime(unpack_returned_tuples=True)
factory = lua.execute((Path(__file__).resolve().parents[2] /
    'mod/mp_lockstep_1/res/scripts/mp/pacing.lua').read_text())
lua.globals().factory = factory
lua.execute('''
local logs, sent = {}, {}
local CM = {cfgFlag=function() return nil end, ticks=0, peers={}, catchingUp2=true, cuPhase="fetch", cuSince=0,
            cuFrom=50, cuAsks=1, histProgressAt=0, effSpeed=1}
local K = {EXEC_DELAY=0.4, SIM_STEP=0.2, BASE="", INSTANCE="b", PEER_STALE_TICKS=100, PACE_OTHER_GAME=600}
factory(CM,K,function(s) logs[#logs+1]=s end)
CM.isLeader=function() return false end
CM.leaderPrecise=function() return 60 end
CM.rxGaps=function() return 1 end
CM.broadcast=function(s) sent[#sent+1]=s end
for i=1,10 do
    CM.ticks=CM.ticks+K.HIST_STALL_TICKS+1
    assert(CM.catchUpTick(50,1)==0)
    assert(CM.catchingUp2 and CM.cuPhase=="fetch")
end
assert(#sent==10 and CM.cuAsks==11)
local warnings=0
for _,s in ipairs(logs) do if s:find("still asking",1,true) then warnings=warnings+1 end end
assert(warnings==1)
CM.histEndSeen=true
CM.rxGaps=function() return 0 end
assert(CM.catchUpTick(50,1)==4 and CM.cuPhase=="run")

-- Catch-up diagnostics use elapsed wall time, only in the running phase.
local realTime = os.time
local wall, leader = 1000, 680
os.time = function() return wall end
CM.leaderPrecise = function() return leader end
CM.cuRate = nil
logs = {}
assert(CM.catchUpTick(100,1)==4 and #logs==0)
wall=1019; leader=699
assert(CM.catchUpTick(140,1)==4 and #logs==0)
wall=1020; leader=700
assert(CM.catchUpTick(142,1)==4)
assert(logs[1]=="CATCHUP: 558.0 unit(s) behind, closing 1.10/s (this game 2.10/s at 4x, the session 1.00/s) -- ~507 s to go")
assert(CM.catchUpTick(142,1)==4 and #logs==1)
-- A late tick divides by the actual 25 seconds, not a fixed interval.
wall=1045; leader=725
assert(CM.catchUpTick(192,1)==4)
assert(logs[2]=="CATCHUP: 533.0 unit(s) behind, closing 1.00/s (this game 2.00/s at 4x, the session 1.00/s) -- ~533 s to go")
-- A stationary or widening gap must not promise an ETA.
wall=1065; leader=745
assert(CM.catchUpTick(212,1)==4)
assert(logs[3]=="CATCHUP: 533.0 unit(s) behind, closing 0.00/s (this game 1.00/s at 4x, the session 1.00/s)")
wall=1085; leader=785
assert(CM.catchUpTick(232,1)==4)
assert(logs[4]=="CATCHUP: 553.0 unit(s) behind, closing -1.00/s (this game 1.00/s at 4x, the session 2.00/s)")
leader=233
assert(CM.catchUpTick(232,1)==nil and CM.cuRate==nil)
-- A new episode drops stale samples while fetching; fetch time is excluded.
CM.cuRate={wall=0,now=0,behind=999}
leader=900
assert(CM.catchUpTick(300,1)==0 and CM.cuRate==nil)
wall=1185; logs={}
assert(CM.catchUpTick(300,1)==0 and #logs==0 and CM.cuRate==nil)
CM.histEndSeen=true
assert(CM.catchUpTick(300,1)==4 and CM.cuRate.wall==1185)
wall=1205; leader=920
assert(CM.catchUpTick(340,1)==4)
assert(logs[#logs]=="CATCHUP: 580.0 unit(s) behind, closing 1.00/s (this game 2.00/s at 4x, the session 1.00/s) -- ~580 s to go")
os.time=realTime

-- PID resets accumulated error on speed changes, unpause and zero crossing.
CM.leaderPrecise=function() return 50 end
CM.catchingUp2=false
for _,kind in ipairs({"speed", "unpause", "crossing"}) do
    CM.ticks=100; CM.pidAt=99; CM.pidI=10; CM.pidEff=1
    CM.unitsPerTick=0.2; CM.pidPrevNow=nil; CM.pidPrevTick=nil; CM.pidFar=nil; CM.pidRecover=nil
    CM.pidLastE=kind=="crossing" and -0.1 or 0.1
    CM.pacePaused=kind=="unpause"
    CM.pidPace(50.1,kind=="speed" and 2 or 1)
    assert(CM.pidI==0,kind)
end

-- Lag smoothing retains part of a spike and clears on pause/disable.
CM.governorOff=function() return false end
CM.ticks=200; CM.peers={a={time=49,at=200}}
CM.governSpeed(50,1)
assert(math.abs(CM.govEmaLag-1)<1e-9)
CM.peers.a.time=40; CM.govAt=nil
CM.governSpeed(50,1)
assert(math.abs(CM.govEmaLag-2.8)<1e-9)
CM.governSpeed(50,0)
assert(CM.govEmaLag==nil and CM.govPrevSmoothed==nil)
CM.govEmaLag=10; CM.govPrevSmoothed=10
CM.governorOff=function() return true end
CM.governSpeed(50,1)
assert(CM.govEmaLag==nil and CM.govPrevSmoothed==nil)
''')
print('PASS: persistent history retries, recovery when complete, catch-up rate diagnostics, PID anti-windup, lag smoothing/reset')
