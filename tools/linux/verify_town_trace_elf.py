#!/usr/bin/env python3
"""Check the native town trace's guard and SysV data flow against build 35924.
Usage: python3 tools/linux/verify_town_trace_elf.py GAME_ELF
Requires pyelftools and capstone; never executes the image.
"""
from pathlib import Path
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

    def read(addr, size):
        seg = next(s for s in loads if s['p_vaddr'] <= addr
                   and addr + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(seg['p_offset'] + addr - seg['p_vaddr'])
        return stream.read(size)

    def check_array(file, name, address):
        source = (ROOT / file).read_text()
        body = re.search(r'unsigned char ' + name + r'\[\] = \{(.*?)\};', source, re.S)[1]
        body = re.sub(r'//[^\n]*', '', body)
        guard = bytes(int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]{2}', body))
        assert read(address, len(guard)) == guard, name
        print(f'PASS: {name}: {len(guard)} bytes at {address:#x}')

    check_array('native/linux/src/town_trace_linux.cpp', 'kGuard', 0x17478d8)
    check_array('native/linux/src/town_seed_linux.cpp', 'kTownContextBytes', 0x1746790)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    insns = list(md.disasm(read(0x1746790, 7322), 0x1746790))
    assert sum(i.size for i in insns) == 7322
    by_addr = {i.address: (i.mnemonic, i.op_str) for i in insns}
    expected = {
        0x17467ae: ('mov', 'qword ptr [rbp - 0xbf8], rdi'),
        0x17467bc: ('mov', 'qword ptr [rbp - 0xbe8], rsi'),
        0x1746825: ('lea', 'rdi, [rbp - 0xa00]'),
        0x1746847: ('mov', 'qword ptr [rbp - 0xc58], rdi'),
        0x174688c: ('mov', 'qword ptr [rbp - 0x40], 0x270'),
        0x1746930: ('mov', 'rdi, qword ptr [rbp - 0xbf8]'),
        0x1746937: ('mov', 'rax, qword ptr [rdi + 8]'),
        0x174693f: ('mov', 'r13, qword ptr [rax]'),
        0x1746973: ('lea', 'r13, [r13 + rbx*8]'),
        0x174698c: ('mov', 'eax, dword ptr [r13]'),
        0x17478d8: ('mov', 'rax, qword ptr [rbp - 0xbf8]'),
        0x17478e3: ('xor', 'ecx, ecx'),
        0x17478e5: ('mov', 'edx, r13d'),
        0x17478f9: ('mov', 'r8, qword ptr [rbp - 0xc58]'),
        0x1747900: ('mov', 'rdi, qword ptr [rax + 0x40]'),
        0x174790b: ('push', '0'),
        0x174790d: ('mov', 'rsi, qword ptr [rbp - 0xbe8]'),
        0x1747914: ('mov', 'r9, rax'),
        0x1747917: ('call', '0x14f5b80'),
    }
    for addr, want in expected.items():
        assert by_addr[addr] == want, (hex(addr), by_addr[addr], want)
    for slot, origin in [('qword ptr [rbp - 0xbf8]', 0x17467ae),
                         ('qword ptr [rbp - 0xbe8]', 0x17467bc),
                         ('qword ptr [rbp - 0xc58]', 0x1746847)]:
        writes = [i.address for i in insns if i.address <= 0x1747917
                  and i.op_str.startswith(slot + ',')]
        assert writes == [origin], (slot, writes)
    assert 0xa00 - 0x40 == 624 * 4
    print('PASS: context/engine spills, node stride/entity, 2504-byte MT, seven SysV arguments and Develop target')
