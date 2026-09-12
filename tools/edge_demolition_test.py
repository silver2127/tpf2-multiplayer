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
''')
print("PASS: crossing demolition cleanup, inverse kind, parallel stubs, orphans, duplicates, absent/protected edges")
