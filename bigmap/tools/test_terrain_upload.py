"""Regression for the 131x131 CPU / 259x259 GPU crash, including stock upload code."""
import ctypes as C
import struct
from pathlib import Path
import numpy as np
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_64,UC_HOOK_CODE
from unicorn.x86_const import *

ROOT=Path(__file__).resolve().parents[1]
U=C.c_size_t
class Vector(C.Structure):_fields_=[('first',U),('last',U),('end',U)]
class Data(C.Structure):_fields_=[('data',C.POINTER(Vector)),('x',C.c_int32),('y',C.c_int32)]
FN=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(Data),C.c_void_p)

def main():
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    expand=dll.BigmapTestExpandTerrainUpload;expand.argtypes=[C.c_void_p,C.c_void_p];expand.restype=None
    invoke=dll.BigmapTestTerrainUpload;invoke.argtypes=[C.c_void_p,C.POINTER(Data),C.c_void_p,FN];invoke.restype=None
    rng=np.random.default_rng(873)
    coords=(np.arange(259)+1)/2
    lo=np.floor(coords).astype(int);f=coords-lo
    fields=[np.zeros((131,131),dtype=np.uint16),np.full((131,131),65535,dtype=np.uint16),
        np.fromfunction(lambda y,x:100*x+200*y,(131,131)).astype(np.uint16),
        rng.integers(0,65536,(131,131),dtype=np.uint16)]
    for src in fields:
        out=np.full((259,259),0x5555,dtype=np.uint16)
        expand(src.ctypes.data,out.ctypes.data)
        # Independent floating-point bilinear oracle at physical -1..257 m.
        a=src[np.ix_(lo,lo)].astype(float);b=src[np.ix_(lo,lo+1)].astype(float)
        c=src[np.ix_(lo+1,lo)].astype(float);d=src[np.ix_(lo+1,lo+1)].astype(float)
        expect=np.floor((1-f[:,None])*((1-f)*a+f*b)+f[:,None]*((1-f)*c+f*d)+.5).astype(np.uint16)
        assert np.array_equal(out,expect)
        assert np.array_equal(out[1::2,1::2],src[1:130,1:130])
    selfbuf=(C.c_uint8*40)();struct.pack_into('<iiii',selfbuf,20,257,257,1,1)
    v=Vector(src.ctypes.data,src.ctypes.data+src.nbytes,src.ctypes.data+src.nbytes)
    data=Data(C.pointer(v),0,0);seen=[]
    @FN
    def original(selfp,arg,tex):
        vv=arg.contents.data.contents;n=vv.last-vv.first
        seen.append((selfp,arg.contents.x,arg.contents.y,tex,C.string_at(vv.first,n)))
    source_before=src.tobytes()
    invoke(selfbuf,C.byref(data),0x1234,original)
    assert seen[-1][:4]==(C.addressof(selfbuf),0,0,0x1234)
    assert seen[-1][4]==expect.tobytes() and src.tobytes()==source_before
    for mode in ('offset','different_size','different_grid'):
        data.x=1 if mode=='offset' else 0
        v.last=v.first+(200 if mode=='different_size' else src.nbytes)
        struct.pack_into('<i',selfbuf,20,129 if mode=='different_grid' else 257)
        invoke(selfbuf,C.byref(data),0x1234,original)
        assert seen[-1][4]==source_before[:v.last-v.first]
    # Original TextureUploader::Upload<uint16> calculates the same requested
    # dimensions/offsets and passes our COMPLETE buffer to the render context.
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True);base=0x140000000
    u=Uc(UC_ARCH_X86,UC_MODE_64);u.mem_map(base+0x347000,0x1000)
    u.mem_write(base+0x347000,pe.get_data(0x347000,0x1000))
    mem=0x200000000;u.mem_map(mem,0x100000)
    ctx=mem+0x1000;vt=mem+0x2000;selfp=mem+0x3000;vec=mem+0x4000;arg=mem+0x4100;tex=mem+0x4200
    payload=mem+0x10000;dest=mem+0x5000;callback=mem+0x6000;stop=mem+0x7000;stack=mem+0xf0000
    def wr(p,fmt,*xs):u.mem_write(p,struct.pack(fmt,*xs))
    wr(ctx,'<Q',vt);wr(vt+0x218,'<Q',callback);wr(selfp,'<Q',ctx)
    wr(selfp+20,'<iiii',257,257,1,1)
    u.mem_write(payload,expect.tobytes());wr(vec,'<QQQ',payload,payload+expect.nbytes,payload+expect.nbytes)
    wr(arg,'<Qii',vec,0,0);wr(dest,'<i',123);wr(tex,'<Qii',dest,2,3)
    captured=[]
    def oncode(uc,a,n,data):
        if a==callback:
            sp=uc.reg_read(UC_X86_REG_RSP);ptr=struct.unpack('<Q',uc.mem_read(sp+0x28,8))[0]
            captured.append((uc.reg_read(UC_X86_REG_R9),ptr,bytes(uc.mem_read(ptr,259*259*2))))
            uc.emu_stop()
    u.hook_add(UC_HOOK_CODE,oncode)
    for mode in (0,1):
        u.mem_write(selfp+8,bytes([1,mode]));wr(stack,'<Q',stop)
        for reg,val in [(UC_X86_REG_RSP,stack),(UC_X86_REG_RCX,selfp),(UC_X86_REG_RDX,arg),(UC_X86_REG_R8,tex)]:u.reg_write(reg,val)
        u.emu_start(base+0x347090,stop,count=1000)
        assert captured[-1]==((259<<32)|259,payload,expect.tobytes())
    print('PASS: full bilinear/border oracle, 2m vertex preservation, unchanged input, pass-through guards; stock upload reads complete 259x259 buffer in both atlas modes')

if __name__=='__main__':main()
