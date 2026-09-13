"""Exercise the real geometry/planning/replay modules on Lua 5.2.

The compact-node contract comes from a live makeProposalData comparison:
the same parallel-track proposal failed with seq/origin-based node ids and
passed after replacing ONLY its node placeholders with -1 .. -N. This mock
checks that contract and graph connectivity; it cannot certify engine builds.
"""
from pathlib import Path
from lupa.lua52 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1]
lua = LuaRuntime(unpack_returned_tuples=True)
for name in ("geom", "roads"):
    lua.globals()[name.upper()] = (ROOT / "mod/mp_lockstep_1/res/scripts/mp" / f"{name}.lua").read_text(encoding="utf-8")
lua.execute(r'''
local nodes, edges, streetMap, trackMap, proposals, logs
local CT = {BASE_NODE=1,BASE_EDGE=2,BASE_EDGE_STREET=3,BASE_EDGE_TRACK=4}
local function vec(x,y,z) return {x=x,y=y,z=z} end
local function edgeNew() return {comp={objects={}}} end
api={type={ComponentType=CT,Vec3f={new=vec},
  NodeAndEntity={new=function() return {comp={}} end},
  SegmentAndEntity={new=edgeNew},
  BaseEdgeTrack={new=function() return {} end},
  BaseEdgeStreet={new=function() return {} end},
  SimpleProposal={new=function() return {streetProposal={nodesToAdd={},edgesToAdd={},nodesToRemove={},edgesToRemove={}}} end},
  Context={new=function() return {} end}},
  engine={util={getPlayer=function() return 99 end},system={streetSystem={
    getNode2StreetEdgeMap=function() return streetMap end,
    getNode2TrackEdgeMap=function() return trackMap end}},
    getComponent=function(id,kind)
      if kind==CT.BASE_NODE then return nodes[id] end
      local e=edges[id]
      if not e then return nil end
      if kind==CT.BASE_EDGE then return e.comp end
      if kind==CT.BASE_EDGE_TRACK and e.type==1 then return e.trackEdge end
      if kind==CT.BASE_EDGE_STREET and e.type==0 then return e.streetEdge end
    end},
  cmd={make={buildProposal=function(sp,ctx) return {sp=sp,ctx=ctx} end},
    sendCommand=function(c) proposals[#proposals+1]=c end}}
game={}
local CM,K
local function reset()
  nodes,edges,streetMap,trackMap,proposals,logs={},{},{},{},{},{}
  local function log(s) logs[#logs+1]=s end
  CM={cmLog=log,originIdx=function(o) return string.byte(o)-string.byte('a') end,xingApiProbed=true}
  K={INSTANCE='a',ROAD_GATHER_BUILDINGS=true}
  local geom=assert(load(GEOM))()(CM,K,log)
  for k,v in pairs(geom) do CM[k]=v end
  assert(load(ROADS))()(CM,K,log)
  CM.roadAuditSnapshot=function() return {} end
  -- stops.lua's reader (not loaded here): roads.lua carries a removed edge's
  -- stops and signals onto its replacement through it
  CM.objectsOnEdge=function(eid)
    local e=edges[eid];local objs=(e and e.comp.objects) or {}
    local list={};for i,o in ipairs(objs) do list[i]={o[1],o[2] or 1} end
    return list,#objs
  end
end
local function node(id,x,y,z) nodes[id]={position=vec(x,y,z)} end
local function edge(id,n0,n1,kind)
  local p,q=nodes[n0].position,nodes[n1].position
  edges[id]={type=kind,comp={node0=n0,node1=n1,type=0,typeIndex=-1,
    tangent0=vec(q.x-p.x,q.y-p.y,q.z-p.z),tangent1=vec(q.x-p.x,q.y-p.y,q.z-p.z),objects={}},
    trackEdge={trackType=0,catenary=true},streetEdge={streetType=15,hasBus=true,tramTrackType=1}}
  local map=kind==1 and trackMap or streetMap
  for _,n in ipairs({n0,n1}) do map[n]=map[n] or {};table.insert(map[n],id) end
end
local function execute(c,keepPlan)
  c.origin=c.origin or 'b';c.seq=c.seq or 8;c.at=100;c.cat=1;c.ttype=0;c.stype=15
  local xv,xh=CM.execPolyline(c,true)
  assert(#proposals==0,'planning changed the world')
  if not keepPlan then c.xv,c.xh=xv,xh end
  CM.execPolyline(c,false)
  assert(#proposals==1,'expected one atomic build: '..table.concat(logs,'\n'))
  local sp=proposals[1].sp.streetProposal
  local ids={}
  for i,n in ipairs(sp.nodesToAdd) do
    assert(n.entity==-i,'new node placeholders must be dense and proposal-local; got '..n.entity)
    ids[n.entity]=true
  end
  local seen={}
  for _,e in ipairs(sp.edgesToAdd) do
    assert(e.comp.node0~=e.comp.node1,'self-loop')
    for _,id in ipairs({e.comp.node0,e.comp.node1}) do assert(ids[id] or nodes[id],'dangling endpoint '..id) end
    local a,b=e.comp.node0,e.comp.node1
    local key=math.min(a,b)..':'..math.max(a,b)
    assert(not seen[key],'duplicate endpoint pair');seen[key]=true
  end
  return sp,xv
end
function test_rail(origin)
  reset()
  node(101,0,-20,0);node(102,0,20,0);edge(201,101,102,0)
  local c={origin=origin,etype=1,pts='-20,0,0,0,0,0,1.32,0,0,20,0,0',links='1,2,2,3,3,4',fv='1,3,4'}
  local sp=execute(c)
  assert(#sp.nodesToAdd==4 and #sp.edgesToAdd==5 and #sp.edgesToRemove==1)
  assert(sp.edgesToRemove[1]==201)
  local roads=0
  for _,e in ipairs(sp.edgesToAdd) do if e.type==0 then
    roads=roads+1;assert(e.streetEdge.streetType==15 and e.streetEdge.hasBus and e.streetEdge.tramTrackType==1,'crossed road properties lost')
  end end
  assert(roads==2)
end
function test_road(count)
  reset()
  for i=1,count do node(100+i*2,i*5,-20,0);node(101+i*2,i*5,20,0);edge(200+i,100+i*2,101+i*2,1) end
  local pts={'-20,0,0'}
  for i=1,count do pts[#pts+1]=(i*5)..',0,0' end
  pts[#pts+1]='30,0,0'
  local links={}
  for i=1,#pts-1 do links[#links+1]=i..','..(i+1) end
  local sp,xv=execute({etype=0,pts=table.concat(pts,','),links=table.concat(links,','),fv='1,'..#pts})
  assert(#sp.edgesToRemove==count,'road failed to split all crossed tracks: '..#sp.edgesToRemove..'/'..count)
  assert(#sp.edgesToAdd==count*3+1)
  local removed={};for _,id in ipairs(sp.edgesToRemove) do removed[id]=true end
  for i=1,count do assert(removed[200+i],'wrong split parent') end
  for i=2,count+1 do assert(xv:find(i..',S,',1,true),'crossing missing from wire plan') end
end
function test_no_crossing(isTrack,bridge,planned)
  reset()
  node(101,0,-20,0);node(102,0,20,0);edge(201,101,102,isTrack and 0 or 1)
  local z=bridge and 0 or 20
  local sp=execute({etype=isTrack and 1 or 0,pts='-20,0,'..z..',0,0,'..z..',20,0,'..z,
    links='1,2,2,3',fv='1,3',bt=bridge and '1,0,1,0' or nil,
    xv=planned and '2,S,0,0,'..z..',0,-20,0,20' or nil},planned)
  assert(#sp.edgesToRemove==0,'over/under pass split the opposite network')
  assert(#sp.edgesToAdd==2)
end
function test_existing_crossing(isTrack)
  reset()
  node(101,0,-20,0);node(102,0,0,0);node(103,0,20,0)
  edge(201,101,102,isTrack and 0 or 1);edge(202,102,103,isTrack and 0 or 1)
  local sp=execute({etype=isTrack and 1 or 0,pts='-20,0,0,0,0,0,20,0,0',links='1,2,2,3',fv='1,3'})
  assert(#sp.nodesToAdd==2 and #sp.edgesToAdd==2 and #sp.edgesToRemove==0)
  for _,e in ipairs(sp.edgesToAdd) do assert(e.comp.node0==102 or e.comp.node1==102,'existing crossing node not shared') end
end
function test_fresh_parallel()
  reset()
  node(101,-20,0,0);node(102,20,0,0);edge(201,101,102,1)
  local sp=execute({etype=1,pts='-20,1,0,0,1,0,20,1,0',links='1,2,2,3',fv='1,2,3'})
  assert(#sp.nodesToAdd==3 and #sp.edgesToAdd==2 and #sp.edgesToRemove==0,'fresh parallel track snapped to existing one')
end
function test_bridge_companion(reverse,isTrack)
  reset()
  node(101,0,-20,20);node(102,0,20,20)
  edge(201,reverse and 102 or 101,reverse and 101 or 102,isTrack and 0 or 1)
  edges[201].comp.type=1;edges[201].comp.typeIndex=4;edges[201].comp.objects={{777,2}}
  local sp=execute({etype=isTrack and 1 or 0,pts='-20,0,0,20,0,0',links='1,2',fv='1,2',br='0,-20,20,0,20,20,4'})
  assert(#sp.edgesToAdd==2 and #sp.nodesToAdd==2 and #sp.edgesToRemove==1)
  assert(sp.edgesToRemove[1]==201)
  local e=sp.edgesToAdd[2]
  assert(e.type==(isTrack and 0 or 1) and e.comp.type==1 and e.comp.typeIndex==4,'bridge kind/model lost')
  assert(e.comp.node0==edges[201].comp.node0 and e.comp.node1==edges[201].comp.node1,'bridge orientation changed')
  assert(e.comp.tangent0.y==edges[201].comp.tangent0.y and e.comp.objects[1][1]==777,'bridge tangent/object lost')
  if isTrack then assert(e.streetEdge.hasBus and e.streetEdge.tramTrackType==1)
  else assert(e.trackEdge.trackType==0 and e.trackEdge.catenary) end
end
function test_invalid_companion(mode)
  reset()
  node(101,0,-20,20);node(102,0,20,20);edge(201,101,102,1)
  edges[201].comp.type=1;edges[201].comp.typeIndex=4
  local br='0,-20,20,0,20,20,4'
  if mode=='height' then br='0,-20,0,0,20,0,4'
  elseif mode=='model' then br='0,-20,20,0,20,20,5'
  elseif mode=='missing' then br='10,-20,20,10,20,20,4'
  elseif mode=='malformed' then br='0,no,20,0,20,20,4' end
  CM.execPolyline({origin='b',seq=8,etype=0,pts='-20,0,0,20,0,0',links='1,2',fv='1,2',br=br},false)
  assert(#proposals==0,'mismatched bridge produced a partial build')
end
-- A road under a ROAD bridge (or a track under a track bridge): the refreshed span is
-- the command's own network, carried as bs. It keeps its OWN properties -- as a link
-- it took the new road's type (street 24 would have become 15 here).
function test_same_network_companion(isTrack)
  reset()
  node(101,40,-20,20);node(102,40,20,20)
  edge(201,101,102,isTrack and 1 or 0)
  edges[201].comp.type=1;edges[201].comp.typeIndex=4;edges[201].comp.objects={{778,1}}
  edges[201].streetEdge.streetType=24;edges[201].trackEdge.trackType=1;edges[201].trackEdge.catenary=false
  local sp=execute({etype=isTrack and 1 or 0,pts='-20,0,0,20,0,0',links='1,2',fv='1,2',bs='40,-20,20,40,20,20,4'})
  assert(#sp.edgesToAdd==2 and #sp.nodesToAdd==2 and #sp.edgesToRemove==1,'same-network companion shape')
  assert(sp.edgesToRemove[1]==201)
  local e=sp.edgesToAdd[2]
  assert(e.type==(isTrack and 1 or 0) and e.comp.type==1 and e.comp.typeIndex==4,'same-network bridge kind/model lost')
  assert(e.comp.node0==101 and e.comp.node1==102 and e.comp.objects[1][1]==778,'same-network bridge orientation/object lost')
  if isTrack then assert(e.trackEdge.trackType==1 and e.trackEdge.catenary==false,'track bridge took the new track props')
  else assert(e.streetEdge.streetType==24,'road bridge took the new road type: '..tostring(e.streetEdge.streetType)) end
end
''')

if __name__ == "__main__":
    import sys
    cases = sys.argv[1:] or ["rail", "road", "guards", "companions"]
    if "rail" in cases:
        for origin in ("a", "b", "h"):
            lua.globals().test_rail(origin)
        print("PASS: compact rail-crossing nodes for origins a/b/h; atomic proposal and road properties")
    if "road" in cases:
        for count in (1, 2, 4):
            lua.globals().test_road(count)
        print("PASS: road crosses 1/2/4 tracks; each parent split once and present in the wire plan")
    if "guards" in cases:
        for is_track in (False, True):
            for bridge in (False, True):
                for planned in (False, True):
                    lua.globals().test_no_crossing(is_track, bridge, planned)
            lua.globals().test_existing_crossing(is_track)
        lua.globals().test_fresh_parallel()
        print("PASS: both bridge/height guards, existing crossing nodes, and fresh parallel tracks")
    if "companions" in cases:
        for reverse in (False, True):
            for is_track in (False, True):
                lua.globals().test_bridge_companion(reverse, is_track)
        for mode in ('height', 'model', 'missing', 'malformed'):
            lua.globals().test_invalid_companion(mode)
        for is_track in (False, True):
            lua.globals().test_same_network_companion(is_track)
        print("PASS: bridge replacement kinds (both networks), properties, objects, orientation and mismatch rejection")
