"""Run installed AutoSig2's real spacing algorithm through our proposal adapter.

Pass the Workshop mod folder as argv[1] if installed elsewhere. Engine submission
is mocked: this proves capture/translation and isolation, not an in-game build.
"""
from pathlib import Path
import sys
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[1]
workshop = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('D:/SteamLibrary/steamapps/workshop/content/1066780/2138210967')
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().MP = (root / 'mod/mp_lockstep_1/res/scripts').as_posix()
lua.globals().WORKSHOP = (workshop / 'res/scripts').as_posix()
lua.globals().AUTOSIG = (workshop / 'res/config/game_script/autosig2.lua').read_text(encoding='utf-8')
lua.execute(r'''
package.path=MP..'/?.lua;'..WORKSHOP..'/?.lua;'..package.path
_=function(s) return s end
getBuildVersion=function() return 35924 end
local function vec(x,y,z)
  return setmetatable({x,y,z},{__index=function(t,k) return rawget(t,({x=1,y=2,z=3})[k]) end})
end
local CT={BASE_EDGE=1,BASE_EDGE_TRACK=2,BASE_NODE=3,TRANSPORT_NETWORK=4,MODEL_INSTANCE_LIST=5}
local edge={node0=11,node1=12,tangent0=vec(2000,0,0),tangent1=vec(2000,0,0),type=0,typeIndex=-1,objects={{301,2}}}
local reverseSeed=false
local make=function() error('native build escaped planner') end
local send=function() error('native command escaped planner') end
api={type={ComponentType=CT,EdgeId={new=function(e,s) return {entity=e,section=s} end},
  SimpleProposal={new=function() return {streetProposal={edgesToAdd={},edgesToRemove={},edgeObjectsToAdd={},edgeObjectsToRemove={}}} end},
  SegmentAndEntity={new=function() return {comp={tangent0=vec(),tangent1=vec()},trackEdge={}} end},
  SimpleStreetProposal={EdgeObject={new=function() return {} end}}},
  engine={util={getPlayer=function() return 99 end},system={
    streetSystem={getNode2TrackEdgeMap=function() return {[11]={201},[12]={201}} end},
    streetConnectorSystem={getNode2StreetConnectorMap=function() return {} end},
    signalSystem={getSignal=function(id,reverse)
      return {entity=((not reverseSeed and reverse and id.section==1) or (reverseSeed and not reverse and id.section==0)) and 301 or -1}
    end}},getComponent=function(id,kind)
      assert(id,'nil entity passed to engine')
      if kind==CT.BASE_EDGE then return edge end
      if kind==CT.BASE_EDGE_TRACK then return {trackType=0,catenary=true} end
      if kind==CT.BASE_NODE then return {position=vec(id==11 and 0 or 2000,0,0)} end
      if kind==CT.TRANSPORT_NETWORK then return {edges={{geometry={length=reverseSeed and 1700 or 100}},{geometry={length=reverseSeed and 300 or 1900}}}} end
    end},cmd={make={buildProposal=make},sendCommand=send}}
game={}
assert(load(AUTOSIG))()
local original=data()
local compat=require('mp/autosig_compat')
local script=compat.wrap(original)
script.load({distance=500,use=true,replace=false,remove=false,backward=false})
local CM={escName=function(s) return s end,unescName=function(s) return s end}
local geom=require('mp/geom')(CM,{},function() end)
CM.hermitePos,CM.hermiteTangent=geom.hermitePos,geom.hermiteTangent
local commands={}
CM.scheduleLocal=function(op,c) assert(op=='STOPADD');commands[#commands+1]=c end
compat.bind(CM,{INSTANCE='a'},function() end)
local seed={track=1,kind=2,origin='a',model='rail/signal.mdl',oneWay=1,company=2}
CM.autoSigCapture(seed);assert(seed.autosig==500)
-- Changes after capture must not change the clicked action's spacing.
script.load({distance=1000,use=false,replace=false,remove=false,backward=true})
CM.autoSigAfterSeed(seed,{11,12},{},false)
assert(#commands==3,'expected 600/1100/1600m signals')
for i,c in ipairs(commands) do
  assert(math.abs(c.x-(100+500*i))<0.001 and c.y==0)
  assert(c.eleft==0 and c.oneWay==1 and c.company==2)
  assert(c.model=='rail/signal.mdl' and c.autosig==nil and c.autosigFollow==1)
  assert(c.node0==nil and c.entity==nil and c.player==nil,'entity id leaked onto wire')
end
assert(original.save().distance==1000 and original.save().use==false and original.save().backward)
assert(api.cmd.make.buildProposal==make and api.cmd.sendCommand==send)
seed.origin='b';CM.autoSigAfterSeed(seed,{11,12},{},false);assert(#commands==3,'peer expanded seed twice')
seed.origin='a';seed.autosig=nil;CM.autoSigAfterSeed(seed,{11,12},{},false);assert(#commands==3,'follow-up recursed')
for _,mode in ipairs({'off','replace','remove'}) do
  script.load({distance=500,use=mode~='off',replace=mode=='replace',remove=mode=='remove'})
  local c={track=1,kind=2};CM.autoSigCapture(c);assert(c.autosig==nil)
end
-- Error path restores both original APIs and UI settings; no partial batch.
seed.autosig=500
local savedGet=api.engine.getComponent
api.engine.getComponent=function() error('test failure') end
assert(not pcall(CM.autoSigAfterSeed,seed,{11,12},{},false))
assert(api.cmd.make.buildProposal==make and api.cmd.sendCommand==send)
assert(original.save().remove==true and #commands==3)
api.engine.getComponent=savedGet
script.guiHandleEvent('streetTerminalBuilder','builder.apply',{proposal={proposal={edgeObjectsToAdd={{resultEntity=-1}}}}})
script.guiHandleEvent('streetTerminalBuilder','builder.apply',nil)
-- Opposite direction uses AutoSig's real route logic, not our own spacing loop.
reverseSeed=true;commands={};edge.objects={{301,2}}
script.load({distance=500,use=true,replace=false,remove=false})
CM.autoSigAfterSeed(seed,{11,12},{},true)
assert(#commands==3)
for i,c in ipairs(commands) do assert(math.abs(c.x-(1700-500*i))<0.001 and c.eleft==1) end
-- Feed generated commands into the real STOPADD replay on two company maps.
for _,pid in ipairs({2002,3003}) do
  require('mp/stops')(CM,{INSTANCE='b',STOP_EDGE_EPS=12},function() end)
  CM.edgeGeomT=geom.edgeGeomT
  CM.findEdgeByEnds=function() return 201 end
  CM.uOnEdgeFine=function(e,x) return x/2000 end
  CM.objectsOnEdge=function() return {},0 end
  CM.cmEnsure=function() end;CM.cmMode='companies';CM.cmCompanyPid={[2]=pid}
  CM.stopSettleOwner=function() end
  local replayed={}
  CM.nativeStopProposal=function(add,remove,why,done)
    replayed[#replayed+1]=add;done(true,{});return true
  end
  for _,c in ipairs(commands) do c.origin='a';assert(CM.execStopAdd(c)) end
  assert(#replayed==3 and #commands==3)
  for i,add in ipairs(replayed) do
    assert(add.player==pid and add.left==true and add.oneWay and add.autoSig)
    assert(math.abs(add.u-commands[i].x/2000)<0.0001)
  end
end
print('PASS: real AutoSig2 algorithm; spacing snapshot, direction, owner, origin-only expansion, no recursive expansion, off modes, API/state restoration, cancelled GUI callback')
print('PASS: reverse direction and generated signals through real STOPADD replay with distinct peer player IDs')
''')
