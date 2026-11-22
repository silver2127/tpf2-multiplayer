#!/usr/bin/env python3
"""Check native speed-hook guards and batch-boundary contract in build 35924.
Usage: verify_speedhook_elf.py /path/to/TransportFever2 (pyelftools, capstone).
"""
from pathlib import Path
import re
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile

source = (Path(__file__).resolve().parents[2] / 'native/linux/src/speedhook_linux.cpp').read_text()
with Path(sys.argv[1]).open('rb') as stream:
    elf = ELFFile(stream)
    assert elf.header['e_machine'] == 'EM_X86_64' and elf.little_endian
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(addr, size):
        seg = next(s for s in loads if s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(seg['p_offset'] + addr - seg['p_vaddr'])
        return stream.read(size)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    # Sizes from the Linux functions export; check the new complete function
    # for branches into the displaced prologue as well as its exact guard.
    for name, constant, size in [('GETTER', 'GETSPEED', None), ('CGAME_STEP', 'CGAME_STEP', 322), ('CGAME_SYNC', 'CGAME_SYNC', 3940)]:
        address = int(re.search(r'RVA_' + constant + r'\s*=\s*(0x[0-9a-f]+)', source)[1], 16)
        values = re.search(name + r'_EXPECTED\[\d+\] = \{(.*?)\};', source, re.S)[1]
        values = re.sub(r'//[^\n]*', '', values)
        expected = bytes(int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', values))
        assert read(address, len(expected)) == expected, name
        instructions = list(md.disasm(read(address, size or len(expected)), address))
        assert sum(i.size for i in instructions) == (size or len(expected)), name
        assert sum(i.size for i in instructions if i.address < address + len(expected)) == len(expected), name
        for ins in instructions:
            if (ins.mnemonic.startswith('j') or ins.mnemonic == 'call') and ins.operands and ins.operands[0].type == X86_OP_IMM:
                assert not address < ins.operands[0].imm < address + len(expected), (name, ins.address)
        print(f'PASS: {name} {address:#x}: {expected.hex(" ")}')
    for site, target in [(0xa616b9, 0xc0dc30), (0xa61710, 0xc0dc30), (0xa31c66, 0xa30cc0)]:
        call = read(site, 5)
        assert call[0] == 0xe8 and site + 5 + struct.unpack('<i', call[1:])[0] == target
    for address, expected in [
        (0xa30cd8, '48 89 75 98'),             # callback reference from SysV RSI
        (0xa310be, '49 8b 87 60 01 00 00'),    # CGame m_data
        (0xa310c5, '44 89 b0 a8 01 00 00'),    # fresh interval written inside Sync
        (0xa31c60, '4c 89 e6 4c 89 ef'),       # Step passes callback and this
        (0xa31c6b, '84 c0'),                   # bool return
        (0xa31c8f, '48 63 9a a8 01 00 00'),    # next due-batch test
        (0xa31cd2, '48 63 b2 a8 01 00 00'),    # interpolation interval after Sync
    ]:
        expected = bytes.fromhex(expected)
        assert read(address, len(expected)) == expected, hex(address)
    print('PASS: build-id, guards, instruction boundaries, no interior branches, calls, SysV arguments and interval accesses')
