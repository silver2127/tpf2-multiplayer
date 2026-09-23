#!/usr/bin/env python3
"""Check the released lobby's actual TLS relocation regression and archive preservation."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import fix_lobby_relocations as repair


def loaded_tls(data, base):
    """Map PE sections and execute its DIR64 relocation records in order."""
    pe = repair.PE(data)
    image = bytearray(pe.image_size)
    for virtual_size, rva, size, offset in pe.sections:
        image[rva:rva + size] = data[offset:offset + size]
    for rva, kind, _ in pe.relocations():
        assert kind == 10
        value = struct.unpack_from("<Q", image, rva)[0]
        struct.pack_into("<Q", image, rva, (value + base - pe.base) & 0xffffffffffffffff)
    return struct.unpack_from("<QQQQ", image, pe.directory(9)[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="official 0.4.22 netpunch.exe")
    args = parser.parse_args()
    original = args.input.read_bytes()
    archive = repair.Archive(original)
    original_dll = archive.unpack(next(e for e in archive.entries if e.name == repair.DLL_NAME))
    assert loaded_tls(original_dll, 0x7abf0000)[2] == 0x8aa7c04c  # actual crash address
    with tempfile.TemporaryDirectory(prefix="tpf2mp-reloc-test-") as temporary:
        root = Path(temporary)
        output = root / "repaired.exe"
        command = [sys.executable, str(Path(repair.__file__)), "--input", str(args.input), "--output", str(output)]
        first = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert first.returncode == 0, first.stderr
        fixed = output.read_bytes()
        assert repair.sha256(fixed) == repair.REPAIRED_SHA256
        rebuilt = repair.Archive(fixed)
        assert len(archive.entries) == len(rebuilt.entries) == 74
        changed = []
        for before, after in zip(archive.entries, rebuilt.entries):
            assert (before.name, before.kind, before.compressed, before.unpacked) == (after.name, after.kind, after.compressed, after.unpacked)
            if archive.unpack(before) != rebuilt.unpack(after):
                changed.append(before.name)
            else:
                assert archive.packed(before) == rebuilt.packed(after)
        assert changed == [repair.DLL_NAME]
        fixed_dll = rebuilt.unpack(next(e for e in rebuilt.entries if e.name == repair.DLL_NAME))
        assert repair.sha256(fixed_dll) == repair.REPAIRED_DLL_SHA256
        for base in (0x7abf0000, 0x100000000, 0x6ffff0000000):
            assert loaded_tls(fixed_dll, base) == tuple(base + rva for rva in (0x20000, 0x20008, 0x1c04c, 0x1f030))
        stamp = output.stat().st_mtime_ns
        assert subprocess.run(command, capture_output=True, timeout=30).returncode == 0
        assert output.stat().st_mtime_ns == stamp
        bad = root / "changed-input.exe"
        bad.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        rejected_output = root / "must-not-exist.exe"
        rejected = subprocess.run([sys.executable, str(Path(repair.__file__)), "--input", str(bad), "--output", str(rejected_output)], capture_output=True, text=True, timeout=30)
        assert rejected.returncode != 0 and "Unknown input" in rejected.stderr
        assert not rejected_output.exists()
        assert args.input.read_bytes() == original
    print("PASS: exact crash reproduced in PE simulation, TLS correct at three rebases, only one of 74 archive members changed, unknown input rejected, repeat output unchanged")


if __name__ == "__main__":
    main()
