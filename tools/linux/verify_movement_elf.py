#!/usr/bin/env python3
"""Read-only build-35924 verification of movement and station hooks."""
from pathlib import Path
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile
root = Path(__file__).resolve().parents[2]
with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(addr, size):
        s = next(s for s in loads if s['p_vaddr'] <= addr and addr+size <= s['p_vaddr']+s['p_filesz'])
        stream.seek(s['p_offset']+addr-s['p_vaddr'])
        return stream.read(size)
    source = (root/'native/linux/src/slice/movement_checks.h').read_text()
    checks = re.findall(r'\{(0x[0-9a-f]+),\s*((?:"[^"\n]+"\s*)+), (\d+)\}', source)
    assert len(checks) == 7
    for addr, data, size in checks:
        expected = bytes(int(x,16) for x in re.findall(r'\\x([0-9a-f]{2})', data))
        assert len(expected) == int(size)
        assert read(int(addr,16),int(size)) == expected, addr
    menu_source = (root/'native/linux/src/menu_game_linux.cpp').read_text()
    menu_bytes = re.search(r'\{ 0x113f450, G_AUTOLOAD, 16, \{([^}]+)\}', menu_source)
    assert menu_bytes
    expected = bytes(int(x.strip(),16) for x in menu_bytes[1].split(','))
    assert len(expected) == 16 and read(0x113f450,16) == expected
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    for start, size, sites in [
        (0x2e557d0,0x360,[(0x2e557d0,7),(0x2e558e4,7),(0x2e55981,7)]),
        (0x2e55b30,0x8d0,[(0x2e55b30,8),(0x2e55db5,10),(0x2e56014,10)]),
        (0x16d75c0,20378,[(0x16d75c0,8)]),
        (0x16692e0,26895,[(0x16692e0,8)]),
        (0x10c02a0,0x154,[(0x10c03c0,9)]),
        (0x113f450,2324,[(0x113f450,16)])]:
        insns = list(md.disasm(read(start,size), start))
        boundaries = {i.address for i in insns}
        for site, steal in sites:
            assert site in boundaries and site+steal in boundaries
            for ins in insns:
                if (ins.mnemonic.startswith('j') or ins.mnemonic == 'call') and ins.operands and ins.operands[0].type == X86_OP_IMM:
                    assert not site < ins.operands[0].imm < site+steal, (hex(site),hex(ins.address))
    assert read(0x2e558e4,7) == read(0x2e55981,7) == bytes.fromhex('f3 0f 58 c2 48 39 d1')
    # Only these two filtered instructions add into the local float sum.
    insns = list(md.disasm(read(0x2e55b30,0x8d0),0x2e55b30))
    additions = [i.address for i in insns if i.mnemonic == 'addss' and 'rbp - 0x44' in i.op_str]
    assert additions == [0x2e55db5,0x2e56014]
    assert read(0x2e55db5,10) == read(0x2e56014,10) == bytes.fromhex('f3 0f 58 45 bc f3 0f 11 45 bc')
    print('PASS: build-id, 7 movement byte spans and menu prologue, all stolen boundaries, no interior branches, both filtered sums')
