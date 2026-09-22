"""Verify targeted save patches, failure/rollback behavior and original operands."""
import ctypes as C
import struct
from pathlib import Path
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_64
from unicorn.x86_const import UC_X86_REG_RAX,UC_X86_REG_R8
from test_world_entry import Host,logtype,basetype,verifytype,hooktype

ROOT=Path(__file__).resolve().parents[1]
BASE=0x140000000
RVAS=(0x2e8641,0x2e87ad)
PATCH=C.CFUNCTYPE(C.c_int,C.c_size_t,C.c_void_p,C.c_uint32)

def main():
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    stock={r:pe.get_data(r,6) for r in RVAS}
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    install=dll.BigmapTestInstallSaveFast
    install.argtypes=[C.POINTER(Host),C.c_int,C.c_int]
    events=[];changes={};fail=None;messages=[];errors=[]
    @logtype
    def log(s):messages.append(s)
    @basetype
    def base():return BASE
    @verifytype
    def verify(r,p,n):
        events.append(('verify',r))
        if n!=6 or C.string_at(p,n)!=stock[r]:errors.append('wrong expected bytes')
        return fail!=('verify',r)
    @PATCH
    def patch(r,p,n):
        events.append(('patch',r));data=C.string_at(p,n)
        if fail==('patch',r) and data!=stock[r]:return 0
        changes[r]=data;return 1
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,
              hooktype(),
              C.cast(patch,C.c_void_p),None)
    for gog,enabled,fail in [(1,1,None),(0,0,None)]+[(0,1,(kind,r)) for kind in ('verify','patch') for r in RVAS]+[(0,1,None)]:
        events.clear();changes.clear();errors.clear()
        ok=install(C.byref(host),gog,enabled)
        assert not errors,errors
        assert bool(ok)==(not gog and enabled and fail is None)
        if gog or not enabled:assert not events
        elif fail:
            assert all(b==stock[r] for r,b in changes.items()),'partial patch not restored'
        else:assert events==[('verify',r) for r in RVAS]+[('patch',r) for r in RVAS]
    # Run actual stock/replacement instructions, resolving the stock level's
    # RIP-relative constant. Neither patch changes instruction block length.
    for patched in (False,True):
        u=Uc(UC_ARCH_X86,UC_MODE_64);u.mem_map(BASE,0x4500000)
        u.mem_write(BASE+0x392767c,pe.get_data(0x392767c,4))
        for r in RVAS:u.mem_write(BASE+r,changes[r] if patched else stock[r])
        u.emu_start(BASE+RVAS[0],BASE+RVAS[0]+6)
        assert u.reg_read(UC_X86_REG_RAX)==(1 if patched else 3)
        u.emu_start(BASE+RVAS[1],BASE+RVAS[1]+6)
        assert u.reg_read(UC_X86_REG_R8)==(65536 if patched else 128)
    print('PASS: exact stock bytes; disabled/build/mismatch guards; write-failure rollback; original and patched level/buffer operands')

if __name__=='__main__':main()
