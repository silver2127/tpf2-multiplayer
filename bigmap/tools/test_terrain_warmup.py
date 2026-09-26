"""Exercise late-loading policy in the built native DLL, without a game."""
import ctypes as C
from pathlib import Path

class State(C.Structure):
    _fields_=[('last',C.c_uint64),('bulk',C.c_uint64),('pending',C.c_bool)]

def main():
    dll=C.CDLL(str(Path(__file__).resolve().parents[1]/'out/tpf2_bigmap.dll'))
    update=dll.BigmapTestWarmUpdate
    update.argtypes=[C.POINTER(State),C.c_uint64,C.c_int,C.c_uint64,C.c_uint64,C.c_int]
    s=State()
    def step(now,busy=0,bulk=0,ui=0,signal=1):
        return bool(update(C.byref(s),now,busy,bulk,ui,signal))
    assert not step(1000,ui=999)
    assert step(2000,busy=1,ui=2000)
    assert step(300000,busy=1,ui=300000)  # long generation never times out
    assert step(301000,ui=301000)  # final burst grace
    assert step(450000,ui=1999)  # stale previous-world frame is not readiness
    assert step(451000,ui=440000)  # stale frame from a stalled UI also rejected
    assert not step(452000,ui=451999)  # fresh gameplay frame
    assert step(600000,bulk=599900,ui=452000)  # saved load in the same process
    assert step(800000,bulk=599900,ui=452000)
    assert not step(801000,bulk=599900,ui=801000)
    assert step(900000,bulk=899900,signal=0)
    assert step(1079999,bulk=899900,signal=0)
    assert not step(1080000,bulk=899900,signal=0)  # old DLL fallback bounded
    assert not step(1081000,bulk=899900,signal=0)  # old burst cannot re-arm
    assert step(1200000,bulk=1199999)
    assert not step(2100000,bulk=1199999)  # cancelled load safety cap
    assert step(2200000,bulk=2199999)
    budget=dll.BigmapTestTerrainBudget
    budget.argtypes=[C.c_int]*4+[C.c_uint64]
    assert budget(1024,4096,0,1,3<<30)==1024  # below the 4 GiB gate: pressure overrides the tail
    assert budget(1024,4096,0,1,7<<30)==4096  # 7 GiB free: 2 GiB reserve leaves room for warm
    assert budget(1024,4096,0,1,8<<30)==4096
    assert budget(1024,0,0,1,16<<30)==1024
    print('PASS: generation, late load, paused-ready frame, stale heartbeat, repeat load, fallback, cancellation, memory pressure')

if __name__=='__main__':main()
