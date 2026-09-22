"""Native game-vector ownership and Steam hook guards; no live game writes."""
import ctypes as C
from pathlib import Path
import pefile
import capstone
from test_world_entry import Host, logtype, basetype, verifytype, hooktype

ROOT=Path(__file__).resolve().parents[1]
N=257*257
class Vec(C.Structure):
    _fields_=[('first',C.c_void_p),('last',C.c_void_p),('end',C.c_void_p)]
class Control(C.Structure):
    _fields_=[('vtable',C.c_void_p),('strong',C.c_uint32),('weak',C.c_uint32),('v',Vec)]
Resize=C.CFUNCTYPE(None,C.POINTER(Vec),C.c_size_t)
Copy=C.CFUNCTYPE(C.c_void_p,C.POINTER(Vec),C.POINTER(Vec))
Destroy=C.CFUNCTYPE(None,C.c_void_p)

def main():
    dll=C.CDLL(str(ROOT/'out/tpf2_bigmap.dll'))
    budget=dll.BigmapTestTerrainBudget;budget.argtypes=[C.c_int]*4+[C.c_uint64]
    for busy,bulk,available,warm,want in [(0,0,16<<30,4096,1024),(1,0,16<<30,4096,4096),
            (0,1,16<<30,4096,4096),(1,1,(8<<30)-1,4096,4096),
            (1,1,(4<<30)-1,4096,1024),  # below the 4 GiB gate
            (1,1,0,4096,1024),(1,0,16<<30,0,1024),(1,0,16<<30,512,1024)]:
        assert budget(1024,warm,busy,bulk,available)==want,(busy,bulk,available,warm,budget(1024,warm,busy,bulk,available),want)
    # Budgets sized from installed RAM (hot 0 / warm -1 in the cfg).
    autoT=dll.BigmapTestAutoTerrainBudgets;autoT.argtypes=[C.c_uint64,C.POINTER(C.c_int),C.POINTER(C.c_int)]
    autoM=dll.BigmapTestAutoMaterialBudgets;autoM.argtypes=[C.c_uint64,C.POINTER(C.c_int),C.POINTER(C.c_int)]
    for gib,(th,tw),(mh,mw) in [(4,(256,341),(96,96)),(8,(273,682),(96,170)),
                                (16,(546,1365),(96,341)),(32,(1092,2730),(182,682)),
                                (96,(3276,8192),(546,2048)),(512,(4096,8192),(1024,4096))]:
        h,w=C.c_int(),C.c_int();autoT(gib<<30,C.byref(h),C.byref(w));assert (h.value,w.value)==(th,tw),(gib,h.value,w.value,th,tw)
        h,w=C.c_int(),C.c_int();autoM(gib<<30,C.byref(h),C.byref(w));assert (h.value,w.value)==(mh,mw),(gib,h.value,w.value,mh,mw)
    live=dll.BigmapTestTerrainBudgetLive;live.argtypes=[C.c_int]*4+[C.c_uint64,C.c_uint64,C.c_uint64]
    G=1<<30
    for args,want in [((1024,4096,1,0,64*G,8000),8000),        # loading: cover every live tile
                      ((1024,4096,0,1,16*G,8000),4096),        # 16 GiB free: 12 reserved, warm is the floor
                      ((1024,4096,1,0,30*G,30000),15360),      # capped at half of available
                      ((1024,4096,1,0,200*G,100000),65536),    # absolute clamp
                      ((1024,4096,1,0,64*G,2000),4096),        # never below warm while loading
                      ((1024,4096,1,0,5*G,20000),4096),        # small machine: 2 GiB floor reserve, warm is the floor
                      ((1024,4096,0,0,64*G,8000),1024),        # not loading: hot
                      ((1024,0,1,0,64*G,8000),1024),           # warm disabled stays disabled
                      ((1024,4096,1,1,7*G,8000),4096),         # 7 GiB free: inside the 12 GiB reserve, warm is the floor
                      ((1024,4096,1,1,(4*G)-1,8000),1024),     # below the gate: hot
                      ((3072,4096,1,0,64*G,0),4096)]:          # no live size: warm only
        assert live(*args,0)==want,(args,live(*args,0),want)
    # The reserve floor follows the machine (PagerHeadroom): 32 GiB -> 4.6 GiB,
    # so a 32 GiB machine with 16 GiB free keeps 8 GiB of tiles while loading
    # (a flat 12 GiB floor allowed 4 GiB, the warm budget).
    headroom=dll.BigmapTestPagerHeadroom;headroom.argtypes=[C.c_uint64];headroom.restype=C.c_uint64
    tight=dll.BigmapTestCommitTightBytes;tight.argtypes=[C.c_uint64];tight.restype=C.c_uint64
    for phys,want in [(0,12*G),(8*G,2*G),(16*G,(16*G)//7),(32*G,(32*G)//7),(94*G,12*G),(512*G,12*G)]:
        assert headroom(phys)==want,(phys,headroom(phys),want)
    for phys,want in [(0,10*G),(8*G,2*G),(16*G,2*G),(32*G,4*G),(94*G,10*G)]:
        assert tight(phys)==want,(phys,tight(phys),want)
    assert live(1024,4096,1,0,16*G,20000,32*G)==8192
    assert live(1024,4096,1,0,5*G,20000,32*G)==4096      # under the 4.6 GiB headroom: warm is the floor
    assert live(1024,4096,1,0,16*G,20000,96*G)==4096     # a big machine keeps its measured 12 GiB
    # Steady state after a load: ramp down instead of snapping, hold or grow
    # while the engine faults evicted tiles back in, cap by what is free.
    # The ceiling counts what the pager holds as room of its own (free RAM +
    # prev), reserves a quarter of that room or the machine's headroom, and
    # gives the terrain pager 3/4 of the rest (share 3), the material pager 1/2.
    steady=dll.BigmapTestTerrainBudgetSteady;steady.argtypes=[C.c_int]*3+[C.c_uint64]*3+[C.c_uint]
    for args,want in [((13365,3195,3195,0,64*G,0,2),12530),     # quiet: down by 1/16 per second
                      ((300,256,256,0,64*G,0,2),256),           # the floor is the policy's own target
                      ((8000,3195,3195,150,64*G,0,2),8000),     # 100..299 cold restores/s: hold
                      ((8000,3195,3195,500,64*G,0,2),9000),     # >= 300/s: grow by 1/8
                      ((600,3195,3195,500,64*G,0,2),3195),      # never below the floor
                      ((8000,3195,3195,500,8*G,0,2),5147),      # 8 GiB free under a 12 GiB headroom: pressure cuts 1/8, room 15.8 GiB - 12 leaves 1.9 GiB on top of hot
                      ((9000,3195,3195,500,24*G,0,2),10125),    # 24 GiB free + 8.8 held: 12 reserved, half of 20.8 on top of hot; grows by 1/8
                      ((8000,3195,3195,0,24*G,0,2),7500),       # a quiet ramp under the ceiling
                      ((12000,3195,3195,0,24*G,0,2),11250),     # a quiet ramp from a high target
                      ((65000,3195,3195,500,400*G,0,2),65536),  # absolute clamp
                      ((3195,3195,3195,500,64*G,0,2),3594),     # from the floor: +1/8, at least 128
                      ((500,400,400,500,64*G,0,2),628),         # small budgets grow by the 128 MiB minimum
                      # A 32 GiB machine after a big load: 12 GiB held, 4 GiB free, engine faulting.
                      # Old rule: the ceiling was the 1092 MiB hot budget (free < 12 GiB) -> mass eviction and decode thrash.
                      ((12000,1092,1092,500,4*G,32*G,3),9652),  # pressure (4 < 4.6 GiB free) cuts 1/8; room 16 GiB - 4.6 reserve, 3/4 of it on top of hot
                      ((9652,1092,1092,50,6*G,32*G,3),9049),    # 6 GiB free: no pressure, a quiet 1/16 ramp
                      ((9000,1092,1092,150,8*G,32*G,3),9000),   # 8 GiB free: hold under a 10.2 GiB ceiling
                      ((9000,1092,1092,500,8*G,32*G,3),10125)]: # ... or grow by 1/8 inside it
        assert steady(*args)==want,(args,steady(*args),want)
    # Adaptive eviction rate: cost cap (a quarter core), stall halving, quiet growth.
    step=dll.BigmapTestEvictRateStep;step.argtypes=[C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint64),C.c_uint64,C.c_uint,C.c_int,C.c_uint];step.restype=C.c_uint
    def run(rate,quiet,avg,stalls,ui,ceil,cost=0):
        r,q,k=C.c_uint(rate),C.c_uint(quiet),C.c_uint64(cost);out=step(C.byref(r),C.byref(q),C.byref(k),avg,stalls,ui,ceil);return out,r.value,q.value
    def cost_after(cost,avg):
        r,q,k=C.c_uint(1000),C.c_uint(0),C.c_uint64(cost);step(C.byref(r),C.byref(q),C.byref(k),avg,0,1,4000);return k.value
    assert cost_after(0,400)==400                      # first measurement is taken whole
    assert cost_after(400,800)==500                    # then three parts old, one new
    assert cost_after(400,0)==400                      # a second without evictions keeps it
    assert run(4000,9,0,0,1,4000,cost=400)==(625,625,10)   # ... and the remembered cost still caps
    assert run(0,0,0,0,1,4000)==(1000,1000,1)          # first second: starts at 1000
    assert run(0,0,0,0,1,600)==(600,600,1)             # ... or at the ceiling if lower
    assert run(1000,0,400,0,1,4000)==(625,625,1)       # 400 us each: 250 ms/s allows 625
    assert run(1000,0,50,0,1,4000)==(1000,1000,1)      # cheap: cost cap above the rate
    assert run(1000,3,400,1,1,4000)==(500,500,0)       # a stall halves and resets quiet
    assert run(120,0,400,1,1,4000)==(100,100,0)        # never below 100
    assert run(500,5,100,0,1,4000)==(625,625,6)        # sixth quiet second: +25%
    assert run(500,5,400,0,1,4000)==(625,625,6)        # ... but capped by the cost
    assert run(3800,9,10,0,1,4000)==(4000,4000,10)     # ... and by the ceiling
    assert run(500,5,0,1,0,4000)==(625,625,6)          # no UI signal: stalls are ignored
    assert run(500,5,5000,0,1,4000)==(100,100,6)       # 5 ms each: floor 100
    assert run(1000,0,400,0,1,0)==(0,1000,0)           # ceiling 0: unlimited, state untouched
    stallsOf=dll.BigmapTestUiStalls;stallsOf.argtypes=[C.POINTER(C.c_uint64),C.c_int];stallsOf.restype=C.c_uint
    def stalls(seq):
        arr=(C.c_uint64*len(seq))(*seq);return stallsOf(arr,len(seq))
    assert stalls([100,125,150,175])==0                # 60 fps: stamps 25 ms apart as sampled
    assert stalls([100,125,225,250])==1                # one 100 ms gap
    assert stalls([100,100,100,160])==1                # stamp held for 60 ms: one stall
    assert stalls([100,159,218])==0                    # 59 ms gaps do not count
    assert stalls([0,0,100,200,0,300])==1              # zeros (no world) never pair with a stamp
    assert dll.BigmapTestCompressionInit()
    resize=dll.BigmapTestCompressionResize;resize.argtypes=[C.POINTER(Vec),C.c_size_t,C.c_int,Resize]
    copy=dll.BigmapTestCompressionCopy;copy.argtypes=[C.POINTER(Vec),C.POINTER(Vec),C.c_int,Copy];copy.restype=C.c_void_p
    destroy=dll.BigmapTestCompressionDestroy;destroy.argtypes=[C.c_void_p,Destroy]
    evict=dll.BigmapTestCompressionEvict;evict.argtypes=[C.c_void_p]
    calls=[];backings=[]
    @Resize
    def stock_resize(v,n):
        calls.append(('resize',n));b=C.create_string_buffer(n*2);backings.append(b)
        v[0]=Vec(C.addressof(b),C.addressof(b)+n*2,C.addressof(b)+n*2)
    @Copy
    def stock_copy(dst,src):calls.append(('copy',));dst[0]=src[0];return C.addressof(dst.contents)
    @Destroy
    def stock_destroy(control):calls.append(('destroy',))
    c=Control(None,2,1,Vec());resize(C.byref(c.v),N,1,stock_resize)
    assert not calls and c.v.last-c.v.first==N*2
    initial=c.v.first
    source=(C.c_uint16*N)(*(i*7%65536 for i in range(N)))
    C.memmove(initial,source,N*2);assert evict(initial)
    c2=Control(None,1,1,Vec())
    assert copy(C.byref(c2.v),C.byref(c.v),1,stock_copy)==C.addressof(c2.v)
    assert c2.v.first!=initial and C.string_at(c2.v.first,N*2)==bytes(source)
    C.c_uint16.from_address(c2.v.first).value=123
    assert C.c_uint16.from_address(initial).value==source[0]
    # Destruction of a compressed COW version leaves the shared original intact.
    assert evict(c2.v.first);destroy(C.byref(c2),stock_destroy)
    assert not c2.v.first and not calls and c.strong==2 and c.weak==1
    resize(C.byref(c.v),3,1,stock_resize)
    resize(C.byref(c.v),N,1,stock_resize)
    assert c.v.first==initial and C.string_at(initial+6,N*2-6)==bytes(N*2-6)
    assert evict(initial)
    # Unexpected growth must migrate back to the engine allocator, with bytes
    # preserved, before generic vector deallocation can encounter our mapping.
    resize(C.byref(c.v),N+100,1,stock_resize)
    assert c.v.first!=initial and calls==[('resize',N+100)]
    assert C.string_at(c.v.first,6)==bytes(source)[:6]
    destroy(C.byref(c),stock_destroy);assert calls[-1]==('destroy',)
    for n,eligible in [(N,0),(12,1),(0,1)]:
        v=Vec();before=len(calls);resize(C.byref(v),n,eligible,stock_resize)
        assert len(calls)==before+1
    v=Vec();copy(C.byref(v),C.byref(c.v),0,stock_copy);assert calls[-1]==('copy',)

    pe=pefile.PE(r'C:\tools\bin\TransportFever2.exe',fast_load=True)
    dis=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);dis.detail=True
    events=[];errors=[];failure=None
    @logtype
    def log(fmt):pass
    @basetype
    def base():return 0x140000000
    @verifytype
    def verify(rva,p,n):
        events.append(('verify',rva));code=C.string_at(p,n)
        if code!=pe.get_data(rva,n):errors.append(('bytes',hex(rva)))
        ins=list(dis.disasm(code,0x140000000+rva))
        if sum(i.size for i in ins)!=n:errors.append(('boundary',hex(rva)))
        if rva in (0x1d5c50,0x1dedd0,0x33de30):
            for i in ins:
                if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL):errors.append('branch')
                if any(o.type==capstone.x86.X86_OP_MEM and o.mem.base==capstone.x86.X86_REG_RIP for o in i.operands):errors.append('rip')
        return failure!=('verify',rva)
    @hooktype
    def hook(target,detour,n,out):
        events.append(('hook',target-0x140000000));out[0]=0x1234
        return failure!=('hook',target-0x140000000)
    host=Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hook,None,None)
    install=dll.BigmapTestInstallCompression;install.argtypes=[C.POINTER(Host),C.c_int,C.c_int,C.c_int,C.c_int]
    for args in [(1,1,1,1024),(0,2,1,1024),(0,0,1,1024),(0,1,0,1024),(0,1,1,127),(0,1,1,8193)]:
        events.clear();assert not install(C.byref(host),*args);assert not events
    for stage,rv in [('verify',0x1d5c50),('verify',0x1dedd0),('verify',0x33de30),
                     ('verify',0x33cca5),('verify',0x33dd8c),('hook',0x33de30),
                     ('hook',0x1dedd0),('hook',0x1d5c50)]:
        failure=(stage,rv);events.clear();assert not install(C.byref(host),0,1,1,1024)
        hooks=[v for s,v in events if s=='hook']
        if stage=='verify':assert not hooks
        else:assert hooks==[0x33de30,0x1dedd0,0x1d5c50][:len(hooks)]
    assert not errors,errors
    print('PASS: native ownership, COW isolation, compressed destruction, resize migration, guards and Steam byte boundaries')

if __name__=='__main__':main()
