#!/usr/bin/env python3
"""Offline depth-12/13 emulation against the native Linux game ELF (read-only).

Usage: test_octree_depth_elf.py GAME_ELF      (requires: pip install unicorn)

Runs the ORIGINAL Linux machine code of the octree descent loop (0x16aa040)
and of the renderer's inlined CalcOctreeLevel (0x13ec528..0x13ec555) in
Unicorn, with the site patches and stubs taken byte-for-byte from
linux/octree_depth.h. Only operator new is stubbed. This validates insertion,
IDs and the decoder; it is not a running renderer or a live game.
"""
import itertools
import random
import re
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'linux/octree_depth.h').read_text()
BUILD_ID = '3a0e156390b0e6f1e372051c24802c8493ae454a'
BASE = 0x100000000
DESCEND, ALLOC = 0x16aa040, 0x6dbce0
NEVER = {0x16a6180: 'node destructor', 0x16a6300: 'child-array destructor'}
ROOT_SITE, ROOT_BYTES = 0xa84234, bytes.fromhex('f30f1005487c4003be0a000000')
LEVEL_ENTRY = 0x13ec528
HEAP, STACK, STUB, STOP = 0x20000000, 0x30000000, 0x40000000, 0x50000000


def array(name):
    m = re.search(r'constexpr uint8_t %s\[\d+\]=\{(.*?)\};' % name, HEADER, re.S)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    return bytes(int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', body))


def const(name):
    return int(re.search(r'\b%s=(0x[0-9a-fA-F]+|\d+)' % name, HEADER).group(1), 0)


def load(path):
    b = Path(path).read_bytes()
    assert b[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', b, 16)[0] == 3
    phoff = struct.unpack_from('<Q', b, 32)[0]
    size, count = struct.unpack_from('<HH', b, 54)
    image, ident = None, None
    for i in range(count):
        kind, _, off, va, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', b, phoff + i * size)
        if kind == 1 and va == 0:
            image = b[off:off + filesz]
        if kind == 4:
            end = off + filesz
            while off + 12 <= end:
                namesz, descsz, ntype = struct.unpack_from('<III', b, off); off += 12
                name = b[off:off + namesz]; off += (namesz + 3) & ~3
                desc = b[off:off + descsz]; off += (descsz + 3) & ~3
                if name == b'GNU\0' and ntype == 3: ident = desc.hex()
    assert ident == BUILD_ID, 'unsupported game build'
    return image


def main():
    image = load(sys.argv[1])
    child_site, child_back = const('ChildSite'), const('ChildBack')
    level_site, level_back = const('LevelSite'), const('LevelBack')
    child_bytes, level_bytes = array('ChildBytes'), array('LevelBytes')
    child_stub, level_stub = array('ChildStub'), array('LevelStub')
    counters_at, child_back_at = const('ChildStubCounters'), const('ChildStubBack')
    level_back_at = const('LevelStubBack')
    assert len(child_stub) == 145 and len(level_stub) == 81
    assert image[child_site:child_site + 20] == child_bytes
    assert image[level_site:level_site + 37] == level_bytes
    assert image[ROOT_SITE:ROOT_SITE + 13] == ROOT_BYTES
    text = image[0x6de300:0x6de300 + 0x37adaa5]
    assert text.count(level_bytes) == 1 and text.count(child_bytes) == 1
    # Preceding context: test esi,esi; js <assert>; and the descent's octant tail.
    assert image[LEVEL_ENTRY:level_site] == bytes.fromhex('85f60f882e010000')
    assert level_stub[0x1c:0x1e] == b'\x85\xf6' and level_stub[0x1e:0x43] == level_bytes
    assert child_stub[0x7c:0x81] == child_bytes[:5] and child_stub[:10] == child_bytes[5:15]
    # Stock jmp at the end of the child step really returns to the loop head.
    assert child_site + 20 + struct.unpack_from('<i', child_bytes, 16)[0] == child_back

    def site_patch(length, stub):
        return b'\xff\x25\0\0\0\0' + struct.pack('<Q', stub) + b'\x90' * (length - 14)

    # ---- renderer level decoder -------------------------------------------
    def decode(index, patched=True):
        u = Uc(UC_ARCH_X86, UC_MODE_64)
        page = (BASE + LEVEL_ENTRY) & ~0xfff
        u.mem_map(page, 0x2000)
        u.mem_write(page, image[page - BASE:page - BASE + 0x2000])
        u.mem_map(STUB, 0x1000)
        stub = bytearray(level_stub)
        struct.pack_into('<Q', stub, level_back_at, BASE + level_back)
        u.mem_write(STUB, bytes(stub))
        if patched:
            u.mem_write(BASE + level_site, site_patch(37, STUB))
        u.reg_write(UC_X86_REG_RSI, index & 0xffffffff)
        u.reg_write(UC_X86_REG_RDX, 0x1234)
        u.emu_start(BASE + LEVEL_ENTRY, BASE + level_back, count=600)
        if u.reg_read(UC_X86_REG_RIP) != BASE + level_back:
            return None  # stock never terminates past level 10
        assert u.reg_read(UC_X86_REG_R12) == 0x1234
        return u.reg_read(UC_X86_REG_R14) & 0xffffffff

    for level in range(11):
        first = (8 ** level - 1) // 7
        last = (8 ** (level + 1) - 1) // 7 - 1
        for index in {first, last, (first + last) // 2}:
            assert decode(index) == decode(index, False) == level, (level, index)
    for index in [0x50000000, 0x50000001, 0x5fffffff]:
        assert decode(index) == 11 and decode(index, False) is None
    for index in [0x60000000, 0x60000001, 0x6fffffff]:
        assert decode(index) == 12

    # ---- original descent loop ---------------------------------------------
    def world(depth, patched, points=None, extents=(0,)):
        u = Uc(UC_ARCH_X86, UC_MODE_64)
        u.mem_map(BASE, (len(image) + 0xfff) & ~0xfff)
        u.mem_write(BASE, image)
        for area, size in [(HEAP, 64 << 20), (STACK, 1 << 20), (STUB, 0x1000), (STOP, 0x1000)]:
            u.mem_map(area, size)
        counters = STUB + 0x800
        u.mem_write(counters, struct.pack('<II', 0x4fffffff, 0x5fffffff))
        stub = bytearray(child_stub)
        struct.pack_into('<Q', stub, counters_at, counters)
        struct.pack_into('<Q', stub, child_back_at, BASE + child_back)
        u.mem_write(STUB, bytes(stub))
        if patched:
            u.mem_write(BASE + child_site, site_patch(20, STUB))
        allocated = []
        nxt = [HEAP + 0x1000]

        def readq(p): return struct.unpack('<Q', u.mem_read(p, 8))[0]
        def readi(p): return struct.unpack('<i', u.mem_read(p, 4))[0]

        def hook(u, address, size, _):
            if address == BASE + ALLOC:
                n = u.reg_read(UC_X86_REG_RDI)
                assert n in (0xa8, 0x40), hex(n)
                result = nxt[0]; nxt[0] += (n + 15) & ~15
                if n == 0xa8: allocated.append(result)
                sp = u.reg_read(UC_X86_REG_RSP)
                u.reg_write(UC_X86_REG_RAX, result)
                u.reg_write(UC_X86_REG_RIP, readq(sp))
                u.reg_write(UC_X86_REG_RSP, sp + 8)
            else:
                raise AssertionError(NEVER[address - BASE] + ' reached')
        for address in [ALLOC] + list(NEVER):
            u.hook_add(UC_HOOK_CODE, hook, begin=BASE + address, end=BASE + address)
        half = 2.0 ** (depth + 5)
        root_slot, center, extent = HEAP, HEAP + 0x40, HEAP + 0x60

        def insert(point, object_extent=0):
            sp = STACK + 0x80000
            u.mem_write(center, struct.pack('<3f', *point))
            u.mem_write(extent, struct.pack('<3f', *([object_extent] * 3)))
            u.mem_write(sp, struct.pack('<Q', STOP))
            u.mem_write(sp + 8, struct.pack('<6f', *([-half] * 3 + [half] * 3)))
            u.mem_write(sp + 0x20, struct.pack('<Q', root_slot))
            for reg, value in [(UC_X86_REG_RSP, sp), (UC_X86_REG_RDI, HEAP + 0x100),
                               (UC_X86_REG_RSI, 0), (UC_X86_REG_RDX, 0),
                               (UC_X86_REG_RCX, center), (UC_X86_REG_R8, extent),
                               (UC_X86_REG_R9, depth)]:
                u.reg_write(reg, value)
            u.emu_start(BASE + DESCEND, STOP, count=100000)
            assert u.reg_read(UC_X86_REG_RIP) == STOP
            node = u.reg_read(UC_X86_REG_RAX)
            loose = struct.unpack('<6f', u.mem_read(node + 0xc, 24))
            want = 512 if object_extent == 200 else 256
            assert all(abs(loose[i + 3] - loose[i] - want) < 0.01 for i in range(3)), loose
            assert all(loose[i] <= point[i] <= loose[i + 3] for i in range(3))
            return node, readi(node + 8)

        if points is None:
            points = list(itertools.product([-half + 72, 72.], repeat=3)) if depth >= 12 else [(-10., 20., 30.)]
            if depth >= 12:
                rng = random.Random(35924)
                points += [tuple(rng.uniform(-half + 1, half - 1) for _ in range(3)) for _ in range(150)]
        leaves = [insert(p) for p in points]
        for p, leaf in zip(points, leaves):
            assert insert(p) == leaf, 'reinsertion changed leaf identity'
        if 200 in extents:
            for p, leaf in zip(points, leaves):
                internal = insert(p, 200)
                assert internal[0] == readq(leaf[0])
                assert 0x50000000 <= internal[1] < 0x60000000
                assert 0x60000000 <= leaf[1] < 0x70000000
                assert insert(p) == leaf
        ids = [readi(n + 8) for n in allocated]
        levels = []
        for n in allocated:
            level, parent = 0, readq(n)
            while parent:
                level += 1; parent = readq(parent)
            levels.append(level)
        return ids, levels, points, len(allocated)

    def check(ids, levels):
        assert len(ids) == len(set(ids)), 'node ID collision'
        assert all(0 <= v <= 0x7fffffff for v in ids)
        for value, level in zip(ids, levels):
            assert decode(value) == level, (hex(value), level)

    counts = {}
    for depth in (13, 12):
        ids, levels, points, counts[depth] = world(depth, True, extents=(0, 200) if depth == 13 else (0,))
        check(ids, levels)
        stock_ids, stock_levels, _, _ = world(depth, False, points)
        assert len(stock_ids) != len(set(stock_ids)) or any(v < 0 for v in stock_ids), \
            'stock overflow not reproduced'
        # Levels 0..10 keep their stock IDs.
        shallow = sorted(v for v, l in zip(ids, levels) if l <= 10)
        assert shallow == sorted(v for v, l in zip(stock_ids, stock_levels) if l <= 10)
    rng = random.Random(11)
    for depth in (11, 10):
        pts = [tuple(rng.uniform(-2.0 ** (depth + 5) + 1, 2.0 ** (depth + 5) - 1) for _ in range(3))
               for _ in range(60)]
        patched = world(depth, True, pts)
        stock = world(depth, False, pts)
        assert patched[:2] == stock[:2], 'depth %d changed by the child stub' % depth
        check(*patched[:2])
    print('PASS: Linux ELF sites; decoder boundaries; %d depth-13 and %d depth-12 original-code nodes; '
          'stock overflow reproduced; internal/leaf ID reuse; depth-10/11 IDs identical to stock'
          % (counts[13], counts[12]))


if __name__ == '__main__':
    main()
