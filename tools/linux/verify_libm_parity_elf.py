#!/usr/bin/env python3
"""Read-only verification of UCRT math GOT imports and street call-site guards.
Requires pyelftools and capstone; usage: verify_libm_parity_elf.py GAME_ELF.
"""
from pathlib import Path
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from elftools.elf.elffile import ELFFile

header = (Path(__file__).resolve().parents[2] / 'native/linux/src/libm_parity_sites_linux.h').read_text()
with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    def read(address, size):
        segment = next(s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD'
                       and s['p_vaddr'] <= address and address + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
        return stream.read(size)
    symbols = elf.get_section_by_name('.dynsym')
    imports = {r['r_offset']: symbols.get_symbol(r['r_info_sym']).name
               for r in elf.get_section_by_name('.rela.plt').iter_relocations() if r['r_info_type'] == 7}
    slots = re.findall(r'Tpf2mpGotSlot k\w+\{(0x[0-9a-f]+), "(\w+)"\}', header)
    assert len(slots) == 6
    for address, name in slots:
        assert imports[int(address, 16)] == name
        print('PASS GOT', address, name)
    assert any(t.entry.d_tag == 'DT_BIND_NOW' or
               (t.entry.d_tag == 'DT_FLAGS' and t.entry.d_val & 8)
               for t in elf.get_section_by_name('.dynamic').iter_tags())
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    sites = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+),\s*\{(.*?)\}, (\d+)\}', header, re.S)
    assert len(sites) == 3
    for call, guard, body, size in sites:
        call, guard, size = int(call, 16), int(guard, 16), int(size)
        expected = bytes(int(v, 16) for v in re.findall(r'0x[0-9a-f]+', body))
        assert len(expected) == size and read(guard, size) == expected
        ins = list(md.disasm(read(guard, 48), guard))
        assert [(i.mnemonic, i.op_str) for i in ins if i.address == call] == [('call', '0x6dbc60')]
        assert ins[0].mnemonic == ins[1].mnemonic == 'cvtss2sd'
        assert any(i.mnemonic == 'cvtsd2ss' for i in ins if i.address > call)
        print(f'PASS street {call:#x}: ' + '; '.join(i.mnemonic + ' ' + i.op_str for i in ins[:7]))
    plt = read(0x6dbc60, 6)
    assert plt[:2] == b'\xff\x25'
    slot = 0x6dbc66 + int.from_bytes(plt[2:], 'little', signed=True)
    assert imports[slot] == 'atan2'
    print('PASS double atan2 PLT and eager binding')
