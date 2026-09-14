"""Real waypoint serialization/replay plus line-edit merging, on Lua 5.2."""
from pathlib import Path
from lupa.lua52 import LuaRuntime
root=Path(__file__).resolve().parents[1]
lua=LuaRuntime(unpack_returned_tuples=True)
lua.globals().package.path=str(root/'mod/mp_lockstep_1/res/scripts/?.lua').replace('\\','/')+';'+lua.globals().package.path
lua.execute(r'''
local models={[301]={x=12,y=23,z=4,model=1},[302]={x=32,y=43,z=4,model=2},
 [901]={x=12,y=23,z=4,model=1},[902]={x=32,y=43,z=4,model=2}}
local CT={MODEL_INSTANCE_LIST=1,SIGNAL_LIST=2,LINE=3,STATION_GROUP=4}
api={type={ComponentType=CT,SignalId={new=function() return {} end},
 Line={new=function() return {stops={}} end,Stop={new=function() return {waypoints={}} end}}},
 engine={entityExists=function(id) return models[id]~=nil end,
 getComponent=function(id,k)
  local m=models[id]
  if k==CT.MODEL_INSTANCE_LIST and m then return {fatInstances={{modelId=m.model,transf={[13]=m.x,[14]=m.y,[15]=m.z}}}} end
  if k==CT.SIGNAL_LIST and m then return {signals={{},{}}} end
  if k==CT.STATION_GROUP then return {stations={}} end
 end},res={modelRep={getName=function(id) return id==1 and 'street/signal_waypoint.mdl' or 'railroad/signal_waypoint.mdl' end}}}
local CM={escName=function(s) return s end,unescName=function(s) return s end}
local K={INSTANCE='b'}
require('mp.lines')(CM,K,function() end)
CM.findStopNear=function(x,y) if x==12 then return 901 elseif x==32 then return 902 end end
local base='100.00,200.00,0,0,0,0,180'
local stops={base,base}
CM.lineCaptureWaypoints('LUPDATE ... wp=1:301:0,1:302:1,2:301:1',stops)
assert(stops[1]:find('~12.000:23.000:4.000:street/signal_waypoint.mdl:0!',1,true))
local w=CM.lineReadWaypoints(stops[1])
assert(#w==2 and w[1].entity==901 and w[1].index==0 and w[2].entity==902 and w[2].index==1)
assert(CM.lineWaypointSuffix(w)==stops[1]:match('~.*'))
local wp2=CM.lineReadWaypoints(stops[2]);assert(#wp2==1 and wp2[1].index==1)
assert(#CM.lineReadWaypoints(base)==0,'waypoint removal must clear leg')
local merged=CM.mergeLineEdit(base,'',stops[1],'',base,'')
assert(merged==stops[1],'queued line edit lost waypoint-only change')
local cleared=CM.mergeLineEdit(stops[1],'',base,'',stops[1],'')
assert(cleared==base,'queued waypoint removal lost')
assert(not pcall(CM.lineCaptureWaypoints,'LUPDATE wp=3:301:0',{base,base}))
assert(not pcall(CM.lineReadWaypoints,base..'~999:23:4:street/signal_waypoint.mdl:0'))
assert(not pcall(CM.lineReadWaypoints,base..'~12:23:4:street/signal_waypoint.mdl:9'))
''')
print('PASS waypoints: road/rail model identity, per-peer entity remap, signal index, multiple legs, queued edits/removal, missing targets rejected')
