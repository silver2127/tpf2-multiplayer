#!/usr/bin/env python3
"""Read-only verification of UCRT parity guards and ELF import identities."""
from pathlib import Path
import re
import struct
import subprocess
import sys

path = Path(sys.argv[1])
image = path.read_bytes()
assert image[:6] == b'\x7fELF\x02\x01'
assert '3a0e156390b0e6f1e372051c24802c8493ae454a' in subprocess.check_output(['readelf', '-n', path], text=True)
phoff = struct.unpack_from('<Q', image, 32)[0]
entsize, count = struct.unpack_from('<HH', image, 54)
segments = [struct.unpack_from('<IIQQQQQQ', image, phoff+i*entsize) for i in range(count)]
def read(addr, size):
    s = next(s for s in segments if s[0] == 1 and s[3] <= addr and addr+size <= s[3]+s[5])
    offset = s[2]+addr-s[3]
    return image[offset:offset+size]
header = (Path(__file__).resolve().parents[2]/'native/linux/src/libm_parity_sites_linux.h').read_text()
header = re.sub(r'//[^\n]*', '', header)
relocs = subprocess.check_output(['readelf', '-rW', path], text=True)
slots = re.findall(r'kLibmGot\w+\{(0x[0-9a-f]+), "(\w+)"\}', header)
assert len(slots) == 6
for addr, name in slots:
    addr = int(addr, 16)
    assert re.search(rf'^{addr:016x}\s+\S+\s+R_X86_64_JUMP_SLOT\s+\S+\s+{name}@', relocs, re.M)
    assert any(s[0] == 0x6474e552 and s[3] <= addr and addr+8 <= s[3]+s[6] for s in segments)
    print(f'PASS {name} import {addr:#x}, RELRO')
dynamic = subprocess.check_output(['readelf', '-dW', path], text=True)
assert 'BIND_NOW' in dynamic
plt = int(re.search(r'kLibmPltAtan2Rva = (0x[0-9a-f]+)', header)[1], 16)
code = read(plt, 6)
assert code[:2] == b'\xff\x25'
slot = plt+6+struct.unpack_from('<i', code, 2)[0]
assert re.search(rf'^{slot:016x}\s+\S+\s+R_X86_64_JUMP_SLOT\s+\S+\s+atan2@', relocs, re.M)
sites = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+),\s*\{([^}]+)\}, (\d+)\}', header)
assert len(sites) == 3
for call, guard, data, size in sites:
    call, guard, size = int(call,16), int(guard,16), int(size)
    expected = bytes(int(v,16) for v in re.findall(r'0x[0-9a-f]+',data))
    assert len(expected) == size and read(guard,size) == expected
    code = read(call,5)
    assert code[0] == 0xe8 and call+5+struct.unpack_from('<i',code,1)[0] == plt
    assert expected[-4:-1] == b'\xf2\x0f\x5a' # full result narrowing instruction
    print(f'PASS atan2 call {call:#x}, full {size}-byte conversion guard')
print('PASS: build-id, six resolved imports, RELRO/BIND_NOW, three guarded atan2 calls')
