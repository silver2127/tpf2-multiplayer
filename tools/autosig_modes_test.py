"""Real AutoSig2 replace/remove planner and guarded replay on two mock worlds.

Run with the AutoSig2 Workshop directory as argv[1]. Engine commands are mocked;
this checks translation and target isolation, not a multiplayer game test.
"""
from pathlib import Path
import sys
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[1]
workshop = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
    "D:/SteamLibrary/steamapps/workshop/content/1066780/2138210967")
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().MP = (root / "mod/mp_lockstep_1/res/scripts").as_posix()
lua.globals().WORKSHOP = (workshop / "res/scripts").as_posix()
lua.globals().AUTOSIG = (workshop / "res/config/game_script/autosig2.lua").read_text(encoding="utf-8")
lua.execute(r'''
package.path=MP..'/?.lua;'..WORKSHOP..'/?.lua;'..package.path
_=function(s) return s end
getBuildVersion=function() return 35924 end
local function vec(x,y,z)
  return setmetatable({x or 0,y or 0,z or 0},{__index=function(t,k) return rawget(t,({x=1,y=2,z=3})[k]) end})
end
local CT={BASE_EDGE=1,BASE_EDGE_TRACK=2,BASE_NODE=3,TRANSPORT_NETWORK=4,
  MODEL_INSTANCE_LIST=5,SIGNAL_LIST=6,NAME=7,PLAYER_OWNED=8,STATION=9,BASE_EDGE_STREET=10}
local W, CM, script, commands
local function sections(eid)
  local e=W.edges[eid];assert(e,'missing edge')
  local cuts={0,2000}
  for _,o in pairs(W.objects) do if o.edge==eid then cuts[#cuts+1]=o.x-W.nodes[e.node0][1] end end
  table.sort(cuts)
  local out={}
  for i=2,#cuts do
    if cuts[i]>cuts[i-1] then out[#out+1]={start=cuts[i-1],finish=cuts[i],geometry={length=cuts[i]-cuts[i-1]}} end
  end
  return out
end
local function get(id,kind)
  assert(id and id>0,'nil/invalid entity reached engine')
  local e,o=W.edges[id],W.objects[id]
  if kind==CT.BASE_EDGE then
    if not e then return nil end
    local result={node0=e.node0,node1=e.node1,tangent0=vec(2000,0,0),tangent1=vec(2000,0,0),type=0,typeIndex=-1,objects={}}
    for oid,obj in pairs(W.objects) do if obj.edge==id then result.objects[#result.objects+1]={oid,2} end end
    table.sort(result.objects,function(a,b) return a[1]<b[1] end)
    return result
  elseif kind==CT.BASE_EDGE_TRACK then return e and {trackType=0,catenary=true}
  elseif kind==CT.BASE_NODE then return W.nodes[id] and {position=W.nodes[id]}
  elseif kind==CT.TRANSPORT_NETWORK then return {edges=sections(id)}
  elseif kind==CT.MODEL_INSTANCE_LIST and o then
    local tf={};tf[13],tf[14],tf[15]=o.x,o.left and -2 or 2,o.z or 0
    return {fatInstances={{modelId=o.model,transf=tf}}}
  elseif kind==CT.SIGNAL_LIST and o then return {signals={{type=o.kind or 0}}}
  elseif kind==CT.NAME and o then return {name=''}
  elseif kind==CT.PLAYER_OWNED and o then return {player=o.player} end
end
local make=function() error('native build escaped the planner') end
local send=function() error('native command escaped the planner') end
api={type={ComponentType=CT,EdgeId={new=function(e,s) return {entity=e,section=s} end},
  SimpleProposal={new=function() return {streetProposal={edgesToAdd={},edgesToRemove={},edgeObjectsToAdd={},edgeObjectsToRemove={}}} end},
  SegmentAndEntity={new=function() return {comp={tangent0=vec(),tangent1=vec()},trackEdge={}} end},
  SimpleStreetProposal={EdgeObject={new=function() return {} end}}},
  res={modelRep={getName=function(id) return id end}},
  engine={getComponent=get,util={getPlayer=function() return W.player end},system={
    streetSystem={getNode2TrackEdgeMap=function()
      local m={};for id,e in pairs(W.edges) do
        m[e.node0]=m[e.node0] or {};m[e.node1]=m[e.node1] or {}
        table.insert(m[e.node0],id);table.insert(m[e.node1],id)
      end;return m
    end},streetConnectorSystem={getNode2StreetConnectorMap=function() return W.frozen end},
    signalSystem={getSignal=function(key,reverse)
      local sec=sections(key.entity)[key.section+1]
      local x=W.nodes[W.edges[key.entity].node0][1]+(reverse and sec.start or sec.finish)
      for id,o in pairs(W.objects) do
        if o.edge==key.entity and o.x==x and o.left==not reverse then return {entity=id} end
      end
      return {entity=-1}
    end}}},cmd={make={buildProposal=make},sendCommand=send}}
game={}
local compat=require('mp/autosig_compat')
local geom
local function reset(offset,player,letter)
  W={edges={},nodes={},objects={},frozen={},player=player,next=offset+100}
  for i,x in ipairs({-2000,0,2000,4000}) do W.nodes[offset+i]=vec(x,0,0) end
  for i=1,3 do W.edges[offset+10+i]={node0=offset+i,node1=offset+i+1} end
  for i,def in ipairs({{-500,false,1},{-1000,true,1},{500,false,2},{1000,false,2},
      {1500,false,2},{1700,true,2},{2500,false,3},{2700,true,3}}) do
    W.objects[offset+20+i]={x=def[1],left=def[2],edge=offset+10+def[3],model='old.mdl',player=player}
  end
  W.objects[offset+24].model='new.mdl' -- newly placed seed
  assert(load(AUTOSIG))();script=compat.wrap(data())
  script.load({distance=500,use=true,replace=false,remove=false,backward=false})
  CM={escName=function(s) return s end,unescName=function(s) return s end,
    cmMode='companies',cmCompanyPid={[2]=player},cmEnsure=function() end,
    cmOwnerOf=function(id) return W.objects[id] and W.objects[id].player end,
    linesUsingStation=function() return {} end}
  geom=require('mp/geom')(CM,{},function() end)
  CM.edgeGeomT,CM.hermitePos,CM.hermiteTangent=geom.edgeGeomT,geom.hermitePos,geom.hermiteTangent
  CM.uOnEdge=function(eid,x) return (x-W.nodes[W.edges[eid].node0][1])/2000 end
  CM.uOnEdgeFine=CM.uOnEdge
  CM.findEdgeByEnds=function(track,ax,ay,bx,by)
    assert(track)
    for id,e in pairs(W.edges) do
      local a,b=W.nodes[e.node0],W.nodes[e.node1]
      if a.x==ax and b.x==bx and ay==0 and by==0 then return id end
    end
  end
  CM.findEdgeContaining=function() error('strict target used a geometry fallback') end
  require('mp/stops')(CM,{INSTANCE=letter,STOP_EDGE_EPS=12},function() end)
  CM.objectsOnEdge=function(eid) local o=get(eid,CT.BASE_EDGE).objects;return o,#o end
  CM.findStopNear=function() error('strict target used nearest-stop fallback') end
  CM.stopSettleOwner=function() end
  W.calls={}
  CM.nativeStopProposal=function(add,remove,why,done)
    assert(remove and W.objects[remove.eo],'removal target missing')
    local old=W.objects[remove.eo]
    assert(old.player==W.player and (old.kind or 0)~=2,'wrong owner or waypoint removed')
    W.calls[#W.calls+1]={old=old,add=add}
    W.objects[remove.eo]=nil
    if add then
      assert(add.eid==remove.eid and add.player==W.player and add.autoSig)
      W.next=W.next+1
      W.objects[W.next]={x=W.nodes[W.edges[add.eid].node0][1]+add.u*2000,left=add.left,
        edge=add.eid,model=add.model,player=add.player,kind=add.oneWay and 1 or 0}
    end
    done(true,{})
    return true
  end
  commands={};CM.scheduleLocal=function(op,c) commands[#commands+1]={op=op,c=c} end
end
local function plan(mode,backward)
  script.load({distance=400,use=true,replace=mode=='replace',remove=mode=='remove',backward=backward})
  local seed={track=1,kind=2,origin='a',company=2,model='new.mdl',oneWay=1}
  CM.autoSigCapture(seed)
  assert(seed.autosigMode==mode and seed.autosigBackward==(backward and 1 or 0))
  -- Change every control while the seed is in flight: the captured action wins.
  script.load({distance=900,use=false,replace=false,remove=false,backward=not backward})
  local old={};for _,obj in ipairs(get(1012,CT.BASE_EDGE).objects) do if obj[1]~=1024 then old[#old+1]=obj[1] end end
  CM.autoSigAfterSeed(seed,{1002,1003},old,false)
  assert(script.save().distance==900 and script.save().use==false and script.save().backward==not backward)
  assert(api.engine.getComponent==get and api.cmd.make.buildProposal==make and api.cmd.sendCommand==send)
  return commands
end
local function signature()
  local out={};for _,o in pairs(W.objects) do out[#out+1]=string.format('%g:%s:%s:%d',o.x,tostring(o.left),o.model,o.kind or 0) end
  table.sort(out);return table.concat(out,'|')
end
for _,mode in ipairs({'replace','remove'}) do
  for _,backward in ipairs({false,true}) do
    reset(1000,2002,'a')
    local before=signature();local batch=plan(mode,backward)
    assert(signature()==before,'planner changed local world')
    assert(#batch==(backward and 4 or 3),'wrong route extent')
    local replacements=0
    for _,entry in ipairs(batch) do
      assert(entry.c.autosigTarget==1 and entry.c.company==2 and entry.c.autosig==nil)
      assert(entry.op=='STOPDEL' or entry.op=='STOPREP')
      if entry.op=='STOPREP' then replacements=replacements+1 end
      for key,value in pairs(entry.c) do
        assert(type(value)=='number' or type(value)=='string','non-scalar wire field')
        assert(key~='entity' and key~='player' and key~='eid','local ID on wire')
      end
    end
    assert(replacements==(mode=='replace' and #batch-1 or 0),'seed must be removed, not replaced')
    local reference
    for _,peer in ipairs({{1000,2002,'a'},{5000,3003,'b'}}) do
      reset(table.unpack(peer))
      for _,entry in ipairs(batch) do
        entry.c.origin='a'
        local ok=entry.op=='STOPDEL' and CM.execStopDel(entry.c) or CM.execStopAdd(entry.c)
        assert(ok,'replay refused planned signal')
      end
      assert(#W.calls==#batch,'wrong action count')
      local now=signature()
      if reference then assert(now==reference,'peers diverged') else reference=now end
      -- Exact targets gone/changed: duplicate commands must not delete neighbours.
      local count=#W.calls
      for _,entry in ipairs(batch) do
        if entry.op=='STOPDEL' then assert(not CM.execStopDel(entry.c))
        else assert(not CM.execStopAdd(entry.c)) end
      end
      assert(#W.calls==count and signature()==now,'duplicate changed world')
    end
    print('PASS: '..mode..' '..(backward and 'backward' or 'forward')..'; real planner, two peer ID/owner maps, exact replay and duplicates')
  end
end
-- A route ending at a branch or frozen station does not walk into it.
for _,boundary in ipairs({'branch','frozen'}) do
  reset(1000,2002,'a')
  if boundary=='frozen' then W.frozen[1004]=true
  else W.nodes[1005]=vec(2000,2000,0);W.edges[1014]={node0=1003,node1=1005} end
  assert(#plan('remove',false)==2,'crossed '..boundary)
end
print('PASS: branch and frozen-station route boundaries')
-- A refused target aborts the entire plan before scheduling any command.
for _,reason in ipairs({'foreign','waypoint'}) do
  reset(1000,2002,'a')
  if reason=='foreign' then W.objects[1027].player=9999 else W.objects[1027].kind=2 end
  assert(not pcall(plan,'remove',false))
  assert(#commands==0 and #W.calls==0,'partial batch escaped validation')
  assert(api.engine.getComponent==get and api.cmd.make.buildProposal==make and api.cmd.sendCommand==send)
end
print('PASS: foreign-company and waypoint targets refuse the whole plan')
reset(1000,2002,'a');local batch=plan('replace',false)
local replacement
for _,entry in ipairs(batch) do if entry.op=='STOPREP' and entry.c.rx==1500 then replacement=entry.c end end
assert(replacement)
for _,reason in ipairs({'missing','model','owner','waypoint','height','ambiguous','edge'}) do
  reset(5000,3003,'b')
  if reason=='missing' then W.objects[5025]=nil
  elseif reason=='model' then W.objects[5025].model='different.mdl'
  elseif reason=='owner' then W.objects[5025].player=9999
  elseif reason=='waypoint' then W.objects[5025].kind=2
  elseif reason=='height' then W.objects[5025].z=10
  elseif reason=='edge' then W.nodes[5003]=vec(2001,0,0)
  else local o={};for k,v in pairs(W.objects[5025]) do o[k]=v end;W.objects[5099]=o end
  local before=signature()
  assert(not CM.execStopAdd(replacement),'unsafe replacement accepted: '..reason)
  assert(not CM.execStopDel(replacement),'unsafe removal accepted: '..reason)
  assert(#W.calls==0 and signature()==before,'unsafe world change: '..reason)
end
print('PASS: missing, changed model/owner/type/height, ambiguous and changed-edge targets never fall back')
''')
