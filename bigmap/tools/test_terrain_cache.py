"""Native policy/ABI guards plus stock SetTerrainEntity and AddTile in Unicorn.
No running game is modified. Requires a built plugin and Steam 35924 executable.
"""
import ctypes as C
import struct
from pathlib import Path
import capstone
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *
from test_world_entry import Host, logtype, basetype, verifytype, hooktype

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x140000000

class Terrain(C.Structure):
    _fields_ = [('x', C.c_int32), ('y', C.c_int32), ('base', C.c_int32),
        ('dx', C.c_float), ('dy', C.c_float), ('dz', C.c_float),
        ('high', C.c_int32), ('offset', C.c_float), ('river', C.c_int32)]

def main():
    pe = pefile.PE(r'C:\tools\bin\TransportFever2.exe', fast_load=True)
    dll = C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    select = dll.BigmapTestSelectTerrainCache
    select.argtypes = [C.POINTER(Terrain), C.c_int]
    def terrain(): return Terrain(114,570,6,4,4,.01,8,-100,123)
    t = terrain(); old = bytes(t)
    assert select(C.byref(t),2) and t.high == 7
    assert bytes(t)[:24] == old[:24] and bytes(t)[28:] == old[28:]
    assert not select(C.byref(t),2)
    assert select(C.byref(t),1) and bytes(t) == old
    assert not select(None,2)
    for spacing in (-1,0,3,4,999):
        t=terrain(); assert not select(C.byref(t),spacing) and bytes(t)==old
    for field,value in [('x',0),('y',-1),('x',2049),('base',7),('base',5),
                        ('dx',2),('dy',8),('dx',float('nan')),('high',6),('high',9)]:
        t=terrain();setattr(t,field,value);before=bytes(t)
        assert not select(C.byref(t),2) and bytes(t)==before

    events=[]; errors=[]; logs=[]; failure=None
    dis=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);dis.detail=True
    @logtype
    def log(fmt):logs.append(fmt)
    @basetype
    def base():return BASE
    @verifytype
    def verify(rva,p,n):
        events.append(('verify',rva));code=C.string_at(p,n)
        if code != pe.get_data(rva,n):errors.append('wrong original bytes')
        ins=list(dis.disasm(code,BASE+rva))
        if sum(i.size for i in ins)!=n:errors.append('split instruction')
        if rva in (0x33da70,0x347090,0x33cd10,0x3c4620,0x33d330,0x33d7c0):
            for i in ins:
                if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL):errors.append('branch in trampoline')
                if any(o.type==capstone.x86.X86_OP_MEM and o.mem.base==capstone.x86.X86_REG_RIP for o in i.operands):errors.append('RIP in trampoline')
        return failure!=('verify',rva)
    @hooktype
    def hook(target,detour,n,out):
        events.append(('hook',target-BASE));out[0]=0x1234
        return failure!=('hook',target-BASE)
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hook,None,None)
    install=dll.BigmapTestInstallTerrainCache
    install.argtypes=[C.POINTER(Host),C.c_int,C.c_int]
    for gog,spacing,failure in [(1,2,None),(0,0,None),(0,3,None),
        (0,2,('verify',0x347090)),(0,2,('hook',0x347090)),
        (0,2,('verify',0x33da70)),(0,2,('verify',0x112210)),
        (0,2,('hook',0x33da70)),(0,2,('hook',0x33cd10)),(0,2,('hook',0x3c4620)),(0,2,('verify',0x33cd10)),(0,2,('verify',0x3c4620)),(0,2,None),(0,1,None)]:
        events.clear();errors.clear()
        ok=install(C.byref(host),gog,spacing)
        assert not errors,errors
        assert bool(ok)==(not gog and spacing in (0,1,2) and failure is None)
        if gog or spacing not in (0,1,2):assert not events
        elif failure and failure[0]=='verify':assert all(e[0]=='verify' for e in events)
        elif spacing==1:
            assert events==[('verify',0x33da70),('verify',0x112210),('hook',0x33da70)]
        else:
            expected=[('verify',r) for r in (0x347090,0x3c4620,0x33cd10,0x33d330,0x33d7c0,0x33da70,0x112210)]
            for r in [0x347090,0x33cd10,0x3c4620,0x33d7c0,0x33d330]+([0x33da70] if spacing else []):
                expected.append(('hook',r))
                if failure==('hook',r):break
            assert events==expected,events

    gettertype=C.CFUNCTYPE(C.c_void_p,C.c_void_p,C.POINTER(C.c_int32))
    originaltype=C.CFUNCTYPE(None,C.c_void_p,C.c_int32)
    binding=dll.BigmapTestBindTerrain
    binding.argtypes=[C.POINTER(Host),C.c_int,C.c_void_p,C.c_int32,gettertype,originaltype]
    binding.restype=None
    selfbuf=(C.c_uint8*0x60)();grid=(C.c_uint8*40)();calls=[]
    @gettertype
    def getter(engine,entity):
        calls.append(('get',engine,entity[0]));return C.addressof(t)
    @originaltype
    def original(ptr,entity):calls.append(('bind',ptr,entity,t.high))
    for kind,oldhigh,spacing,want in [('new',8,2,7),('load',8,2,7),('saved2m',7,2,7),
        ('restore',7,1,8),('disabled',8,0,8),('already_bound',8,2,8),
        ('populated_grid',8,2,8),('missing_engine',8,2,8),('invalid_entity',8,2,8)]:
        t=terrain();t.high=oldhigh;calls.clear()
        C.memset(selfbuf,0,C.sizeof(selfbuf));C.memset(grid,0,C.sizeof(grid))
        struct.pack_into('<Q',selfbuf,8,0 if kind=='missing_engine' else 0x11223344)
        struct.pack_into('<i',selfbuf,0x10,12 if kind=='already_bound' else -1)
        struct.pack_into('<Q',selfbuf,0x18,C.addressof(grid))
        if kind=='populated_grid':struct.pack_into('<ii',grid,8,114,570)
        entity=-1 if kind=='invalid_entity' else 123
        binding(C.byref(host),spacing,selfbuf,entity,getter,original)
        assert t.high==want,(kind,t.high)
        assert calls[-1]==('bind',C.addressof(selfbuf),entity,want)
        if kind in ('already_bound','populated_grid','missing_engine','invalid_entity'):assert len(calls)==1
    print('PASS: native field preservation, unsupported-layout guards, 1m restoration, binding ABI, installer failure paths')

    # Execute original SetTerrainEntity, then original AddTile. External ECS,
    # allocator and vector calls are bounded test fixtures; all copied field,
    # sample-count, grid-offset and revision instructions are original game code.
    for high in (7,8):
        u=Uc(UC_ARCH_X86,UC_MODE_64)
        u.mem_map(BASE,0x4500000)
        for section in pe.sections:
            if section.Name.startswith((b'.text',b'.rdata')):
                u.mem_write(BASE+section.VirtualAddress,section.get_data())
        mem=0x200000000;u.mem_map(mem,0x2000000)
        selfp=mem+0x1000;engine=mem+0x2000;comp=mem+0x3000
        gridp=mem+0x4000;cells=mem+0x10000;vec=mem+0x300000
        stack=mem+0x1800000;stop=mem+0x1f00000;manager=mem+0x5000;cv=mem+0x6000;tile=mem+0x7000
        def wr(a,fmt,*v):u.mem_write(a,struct.pack(fmt,*v))
        def rd(a,fmt):return struct.unpack(fmt,u.mem_read(a,struct.calcsize(fmt)))
        t=terrain();t.high=high;u.mem_write(comp,bytes(t))
        wr(selfp+8,'<Q',engine);wr(selfp+0x10,'<i',-1)
        wr(selfp+0x18,'<Q',gridp);wr(selfp+0x4c,'<i',1)
        wr(engine+0x88,'<Q',manager);wr(manager+16,'<Q',cv);wr(cv+0x68,'<Q',tile)
        wr(tile,'<iii',123,-57,-285)
        sizes=[]
        def run(rva,args):
            for reg,value in zip((UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9),args):u.reg_write(reg,value)
            wr(stack,'<Q',stop);u.reg_write(UC_X86_REG_RSP,stack)
            u.emu_start(BASE+rva,stop,count=20000)
            assert u.reg_read(UC_X86_REG_RIP)==stop
        def oncode(uc,a,n,data):
            instruction=next(dis.disasm(bytes(uc.mem_read(a,n)),a))
            if instruction.mnemonic!='call':return
            target=instruction.operands[0].imm if instruction.operands[0].type==capstone.x86.X86_OP_IMM else None
            r=target-BASE if target is not None else None
            rcx=uc.reg_read(UC_X86_REG_RCX);rdx=uc.reg_read(UC_X86_REG_RDX)
            if r==0x1123d0:uc.reg_write(UC_X86_REG_RAX,comp)
            elif r==0x2bf3a80:assert rcx==40;uc.reg_write(UC_X86_REG_RAX,gridp+0x100)
            elif r==0x33c540:
                assert rdx==114*570;wr(rcx,'<QQQ',cells,cells+rdx*40,cells+rdx*40)
            elif r==0x0d0920:uc.reg_write(UC_X86_REG_RAX,0)
            elif r==0x33dd20:uc.reg_write(UC_X86_REG_RAX,vec)
            elif r==0x1d5c50:sizes.append(rdx)
            elif r not in (0x33def0,0x2bf3abc,0x700e0):raise AssertionError(('unexpected call',r,hex(a)))
            uc.reg_write(UC_X86_REG_RIP,a+n)
        u.hook_add(UC_HOOK_CODE,oncode)
        run(0x33da70,[selfp,123])
        assert bytes(u.mem_read(selfp+0x20,36))==bytes(t)
        assert rd(selfp+0x44,'<ff')==(256.,256.)
        wr(selfp+0x50,'<i',3);wr(selfp+0x58,'<i',2)
        run(0x33cb60,[selfp,456])
        assert sizes==[((1<<high)+1)**2],sizes
        assert rd(cells,'<i')==(456,) and rd(cells+32,'<I')==(1,)
    print('PASS: original game binding and tile-allocation instructions: 129x129 at level 7, 257x257 at level 8; 256 m tile extent preserved')
    print('Expected payload saving for 114x570 tiles: %.3f GiB' % (114*570*(257**2-129**2)*2/2**30))

if __name__=='__main__':main()
