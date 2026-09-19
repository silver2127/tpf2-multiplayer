"""Construction strings survive the real line receiver and station diff replay."""
from pathlib import Path
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[1]
scripts = root / 'mod/mp_lockstep_1/res/scripts/mp'
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().CONS = (scripts / 'cons.lua').read_text(encoding='utf-8')
net = (scripts / 'net.lua').read_text(encoding='utf-8')
lua.globals().NET = net
lua.globals().CODEC = net[net.index('local function encodeCmd(c)'):net.index('function CM.scheduleLocal(op, args)')]
lua.execute(r'''
local logs = {}
local CM = {ticks=1, queue={}, cmOriginCompany={}}
local K = {INSTANCE='a', EVENTS_FILE='fixture'}
local function log(s) logs[#logs+1]=s end
assert(load(CONS))()(CM,K,log)
assert(load(NET))()(CM,K,log)
local encode = assert(load(CODEC .. '\nreturn encodeCmd'))()
CM.gameTime=function() return 100 end
local samples = {'line1\nline2', '\r\n', '\r', '\n', '\\n', '\\\n123',
  '"quotes" \\ backslash', 'Bahnhof ä 東京', 'params=x op=CONU', ''}
for byte=0,255 do samples[#samples+1]=string.char(byte)..'0123' end
local all={}
for byte=0,255 do all[#all+1]=string.char(byte) end
samples[#samples+1]=table.concat(all)
for _,s in ipairs(samples) do
  local p = {[s]=s, modules={[7400030]={metadata={description=s}}}}
  local literal=CM.ser(p)
  assert(not literal:find('[\r\n]'), 'literal contains a record separator')
  local back=assert(CM.deserParams(literal))
  assert(back[s]==s and back.modules[7400030].metadata.description==s)
end
assert(CM.ser({name='plain',n=12})=='{["n"]=12,["name"]="plain"}')

-- A removed slot plus a changed slot with multiline metadata. Both origin and
-- peer execute the same real diff handler; only the engine boundary is mocked.
local function before()
  return {modules={[7400030]={name='remove'},[7400040]={name='keep'},
    [7400050]={metadata={description='old'}}}}
end
local after=before()
after.modules[7400030]=nil
after.modules[7400050]={metadata={description='Platform\nTrack 2\r\n\\123'}}
local diff=CM.conDiff(before(),after)
local command={op='CONU',at=101,origin='b',seq=104,strict=1,diff=1,
  file='station/rail/modular_station/modular_station_a.con',x=10,y=20,params=CM.ser(diff)}
local line=encode(command)
-- Include a duplicate and a following independent command, as in the bridge IPC.
local following={op='CONU',at=102,origin='b',seq=105,strict=1,diff=1,
  file=command.file,x=10,y=20,params=CM.ser({mdel={[7400040]=true}})}
local data=line..'\r\n'..line..'\n'..encode(following)..'\n'
CM.readFrom=function() return data,#data end
CM.pollEvents()
assert(#CM.queue==2, 'receiver lost or duplicated a command: '..table.concat(logs,'\n'))
assert(CM.queue[1].params==command.params and CM.queue[2].seq==105)
assert(CM.histIdx['b:104'].line==line, 'history changed the literal')
local results={}
for _,instance in ipairs({'b','a'}) do
  K.INSTANCE=instance
  local state=before()
  local calls=0
  api={engine={entityExists=function(id) assert(id==42); return true end}}
  game={interface={getEntity=function(id) assert(id==42); return {params=state} end,
    upgradeConstruction=function(id,file,params)
      assert(id==42 and file==command.file); state=params; calls=calls+1
    end}}
  CM.consByKey={['10.0/20.0']={id=42,file=command.file}}
  CM.execConU(instance=='b' and command or CM.queue[1])
  assert(calls==1 and state.modules[7400030]==nil)
  assert(state.modules[7400040].name=='keep')
  assert(state.modules[7400050].metadata.description==after.modules[7400050].metadata.description)
  CM.execConU(instance=='b' and following or CM.queue[2])
  assert(calls==2 and state.modules[7400040]==nil)
  results[#results+1]=CM.ser(state)
end
assert(results[1]==results[2], 'origin and receiver differ')
for _,s in ipairs(logs) do assert(not s:find('error') and not s:find('SKIPPED'),s) end
assert(CM.deserParams('{["broken"]="unfinished')==nil, 'malformed input must still be refused')
''')
print('PASS: all bytes in keys/values, line receiver, duplicate/history, station removal diff on origin and peer')
