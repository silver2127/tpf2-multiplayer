"""Material-index cell ownership, release paths and Steam hook guards; no live game writes.

Evidence for every site: docs/material-grid-lifetime.md.
"""
import ctypes as C
from pathlib import Path
import pefile
import capstone
from test_world_entry import Host, logtype, basetype, verifytype, hooktype

ROOT = Path(__file__).resolve().parents[1]
N = 260 * 260 + 1


class Vec(C.Structure):
    _fields_ = [('first', C.c_void_p), ('last', C.c_void_p), ('end', C.c_void_p)]


Resize = C.CFUNCTYPE(None, C.POINTER(Vec), C.c_size_t)
Assign = C.CFUNCTYPE(None, C.POINTER(Vec), C.c_void_p, C.c_void_p)
InternDestroy = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Vec))


def main():
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    assert dll.BigmapTestMaterialInit()
    resize = dll.BigmapTestMaterialResize; resize.argtypes = [C.POINTER(Vec), C.c_size_t, C.c_int, Resize]
    assign = dll.BigmapTestMaterialAssign; assign.argtypes = [C.POINTER(Vec), C.c_void_p, C.c_void_p, Assign, Resize]
    destroy = dll.BigmapTestMaterialInternDestroy; destroy.argtypes = [C.c_void_p, C.POINTER(Vec), InternDestroy]
    cells = dll.BigmapTestMaterialGridCells; cells.argtypes = [C.c_void_p]
    evict = dll.BigmapTestMaterialEvict; evict.argtypes = [C.c_void_p]
    counter = dll.BigmapTestMaterialCounter; counter.argtypes = [C.c_int]; counter.restype = C.c_int64
    calls = []; backings = []

    @Resize
    def stock_resize(v, n):
        calls.append(('resize', n)); b = C.create_string_buffer(max(n, 1)); backings.append(b)
        v[0] = Vec(C.addressof(b), C.addressof(b) + n, C.addressof(b) + max(n, 1))

    @Assign
    def stock_assign(v, first, last):
        n = last - first; calls.append(('assign', n))
        if v[0].end - v[0].first < n:
            b = C.create_string_buffer(n); backings.append(b); v[0] = Vec(C.addressof(b), C.addressof(b), C.addressof(b) + n)
        C.memmove(v[0].first, first, n); v[0].last = v[0].first + n

    @InternDestroy
    def stock_destroy(grid, v):
        calls.append(('intern_destroy',))

    # Only the InternCreate call (eligible) with exactly 67,601 bytes on an empty vector is owned.
    cell = Vec(); resize(C.byref(cell), N, 1, stock_resize)
    assert not calls and cell.last - cell.first == N and cell.end == cell.last
    owned = cell.first
    assert C.string_at(owned, N) == bytes(N)                       # zero-filled like the stock grow path
    for n, eligible in [(N, 0), (1157, 1), (N - 1, 1)]:              # other callers / ambient cells / sizes
        v = Vec(); before = len(calls); resize(C.byref(v), n, eligible, stock_resize)
        assert len(calls) == before + 1
    pattern = bytes((20 + (i // 260 + i % 260 // 11) % 5) for i in range(N - 1)) + b'V'
    C.memmove(owned, pattern, N)
    assert evict(owned) and C.string_at(owned, N) == pattern         # fault restore at the same address

    # FinishBox copies a same-size job cache into the owned cell: in place, no migration.
    cache = C.create_string_buffer(bytes(reversed(pattern[:-1])) + b'V', N)
    before = len(calls)
    assign(C.byref(cell), C.addressof(cache), C.addressof(cache) + N, stock_assign, stock_resize)
    assert cell.first == owned and calls[before:] == [('assign', N)] and counter(1) == 0
    assert C.string_at(owned, N) == C.string_at(cache, N)

    # InternDestroy on an owned cell: stock accounting, release, null triple; the original is not called.
    grid = (C.c_uint8 * 0x90)()
    C.c_int32.from_buffer(grid, 0x0c).value = 260
    C.c_int32.from_buffer(grid, 0x10).value = 260
    C.c_int32.from_buffer(grid, 0x60).value = 1000000
    before = len(calls)
    destroy(C.addressof(grid), C.byref(cell), stock_destroy)
    assert (cell.first, cell.last, cell.end) == (None, None, None) and len(calls) == before
    assert C.c_int32.from_buffer(grid, 0x60).value == 1000000 - 67600 and counter(0) == 1
    # A heap cell (ambient or unowned) goes to the original InternDestroy.
    plain = Vec(); resize(C.byref(plain), 1157, 1, stock_resize)
    destroy(C.addressof(grid), C.byref(plain), stock_destroy)
    assert calls[-1] == ('intern_destroy',)

    # Unexpected growth: migrate to the heap with bytes preserved, then run the stock operation.
    cell = Vec(); resize(C.byref(cell), N, 1, stock_resize); owned = cell.first
    C.memmove(owned, pattern, N); assert evict(owned)
    big = C.create_string_buffer(N + 500)
    assign(C.byref(cell), C.addressof(big), C.addressof(big) + N + 500, stock_assign, stock_resize)
    assert cell.first != owned and counter(1) == 1 and calls[-2][0] == 'resize' and calls[-1] == ('assign', N + 500)
    cell2 = Vec(); resize(C.byref(cell2), N, 1, stock_resize); owned2 = cell2.first
    C.memmove(owned2, pattern, N)
    resize(C.byref(cell2), N + 10, 1, stock_resize)
    assert cell2.first != owned2 and C.string_at(cell2.first, N) == pattern and counter(1) == 2
    # resize(0) on an owned cell (InternDestroy's tail) keeps ownership and capacity.
    cell3 = Vec(); resize(C.byref(cell3), N, 1, stock_resize); owned3 = cell3.first
    resize(C.byref(cell3), 0, 0, stock_resize)
    assert cell3.first == owned3 and cell3.last == owned3

    # Grid destructor safety net: owned cells still live are released and nulled; heap cells untouched.
    grid2 = (C.c_uint8 * 0x90)()
    cellarr = (C.c_uint8 * (4 * 0x48))()                              # 0x48-byte cells, payload at +0
    C.memmove(C.addressof(cellarr), C.addressof(cell3), 0x18)
    C.memmove(C.addressof(cellarr) + 0x48, C.addressof(plain), 0x18)
    C.c_uint64.from_buffer(grid2, 0x78).value = C.addressof(cellarr)
    C.c_uint64.from_buffer(grid2, 0x80).value = C.addressof(cellarr) + 4 * 0x48
    cells(C.addressof(grid2))
    assert C.string_at(C.addressof(cellarr), 0x18) == bytes(0x18) and counter(2) == 1
    assert Vec.from_buffer(cellarr, 0x48).first == plain.first

    # Steam byte guards, boundaries and install ordering.
    pe = pefile.PE(r'C:\tools\bin\TransportFever2.exe', fast_load=True)
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); dis.detail = True
    events = []; errors = []; failure = None

    @logtype
    def log(fmt): pass

    @basetype
    def base(): return 0x140000000

    @verifytype
    def verify(rva, p, n):
        events.append(('verify', rva)); code = C.string_at(p, n)
        if code != pe.get_data(rva, n): errors.append(('bytes', hex(rva)))
        ins = list(dis.disasm(code, 0x140000000 + rva))
        if sum(i.size for i in ins) != n: errors.append(('boundary', hex(rva)))
        if rva in (0x1d5830, 0x330350, 0x32a6f0, 0x1b3d90):
            if n < 14: errors.append(('steal', hex(rva)))
            for i in ins:
                if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL): errors.append(('branch', hex(rva)))
                if any(o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP for o in i.operands):
                    errors.append(('rip', hex(rva)))
        if rva == 0x3303de:
            call = ins[-1]
            if call.mnemonic != 'call' or int(call.op_str, 16) != 0x1401d5830 or call.address + call.size != 0x1403303e6:
                errors.append(('callsite', call.mnemonic, call.op_str))
        return failure != ('verify', rva)

    @hooktype
    def hook(target, detour, n, out):
        events.append(('hook', target - 0x140000000)); out[0] = 0x1234
        return failure != ('hook', target - 0x140000000)

    host = Host(C.sizeof(Host), 1, log, None, None, None, base, None, verify, hook, None, None)
    install = dll.BigmapTestInstallMaterial; install.argtypes = [C.POINTER(Host), C.c_int, C.c_int, C.c_int, C.c_int]
    # hot 0 and warm -1 now mean 'size from installed RAM', so -2 is the invalid case.
    for args in [(1, 1, 256, 1024), (0, 0, 256, 1024), (0, 2, 256, 1024), (0, 1, 63, 1024), (0, 1, 8193, 1024), (0, 1, 256, -2), (0, 1, 256, 8193)]:
        events.clear(); assert not install(C.byref(host), *args); assert not events
    order = [0x330350, 0x32a6f0, 0x1b3d90, 0x1d5830]
    for stage, rv in [('verify', 0x1d5830), ('verify', 0x3303de), ('verify', 0x330350), ('verify', 0x32a6f0),
                      ('verify', 0x1b3d90)] + [('hook', r) for r in order]:
        failure = (stage, rv); events.clear(); assert not install(C.byref(host), 0, 1, 256, 1024)
        hooks = [v for s, v in events if s == 'hook']
        if stage == 'verify': assert not hooks
        else: assert hooks == order[:len(hooks)] and hooks[-1] == rv and 0x1d5830 not in hooks[:-1]
    assert not errors, errors
    print('PASS: material cell ownership (InternCreate-only, 67,601 bytes), fault restore, in-place assign, '
          'InternDestroy accounting/release, grid-dtor safety net, growth migration, Steam byte guards and hook order')


if __name__ == '__main__':
    main()
