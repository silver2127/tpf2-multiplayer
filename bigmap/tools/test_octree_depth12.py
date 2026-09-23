"""Offline depth-12/13 tests. No game process or game files are modified.

Requires pefile, unicorn; build.bat first. Executes the original Steam 35924
octree descent instructions in Unicorn with allocation stubs. The entry hook
uses the compiled DLL's real ID allocator; decoder bytes also come from DLL.
This validates insertion/IDs/decoder, not a complete running renderer.
"""
import argparse
import ctypes as C
import itertools
import random
import struct
from pathlib import Path
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x140000000
DESCEND = BASE + 0xa507e0
ALLOC = BASE + 0x2bf3a80
CTOR = BASE + 0x2bf3c9c
STOP = 0x40000000
STACK = 0x30000000
HEAP = 0x20000000

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', type=Path, default=Path(
        r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe'))
    args = ap.parse_args()
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    choose = dll.BigmapTestOctreeId
    choose.argtypes = [C.c_int] * 6
    choose.restype = C.c_int
    build = dll.BigmapTestOctreeLevelStub
    build.argtypes = [C.c_void_p, C.c_size_t]
    build.restype = C.c_size_t
    pe = pefile.PE(str(args.exe), fast_load=True)
    assert pe.get_data(0x2304f8, 13).hex() == 'f30f1015984cd302ba0a000000'
    assert pe.get_data(0xa507e0, 21).hex() == '488bc455565741544155415641574881ecf0000000'
    stock_loop = bytes.fromhex('41b801000000458bcb458be3413bd07c164503c841ffc4468d04c500000000438d04083bd07dea')
    assert pe.get_data(0x853d30, 39) == stock_loop
    assert bytes(pe.__data__).count(stock_loop) == 1

    # Exercise the actual installer with a mock host: a mismatch or earlier
    # failure must never publish the depth/root change.
    logtype = C.CFUNCTYPE(None, C.c_char_p)
    basetype = C.CFUNCTYPE(C.c_size_t)
    verifytype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    hooktype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_int, C.POINTER(C.c_void_p))
    patchtype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    class Host(C.Structure):
        _fields_ = [('size',C.c_uint32),('abi',C.c_uint32),('log',logtype),
            ('cfgInt',C.c_void_p),('cfgBool',C.c_void_p),('cfgStr',C.c_void_p),
            ('base',basetype),('buildOk',C.c_void_p),('verify',verifytype),
            ('hook',hooktype),('patch',patchtype),('dataDir',C.c_void_p)]
    install = dll.BigmapTestInstallOctree12
    install.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    install.restype = C.c_int
    cases = list(itertools.product([12, 13], ['none','gog','root_verify','descend_verify','level_verify','hook','level_patch','root_patch']))
    cases += [(14, 'invalid_depth')]
    for depth, fail in cases:
        events = []
        callback_errors = []
        def checked(fn):
            def wrapped(*args):
                try: return fn(*args)
                except BaseException as exc:
                    callback_errors.append(exc)
                    return 0
            return wrapped
        @logtype
        def log(fmt): pass
        @basetype
        def base(): return BASE
        @verifytype
        @checked
        def verify(rva, ptr, size):
            label = {0x2304f8:'root',0xa507e0:'descend',0x853d30:'level'}[rva]
            events.append(label + '_verify')
            assert C.string_at(ptr,size) == pe.get_data(rva,size)
            return fail != label + '_verify'
        @hooktype
        @checked
        def hook(target, detour, steal, output):
            events.append('hook')
            assert target == DESCEND and steal == 21 and detour
            output[0] = 1  # Never executed by this host.
            return fail != 'hook'
        @patchtype
        @checked
        def patch(rva, ptr, size):
            events.append('root_patch' if rva == 0x2304f8 else 'level_patch')
            data = C.string_at(ptr,size)
            if rva == 0x2304f8:
                assert data == b'\xb8' + struct.pack('<f', 2.0**(depth+5)) + bytes.fromhex('660f6ed031d2b2') + bytes([depth])
            else:
                assert rva == 0x853d30 and size == 39
                assert data[:6] == bytes.fromhex('ff2500000000')
            return fail != events[-1]
        host = Host(C.sizeof(Host),1,log,None,None,None,base,None,verify,hook,patch,None)
        result = install(C.byref(host), fail == 'gog', depth)
        assert not callback_errors, callback_errors
        assert result == (fail == 'none')
        if fail == 'none':
            assert events == ['root_verify','descend_verify','level_verify','hook','level_patch','root_patch']
        elif fail in ['gog', 'invalid_depth']: assert not events
        else:
            assert events[-1] == fail
            if fail != 'root_patch': assert 'root_patch' not in events

    # Exercise the native nine-argument wrapper ABI, including stack arguments.
    fnargs = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_int,
              C.c_void_p, C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
    callback_type = C.CFUNCTYPE(C.c_void_p, *fnargs)
    captured = []
    @callback_type
    def original(*values):
        captured.append(values)
        return values[1]
    invoke = dll.BigmapTestOctreeDescend
    invoke.argtypes = [callback_type] + fnargs
    invoke.restype = C.c_void_p
    parent = C.create_string_buffer(0xa8)
    C.c_int.from_buffer(parent, 8).value = 0x09249249
    slot = C.c_void_p()
    forwarded = [0x1234, 0x5678, C.addressof(parent), -99, 0x1110, 0x2220,
                 1, 0x3330, C.addressof(slot)]
    assert invoke(original, *forwarded) == 0x5678
    assert captured[0][:3] == tuple(forwarded[:3])
    assert captured[0][4:] == tuple(forwarded[4:])
    assert captured[0][3] >= 0x50000000

    # Execute generated decoder stub with the real stock register contract.
    buf = C.create_string_buffer(256)
    n = build(buf, STOP)
    stub = buf.raw[:n]
    assert stub[33:72] == stock_loop
    def decode(index, patched=True):
        u = Uc(UC_ARCH_X86, UC_MODE_64)
        u.mem_map(STOP, 4096)
        u.mem_map(STOP + 4096, 4096)
        u.mem_write(STOP + 4096, stub if patched else stock_loop)
        u.reg_write(UC_X86_REG_EDX, index & 0xffffffff)
        u.reg_write(UC_X86_REG_R11, 0)
        end = STOP if patched else STOP + 4096 + len(stock_loop)
        u.emu_start(STOP + 4096, end, count=500)
        assert u.reg_read(UC_X86_REG_RIP) == end, 'decoder did not terminate'
        return u.reg_read(UC_X86_REG_R12)
    for level in range(11):
        first = (8**level - 1) // 7
        last = (8**(level + 1) - 1) // 7 - 1
        for index in {first, last, (first + last)//2}:
            assert decode(index) == decode(index, False) == level
    for index in [0x50000000, 0x50000001, 0x5fffffff]:
        assert decode(index) == 11
    for index in [0x60000000, 0x60000001, 0x6fffffff]:
        assert decode(index) == 12

    # Run original machine code, replacing only allocator/empty array ctor and
    # the node-ID argument exactly as the native detour does.
    def world(depth, patched):
        u = Uc(UC_ARCH_X86, UC_MODE_64)
        pages = set()
        for address, size in [(DESCEND, 0x437), (ALLOC, 1), (CTOR, 1),
                              (BASE + 0x2f1e988, 4)]:
            for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                if page not in pages:
                    u.mem_map(page, 4096)
                    pages.add(page)
            u.mem_write(address, pe.get_data(address - BASE, size))
        u.mem_map(HEAP, 16 * 1024 * 1024)
        u.mem_map(STACK, 1024 * 1024)
        u.mem_map(STOP, 4096)
        allocated = []
        next_alloc = HEAP + 4096
        def readq(p): return struct.unpack('<Q', u.mem_read(p, 8))[0]
        def readi(p): return struct.unpack('<i', u.mem_read(p, 4))[0]
        def ret(value):
            sp = u.reg_read(UC_X86_REG_RSP)
            u.reg_write(UC_X86_REG_RAX, value)
            u.reg_write(UC_X86_REG_RIP, readq(sp))
            u.reg_write(UC_X86_REG_RSP, sp + 8)
        def hook(u, address, size, data):
            nonlocal next_alloc
            if address == ALLOC:
                size = u.reg_read(UC_X86_REG_RCX)
                result = next_alloc
                next_alloc += (size + 15) & ~15
                assert next_alloc < HEAP + 16 * 1024 * 1024
                if size == 0xa8: allocated.append(result)
                ret(result)
            elif address == CTOR:
                # Each empty unique_ptr's constructor stores null; calloc'd
                # emulator heap already has these exact bytes.
                assert u.reg_read(UC_X86_REG_RDX) == 8
                assert u.reg_read(UC_X86_REG_R8) == 8
                ret(0)
            elif address == DESCEND and patched:
                sp = u.reg_read(UC_X86_REG_RSP)
                parent = u.reg_read(UC_X86_REG_R8)
                remaining = readi(sp + 0x38)
                existing = readq(readq(sp + 0x48))
                index = C.c_int(u.reg_read(UC_X86_REG_R9D)).value
                value = choose(index, readi(parent + 8) if parent else 0,
                    bool(parent), remaining, readi(existing + 8) if existing else 0,
                    bool(existing))
                u.reg_write(UC_X86_REG_R9D, value & 0xffffffff)
        for address in [ALLOC, CTOR, DESCEND]:
            u.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)
        def insert(point, object_extent=0):
            sp = STACK + 0x80008
            result, center, extent, box, root_slot = [HEAP + v for v in [0x20,0x40,0x60,0x80,0]]
            half = 2.0 ** (depth + 5)
            u.mem_write(center, struct.pack('<3f', *point))
            u.mem_write(extent, struct.pack('<3f', *([object_extent]*3)))
            u.mem_write(box, struct.pack('<6f', *([-half]*3 + [half]*3)))
            u.mem_write(sp, struct.pack('<Q', STOP))
            u.mem_write(sp + 0x28, struct.pack('<5Q', center, extent, depth, box, root_slot))
            for reg, value in [(UC_X86_REG_RSP, sp), (UC_X86_REG_RCX, 0x1234),
                (UC_X86_REG_RDX, result), (UC_X86_REG_R8, 0), (UC_X86_REG_R9, 0)]:
                u.reg_write(reg, value)
            u.emu_start(DESCEND, STOP, count=50000)
            assert u.reg_read(UC_X86_REG_RIP) == STOP
            node = readq(result)
            # Engine nodes store a loose box twice the tight cell's width.
            loose = struct.unpack('<6f', u.mem_read(node + 0xc, 24))
            expected_loose = 512 if object_extent == 200 else 256
            assert all(abs(loose[i+3] - loose[i] - expected_loose) < 0.01 for i in range(3)), loose
            assert all(loose[i] <= point[i] <= loose[i+3] for i in range(3))
            return node, readi(node + 8)
        half = 2.0 ** (depth + 5)
        points = list(itertools.product([-half + 72, 72.], repeat=3)) if depth >= 12 else [(-10., 20., 30.)]
        if patched and depth >= 12:
            rng = random.Random(35924)
            points += [tuple(rng.uniform(-half+1,half-1) for _ in range(3)) for _ in range(150)]
        leaves = [insert(point) for point in points]
        for point, leaf in zip(points, leaves):
            assert insert(point) == leaf, 'reinsertion changed leaf identity'
        if patched and depth == 13:
            for point, leaf in zip(points, leaves):
                internal = insert(point, 200)
                assert internal[0] == readq(leaf[0])
                assert 0x50000000 <= internal[1] < 0x60000000
                assert 0x60000000 <= leaf[1] < 0x70000000
                assert insert(point) == leaf
        ids = [readi(node + 8) for node in allocated]
        if patched or depth <= 11:
            assert len(ids) == len(set(ids)), 'node ID collision'
            assert all(0 <= value <= 0x7fffffff for value in ids)
            for node in allocated:
                level, parent = 0, readq(node)
                while parent:
                    level += 1
                    parent = readq(parent)
                # Arithmetic decoder for old IDs, generated decoder sampled
                # above at every boundary and for the full compact range.
                value = readi(node + 8)
                if value >= 0x50000000: actual = (value >> 28) + 6
                else:
                    actual, threshold, width = 0, 1, 1
                    while value >= threshold:
                        actual += 1; width *= 8; threshold += width
                assert actual == level
        else:
            assert len(ids) != len(set(ids)) or any(value < 0 for value in ids)
        return len(allocated)
    count13 = world(13, True)
    world(13, False)
    count = world(12, True)
    world(12, False)
    world(11, True)
    world(10, True)
    bounds = dll.BigmapTestOctreeSize
    bounds.argtypes = [C.c_int]*3 + [C.POINTER(C.c_int)]*2
    for depth, cap, enabled, wanted, expected in [
        (13,2048,1,(2048,64),(2048,64)),
        (13,2048,1,(2048,254),(2048,254)),
        (13,2048,1,(2048,256),(2046,256)),
        (13,2048,1,(2048,2048),(724,724)),
        (12,2048,1,(2048,64),(1024,64)),
        (11,2048,1,(2048,64),(512,64)),
        (13,2048,0,(2048,64),(256,64)),
        (13,512,1,(2048,64),(512,64)),
        (13,2048,1,(2049,65),(2048,64)),
    ]:
        x, y = map(C.c_int, wanted)
        bounds(depth,cap,enabled,C.byref(x),C.byref(y))
        assert (x.value,y.value) == expected, (wanted,x.value,y.value)
        assert (x.value*64+1)*(y.value*64+1) <= 2147483647
    # The GOG build cannot install depths 12/13, so its ceiling is the patched
    # depth-11 box whatever the config asks: the menu may never offer more tiles
    # than the root that actually went in can hold.
    ceiling = dll.BigmapTestOctreeCeiling
    ceiling.argtypes = [C.c_int]*4
    for gog, depth, cap, enabled, expected in [
        (0,13,2048,1,2048),
        (0,12,2048,1,1024),
        (0,11,2048,1,512),
        (1,13,2048,1,512),
        (1,12,2048,1,512),
        (1,11,2048,1,512),
        (1,13,2048,0,256),
        (0,13,2048,0,256),
        (1,13,256,1,256),
    ]:
        got = ceiling(gog,depth,cap,enabled)
        assert got == expected, (gog,depth,cap,enabled,got,expected)
    shape = dll.BigmapTestDeriveShape
    shape.argtypes = [C.c_int]*3 + [C.POINTER(C.c_int)]*2
    for side in range(2, 2049, 2):
        for ratio in range(5):
            x, y = C.c_int(), C.c_int()
            shape(side, ratio, 2048, C.byref(x), C.byref(y))
            assert x.value % 2 == y.value % 2 == 0
            assert y.value == x.value * (ratio + 1)
            assert max(x.value,y.value) <= 2048
            assert (x.value*64+1)*(y.value*64+1) <= 2147483647
    print(f'PASS: installer failure guards; native wrapper ABI; decoder boundaries; '
          f'{count13} depth-13 and {count} depth-12 original-code nodes; '
          'stock overflow reproduced; internal/leaf ID reuse; depth-10/11 compatibility; 5120 shapes')

if __name__ == '__main__':
    main()
