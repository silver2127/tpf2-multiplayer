"""Original action sounds: real Lua module, separate engine/GUI factories.

Mocks only the GUI audio boundary. No audible or in-game test implied.
"""
from pathlib import Path
import lupa.lua52 as lupa

ROOT = Path(__file__).resolve().parents[1]
lua = lupa.LuaRuntime(unpack_returned_tuples=True)
lua.globals().SOURCE = (ROOT / "mod/mp_lockstep_1/res/scripts/mp/action_sounds.lua").read_text(encoding="utf-8")
lua.execute(r'''
local factory=assert(load(SOURCE,"@action_sounds.lua"))()
local sounds, logs={},{}
local available, fails=true,false
api={gui={util={getGameUI=function()
  if not available then return nil end
  return {playSoundEffect=function(self,name)
    if fails then error("device unavailable") end
    sounds[#sounds+1]=name
  end}
end}}}
local function runtime(letter)
  local cm={}
  factory(cm,{INSTANCE=letter,STRICT_OPS={VBUY=true,VSELL=true,VLINE=true,VDEPOT=true,VREPL=true}},
    function(s) logs[#logs+1]=s end)
  return cm
end
local engine,gui=runtime("a"),runtime("a")
local function same(a,b)
  if type(a)~=type(b) then return false end
  if type(a)~="table" then return a==b end
  for k,v in pairs(a) do if not same(v,b[k]) then return false end end
  for k in pairs(b) do if a[k]==nil then return false end end
  return true
end
assert(same(engine.actionSoundsSave(),gui.actionSoundsSave()),
  "independent game states must produce identical ScriptSave before simulation")
print("ok  independent initial ScriptSave states are equal")
local function emit(op,seq,origin,armed)
  engine.actionSoundSuccess({op=op,seq=seq,origin=origin or "a",armed=armed or 1})
end
local function sync(muted)
  gui.actionSoundsLoad(engine.actionSoundsSave()); gui.actionSoundsGuiTick(muted)
end
sync()
emit("VBUY",1); emit("VBUY",1); sync(); sync()
assert(#sounds==1 and sounds[1]=="buyVehicle")
emit("VSELL",2,"b"); emit("VSELL",3,"a",0); emit("VNAME",4); sync()
assert(#sounds==1)
for i,op in ipairs({"VSELL","VLINE","VDEPOT","VREPL"}) do emit(op,10+i) end
sync()
assert(table.concat(sounds,",")=="buyVehicle,sellVehicle,setLine,sendToDepot,replaceVehicle")
print("ok  original sound names, local cancelled actions only, one per command")

local saved=engine.actionSoundsSave()
gui=runtime("a"); gui.actionSoundsLoad(saved); gui.actionSoundsGuiTick(false)
assert(#sounds==5)
engine=runtime("a"); engine.actionSoundsLoad(saved); sync()
assert(#sounds==5 and same(engine.actionSoundsSave(),saved))
local replica=runtime("a"); replica.actionSoundsLoad(saved)
assert(same(replica.actionSoundsSave(),engine.actionSoundsSave()))
local legacy={epoch="table: 0xOLD:12345",seq=7,events={{seq=7,name="buyVehicle"}}}
local restored=runtime("a"); restored.actionSoundsLoad(legacy)
assert(same(restored.actionSoundsSave(),legacy))
restored.actionSoundSuccess({op="VBUY",seq=9,origin="a",armed=1})
assert(restored.actionSoundsSave().epoch==legacy.epoch and restored.actionSoundsSave().seq==8)
emit("VBUY",1); sync(); assert(#sounds==6)
emit("VLINE",2); sync(true); sync(false); assert(#sounds==6)
print("ok  save/load round trip, legacy epoch, silent reload baseline and recovery")

available=false; emit("VBUY",3); sync(); assert(#sounds==6)
available=true; sync(); assert(#sounds==7)
fails=true; emit("VBUY",4); sync(); sync(); assert(#logs==1 and #sounds==7)
fails=false; emit("VBUY",5); sync(); assert(#sounds==8)
print("ok  unavailable GUI waits; audio failure is consumed and does not stop next event")

for i=100,199 do emit("VBUY",i) end
assert(#engine.actionSoundsSave().events==32)
sync(); assert(#sounds==40); sync(); assert(#sounds==40)
print("ok  bounded history and repeated GUI sync without duplicate playback")
''')

# Drive the real VLINE callbacks through their notification boundary, including
# failed first stop, successful fallback and a remote origin.
from vehicle_line_choice_test import runtime

for letter, expected in [("a", 1), ("b", 0)]:
    h = runtime(letter)
    emitted = []
    h.CM.actionSoundSuccess = lambda command: emitted.append(command.op) if command.origin == letter else None
    h.order(-1)
    h.callback()
    assert emitted == []
    h.retry()
    h.callback()
    assert len(emitted) == expected
print("ok  real VLINE replay emits only after successful fallback")

from depot_buy_test import runtime as buy_runtime

for success in [False, True]:
    h = buy_runtime()
    h.add(100, "depot/train.con", 10, 20, 101, False)
    h.buySuccess(success)
    emitted = []
    h.CM.actionSoundSuccess = lambda command: emitted.append(command.op)
    h.buy()
    assert emitted == (["VBUY"] if success else [])
print("ok  real VBUY replay emits only after successful purchase")
