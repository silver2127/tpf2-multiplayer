"""Native dimensions/labels and original combo loop in Unicorn. No game writes."""
import ctypes as C
import math
import struct
from pathlib import Path
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_64,UC_HOOK_CODE
from unicorn.x86_const import *
from test_world_entry import Host,logtype,basetype,verifytype,hooktype

ROOT=Path(__file__).resolve().parents[1];BASE=0x140000000
class String(C.Structure):
    _fields_=[('buf',C.c_char*16),('size',C.c_uint64),('cap',C.c_uint64)]
Original=C.CFUNCTYPE(C.c_uint64,C.c_int,C.c_int,C.c_void_p)
Patch=C.CFUNCTYPE(C.c_int,C.c_size_t,C.c_void_p,C.c_uint32)

def main():
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    label=dll.BigmapTestRatioText;label.argtypes=[C.POINTER(String),C.c_int];label.restype=C.c_void_p
    for f in list(range(20))+[-1,20,2**31-1]:
        s=String();assert label(C.byref(s),f)==C.addressof(s)
        want=f'1:{f+1 if 0<=f<20 else 1}'.encode()
        assert s.buf==want and s.size==len(want) and s.cap==15
    derive=dll.BigmapTestDeriveShape;derive.argtypes=[C.c_int]*3+[C.POINTER(C.c_int)]*2
    checked=0
    for cap in (40,184,224,256,512,1024,2048):
        for side in (2,24,44,96,128,256,512,1024,2048):
            for f in range(20):
                x=C.c_int();y=C.c_int();derive(side,f,cap,C.byref(x),C.byref(y))
                assert 2<=x.value<=cap and 2<=y.value<=cap
                assert x.value%2==y.value%2==0 and y.value==x.value*(f+1)
                assert (x.value*64+1)*(y.value*64+1)<=2147483647
                checked+=1
    for f in (-1,20,2**31-1):
        x=C.c_int();y=C.c_int();derive(256,f,16,C.byref(x),C.byref(y))
        assert 2<=x.value<=16 and 2<=y.value<=16
    calls=[]
    @Original
    def original(size,f,cfg):
        calls.append((size,f));assert 0<=size<7 and 0<=f<5
        side=[24,32,44,56,64,80,96][size]
        return side|(side<<32)
    sizefn=dll.BigmapTestRatioSize;sizefn.argtypes=[C.c_int,C.c_int,C.c_void_p,Original,C.c_int];sizefn.restype=C.c_uint64
    cfg=C.create_string_buffer(0x40)
    for sz in range(7):
        for f in range(20):
            calls.clear();v=sizefn(sz,f,cfg,original,2048);x=v&0xffffffff;y=v>>32
            assert calls==[(sz,f if f<5 else 0)]
            if f>=5:assert y==x*(f+1)
            else:assert x==y
    struct.pack_into('<ii',cfg,0x28,10,20)
    assert sizefn(6,19,cfg,original,2048)==96|(96<<32)
    assert sizefn(-1,-1,None,original,2048)==24|(24<<32)

    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    events=[];errors=[];writes={};fail=None
    @logtype
    def log(fmt):pass
    @basetype
    def base():return BASE
    @verifytype
    def verify(rva,p,n):
        events.append(('verify',rva));code=C.string_at(p,n)
        if pe.get_data(rva,n)!=code:errors.append(hex(rva))
        return fail!=('verify',rva)
    @Patch
    def patch(rva,p,n):
        events.append(('patch',rva))
        if fail==('patch',rva):return 0
        writes[rva]=C.string_at(p,n);return 1
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hooktype(),C.cast(patch,C.c_void_p),None)
    install=dll.BigmapTestInstallRatios;install.argtypes=[C.POINTER(Host)]+[C.c_int]*4
    for args in [(1,20,2048,1),(0,5,2048,1),(0,20,2048,0),(0,20,10,1)]:
        events.clear();assert not install(C.byref(host),*args);assert not events
    for failure in [('verify',0x880940),('verify',0x662ac9),('verify',0x662a25),
                    ('patch',0x880940),('patch',0x662a27),('patch',0x662acb),None]:
        fail=failure;events.clear();writes.clear()
        assert bool(install(C.byref(host),0,20,2048,1))==(fail is None)
        if fail:assert 0x662acb not in writes
    assert not errors,errors
    assert writes[0x662acb]==bytes([20]) and writes[0x662a27]==bytes([19])
    assert writes[0x880940][:6]==b'\xff\x25\0\0\0\0'

    # Execute the game's actual loop with the two installed immediate bytes.
    u=Uc(UC_ARCH_X86,UC_MODE_64);u.mem_map(BASE,0x4500000)
    for sec in pe.sections:
        if sec.Name.startswith((b'.text',b'.rdata')):u.mem_write(BASE+sec.VirtualAddress,sec.get_data())
    for rva in (0x662a27,0x662acb):u.mem_write(BASE+rva,writes[rva])
    mem=0x200000000;u.mem_map(mem,0x10000)
    u.reg_write(UC_X86_REG_RSP,mem+0x8000);u.reg_write(UC_X86_REG_RBP,mem+0x9000)
    u.reg_write(UC_X86_REG_RSI,mem+0x1000);u.reg_write(UC_X86_REG_RBX,0)
    items=[];enabled=[];selected=[]
    def ret(value=0):
        rsp=u.reg_read(UC_X86_REG_RSP);back=struct.unpack('<Q',u.mem_read(rsp,8))[0]
        u.reg_write(UC_X86_REG_RAX,value);u.reg_write(UC_X86_REG_RSP,rsp+8);u.reg_write(UC_X86_REG_RIP,back)
    def hook(uc,address,size,user):
        rcx=uc.reg_read(UC_X86_REG_RCX);rdx=uc.reg_read(UC_X86_REG_RDX)
        rva=address-BASE
        if rva==0x880940:
            s=String();label(C.byref(s),rdx);uc.mem_write(rcx,bytes(s));ret(rcx)
        elif rva==0x98270:uc.mem_write(rcx,bytes(uc.mem_read(rdx,32)));ret(rcx)
        elif rva==0x226d210:items.append(bytes(uc.mem_read(rdx+8,16)).split(b'\0')[0]);ret()
        elif rva==0x226df60:enabled.append((rdx,uc.reg_read(UC_X86_REG_R8)&255));ret()
        elif rva==0x226df90:selected.append(rdx);ret()
        elif rva==0x662adf:uc.emu_stop()
        elif rva in (0x2a49c0,0x2bf3abc):raise AssertionError('unexpected experimental gate / heap allocation')
    u.hook_add(UC_HOOK_CODE,hook);u.emu_start(BASE+0x662a1e,BASE+0x662adf,count=20000)
    assert items==[f'1:{k}'.encode() for k in range(1,21)]
    assert enabled==[(i,1) for i in range(20)] and selected==[0]
    print(f'PASS: {checked} bounded exact-ratio shapes, stock forwarding, override preservation, SSO labels, installer failures, original 20-row combo loop')

if __name__=='__main__':main()
