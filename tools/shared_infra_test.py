"""Exercise actual Lua ownership logic and assembled native relay (Unicorn)."""
from pathlib import Path
import struct
import pefile
from lupa.lua52 import LuaRuntime
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().SRC = (ROOT / 'mod/mp_lockstep_1/res/scripts/mp/shared_infra.lua').read_text()
lua.execute('''
local capability = true
io.open = function() return {read=function() return capability and "a\\npid=123\\nentity_owner_v1=1\\n" or "a\\npid=123\\n" end,close=function() end} end
local owners, calls = {[10]=1,[20]=2,[21]=2,[30]=1,[31]=1,[32]=2}, {}
local vehicles = {[30]={line=10},[31]={line=10},[32]={line=99}}
local CT = {LINE=1,TRANSPORT_VEHICLE=2,BASE_EDGE=3}
api = {type={ComponentType=CT,enum={TransportVehicleState={IN_DEPOT=0}}},engine={
 entityExists=function(id) return owners[id] ~= nil end,
 getComponent=function(id,k)
  if k==CT.LINE and id==10 then return {} end
  if k==CT.TRANSPORT_VEHICLE then return vehicles[id] end
  if k==CT.BASE_EDGE and id==21 then return {node0=100,node1=101} end
 end,
 system={transportVehicleSystem={getVehiclesWithState=function() return {31,32} end},
 streetSystem={getNode2TrackEdgeMap=function() return {[100]={21}} end,
 getNode2StreetEdgeMap=function() return {} end}}}}
game = {interface={getEntities=function() return {30,32} end,
 setPlayer=function(id,pid)
  calls[#calls+1]={id,pid}
  if pid >= 1610612736 then owners[id]=pid-1610612736
  else
   owners[id]=pid
   if id==10 then owners[20]=pid; owners[21]=pid; owners[32]=pid end
   if id==20 then owners[32]=pid end
  end
 end}}
local CM = {cmMode="companies",cmEnsure=function() end,cmNote=function() end}
CM.cmForeignOwner=function(id) return owners[id]~=1 end
CM.cmOwnerOf=function(id) return owners[id] end
assert(load(SRC))()(CM, {IDENTITY_FILE="identity"})
CM.cmCompanyPid={[1]=1001,[2]=2002}
local ctx={player=1001}
assert(CM.cmRoadPlayer({company=2},ctx) and ctx.player==2002)
assert(not pcall(CM.cmRoadPlayer,{company=3},ctx))
CM.cmSetPlayer(10,2)
assert(owners[10]==2 and owners[30]==2 and owners[31]==2)
assert(owners[20]==2 and owners[21]==2 and owners[32]==2)
CM.cmSetPlayer(10,1)
assert(owners[10]==1 and owners[30]==1 and owners[31]==1)
assert(owners[20]==2 and owners[21]==2 and owners[32]==2, "shared assets stolen")
CM.cmSetConstructionPlayer(20,1)
assert(owners[20]==1 and owners[32]==2, "visiting aircraft stolen")
owners[20]=2
assert(not CM.cmMayModify(20) and CM.cmMayModify(10))
assert(not CM.cmMayModify(999))
assert(not CM.cmMayReplaceEdges({{100,101}},true))
owners[21]=1
assert(CM.cmMayReplaceEdges({{101,100}},true)==false) -- unresolved node map
assert(CM.cmMayReplaceEdges({{100,101}},true))
capability=false
assert(load(SRC))()(CM, {IDENTITY_FILE="identity"})
local n=#calls
assert(not pcall(CM.cmSetPlayer,10,2) and #calls==n)
capability=true
assert(not pcall(CM.cmSetEntityOwner,10,-1))
assert(not pcall(CM.cmSetEntityOwner,10,268435456))
CM.cmMode="coop"
assert(not CM.cmRoadPlayer({company=2},ctx))
CM.cmSetPlayer(10,2)
assert(calls[#calls][2]==2, "coop must retain stock behavior")
''')
print('PASS Lua: separate fleets, parked vehicles, retained station/track owners, permissions, capability failure, coop')

guards = LuaRuntime(unpack_returned_tuples=True)
guards.globals().SHARE = (ROOT/'mod/mp_lockstep_1/res/scripts/mp/shared_infra.lua').read_text()
guards.globals().INJECT = (ROOT/'mod/mp_lockstep_1/res/scripts/mp/inject.lua').read_text()
guards.execute(r'''
local CT={CONSTRUCTION=1,BASE_EDGE=2}
api={type={ComponentType=CT},engine={entityExists=function(id) return id==20 or id==21 end,
 getComponent=function(id,k)
  if id==20 and k==CT.CONSTRUCTION then return {fileName="station.con",transf={[13]=100,[14]=200}} end
  if id==21 and k==CT.BASE_EDGE then return {node0=100,node1=101} end
 end,system={streetSystem={getNode2TrackEdgeMap=function() return {[100]={21}} end,
 getNode2StreetEdgeMap=function() return {} end}}}}
game={interface={getEntity=function() return {} end}}
for _, foreign in ipairs({true,false}) do
 local scheduled, logs = {}, {}
 local CM={cmMode="companies",peerSeen=true,cmEnsure=function() end,cmNote=function() end,
 cmForeignOwner=function() return foreign end,groundAt=function() return 0 end,
 deserParams=function() return {} end,
 scheduleLocal=function(op,c) scheduled[#scheduled+1]=op end}
 local wire="ARMED 1\nCONUP 20 station.con t=- params={}\nCDEMO 1 20\n"
 CM.readFrom=function() return wire,#wire end
 local K={INJECT_FILE="inject"}
 assert(load(SHARE))()(CM,K)
 assert(load(INJECT))()(CM,K,function(s) logs[#logs+1]=s end)
 CM.pollInject()
 assert(#scheduled==(foreign and 0 or 2),table.concat(logs,'\n'))
 assert(not table.concat(logs,'\n'):find('dispatch error'),table.concat(logs,'\n'))
 -- The whole replacement proposal is refused before any plan or command is made.
 if foreign then
  wire="ARMED 1\nROADE 0 1 16 1 0 1 0 1 100 101 1 0 0 1 0 0 100 101 1 0 0 1 0 0\n"
  CM.pollInject()
  assert(#scheduled==0)
  assert(table.concat(logs,'\n'):find('refused change to foreign',1,true))
 end
end
''')
print('PASS real inject.lua: foreign upgrades/demolition/replacements refused, owner construction changes scheduled')

# Check guarded instruction bytes against the real installed executable.
exe = pefile.PE(r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe', fast_load=True)
assert exe.get_data(0x11673da,18) == bytes.fromhex('90 48 85 f6 74 26 48 8d 85 80 00 00 00 48 89 44 24 20')
assert exe.get_data(0x11677a3,22) == bytes.fromhex('4c 8d 85 80 00 00 00 48 8d 95 90 00 00 00 49 8b cf e8 67 65 01 00')

# Load the actual MASM COFF object, resolving its RIP-relative data relocations.
obj = (ROOT/'native/out/setplayerrelay_mp.obj').read_bytes()
_, nsec, _, symoff, nsym, opt, _ = struct.unpack_from('<HHIIIHH',obj)
strtab = symoff + nsym*18
def symbol(idx):
    raw = obj[symoff+idx*18:symoff+idx*18+8]
    if raw[:4] == b'\0'*4:
        off = strtab+struct.unpack_from('<I',raw,4)[0]
        return obj[off:obj.index(b'\0',off)].decode()
    return raw.rstrip(b'\0').decode()
for i in range(nsec):
    off = 20+opt+i*40
    if obj[off:off+8].startswith(b'.text'):
        size, ptr, rel = struct.unpack_from('<III',obj,off+16)
        count = struct.unpack_from('<H',obj,off+32)[0]
        break
else: raise AssertionError('missing assembled .text')
code = bytearray(obj[ptr:ptr+size])
dest = {'g_setPlayerGeneric':0x300000,'g_setPlayerConstruction':0x300010,'g_setPlayerLine':0x300020}
slots = {name:0x200000+i*8 for i,name in enumerate(dest)}
for i in range(count):
    pos, idx, typ = struct.unpack_from('<IIH',obj,rel+i*10)
    assert typ==4
    addend = struct.unpack_from('<i',code,pos)[0]
    struct.pack_into('<i',code,pos,slots[symbol(idx)] + addend - (0x100000+pos+4))
for pid in (0,1,12345,0xffffffff,0x60000000,0x60000001,0x6fffffff):
    for construction in (0,0x456789):
        uc=Uc(UC_ARCH_X86,UC_MODE_64)
        for base,length in ((0x100000,0x1000),(0x200000,0x1000),(0x300000,0x1000),(0x400000,0x10000)):
            uc.mem_map(base,length)
        uc.mem_write(0x100000,bytes(code))
        for name,slot in slots.items(): uc.mem_write(slot,struct.pack('<Q',dest[name]))
        regs={UC_X86_REG_RBP:0x408000,UC_X86_REG_RSP:0x407000,UC_X86_REG_RSI:construction,
              UC_X86_REG_RBX:42,UC_X86_REG_RCX:43,UC_X86_REG_RDX:44,UC_X86_REG_R8:45,
              UC_X86_REG_R9:46,UC_X86_REG_R12:47,UC_X86_REG_R13:48,UC_X86_REG_R14:49,UC_X86_REG_R15:50}
        for reg,value in regs.items(): uc.reg_write(reg,value)
        uc.mem_write(0x408080,struct.pack('<I',pid))
        uc.hook_add(UC_HOOK_CODE,lambda u,a,s,d: u.emu_stop() if a in dest.values() else None)
        uc.emu_start(0x100000,0,count=100)
        encoded = (pid & 0xf0000000)==0x60000000
        expected = dest['g_setPlayerGeneric' if encoded else 'g_setPlayerConstruction' if construction else 'g_setPlayerLine']
        assert uc.reg_read(UC_X86_REG_RIP)==expected
        assert struct.unpack('<I',uc.mem_read(0x408080,4))[0] == (pid & 0xfffffff if encoded else pid)
        for reg,value in regs.items(): assert uc.reg_read(reg)==value
        if construction and not encoded:
            assert struct.unpack('<Q',uc.mem_read(0x407020,8))[0]==0x408080
print('PASS native: executable guards and actual assembled relay, 14 dispatch/register/stack cases')
