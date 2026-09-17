#!/usr/bin/env python3
"""Read-only checks for Linux 35924 company UI branches; never executes the ELF."""
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
    def unhex(s):
        return bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', s))
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    source = (root/'native/linux/src/company_ui_checks_linux.h').read_text()
    checks = re.findall(r'\{(0x[0-9a-f]+), "([^"\n]+)", (\d+)\}', source)
    assert len(checks) == 10
    for address, data, size in checks:
        addr, size, data = int(address, 16), int(size), unhex(data)
        assert len(data) == size and read(addr, size) == data, address
        assert sum(i.size for i in md.disasm(data, addr)) == size, address
    movement = (root/'native/linux/src/slice/movement_linux.cpp').read_text()
    patches = re.findall(r'\{(0x[0-9a-f]+), "([^"\n]+)", "([^"\n]+)", (\d+)\}', movement)
    assert len(patches) == 5
    original_targets = [0x1383b00, 0x138bacf, 0x138ba71, 0x1090e18, 0x1446e0a]
    bodies = [(0x13837d0, 1385), (0x138b9f0, 4381), (0x1090ba0, 1963), (0x1446c60, 1156)]
    instructions = [i for a, n in bodies for i in md.disasm(read(a, n), a)]
    boundaries = {i.address for i in instructions}
    for (address, old, new, size), target in zip(patches, original_targets):
        a, size, old, new = int(address, 16), int(size), unhex(old), unhex(new)
        assert read(a, size) == old and len(new) == size
        before = list(md.disasm(old, a)); after = list(md.disasm(new, a))
        assert len(before) == 1 and before[0].operands[0].imm == target
        assert a in boundaries and a+size in boundaries and target in boundaries
        assert sum(i.size for i in after) == size
        if before[0].mnemonic == 'je':
            assert after[0].mnemonic == 'jmp' and after[0].operands[0].imm == target
        else:
            assert before[0].mnemonic == 'jne' and all(i.mnemonic == 'nop' for i in after)
        for i in instructions:
            if (i.mnemonic.startswith('j') or i.mnemonic == 'call') and i.operands and i.operands[0].type == X86_OP_IMM:
                assert not a < i.operands[0].imm < a+size
    rs = {r['r_offset']: r['r_addend'] for r in elf.get_section_by_name('.rela.dyn').iter_relocations()}
    name = b'N3ecs9component11PlayerOwnedE\0'
    assert read(rs[0x5a01c20], len(name)) == name
    # Itanium ViewCreator: two destructors, then CanCreateView in slot 2.
    assert rs[0x59bc448] == 0x1446c60
    for flag in ['showicons', 'foreignwindows']:
        assert f'FlagOff(root,data,"{flag}")' in movement
    print('PASS: build-id, 10 guard spans, five original/modified branches, instruction boundaries, no interior targets, PlayerOwned RTTI and ViewCreator vtable')
