"""Native forwarding ABI and byte-verified world-entry hook installer tests.
No game process is modified. Build the DLL first.
"""
import ctypes as C
from pathlib import Path
import pefile
import capstone

ROOT=Path(__file__).resolve().parents[1]
BASE=0x140000000
RVAS=[0x230440,0x3bcb90,0x3bc9e0,0x3b8bd0,0x3b8660,0x157390,0x937590]
COUNTS=[2,10,3,6,6,8,10]
ORDER=RVAS[:5]+[RVAS[6],RVAS[5]]
logtype=C.CFUNCTYPE(None,C.c_char_p)
basetype=C.CFUNCTYPE(C.c_size_t)
verifytype=C.CFUNCTYPE(C.c_int,C.c_size_t,C.c_void_p,C.c_uint32)
hooktype=C.CFUNCTYPE(C.c_int,C.c_size_t,C.c_void_p,C.c_int,C.POINTER(C.c_void_p))
class Host(C.Structure):
    _fields_=[('size',C.c_uint32),('abi',C.c_uint32),('log',logtype),
        ('cfgInt',C.c_void_p),('cfgBool',C.c_void_p),('cfgStr',C.c_void_p),
        ('base',basetype),('buildOk',C.c_void_p),('verify',verifytype),
        ('hook',hooktype),('patch',C.c_void_p),('dataDir',C.c_void_p)]

def main():
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    busy=dll.BigmapTestWorldBusy
    busy.restype=C.c_int
    activity=dll.BigmapTestInstallWorldActivity
    activity.argtypes=[C.POINTER(Host),C.c_int]
    install=dll.BigmapTestInstallWorldEntry
    install.argtypes=[C.POINTER(Host),C.c_int,C.c_int]
    call=dll.BigmapTestInvokeWorldEntry
    call.argtypes=[C.POINTER(Host),C.c_int,C.POINTER(C.c_size_t)]
    call.restype=None
    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    dis=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    dis.detail=True
    events,errors,logs,callbacks,received=[],[],[],[],[]
    args=(C.c_size_t*10)(*[0x1234567800000000+i for i in range(10)])
    @logtype
    def log(fmt): logs.append(fmt.decode())
    @basetype
    def base(): return BASE
    @verifytype
    def verify(rva,ptr,size):
        events.append(('verify',rva))
        code=C.string_at(ptr,size)
        if code!=pe.get_data(rva,size): errors.append('byte mismatch')
        instructions=list(dis.disasm(code,BASE+rva))
        if sum(x.size for x in instructions)!=size: errors.append('split instruction')
        for ins in instructions:
            if ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP): errors.append('relative control flow')
            if any(op.type==capstone.x86.X86_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP for op in ins.operands): errors.append('RIP relative')
        return failure!=('verify',rva)
    @hooktype
    def hook(target,detour,steal,output):
        rva=target-BASE
        events.append(('hook',rva))
        i=RVAS.index(rva)
        types=[C.c_size_t]*COUNTS[i]
        if i in (3,4): types[4]=C.c_uint32
        if i in (5,6): types[3]=types[4]=C.c_uint32
        if i==6: types[9]=C.c_uint8
        @C.CFUNCTYPE(None,*types)
        def original(*values):
            received.append((i,values))
            if i==5:
                if busy()!=1: errors.append('outer activity scope missing')
                # Nested phases see the active scope; standalone phases do not.
                for j in [0,1,2,3,4,6]: call(C.byref(host),j,args)
        callbacks.append(original)
        output[0]=C.cast(original,C.c_void_p).value
        return failure!=('hook',rva)
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hook,None,None)
    for failure in [('disabled',0),('gog',0)]+[(kind,rva) for kind in ('verify','hook') for rva in RVAS]+[('none',0)]:
        events.clear(); errors.clear(); logs.clear()
        ok=install(C.byref(host),failure[0]=='gog',failure[0]!='disabled')
        assert not errors,errors
        assert bool(ok)==(failure[0]=='none')
        if failure[0] in ('disabled','gog'): assert not events
        elif failure[0]=='verify': assert events==[('verify',r) for r in RVAS[:RVAS.index(failure[1])+1]]
        else:
            prefix=[('verify',r) for r in RVAS]
            expected=ORDER if failure[0]=='none' else ORDER[:ORDER.index(failure[1])+1]
            assert events==prefix+[('hook',r) for r in expected]
    logs.clear()
    for i in [0,1,2,3,4,6]: call(C.byref(host),i,args)
    assert not logs
    received.clear()
    call(C.byref(host),5,args)
    assert [i for i,_ in received]==[5,0,1,2,3,4,6]
    for i,values in received:
        expected=list(args[:COUNTS[i]])
        if i in (3,4): expected[4]&=0xffffffff
        if i in (5,6): expected[3]&=0xffffffff; expected[4]&=0xffffffff
        if i==6: expected[9]&=0xff
        assert tuple(expected)==values,(i,expected,values)
    assert len(logs)==14 # begin/end for total and the six inner stages
    logs.clear()
    call(C.byref(host),0,args)
    assert not logs # TLS nesting restored
    assert busy()==0 and not errors,errors
    for failure in [('gog',0),('verify',RVAS[5]),('hook',RVAS[5]),('none',0)]:
        events.clear()
        assert bool(activity(C.byref(host),failure[0]=='gog'))==(failure[0]=='none')
        expected=[] if failure[0]=='gog' else [('verify',RVAS[5])]
        if failure[0] in ('hook','none'): expected.append(('hook',RVAS[5]))
        assert events==expected,events
    print('PASS: all seven prologues/instruction boundaries; installer failure guards; native 2/3/6/8/10-argument forwarding; scoped timing')

if __name__=='__main__': main()
