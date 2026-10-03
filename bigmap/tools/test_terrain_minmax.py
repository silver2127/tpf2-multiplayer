"""Compare terrain_minmax_fast with ORIGINAL executable code, natively.

Part 1, CalcMinMaxHeight (inlined in 0x33cd10): the stock per-tile bytes
0x33cec1..0x33cf3c (scan, int->float, scale, the min<=max assert branch and
the record stores) run in a register harness, next to the same bytes with the
69-byte patch applied (which calls the plugin's SSE2 scan). Every live output
register, the record, the assert outcome and the assert-path floats must be
identical, and both must match an independent numpy reference.
Part 2, uint16 block copy 0x30a540 (pdata chunk 0x30a55c): the stock function
is copied verbatim into this process and compared with the detour on complete
buffers. Also: byte/boundary/liveness audit of the patch sites, guard-page
bounds, installer refusal paths and native per-call benchmarks.
No game process or game file is touched.
"""
import ctypes as C
import math
import random
import struct
import time
from pathlib import Path

import capstone
import numpy as np
import pefile

ROOT = Path(__file__).resolve().parents[1]
EXE = r'C:\tools\bin\TransportFever2.exe'
IB = 0x140000000
PAGE = 0x1000
FUNC, FUNC_END = 0x33cd10, 0x33d044
LOOP, LOOP_END, EPI, EPI_END = 0x33ce80, 0x33cf4d, 0x33cfe6, 0x33d002
SCAN, SCAN_END, REGION_END = 0x33cec1, 0x33cf06, 0x33cf3c
JB_OFF = 0x33cf26 - SCAN
ASSERT_CALL = 0x221adf0
COOKIE_CALL = 0x2bf3a30   # __security_check_cookie
FREE_CALL = 0x2bf3abc     # jmp to the CRT free
COPY, COPY_END, COPY_STEAL = 0x30a540, 0x30a610, 14

k = C.WinDLL('kernel32', use_last_error=True)
k.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
k.VirtualAlloc.restype = C.c_void_p
k.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(C.c_uint32)]
k.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
ALLOCS = []


def alloc(size, prot=0x04):
    p = k.VirtualAlloc(None, size, 0x3000, prot)
    assert p
    ALLOCS.append(p)
    return p


def executable(code):
    p = alloc(max(len(code), PAGE), 0x40)
    C.memmove(p, bytes(code), len(code))
    return p


class Guarded:
    """Committed pages with a PAGE_NOACCESS page on either side."""
    def __init__(self, nbytes):
        pages = (nbytes + PAGE - 1) // PAGE
        self.base = alloc((pages + 2) * PAGE)
        old = C.c_uint32()
        assert k.VirtualProtect(self.base, PAGE, 0x01, C.byref(old))
        assert k.VirtualProtect(self.base + (pages + 1) * PAGE, PAGE, 0x01, C.byref(old))
        self.lo, self.hi = self.base + PAGE, self.base + (pages + 1) * PAGE


# ---------------------------------------------------------------- encoding
REG = {n: i for i, n in enumerate('rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15'.split())}


def _mem(reg, base, disp):
    b = REG[base]
    mod, tail = (1, struct.pack('<b', disp)) if -128 <= disp < 128 else (2, struct.pack('<i', disp))
    modrm = bytes([(mod << 6) | ((reg & 7) << 3) | (b & 7)]) + (b'\x24' if b & 7 == 4 else b'')
    return (reg >> 3) & 1, (b >> 3) & 1, modrm + tail


def load(dst, base, disp):
    r, b, m = _mem(REG[dst], base, disp)
    return bytes([0x48 | r << 2 | b, 0x8b]) + m


def store(base, disp, src):
    r, b, m = _mem(REG[src], base, disp)
    return bytes([0x48 | r << 2 | b, 0x89]) + m


def movss(x, base, disp, to_mem=False):
    r, b, m = _mem(x, base, disp)
    rex = 0x40 | r << 2 | b
    return b'\xf3' + (bytes([rex]) if rex != 0x40 else b'') + b'\x0f' + (b'\x11' if to_mem else b'\x10') + m


# ctx slots (uint64 each)
BEGIN, END, R11, R14, RDI, RBX, R12, R15, RBP, JUNK, JUNKF, STATUS, ITER = range(13)
OUT_REGS = ['r10', 'rcx', 'r11', 'rdi', 'rbx', 'r12', 'r14', 'r15', 'rbp', 'rax']  # slots 13..22
OUT_X = 23  # xmm0, xmm1, xmm2 low 32 bits in slots 23..25
HFN = C.CFUNCTYPE(None, C.c_void_p)


def build_harness(region):
    """MS-x64 function(ctx): set the 0x33cec1 entry registers, run `region`
    (123 bytes standing at 0x33cec1..0x33cf3c), record live outputs. The stock
    jb to the CalcMinMaxHeight assert is retargeted to a harness label.
    Frame: 8 pushes + 0x158 keeps rsp % 16 == 0 with a free shadow area, as in
    0x33cd10's own frame. Loops ctx[ITER] times (native benchmark)."""
    assert len(region) == REGION_END - SCAN
    code = bytearray(bytes.fromhex('5355565741544155415641574881ec58010000') + store('rsp', 0x150, 'rcx'))
    reload = len(code)
    code += load('rax', 'rsp', 0x150)
    for reg, slot in [('rdx', BEGIN), ('rsi', END), ('r11', R11), ('r14', R14), ('rdi', RDI),
                      ('rbx', RBX), ('r12', R12), ('r15', R15), ('rbp', RBP),
                      ('r8', JUNK), ('r9', JUNK), ('r10', JUNK), ('rcx', JUNK)]:
        code += load(reg, 'rax', 8 * slot)
    code += movss(0, 'rax', 8 * JUNKF) + movss(1, 'rax', 8 * JUNKF)
    code += bytes.fromhex('f3410f105634')          # movss xmm2,[r14+34h] as stock 0x33ceab
    code += load('rax', 'rax', 8 * JUNK)
    region_off = len(code)
    code += region
    code += bytes.fromhex('41bd01000000')          # ok: mov r13d,1
    jmp_store = len(code)
    code += b'\xeb\x00'
    assert_off = len(code)
    code += bytes.fromhex('41bd02000000')          # assert: mov r13d,2
    code[jmp_store + 1] = len(code) - (jmp_store + 2)
    code += load('rdx', 'rsp', 0x150)
    for i, reg in enumerate(OUT_REGS):
        code += store('rdx', 8 * (13 + i), reg)
    for i in range(3):
        code += movss(i, 'rdx', 8 * (OUT_X + i), True)
    code += bytes.fromhex('4c896a58')              # mov [rdx+58h],r13 (STATUS)
    code += bytes.fromhex('4183fd01')              # cmp r13d,1
    jne_exit = len(code)
    code += b'\x0f\x85\0\0\0\0'
    code += bytes.fromhex('48ff4a60')              # dec qword ptr [rdx+60h] (ITER)
    jne_reload = len(code)
    code += b'\x0f\x85' + struct.pack('<i', reload - (jne_reload + 6))
    struct.pack_into('<i', code, jne_exit + 2, len(code) - (jne_exit + 6))
    code += bytes.fromhex('4881c45801000041' '5f415e415d415c5f5e5d5bc3')
    jb = region_off + JB_OFF
    assert code[jb:jb + 2] == b'\x0f\x82'
    struct.pack_into('<i', code, jb + 2, assert_off - (jb + 6))
    # The harness must decode as one contiguous instruction stream.
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoded = list(md.disasm(bytes(code), 0))
    assert sum(i.size for i in decoded) == len(code)
    assert any(i.address == jb and i.mnemonic == 'jb' and int(i.op_str, 16) == assert_off for i in decoded)
    return bytes(code)


BENCHFN = C.CFUNCTYPE(None, C.c_void_p)


def build_bench():
    """MS-x64 bench(ctx): ctx[0] = callee, ctx[1..10] = its ten arguments,
    ctx[11] = iterations. Calls it in a native loop, so a per-call time is the
    callee's own cost and not ctypes marshalling."""
    code = bytearray(bytes.fromhex('5355565741544155415641574881ec58000000'))
    code += bytes.fromhex('4889cb')                 # mov rbx, rcx
    code += load('r12', 'rbx', 8 * 11)
    loop = len(code)
    for reg, slot in (('rcx', 1), ('rdx', 2), ('r8', 3), ('r9', 4)):
        code += load(reg, 'rbx', 8 * slot)
    for slot in range(5, 11):
        code += load('rax', 'rbx', 8 * slot) + store('rsp', 0x20 + 8 * (slot - 5), 'rax')
    code += bytes.fromhex('ff13')                   # call qword ptr [rbx]
    code += bytes.fromhex('49ffcc')                 # dec r12
    code += bytes.fromhex('0f85') + struct.pack('<i', loop - (len(code) + 6))
    code += bytes.fromhex('4881c458000000' '415f415e415d415c5f5e5d5b' 'c3')
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    assert sum(i.size for i in md.disasm(bytes(code), 0)) == len(code)
    return bytes(code)


class Rig:
    def __init__(self):
        self.ctx = (C.c_uint64 * 32)()
        self.this = (C.c_uint8 * 0x40)()
        self.record = (C.c_uint8 * 0x100)()

    def run(self, fn, begin, end, scale_bits, iters=1):
        struct.pack_into('<I', self.this, 0x34, scale_bits)
        C.memset(self.record, 0xa5, 0x100)
        struct.pack_into('<i', self.record, 5 * 8 + 0x20, 0x7ffffff0)
        c = self.ctx
        C.memset(c, 0, C.sizeof(c))
        c[BEGIN], c[END], c[R11], c[R14] = begin, end, 0x1111222233334444, C.addressof(self.this)
        c[RDI], c[RBX], c[R12], c[R15], c[RBP] = C.addressof(self.record), 5, 0, 0x5555666677778888, 0x9999aaaabbbbcccc
        c[JUNK], c[JUNKF], c[ITER] = 0xdeadbeefcafef00d, 0x7fc0dead, iters
        fn(C.addressof(c))
        return tuple(c[STATUS:26]), bytes(self.record)


def f32bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reference(values, n, scale_bits):
    """numpy model of stock semantics: exact unsigned min/max, IEEE single mul."""
    v = values[:n] if n else values[:1]
    lo, hi = int(v.min()), int(v.max())
    scale = np.frombuffer(struct.pack('<I', scale_bits), dtype=np.float32)[0]
    with np.errstate(all='ignore'):
        fmin, fmax = np.float32(lo) * scale, np.float32(hi) * scale
    ok = not (np.isnan(fmin) or np.isnan(fmax) or fmax < fmin)
    return lo, hi, int(fmin.view(np.uint32)), int(fmax.view(np.uint32)), ok


def main():
    pe = pefile.PE(EXE, fast_load=True)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))

    # ------------------------------------------------ bytes and boundaries
    func = {i.address - IB: i for i in md.disasm(pe.get_data(FUNC, FUNC_END - FUNC), IB + FUNC)}
    assert max(a + i.size for a, i in func.items()) == FUNC_END
    for a in (LOOP, SCAN, SCAN_END, REGION_END, LOOP_END, EPI, EPI_END):
        assert a in func, hex(a)
    for a, i in func.items():
        if i.group(capstone.CS_GRP_JUMP) and i.operands[0].type == capstone.x86.X86_OP_IMM:
            t = i.operands[0].imm - IB
            assert not (SCAN < t < SCAN_END) or SCAN <= a < SCAN_END, (hex(a), hex(t))
    scan_ins = [i for a, i in sorted(func.items()) if SCAN <= a < SCAN_END]
    assert not any(i.group(capstone.CS_GRP_CALL) for i in scan_ins)
    tail = [i for a, i in sorted(func.items()) if SCAN_END <= a < 0x33cf26]
    assert tail[-1].mnemonic == 'comiss' and not any(i.group(capstone.CS_GRP_JUMP) for i in tail)
    copy_raw = pe.get_data(COPY, COPY_END - COPY)
    copy_ins = list(md.disasm(copy_raw, IB + COPY))
    assert sum(i.size for i in copy_ins) == len(copy_raw) and copy_ins[-1].mnemonic == 'ret'
    assert COPY + COPY_STEAL in [i.address - IB for i in copy_ins]
    assert not any(i.group(capstone.CS_GRP_CALL) for i in copy_ins)
    for i in copy_ins:
        for op in i.operands:
            assert not (op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP)
        if i.group(capstone.CS_GRP_JUMP):
            t = i.operands[0].imm - IB
            assert COPY + COPY_STEAL <= t < COPY_END
    print('PASS: original bytes, instruction boundaries, no external branch into the patched scan or the stolen copy prologue')

    # ---------------------------------------------- liveness after 0x33cf06
    fam = {}
    for b64, names in {'rax': 'rax eax ax al ah', 'rcx': 'rcx ecx cx cl ch', 'rdx': 'rdx edx dx dl dh',
                       'rbx': 'rbx ebx bx bl bh', 'rsi': 'rsi esi si sil', 'rdi': 'rdi edi di dil',
                       'rbp': 'rbp ebp bp bpl', 'rsp': 'rsp esp sp spl'}.items():
        for n in names.split():
            fam[n] = b64
    for r in range(8, 16):
        for suffix in ('', 'd', 'w', 'b'):
            fam[f'r{r}{suffix}'] = f'r{r}'
    clobbered = {'rax', 'rdx', 'rsi', 'r8', 'r9', 'xmm0', 'xmm1'}   # what the patch leaves different
    volatile = {'rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11', 'xmm0', 'xmm1', 'xmm2', 'xmm3', 'xmm4', 'xmm5'}
    full_width = {'rax', 'eax', 'rdx', 'edx', 'rsi', 'esi', 'r8', 'r8d', 'r9', 'r9d'}
    # Callee argument reads, justified from the callees' own bytes: the stack
    # cookie check touches only rcx; the assert takes four; free gets rcx+rdx
    # (in case it is the sized operator delete). Any unlisted direct callee is
    # assumed to read all four argument registers.
    cookie = []
    for i in md.disasm(pe.get_data(COOKIE_CALL, 48), IB + COOKIE_CALL):
        cookie.append(i)
        if 'ret' in i.mnemonic or 'jmp' in i.mnemonic:
            break
    cookie_regs = {fam.get(ins.reg_name(r), ins.reg_name(r)) for ins in cookie for r in ins.regs_access()[0]}
    assert cookie_regs <= {'rcx', 'rsp', 'rip', 'rflags'}, cookie_regs
    CALLEE_ARGS = {COOKIE_CALL: {'rcx'}, FREE_CALL: {'rcx', 'rdx'},
                   ASSERT_CALL: {'rcx', 'rdx', 'r8', 'r9'}}
    stack, seen, paths = [(SCAN_END, frozenset())], set(), 0
    while stack:
        a, dead = stack.pop()
        if (a, dead) in seen:
            continue
        seen.add((a, dead))
        if a == SCAN:          # back at the patch entry: inputs must be fresh by now
            paths += 1
            continue
        i = func[a]
        ops = i.operands
        zero_idiom = i.mnemonic in ('xorps', 'pxor', 'xor') and len(ops) == 2 and ops[0].type == ops[1].type == capstone.x86.X86_OP_REG and ops[0].reg == ops[1].reg
        reads, writes = i.regs_access()
        rd = set() if zero_idiom else {fam.get(i.reg_name(r), i.reg_name(r)) for r in reads}
        for op in ops:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RSP:
                assert op.mem.disp >= 0x20, ('shadow area read', hex(a))
        call = bool(i.group(capstone.CS_GRP_CALL))
        target = ops[0].imm - IB if call and ops[0].type == capstone.x86.X86_OP_IMM else None
        nxt = func.get(a + i.size)
        # A call immediately followed by int3 is the CRT fail-fast
        # (_invalid_parameter_noinfo_noreturn, reached only from a corrupt
        # allocation header): it takes no arguments, never returns, and the
        # process aborts there whichever scan ran.
        failfast = call and nxt is not None and nxt.mnemonic == 'int3'
        if call and not failfast:
            rd |= CALLEE_ARGS.get(target, {'rcx', 'rdx', 'r8', 'r9'})
        bad = (rd & clobbered) - dead
        assert not bad, (hex(a), i.mnemonic, i.op_str, bad)
        if i.mnemonic in ('ret', 'int3'):
            paths += 1
            continue
        if call:
            if failfast or target == ASSERT_CALL:
                paths += 1
                continue
            dead = dead | (volatile & clobbered)
        for r in writes:
            name = i.reg_name(r)
            if name in full_width:
                dead = dead | {fam[name]}
            elif name in ('xmm0', 'xmm1') and (zero_idiom or i.mnemonic in ('movd', 'movq') or
                                              (i.mnemonic == 'movss' and ops[1].type == capstone.x86.X86_OP_MEM)):
                dead = dead | {name}
        dead = frozenset(dead)
        if i.group(capstone.CS_GRP_JUMP):
            stack.append((ops[0].imm - IB, dead))
            if i.mnemonic == 'jmp':
                continue
        stack.append((a + i.size, dead))
    print(f'PASS: rax/rdx/rsi/r8/r9/xmm0/xmm1 are rewritten before any read on all {paths} paths after 0x33cf06; shadow area unread')

    # ------------------------------------------------ patch and harnesses
    scan_addr = dll.BigmapTestTerrainMinMaxScanAddress
    scan_addr.restype = C.c_size_t
    scan_fn = scan_addr()
    make_patch = dll.BigmapTestTerrainMinMaxPatch
    make_patch.argtypes = [C.c_void_p, C.c_size_t]
    patch = (C.c_uint8 * (SCAN_END - SCAN))()
    make_patch(patch, scan_fn)
    patch = bytes(patch)
    pins = list(md.disasm(patch, IB + SCAN))
    text = [(i.mnemonic, i.op_str) for i in pins[:11]]
    assert text[0] == ('mov', 'rcx, rdx') and text[1] == ('mov', 'rdx, rsi') and text[2] == ('mov', 'rsi, r11')
    assert text[3][0] in ('mov', 'movabs') and pins[3].operands[1].imm == scan_fn
    assert text[4:] == [('call', 'rax'), ('mov', 'r11, rsi'), ('movss', 'xmm2, dword ptr [r14 + 0x34]'),
                        ('movzx', 'r10d, ax'), ('shr', 'eax, 0x10'), ('mov', 'ecx, eax'), ('jmp', hex(IB + SCAN_END))]
    used = sum(i.size for i in pins[:11])
    assert patch[used:] == b'\xcc' * (len(patch) - used)
    stock_region = pe.get_data(SCAN, REGION_END - SCAN)
    fast_region = patch + pe.get_data(SCAN_END, REGION_END - SCAN_END)
    stock = HFN(executable(build_harness(stock_region)))
    fast = HFN(executable(build_harness(fast_region)))
    rig = Rig()
    print('PASS: patch decodes to the documented 11 instructions, jumps to 0x33cf06, int3 padding')

    rng = np.random.default_rng(35924)
    prng = random.Random(35924)
    scales = [f32bits(s) for s in (1.0, 0.1, 0.25, 1 / 64, 0.01, 3.0, 65536.0, 0.0, -0.0, -1.0, -0.1,
                                   math.inf, -math.inf, 3.4028235e38, 1.17549435e-38, 1e-45, 1e-40)]
    scales += [0x7fc00000, 0xffc00000, 0x7f800001, 0x80000001, 0x00000001]
    cases = 0

    def compare(values, n, begin, end, scale_bits, check_reference=True):
        nonlocal cases
        a = rig.run(stock, begin, end, scale_bits)
        b = rig.run(fast, begin, end, scale_bits)
        if a != b:
            raise AssertionError(('stock/fast differ', n, hex(begin), hex(end), hex(scale_bits), a[0], b[0]))
        if check_reference:
            lo, hi, fmin, fmax, ok = reference(values, n, scale_bits)
            outs = a[0]
            assert outs[2] == lo and outs[3] == hi, (n, lo, hi, outs[2], outs[3])   # r10, rcx
            assert outs[0] == (1 if ok else 2)
            if ok:
                assert struct.unpack_from('<II', a[1], 5 * 8 + 0x18) == (fmin, fmax)
                assert struct.unpack_from('<i', a[1], 5 * 8 + 0x20)[0] == 0x7ffffff1
            else:
                assert (outs[OUT_X - STATUS] & 0xffffffff, outs[OUT_X - STATUS + 1] & 0xffffffff) == (fmin, fmax)
                # The assert path stores nothing and never bumps the counter.
                assert a[1][5 * 8 + 0x18:5 * 8 + 0x20] == b'\xa5' * 8
                assert struct.unpack_from('<i', a[1], 5 * 8 + 0x20)[0] == 0x7ffffff0
            assert outs[4] == 0x1111222233334444 and outs[9] == 0x5555666677778888   # r11, r15
        cases += 1

    def run_values(values, scale_bits=None):
        values = np.ascontiguousarray(values, dtype=np.uint16)
        n = len(values)
        padded = np.append(values, np.uint16(prng.randrange(65536)))
        begin = padded.ctypes.data
        compare(padded, n, begin, begin + 2 * n, scales[0] if scale_bits is None else scale_bits)
        return padded

    for n in range(1, 131):
        for fill in (0, 1, 0x7fff, 0x8000, 0xfffe, 65535, prng.randrange(65536)):
            run_values(np.full(n, fill))
        run_values(rng.integers(0, 65536, n))
        run_values(rng.integers(0x7ff0, 0x8010, n))
        for pos in range(n):
            for base, spike in ((30000, 29999), (30000, 30001), (65535, 0), (0, 65535), (0x7fff, 0x8000), (0x8000, 0x7fff)):
                values = np.full(n, base)
                values[pos] = spike
                run_values(values)
            if n > 1:
                values = np.full(n, 20000)
                values[pos] = 0
                values[(pos * 7 + 3) % n] = 65535
                run_values(values)
    print(f'PASS: {cases} register-harness comparisons, n=1..130, all-equal, extremes, single low/high outliers at every position')

    for n in (129 * 129, 257 * 257, 257 * 257 - 1, 257 * 257 + 1, 1000, 4095, 4096, 4097, 65536):
        positions = sorted({0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, n // 2, n - 33, n - 32, n - 31,
                            n - 9, n - 8, n - 7, n - 2, n - 1})
        base = rng.integers(20000, 20100, n)
        for pos in positions:
            for spike in (0, 19999, 20100, 65535):
                values = base.copy()
                values[pos] = spike
                run_values(values)
        run_values(np.full(n, 65535))
        run_values(np.full(n, 0))
        run_values(rng.integers(0, 65536, n))
        run_values(np.sort(rng.integers(0, 65536, n)))
        run_values(np.sort(rng.integers(0, 65536, n))[::-1])
        run_values(np.tile(np.array([0x7fff, 0x8000]), n)[:n])
        run_values(np.clip(np.cumsum(rng.integers(-3, 4, n)) + 30000, 0, 65535))
    print(f'PASS: {cases} total; tile sizes 129^2, 257^2 (+-1), 4096 boundaries, outliers at every SIMD block class')

    for values in (np.full(300, 0), np.full(300, 7), rng.integers(0, 65536, 300), rng.integers(1, 65536, 66049),
                   np.array([0]), np.array([65535]), np.array([1, 65535])):
        for s in scales + [prng.getrandbits(32) for _ in range(20)]:
            run_values(values, s)
    print(f'PASS: {cases} total; scale float edge values (0, -0, negative, inf, NaN, FLT_MAX, denormal): stores and assert path identical')

    # Count formula: odd byte lengths read one element past end; begin>=end.
    for n in list(range(1, 70)) + [66049, 16641]:
        values = rng.integers(0, 65536, n + 2).astype(np.uint16)
        begin = values.ctypes.data
        for end, count in ((begin + 2 * n - 1, n), (begin + 2 * n + 1, n + 1), (begin, 0)):
            compare(values, count, begin, end, scales[1])
    values = rng.integers(0, 65536, 8).astype(np.uint16)
    compare(values, 0, values.ctypes.data + 8, values.ctypes.data + 2, scales[0], check_reference=False)
    lo_hi = rig.run(fast, values.ctypes.data + 8, values.ctypes.data + 2, scales[0])[0][2:4]
    assert lo_hi == (int(values[4]), int(values[4]))
    print(f'PASS: {cases} total; stock count formula (odd byte length, empty, begin > end)')

    # Guard pages: the replacement reads no byte outside stock's read range.
    for n in (1, 2, 7, 8, 31, 32, 33, 64, 65, 16641, 66049):
        g = Guarded(2 * n + 2)
        data = rng.integers(0, 65536, n).astype(np.uint16)
        for begin, end in ((g.hi - 2 * n, g.hi), (g.hi - 2 * n, g.hi - 1), (g.lo, g.lo + 2 * n)):
            C.memmove(begin, data.ctypes.data, 2 * n)
            compare(data, n, begin, end, scales[0])
    print(f'PASS: {cases} total; guard pages on both sides of the scanned range')

    scan = dll.BigmapTestTerrainMinMaxScan
    scan.argtypes = [C.c_void_p, C.c_void_p]
    scan.restype = C.c_uint32
    for _ in range(4000):
        n = prng.choice([prng.randrange(1, 100), prng.randrange(1, 70000)])
        kind = prng.randrange(4)
        if kind == 0:
            values = rng.integers(0, 65536, n)
        elif kind == 1:
            values = rng.integers(prng.randrange(65536) // 2, 65536, n)
        elif kind == 2:
            values = np.clip(np.cumsum(rng.integers(-5, 6, n)) + prng.randrange(65536), 0, 65535)
        else:
            values = np.full(n, prng.randrange(65536))
        values = np.ascontiguousarray(values, dtype=np.uint16)
        r = scan(values.ctypes.data, values.ctypes.data + 2 * n)
        assert r == int(values.min()) | (int(values.max()) << 16)
        if _ % 8 == 0:
            run_values(values, prng.choice(scales))
    print(f'PASS: {cases} harness comparisons in total; 4000 direct scans equal numpy min/max')

    # ------------------------------------------------------ block copy
    COPYFN = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, *[C.c_int] * 8)
    stock_copy_addr = executable(copy_raw)
    stock_copy = COPYFN(stock_copy_addr)
    fast_copy = dll.BigmapTestTerrainBlockCopy
    fast_copy.argtypes = [COPYFN, C.c_void_p, C.c_void_p] + [C.c_int] * 8
    fast_copy.restype = None
    fallbacks = []

    @COPYFN
    def recorder(*args):
        fallbacks.append(args)

    def i32(x):
        return (x + 2 ** 31) % 2 ** 32 - 2 ** 31

    def span(stride, x, y, w, h):
        first = i32(stride * y) + x
        last = first + (h - 1) * stride
        return min(first, last), max(first, last) + w

    copies = 0

    def copy_case(ss, ds, sx, sy, w, h, dx, dy):
        nonlocal copies
        slo, shi = span(ss, sx, sy, w, h)
        dlo, dhi = span(ds, dx, dy, w, h)
        src = rng.integers(0, 65536, shi - slo + 16).astype(np.uint16)
        keep = src.copy()
        d1 = rng.integers(0, 65536, dhi - dlo + 16).astype(np.uint16)
        d2 = d1.copy()
        sbase = src.ctypes.data + 2 * (8 - slo)
        args = (ss, ds, sx, sy, w, h, dx, dy)
        stock_copy(sbase, d1.ctypes.data + 2 * (8 - dlo), *args)
        del fallbacks[:]
        fast_copy(recorder, sbase, d2.ctypes.data + 2 * (8 - dlo), *args)
        assert not fallbacks
        fast_copy(stock_copy, sbase, d2.ctypes.data + 2 * (8 - dlo), *args)
        assert np.array_equal(d1, d2) and np.array_equal(src, keep), args
        copies += 1

    for _ in range(3000):
        w, h = prng.randrange(1, 300), prng.randrange(1, 300)
        ss, ds = prng.choice([w, w + 1, prng.randrange(1, 600)]), prng.choice([w, 257, 129, prng.randrange(1, 600)])
        copy_case(ss, ds, prng.randrange(0, 40), prng.randrange(0, 40), w, h, prng.randrange(0, 40), prng.randrange(0, 40))
    for ss, ds, sx, sy, w, h, dx, dy in [
            (513, 257, 256, 256, 257, 257, 0, 0), (257, 257, 0, 0, 257, 257, 0, 0), (257, 513, 0, 0, 257, 257, 256, 0),
            (131, 129, 1, 1, 129, 129, 0, 0), (1, 1, 0, 0, 1, 1, 0, 0), (3, 2, 0, 0, 5, 7, 0, 0),       # rows overlap
            (-7, 5, 3, 2, 4, 9, 1, 1), (6, -6, -3, 4, 6, 5, 2, -1), (0, 0, 0, 0, 9, 50, 0, 0),         # negative/zero
            (3, 1, 0, 0x55555556, 4, 6, 0, 0), (0x10000, 0x8000, 5, 0x10000, 3, 1, 1, 0x20000)]:          # int32 wrap
        copy_case(ss, ds, sx, sy, w, h, dx, dy)
    print(f'PASS: {copies} block copies identical (full destination, source unchanged, no fallback): '
          'random, publication/GetBlock geometries, self-overlapping rows, negative strides, int32 wrap')

    for args in [(5, 5, 0, 0, 0, 5, 0, 0), (5, 5, 0, 0, -3, 5, 0, 0), (5, 5, 0, 0, 5, 0, 0, 0), (5, 5, 0, 0, 5, -2, 0, 0)]:
        stock_copy(None, None, *args)
        del fallbacks[:]
        fast_copy(recorder, None, None, *args)
        assert not fallbacks
    print('PASS: w<=0 / h<=0 touch no memory in stock or detour (null buffers)')

    def same_buffer(sshift, dshift, args, expect_fallback, dtype_bytes=False):
        nonlocal copies
        buf = rng.integers(0, 256, 40000).astype(np.uint8)
        b1, b2, b3 = buf.copy(), buf.copy(), buf.copy()
        stock_copy(b1.ctypes.data + sshift, b1.ctypes.data + dshift, *args)
        fast_copy(stock_copy, b2.ctypes.data + sshift, b2.ctypes.data + dshift, *args)
        assert np.array_equal(b1, b2), args
        del fallbacks[:]
        fast_copy(recorder, b3.ctypes.data + sshift, b3.ctypes.data + dshift, *args)
        assert len(fallbacks) == int(expect_fallback), (sshift, dshift, args)
        copies += 1

    same_buffer(100, 102, (1, 1, 0, 0, 50, 1, 0, 0), True)        # forward smear
    same_buffer(102, 100, (1, 1, 0, 0, 50, 1, 0, 0), True)
    same_buffer(100, 101, (10, 10, 0, 0, 10, 10, 0, 0), True)     # odd byte offset
    same_buffer(100, 100, (10, 10, 0, 0, 10, 10, 0, 0), True)
    same_buffer(100, 100, (10, 10, 0, 0, 10, 2, 0, 1), True)      # dst row 1 over src row 1
    same_buffer(100, 300, (10, 10, 0, 0, 10, 10, 0, 0), False)    # spans exactly adjacent
    same_buffer(100, 120, (20, 20, 0, 0, 10, 5, 0, 0), True)      # rows interleave, no row overlaps
    same_buffer(100, 100 + 2 * 50, (10, 10, 0, 0, 10, 5, 0, 0), False)   # adjacent, disjoint
    same_buffer(100 + 2 * 50, 100, (10, 10, 0, 0, 10, 5, 0, 0), False)
    same_buffer(100, 101 + 2 * 50, (10, 10, 0, 0, 10, 5, 0, 0), False)
    one = rng.integers(0, 65536, 4).astype(np.uint16)
    d1, d2 = one.copy(), one.copy()
    rows = (1 << 20) + 1
    del fallbacks[:]
    fast_copy(recorder, one.ctypes.data, d2.ctypes.data, 0, 0, 0, 0, 1, rows, 1, 0)
    assert len(fallbacks) == 1
    stock_copy(one.ctypes.data, d1.ctypes.data, 0, 0, 0, 0, 1, rows, 1, 0)
    fast_copy(stock_copy, one.ctypes.data, d2.ctypes.data, 0, 0, 0, 0, 1, rows, 1, 0)
    assert np.array_equal(d1, d2)
    wide = (1 << 20) + 1
    src = rng.integers(0, 65536, wide).astype(np.uint16)
    d1, d2 = np.zeros(wide, np.uint16), np.zeros(wide, np.uint16)
    del fallbacks[:]
    fast_copy(recorder, src.ctypes.data, d2.ctypes.data, wide, wide, 0, 0, wide, 1, 0, 0)
    assert len(fallbacks) == 1
    stock_copy(src.ctypes.data, d1.ctypes.data, wide, wide, 0, 0, wide, 1, 0, 0)
    fast_copy(stock_copy, src.ctypes.data, d2.ctypes.data, wide, wide, 0, 0, wide, 1, 0, 0)
    assert np.array_equal(d1, d2)
    print('PASS: any shared byte (smear, odd offset, aliasing, interleaved rows) and >2^20 rows/columns fall back to the original; adjacent spans do not')

    for w, h, stride in ((1, 1, 1), (257, 257, 257), (31, 3, 40), (513, 2, 513)):
        need = ((h - 1) * stride + w) * 2
        gs, gd = Guarded(need), Guarded(need)
        payload = rng.integers(0, 256, need).astype(np.uint8)
        for sb, db in ((gs.hi - need, gd.hi - need), (gs.lo, gd.lo), (gs.hi - need, gd.lo)):
            C.memmove(sb, payload.ctypes.data, need)
            C.memset(db, 0, need)
            fast_copy(stock_copy, sb, db, stride, stride, 0, 0, w, h, 0, 0)
            got = C.string_at(db, need)
            C.memset(db, 0, need)
            stock_copy(sb, db, stride, stride, 0, 0, w, h, 0, 0)
            assert got == C.string_at(db, need)
    print('PASS: block copy guard pages on both sides of source and destination')

    # ---------------------------------------------------------- installer
    logtype = C.CFUNCTYPE(None, C.c_char_p)
    basetype = C.CFUNCTYPE(C.c_size_t)
    verifytype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
    hooktype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_int, C.POINTER(C.c_void_p))
    patchtype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)

    class Host(C.Structure):
        _fields_ = [('size', C.c_uint32), ('abi', C.c_uint32), ('log', logtype),
                    ('cfgInt', C.c_void_p), ('cfgBool', C.c_void_p), ('cfgStr', C.c_void_p),
                    ('base', basetype), ('buildOk', C.c_void_p), ('verify', verifytype),
                    ('hook', hooktype), ('patch', patchtype), ('dataDir', C.c_void_p)]

    install = dll.BigmapTestInstallTerrainMinMax
    install.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
    sites = [(LOOP, LOOP_END - LOOP), (EPI, EPI_END - EPI), (COPY, COPY_END - COPY)]
    for failure in ('none', 'disabled', 'gog', 'verify0', 'verify1', 'verify2', 'patch', 'hook', 'both'):
        events, errors, logs = [], [], []

        @logtype
        def log(fmt):
            logs.append(fmt)

        @basetype
        def getbase():
            return IB

        @verifytype
        def verify(rva, ptr, size):
            index = len([e for e in events if e[0] == 'verify'])
            events.append(('verify', rva, size))
            if (rva, size) != sites[index] or C.string_at(ptr, size) != pe.get_data(rva, size):
                errors.append(('verify', hex(rva), size))
            return failure != f'verify{index}'

        @patchtype
        def patcher(rva, ptr, size):
            events.append(('patch', rva, size))
            if rva != SCAN or size != SCAN_END - SCAN or C.string_at(ptr, size) != patch:
                errors.append('patch')
            return failure not in ('patch', 'both')

        @hooktype
        def hook(target, detour, steal, out):
            events.append(('hook', target - IB, steal))
            if target != IB + COPY or steal != COPY_STEAL or not detour:
                errors.append('hook')
            out[0] = 0x1000
            return failure not in ('hook', 'both')

        host = Host(C.sizeof(Host), 1, log, None, None, None, getbase, None, verify, hook, patcher, None)
        result = bool(install(C.byref(host), failure == 'gog', failure != 'disabled'))
        assert not errors, (failure, errors)
        full = [('verify',) + sites[0], ('verify',) + sites[1], ('verify',) + sites[2],
                ('patch', SCAN, SCAN_END - SCAN), ('hook', COPY, COPY_STEAL)]
        if failure in ('disabled', 'gog'):
            assert events == [] and logs == [] and not result
        elif failure.startswith('verify'):
            assert events == full[:int(failure[-1]) + 1] and len(logs) == 1 and not result
        else:
            assert events == full and len(logs) == 1 and result == (failure != 'both'), (failure, events, result)
    print('PASS: installer preflights all three byte ranges before patching; refuses when disabled, on GOG or on any mismatch; '
          'one log line; partial install only when one half fails')

    # --------------------------------------------------------- benchmarks
    print('--- native micro-benchmarks (warm cache, one core; not an in-game load time) ---')
    n = 257 * 257
    for label, values in (('random uniform', rng.integers(0, 65536, n)),
                          ('terrain-like walk', np.clip(np.cumsum(rng.integers(-4, 5, n)) + 30000, 0, 65535)),
                          ('flat tile', np.full(n, 12345))):
        values = np.ascontiguousarray(values, dtype=np.uint16)
        begin = values.ctypes.data
        timings = []
        for fn in (stock, fast):
            outs, _ = rig.run(fn, begin, begin + 2 * n, scales[1], iters=50)
            assert outs[0] == 1 and outs[2] == int(values.min()) and outs[3] == int(values.max())
            best = math.inf
            for _ in range(5):
                t = time.perf_counter()
                rig.run(fn, begin, begin + 2 * n, scales[1], iters=400)
                best = min(best, (time.perf_counter() - t) / 400)
            timings.append(best)
        print(f'CalcMinMaxHeight 257x257 {label}: stock={timings[0] * 1e6:.1f} us fast={timings[1] * 1e6:.2f} us '
              f'ratio={timings[0] / timings[1]:.1f}x')
    detour_addr_fn = dll.BigmapTestTerrainBlockCopyAddress
    detour_addr_fn.restype = C.c_size_t
    detour_addr = detour_addr_fn()
    set_original = dll.BigmapTestTerrainBlockCopySetOriginal
    set_original.argtypes = [COPYFN]
    set_original(stock_copy)
    bench = BENCHFN(executable(build_bench()))
    bench_ctx = (C.c_uint64 * 12)()

    def bench_call(addr, call_args, iters):
        bench_ctx[0] = addr
        for i, v in enumerate(call_args):
            bench_ctx[1 + i] = v & 0xffffffffffffffff
        bench_ctx[11] = iters
        t = time.perf_counter()
        bench(C.addressof(bench_ctx))
        return (time.perf_counter() - t) / iters

    src = rng.integers(0, 65536, 257 * 257).astype(np.uint16)
    dst = np.zeros(257 * 257, np.uint16)
    args = (src.ctypes.data, dst.ctypes.data, 257, 257, 0, 0, 257, 257, 0, 0)
    warm = []
    for addr in (stock_copy_addr, detour_addr):
        dst.fill(0)
        bench_call(addr, args, 50)
        assert np.array_equal(dst, src)          # the benchmark really copied
        warm.append(min(bench_call(addr, args, 2000) for _ in range(5)))
    print(f'block copy 257x257 warm (native loop): stock={warm[0] * 1e6:.1f} us fast={warm[1] * 1e6:.2f} us '
          f'ratio={warm[0] / warm[1]:.1f}x')
    cold = []
    for which in (0, 1):
        buffers = [k.VirtualAlloc(None, 2 * 257 * 257, 0x3000, 0x04) for _ in range(300)]
        t = time.perf_counter()
        for b in buffers:
            if which == 0:
                stock_copy(src.ctypes.data, b, 257, 257, 0, 0, 257, 257, 0, 0)
            else:
                fast_copy(stock_copy, src.ctypes.data, b, 257, 257, 0, 0, 257, 257, 0, 0)
        cold.append((time.perf_counter() - t) / len(buffers))
        for b in buffers:
            k.VirtualFree(b, 0, 0x8000)
    print(f'block copy 257x257 into never-touched pages (first-touch faults): stock={cold[0] * 1e6:.1f} us '
          f'fast={cold[1] * 1e6:.1f} us ratio={cold[0] / cold[1]:.2f}x')
    for p in ALLOCS:
        k.VirtualFree(p, 0, 0x8000)


if __name__ == '__main__':
    main()
