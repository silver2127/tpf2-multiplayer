"""terrain_blocks: the stock bytes of the block-vector constructor in the real
executable, the free import slot's identity, the constructor detour's
fall-through, and the free detour's routing of arena pointers to the pager."""
import ctypes as C
from pathlib import Path
import pefile

ROOT=Path(__file__).resolve().parents[1]
N=257*257
class Vec(C.Structure):
    _fields_=[('first',C.c_void_p),('last',C.c_void_p),('end',C.c_void_p)]
Ctor=C.CFUNCTYPE(None,C.POINTER(Vec),C.c_size_t)
Free=C.CFUNCTYPE(None,C.c_void_p)
Resize=C.CFUNCTYPE(None,C.POINTER(Vec),C.c_size_t)

def main():
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe')
    assert pe.get_data(0x310230,19)==bytes.fromhex('48894c240857 4883ec30 48c7442420feffffff'.replace(' ','')),'block ctor bytes'
    # The constructor is only ever a value-initialising vector<uint16_t>(n):
    # its three qword stores at [rcx], [rcx+8], [rcx+0x10] follow the prologue.
    assert pe.get_data(0x310230+0x20,13)==bytes.fromhex('33c0 488901 48894108 48894110'.replace(' ','')),'vector header stores'
    # The return address the detour accepts is the instruction after GetBlock's call.
    assert pe.get_data(0x3c4ba2,5)==b'\xe8'+(0x310230-(0x3c4ba2+5)).to_bytes(4,'little',signed=True),'GetBlock call'
    # 0xaac4d9 follows a call to the terrain vector resize 0x1d5c50.
    assert pe.get_data(0xaac4d4,5)==b'\xe8'+(0x1d5c50-(0xaac4d4+5)).to_bytes(4,'little',signed=True),'UpdateSubterrains resize call'
    slot=None
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in entry.imports:
            if imp.address-pe.OPTIONAL_HEADER.ImageBase==0x2f0b5b8: slot=(entry.dll.decode(),imp.name.decode())
    assert slot==('api-ms-win-crt-heap-l1-1-0.dll','free'),slot
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    assert dll.BigmapTestCompressionInit()
    calls=[]
    @Ctor
    def stock_ctor(v,n):calls.append(('ctor',n));v[0]=Vec(0x1000,0x1000+n*2,0x1000+n*2)
    ctor=dll.BigmapTestBlockCtor;ctor.argtypes=[C.POINTER(Vec),C.c_size_t,Ctor]
    v=Vec()
    ctor(C.byref(v),N,stock_ctor)          # not from GetBlock: the original runs
    assert calls==[('ctor',N)] and v.first==0x1000
    # A pager slot (through the resize hook's eligible path) released via free,
    # by its data pointer and by its raw base (what the aligned delete passes).
    resize=dll.BigmapTestCompressionResize;resize.argtypes=[C.POINTER(Vec),C.c_size_t,C.c_int,Resize]
    freed=[]
    @Free
    def stock_free(p):freed.append(p)
    free=dll.BigmapTestFree;free.argtypes=[C.c_void_p,Free]
    stats=dll.BigmapTestBlockStats;stats.argtypes=[C.POINTER(C.c_uint64)]
    out=(C.c_uint64*3)()
    @Resize
    def never(v,n):raise AssertionError('stock resize used')
    for by_base in (False,True):
        w=Vec();resize(C.byref(w),N,1,never);assert w.first
        p=w.first-32 if by_base else w.first
        free(p,stock_free)
        assert not freed,'arena pointer reached the CRT'
        stats(out);assert out[1]==(2 if by_base else 1) and out[2]==0
    free(w.first,stock_free)                # already released: dropped, never passed on
    stats(out);assert out[2]==1 and not freed
    buf=C.create_string_buffer(64)
    free(C.addressof(buf),stock_free)       # a heap pointer goes to the CRT
    assert freed==[C.addressof(buf)]
    # A small-pager span: touched, then freed by data pointer and by raw base.
    assert dll.BigmapTestSmallInit()
    salloc=dll.BigmapTestSmallAllocate;salloc.argtypes=[C.c_size_t];salloc.restype=C.c_void_p
    freed.clear()
    for by_base in (False,True):
        q=salloc(8000);assert q and q%32==0
        C.memset(q,0x11,16000)              # first touch commits the span
        assert C.string_at(q-8,8)==(q-32).to_bytes(8,'little')   # aligned-alloc header
        free(q-32 if by_base else q,stock_free)
        assert not freed,'small span reached the CRT'
    stats(out);assert out[1]>=4 and out[2]==1
    print('PASS: stock ctor and call-site bytes, free import identity, ctor fall-through, arena frees routed to the pager (data and base pointer), stray drop, heap pass-through')

if __name__=='__main__':main()
