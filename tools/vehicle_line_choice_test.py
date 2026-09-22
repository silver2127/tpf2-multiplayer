"""Real Lua VLINE/clone replay: bounded automatic stop search and stale orders.

Engine callbacks and simulation pump are mocked, including delayed callbacks on
peers with different local ids and clocks. No game installation.
"""
from pathlib import Path
import os
import lupa.lua52 as lupa

SOURCE = Path(os.environ.get("TPF2_VEHICLES_LUA", Path(__file__).resolve().parents[1] / "mod/mp_lockstep_1/res/scripts/mp/vehicles.lua")).read_text(encoding="utf-8")


def runtime(letter="a", vid=70, lid=80):
    lua = lupa.LuaRuntime(unpack_returned_tuples=True)
    lua.globals().SOURCE = SOURCE
    lua.globals().LETTER, lua.globals().VID, lua.globals().LID = letter, vid, lid
    return lua.execute(r'''
local sent, callbacks, logs = {}, {}, {}
local count, successAt, now = 3, 1, 100
local CM = {ticks=0, seqNo=0}
local K = {INSTANCE=LETTER,STRICT_OPS={VLINE=true,VDEPOT=true,VSELL=true,VSTOP=true},VLINE_RETRY_STEPS=5,BIND_GUARD_STEPS=10}
function CM.gameTime() return now end
function CM.stepOf(t) return math.floor(t*5+0.5) end
function CM.lineIdFor(key) return LID end
api={type={ComponentType={LINE=1}},engine={getComponent=function(id,t)
  assert(id==LID); local stops={}; for i=1,count do stops[i]={} end; return {stops=stops}
end},cmd={make={
  setLine=function(v,l,s) assert(v==VID and l==LID); return {stop=s} end,
  sendToDepot=function(v) return {depot=true} end,
  sellVehicle=function(v) return {sell=true} end,
  setUserStopped=function(v,stopped) assert(v==VID); assert(type(stopped)=="boolean"); return {userStopped=stopped} end,
},sendCommand=function(cmd,cb)
  sent[#sent+1]={stop=cmd.stop,step=now*5,userStopped=cmd.userStopped}
  callbacks[#callbacks+1]=function() cb({},cmd.stop==successAt or cmd.depot or cmd.sell or cmd.userStopped~=nil or false) end
end}}
assert(load(SOURCE,"@vehicles.lua"))()(CM,K,function(s) logs[#logs+1]=s end)
CM.primedVeh[VID]=true
local H={CM=CM}
function H.order(stop,seq,armed)
  local c={op="VLINE",origin="a",seq=seq or 1,at=100,notBeforeStep=500,key="s:"..VID,line="a:3",stop=stop,armed=armed or 1}
  CM.execVehCmd(c); return c
end
function H.callback() local f=table.remove(callbacks,1); assert(f); f() end
function H.retry()
  local q=CM.retryQueue or {}; CM.retryQueue={}; for _,c in ipairs(q) do now=c.notBeforeStep/5; CM.execVehCmd(c) end
end
function H.cancel(op) CM.execVehCmd({op=op,origin="a",seq=9,at=100,armed=1,key="s:"..VID,keys="s:"..VID}) end
function H.vstop(origin,stopped,armed) CM.execVehCmd({op="VSTOP",origin=origin,seq=11,at=100,armed=armed,key="s:"..VID,stopped=stopped}) end
function H.userStopped(i) return sent[i].userStopped end
function H.clone() CM.queueCloneAssign({origin="a",seq=1,at=100,cline="a:3"},"s:"..VID) end
function H.clock(t) now=t end
function H.count(n) count=n end
function H.success(n) successAt=n end
function H.nsent() return #sent end
function H.stop(i) return sent[i].stop end
function H.step(i) return sent[i].step end
function H.nretry() return #(CM.retryQueue or {}) end
function H.logs() return table.concat(logs,"\n") end
function H.bound(yes) CM.primedVeh[VID]=yes end
return H
''')


def run():
    # Both peers get identical stops and agreed retry steps, even if the first
    # callback arrives with different local clocks. No send inside callbacks.
    for letter, vid, lid, clock in [("a", 70, 80, 100), ("b", 170, 180, 101)]:
        h = runtime(letter, vid, lid)
        c = h.order(-1)
        h.clock(clock)
        h.callback()
        assert h.nsent() == 1 and c.notBeforeStep == 505 and c.at == 100
        h.retry()
        assert h.stop(2) == 1 and h.step(2) == 505
        h.callback()
        assert h.nretry() == 0 and h.nsent() == 2
    print("ok  host/peer automatic fallback, fixed steps, unchanged stamp")

    h = runtime(); h.success(-99); h.order(-1)
    for _ in range(3):
        h.callback(); h.retry()
    assert [h.stop(i) for i in range(1,4)] == [0,1,2] and h.nretry() == 0
    assert [h.step(i) for i in range(1,4)] == [500,505,510]
    print("ok  all stops fail: each tried once, bounded exhaustion")

    for stop in [0,1]:
        h = runtime(); h.success(-99); h.order(stop); h.callback()
        assert h.nsent()==1 and h.nretry()==0
    h=runtime(); h.success(0); h.order(-1); h.callback()
    assert h.nsent()==1 and h.nretry()==0
    print("ok  explicit selection preserved; first-stop success ends search")

    for before_callback in [True,False]:
        for op in ["VLINE","VDEPOT","VSELL"]:
            h=runtime(); h.order(-1)
            if not before_callback: h.callback()
            if op=="VLINE": h.order(1,2)
            else: h.cancel(op)
            if before_callback: h.callback()
            h.retry()
            assert h.nsent()==2, (op,h.logs())
    print("ok  newer assignment/depot/sell cancels old callback or queued retry")

    h=runtime(); h.order(-1); h.callback(); h.order(1,2,0); h.retry()
    assert h.nsent()==1 and h.nretry()==0
    h=runtime(); h.bound(False); h.order(-1)
    assert h.nsent()==0 and h.nretry()==1
    h.bound(True); h.retry(); h.callback(); h.retry(); h.callback()
    assert [h.step(i) for i in [1,2]]==[505,510]
    print("ok  uncancelled order supersedes search; key-binding wait precedes stop search")

    h=runtime(); h.order(-1); h.callback(); h.count(1); h.retry()
    assert h.nsent()==1 and h.nretry()==0
    h=runtime(); h.count(0); h.order(-1)
    assert h.nsent()==0 and h.nretry()==0
    print("ok  removed stops/empty line cannot produce an invalid index")

    h=runtime(); h.clone(); h.retry(); h.callback(); h.retry(); h.callback()
    assert [h.stop(i) for i in [1,2]]==[0,1]
    assert [h.step(i) for i in [1,2]]==[510,515]
    print("ok  clones use automatic selection after the binding guard")

    # VSTOP: a peer applies the shipped state; the originator replays only when
    # the slice cancelled its click (armed=1), never twice when it ran natively.
    h=runtime(); h.vstop("b",1,1)
    assert h.nsent()==1 and h.userStopped(1) is True, h.logs()
    h.vstop("b",0,1)
    assert h.nsent()==2 and h.userStopped(2) is False
    h=runtime(); h.vstop("a",1,1)
    assert h.nsent()==1 and h.userStopped(1) is True, h.logs()
    h=runtime(); h.vstop("a",1,0)
    assert h.nsent()==0, h.logs()
    print("ok  VSTOP carries the absolute stop state; strict originator replay, native one skipped")


if __name__ == "__main__":
    run()
