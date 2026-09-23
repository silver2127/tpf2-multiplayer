#!/usr/bin/env python3
"""Repair duplicate miniupnpc PE relocations inside the official 0.4.22 lobby.

Python standard library only. No Python bytecode or game/mod DLL is rebuilt.
The PyInstaller CArchive cookie/TOC formats are documented in its archive reader.
"""
import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

OFFICIAL_SHA256 = "54bed86eaa76a54c70560f380716fef9bf5bded6ac549df4643853e6a7e6882c"
DLL_SHA256 = "820742e7c52efab376b653e00b81c64d1174c0779cefb5621f8e72833e620642"
DLL_NAME = "miniupnpc-5520bde33208435242b8509993047f10.dll"
REPAIRED_SHA256 = "da4fb91961f1aed3c7e7543a837220902a7af11a0ea59412dd7b77bfd001111f"
REPAIRED_DLL_SHA256 = "0e8e86395f7d85bfb7e861bd055962fc613419bcab79c4b11c480ace7b2878ee"
COOKIE = struct.Struct("!8sIIII64s")
TOC = struct.Struct("!IIIIBc")
MAGIC = b"MEI\014\013\012\013\016"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class PE:
    def __init__(self, data):
        self.data = data
        require(data[:2] == b"MZ", "Missing PE DOS header")
        self.header = struct.unpack_from("<I", data, 0x3c)[0]
        require(data[self.header:self.header + 4] == b"PE\0\0", "Missing PE signature")
        require(struct.unpack_from("<H", data, self.header + 4)[0] == 0x8664, "Expected x64 PE")
        self.optional = self.header + 24
        require(struct.unpack_from("<H", data, self.optional)[0] == 0x20b, "Expected PE32+")
        self.base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        self.image_size = struct.unpack_from("<I", data, self.optional + 56)[0]
        self.checksum_offset = self.optional + 64
        require(self.directory(4) == (0, 0), "Signed input is not supported")
        section_start = self.optional + struct.unpack_from("<H", data, self.header + 20)[0]
        self.sections = []
        for index in range(struct.unpack_from("<H", data, self.header + 6)[0]):
            self.sections.append(struct.unpack_from("<IIII", data, section_start + index * 40 + 8))

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.optional + 112 + index * 8)

    def offset(self, rva, length=1):
        for virtual_size, start, raw_size, offset in self.sections:
            if start <= rva and rva + length <= start + raw_size:
                require(offset + rva - start + length <= len(self.data), "PE section exceeds file")
                return offset + rva - start
        raise ValueError(f"RVA {rva:#x} has no complete raw section data")

    def relocations(self):
        current, size = self.directory(5)
        end = current + size
        while current < end:
            page, length = struct.unpack_from("<II", self.data, self.offset(current, 8))
            require(length >= 8 and length % 2 == 0 and current + length <= end, "Malformed relocation block")
            block = self.offset(current, length)
            for entry_offset in range(block + 8, block + length, 2):
                word = struct.unpack_from("<H", self.data, entry_offset)[0]
                if word >> 12:
                    yield page + (word & 0xfff), word >> 12, entry_offset
            current += length


def set_checksum(data):
    result = bytearray(data)
    checksum_offset = PE(result).checksum_offset
    struct.pack_into("<I", result, checksum_offset, 0)
    padded = bytes(result) + (b"\0" if len(result) % 2 else b"")
    total = sum(word[0] for word in struct.iter_unpack("<H", padded))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    struct.pack_into("<I", result, checksum_offset, total + len(result))
    return bytes(result)


def repair_dll(data):
    require(sha256(data) == DLL_SHA256, "Unknown miniupnpc DLL; refusing to repair")
    pe = PE(data)
    require(pe.base == 0x6ad80000 and pe.directory(9) == (0x183a0, 40), "Unexpected miniupnpc TLS layout")
    result = bytearray(data)
    seen, duplicates = set(), []
    for rva, kind, position in pe.relocations():
        require(kind == 10 and rva + 8 <= pe.image_size, "Unexpected relocation type or target")
        if rva in seen:
            duplicates.append(position)
            struct.pack_into("<H", result, position, 0)  # IMAGE_REL_BASED_ABSOLUTE padding
        seen.add(rva)
    require(len(duplicates) == 64, f"Expected 64 duplicated DIR64 relocations, found {len(duplicates)}")
    result = set_checksum(result)
    allowed = {p + i for p in duplicates for i in range(2)} | set(range(pe.checksum_offset, pe.checksum_offset + 4))
    require(all(a == b or i in allowed for i, (a, b) in enumerate(zip(data, result))), "Unexpected DLL change")
    remaining = [(rva, kind) for rva, kind, _ in PE(result).relocations()]
    require(len(remaining) == len(set(remaining)) == 2115, "Repaired relocations are not unique")
    require(sha256(result) == REPAIRED_DLL_SHA256, "Repaired DLL does not match the independently verified result")
    verify_tls(result)
    return result


def verify_tls(data):
    """Simulate the actual loader at several bases, including the observed crash."""
    pe = PE(data)
    tls, _ = pe.directory(9)
    original = struct.unpack_from("<QQQQ", data, pe.offset(tls, 32))
    counts = {tls + i * 8: 0 for i in range(4)}
    for rva, kind, _ in pe.relocations():
        if rva in counts:
            require(kind == 10, "Unexpected TLS relocation type")
            counts[rva] += 1
    for new_base in (0x7abf0000, 0x100000000, 0x6ffff0000000):
        actual = [value + counts[tls + i * 8] * (new_base - pe.base) for i, value in enumerate(original)]
        require(actual == [new_base + 0x20000, new_base + 0x20008, new_base + 0x1c04c, new_base + 0x1f030],
                f"TLS fields relocate incorrectly at base {new_base:#x}")


@dataclass
class Entry:
    record: int
    length: int
    offset: int
    size: int
    unpacked: int
    compressed: int
    kind: bytes
    name: str


class Archive:
    def __init__(self, data):
        self.data = data
        self.cookie = COOKIE.unpack_from(data, len(data) - COOKIE.size)
        magic, size, self.toc_offset, self.toc_length, python, library = self.cookie
        require(magic == MAGIC and python == 312 and library.rstrip(b"\0") == b"python312.dll", "Unexpected CArchive cookie")
        self.start = len(data) - size
        require(self.start > 0 and self.toc_offset + self.toc_length + COOKIE.size == size, "Unexpected CArchive bounds")
        self.toc = data[self.start + self.toc_offset:-COOKIE.size]
        self.entries = []
        position = 0
        while position < self.toc_length:
            length, offset, size, unpacked, compressed, kind = TOC.unpack_from(self.toc, position)
            require(length >= TOC.size + 1 and position + length <= self.toc_length, "Malformed CArchive TOC")
            name = self.toc[position + TOC.size:position + length].rstrip(b"\0").decode("utf-8")
            require(offset + size <= self.toc_offset and compressed in (0, 1), "CArchive entry exceeds payload")
            self.entries.append(Entry(position, length, offset, size, unpacked, compressed, kind, name))
            position += length

    def packed(self, entry):
        return self.data[self.start + entry.offset:self.start + entry.offset + entry.size]

    def unpack(self, entry):
        result = zlib.decompress(self.packed(entry)) if entry.compressed else self.packed(entry)
        require(len(result) == entry.unpacked, f"Wrong CArchive member size: {entry.name}")
        return result


def stored_zlib(data):
    # Fixed DEFLATE stored blocks make output independent of zlib compressor versions.
    result = bytearray(b"\x78\x01")
    for position in range(0, len(data), 65535):
        block = data[position:position + 65535]
        result += bytes([int(position + len(block) == len(data))])
        result += struct.pack("<HH", len(block), len(block) ^ 0xffff) + block
    result += struct.pack("!I", zlib.adler32(data))
    require(zlib.decompress(result) == data, "Internal compression verification failed")
    return bytes(result)


def repair_executable(data):
    require(sha256(data) == OFFICIAL_SHA256, "Unknown input executable; require the official Windows 0.4.22 netpunch.exe")
    archive = Archive(data)
    targets = [entry for entry in archive.entries if entry.name == DLL_NAME]
    require(len(targets) == 1 and targets[0].kind == b"b" and targets[0].compressed == 1, "Expected exactly one compressed miniupnpc DLL")
    target = targets[0]
    fixed_dll = repair_dll(archive.unpack(target))
    packed = stored_zlib(fixed_dll)
    delta = len(packed) - target.size
    toc = bytearray(archive.toc)
    for entry in archive.entries:
        if entry == target:
            struct.pack_into("!I", toc, entry.record + 8, len(packed))
        elif entry.kind != b"o":
            require(entry.size == 0 or entry.offset + entry.size <= target.offset or entry.offset >= target.offset + target.size,
                    "Overlapping CArchive members")
            if entry.offset >= target.offset + target.size:
                struct.pack_into("!I", toc, entry.record + 4, entry.offset + delta)
    cookie = list(archive.cookie)
    cookie[1] += delta
    cookie[2] += delta
    result = (data[:archive.start + target.offset] + packed
              + data[archive.start + target.offset + target.size:archive.start + archive.toc_offset]
              + toc + COOKIE.pack(*cookie))
    result = set_checksum(result)
    rebuilt = Archive(result)
    require(len(rebuilt.entries) == len(archive.entries), "CArchive member count changed")
    for before, after in zip(archive.entries, rebuilt.entries):
        require((before.name, before.kind, before.compressed, before.unpacked) ==
                (after.name, after.kind, after.compressed, after.unpacked), "CArchive entry metadata changed")
        if before == target:
            require(rebuilt.unpack(after) == fixed_dll, "Repaired DLL did not round trip")
        else:
            require(archive.packed(before) == rebuilt.packed(after) and archive.unpack(before) == rebuilt.unpack(after),
                    f"Unrelated CArchive member changed: {before.name}")
        if before.kind == b"o":
            require(archive.toc[before.record:before.record + before.length] ==
                    rebuilt.toc[after.record:after.record + after.length], "Runtime option record changed")
    checksum = PE(data).checksum_offset
    require(all(a == b or checksum <= i < checksum + 4 for i, (a, b) in enumerate(zip(data[:archive.start], result[:archive.start]))),
            "Bootloader changed outside its checksum")
    require(sha256(result) == REPAIRED_SHA256, "Repaired executable does not match the pinned result")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, target = args.input.resolve(), args.output.absolute()
    require(source != target.resolve(), "Output must be separate from the official input")
    repaired = repair_executable(source.read_bytes())
    require(not target.is_symlink(), "Output cannot be a symlink")
    if target.exists():
        require(target.read_bytes() == repaired, "Output exists with different contents; choose a fresh output path")
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=".netpunch-repair-", dir=target.parent)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(repaired)
            os.chmod(temporary, source.stat().st_mode & 0o777)
            os.replace(temporary, target)
        finally:
            Path(temporary).unlink(missing_ok=True)
    print(f"PASS: repaired 64 duplicate relocations; all other archive members and runtime options preserved\nSHA256 {sha256(repaired)}\nOutput: {target}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error, zlib.error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
