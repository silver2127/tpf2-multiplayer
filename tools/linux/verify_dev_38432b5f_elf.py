#!/usr/bin/env python3
"""Read-only verification of dev 38432b5f native paused tick and load progress."""
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
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    for name, count in [('paused', 6), ('progress', 4)]:
        source = (root/f'native/linux/src/{name}_checks_linux.h').read_text()
        checks = re.findall(r'\{(0x[0-9a-f]+), "([^"\n]+)", (\d+)\}', source)
        assert len(checks) == count
        for address, data, size in checks:
            addr, size = int(address, 16), int(size)
            expected = bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', data))
            assert len(expected) == size and read(addr, size) == expected, address
            assert sum(i.size for i in md.disasm(expected, addr)) == size, address
    insns = list(md.disasm(read(0xa61250, 0x69d), 0xa61250))
    byaddr = {i.address: i for i in insns}
    paused, running, advance = 0xa61860, 0xa617d6, 0x179d840
    for site in (paused, running):
        ins = byaddr[site]
        assert ins.mnemonic == 'call' and ins.size == 5 and ins.operands[0].imm == advance
        assert site+5 in byaddr
    assert byaddr[0xa61854].mnemonic == 'xor' and byaddr[0xa61854].op_str == 'edx, edx'
    assert byaddr[0xa617c7].op_str == 'edx, 1'
    for ins in insns:
        if (ins.mnemonic.startswith('j') or ins.mnemonic == 'call') and ins.operands and ins.operands[0].type == X86_OP_IMM:
            assert not paused < ins.operands[0].imm < paused+5
    relocs = {r['r_offset']: r['r_addend'] for r in elf.get_section_by_name('.rela.dyn').iter_relocations()}
    assert read(0x4f368c0, len(b'N2UI15ProgressMonitorE\0')) == b'N2UI15ProgressMonitorE\0'
    assert relocs[0x5a33828] == 0x4f368c0 and relocs[0x59d8c58] == 0x5a33820
    assert [relocs[0x59d8c60+i*8] for i in range(4)] == [0x30ebcd0, 0x30ebe00, 0x30ebd30, 0x30ebd10]
    movement = (root/'native/linux/src/slice/movement_linux.cpp').read_text()
    assert 'FlagOff(root,data,"pausedtick")' in movement
    assert 'Tpf2mpCodeWriteSelf(base+0xa61860,nop,sizeof(nop),&error)' in movement
    assert 'Check(base,kPausedChecks)' in movement
    print('PASS: build-id, 10 complete instruction spans, paused/running SysV calls, no interior branch, ProgressMonitor RTTI/vtable')
