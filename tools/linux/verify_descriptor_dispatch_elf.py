#!/usr/bin/env python3
"""Check the native descriptor dispatcher against the build-35924 ELF (read-only)."""
from pathlib import Path
import re
import sys
from elftools.elf.elffile import ELFFile

source = (Path(__file__).resolve().parents[2] / 'native/linux/src/overlay_vk_linux.cpp').read_text()
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
    assert read(0x35096e9, 5) == bytes.fromhex('e8 e2 d0 00 00')
    block = source.split('RESET_POOL_LOOKUP[] = {')[1].split('};')[0]
    block = re.sub(r'//[^\n]*', '', block)
    guard = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-f]{2})', block))
    assert len(guard) == 40 and read(0x35181e1, len(guard)) == guard
    # Each named lookup is followed by the gdpa call and a store of its result.
    for name, lea, call, store, slot in [
        ('vkAllocateDescriptorSets', 0x3516840, 0x351684e, 0x351685e, 0x28),
        ('vkDestroyDescriptorPool', 0x3517772, 0x3517783, 0x3517793, 0x5a8),
        ('vkDestroyDescriptorSetLayout', 0x3517789, 0x351779a, 0x35177aa, 0x5b0),
        ('vkFreeDescriptorSets', 0x3517a1f, 0x3517a30, 0x3517a40, 0x6e8),
        ('vkResetDescriptorPool', 0x35181e1, 0x35181f2, 0x3518202, 0xb28),
    ]:
        instruction = read(lea, 7)
        assert instruction[:3] == bytes.fromhex('48 8d 35')
        string = lea + 7 + int.from_bytes(instruction[3:], 'little', signed=True)
        assert read(string, len(name) + 1) == name.encode() + b'\0'
        assert read(call, 6) == bytes.fromhex('ff 93 98 07 00 00')
        expected = (b'\x48\x89\x43' + bytes([slot]) if slot < 128 else
                    b'\x48\x89\x83' + slot.to_bytes(4, 'little'))
        assert read(store, len(expected)) == expected
        print(f'PASS: {name} lookup {lea:#x}, store {store:#x}, slot +{slot:#x}')
    print('PASS: build-id, init call and 40-byte runtime search-bound guard')
