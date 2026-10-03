"""Compare fast material selection with ORIGINAL executable code, natively.

Copies the self-contained stock function into this test process, relocating
only its two RIP-relative constants. No game process/files are modified.
"""
import ctypes as C
import random
import struct
import time
from pathlib import Path
import capstone
import numpy as np
import pefile

ROOT=Path(__file__).resolve().parents[1]
U=C.c_size_t
ARGS=[C.c_uint64]*4+[C.POINTER(U),C.POINTER(C.c_uint8),C.POINTER(C.c_int32),C.POINTER(U),C.POINTER(U)]
FN=C.CFUNCTYPE(None,*ARGS)

def main():
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    start,end=0x315f20,0x3163ba
    raw=bytearray(pe.get_data(start,end-start))
    assert raw[:16].hex()=='48894c24085556574154415541564157'
    dis=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);dis.detail=True
    instructions=list(dis.disasm(raw,0x140000000+start))
    assert instructions[-1].mnemonic=='ret'
    assert not any(i.group(capstone.CS_GRP_CALL) for i in instructions)
    k=C.WinDLL('kernel32',use_last_error=True)
    k.VirtualAlloc.argtypes=[C.c_void_p,U,C.c_uint32,C.c_uint32];k.VirtualAlloc.restype=C.c_void_p
    k.VirtualFree.argtypes=[C.c_void_p,U,C.c_uint32]
    block=k.VirtualAlloc(None,0x10000,0x3000,0x40)
    assert block
    constants={0x2f20a14:(0x2000,4),0x2f87d20:(0x3000,63*63*4)}
    relocated=[]
    for ins in instructions:
        for op in ins.operands:
            if op.type==capstone.x86.X86_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP:
                target=ins.address+ins.size+op.mem.disp-0x140000000
                off,size=constants[target]
                pos=ins.address-(0x140000000+start)
                struct.pack_into('<i',raw,pos+ins.disp_offset,off-pos-ins.size)
                relocated.append(target)
    assert sorted(relocated)==sorted(constants)
    for rva,(off,size) in constants.items(): C.memmove(block+off,pe.get_data(rva,size),size)
    C.memmove(block,bytes(raw),len(raw))
    stock=FN(block)
    dither=C.cast(block+0x3000,C.POINTER(C.c_float))
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    fast=dll.BigmapTestMaterialIndex
    fast.argtypes=[FN,C.POINTER(C.c_float)]+ARGS;fast.restype=None
    rng=random.Random(35924)
    nrng=np.random.default_rng(35924)
    stride=132
    heightmaps=[nrng.uniform(-.05,1.05,(stride*stride)).astype(np.float32) for _ in range(40)]
    vectors=[(U*3)(a.ctypes.data,a.ctypes.data+a.nbytes,a.ctypes.data+a.nbytes) for a in heightmaps]
    entries=(C.c_uint8*(24*40))()
    for i,v in enumerate(vectors):
        struct.pack_into('<Q',entries,i*24,C.addressof(v))
        entries[i*24+12]=[0,1,0xe9,0xff,17,93][i%6]
    layers=(C.c_uint8*48)()
    struct.pack_into('<Q',layers,0,C.addressof(entries))
    struct.pack_into('<i',layers,36,stride)
    overlayData=(C.c_uint8*65536)()
    mask=(C.c_uint32*2048)()
    overlay=(U*6)(C.addressof(overlayData),C.addressof(overlayData)+65536,0,C.addressof(mask),0,0)
    baseData=(C.c_uint8*65536)()
    base=(U*3)(C.addressof(baseData),C.addressof(baseData)+65536,0)
    out1=(C.c_uint8*65536)();out2=(C.c_uint8*65536)()
    output1=(U*3)(C.addressof(out1),C.addressof(out1)+65536,0)
    output2=(U*3)(C.addressof(out2),C.addressof(out2)+65536,0)
    cell=(C.c_int32*2)(0,0)
    def pair(x,y): return (x&0xffffffff)|((y&0xffffffff)<<32)
    def run(n,w,h,jx,jy,kind):
        struct.pack_into('<i',layers,40,n)
        layers[24]=rng.choice([0,1,7,0xe9,0xff])
        for data in (baseData,overlayData):
            if kind=='empty': C.memset(C.addressof(data),0,65536)
            else:
                a=nrng.choice(np.array([0,0,0,0,1,0xe9,0xff,19],dtype=np.uint8),65536)
                C.memmove(C.addressof(data),a.ctypes.data,65536)
        a=nrng.integers(0,2**32,2048,dtype=np.uint32)
        C.memmove(C.addressof(mask),a.ctypes.data,a.nbytes)
        overlay[1]=overlay[0] if kind=='no_overlay' else overlay[0]+65536
        C.memset(C.addressof(out1),0x42,65536);C.memset(C.addressof(out2),0x42,65536)
        cell[0],cell[1]=rng.randrange(2),rng.randrange(2)
        shift=-2000 if cases%2 else 0
        args=[pair(w,h),pair(1200+shift,1800+shift),pair(jx,jy),pair(31+shift,11+shift),overlay,layers,cell,base]
        stock(*args,output1)
        fast(stock,dither,*args,output2)
        if bytes(out1)!=bytes(out2):
            mismatch=[i for i,(a,b) in enumerate(zip(out1,out2)) if a!=b]
            raise AssertionError((n,w,h,jx,jy,kind,mismatch[:8],[(out1[i],out2[i]) for i in mismatch[:8]]))
        return args
    cases=0
    for n in (-1,0,1,7,8,9,15,16,17,24,32,33,40):
        for kind in ('empty','mixed','no_overlay'):
            for w,h,jx,jy in [(8,8,0,0),(16,8,7,21),(32,32,7,7),(13,7,5,9)]:
                run(n,w,h,jx,jy,kind);cases+=1
    print(f'PASS: {cases} native original-code comparisons; complete output buffers identical')
    # Layer misses exercise every batch and final fallback, not only top-layer hits.
    for a in heightmaps: a.fill(-1.0)
    for n in (1,8,9,16,17,33,40): run(n,16,16,4,9,'empty')
    print('PASS: all-layer misses, overlapping batch boundaries, fallback and sentinel material IDs')
    fallback_calls=[]
    @FN
    def fallback(*a): fallback_calls.append(a[:4])
    args=[pair(0,8),pair(1,1),pair(0,0),pair(0,0),overlay,layers,cell,base,output2]
    fast(fallback,dither,*args)
    assert fallback_calls==[tuple(args[:4])]
    args[0]=pair(8,8)
    alias=(U*3)(base[0],base[1],base[2])
    args[-1]=alias
    fast(fallback,dither,*args)
    assert len(fallback_calls)==2
    print('PASS: unsupported geometry and overlapping input/output buffers fall back to original')
    # Warm-cache native microbenchmarks; not an end-to-end game speedup.
    for label,fill in [('all-miss',-1.0),('top-hit',2.0)]:
        for a in heightmaps: a.fill(fill)
        for i in range(40): entries[i*24+12]=1+i
        args=run(40,64,64,1,1,'empty')
        timings=[]
        for which in range(2):
            t=time.perf_counter()
            for _ in range(100):
                if which==0: stock(*args,output1)
                else: fast(stock,dither,*args,output2)
            timings.append(time.perf_counter()-t)
        print(f'{label}: stock={timings[0]:.4f}s fast={timings[1]:.4f}s ratio={timings[0]/timings[1]:.2f}x')
    from test_world_entry import Host,logtype,basetype,verifytype,hooktype
    install=dll.BigmapTestInstallMaterialIndex
    install.argtypes=[C.POINTER(Host),C.c_int,C.c_int]
    for failure in ('none','disabled','gog','verify','hook'):
        events,errors=[],[]
        @logtype
        def log(fmt): pass
        @basetype
        def getbase(): return 0x140000000
        @verifytype
        def verify(rva,ptr,size):
            events.append('verify')
            if rva!=start or size!=16 or C.string_at(ptr,size)!=pe.get_data(start,16): errors.append('verify')
            return failure!='verify'
        @hooktype
        def hook(target,detour,steal,out):
            events.append('hook')
            if target!=0x140000000+start or steal!=16 or not detour: errors.append('hook')
            out[0]=block
            return failure!='hook'
        host=Host(C.sizeof(Host),1,log,None,None,None,getbase,None,verify,hook,None,None)
        assert bool(install(C.byref(host),failure=='gog',failure!='disabled'))==(failure=='none')
        assert not errors
        assert events==([] if failure in ('disabled','gog') else ['verify'] if failure=='verify' else ['verify','hook'])
    print('PASS: installer byte verification and refusal paths')
    k.VirtualFree(block,0,0x8000)

if __name__=='__main__': main()
