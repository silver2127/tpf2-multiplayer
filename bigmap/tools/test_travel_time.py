"""The travel-time cells: stock bytes in the real executable, the rewrite, the
guards (disabled, GOG, mismatch, write failure), and the clamp."""
import ctypes as C
import struct
from pathlib import Path
import pefile
from test_world_entry import Host,logtype,basetype,verifytype,hooktype

ROOT=Path(__file__).resolve().parents[1]
BASE=0x140000000
CELLS={0x3094978:1200.0,0x309497c:6000.0}
PATCH=C.CFUNCTYPE(C.c_int,C.c_size_t,C.c_void_p,C.c_uint32)

def main():
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    for rva,val in CELLS.items():
        assert pe.get_data(rva,4)==struct.pack('<f',val),('stock cell',hex(rva))
    # Nothing else in .rdata/.data mistaken for these cells: they are the only
    # 1200.0f / 6000.0f floats the plugin touches, at fixed RVAs.
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    install=dll.BigmapTestInstallTravelTime
    install.argtypes=[C.POINTER(Host),C.c_int,C.c_int,C.c_int]
    events=[];changes={};messages=[];errors=[];fail=None
    @logtype
    def log(s):messages.append(s.decode() if isinstance(s,bytes) else s)
    @basetype
    def base():return BASE
    @verifytype
    def verify(r,p,n):
        events.append(('verify',r))
        if n!=4 or C.string_at(p,n)!=pe.get_data(r,4):errors.append('wrong expected bytes')
        return fail!=('verify',r)
    @PATCH
    def patch(r,p,n):
        events.append(('patch',r))
        if fail==('patch',r):return 0
        changes[r]=C.string_at(p,n);return 1
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hooktype(),C.cast(patch,C.c_void_p),None)
    def run(gog,limit,cargo,f=None):
        nonlocal fail
        fail=f;events.clear();changes.clear();errors.clear();messages.clear()
        ok=install(C.byref(host),gog,limit,cargo)
        assert not errors,errors
        return ok
    assert not run(1,3600,0) and not events                         # GOG: nothing
    assert not run(0,0,0) and not events                            # both stock: nothing
    assert run(0,3600,0)
    assert events==[('verify',0x3094978),('patch',0x3094978)]
    assert changes=={0x3094978:struct.pack('<f',3600.0)}
    assert run(0,0,9000)
    assert changes=={0x309497c:struct.pack('<f',9000.0)}
    assert run(0,3600,9000) and len(changes)==2
    assert run(0,10,0) and changes[0x3094978]==struct.pack('<f',60.0)      # clamp low
    assert run(0,999999,0) and changes[0x3094978]==struct.pack('<f',86400.0)   # clamp high
    assert not run(0,3600,0,('verify',0x3094978)) and not changes         # mismatch: untouched
    assert not run(0,3600,0,('patch',0x3094978)) and not changes          # write failure: reported
    assert run(0,3600,9000,('verify',0x3094978)) and list(changes)==[0x309497c]   # one cell refused, the other applied
    print('PASS: stock 1200/6000 cells; GOG, stock, mismatch and write-failure guards; both rewrites; clamp 60..86400')

if __name__=='__main__':main()
