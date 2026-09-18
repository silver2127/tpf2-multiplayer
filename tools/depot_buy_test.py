"""Exercise real VBUY replay with offset depot geometry and stale construction keys.

Lua 5.2, mocked engine boundary; no installation or game commands.
"""
from pathlib import Path
import os
import lupa.lua52 as lupa

ROOT = Path(__file__).resolve().parents[1]
source = Path(os.environ.get("TPF2_VEHICLES_LUA", ROOT / "mod/mp_lockstep_1/res/scripts/mp/vehicles.lua")).read_text(encoding="utf-8")


def runtime():
    lua = lupa.LuaRuntime(unpack_returned_tuples=True)
    lua.globals().SOURCE = source
    return lua.execute(r'''
local world, nearby, all, sent, logs = {}, {}, {}, {}, {}
local scans = 0
local buySuccess = true
local CM = {consByKey={}, ticks=10, cmMode="coop"}
local K = {INSTANCE="a", STRICT_OPS={VBUY=true, VLINE=true}}
function CM.conKey(x,y) return string.format("%.1f/%.1f",x,y) end
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor(t*5+0.5) end
api = {
  type = {ComponentType={CONSTRUCTION=1}},
  engine = {
    entityExists=function(id) assert(type(id)=="number" and id>=0); return world[id]~=nil end,
    getComponent=function(id,t) assert(type(id)=="number" and id>=0); return world[id] end,
    util={getPlayer=function() return 1 end},
  },
  cmd = {
    make={buyVehicle=function(who,depot,config) return {who=who,depot=depot} end,
          setLine=function(v,l,s) return {vehicle=v,line=l,stop=s} end},
    sendCommand=function(cmd,cb) sent[#sent+1]=cmd; cb({resultEntity=700},not cmd.line and buySuccess) end,
  },
}
game={interface={getEntities=function(area)
  if area.radius==6 then return nearby end
  scans=scans+1; return all
end}}
assert(load(SOURCE,"@vehicles.lua"))()(CM,K,function(s) logs[#logs+1]=s end)
-- Configuration decoding is covered elsewhere; exercise actual depot resolution,
-- strict-origin replay, command submission and callback here.
buildVehConfig=function() return {},1 end
local H={CM=CM}
function H.add(id,file,x,y,child,near)
  world[id]={fileName=file,transf={[13]=x,[14]=y},depots=child and {child} or {}}
  all[#all+1]=id
  if near then nearby[#nearby+1]=id end
end
function H.cache(id) CM.consByKey[CM.conKey(10,20)]={id=id} end
function H.remove(id) world[id]=nil end
function H.buy(origin,armed,file)
  CM.execVBuy({op="VBUY",origin=origin or "a",armed=armed or 1,seq=#sent+1,at=100,x=10,y=20,file=file or "depot/train.con"})
end
function H.buySuccess(yes) buySuccess=yes end
function H.count() return #sent end
function H.target() return sent[#sent] and sent[#sent].depot end
function H.scans() return scans end
function H.logs() return table.concat(logs,"\n") end
function H.assign(stop)
  CM.primedVeh[700]=true
  world[800]={stops={{},{}}}
  K.VLINE_RETRY_STEPS=5
  CM.lineIdFor=function(key) return key=="b:32" and 800 or nil end
  CM.execVehCmd({op="VLINE",origin="a",armed=1,seq=4,at=100,key="s:700",line="b:32",stop=stop})
end
function H.stop() return sent[#sent] and sent[#sent].stop end
return H
''')


def check(label, fn):
    fn()
    print("ok  " + label)


def offset():
    h = runtime()
    h.add(100, "depot/train.con", 10, 20, 101, False)
    h.buy()
    assert h.count() == 1 and h.target() == 101, h.logs()
    scans = h.scans()
    h.buy("b")
    assert h.count() == 2 and h.scans() == scans, h.logs()


def stale():
    for file, x, y, child in [("depot/road.con", 10, 20, 201), ("depot/train.con", 15, 20, 201), ("depot/train.con", 10, 20, None)]:
        h = runtime()
        h.add(200, file, x, y, child, True)
        h.cache(200)
        h.add(100, "depot/train.con", 10, 20, 101, False)
        h.buy()
        assert h.count() == 1 and h.target() == 101, h.logs()


def nearby_order():
    h = runtime()
    h.add(200, "depot/road.con", 10, 20, 201, True)
    h.add(300, "depot/train.con", 11, 20, 301, True)
    h.add(100, "depot/train.con", 10, 20, 101, True)
    h.buy()
    assert h.target() == 101 and h.scans() == 0, h.logs()


def invalid():
    for cache in [None, -1, 999]:
        h = runtime()
        if cache is not None:
            h.cache(cache)
        h.add(100, "depot/train.con", 11, 20, 101, False)
        h.buy()
        assert h.count() == 0, h.logs()
    h = runtime()
    h.add(100, "depot/train.con", 10, 20, 101, False)
    h.add(200, "depot/train.con", 10, 20, 201, False)
    h.buy()
    assert h.count() == 0, "ambiguous depot must not depend on local enumeration order"


def replacement():
    h = runtime()
    h.add(100, "depot/train.con", 10, 20, 101, False)
    h.buy()
    h.remove(100)
    h.add(200, "depot/train.con", 10, 20, 201, False)
    h.buy()
    assert h.count() == 2 and h.target() == 201, h.logs()


def uncancelled():
    h = runtime()
    h.add(100, "depot/train.con", 10, 20, 101, True)
    h.buy("a", 0)
    assert h.count() == 0 and h.scans() == 0, h.logs()


def assignment_diagnostic():
    for requested, actual in [(-1, 0), (1, 1)]:
        h = runtime()
        h.assign(requested)
        assert h.count() == 1 and h.stop() == actual, h.logs()
        assert f"line=b:32 stop={actual} requestedStop={requested} success=false" in h.logs(), h.logs()
        if requested < 0:
            assert len(h.CM.retryQueue) == 1 and h.CM.retryQueue[1].autoStop == 1
        else:
            assert h.CM.retryQueue is None, "explicit stop must not fall back"


if __name__ == "__main__":
    for name, test in [("offset geometry, origin and peer, cached batch", offset),
                       ("stale/reused registry entry", stale), ("nearby order and position/type filtering", nearby_order),
                       ("missing, invalid and ambiguous depot", invalid), ("replacement invalidates cache", replacement),
                       ("uncancelled origin never buys twice", uncancelled),
                       ("line failure records destination and preserves explicit stop", assignment_diagnostic)]:
        check(name, test)
