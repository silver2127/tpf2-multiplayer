"""Compare compiled spacing fix against original Steam instructions in Unicorn.

No running game or installed files are modified. Requires pefile and unicorn.
"""
import ctypes as C
import math
import random
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x140000000
ENTRY = BASE + 0x910ce0
HEAP, STACK, STOP = 0x20000000, 0x30000000, 0x40000000
LIMIT = 2**31 - 1

class Point(C.Structure):
    _fields_ = [('x', C.c_int32), ('y', C.c_int32), ('angle', C.c_float)]

class Exclusion(C.Structure):
    _fields_ = [('x', C.c_int32), ('y', C.c_int32)]

class Vector(C.Structure):
    _fields_ = [('begin', C.c_void_p), ('end', C.c_void_p), ('capacity', C.c_void_p)]

def vector(array):
    start = C.addressof(array)
    return Vector(start, start + C.sizeof(array), start + C.sizeof(array))

def f32(value):
    return C.c_float(value).value

def bits(value):
    return struct.pack('<f', value)

def main():
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    fixed = dll.BigmapTestPlacementSpacing
    fixed.argtypes = [C.POINTER(Vector), C.POINTER(Vector), C.c_float,
                      C.POINTER(Vector), C.c_float]
    fixed.restype = None
    pe = pefile.PE(r'C:\tools\bin\TransportFever2.exe', fast_load=True)
    expected = bytes.fromhex('4c8bdc49894b08534881eca0000000')
    assert pe.get_data(0x910ce0, len(expected)) == expected
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    for rva in [0x910000, 0x1de000, 0x2bf6000, 0x2bf3000, 0x2f94000]:
        uc.mem_map(BASE + rva, 0x1000)
        uc.mem_write(BASE + rva, pe.get_data(rva, 0x1000))
    uc.mem_map(HEAP, 0x100000)
    uc.mem_map(STACK, 0x20000)
    uc.mem_map(STOP, 0x1000)

    def ret():
        rsp = uc.reg_read(UC_X86_REG_RSP)
        target, = struct.unpack('<Q', uc.mem_read(rsp, 8))
        uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        uc.reg_write(UC_X86_REG_RIP, target)

    def hook(uc, address, size, data):
        if address == BASE + 0x1dee90:
            target = uc.reg_read(UC_X86_REG_RCX)
            count = uc.reg_read(UC_X86_REG_RDX)
            start = HEAP + 0x80000
            uc.mem_write(start, struct.pack('<i', LIMIT) * count)
            uc.mem_write(target, struct.pack('<QQQ', start, start+4*count, start+4*count))
            ret()
        elif address == BASE + 0x2bf6847:
            value, = struct.unpack('<f', (uc.reg_read(UC_X86_REG_XMM0) & 0xffffffff).to_bytes(4, 'little'))
            result = math.sqrt(value) if value >= 0 else float('nan')
            uc.reg_write(UC_X86_REG_XMM0, int.from_bytes(bits(result), 'little'))
            ret()
        elif address == BASE + 0x2bf3abc:
            ret()

    uc.hook_add(UC_HOOK_CODE, hook)

    def run(points, exclusions=(), minimum=1000.0, resolution=4.0):
        p = (Point * len(points))(*(Point(x, y, 0.123) for x, y in points))
        e = (Exclusion * len(exclusions))(*(Exclusion(x, y) for x, y in exclusions))
        output = (C.c_float * len(points))()
        vp, ve, vo = vector(p), vector(e), vector(output)
        before = bytes(p), bytes(e), bytes(vp), bytes(ve), bytes(vo)
        fixed(C.byref(vo), C.byref(vp), minimum, C.byref(ve), resolution)
        assert before == (bytes(p), bytes(e), bytes(vp), bytes(ve), bytes(vo))
        def distance(a, b):
            return min(LIMIT, (a[0]-b[0])**2 + (a[1]-b[1])**2)
        reference = []
        for i, point in enumerate(points):
            nearest = min([LIMIT] + [distance(point, q) for j, q in enumerate(points) if i != j]
                          + [distance(point, q) for q in exclusions])
            d = f32(math.sqrt(f32(f32(nearest) * f32(resolution * resolution))))
            reference.append(f32(minimum / d) if minimum <= d else 99999.0)
        assert bytes(output) == b''.join(map(bits, reference)), (points, list(output), reference)

        for offset, array in [(0x1000, p), (0x20000, e)]:
            if C.sizeof(array): uc.mem_write(HEAP + offset, bytes(array))
        for offset, start, length in [(0, 0x1000, C.sizeof(p)), (0x20, 0x20000, C.sizeof(e)),
                                      (0x40, 0x40000, C.sizeof(output))]:
            uc.mem_write(HEAP + offset, struct.pack('<QQQ', HEAP+start, HEAP+start+length, HEAP+start+length))
        rsp = STACK + 0x10008
        uc.mem_write(rsp, struct.pack('<Q', STOP))
        uc.mem_write(rsp + 0x28, bits(resolution))
        uc.reg_write(UC_X86_REG_RSP, rsp)
        uc.reg_write(UC_X86_REG_RCX, HEAP + 0x40)
        uc.reg_write(UC_X86_REG_RDX, HEAP)
        uc.reg_write(UC_X86_REG_XMM2, int.from_bytes(bits(minimum), 'little'))
        uc.reg_write(UC_X86_REG_R9, HEAP + 0x20)
        uc.emu_start(ENTRY, STOP, count=10000000)
        assert uc.reg_read(UC_X86_REG_RIP) == STOP
        stock = struct.unpack('<' + 'f'*len(points), uc.mem_read(HEAP+0x40000, C.sizeof(output))) if points else ()
        return tuple(output), stock

    rng = random.Random(35924)
    cases = [([], []), ([(0, 0)], []), ([(0, 0), (0, 0)], []),
             ([(0, 0), (46340, 0)], []), ([(0, 0), (32767, 32767)], [])]
    for _ in range(75):
        cases.append(([(rng.randrange(15000), rng.randrange(15000)) for _ in range(rng.randrange(1, 25))],
                      [(rng.randrange(15000), rng.randrange(15000)) for _ in range(rng.randrange(5))]))
    for points, exclusions in cases:
        result, stock = run(points, exclusions)
        assert b''.join(map(bits, result)) == b''.join(map(bits, stock)), (result, stock)
    print(f'PASS: {len(cases)} non-overflow cases bit-identical to original game instructions')

    for points, exclusions in [([(0, 0), (46341, 0)], []),
                                ([(0, 0), (32768, 32768)], []),
                                ([(0, 0)], [(0, 60000)]),
                                ([(0, 0), (0, 72960)], [])]:
        result, stock = run(points, exclusions)
        assert all(math.isfinite(x) and 0 < x < 1 for x in result)
        assert any(not math.isfinite(x) or bits(x) != bits(y) for x, y in zip(stock, result)), stock
    print('PASS: reproduced stock NaNs/false distances above 185 km; fixed pair and exclusion scores finite')
    for _ in range(40):
        run([(rng.randrange(14593), rng.randrange(72961)) for _ in range(20)], [(0, 0), (14592, 72960)])
    run([(-2**31, -2**31), (2**31-1, 2**31-1)])
    run([(0, 0), (131072, 16384)], [(131071, 16383)])
    print('PASS: 292 km map, depth-13 limits, extreme int32 coordinates and independent wide reference')

    # Installer refuses unmeasured builds and byte/hook failures. Invoke the
    # actual detour pointer supplied to the host to verify its native ABI too.
    logtype = C.CFUNCTYPE(None, C.c_char_p)
    basetype = C.CFUNCTYPE(C.c_size_t)
    verifytype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    hooktype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_int, C.POINTER(C.c_void_p))
    class Host(C.Structure):
        _fields_ = [('size',C.c_uint32),('abi',C.c_uint32),('log',logtype),
            ('cfgInt',C.c_void_p),('cfgBool',C.c_void_p),('cfgStr',C.c_void_p),
            ('base',basetype),('buildOk',C.c_void_p),('verify',verifytype),
            ('hook',hooktype),('patch',C.c_void_p),('dataDir',C.c_void_p)]
    install = dll.BigmapTestInstallPlacement
    install.argtypes = [C.POINTER(Host), C.c_int]
    install.restype = C.c_int
    for fail in ['none', 'gog', 'verify', 'hook']:
        events, errors, detours = [], [], []
        @logtype
        def log(fmt): pass
        @basetype
        def base(): return BASE
        @verifytype
        def verify(rva, ptr, size):
            events.append('verify')
            if rva != 0x910ce0 or C.string_at(ptr, size) != expected: errors.append('verify bytes')
            return fail != 'verify'
        @hooktype
        def hookfn(target, detour, steal, output):
            events.append('hook')
            if target != ENTRY or steal != 15: errors.append('hook target')
            detours.append(detour)
            output[0] = 1
            return fail != 'hook'
        host = Host(C.sizeof(Host), 1, log, None, None, None, base, None, verify, hookfn, None, None)
        assert install(C.byref(host), fail == 'gog') == (fail == 'none')
        assert not errors
        assert events == ([] if fail == 'gog' else ['verify'] if fail == 'verify' else ['verify', 'hook'])
        if fail == 'none':
            fixed = C.CFUNCTYPE(None, *fixed.argtypes)(detours[0])
            run([(0, 0), (0, 72960)])
    print('PASS: native detour ABI and installer failure guards')

    fast = dll.BigmapTestInstallFastPlacement
    fast.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    fast.restype = C.c_int
    patchtype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    fast_expected = bytes.fromhex('41b9c8000000')
    assert pe.get_data(0x912f59, 6) == fast_expected
    uc.mem_map(BASE + 0x912000, 0x1000)
    uc.mem_write(BASE + 0x912000, pe.get_data(0x912000, 0x1000))
    seen = []
    def worker_hook(uc, address, size, data):
        if address == BASE + 0x9106c0:
            uc.reg_write(UC_X86_REG_RAX, HEAP + 0x72000)
            ret()
        elif address == BASE + 0x910f40:
            rsp = uc.reg_read(UC_X86_REG_RSP)
            extras = [struct.unpack('<Q', uc.mem_read(rsp+0x28+8*i, 8))[0] for i in range(6)]
            seen.append((uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_RDX),
                uc.reg_read(UC_X86_REG_R8), uc.reg_read(UC_X86_REG_R9), extras))
            ret()
    uc.hook_add(UC_HOOK_CODE, worker_hook)
    for attempts, fail in [(50,'none'), (1,'none'), (199,'none'), (200,'none'),
                           (0,'none'), (201,'none'), (50,'gog'), (50,'verify'), (50,'patch')]:
        events, errors, emitted = [], [], []
        @verifytype
        def fast_verify(rva, ptr, size):
            events.append('verify')
            if rva != 0x912f59 or C.string_at(ptr,size) != fast_expected: errors.append('fast verify')
            return fail != 'verify'
        @patchtype
        def fast_patch(rva, ptr, size):
            events.append('patch')
            value=C.string_at(ptr,size)
            if rva != 0x912f59 or value != b'\x41\xb9'+struct.pack('<I',attempts): errors.append('fast bytes')
            emitted.append(value)
            return fail != 'patch'
        host=Host(C.sizeof(Host),1,log,None,None,None,base,None,fast_verify,hookfn,
                  C.cast(fast_patch,C.c_void_p),None)
        success = fail == 'none' and 1 <= attempts < 200
        assert fast(C.byref(host),fail=='gog',attempts) == success
        assert not errors
        assert events == ([] if fail=='gog' or not 1 <= attempts < 200 else
                          ['verify'] if fail=='verify' else ['verify','patch'])
        if not success: continue
        uc.mem_write(BASE+0x912f59,emitted[0])
        uc.ctl_remove_cache(BASE+0x912f10, BASE+0x912fb7)
        uc.mem_write(HEAP+0x70070,struct.pack('<I',123))
        uc.mem_write(HEAP+0x70078,struct.pack('<Q',HEAP+0x71000))
        uc.mem_write(HEAP+0x71030,struct.pack('<ff',40.0,1000.0))
        uc.mem_write(HEAP+0x71068,struct.pack('<Q',0x12345678))
        rsp=STACK+0x10008
        uc.mem_write(rsp,struct.pack('<Q',STOP))
        uc.mem_write(rsp-0x200,b'\0'*0x200)
        uc.reg_write(UC_X86_REG_RSP,rsp)
        uc.reg_write(UC_X86_REG_RCX,HEAP+0x70000)
        uc.reg_write(UC_X86_REG_RDX,HEAP+0x73000)
        uc.emu_start(BASE+0x912f10,STOP,count=1000)
        assert uc.reg_read(UC_X86_REG_RIP)==STOP
        assert uc.reg_read(UC_X86_REG_RAX)==HEAP+0x73000
        rcx,rdx,r8,r9,extras=seen[-1]
        assert (rcx,rdx,r8,r9)==(HEAP+0x73000,HEAP+0x72000,123,attempts)
        assert extras==[HEAP+0x71050,0x12345678,HEAP+0x71000,
                       int.from_bytes(bits(40.0),'little'),int.from_bytes(bits(1000.0),'little'),HEAP+0x71038]
    print('PASS: fast placement patch guards and original worker instructions; only attempt argument changes')

if __name__ == '__main__':
    main()
