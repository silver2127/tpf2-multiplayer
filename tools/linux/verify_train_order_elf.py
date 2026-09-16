#!/usr/bin/env python3
"""Verify train reservation-order sites against build 35924, without running it.
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
    source = (root / 'native/linux/src/slice/train_order_checks.h').read_text()
    checks = re.findall(r'\{(0x[0-9a-f]+), \{([^}]+)\}, (\d+)\}', source)
    assert len(checks) == 16
    for addr, bytes_, size in checks:
        expected = bytes(int(x, 16) for x in bytes_.split(','))
        assert len(expected) == int(size)
        assert read(int(addr, 16), int(size)) == expected, addr
    for site, target in [(0x175849d, 0x175a510), (0x1758437, 0xc0cf10),
                         (0xc0cf20, 0xc11230), (0x149114e, 0x9e4ab0)]:
        data = read(site, 5)
        assert data[0] == 0xe8 and site + 5 + struct.unpack('<i', data[1:])[0] == target
    name = struct.unpack('<Q', read(0x5a02608, 8))[0]
    assert read(name, 22) == b'N3ecs9component4NameE\0'
    # Patch is exactly one complete CALL: nothing else is skipped. Decode all
    # of Update2 to establish the boundary and reject interior branch targets.
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    insns = list(md.disasm(read(0x1758160, 7234), 0x1758160))
    site = next(i for i in insns if i.address == 0x175849d)
    assert site.mnemonic == 'call' and site.size == 5
    for ins in insns:
        if (ins.mnemonic.startswith('j') or ins.mnemonic == 'call') and ins.operands and ins.operands[0].type == X86_OP_IMM:
            assert not 0x175849d < ins.operands[0].imm < 0x17584a2
    print('PASS: build-id, 16 runtime byte checks, shuffle/seed/type calls, Name RTTI, CALL boundary and Update2 branches')
