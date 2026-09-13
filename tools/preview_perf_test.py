"""Operation-count comparison and lifecycle checks for cosmetic previews.

Runs the real Lua module with deterministic clocks/files and a native ACK stub.
Optional --baseline points at a saved previews.lua, never at an installed file.
Counts are workload measurements, not game FPS or native renderer timings.
"""
import argparse
import json
from pathlib import Path

from lupa.lua52 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1]
SCENARIO = r'''
local T, files, failPath, failClose = 100, {}, nil, nil
local counts = {}
local function bump(k) counts[k] = (counts[k] or 0)+1 end
os = {time=function() return math.floor(T) end, clock=function() return T end}
io = {open=function(path, mode)
  if mode == 'r' then
    bump(path:find('_ready',1,true) and 'readyReads' or 'reads')
    if not files[path] then return nil end
    return {read=function(_, n) return files[path]:sub(1,n) end, close=function() end}
  end
  if failPath == path then failPath=nil; return nil end
  bump(path:find('_native_',1,true) and 'nativeWrites' or 'snapshotWrites')
  local value=''
  return {write=function(self, s) value=value..s; return self end, close=function()
    if failClose==path then failClose=nil; return nil,'simulated flush failure' end
    files[path]=value; return true
  end}
end}
local draws, nativeLive, modes, session = {}, {}, {}, '12345'
local function rep()
  return {find=function() bump('resourceFinds'); return 1 end,
    getName=function() bump('resourceNames'); return 'standard/town_small_new.lua' end}
end
api = {type={}, res={streetTypeRep=rep()}, cmd={}}
api.type.SimpleProposal={new=function() return {streetProposal={nodesToAdd={},edgesToAdd={}}} end}
api.type.NodeAndEntity={new=function() return {comp={}} end}
api.type.SegmentAndEntity={new=function() return {comp={}} end}
api.type.Vec3f={new=function(x,y,z) return {x=x,y=y,z=z} end}
api.type.BaseEdgeStreet={new=function() return {} end}
api.cmd.sendCommand=function() error('Cosmetic preview submitted a game command') end
api.cmd.make={buildProposal=function(sp)
  bump('conversions')
  local request=assert(files['B/tpf2mp_preview_native_request.txt'])
  local sid, nonce, origin, mode=request:match('^(%d+) (%d+) (%a+) (%a+)')
  assert(sid == session)
  local ok=true
  if mode=='clear' then nativeLive[origin]=nil
  elseif mode=='keep' then ok=nativeLive[origin] ~= nil
  else nativeLive[origin]=sp; bump('draws') end
  modes[origin]=mode
  files['B/tpf2mp_preview_native_ack.txt']=sid..' '..nonce..(ok and ' ok' or ' error')..'\nend\n'
end}
game={interface={setZone=function(key, zone) draws[key]=zone end}}
local pending={}
local function side(letter, base)
  local C,K={}, {INSTANCE=letter,BASE=base}
  assert(load(SOURCE,'@previews.lua'))()(C,K,function() end)
  C.cursorColor=function() return 1,0,0 end
  C.previewControlsVisible=function() return true end
  C.broadcast=function(line) bump('packets'); pending[#pending+1]=line end
  for _,name in ipairs({'previewEncode','previewDecode'}) do
    local fn=C[name]
    C[name]=function(...) bump(name); return fn(...) end
  end
  return C,K
end
local gui, gk=side('a','A/')
local engine=side('a','A/')
local receiver=side('b','B/')
local view=side('b','B/')
local function frame(dt)
  T=T+(dt or 0.25)
  gui.previewGuiTick(); engine.previewTick()
  for _,line in ipairs(pending) do receiver.previewRecv(line) end
  pending={}
  receiver.previewTick(); view.previewGuiTick()
end
local function proposal(x, invalid)
  return {kind='road',invalid=invalid,curves={{x,0,x+100,0,100,0,100,0}},
    details={{12,22,0,0,terrain=0,file='standard/town_small_new.lua',bus=0,tram=0,cat=0}}}
end
files['B/tpf2mp_preview_native_ready.txt']=session..'\nend\n'
frame()
gui.previewLocal=proposal(0,false); gui.previewEventAt=T
frame()
assert(nativeLive.a and modes.a=='drawok')
counts={}
for i=1,40 do frame(); assert(nativeLive.a, 'held preview expired') end
local held=counts
counts={}
for i=1,20 do
  gui.previewLocal=proposal(i,false); gui.previewEventAt=T
  frame()
  assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==i, 'changed geometry delayed')
end
local moving=counts
counts={}
gui.previewLocal=proposal(20,true); frame(); assert(modes.a=='drawbad','status-only change lost')
gui.previewLocal=proposal(20,false); frame(); assert(modes.a=='drawok')

-- Resource resolution on a long route must preserve every segment.
local many=proposal(0,false)
for i=2,24 do
  local p=proposal(i*100,false)
  many.curves[i],many.details[i]=p.curves[1],p.details[1]
end
counts={}
assert(#view.previewNativeProposal(many).streetProposal.edgesToAdd==24)
local resources=counts
counts={}

-- Failed writes must be retried on the next poll, including same-body writes.
gui.previewLocal=proposal(70,false)
failPath='A/tpf2mp_preview_out_a.txt'
frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==20)
frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==70)
gui.previewLocal=proposal(80,false)
failPath='B/tpf2mp_preview_in_b.txt'
frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==70)
frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==80)
if VERIFY then
  gui.previewLocal=proposal(90,false)
  failClose='A/tpf2mp_preview_out_a.txt'
  frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==80)
  frame(); assert(nativeLive.a.streetProposal.nodesToAdd[1].comp.position.x==90,'failed flush was cached')
end
gui.previewLocal=nil; frame(); assert(not nativeLive.a,'cancel delayed')

-- Same geometry must redraw after cancellation and native service restart.
gui.previewLocal=proposal(80,false); frame(); assert(nativeLive.a)
session='54321'; nativeLive={}
files['B/tpf2mp_preview_native_ready.txt']=session..'\nend\n'
frame(); assert(nativeLive.a,'new native session retained stale drawn cache')
nativeLive={} -- native renderer expired while Lua retained its signature
frame(1.1); assert(nativeLive.a,'rejected keep did not rebuild geometry')

-- Malformed/replayed traffic cannot extend a valid peer heartbeat.
local old=receiver.previewPeers.a
local age=old.at
T=T+0.25
receiver.previewRecv('LSPREVIEW o=a g='..old.generation..' s='..(old.seq+1)..' road broken')
assert(receiver.previewPeers.a.at==age)
receiver.previewRecv('LSPREVIEW o=a g='..old.generation..' s='..old.seq..' '..old.body)
assert(receiver.previewPeers.a.at==age)

-- The GUI must still expire even when cached content is identical.
T=T+6; receiver.previewTick(); view.previewGuiTick(); assert(not nativeLive.a,'peer expiry lost')
frame(); assert(nativeLive.a,'same-body preview did not recover')
T=T+6; view.previewGuiTick(); assert(not nativeLive.a,'engine expiry lost')
frame(); assert(nativeLive.a)
T=T+6; engine.previewTick()
for _,line in ipairs(pending) do receiver.previewRecv(line) end
pending={}; receiver.previewTick(); view.previewGuiTick()
assert(not nativeLive.a,'GUI expiry lost')

-- Multiple origins share one native ready read per GUI poll.
T=T+1
local body=gui.previewEncode(proposal(0,false))
files['B/tpf2mp_preview_in_b.txt']=math.floor(T)..'\na 1 0 0 '..body..'\nc 0 0 1 '..body..'\nend\n'
counts={}; view.previewGuiTick()
assert(nativeLive.a and nativeLive.c)
local peers=counts
counts={}

-- Empty/partial snapshots remove render state; a complete retry restores it.
T=T+0.25; files['B/tpf2mp_preview_in_b.txt']='partial'
view.previewGuiTick(); assert(not nativeLive.a and not nativeLive.c)
T=T+0.25
files['B/tpf2mp_preview_in_b.txt']=math.floor(T)..'\na 1 0 0 '..body..'\nend\n'
view.previewGuiTick(); assert(nativeLive.a)
-- An instance change must publish off even if its old preview was cached.
gk.INSTANCE='d'; frame()
assert(files['A/tpf2mp_preview_out_d.txt']:find('\noff\n',1,true))
return {held=held,moving=moving,resources=resources,peers=peers}
'''


def run(path, verify=False):
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.globals().SCRIPT_PATH = (Path(__file__).resolve().parents[1] / 'mod/mp_lockstep_1/res/scripts/?.lua').as_posix()
    lua.execute("package.path = SCRIPT_PATH .. ';' .. package.path")
    lua.globals().SOURCE = path.read_text(encoding='utf-8')
    lua.globals().VERIFY = verify
    result = lua.execute(SCENARIO)
    return {name: dict(values.items()) for name, values in result.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    current = run(ROOT / 'mod/mp_lockstep_1/res/scripts/mp/previews.lua', verify=True)
    assert current['held'].get('previewEncode', 0) == 0
    assert current['held'].get('previewDecode', 0) == 0
    assert current['held']['snapshotWrites'] <= 40
    assert current['held'].get('draws', 0) == 0
    assert current['moving']['draws'] == 20
    assert current['resources']['resourceFinds'] == 1
    assert current['peers']['readyReads'] == 1
    output = {'current': current}
    if args.baseline:
        baseline = run(args.baseline)
        output['baseline'] = baseline
        assert current['held']['snapshotWrites'] < baseline['held']['snapshotWrites']
        assert current['moving']['draws'] == baseline['moving']['draws']
        assert current['held']['packets'] == baseline['held']['packets']
    print(json.dumps(output, indent=2, sort_keys=True))
    print('Preview lifecycle and operation-count checks passed (no game timing claim).')


if __name__ == '__main__':
    main()
