"""Real inject.lua: unchanged bridge spans are separate replacement records.

Fixture: green road-under-rail-bridge capture, 2026-09-13, simplified only in
the surrounding world. No game engine is mocked as accepting a build here.

'removed' is the same capture as the slice ships it since the road-under-bridge
fix: the span the engine replaced in place carries its removal. That span is still
an unchanged companion, and its removal must not also travel in rm (the replay
matches removals in the command's own network, where a rail span never matches).
'removed_changed' keeps a real replacement -- changed geometry -- explicit.
'same_kind' is a road under a ROAD bridge: the span travels as bs and keeps its own
properties. An upgrade-shaped capture (every edge replaces a removal) keeps its own
network's span explicit (runUpgradeShape).
"""
from pathlib import Path
from lupa.lua52 import LuaRuntime

SOURCE = (Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/mp/inject.lua').read_text(encoding='utf-8')
L = LuaRuntime(unpack_returned_tuples=True)
L.globals().SOURCE = SOURCE
L.execute(r'''
function runCase(mode)
  local primaryTrack=mode=='inverse'
  local CT={BASE_NODE=1,BASE_EDGE=2,BASE_EDGE_TRACK=3,BASE_EDGE_STREET=4}
  local p0,p1={-48.4622,-474.1883,22.1595},{-38.1558,-392.8513,21.8020}
  local t={x=10.3065,y=81.3370,z=-0.3575}
  local be={node0=33254,node1=27347,type=1,typeIndex=4,tangent0=t,tangent1=t}
  if mode=='changed' or mode=='removed_changed' then be.tangent0={x=11,y=81.3370,z=-0.3575} end
  if mode=='different_model' then be.typeIndex=3 end
  local foreignMap={[33254]={123},[27347]={123}}
  if mode=='missing' then foreignMap={} end
  local function map(track)
    if mode=='same_kind' then return track==primaryTrack and foreignMap or {} end
    return track~=primaryTrack and foreignMap or {}
  end
  api={type={ComponentType=CT},engine={system={streetSystem={
    getNode2TrackEdgeMap=function() return map(true) end,
    getNode2StreetEdgeMap=function() return map(false) end}},
    getComponent=function(id,kind)
      if kind==CT.BASE_EDGE and id==123 then return be end
      if kind==CT.BASE_NODE then
        local p=id==33254 and p0 or (id==27347 and p1)
        if p then return {position={x=p[1],y=p[2],z=p[3]}} end
      end
      if id==123 and ((kind==CT.BASE_EDGE_TRACK and not primaryTrack) or (kind==CT.BASE_EDGE_STREET and primaryTrack)) then return {} end
    end}}
  game={}
  local removed=mode=='removed' or mode=='removed_changed'
  local e='33254 27347 10.3065 81.3370 -0.3575 10.3065 81.3370 -0.3575'
  if mode=='reverse' then e='27347 33254 -10.3065 -81.3370 0.3575 -10.3065 -81.3370 0.3575' end
  local wire='ARMED 1\nROADE 2 '..(primaryTrack and '1 -1 0 1' or '0 25 1 0')..' 2 0 '..(removed and '1' or '0')
    ..' -1 6.5205 -446.5256 13.8915 -2 -85.9093 -442.9310 6.8005'
    ..' -1 -2 -92.4298 3.5946 -7.0910 -92.4298 3.5946 -7.0910 '..e
    ..(removed and (' '..e) or '')..' 0 -1 1 4 OWNERS -1 3921\n'
  local CM={peerSeen=true,injectOffset=0,seqNo=0,ticks=0}
  CM.cmMode='companies';CM.cmEnsure=function() end
  CM.cmCompanyOfPid=function(pid) return pid==3921 and 2 or nil end
  assert(load(GEOM))()(CM,{},function() end)
  local K={INSTANCE='a',INJECT_FILE='mock'}
  local scheduled,planned,logs={},{},{}
  CM.gameTime=function() return 100 end
  CM.readFrom=function(p,off) return off==0 and wire or nil,1 end
  CM.scheduleLocal=function(op,args) scheduled[#scheduled+1]=args end
  CM.geomScopeBegin=function() end;CM.geomScopeEnd=function() end
  CM.netMap=map
  CM.findEdgeContaining=function() return nil end
  CM.execPolyline=function(c) planned[#planned+1]=c end
  assert(load(SOURCE))()(CM,K,function(s) logs[#logs+1]=s end)
  CM.pollInject()
  assert(#scheduled==1,'expected one command: '..table.concat(logs,'\n'))
  local c=scheduled[1]
  local drop=mode=='normal' or mode=='reverse' or mode=='inverse' or mode=='removed' or mode=='same_kind'
  local key=(mode=='same_kind') and 'bs' or 'br'
  local other=(key=='br') and 'bs' or 'br'
  assert(c.links==(drop and '1,2' or '1,2,3,4'),mode..': wrong edges: '..c.links)
  assert(c.bt==(drop and '0,-1' or '0,-1,1,4'),mode..': bridge type tail misaligned')
  assert(c.own==(drop and '0' or '0,2'),mode..': ownership tail misaligned')
  assert(c.fv=='1,2',mode..': fresh-node hints changed')
  assert(planned[1].links==c.links and planned[1].bt==c.bt,'plan differs from wire')
  assert(planned[1].br==c.br and planned[1].bs==c.bs,'bridge companion missing from planning pass')
  if drop then
    assert(c[key] and c[key]:find('22.1595',1,true) and c[key]:match(',4$'),mode..': bridge positions/model missing from '..key)
    assert(not c[other],mode..': companion shipped under '..other)
  else assert(not c.br and not c.bs,'changed or explicit replacement incorrectly moved to companions') end
  local n=0;for _ in c.tans:gmatch('[^,]+') do n=n+1 end
  assert(n==(drop and 6 or 12),'tangents misaligned')
  if mode=='removed' then
    assert(not c.rm,'the companion span was also shipped as a removal: '..tostring(c.rm))
    assert(table.concat(logs,'\n'):find('1 in-place removal(s) from the slice travel with those replacements',1,true),'in-place removal not reported')
  end
  if mode=='removed_changed' then assert(c.rm,'explicit replacement removal was lost') end
end

-- The upgrade tool: every edge replaces a removal between the same two existing nodes.
-- Its own network's span is the upgrade itself and stays an explicit edge + removal.
function runUpgradeShape(owner)
  local CT={BASE_NODE=1,BASE_EDGE=2,BASE_EDGE_TRACK=3,BASE_EDGE_STREET=4}
  local p0,p1={-48.4622,-474.1883,22.1595},{-38.1558,-392.8513,21.8020}
  local t={x=10.3065,y=81.3370,z=-0.3575}
  local be={node0=33254,node1=27347,type=1,typeIndex=4,tangent0=t,tangent1=t}
  local streetMap={[33254]={123},[27347]={123}}
  api={type={ComponentType=CT},engine={getComponent=function(id,kind)
    if kind==CT.BASE_EDGE and id==123 then return be end
    if kind==CT.BASE_NODE then
      local p=id==33254 and p0 or (id==27347 and p1)
      if p then return {position={x=p[1],y=p[2],z=p[3]}} end
    end
  end}}
  game={}
  local e='33254 27347 10.3065 81.3370 -0.3575 10.3065 81.3370 -0.3575'
  local wire='ARMED 1\nROADE 0 0 25 1 0 1 0 1 '..e..' '..e..' 1 4\n'
  if owner then wire=wire:sub(1,-2)..' OWNERS '..owner..'\n' end
  local CM={peerSeen=true,injectOffset=0,seqNo=0,ticks=0}
  CM.cmMode='companies';CM.cmCompanyPid={[2]=3921};CM.cmEnsure=function() end
  CM.cmCompanyOfPid=function(pid) return pid==3921 and 2 or nil end
  assert(load(GEOM))()(CM,{},function() end)
  local K={INSTANCE='a',INJECT_FILE='mock'}
  local scheduled,logs={},{}
  CM.gameTime=function() return 100 end
  CM.readFrom=function(p,off) return off==0 and wire or nil,1 end
  CM.scheduleLocal=function(op,args) scheduled[#scheduled+1]=args end
  CM.geomScopeBegin=function() end;CM.geomScopeEnd=function() end
  CM.netMap=function(track) return track and {} or streetMap end
  CM.findEdgeContaining=function() return nil end
  CM.execPolyline=function() end
  assert(load(SOURCE))()(CM,K,function(s) logs[#logs+1]=s end)
  CM.pollInject()
  assert(#scheduled==1,'upgrade: expected one command: '..table.concat(logs,'\n'))
  local c=scheduled[1]
  assert(c.links=='1,2' and c.bt=='1,4','upgrade: the span must stay an explicit edge: '..tostring(c.links))
  assert(c.rm,'upgrade: its removal must still ship')
  assert(not c.bs and not c.br,'upgrade: moved to companions')
  assert(c.own==(owner and (owner=='-1' and '0' or '2') or nil),'ownership missing or raw player ID sent')
  return c
end
''')

if __name__ == '__main__':
    L.globals().GEOM = (Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/mp/geom.lua').read_text(encoding='utf-8')
    for case in ('normal', 'reverse', 'inverse', 'same_kind', 'removed', 'removed_changed', 'changed', 'different_model', 'missing'):
        L.globals().runCase(case)
        print('PASS:', case)
    L.globals().runUpgradeShape()
    print('PASS: an upgrade-shaped capture keeps its own span explicit')
    net = (Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/mp/net.lua').read_text(encoding='utf-8')
    codec = net[net.index('local function encodeCmd(c)'):net.index('function CM.scheduleLocal(op, args)')]
    encode, decode = L.execute(codec + '\nreturn encodeCmd, decodeCmd')
    for owner in ('3921', '-1'):
        command = L.globals().runUpgradeShape(owner)
        command.op, command.at, command.origin, command.seq = 'ROADP', 100, 'b', 28
        assert decode(encode(command)).own == (2 if owner == '3921' else 0)
    print('PASS: ownership upgrade and public reversal survive capture and real wire codec without player entity IDs')
    bridges = '-48.4622,-474.1883,22.1595,-38.1558,-392.8513,21.8020,4;0,0,20,30,0,20,2'
    command = L.table_from(dict(op='ROADP', at=100, origin='b', seq=27, br=bridges, bs=bridges, links='1,2', etype=0))
    decoded = decode(encode(command))
    assert decoded.br == bridges and decoded.bs == bridges
    print('PASS: positional bridge records (br and bs) round-trip through the real wire codec')
