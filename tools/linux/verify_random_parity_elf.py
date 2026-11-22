#!/usr/bin/env python3
"""Check the RNG parity guards and stolen instructions against the actual ELF.
Requires pyelftools and capstone. Does not execute game code.
"""
from pathlib import Path
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_OP_IMM
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[2] / 'native/linux/src'
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(addr, size):
        s = next(s for s in loads if s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(s['p_offset'] + addr - s['p_vaddr'])
        return stream.read(size)
    def arrays(source):
        return {name: bytes(int(v, 16) for v in re.findall(r'0x[0-9a-f]+', body))
                for name, body in re.findall(r'unsigned char (\w+)\[\] = \{(.*?)\};', source, re.S)}
    ranges = []
    def stolen(addr, size):
        ranges.append((addr, size))
        instructions = list(md.disasm(read(addr, size), addr))
        assert sum(i.size for i in instructions) == size, hex(addr)
        for i in instructions:
            assert not i.mnemonic.startswith(('j', 'call')), (hex(addr), i.mnemonic)
            assert not any(o.type == X86_OP_MEM and o.mem.base == X86_REG_RIP for o in i.operands)
        print(f'  stolen {addr:#x} ({size}): ' + '; '.join(i.mnemonic + ' ' + i.op_str for i in instructions))
    seed = (root / 'sim_seed_sites_linux.h').read_text()
    a = arrays(seed)
    sites = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (\d+),.*?(kSimSeedContext\d+),', seed)
    assert len(sites) == 8
    for context, hook, size, name in sites:
        context, hook, size = int(context, 16), int(hook, 16), int(size)
        assert read(context, len(a[name])) == a[name], name
        assert context + len(a[name]) == hook + size
        print(f'PASS {name} at {context:#x}, {len(a[name])} bytes')
        stolen(hook, size)
    header = (root / 'engine_parity_sites_linux.h').read_text()
    source = (root / 'engine_parity_linux.cpp').read_text()
    a = arrays(header)
    constants = dict((n, int(v, 16)) for n, v in re.findall(r'uintptr_t (\w+) = (0x[0-9a-f]+)', header + source))
    guards = re.findall(r'\{(k\w+Rva), (k\w+Bytes), sizeof', source)
    assert len(guards) == 20
    for address, name in guards:
        addr = constants[address]
        assert read(addr, len(a[name])) == a[name], name
        print(f'PASS {name} at {addr:#x}, {len(a[name])} bytes')
    hooks = re.findall(r'\{(k\w+Rva), reinterpret_cast<void\*>\(\w+\), (\d+),', source)
    assert len(hooks) == 11
    for addr, size in hooks:
        stolen(constants[addr], int(size))
    assert read(constants['kOneRva'], 4) == bytes.fromhex('00 00 80 3f')
    assert read(constants['kTwoPowMinus31Rva'], 4) == bytes.fromhex('00 00 00 30')
    assert read(constants['kUnitStdClampRva'], 2) == bytes.fromhex('73 28')
    assert read(constants['kDoubleBoostClampRva'], 2) == bytes.fromhex('73 5d')
    print('PASS: build-id, 28 guards, 19 stolen instruction ranges, constants and clamps')
    # Enclosing interior-hook functions from the build-35924 function export.
    functions = [(0x16e4480,13313), (0x16d2c50,3584), (0x172e140,7025),
                 (0x16f6090,12884), (0x16edd30,2919), (0x16ed220,888),
                 (0x16ed5b0,764), (0x154a480,6175), (0x182a6f0,12739),
                 (0x2ecc8b0,1345)]
    ranges.append((constants['kAirPatchRva'], constants['kAirResumeRva'] - constants['kAirPatchRva']))
    for start, size in functions:
        instructions = list(md.disasm(read(start, size), start))
        assert sum(i.size for i in instructions) == size, hex(start)
        boundaries = {i.address for i in instructions} | {start + size}
        for address, stolen_size in ranges:
            if not start <= address < start + size:
                continue
            assert address in boundaries and address + stolen_size in boundaries
            for i in instructions:
                if (i.mnemonic.startswith('j') or i.mnemonic == 'call') and i.operands and i.operands[0].type == X86_OP_IMM:
                    if address == 0x2eccc54 and i.address == 0x2eccd51:
                        # Dead unsigned-conversion tail: its sole entry was the
                        # JS inside the removed window. No preceding fallthrough.
                        assert i.operands[0].imm == 0x2eccc6a
                        incoming = [j.address for j in instructions
                                    if j.mnemonic.startswith('j') and j.operands
                                    and j.operands[0].type == X86_OP_IMM
                                    and 0x2eccd33 <= j.operands[0].imm <= 0x2eccd51]
                        assert incoming == [0x2eccc5b]
                        assert read(0x2eccd2e, 5) == bytes.fromhex('e9 93 fd ff ff')
                        print('PASS AirConnectParts old interior branch is unreachable after rewrite')
                        continue
                    assert not address < i.operands[0].imm < address + stolen_size, (hex(address), hex(i.address))
        print(f'PASS function {start:#x}: boundaries and no live direct branches into replaced interiors')
