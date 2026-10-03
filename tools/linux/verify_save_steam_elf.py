#!/usr/bin/env python3
"""Check save-compression and Steam-poll guards/calls against the build-35924 ELF."""
from pathlib import Path
import re
import sys
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'native/linux/src/slice'


def literals(text):
    return bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', text))


with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

    def read(address, size):
        segment = next(s for s in loads if s['p_vaddr'] <= address and
                       address + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(segment['p_offset'] + address - segment['p_vaddr'])
        return stream.read(size)

    def guard(address, expected):
        assert read(address, len(expected)) == expected, hex(address)
        print(f'PASS: whole-function guard {address:#x}, {len(expected)} bytes')

    def call(site, target):
        code = read(site, 5)
        assert code[0] == 0xe8 and site + 5 + int.from_bytes(code[1:], 'little', signed=True) == target
        print(f'PASS: SysV call {site:#x} -> {target:#x}')

    save = (SOURCE / 'save_zstd_linux.cpp').read_text()
    guards = re.findall(r'\{(0x[0-9a-f]+), (\d+),\s*//[^\n]*\n(.*?)\}', save, re.S)
    assert len(guards) == 4
    for address, size, body in guards:
        expected = literals(body)
        assert len(expected) == int(size)
        guard(int(address, 16), expected)
    constants = {name: int(value, 16) for name, value in
                 re.findall(r'(k\w+) = (0x[0-9a-f]+)', save)}
    sites = re.findall(r'\{(0x[0-9a-f]+), (k\w+), \(void\*\)&Hook', save)
    assert len(sites) == 6
    for at, callee in sites:
        call(int(at, 16), constants[callee])
    poll = (SOURCE / 'steam_poll_linux.cpp').read_text()
    constants = {name: int(value, 16) for name, value in
                 re.findall(r'(k\w+) = (0x[0-9a-f]+)', poll)}
    expected = literals(poll.split('const char kLoopBytes[] =')[1].split(';')[0])
    assert len(expected) == constants['kLoopSize']
    guard(constants['kLoop'], expected)
    call(constants['kSite'], constants['kPoll'])
    print('PASS: build-id, 5 function guards and 7 call targets')
