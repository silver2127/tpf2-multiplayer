"""Compare fast terrain alignment with ORIGINAL executable code, natively.

Maps TransportFever2.exe at its preferred base 0x140000000 inside this process,
so terrain_alignment_util::CalculateHeightMod (0x3b3470), the rasterizer
(0x2375420 / 0x23754b0), both PredHeightModRasterizable predicates (0x3b7440,
0x3b6ee0) and the stock vector resize (0x3af850) all execute as original machine
code. Only CRT imports are resolved (malloc/free/memmove/floorf/ceilf/...), so
every allocation and every float operation of the stock path is the real one.
The fast path is the DLL's replacement, which calls the SAME mapped rasterizer.
No game process or installed file is touched. Build the DLL first: build.bat
"""
import ctypes as C
import random
import struct
import threading
import time
from pathlib import Path

import capstone
import numpy as np
import pefile

ROOT = Path(__file__).resolve().parents[1]
EXE = r'C:\tools\bin\TransportFever2.exe'
IMAGE = 0x140000000
FUNC, FUNC_END = 0x3b3470, 0x3b3cd9
PROLOGUE = bytes.fromhex('488bc45556574154415541564157488da8f8fcffff')
REGIONS = [(0x3b3485, FUNC_END - 0x3b3485), (0x3b0190, 0x3b02cd - 0x3b0190),
           (0x3af850, 0x3afa48 - 0x3af850), (0x2f99078, 4), (0x2f1e98c, 4), (0x2f1e988, 4),
           (0x2fb6658, 13)]
VTABLES = [(0x2fb64c0, (0x3b09a0, 0x3b7440)), (0x2fb64d8, (0x3b09a0, 0x3b6ee0))]
# void CalculateHeightMod(const Box2&, const CVec2i&, float scale, float offset,
#                         const vector<TerrainAlignment const*>&, vector<uint16>&)
FN = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_float, C.c_float, C.c_void_p, C.c_void_p)
U16 = np.uint16
F32 = np.float32

k32 = C.WinDLL('kernel32', use_last_error=True)
k32.VirtualAlloc.restype = C.c_void_p
k32.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
k32.GetProcAddress.restype = C.c_void_p
k32.GetProcAddress.argtypes = [C.c_void_p, C.c_char_p]
k32.RtlAddFunctionTable.argtypes = [C.c_void_p, C.c_uint32, C.c_uint64]


class Vec(C.Structure):
    _fields_ = [('first', C.c_void_p), ('last', C.c_void_p), ('end', C.c_void_p)]


def map_image(pe):
    """The whole exe at its preferred base: no relocation, absolute vtables valid."""
    base = k32.VirtualAlloc(C.c_void_p(IMAGE), pe.OPTIONAL_HEADER.SizeOfImage, 0x3000, 0x40)
    assert base == IMAGE, f'preferred image base {IMAGE:#x} is not free in this process ({base})'
    headers = pe.OPTIONAL_HEADER.SizeOfHeaders
    C.memmove(IMAGE, bytes(pe.__data__[:headers]), headers)
    for section in pe.sections:
        data = section.get_data()
        span = (section.Misc_VirtualSize + 0xfff) & ~0xfff
        size = min(len(data), span)
        C.memmove(IMAGE + section.VirtualAddress, data[:size], size)
    return base


def resolve_imports(pe):
    """CRT imports point at this process's real CRT; everything else traps."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    handles = {}
    for name in ('ucrtbase', 'vcruntime140', 'kernel32'):
        try:
            handles[name] = C.WinDLL(name)._handle
        except OSError:
            pass
    trap = k32.VirtualAlloc(None, 0x1000, 0x3000, 0x40)
    C.memset(trap, 0xcc, 0x1000)
    resolved, trapped = {}, 0
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode().lower()
        key = 'ucrtbase' if dll.startswith('api-ms-win-crt') else dll[:-4]
        handle = handles.get(key)
        for symbol in entry.imports:
            slot = symbol.address - pe.OPTIONAL_HEADER.ImageBase
            address = k32.GetProcAddress(C.c_void_p(handle), symbol.name) if handle and symbol.name else None
            if address:
                resolved[symbol.name.decode()] = address
            else:
                address, trapped = trap, trapped + 1
            C.memmove(IMAGE + slot, struct.pack('<Q', address), 8)
    for needed in ('malloc', 'free', 'memmove', 'floorf', 'ceilf', '_isnan', '_finite'):
        assert needed in resolved, needed
    pdata = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.pdata'][0]
    k32.RtlAddFunctionTable(C.c_void_p(IMAGE + pdata.VirtualAddress),
                            pdata.Misc_VirtualSize // 12, IMAGE)
    return trapped


# ---------------------------------------------------------------- fixtures
class Alignment:
    """One terrain::TerrainAlignment: vector<CVec3f> tris, vector<CVec3f> weights, int type."""
    def __init__(self, triangles, weights, kind):
        self.tris = np.ascontiguousarray(triangles, F32).reshape(-1)
        self.weights = None if weights is None else np.ascontiguousarray(weights, F32).reshape(-1)
        self.blob = (C.c_uint8 * 0x38)()
        first = self.tris.ctypes.data
        struct.pack_into('<QQQ', self.blob, 0, first, first + self.tris.nbytes, first + self.tris.nbytes)
        if self.weights is None:
            struct.pack_into('<QQQ', self.blob, 0x18, 0, 0, 0)
        else:
            w = self.weights.ctypes.data
            struct.pack_into('<QQQ', self.blob, 0x18, w, w + self.weights.nbytes, w + self.weights.nbytes)
        struct.pack_into('<i', self.blob, 0x30, kind)


class Case:
    """A complete CalculateHeightMod call: block, box, alignment list and result vector."""
    GUARD = 16      # words of untouched memory on both sides of the result

    def __init__(self, sx, sy, box, scale, offset, alignments, heights):
        self.sx, self.sy, self.box, self.scale, self.offset = sx, sy, box, scale, offset
        self.alignments = alignments
        self.n = sx * sy
        self.heights = heights
        self.size = (C.c_int32 * 2)(sx, sy)
        self.boxf = (C.c_float * 4)(*box)
        pointers = [C.addressof(a.blob) for a in alignments]
        self.pointers = (C.c_void_p * max(1, len(pointers)))(*pointers)
        start = C.addressof(self.pointers)
        self.list = Vec(start, start + 8 * len(pointers), start + 8 * len(pointers))

    def arena(self):
        return self.heights[:self.n + 2 * self.GUARD].copy()

    def vector(self, arena):
        first = arena.ctypes.data + 2 * self.GUARD
        return Vec(first, first + 2 * self.n, first + 2 * self.n)

    def call(self, fn, arena, fast=None, base=None):
        vec = self.vector(arena)
        args = (C.addressof(self.boxf), C.addressof(self.size), self.scale, self.offset,
                C.byref(self.list), C.byref(vec))
        if fast is None:
            fn(*args)
        else:
            fast(fn, base, *args)


def triangles(kind, rng, nrng, sx, sy, x0, y0, cell, zlo, zhi, count):
    """Alignment triangle fans in the same world space as the block box."""
    x1, y1 = x0 + (sx - 1) * cell, y0 + (sy - 1) * cell
    w, h = x1 - x0, y1 - y0
    out = []

    def z():
        return rng.choice([rng.uniform(zlo, zhi), rng.choice([zlo, zhi, (zlo + zhi) / 2]),
                           zlo - (zhi - zlo), zhi + (zhi - zlo)])
    for _ in range(count):
        if kind == 'band':          # a road/track strip crossing the block
            ax, ay = rng.uniform(x0 - w, x1 + w), rng.uniform(y0 - h, y1 + h)
            angle = rng.uniform(0, 6.283)
            length, width = rng.uniform(0.5, 3.0) * max(w, h), rng.uniform(0.5, 12.0) * cell
            dx, dy = np.cos(angle) * length, np.sin(angle) * length
            nx, ny = -np.sin(angle) * width, np.cos(angle) * width
            za, zb = z(), z()
            out.append([ax, ay, za, ax + dx, ay + dy, zb, ax + nx, ay + ny, za])
            out.append([ax + dx, ay + dy, zb, ax + dx + nx, ay + dy + ny, zb, ax + nx, ay + ny, za])
        elif kind == 'blob':        # a construction footprint
            cx, cy = rng.uniform(x0, x1), rng.uniform(y0, y1)
            r = rng.uniform(1.0, 0.4 * max(w, h, 1.0))
            zz = z()
            out.append([cx, cy, zz, cx + r, cy, z(), cx, cy + r, z()])
        elif kind == 'cover':       # the whole block in two triangles
            out.append([x0 - cell, y0 - cell, z(), x1 + cell, y0 - cell, z(), x0 - cell, y1 + cell, z()])
            out.append([x1 + cell, y1 + cell, z(), x1 + cell, y0 - cell, z(), x0 - cell, y1 + cell, z()])
        elif kind == 'grid':        # vertices exactly on sample positions
            gx, gy = rng.randrange(0, sx), rng.randrange(0, sy)
            ex = rng.randrange(1, max(2, sx // 2))
            ey = rng.randrange(1, max(2, sy // 2))
            ax, ay = x0 + gx * cell, y0 + gy * cell
            out.append([ax, ay, z(), ax + ex * cell, ay, z(), ax, ay + ey * cell, z()])
        elif kind == 'tiny':        # sub-cell and degenerate: the rasterizer rejects short edges
            ax, ay = rng.uniform(x0, x1), rng.uniform(y0, y1)
            d = rng.choice([0.0, 1e-4, 1e-3, 0.5 * cell])
            out.append([ax, ay, z(), ax + d, ay, z(), ax, ay + d, z()])
        elif kind == 'line':        # collinear vertices
            ax, ay = rng.uniform(x0, x1), rng.uniform(y0, y1)
            dx, dy = rng.uniform(-w, w), rng.uniform(-h, h)
            out.append([ax, ay, z(), ax + dx, ay + dy, z(), ax + 2 * dx, ay + 2 * dy, z()])
        elif kind == 'outside':
            ax, ay = x1 + 5 * cell + rng.uniform(0, w), y1 + 5 * cell + rng.uniform(0, h)
            out.append([ax, ay, z(), ax + cell, ay, z(), ax, ay + cell, z()])
        elif kind == 'edge':        # straddling one block edge exactly
            side = rng.randrange(4)
            ax = {0: x0, 1: x1, 2: rng.uniform(x0, x1), 3: rng.uniform(x0, x1)}[side]
            ay = {0: rng.uniform(y0, y1), 1: rng.uniform(y0, y1), 2: y0, 3: y1}[side]
            r = rng.uniform(cell, 6 * cell)
            out.append([ax - r, ay - r, z(), ax + r, ay - r, z(), ax, ay + r, z()])
        else:
            raise KeyError(kind)
    if not out:
        return np.zeros((0, 9), F32)
    return np.array(out, F32)


def weights_for(kind, rng, nrng, count):
    if kind is None:
        return None
    if kind == 'ones':
        return np.ones((count, 3), F32)
    if kind == 'edges':
        return nrng.choice(np.array([0.0, 1.0, 0.5, 1 / 65535, 1 - 1 / 65535, 65534 / 65535], F32),
                           (count, 3))
    if kind == 'ramp':
        return (np.arange(count * 3, dtype=F32) % 65536 / 65535).reshape(count, 3)
    if kind == 'random':
        return nrng.random((count, 3)).astype(F32)
    if kind == 'wild':
        return (nrng.random((count, 3)).astype(F32) * F32(2.4) - F32(0.7))
    raise KeyError(kind)


def build_case(rng, nrng, sx, sy, mix, scale=None, offset=None, cell=None, zbase=None):
    cell = cell if cell is not None else rng.choice([1.0, 1.0, 1.0, 2.0, 0.5, 4.0])
    x0 = float(rng.randrange(-4096, 4096)) * cell
    y0 = float(rng.randrange(-4096, 4096)) * cell
    scale = scale if scale is not None else rng.choice([0.05, 0.0625, 0.1, 0.03125, 1.0 / 512])
    offset = offset if offset is not None else rng.choice([0.0, -100.0, -1000.0, 12.5])
    zbase = zbase if zbase is not None else offset + scale * rng.uniform(0, 40000)
    zlo, zhi = zbase, zbase + scale * rng.uniform(10, 30000)
    alignments = []
    for kind, wkind, count, typ in mix:
        tris = triangles(kind, rng, nrng, sx, sy, x0, y0, cell, zlo, zhi, count)
        alignments.append(Alignment(tris, weights_for(wkind, rng, nrng, len(tris)),
                                    typ if typ is not None else rng.randrange(3)))
    heights = nrng.integers(0, 65536, sx * sy + 2 * Case.GUARD, dtype=np.int64).astype(U16)
    box = (x0, y0, x0 + (sx - 1) * cell, y0 + (sy - 1) * cell)
    return Case(sx, sy, box, scale, offset, alignments, heights)


def main():
    pe = pefile.PE(EXE, fast_load=True)
    map_image(pe)
    trapped = resolve_imports(pe)
    assert pe.get_data(FUNC, len(PROLOGUE)) == PROLOGUE, 'unexpected build'
    stock = FN(IMAGE + FUNC)
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    fast = dll.BigmapTestTerrainAlign
    fast.argtypes = [FN, C.c_size_t] + list(FN._argtypes_)
    fast.restype = None
    rng = random.Random(35924)
    nrng = np.random.default_rng(35924)
    stats = {'cases': 0, 'samples': 0, 'written': 0, 'triangles': 0}

    def compare(case, label=''):
        a1, a2 = case.arena(), case.arena()
        case.call(stock, a1)
        case.call(stock, a2, fast=fast, base=IMAGE)
        if not np.array_equal(a1, a2):
            bad = np.nonzero(a1 != a2)[0]
            raise AssertionError(dict(label=label, sx=case.sx, sy=case.sy, box=case.box,
                                      scale=case.scale, offset=case.offset, count=len(bad),
                                      first=[(int(i), int(a1[i]), int(a2[i])) for i in bad[:8]]))
        stats['cases'] += 1
        stats['samples'] += case.n
        stats['written'] += int(np.count_nonzero(a1 != case.heights))
        stats['triangles'] += sum(len(a.tris) // 9 for a in case.alignments)
        return a1

    # 1. Engine-shaped blocks: a 1 m tile block is 257x257, the 2 m one 129x129.
    kinds = ['band', 'blob', 'cover', 'grid', 'tiny', 'line', 'outside', 'edge']
    wkinds = [None, 'ones', 'edges', 'ramp', 'random', 'wild']
    for sx, sy in ((257, 257), (129, 129), (65, 65)):
        for kind in kinds:
            for wkind in wkinds:
                mix = [(kind, wkind, rng.randrange(1, 6), t) for t in (0, 1, 2)]
                compare(build_case(rng, nrng, sx, sy, mix), f'{sx}x{sy} {kind} {wkind}')
    print(f"PASS: engine tile blocks 257x257/129x129/65x65, {len(kinds)} triangle shapes x "
          f"{len(wkinds)} weight patterns per alignment type ({stats['cases']} cases)")

    # 2. All three targets fed the SAME geometry: ties in lo/hi, weights of exactly
    #    1.0, and every branch of the 1.0 zeroing and the min/max selection.
    for _ in range(400):
        sx = sy = rng.choice([17, 33, 65])
        kind = rng.choice(['cover', 'grid', 'band'])
        wkind = rng.choice(['ones', 'edges', 'ramp'])
        count = rng.randrange(1, 4)
        shared = [(kind, wkind, count, t) for t in (0, 1, 2)]
        case = build_case(rng, nrng, sx, sy, shared)
        same = case.alignments[0]
        for other in case.alignments[1:]:      # identical triangles in each target
            other.tris[:] = same.tris
            if other.weights is not None and same.weights is not None:
                other.weights[:] = same.weights
        compare(case, 'shared geometry')
    print(f"PASS: identical geometry in all three targets (height ties, weight 1.0 branches)")

    # 3. Arbitrary block shapes, cells, scales and offsets, many alignments.
    for _ in range(700):
        sx, sy = rng.randrange(2, 200), rng.randrange(2, 200)
        mix = [(rng.choice(kinds), rng.choice(wkinds), rng.randrange(0, 5), None)
               for _ in range(rng.randrange(0, 8))]
        compare(build_case(rng, nrng, sx, sy, mix,
                           scale=rng.choice([0.05, 1.0, 0.001, 7.5, -0.05]),
                           offset=rng.choice([0.0, -1000.0, 1e4, -3.25])), 'random')
    print(f"PASS: random block shapes 2..199, cell sizes, scales (incl. negative) and offsets")

    # 4. Empty work: no alignments, empty triangle lists, alignments outside the block.
    for sx, sy in ((257, 257), (2, 2), (2, 300), (300, 2), (3, 7)):
        compare(Case(sx, sy, (0.0, 0.0, float(sx - 1), float(sy - 1)), 0.05, -100.0, [],
                     nrng.integers(0, 65536, sx * sy + 32, dtype=np.int64).astype(U16)), 'empty list')
        compare(build_case(rng, nrng, sx, sy, [('outside', 'ones', 3, 0), ('band', None, 0, 1)]), 'no cover')
    print('PASS: empty alignment lists, empty triangle vectors, geometry entirely outside the block')

    # 5. Non-default MXCSR rounding: the same operations, the same results.
    crt = C.CDLL('ucrtbase')
    crt._controlfp_s.argtypes = [C.POINTER(C.c_uint), C.c_uint, C.c_uint]
    for mode, bits in (('down', 0x100), ('up', 0x200), ('chop', 0x300)):
        old = C.c_uint()
        assert crt._controlfp_s(C.byref(old), bits, 0x300) == 0
        try:
            for _ in range(40):
                sx = sy = rng.choice([33, 65, 129])
                mix = [(rng.choice(kinds), rng.choice(wkinds), 2, t) for t in (0, 1, 2)]
                compare(build_case(rng, nrng, sx, sy, mix), f'rounding {mode}')
        finally:
            crt._controlfp_s(C.byref(old), 0, 0x300)
    print('PASS: rounding modes down/up/chop')
    print(f"PASS: {stats['cases']} native original-code comparisons, {stats['samples']} block samples, "
          f"{stats['triangles']} triangles, {stats['written']} samples changed by the stock blend; "
          f"complete result buffers and their guard words identical")

    # 6. Shapes the replacement refuses: the original runs with untouched arguments.
    calls = []

    @FN
    def fallback(*args):
        calls.append(args)
    base_case = build_case(rng, nrng, 16, 16, [('cover', 'ones', 1, 0)])

    def forwarded(case, expect, label):
        calls.clear()
        arena = case.arena()
        case.call(fallback, arena, fast=fast, base=IMAGE)
        assert len(calls) == expect, (label, len(calls))
        if expect:
            assert calls[0][0] == C.addressof(case.boxf) and calls[0][1] == C.addressof(case.size)
            # the stub does nothing, so a forwarded call must leave the result alone
            assert np.array_equal(arena, case.heights[:len(arena)]), label
    # size.x * size.y != result.size() -- the stock assert
    bad = build_case(rng, nrng, 16, 16, [('cover', 'ones', 1, 0)])
    bad.n = 16 * 16 - 1
    forwarded(bad, 1, 'size assert')
    bad.n = 16 * 16
    for sx, sy in ((1, 16), (16, 1), (0, 4), (-2, 4), (2000, 600)):
        case = build_case(rng, nrng, max(sx, 1), max(sy, 1), [('cover', 'ones', 1, 0)])
        case.size[0], case.size[1] = sx, sy
        case.n = max(sx, 0) * max(sy, 0) if sx > 0 and sy > 0 else 0
        forwarded(case, 1, f'shape {sx}x{sy}')
    for typ in (3, -1, 0x7fffffff):
        case = build_case(rng, nrng, 16, 16, [('cover', 'ones', 1, typ)])
        forwarded(case, 1, f'type {typ}')
    for typ in (3, -1):                       # no triangles: the stock code never reads the type
        case = build_case(rng, nrng, 16, 16, [('band', 'ones', 0, typ), ('cover', 'ones', 1, 0)])
        forwarded(case, 0, f'empty type {typ}')
        compare(case, f'empty type {typ}')
    case = build_case(rng, nrng, 16, 16, [('cover', 'ones', 1, 0)])
    first, last, _ = struct.unpack_from('<QQQ', case.alignments[0].blob, 0)
    struct.pack_into('<QQQ', case.alignments[0].blob, 0, last, first, first)   # last < first
    forwarded(case, 1, 'reversed triangle vector')
    case = build_case(rng, nrng, 16, 16, [('cover', 'ones', 1, 0)])
    case.list.last = case.list.first + 4                                        # not a pointer count
    forwarded(case, 1, 'ragged alignment vector')
    print('PASS: stock assert, blocks with a side < 2, oversized blocks, unindexable alignment types, '
          'malformed vectors -- forwarded to the original untouched; an unread bad type stays fast')

    # 7. Pool reuse across threads: many concurrent calls, all bit-identical.
    cases = [build_case(rng, nrng, rng.choice([33, 65, 129, 257]),
                        rng.choice([33, 65, 129, 257]),
                        [(rng.choice(kinds), rng.choice(wkinds), 3, t) for t in (0, 1, 2)])
             for _ in range(16)]
    expected = [c.arena() for c in cases]
    for case, arena in zip(cases, expected):
        case.call(stock, arena)
    errors = []

    def worker(index):
        try:
            for _ in range(20):
                case = cases[index]
                arena = case.arena()
                case.call(stock, arena, fast=fast, base=IMAGE)
                if not np.array_equal(arena, expected[index]):
                    errors.append(index)
        except Exception as exc:                     # noqa: BLE001 - reported below
            errors.append(repr(exc))
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(len(cases))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors
    print(f'PASS: {len(threads)} threads x 20 calls sharing the scratch pool, all identical')

    # 8. Installer: prologue boundary, every verified region, refusal paths.
    from test_world_entry import Host, logtype, basetype, verifytype, hooktype
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    steal = list(dis.disasm(PROLOGUE, IMAGE + FUNC))
    assert sum(i.size for i in steal) == len(PROLOGUE) and len(steal) == 9
    for ins in steal:
        assert not ins.group(capstone.CS_GRP_JUMP) and not ins.group(capstone.CS_GRP_CALL)
        assert not any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                       for op in ins.operands)
    install = dll.BigmapTestInstallTerrainAlign
    install.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    expected_verify = [(FUNC, len(PROLOGUE))] + REGIONS + [(rva, 16) for rva, _ in VTABLES]
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
            if target != IMAGE + FUNC or steal_bytes != len(PROLOGUE) or not detour:
                errors.append('hook')
            output[0] = IMAGE + FUNC
            return failure != 'hook'
        host = Host(C.sizeof(Host), 1, log, None, None, None, getbase, None, verify, hook, None, None)
        ok = install(C.byref(host), failure == 'gog', failure != 'disabled')
        assert bool(ok) == (failure == 'none'), failure
        assert not errors, (failure, errors)
        wanted = [('verify', r, n) for r, n in expected_verify]
        if failure in ('disabled', 'gog'):
            assert events == []
        elif isinstance(failure, tuple):
            assert events == wanted[:failure[1] + 1], (failure, events)
        else:
            assert events == wanted + [('hook',)], events
    print(f'PASS: installer verifies the 21-byte prologue (9 whole instructions, no RIP/branches), '
          f'{sum(n for _, n in REGIONS)} bytes of body/constructor/resize/constants and both predicate '
          f'vtables at the running base; disabled, GOG, each mismatch and a hook failure refuse')

    # 9. Warm native microbenchmark (ctypes overhead included, identical for both).
    print(f'benchmark (257x257 block, per call, best of 5 rounds; {trapped} unused imports trapped):')
    scenarios = [
        ('no alignments', []),
        ('6 road strips', [('band', 'ramp', 3, 0), ('band', 'ramp', 3, 1), ('band', 'ones', 2, 2)]),
        ('block fully covered', [('cover', 'ramp', 1, 0), ('cover', 'ramp', 1, 2)]),
        ('120 small footprints', [('blob', 'random', 40, 0), ('blob', 'random', 40, 1),
                                  ('blob', 'random', 40, 2)]),
    ]
    for label, mix in scenarios:
        case = build_case(rng, nrng, 257, 257, mix)
        a1, a2 = case.arena(), case.arena()
        case.call(stock, a1)
        case.call(stock, a2, fast=fast, base=IMAGE)
        assert np.array_equal(a1, a2)
        touched = int(np.count_nonzero(a1 != case.heights))
        best = [1e9, 1e9]
        for _ in range(5):
            t = time.perf_counter()
            for _ in range(20):
                case.call(stock, a1)
            best[0] = min(best[0], (time.perf_counter() - t) / 20)
            t = time.perf_counter()
            for _ in range(20):
                case.call(stock, a2, fast=fast, base=IMAGE)
            best[1] = min(best[1], (time.perf_counter() - t) / 20)
        assert np.array_equal(a1, a2)
        print(f'  {label}: stock {best[0] * 1e6:8.1f} us, fast {best[1] * 1e6:8.1f} us, '
              f'speedup {best[0] / best[1]:.2f}x ({touched} of {case.n} samples changed)')


if __name__ == '__main__':
    main()
