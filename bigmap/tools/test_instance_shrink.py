"""Instance shrink (Steam 35924): byte guards, installer and execution tests.

The execution test runs the ORIGINAL publish_model_instances (0x3bc760), cell
grouping (0x3bdad0) with the engine's push_back growth, the grouped-map
destructor (0x3c21d0), the move-assign AddComponent ends in (0x1eef60), the
engine allocators (0x15f4c0 / 0x15f5c0) and vector destructors (0xabf20 /
0x25ad60) in Unicorn over host memory shared with the DLL. The DLL's shrink is
injected at the 0x3bc8d1 AddComponent call; malloc/free are the only emulated
imports that touch the heap, so every block is tracked from allocation to free.
No game process or installed file is touched. Build the DLL first.
"""
import ctypes as C
import math
import random
import struct
from pathlib import Path

import capstone
import pefile
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
                     UC_PROT_ALL, UC_PROT_READ, UC_PROT_WRITE)
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
EXE = r'C:\tools\bin\TransportFever2.exe'
BASE = 0x140000000
PUBLISH, ADD, CALL, RET = 0x3bc760, 0x1691c0, 0x3bc8d1, 0x3bc8d6
ALLOC = {0x18: 0x15f4c0, 0xc0: 0x15f5c0}
FREE = {0x18: 0xabf20, 0xc0: 0x25ad60}
MOVE_ASSIGN = 0x1eef60
SITES = [(0x3bc8bb, 27), (0x1691c0, 14), (0x3bc760, 15), (0x3b8f51, 5), (0x3b89e1, 5),
         (0x15f4c0, 94), (0x15f5c0, 94), (0xabf20, 122), (0x25ad60, 180),
         (0x2bf3abc, 5), (0x2bf41c0, 5), (0x2bf67b7, 6)]
STEALS = {0x3bc760: 15, 0x1691c0: 14}
STACK, HSTACK, STUBS, STOP = 0x100000000000, 0x100010000000, 0x100020000000, 0x100030000000
STACK_SIZE = 0x100000
# Internal publish callees that need engine state; everything else is original code.
SKIP = [0x23e4a60, 0x9c9d20, 0x9ca930, 0x23dca30, 0x9ca890, 0x145b20, 0x161450,
        0x161980, 0x23de760, 0x23e4e10]

k32 = C.WinDLL('kernel32', use_last_error=True)
k32.VirtualAlloc.restype = C.c_void_p
k32.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]


class Vec(C.Structure):
    _fields_ = [('first', C.c_uint64), ('last', C.c_uint64), ('end', C.c_uint64)]


ALLOCFN = C.CFUNCTYPE(C.c_uint64, C.c_uint64, C.c_uint64)
RELEASEFN = C.CFUNCTYPE(None, C.c_uint64)
ADDFN = C.CFUNCTYPE(None, C.c_uint64, C.c_uint64, C.c_uint64, C.c_uint64, C.c_uint64)
PUBLISHFN = C.CFUNCTYPE(None, C.c_uint64, C.c_uint64, C.c_uint64, C.c_float)


class Engine(C.Structure):
    _fields_ = [('allocateThin', ALLOCFN), ('allocateFat', ALLOCFN),
                ('releaseThin', RELEASEFN), ('releaseFat', RELEASEFN), ('add', ADDFN)]


class Emulator:
    def __init__(self, pe):
        self.pe = pe
        self.image = pe.OPTIONAL_HEADER.SizeOfImage
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.heap_size = 64 << 20
        self.heap = k32.VirtualAlloc(None, self.heap_size, 0x3000, 0x04)
        assert self.heap and self.heap % 0x1000 == 0
        for lo, hi in [(BASE, BASE + self.image), (STACK, STOP + 0x1000)]:
            assert self.heap + self.heap_size <= lo or self.heap >= hi, 'heap overlaps fixture'
        self.uc.mem_map_ptr(self.heap, self.heap_size, UC_PROT_READ | UC_PROT_WRITE, self.heap)
        self.scratch = self.heap + 0x100      # 24-byte vector copies for the destructors
        self.bump = self.heap + 0x1000
        for addr in (STACK, HSTACK):
            self.uc.mem_map(addr, STACK_SIZE)
        self.uc.mem_map(STOP, 0x1000)
        self.live, self.mallocs, self.frees, self.errors = {}, [], [], []
        self.mapped = set()
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self._imports()
        self.intercept = None
        self.uc.hook_add(UC_HOOK_CODE, self._add_entry, begin=BASE + ADD, end=BASE + ADD)
        self.entity = 100
        for rva in SKIP:
            self.uc.hook_add(UC_HOOK_CODE, self._skip, rva, BASE + rva, BASE + rva)

    # ---- memory ----
    def q(self, addr):
        return struct.unpack('<Q', self.uc.mem_read(addr, 8))[0]

    def raw(self, size):
        """Fixture memory in the shared heap; never passed to free."""
        addr = (self.bump + 15) & ~15
        self.bump = addr + size + 16
        assert self.bump < self.heap + self.heap_size
        C.memset(addr, 0, size)
        return addr

    def _map_page(self, page):
        if page in self.mapped:
            return
        try:
            data = self.pe.get_data(page, 0x1000)
        except pefile.PEFormatError:
            data = b''
        self.uc.mem_map(BASE + page, 0x1000, UC_PROT_ALL)
        self.uc.mem_write(BASE + page, data[:0x1000].ljust(0x1000, b'\0'))
        self.mapped.add(page)
        for slot, stub in self.iat.get(page, []):
            self.uc.mem_write(BASE + slot, struct.pack('<Q', stub))

    def _unmapped(self, uc, access, address, size, value, data):
        if not BASE <= address < BASE + self.image:
            self.errors.append(f'unmapped access {address:#x}')
            return False
        for page in range((address - BASE) & ~0xfff, address - BASE + size, 0x1000):
            self._map_page(page)
        return True

    # ---- imports ----
    def _imports(self):
        self.pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        self.iat, self.stub_names = {}, {}
        index = 0
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for symbol in entry.imports:
                slot = symbol.address - self.pe.OPTIONAL_HEADER.ImageBase
                stub = STUBS + index * 8
                name = symbol.name.decode() if symbol.name else f'ord{symbol.ordinal}'
                self.iat.setdefault(slot & ~0xfff, []).append((slot, stub))
                self.stub_names[stub] = name
                index += 1
        self.uc.mem_map(STUBS, (index * 8 + 0xfff) & ~0xfff)
        self.uc.mem_write(STUBS, b'\xcc' * index * 8)
        self.uc.hook_add(UC_HOOK_CODE, self._stub, begin=STUBS, end=STUBS + index * 8)

    def ret(self, rax=None):
        rsp = self.uc.reg_read(UC_X86_REG_RSP)
        self.uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        self.uc.reg_write(UC_X86_REG_RIP, self.q(rsp))
        if rax is not None:
            self.uc.reg_write(UC_X86_REG_RAX, rax)

    def fail(self, message):
        self.errors.append(message)
        self.uc.emu_stop()

    def _stub(self, uc, address, size, data):
        name = self.stub_names.get(address)
        rcx, rdx, r8 = (uc.reg_read(r) for r in (UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8))
        if name == 'malloc':
            addr = (self.bump + 15) & ~15
            self.bump = addr + max(rcx, 1) + 16
            assert self.bump < self.heap + self.heap_size
            C.memset(addr, 0xa5, rcx)       # engine code must not rely on zeroed blocks
            self.live[addr] = rcx
            self.mallocs.append((addr, rcx))
            self.ret(addr)
        elif name == 'free':
            if rcx:
                if rcx not in self.live:
                    return self.fail(f'free of a block malloc never returned: {rcx:#x}')
                del self.live[rcx]
                self.frees.append(rcx)
            self.ret(0)
        elif name in ('memmove', 'memcpy'):
            uc.mem_write(rcx, bytes(uc.mem_read(rdx, r8)))
            self.ret(rcx)
        elif name == 'memset':
            uc.mem_write(rcx, bytes([rdx & 0xff]) * r8)
            self.ret(rcx)
        elif name == 'floorf':
            value = struct.unpack('<f', struct.pack('<I', uc.reg_read(UC_X86_REG_XMM0) & 0xffffffff))[0]
            uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<I', struct.pack('<f', math.floor(value)))[0])
            self.ret()
        else:
            self.fail(f'unexpected import {name}')

    def _skip(self, uc, address, size, rva):
        rcx, rdx = uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_RDX)
        if rva == 0x9c9d20:            # CalcBoundingBox: return a box by value
            uc.mem_write(rcx, struct.pack('<6f', 0, 0, 0, 1, 1, 1))
            return self.ret(rcx)
        if rva == 0x23dca30:           # CreateEntity(engine, &entity)
            self.entity += 1
            uc.mem_write(rdx, struct.pack('<i', self.entity))
        self.ret(rcx)

    def _add_entry(self, uc, address, size, data):
        if self.intercept is not None:
            self.intercept = True
            uc.emu_stop()

    # ---- calls ----
    def call(self, rva, rcx=0, rdx=0, r8=0, r9=0):
        rsp = HSTACK + STACK_SIZE - 0x1008
        self.uc.mem_write(rsp, struct.pack('<Q', STOP))
        for reg, value in [(UC_X86_REG_RCX, rcx), (UC_X86_REG_RDX, rdx), (UC_X86_REG_R8, r8),
                           (UC_X86_REG_R9, r9), (UC_X86_REG_RSP, rsp)]:
            self.uc.reg_write(reg, value)
        self.uc.emu_start(BASE + rva, STOP, count=20_000_000)
        assert not self.errors, self.errors
        assert self.uc.reg_read(UC_X86_REG_RIP) == STOP
        return self.uc.reg_read(UC_X86_REG_RAX)

    def publish(self, accumulator, handle_add):
        rsp = STACK + STACK_SIZE - 0x2008
        self.uc.mem_write(rsp, struct.pack('<Q', STOP))
        self.uc.reg_write(UC_X86_REG_RSP, rsp)
        self.uc.reg_write(UC_X86_REG_RCX, self.raw(0x100))
        self.uc.reg_write(UC_X86_REG_RDX, self.raw(0x100))
        self.uc.reg_write(UC_X86_REG_R8, accumulator)
        self.uc.reg_write(UC_X86_REG_XMM3, struct.unpack('<I', struct.pack('<f', 64.0))[0])
        start = BASE + PUBLISH
        while True:
            self.intercept = False
            self.uc.emu_start(start, STOP, count=50_000_000)
            assert not self.errors, self.errors
            if not self.intercept:
                break
            self.intercept = None
            rsp = self.uc.reg_read(UC_X86_REG_RSP)
            returned = self.q(rsp)
            assert returned == BASE + RET, hex(returned)
            args = tuple(self.uc.reg_read(r) for r in (UC_X86_REG_RCX, UC_X86_REG_RDX,
                                                       UC_X86_REG_R8, UC_X86_REG_R9)) + (self.q(rsp + 0x28),)
            context = self.uc.context_save()
            handle_add(args)
            self.uc.context_restore(context)
            self.uc.reg_write(UC_X86_REG_RSP, rsp + 8)
            start = returned
        self.intercept = None
        assert self.uc.reg_read(UC_X86_REG_RIP) == STOP


def vec(emu, addr):
    first, last, end = struct.unpack('<QQQ', emu.uc.mem_read(addr, 24))
    return first, last, end


def make_accumulator(emu, rng):
    trees, assets = [], []
    tree_cells = [(rng.randrange(40), rng.randrange(40)) for _ in range(70)]
    counts = [rng.randrange(1, 60) for _ in tree_cells]
    tree_cells += [(41, 41), (42, 42), (43, 43), (44, 44)]
    counts += [400, 1, 4, 42]          # large-block header; no slack at 1, 4 and 42
    for (cx, cy), n in zip(tree_cells, counts):
        for _ in range(n):
            trees.append(struct.pack('<i5f', rng.randrange(1, 900), cx * 64 + rng.random() * 63.5,
                                     cy * 64 + rng.random() * 63.5, rng.random() * 50,
                                     rng.random() * 6.28, 0.5 + rng.random()))
    # (41, 41) is the kept (poisoned) fat list; (46, 46) exercises the large fat block path.
    asset_cells = [(rng.randrange(40), rng.randrange(40)) for _ in range(40)] + [(41, 41), (45, 45), (46, 46)]
    asset_counts = [rng.randrange(1, 9) for _ in range(40)] + [40, 7, 30]
    for (cx, cy), n in zip(asset_cells, asset_counts):
        for _ in range(n):
            m = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0,
                 cx * 64 + rng.random() * 63.5, cy * 64 + rng.random() * 63.5, rng.random() * 40, 1.0]
            record = bytearray(0xc0)
            struct.pack_into('<i16f', record, 0, rng.randrange(1, 900), *m)
            struct.pack_into('<i', record, 0x88, -1)
            assets.append(bytes(record))
    rng.shuffle(trees)
    rng.shuffle(assets)
    acc = emu.raw(0x38)
    thin = emu.raw(len(trees) * 0x18)
    fat = emu.raw(len(assets) * 0xc0)
    C.memmove(thin, b''.join(trees), len(trees) * 0x18)
    C.memmove(fat, b''.join(assets), len(assets) * 0xc0)
    C.memmove(acc, struct.pack('<6Q', thin, thin + len(trees) * 0x18, thin + len(trees) * 0x18,
                               fat, fat + len(assets) * 0xc0, fat + len(assets) * 0xc0), 48)

    def key(x, y):
        return (math.floor(x / 64.0), math.floor(y / 64.0))
    expected = {}
    for t in trees:
        _, x, y = struct.unpack_from('<iff', t)
        expected.setdefault(key(x, y), [b'', b''])[0] += t
    for a in assets:
        x, y = struct.unpack_from('<ff', a, 0x34)
        expected.setdefault(key(x, y), [b'', b''])[1] += a
    return acc, expected


def run(pe, dll, shrink):
    emu = Emulator(pe)
    acc, expected = make_accumulator(emu, random.Random(35924))
    cells, components, calls, forwarded = {}, [], [], []
    allocations, releases = [], []
    totals = [0, 0, 0, 0]
    kept_key = (41, 41)
    poisoned = []

    def move_into_component(list_ptr):
        slot = emu.raw(0x38)
        emu.call(MOVE_ASSIGN, slot, list_ptr)
        components.append(slot)
        return slot

    def allocate(kind):
        def fn(vector, count):
            before = len(emu.mallocs)
            ptr = emu.call(ALLOC[kind], vector, count)
            allocations.append((kind, count, ptr, emu.mallocs[before:]))
            return ptr
        return fn

    def release(kind):
        def fn(pointer):
            data = C.string_at(pointer, 24)
            C.memmove(emu.scratch, data, 24)
            before = len(emu.frees)
            emu.call(FREE[kind], emu.scratch)
            C.memmove(pointer, C.string_at(emu.scratch, 24), 24)
            releases.append((kind, struct.unpack('<QQQ', data), emu.frees[before:]))
        return fn

    def original(*args):
        forwarded.append(args)
        move_into_component(args[2])

    engine = Engine(ALLOCFN(allocate(0x18)), ALLOCFN(allocate(0xc0)),
                    RELEASEFN(release(0x18)), RELEASEFN(release(0xc0)), ADDFN(original))

    def handle_add(args):
        rcx, rdx, r8, r9, a5 = args
        node = r8 - 0x18
        key = struct.unpack('<ii', emu.uc.mem_read(node + 0x10, 8))
        thin, fat = vec(emu, r8), vec(emu, r8 + 0x18)
        assert emu.uc.mem_read(r8 + 0x30, 1) == b'\0'
        if shrink and key == kept_key:
            block = emu.call(ALLOC[0x18], 0, 1)   # a real engine block (freed by 0x151870 later)
            emu.uc.mem_write(fat[0] + 0x90, struct.pack('<QQQ', block, block + 4, block + 4))
            poisoned.append((key, block))
        cells[key] = (thin, fat)
        calls.append(args)
        if shrink:
            out = (C.c_uint64 * 4)()
            dll.BigmapTestInstanceShrinkAdd(C.byref(engine), rcx, rdx, r8, r9, a5, out)
            for i in range(4):
                totals[i] += out[i]
        else:
            original(rcx, rdx, r8, r9, a5)

    emu.publish(acc, handle_add)
    assert len(calls) == len(expected) == len(components) == len(forwarded)
    assert [c[:4] for c in calls] == [f[:4] for f in forwarded]
    assert [c[4] for c in calls] == [f[4] for f in forwarded]
    result = {}
    for (key, _), slot in zip(cells.items(), components):
        thin, fat = vec(emu, slot), vec(emu, slot + 0x18)
        data = (bytes(emu.uc.mem_read(thin[0], thin[1] - thin[0])) if thin[0] else b'',
                bytes(emu.uc.mem_read(fat[0], fat[1] - fat[0])) if fat[0] else b'')
        if poisoned and key == poisoned[0][0]:
            data = (data[0], data[1][:0x90] + bytes(24) + data[1][0xa8:])
        # The engine copy constructor 0x1e5070 never copies fat padding +0x8c..+0x8f,
        # so grouped records carry heap bytes there; stock and shrunk still compare in full.
        def unpadded(fat_bytes):
            return b''.join(fat_bytes[i:i + 0x8c] + bytes(4) + fat_bytes[i + 0x90:i + 0xc0]
                            for i in range(0, len(fat_bytes), 0xc0))
        assert [data[0], unpadded(data[1])] == expected[key], key
        result[key] = (data, thin, fat, emu.uc.mem_read(slot + 0x30, 1))
    # The grouped map is gone: only the moved component buffers and poison remain live.
    owned = set()
    for slot in components:
        for off in (0, 0x18):
            first, last, end = vec(emu, slot + off)
            if first:
                owned.add(first if end - first < 0x1000 else emu.q(first - 8))
    owned.update(block for _, block in poisoned)
    assert set(emu.live) == owned, (len(emu.live), len(owned))
    for slot in components:               # component destruction: engine destructors
        emu.call(FREE[0x18], slot)
        emu.call(FREE[0xc0], slot + 0x18)
    assert not emu.live, f'{len(emu.live)} blocks leaked'
    return result, cells, calls, allocations, releases, totals, poisoned


def check_allocations(allocations, releases, cells_before):
    for kind, count, ptr, mallocs in allocations:
        assert len(mallocs) == 1, mallocs
        raw, size = mallocs[0]
        nbytes = count * kind
        if nbytes < 0x1000:
            assert (ptr, size) == (raw, nbytes)
        else:
            assert size == nbytes + 0x27 and ptr == (raw + 0x27) & ~0x1f
            assert struct.unpack('<Q', C.string_at(ptr - 8, 8))[0] == raw
    for kind, (first, last, end), frees in releases:
        assert last == first, 'released copy must carry no records'
        expected_raw = first if end - first < 0x1000 else struct.unpack('<Q', C.string_at(first - 8, 8))[0]
        if end - first >= 0x1000:
            assert first - 8 - expected_raw <= 0x1f
        assert frees == [expected_raw], (frees, hex(first))
    old_buffers = {(k, r[0]) for k, r, _ in releases}
    expected = {(0x18, t[0]) for t, f in cells_before.values() if t[1] != t[2]}
    expected |= {(0xc0, f[0]) for key, (t, f) in cells_before.items() if f[1] != f[2] and key != (41, 41)}
    assert old_buffers == expected


def execution_tests(pe, dll):
    stock, stock_cells, stock_calls, stock_allocs, stock_releases, _, _ = run(pe, dll, False)
    assert not stock_allocs and not stock_releases
    shrunk, cells, calls, allocations, releases, totals, poisoned = run(pe, dll, True)
    assert [c[4] & 0xff for c in calls] == [0] * len(calls) and all(c[3] & 0xff == 0 for c in calls)
    assert list(stock) == list(shrunk), 'cell order changed'
    slack_cells, slack_bytes = 0, 0
    for key, (data, thin, fat, dynamic) in stock.items():
        s_data, s_thin, s_fat, s_dynamic = shrunk[key]
        assert dynamic == s_dynamic == b'\0'
        if key == poisoned[0][0]:
            assert s_data[0] == data[0] and s_data[1][:0x90] == data[1][:0x90]
        else:
            assert s_data == data, key
        trimmed = (thin[2] - thin[1])
        assert s_thin[1] == s_thin[2], ('thin not at size', key)
        if key != poisoned[0][0]:
            trimmed += fat[2] - fat[1]
            assert s_fat[1] == s_fat[2], ('fat not at size', key)
        else:
            assert s_fat[2] - s_fat[1] == fat[2] - fat[1] > 0, 'kept fat list must keep capacity'
        slack_cells += trimmed > 0
        slack_bytes += trimmed
    assert slack_cells > 50 and slack_bytes > 0
    assert totals == [slack_bytes, slack_cells, len(stock), 1], (totals, slack_bytes, slack_cells)
    check_allocations(allocations, releases, cells)
    large = [a for a in allocations if a[0] * a[1] >= 0x1000]
    assert {a[0] for a in large} == {0x18, 0xc0}, 'both large-block paths exercised'
    print(f'PASS: original publish/group/move/destructor instructions; {len(stock)} cells identical '
          f'to stock, {slack_cells} trimmed by {slack_bytes} bytes to size == capacity; '
          f'{len(allocations)} engine allocations and {len(releases)} engine frees balanced; '
          'kept fat list with a live embedded vector; AddComponent received identical arguments')


def static_tests(pe):
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    call = next(dis.disasm(pe.get_data(CALL, 5), BASE + CALL))
    assert call.mnemonic == 'call' and int(call.op_str, 16) == BASE + ADD
    setup = list(dis.disasm(pe.get_data(0x3bc8bb, 27), BASE + 0x3bc8bb))
    assert [f'{i.mnemonic} {i.op_str}' for i in setup] == [
        'mov byte ptr [rsp + 0x20], 0', 'xor r9d, r9d', 'lea r8, [rbx + 0x18]',
        'lea rdx, [rsp + 0x38]', 'mov rcx, qword ptr [rsp + 0x30]', 'call 0x1401691c0']
    body = list(dis.disasm(pe.get_data(PUBLISH, 0x279), BASE + PUBLISH))
    targets = [int(i.op_str, 16) - BASE for i in body if i.mnemonic == 'call' and i.op_str.startswith('0x')]
    assert targets.count(ADD) == 1 and targets[0] == 0x3bdad0
    for site, name in [(0x3b8f51, 'create_trees'), (0x3b89e1, 'create_assets')]:
        ins = next(dis.disasm(pe.get_data(site, 5), BASE + site))
        assert int(ins.op_str, 16) == BASE + PUBLISH, name
    print('PASS: call bytes, arguments (flags 0/0, r8 = cell+0x18), single call in publish, both callers')


def installer_tests(pe, dll):
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    logtype = C.CFUNCTYPE(None, C.c_char_p)
    basetype = C.CFUNCTYPE(C.c_size_t)
    verifytype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    hooktype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_int, C.POINTER(C.c_void_p))

    class Host(C.Structure):
        _fields_ = [('size', C.c_uint32), ('abi', C.c_uint32), ('log', logtype),
                    ('cfgInt', C.c_void_p), ('cfgBool', C.c_void_p), ('cfgStr', C.c_void_p),
                    ('base', basetype), ('buildOk', C.c_void_p), ('verify', verifytype),
                    ('hook', hooktype), ('patch', C.c_void_p), ('dataDir', C.c_void_p)]

    install = dll.BigmapTestInstallInstanceShrink
    install.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    install.restype = C.c_int
    events, errors, logs, detours, received = [], [], [], {}, []

    @PUBLISHFN
    def publish_original(*args):
        received.append(('publish', args))

    @ADDFN
    def add_original(*args):
        received.append(('add', args))

    @logtype
    def log(fmt):
        logs.append(fmt.decode())

    @basetype
    def base():
        return BASE

    @verifytype
    def verify(rva, ptr, size):
        events.append(('verify', rva))
        code = C.string_at(ptr, size)
        if (rva, size) not in SITES or code != pe.get_data(rva, size):
            errors.append(('bytes', hex(rva)))
        if rva in STEALS:
            ins = list(dis.disasm(code, BASE + rva))
            if sum(i.size for i in ins) != size or size != STEALS[rva]:
                errors.append('split instruction')
            for i in ins:
                if i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_JUMP):
                    errors.append('relative control flow')
                if any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                       for op in i.operands):
                    errors.append('RIP relative')
        return failure != ('verify', rva)

    @hooktype
    def hook(target, detour, steal, output):
        rva = target - BASE
        events.append(('hook', rva))
        if STEALS.get(rva) != steal:
            errors.append(('steal', hex(rva), steal))
        detours[rva] = detour
        output[0] = C.cast(publish_original if rva == PUBLISH else add_original, C.c_void_p).value
        return failure != ('hook', rva)

    host = Host(C.sizeof(Host), 1, log, None, None, None, base, None, verify, hook, None, None)
    verifies = [('verify', rva) for rva, _ in SITES]
    cases = [('disabled', 0), ('gog', 0)] + [('verify', rva) for rva, _ in SITES] + \
            [('hook', PUBLISH), ('hook', ADD), ('none', 0)]
    for failure in cases:
        events.clear(); errors.clear(); logs.clear()
        ok = install(C.byref(host), failure[0] == 'gog', failure[0] != 'disabled')
        assert not errors, errors
        assert bool(ok) == (failure[0] == 'none'), failure
        if failure[0] == 'disabled':
            assert not events and not logs
        elif failure[0] == 'gog':
            assert not events and len(logs) == 1
        elif failure[0] == 'verify':
            assert events == verifies[:verifies.index(failure) + 1] and len(logs) == 1
        else:
            hooks = [('hook', PUBLISH), ('hook', ADD)]
            upto = hooks if failure[0] == 'none' else hooks[:hooks.index(failure) + 1]
            assert events == verifies + upto and len(logs) == 1, (failure, events, logs)
    assert logs[0].startswith('instance shrink:')

    # The installed AddComponent detour, called from anywhere but 0x3bc8d6, only forwards.
    thin = (C.c_ubyte * 48)()
    lst = (C.c_uint64 * 7)(C.addressof(thin), C.addressof(thin) + 24, C.addressof(thin) + 48, 0, 0, 0, 0)
    before = bytes(lst)
    add = ADDFN(detours[ADD])
    received.clear()
    add(0x1111, 0x2222, C.addressof(lst), 0xfedcba9876543200, 0x0123456789abcd00)
    assert received == [('add', (0x1111, 0x2222, C.addressof(lst), 0xfedcba9876543200, 0x0123456789abcd00))]
    assert bytes(lst) == before
    logs.clear(); received.clear()
    assert detours[PUBLISH]
    invoke = dll.BigmapTestInvokeInstanceShrinkPublish
    invoke.argtypes = [C.POINTER(Host), C.c_uint64, C.c_uint64, C.c_uint64, C.c_float]
    invoke.restype = None
    invoke(C.byref(host), 0x3333, 0x4444, 0x5555, 64.0)
    assert received == [('publish', (0x3333, 0x4444, 0x5555, 64.0))]
    assert len(logs) == 1 and logs[0].startswith('instance shrink: %s trimmed')
    print('PASS: installer guards (disabled, GOG, 12 byte sites, both hooks), steal boundaries; '
          'other AddComponent callers pass through untouched; publish forwards and logs one line')


def main():
    pe = pefile.PE(EXE, fast_load=True)
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    dll.BigmapTestInstanceShrinkAdd.restype = None
    dll.BigmapTestInstanceShrinkAdd.argtypes = [C.POINTER(Engine), C.c_uint64, C.c_uint64, C.c_uint64,
                                                 C.c_uint64, C.c_uint64, C.POINTER(C.c_uint64)]
    for rva, size in SITES:
        assert len(pe.get_data(rva, size)) == size
    static_tests(pe)
    installer_tests(pe, dll)
    execution_tests(pe, dll)


if __name__ == '__main__':
    main()
