#!/usr/bin/env python3
"""Verify hot-join canonical-order sites against build 35924, without running it.
Requires pyelftools and capstone; accepts the game ELF as its sole argument.
"""
from pathlib import Path
import re
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[2]
with Path(sys.argv[1]).open('rb') as stream:
    elf = ELFFile(stream)
    assert elf.header['e_machine'] == 'EM_X86_64' and elf.little_endian
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(addr, size):
        s = next(s for s in loads if s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(s['p_offset'] + addr - s['p_vaddr'])
        return stream.read(size)
    source = (root / 'native/linux/src/order_canon_linux.cpp').read_text()
    checks = re.findall(r'\{ "([a-z-]+)", (0x[0-9a-f]+), (\d+), (-?0x[0-9a-f]+),\s*\{([^}]+)\}', source)
    assert len(checks) == 7
    functions = [(0x1502600,1148),(0x16f0ae0,354),(0x16b5790,2198),(0x1700540,5117),(0x2e6e0c0,6794),(0x32567a0,1315),(0x32515b0,156)]
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    for (name, address, steal, offset, values),(start,size) in zip(checks,functions):
        address,steal = int(address,16),int(steal)
        expected=bytes(int(v.strip(),16) for v in values.split(','))
        assert read(address,len(expected))==expected, name
        instructions=list(md.disasm(read(start,size),start))
        assert sum(i.size for i in instructions)==size, name
        assert any(i.address==address for i in instructions), name
        assert any(i.address==address+steal for i in instructions), name
        for i in instructions:
            if (i.mnemonic.startswith('j') or i.mnemonic=='call') and i.operands and i.operands[0].type==X86_OP_IMM:
                assert not address<i.operands[0].imm<address+steal, (name,i.address)
        print(f'PASS: {name}: {address:#x}, {expected.hex(" ")}, {steal} bytes; base offset {offset}')
    assert read(0xa914c0,9) == bytes.fromhex('f3 0f 1e fa 48 8d 47 08 c3')
    assert read(0xa914e0,7) == bytes.fromhex('f3 0f 1e fa 31 c0 c3')
    for n in range(1,6):
        vft=0x59ac260+0x20*(n-1)
        ti=struct.unpack('<Q',read(vft-8,8))[0]
        name=struct.unpack('<Q',read(ti+8,8))[0]
        assert read(name,21).split(b'\0')[0] == f'N3ecs8NodeListILi{n}EEE'.encode()
    assert read(0xa617e8,5) == bytes.fromhex('e8 c3 fd 7e 02')
    print('PASS: family getters, NodeList<1..5> RTTI/vtables, Step iteration call')
    print('PASS: build-id, all seven byte guards, instruction boundaries, no interior branches')
