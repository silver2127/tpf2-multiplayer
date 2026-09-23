#!/usr/bin/env python3
"""Recheck the Linux preview backend against an unmodified build-35924 ELF.

Requires pyelftools and capstone. Does not load, execute, or modify the game.
Usage: python3 tools/linux/verify_preview_elf.py /path/to/TransportFever2
"""
import argparse
from pathlib import Path
import re
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from elftools.elf.elffile import ELFFile

EXPECTED_BUILD_ID = "3a0e156390b0e6f1e372051c24802c8493ae454a"
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "native/linux/src/plugin/preview_plugin_linux.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    source = SOURCE.read_text()
    with args.game.open("rb") as stream:
        elf = ELFFile(stream)
        assert elf.elfclass == 64 and elf.little_endian
        assert elf.header["e_machine"] == "EM_X86_64"
        notes = [n for segment in elf.iter_segments() if segment["p_type"] == "PT_NOTE"
                 for n in segment.iter_notes() if n["n_type"] == "NT_GNU_BUILD_ID"]
        assert any(n["n_desc"] == EXPECTED_BUILD_ID for n in notes), "game build-id differs"
        segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]

        def read(rva, length):
            segment = next(s for s in segments
                           if s["p_vaddr"] <= rva and rva + length <= s["p_vaddr"] + s["p_filesz"])
            stream.seek(segment["p_offset"] + rva - segment["p_vaddr"])
            result = stream.read(length)
            assert len(result) == length
            return result

        def expect(rva, expected):
            actual = read(rva, len(expected))
            assert actual == expected, f"bytes differ at {rva:#x}: {actual.hex()} != {expected.hex()}"

        # Consume the actual runtime signatures, so code and verifier cannot
        # silently disagree about a prologue or stolen instruction length.
        pattern = r'\{(0x[0-9a-f]+),"((?:\\x[0-9a-f]{2})+)",(\d+)'
        signatures = list(re.finditer(pattern, source))
        assert len(signatures) == 17, "expected eight hooks and nine dependencies"
        decoder = Cs(CS_ARCH_X86, CS_MODE_64)
        hook_start, hook_end = source.index("Hook hooks[]="), source.index("for(const auto& hook:hooks)")
        hook_count = 0
        for match in signatures:
            rva, encoded, length = int(match[1], 16), match[2], int(match[3])
            expected = bytes.fromhex(encoded.replace("\\x", ""))
            assert len(expected) == length
            expect(rva, expected)
            if hook_start < match.start() < hook_end:
                instructions = list(decoder.disasm(expected, rva))
                assert instructions and sum(i.size for i in instructions) == length
                assert all("rip" not in i.op_str and i.mnemonic not in ("call", "jmp")
                           and not i.mnemonic.startswith("j") for i in instructions)
                hook_count += 1
        assert hook_count == 8
        color = source.split("const uint8_t colorBytes[]={", 1)[1].split("};", 1)[0]
        expect(0x1391F00, bytes(int(b, 16) for b in re.findall(r"0x[0-9a-f]+", color)))
        table = (0, 0x5A20748, 0x13948D0, 0x1394D00, 0x138EBA0,
                 0x1395340, 0xE160B0, 0x1395910, 0x139C320, 0x1395C90)
        expect(0x59BBA38, struct.pack("<10Q", *table))
        expect(0x426D4E0, b"N2UI15BuilderRendererE\0")
        expect(0x41D3CC0, b"N2UI18CRendererComponentE\0")

        # Independent callsites establish class/operation ownership and calling
        # convention. The two Lua calls are conversion and proposal evaluation.
        calls = {
            0x1971288: 0x2E237B0,
            0xE7D145: 0x13FA4B0,
            0xFFB241: 0x11DA630,
            0x13FA54C: 0x139A1A0,
            0x139A315: 0x1390CC0,
            0x139A359: 0x13A02F0,
            0xEF0BD1: 0x13915E0,
            0xEF0BFA: 0x1394D80,
            0xEF0C0F: 0x1397170,
            0x1397E3A: 0xD0A070,
            0x1399302: 0xD09950,
            0xEF075E: 0x1391F00,
            0x1394D10: 0x13948D0,
            0xE596F3: 0xDE3DF0,
        }
        for site, target in calls.items():
            raw = read(site, 5)
            assert raw[0] == 0xE8 and site + 5 + struct.unpack("<i", raw[1:])[0] == target, hex(site)

        # Allocation, size-delete and offset loads independently anchor storage.
        anchors = {
            0x13FA4F2: "bf b0 01 00 00",       # Renderer allocation 0x1b0
            0x1394D1C: "be b0 01 00 00",      # same sized delete
            0x139A31A: "bf 18 19 00 00",      # state allocation 0x1918
            0x139A35E: "4c 89 a3 98 01 00 00",# renderer.state +0x198
            0xE596F8: "be 00 09 00 00",       # ProposalData sized delete 0x900
            0x1397E2F: "48 8d b7 30 18 00 00",# state height descriptors +0x1830
            0x1397E36: "48 8b 78 50",         # renderer terrain +0x50
            0x19712A3: "b9 0d 00 00 00",      # Context zero 13 qwords (0x68)
            0x19712C1: "49 8d 44 24 48",      # Context single map bucket +0x48
            0x11D95FF: "48 8b 40 10",         # Update virtual slot 2
            0x11D9608: "f3 0f 10 45 cc",      # Update float dt in XMM0
            0x11D960D: "4c 89 e6 ff d0",      # component in RSI, indirect call
        }
        for site, raw in anchors.items():
            expect(site, bytes.fromhex(raw))
    print(f"Preview ELF verified: build-id, {hook_count} complete detours, 9 dependencies, "
          f"setter, RTTI/vtable, {len(calls)} calls and {len(anchors)} layout anchors")


if __name__ == "__main__":
    main()
