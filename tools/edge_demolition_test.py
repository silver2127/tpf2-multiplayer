"""Real EDEMO replay on Lua 5.2; topology and native cleanup contract.

Live makeProposalData checks rejected both remaining rail stubs at a level
crossing. Changing ONLY Context.cleanupStreetGraph to true accepted both.
This test verifies the replay contract, not the engine's internal cleanup.
"""
from pathlib import Path
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[1] / "mod/mp_lockstep_1/res/scripts/mp"
lua = LuaRuntime(unpack_returned_tuples=True)
for name in ("geom", "cons"):
    lua.globals()[name.upper()] = (root / f"{name}.lua").read_text(encoding="utf-8")
lua.execute(r'''
local CT={BASE_NODE=1,BASE_EDGE=2,BASE_EDGE_TRACK=3,BASE_EDGE_STREET=4}
local nodes,edges,maps,sent,logs,CM
api={type={ComponentType=CT,
 Context={new=function() return {cleanupStreetGraph=false} end},
 SimpleProposal={new=function() return {streetProposal={
  nodesToRemove={},edgesToRemove={},nodesToAdd={},edgesToAdd={}}} end}},
 engine={system={streetSystem={
  getNode2StreetEdgeMap=function() return maps[0] end,
  getNode2TrackEdgeMap=function() return maps[1] end}},
  getComponent=function(id,kind)
   if kind==CT.BASE_NODE then return nodes[id] end
   local e=edges[id];if not e then return nil end
   if kind==CT.BASE_EDGE then return e end
   if (kind==CT.BASE_EDGE_TRACK and e.kind==1) or
      (kind==CT.BASE_EDGE_STREET and e.kind==0) then return {} end
  end},
 cmd={make={buildProposal=function(sp,ctx,ignoreErrors)
  return {sp=sp,ctx=ctx,ignoreErrors=ignoreErrors}
 end},sendCommand=function(cmd) sent[#sent+1]=cmd end}}
local function reset()
 nodes,edges,maps,sent,logs={},{},{[0]={},[1]={}},{},{}
 CM={};local K={EDEMO_TOL_SQ=1.0}
 local function log(s) logs[#logs+1]=s end
 assert(load(GEOM))()(CM,K,log)
 assert(load(CONS))()(CM,K,log)
end
local function node(id,x,y)
 nodes[id]={position={x=x,y=y,z=0}}
end
local function edge(id,a,b,kind,objects)
 edges[id]={node0=a,node1=b,kind=kind,objects=objects or {}}
 for _,n in ipairs({a,b}) do
  maps[kind][n]=maps[kind][n] or {};table.insert(maps[kind][n],id)
 end
end
local function run(params)
 CM.execEdgeDemolish({params=params,seq=46,origin='b',at=100})
end
local function check(expectedEdges,expectedNodes)
 assert(#sent==1,'one atomic proposal expected: '..table.concat(logs,'\n'))
 local c=sent[1];local sp=c.sp.streetProposal
 assert(c.ctx and c.ctx.cleanupStreetGraph==true,'crossing demolition must enable native graph cleanup')
 assert(#sp.edgesToAdd==0 and #sp.nodesToAdd==0,'do not reconstruct unrelated geometry')
 local function same(actual,expected)
  assert(#actual==#expected,'unexpected removal count')
  local seen={};for _,id in ipairs(actual) do assert(not seen[id]);seen[id]=true end
  for _,id in ipairs(expected) do assert(seen[id],'wrong entity removed') end
 end
 same(sp.edgesToRemove,expectedEdges);same(sp.nodesToRemove,expectedNodes)
end
-- Rail stub at a road crossing; inverse street stub at a rail crossing.
-- The shared node belongs to BOTH maps and must survive the removal.
for kind=0,1 do
 reset();node(1,-10,0);node(2,0,0);node(3,0,-10);node(4,0,10)
 edge(11,1,2,kind);edge(12,3,2,1-kind);edge(13,2,4,1-kind)
 run('-10,0,0,0,0,0,'..kind);check({11},{1})
end
-- Two parallel stubs and the connecting road: shared crossing nodes survive.
reset()
node(1,-10,0);node(2,0,0);node(3,-10,5);node(4,0,5)
node(5,0,-10);node(6,0,15)
edge(11,1,2,1);edge(12,3,4,1)
edge(21,5,2,0);edge(22,2,4,0);edge(23,4,6,0)
run('-10,0,0,0,0,0,1;-10,5,0,0,5,0,1');check({11,12},{1,3})
-- Ordinary isolated segment removes both orphan nodes; duplicate records dedup.
reset();node(1,0,0);node(2,10,0);edge(11,1,2,1)
run('0,0,0,10,0,0,1;0,0,0,10,0,0,1');check({11},{1,2})
-- Already absent: no dispatch. Signals/stops: retain the fatal-assert guard.
reset();run('0,0,0,10,0,0,1');assert(#sent==0)
reset();node(1,0,0);node(2,10,0);edge(11,1,2,1,{{999,0.5}})
run('0,0,0,10,0,0,1');assert(#sent==0)

-- A dragged bulldoze = many EDEMOs on one tick. The node index is built once
-- and reused while nothing changes the network, and rebuilt after a removal.
local fetches=0
local gs,gt=api.engine.system.streetSystem.getNode2StreetEdgeMap,api.engine.system.streetSystem.getNode2TrackEdgeMap
api.engine.system.streetSystem.getNode2StreetEdgeMap=function() fetches=fetches+1;return gs() end
-- the engine applies a sent removal before the next command: mirror that
local function applying(cmd)
 sent[#sent+1]=cmd
 local sp=cmd.sp.streetProposal
 for _,e in ipairs(sp.edgesToRemove) do
  local r=edges[e];edges[e]=nil
  for _,n in ipairs({r.node0,r.node1}) do
   local l=maps[r.kind][n] or {}
   for i=#l,1,-1 do if l[i]==e then table.remove(l,i) end end
  end
 end
 for _,n in ipairs(sp.nodesToRemove) do nodes[n]=nil;maps[0][n]=nil;maps[1][n]=nil end
end
local plainSend=api.cmd.sendCommand
api.cmd.sendCommand=applying
reset();CM.ticks=7
node(1,0,0);node(2,10,0);node(3,20,0);edge(11,1,2,0);edge(12,2,3,0)
for i=1,5 do run('100,100,0,110,100,0,0') end            -- nothing there: no send
assert(fetches==1,'five no-op EDEMOs on one tick must share one index, fetched '..fetches)
run('0,0,0,10,0,0,0');assert(#sent==1 and fetches==1)   -- removes 11 + orphan 1
run('10,0,0,20,0,0,0');assert(fetches==2,'a removal must drop the index')
-- stale data would still list edge 11 at node 2 and keep it; the fresh index frees it
local sp=sent[2].sp.streetProposal
assert(#sp.edgesToRemove==1 and sp.edgesToRemove[1]==12)
local freed={};for _,n in ipairs(sp.nodesToRemove) do freed[n]=true end
assert(freed[2] and freed[3] and #sp.nodesToRemove==2,'node 2 lost its last edge: must go with it')
CM.ticks=8;run('100,100,0,110,100,0,0');assert(fetches==3,'a new tick must rebuild')
api.engine.system.streetSystem.getNode2StreetEdgeMap=gs

-- The cell index answers exactly what the full scan did: dense random nodes
-- (many within the 1 m radius of each other, equal distances, negative
-- coordinates across cell edges), compared against the old nested loop.
api.cmd.sendCommand=plainSend
local function brute(want)
 local best,bestD={},{}
 for kind=0,1 do
  for nid in pairs(maps[kind]) do
   local p=nodes[nid].position
   for i,w in ipairs(want) do
    if w[4]==kind then
     local dx,dy=p.x-w[1],p.y-w[2];local d=dx*dx+dy*dy
     if d<=1.0 and (not bestD[i] or d<bestD[i] or (d==bestD[i] and nid<best[i])) then best[i],bestD[i]=nid,d end
    end
   end
  end
 end
 return best
end
math.randomseed(20260924)
for trial=1,40 do
 reset();CM.ticks=trial
 local ids={}
 for id=1,120 do
  -- quarter-metre lattice: exact ties and exact 1 m distances happen
  node(id,math.random(-24,24)*0.25-8,math.random(-24,24)*0.25+4);ids[#ids+1]=id
 end
 for e=1,90 do
  local a,b=ids[math.random(#ids)],ids[math.random(#ids)]
  if a~=b then edge(1000+e,a,b,math.random(0,1)) end
 end
 for k=1,6 do
  local recs,want={},{}
  for r=1,math.random(1,5) do
   local x0,y0=math.random(-26,26)*0.25-8,math.random(-26,26)*0.25+4
   local x1,y1=math.random(-26,26)*0.25-8,math.random(-26,26)*0.25+4
   local kind=math.random(0,1)
   recs[#recs+1]=string.format('%g,%g,0,%g,%g,0,%d',x0,y0,x1,y1,kind)
   want[#want+1]={x0,y0,0,kind};want[#want+1]={x1,y1,0,kind}
  end
  local expect=brute(want)
  local got=CM.edemoMatchNodes(want)   -- the index is reused across k: nothing is sent here
  for i=1,#want do
   assert(got[i]==expect[i],string.format('trial %d want %d: cell index %s, full scan %s',trial,i,tostring(got[i]),tostring(expect[i])))
  end
 end
 assert(CM.edemoCache.reused==5,'six lookups on one tick share one index')
end
''')
print("PASS: crossing demolition cleanup, inverse kind, parallel stubs, orphans, duplicates, absent/protected edges, "
      "one node index per bulldoze burst (rebuilt after a removal and on a new tick), cell index == full scan")
