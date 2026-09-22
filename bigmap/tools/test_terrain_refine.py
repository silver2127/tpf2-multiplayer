"""Compare fast terrain height refinement with ORIGINAL executable code, natively.

Copies sub_terrain_util::InternBicubicRefine (Steam 35924 RVA 0x3ac6c0) and the
CMat4f product it calls (0x2fadc0) into this process. RIP-relative constants
and the stack cookie are relocated; the three call targets become: product ->
copied product, cookie check -> ret, assert -> a stub that counts and leaves
through the stock epilogue. Complete output buffers (including unwritten and
overlapping regions) must be identical. No game process or files are touched.
Build the DLL first: build.bat
"""
import ctypes as C
import random
import struct
import sys
import time
from pathlib import Path
import capstone
import numpy as np
import pefile

ROOT = Path(__file__).resolve().parents[1]
EXE = r'C:\tools\bin\TransportFever2.exe'
IMAGE = 0x140000000
FUNC, FUNC_END = 0x3ac6c0, 0x3acf50
MATMUL, MATMUL_END = 0x2fadc0, 0x2fafbb
EPILOGUE = 0x3ace6e          # mov rcx,[rbp+0x160]: cookie check, add rsp, pops, ret
COOKIE, COOKIE_CHECK, ASSERT = 0x41cc1b8, 0x2bf3a30, 0x221adf0
CONSTANTS = [0x2f1e988, 0x2f20a14, 0x2f20a70, 0x2f20ba0, 0x2fa7e00, 0x2fa7e10,
             0x2fa7e30, 0x2fa7e50, 0x2fb4700, 0x2fb4710]
PROLOGUE = bytes.fromhex('4055534157488dac24c0fdffff4881ec40030000')
REGIONS = [(0x3ac6d4, 2172), (0x2fadc0, 507), (0x2f1e988, 4), (0x2f20a14, 4), (0x2f20a70, 16),
           (0x2f20ba0, 16), (0x2fa7e00, 32), (0x2fa7e30, 16), (0x2fa7e50, 16), (0x2fb4700, 32)]
FN = C.CFUNCTYPE(None, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int,
                 C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int)
U16 = np.uint16
F32 = np.float32


def load_stock(pe, k32):
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    func = bytearray(pe.get_data(FUNC, FUNC_END - FUNC))
    matmul = pe.get_data(MATMUL, MATMUL_END - MATMUL)
    assert func[:20] == PROLOGUE
    block = k32.VirtualAlloc(None, 0x10000, 0x3000, 0x40)
    assert block
    layout = {MATMUL: 0x1000, COOKIE_CHECK: 0x1800, ASSERT: 0x1810}
    data = {COOKIE: 0x3008}
    for n, rva in enumerate(CONSTANTS):
        data[rva] = 0x2000 + 0x20 * n           # movaps needs 16-byte alignment
    strings = 0x3100
    calls, rip, jumps = [], set(), 0
    for ins in dis.disasm(bytes(func), FUNC):
        pos = ins.address - FUNC
        if ins.mnemonic == 'call':
            target = ins.operands[0].imm
            calls.append(target)
            struct.pack_into('<i', func, pos + 1, layout[target] - (pos + ins.size))
            continue
        if ins.group(capstone.CS_GRP_JUMP):
            assert FUNC <= ins.operands[0].imm < FUNC_END, ins
            jumps += 1
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp
                rip.add(target)
                if target in data:
                    off = data[target]
                else:
                    assert ins.mnemonic == 'lea' and 0x2fb4370 <= target < 0x2fb4540, ins
                    off = strings                  # assert message strings: never read
                struct.pack_into('<i', func, pos + ins.disp_offset, off - (pos + ins.size))
    assert sorted(calls) == sorted([MATMUL] * 2 + [COOKIE_CHECK] + [ASSERT] * 6), calls
    assert set(data) <= rip and jumps > 0
    for ins in dis.disasm(matmul, MATMUL):          # self-contained leaf
        assert not ins.group(capstone.CS_GRP_CALL) and not ins.group(capstone.CS_GRP_JUMP)
        assert not any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                       for op in ins.operands)
    assert sum(i.size for i in dis.disasm(matmul, MATMUL)) == len(matmul) and matmul[-1] == 0xc3
    C.memmove(block, bytes(func), len(func))
    C.memmove(block + 0x1000, matmul, len(matmul))
    C.memmove(block + 0x1800, b'\xc3', 1)
    stub = bytearray(b'\x48\x83\xc4\x08' + b'\x48\xff\x05\0\0\0\0' + b'\xe9\0\0\0\0')
    struct.pack_into('<i', stub, 7, 0x3000 - (0x1810 + 11))
    struct.pack_into('<i', stub, 12, (EPILOGUE - FUNC) - (0x1810 + 16))
    C.memmove(block + 0x1810, bytes(stub), len(stub))
    for rva, off in data.items():
        if rva != COOKIE:
            C.memmove(block + off, pe.get_data(rva, 16), 16)
    C.memmove(block + 0x3008, struct.pack('<Q', 0x2ddfa232), 8)
    counter = C.c_uint64.from_address(block + 0x3000)
    return block, FN(block), counter


class Arena:
    """One contiguous uint16 buffer holding source samples and output."""
    def __init__(self, words):
        self.a = np.zeros(words, dtype=U16)

    def ptr(self, index):
        return self.a.ctypes.data + 2 * index


def reference(k, data, width, x1, y1, stride, out):
    """Independent numpy float32 model of the stock op order, zero terms kept."""
    ys, xs = np.arange(0, y1 - 1), np.arange(0, x1 - 1)
    base = ys[:, None] * width + xs[None, :]
    a = {(r, c): data[base + r * width + c].astype(F32) for r in range(3) for c in range(3)}
    q, h = F32(0.25), F32(0.5)
    a00, a01, a02, a10, a11, a12, a20, a21, a22 = (a[r, c] for r in range(3) for c in range(3))
    G = [(((a01 + a00) + a10) + a11) * q, (((a02 + a01) + a11) + a12) * q,
         ((a01 - a00) + (a11 - a10)) * h, ((a02 - a01) + (a12 - a11)) * h,
         (((a11 + a10) + a20) + a21) * q, (((a12 + a11) + a21) + a22) * q,
         ((a11 - a10) + (a21 - a20)) * h, ((a12 - a11) + (a22 - a21)) * h,
         ((a10 - a00) + (a11 - a01)) * h, ((a12 - a02) + (a11 - a01)) * h,
         (a11 - a10) - (a01 - a00), (a12 - a11) - (a02 - a01),
         ((a20 - a10) + (a21 - a11)) * h, ((a22 - a12) + (a21 - a11)) * h,
         (a21 - a20) - (a11 - a10), (a22 - a21) - (a12 - a11)]
    M1 = [F32(v) for v in (1, 0, -3, 2, 0, 0, 3, -2, 0, 1, -2, 1, 0, 0, -1, 1)]
    M2 = [F32(v) for v in (1, 0, 0, 0, 0, 0, 1, 0, -3, 3, -2, -1, 2, -2, 1, 1)]

    def product(A, B):   # 0x2fadc0(out, A=rdx, B=r8)
        return [((A[4 + c] * B[4 * r + 1] + A[c] * B[4 * r]) + A[8 + c] * B[4 * r + 2]) + A[12 + c] * B[4 * r + 3]
                for r in range(4) for c in range(4)]
    Cc = product(product(M1, G), M2)
    kf, half, extreme = F32(k), k >> 1, 0
    for i in range(k):
        t = F32(i) / kf
        t2 = t * t
        t3 = t2 * t
        P = [((Cc[4 + c] * t + Cc[c]) + t2 * Cc[8 + c]) + Cc[12 + c] * t3 for c in range(4)]
        for j in range(k):
            s = F32(j) / kf
            s2 = s * s
            s3 = s2 * s
            v = ((P[1] * s + P[0]) + s2 * P[2]) + P[3] * s3
            assert np.all(np.abs(v) < 2 ** 31)
            extreme += int(np.count_nonzero((v < 0) | (v >= 65536)))
            idx = (k * ys + half + i)[:, None] * stride + (k * xs + half + j)[None, :]
            out[idx] = (v.astype(np.int32) & 0xffff).astype(U16)
    return extreme


def main():
    pe = pefile.PE(EXE, fast_load=True)
    k32 = C.WinDLL('kernel32', use_last_error=True)
    k32.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
    k32.VirtualAlloc.restype = C.c_void_p
    k32.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
    block, stock, asserts = load_stock(pe, k32)
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    fast = dll.BigmapTestBicubicRefine
    fast.argtypes = [FN] + list(FN._argtypes_)
    fast.restype = None
    crt = C.CDLL('ucrtbase')
    crt._controlfp_s.argtypes = [C.POINTER(C.c_uint), C.c_uint, C.c_uint]
    rng = random.Random(35924)
    nrng = np.random.default_rng(35924)
    scale = (C.c_float * 3)(4.0, 4.0, 1.0)

    def pattern(kind, n, width):
        idx = np.arange(n)
        x, y = idx % width, idx // width
        if kind == 'zero': return np.zeros(n, U16)
        if kind == 'max': return np.full(n, 65535, U16)
        if kind == 'ramp_x': return ((x * 997) % 65536).astype(U16)
        if kind == 'ramp_y': return ((y * 1231) % 65536).astype(U16)
        if kind == 'diagonal': return ((x * 311 + y * 173) % 65536).astype(U16)
        if kind == 'checker': return np.where((x + y) % 2, 65535, 0).astype(U16)
        if kind == 'stripes': return np.where((x // 2) % 2, 65535, 0).astype(U16)
        if kind == 'random': return nrng.integers(0, 65536, n, dtype=np.int64).astype(U16)
        if kind == 'spikes':
            a = np.full(n, 32768, U16)
            m = nrng.random(n) < 0.05
            a[m] = nrng.choice(np.array([0, 65535], U16), int(m.sum()))
            return a
        if kind == 'terrain':
            walk = np.cumsum(nrng.integers(-300, 301, n)) + 20000
            return np.clip(walk, 0, 65535).astype(U16)
        if kind == 'edges':
            return nrng.choice(np.array([0, 1, 2, 32767, 32768, 65533, 65534, 65535], U16), n)
        raise KeyError(kind)
    kinds = ['zero', 'max', 'ramp_x', 'ramp_y', 'diagonal', 'checker', 'stripes', 'random',
             'spikes', 'terrain', 'edges']

    stats = {'cases': 0, 'pixels': 0, 'extreme': 0, 'reference': 0}
    rounding = {'near': 0, 'down': 0x100, 'up': 0x200, 'chop': 0x300}

    def run(k, width, x0, y0, x1, y1, stride, dx, dy, kind, alias=False, mode='near', ref=False):
        rows_in = y1 + 2
        n_in = rows_in * width + 8
        cells_y, cells_x = max(0, y1 - 1 - y0), max(0, x1 - 1 - x0)
        half = k >> 1
        if cells_x and cells_y:
            rmin, rmax = half + dy, k * (cells_y - 1) + half + dy + k - 1
            cmin, cmax = half + dx, k * (cells_x - 1) + half + dx + k - 1
            lo, hi = rmin * stride + cmin, rmax * stride + cmax
        else:
            lo = hi = 0
        n_out = hi - lo + 1 + 32
        if alias:   # output region starts inside the source samples
            start_in = max(0, 16 - lo)
            out_index = start_in + rng.randrange(0, max(1, n_in // 2)) - lo
            words = max(start_in + n_in, out_index + hi + 16) + 16
        else:
            start_in = 16 + max(0, lo)          # keeps out_index >= 0 for the model slice
            out_index = start_in + n_in + 16 - lo
            words = out_index + hi + 32
        assert out_index + lo >= 0
        arena1 = Arena(words)
        arena1.a[:] = nrng.integers(0, 65536, words, dtype=np.int64).astype(U16)
        arena1.a[start_in:start_in + n_in] = pattern(kind, n_in, width)
        arena2 = Arena(words)
        arena2.a[:] = arena1.a
        vec1 = (C.c_void_p * 3)(arena1.ptr(start_in), arena1.ptr(start_in + n_in), arena1.ptr(start_in + n_in))
        vec2 = (C.c_void_p * 3)(arena2.ptr(start_in), arena2.ptr(start_in + n_in), arena2.ptr(start_in + n_in))
        before = arena1.a.copy() if ref else None
        tail = (C.addressof(scale),)
        a1 = (k, C.addressof(vec1), width, x0, y0, x1, y1) + tail + (arena1.ptr(out_index), stride, dx, dy)
        a2 = (k, C.addressof(vec2), width, x0, y0, x1, y1) + tail + (arena2.ptr(out_index), stride, dx, dy)
        count = asserts.value
        old = C.c_uint()
        if mode != 'near':
            assert crt._controlfp_s(C.byref(old), rounding[mode], 0x300) == 0
        try:
            stock(*a1)
            fast(stock, *a2)
        finally:
            if mode != 'near':
                crt._controlfp_s(C.byref(old), 0, 0x300)
        assert asserts.value == count, 'stock asserted on a valid case'
        if not np.array_equal(arena1.a, arena2.a):
            bad = np.nonzero(arena1.a != arena2.a)[0]
            raise AssertionError(dict(k=k, width=width, x0=x0, y0=y0, x1=x1, y1=y1, stride=stride,
                                      dx=dx, dy=dy, kind=kind, alias=alias, mode=mode, count=len(bad),
                                      first=[(int(i), int(arena1.a[i]), int(arena2.a[i])) for i in bad[:8]]))
        if ref:
            assert x0 == y0 == dx == dy == 0 and not alias and mode == 'near'
            model = before[out_index:].copy()
            stats['extreme'] += reference(k, before[start_in:start_in + n_in], width, x1, y1, stride, model)
            assert np.array_equal(model, arena1.a[out_index:]), 'numpy reference differs from stock'
            stats['reference'] += 1
        stats['cases'] += 1
        stats['pixels'] += cells_x * cells_y * k * k

    # 1. Real tile shapes: 1 m cache (k=4, 67x67 -> 268 stride) and 2 m cache
    #    (k=2, 131x131 -> 262), every data pattern, checked against numpy too.
    for k, width in ((4, 67), (2, 131)):
        for kind in kinds:
            run(k, width, 0, 0, width - 1, width - 1, width * k, 0, 0, kind, ref=True)
    # Exact weights on the 9 samples are non-negative and sum to 1, so valid
    # inputs never leave 0..65535 before truncation (docs/terrain-refine.md).
    assert stats['extreme'] == 0
    print(f"PASS: stock tile shapes, {len(kinds)} patterns each; numpy float32 model agrees "
          f"(0 raw values outside 0..65535, as the convex weights predict)")

    # 2. Engine-shaped calls from BaseGetHeightmapRefined: x0=y0=0, x1=W-1,
    #    y1=H-1, stride=W*k, arbitrary sub-terrain block sizes; k from
    #    1 << (highLevels-baseLevels) for every level gap up to 6.
    for k in (2, 4, 8, 16, 32, 64):
        for _ in range({2: 500, 4: 500, 8: 150, 16: 60, 32: 25, 64: 12}[k]):
            limit = max(3, min(140, int((400000 / (k * k)) ** 0.5)))
            width, height = rng.randrange(3, limit + 1), rng.randrange(3, limit + 1)
            run(k, width, 0, 0, width - 1, height - 1, width * k, 0, 0, rng.choice(kinds),
                ref=(k <= 8 and rng.random() < 0.05))
    print(f"PASS: engine-shaped calls, k in 2..64 powers, random block sizes ({stats['cases']} cases so far)")

    # 3. Arbitrary valid arguments: every even k to 64, offsets, x1 == srcDim
    #    (reads into the next row), empty loops, small and odd strides
    #    (overlapping writes), negative dx/dy.
    for _ in range(2500):
        k = rng.randrange(2, 65, 2)
        width = rng.randrange(1, 90)
        x0 = rng.randrange(0, width + 1)
        x1 = rng.randrange(x0, width + 1)
        y0 = rng.randrange(0, 40)
        y1 = y0 + rng.choice([0, 1, 2, 3, rng.randrange(0, 30)])
        while max(0, x1 - 1 - x0) * max(0, y1 - 1 - y0) * k * k > 300000:
            y1 = y0 + (y1 - y0) // 2
        stride = rng.choice([k * max(1, x1 - x0), rng.randrange(1, 4 * k + 2), rng.randrange(1, 900)])
        dx, dy = rng.randrange(-3 * k, 3 * k), rng.randrange(-3, 4)
        run(k, width, x0, y0, x1, y1, stride, dx, dy, rng.choice(kinds))
    print(f"PASS: arbitrary offsets, borders, strides and every even k ({stats['cases']} cases so far)")

    # 4. Output aliasing the source samples (stock reads each cell before its writes).
    for _ in range(300):
        k = rng.choice([2, 4, 6, 8])
        width = rng.randrange(3, 60)
        run(k, width, 0, 0, width - 1, rng.randrange(2, 40), rng.choice([width * k, rng.randrange(1, 300)]),
            rng.randrange(-4, 4), rng.randrange(-2, 3), rng.choice(kinds), alias=True)
    print('PASS: output buffer overlapping the source samples')

    # 5. Non-default MXCSR rounding: same operations, same results.
    for mode in ('down', 'up', 'chop'):
        for kind in kinds:
            run(4, 67, 0, 0, 66, 66, 268, 0, 0, kind, mode=mode)
            run(2, 131, 0, 0, 130, 130, 262, 0, 0, kind, mode=mode)
        for _ in range(60):
            k = rng.randrange(2, 34, 2)
            width = rng.randrange(3, 50)
            run(k, width, rng.randrange(0, 2), rng.randrange(0, 2), width - rng.randrange(0, 2),
                rng.randrange(3, 30), width * k + rng.randrange(0, 3), rng.randrange(-2, 2),
                rng.randrange(-1, 2), rng.choice(kinds), mode=mode)
    print('PASS: rounding modes down/up/chop')
    print(f"PASS: {stats['cases']} native original-code comparisons, {stats['pixels']} refined samples, "
          f"{stats['reference']} also against the numpy model; complete buffers identical")

    # 6. Stock asserts and oversized k go to the original with every argument intact.
    calls = []

    @FN
    def fallback(*args):
        calls.append(args)
    data = np.arange(64 * 64, dtype=U16)
    vec = (C.c_void_p * 3)(data.ctypes.data, data.ctypes.data + data.nbytes, data.ctypes.data + data.nbytes)
    out = np.zeros(1 << 20, U16)
    tail = (C.addressof(scale), out.ctypes.data, 400, 0, 0)
    invalid = [(0, 20, 0, 0, 10, 10), (1, 20, 0, 0, 10, 10), (3, 20, 0, 0, 10, 10), (-2, 20, 0, 0, 10, 10),
               (4, 20, -1, 0, 10, 10), (4, 20, 0, -1, 10, 10), (4, 20, 0, 0, 21, 10),
               (4, 20, 5, 0, 4, 10), (4, 20, 0, 5, 10, 4), (-2147483648, 20, 0, 0, 10, 10)]
    for k, width, x0, y0, x1, y1 in invalid:
        calls.clear()
        args = (k, C.addressof(vec), width, x0, y0, x1, y1) + tail
        fast(fallback, *args)
        assert len(calls) == 1 and calls[0] == args, (args, calls)
        count = asserts.value
        stock(*args)
        assert asserts.value == count + 1, ('stock did not assert', args)
    for k in (66, 128, 256):
        calls.clear()
        args = (k, C.addressof(vec), 8, 0, 0, 7, 5, C.addressof(scale), out.ctypes.data, 8 * k, 0, 0)
        fast(fallback, *args)
        assert len(calls) == 1 and calls[0] == args
        run(k, 8, 0, 0, 7, 4, 8 * k, 0, 0, 'random')          # fast path == original here
    for k, width, x0, y0, x1, y1 in [(2, 20, 0, 0, 20, 10), (4, 20, 3, 3, 3, 3), (4, 20, 0, 0, 1, 10),
                                     (4, 20, 0, 0, 10, 1), (64, 20, 0, 0, 3, 3), (2, 20, 19, 0, 20, 5)]:
        calls.clear()
        fast(fallback, k, C.addressof(vec), width, x0, y0, x1, y1, *tail)
        assert not calls
        count = asserts.value
        stock(k, C.addressof(vec), width, x0, y0, x1, y1, *tail)
        assert asserts.value == count
        run(k, width, x0, y0, x1, y1, 400, 0, 0, 'random')
    print('PASS: all six stock assert conditions and k > 64 forward the untouched arguments to the original; '
          'boundary-valid inputs stay on the fast path')

    # 7. Installer: prologue boundary, every verified region, refusal paths.
    from test_world_entry import Host, logtype, basetype, verifytype, hooktype
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    steal = list(dis.disasm(PROLOGUE, IMAGE + FUNC))
    assert sum(i.size for i in steal) == 20 and [i.size for i in steal] == [2, 1, 2, 8, 7]
    for ins in steal:
        assert not ins.group(capstone.CS_GRP_JUMP) and not ins.group(capstone.CS_GRP_CALL)
        assert not any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                       for op in ins.operands)
    install = dll.BigmapTestInstallTerrainRefine
    install.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    expected_verify = [(FUNC, 20)] + REGIONS
    failures = ['none', 'disabled', 'gog', 'hook'] + [('verify', i) for i in range(len(expected_verify))]
    for failure in failures:
        events, errors = [], []

        @logtype
        def log(fmt):
            pass

        @basetype
        def getbase():
            return IMAGE

        @verifytype
        def verify(rva, ptr, size):
            events.append(('verify', rva, size))
            if C.string_at(ptr, size) != pe.get_data(rva, size):
                errors.append(('bytes', hex(rva)))
            return failure != ('verify', len(events) - 1)

        @hooktype
        def hook(target, detour, steal_bytes, output):
            events.append(('hook',))
            if target != IMAGE + FUNC or steal_bytes != 20 or not detour:
                errors.append('hook')
            output[0] = block
            return failure != 'hook'
        host = Host(C.sizeof(Host), 1, log, None, None, None, getbase, None, verify, hook, None, None)
        ok = install(C.byref(host), failure == 'gog', failure != 'disabled')
        assert bool(ok) == (failure == 'none'), failure
        assert not errors, errors
        verified = [('verify', r, n) for r, n in expected_verify]
        if failure in ('disabled', 'gog'):
            assert events == []
        elif isinstance(failure, tuple):
            assert events == verified[:failure[1] + 1], (failure, events)
        else:
            assert events == verified + [('hook',)], events
    print(f'PASS: installer verifies the 20-byte prologue (5 whole instructions, no RIP/branches) and '
          f'{sum(n for _, n in REGIONS)} bytes of body/product/constants; disabled, GOG, each mismatch and hook failure refuse')

    # 8. Warm-cache native microbenchmark (ctypes call overhead included, same for both).
    print('benchmark (per call, best of 5 rounds):')
    for label, k, width, kind, calls_per_round in (('1 m tile k=4 67x67', 4, 67, 'terrain', 200),
                                                   ('2 m tile k=2 131x131', 2, 131, 'terrain', 200),
                                                   ('1 m tile k=4 random', 4, 67, 'random', 200),
                                                   ('k=8 35x35', 8, 35, 'terrain', 200)):
        n_in = (width + 2) * width + 8
        src = pattern(kind, n_in, width)
        vecb = (C.c_void_p * 3)(src.ctypes.data, src.ctypes.data + src.nbytes, src.ctypes.data + src.nbytes)
        o1 = np.zeros(width * k * width * k + 64, U16)
        o2 = o1.copy()
        args1 = (k, C.addressof(vecb), width, 0, 0, width - 1, width - 1, C.addressof(scale), o1.ctypes.data, width * k, 0, 0)
        args2 = args1[:8] + (o2.ctypes.data,) + args1[9:]
        best = [1e9, 1e9]
        for _ in range(5):
            t = time.perf_counter()
            for _ in range(calls_per_round):
                stock(*args1)
            best[0] = min(best[0], (time.perf_counter() - t) / calls_per_round)
            t = time.perf_counter()
            for _ in range(calls_per_round):
                fast(stock, *args2)
            best[1] = min(best[1], (time.perf_counter() - t) / calls_per_round)
        assert np.array_equal(o1, o2)
        cells = (width - 2) ** 2
        print(f'  {label}: stock {best[0] * 1e6:.1f} us, fast {best[1] * 1e6:.1f} us '
              f'({best[0] / cells * 1e9:.0f} vs {best[1] / cells * 1e9:.0f} ns/cell), speedup {best[0] / best[1]:.2f}x')
    k32.VirtualFree(block, 0, 0x8000)


if __name__ == '__main__':
    main()
