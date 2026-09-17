#!/usr/bin/env python3
"""Read-only verification of construction/asset runtime probes against build 35924."""
import argparse
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD_ID = '3a0e156390b0e6f1e372051c24802c8493ae454a'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    data = args.game.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    if BUILD_ID not in subprocess.check_output(['readelf', '-n', str(args.game)], text=True):
        raise ValueError('unknown build-id')
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, offset + i*stride) for i in range(count)]
    checked = 0
    for name in ('slice_construction_checks.h', 'slice_terrain_assets_checks.h'):
        source = (ROOT / 'native/linux/src/slice' / name).read_text()
        probes = re.findall(r'\{\s*(0x[\da-f]+),\s*\{([^}]+)\},\s*(\d+)\s*\}', source)
        if not probes:
            raise ValueError('no probes in ' + name)
        for address, raw, length in probes:
            rva = int(address, 16)
            expected = bytes(int(x.strip(), 16) for x in raw.split(',') if x.strip())
            if len(expected) != int(length):
                raise ValueError('probe length mismatch')
            for kind, flags, fileoff, virtual, _, filesz, _, _ in segments:
                if kind == 1 and flags & 1 and virtual <= rva and rva + len(expected) <= virtual + filesz:
                    actual = data[fileoff+rva-virtual:fileoff+rva-virtual+len(expected)]
                    if actual != expected:
                        raise ValueError(f'{name} {address}: {actual.hex()} != {expected.hex()}')
                    break
            else:
                raise ValueError('probe outside executable segment: ' + address)
            checked += 1
    print(f'PASS: build-id and {checked} construction/asset probes; ELF read only')

if __name__ == '__main__':
    main()
