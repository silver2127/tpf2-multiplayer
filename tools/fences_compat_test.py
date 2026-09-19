"""Fences capture/planning/replay isolation; optional installed mod integration.

--workshop PATH --game PATH runs the same checks with the actual mod planner.
Engine submission is mocked; this is not a multiplayer game test.
"""
import argparse
import tempfile
from pathlib import Path
from lupa.lua52 import LuaRuntime

parser = argparse.ArgumentParser()
parser.add_argument('--workshop', type=Path)
parser.add_argument('--game', type=Path)
parser.add_argument('--capture', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().MP = (root / 'mod/mp_lockstep_1/res/scripts').as_posix()
lua.globals().REAL = bool(args.workshop)
lua.globals().CAPTURE = args.capture.as_posix() if args.capture else None
if args.workshop:
    lua.globals().WORKSHOP = (args.workshop / 'res/scripts').as_posix()
    lua.globals().GAME = (args.game / 'res/scripts').as_posix()
    lua.globals().SOURCE = (args.workshop / 'res/config/game_script/snowball_fences_callback.lua').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory() as tmp:
    lua.globals().BASE = Path(tmp).as_posix() + '/'
    lua.execute(r'''
package.path=MP..'/?.lua;'..package.path
local function fail() error('world mutation escaped planner') end
local globalRandom=math.random
math.random=fail
game={interface={buildConstruction=fail,bulldoze=fail,sendScriptEvent=fail,getHeight=function(p) return p[1]*0.01+4 end}}
local CT={CONSTRUCTION=1}
local objects, submitted, commands={},{},{}
local function mat(a,b,c,d) local t={};for _,v in ipairs({a,b,c,d}) do for _,n in ipairs(v) do t[#t+1]=n end end;return t end
api={type={ComponentType=CT,Context={new=function() return {} end},
  Vec4f={new=function(...) return {...} end},Mat4f={new=mat},
  SimpleProposal={new=function() return {constructionsToAdd={},constructionsToRemove={},streetProposal={edgesToRemove={}}} end,
    ConstructionEntity={new=function() return {} end}}},
  engine={util={getPlayer=function() return 99 end},entityExists=function(id) assert(id);return objects[id]~=nil end,
    getComponent=function(id) assert(id);return objects[id] end},
  cmd={make={buildProposal=function(sp,ctx,ignore) assert(not ignore);return {sp=sp,ctx=ctx} end},
    sendCommand=function(cmd,cb) submitted[#submitted+1]={cmd=cmd,cb=cb} end}}
local script
if REAL then
  package.path=WORKSHOP..'/?.lua;'..GAME..'/?.lua;'..package.path
  _=function(s) return s end
  require('snowball/fences/config').load()
  assert(load(SOURCE))();script=data()
else
  -- A minimal independently written model of the mod's public event contract.
  local from,previewId
  script={guiHandleEvent=function(_,name,p)
    local a=p.proposal.toAdd[1]
    if a.fileName~='asset/snowball_fence_hedges.con' then return end
    game.interface.sendScriptEvent('__fencesEvent__',a.params.snowball_fences_mode==3 and 'finish' or name,
      {position={a.transf[13],a.transf[14],a.transf[15]},id=p.result[1]})
  end,handleEvent=function(_,_,name,p)
    if previewId then game.interface.bulldoze(previewId);previewId=nil end
    if name=='finish' then from=nil;return end
    if p.id then game.interface.bulldoze(p.id) end
    if from then
      local t={1,0,0,0,0,1,0,0,0,0,1,0,from[1],from[2],from[3],1}
      math.random()
      local id=game.interface.buildConstruction('asset/snowball_fences_fence_off.con',
        {result={models={{id='snowball/test.mdl',transf=t}}}}, {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1})
      if name=='builder.proposalCreate' then previewId=id end
    end
    if name=='builder.apply' then from=p.position end
  end}
end
local CM={peerSeen=true,ticks=1,cmOriginCompany={}}
local K={INSTANCE='a',BASE=BASE}
local logs={};local function log(s) logs[#logs+1]=s end
require('mp/cons')(CM,K,log)
local adapter=require('mp/fences_compat')
adapter.bind(CM,K,log)
local wrapped=adapter.wrap(script);wrapped.init()
CM.scheduleLocal=function(op,c) assert(op=='FENCE');c.op=op;c.origin='a';c.seq=#commands+1;c.at=10;commands[#commands+1]=c end
local function click(x,y,mode,typ)
  local t={1,0,0,0,0,1,0,0,0,0,1,0,x,y,10,1}
  local params={snowball_fences_mode=mode or 0,snowball_fences_type=typ or 0,snowball_fences_adjust=1,
    snowball_fences_collision=2,snowball_fences_face=0,snowball_fences_stepped=0}
  assert(CM.fencesCapture('asset/snowball_fence_hedges.con',t,CM.ser(params),0))
end
click(100,200);assert(#commands==0,'start marker became a construction')
click(110,200);assert(#commands==1 and #submitted==0,'segment must only be scheduled')
assert(math.random==fail and game.interface.buildConstruction==fail and game.interface.bulldoze==fail and game.interface.sendScriptEvent==fail)
-- Native builder notification cannot duplicate the captured click or demolish its result.
wrapped.handleEvent('snowball_fences_callback.lua','__fencesEvent__','builder.apply',{id=999,position={110,200,10}})
assert(#commands==1)
local previewParam={fence='snowball_fence_hedges',type=0,position={120,210,10},segmentType=0,adjust=1,collision=2,offsetZ=0}
for i=1,3 do wrapped.handleEvent('snowball_fences_callback.lua','__fencesEvent__','builder.proposalCreate',previewParam) end
assert(#commands==1 and #submitted==0 and math.random==fail,'preview changed world/RNG')
local p=assert(CM.fencesPreviewRead());assert(p.details and p.fence)
click(120,210,1);assert(#commands==2,'curve continuation lost')
click(125,220,2);assert(#commands==3,'arc continuation lost')
click(125,220,3);click(200,300,0,1);assert(#commands==3,'finish did not reset chain')
click(210,300,0,1);assert(#commands==4,'decorative bushes not captured')
assert(CM.fencesPreviewRead()==nil,'committed preview was retained')
assert(not CM.fencesCapture('asset/unrelated.con',{},'{}',0))
assert(not CM.fencesCapture('asset/snowball_fence_hedges.con',{},'{}',1))
-- Actual network codec transports complete geometry; peer ids stay local.
local NET=assert(io.open(MP..'/mp/net.lua')):read('*a')
local codec=NET:sub(assert(NET:find('local function encodeCmd(c)',1,true)),assert(NET:find('function CM.scheduleLocal(op, args)',1,true))-1)
local enc,dec=assert(load(codec..'\nreturn encodeCmd,decodeCmd'))()
local wire=enc(commands[1]);local command=dec(wire)
if CAPTURE then local f=assert(io.open(CAPTURE,'w'));f:write('return '..CM.ser(commands[1]));f:close() end
assert(command.op=='FENCE' and command.at==10)
for _,pid in ipairs({2002,3003}) do
  command.company=2
  CM.cmRoadPlayer=function(c,ctx) assert(c.company==2);ctx.player=pid;return true end
  CM.execFence(command)
  local s=submitted[#submitted];local ce=s.cmd.sp.constructionsToAdd[1]
  assert(ce.playerEntity==pid and s.cmd.ctx.player==pid)
  assert(s.cmd.ctx.gatherBuildings==false and s.cmd.ctx.gatherFields==false and s.cmd.ctx.cleanupStreetGraph==false)
  assert(#s.cmd.sp.constructionsToRemove==0 and #s.cmd.sp.streetProposal.edgesToRemove==0)
  assert(CM.ser(ce.params)==commands[1].params,'receiver changed fence geometry')
  local id=pid+10;objects[id]={fileName=ce.fileName,transf=ce.transf}
  s.cb({resultEntities={id}},true)
  assert(CM.consByKey[CM.conKey(ce.transf[13],ce.transf[14])].id==id)
end
-- Bounds and unsupported output reject before submission.
local n=#submitted
local bad={file=command.file,x=command.x,y=command.y,z=command.z,params='{result={models={},terrainAlignmentLists={}}}'}
assert(not pcall(CM.execFence,bad) and #submitted==n)
-- No callback can dereference a missing result entity.
submitted[#submitted].cb({},false);submitted[#submitted].cb({resultEntities={-1}},true)
-- A planning error restores every temporarily overridden API.
local old=script.handleEvent
script.handleEvent=function() math.random();error('planned failure') end
assert(not pcall(click,220,300))
assert(math.random==fail and game.interface.buildConstruction==fail and game.interface.bulldoze==fail and game.interface.sendScriptEvent==fail)
script.handleEvent=old
-- Real inject reader takes the cancelled clicks instead of parking cursor
-- constructions for generic CONP replay. Serial/offset handling is unchanged.
wrapped.handleEvent('snowball_fences_callback.lua','__fencesEvent__','finish',{})
require('mp/inject')(CM,K,log)
K.INJECT_FILE='fixture'
local captures='CONXP asset/snowball_fence_hedges.con t=1,0,0,0,0,1,0,0,0,0,1,0,100,200,10,1 ps=1 rc=0 params={snowball_fences_type=0,snowball_fences_mode=0}\n'..
  'CONXP asset/snowball_fence_hedges.con t=1,0,0,0,0,1,0,0,0,0,1,0,110,200,10,1 ps=2 rc=0 params={snowball_fences_type=0,snowball_fences_mode=0}\n'
CM.readFrom=function() local s=captures;captures=nil;return s,100 end
CM.gameTime=function() return 10 end
local before=#commands
CM.pollInject()
assert(#commands==before+1 and #CM.pendingCons==0,'real CONXP capture did not reach Fences adapter: '..table.concat(logs,'\n'))
CM.pollInject();assert(#commands==before+1,'inject replayed consumed click')
CM.actionsOff=true;click(130,200);assert(#commands==before+1);CM.actionsOff=false
CM.resyncHold=true;click(140,200);assert(#commands==before+1);CM.resyncHold=false
-- Solo events are passed straight to the original script.
CM.peerSeen=false;local called=false
script.handleEvent=function() called=true end
wrapped.handleEvent('snowball_fences_callback.lua','__fencesEvent__','finish',{})
assert(called)
math.random=globalRandom
print('PASS: '..(REAL and 'installed Fences' or 'Fences contract')..': click chain, previews, curves/arcs, finish, bushes, RNG isolation, wire codec, two player mappings, additive replay, error cleanup and solo')
''')
