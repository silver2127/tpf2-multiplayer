#!/usr/bin/env python3
"""Read-only verification of the Linux HUD company tint against the game ELF.

Usage: verify_company_tint_elf.py /path/to/TransportFever2

Checks, against the shipped Steam Linux build 35924:
  * every byte anchor in native/linux/src/slice/company_tint_checks_linux.h,
    and that each one covers whole instructions;
  * that all sixteen carrier-class sites really are `call rel32` reaching
    CComponent::addStyleClass 0x30550d0, and that no branch inside the two
    icon functions lands in the middle of one of them;
  * that the two context sites are the calls the tint expects
    (0x109a8f0 in the StationItem constructor, 0x9e5590 in DoStep);
  * that the Player, PlayerOwned and StationGroup typeinfo pointers the engine
    walk resolves type indices with really name those components;
  * that the source still spells the kill switch, the install gates and the
    company-rename gate that slice-lines uses.
"""
from pathlib import Path
import re
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[2]
source = ((root / 'native/linux/src/slice/company_tint_checks_linux.h').read_text() +
          (root / 'native/linux/src/slice/ecs_checks_linux.h').read_text())
tint = (root / 'native/linux/src/slice/company_tint_linux.cpp').read_text()

ADD_STYLE_CLASS = 0x30550d0
GET_COMPONENT = 0x109a8f0
GET_DATA_INDEX = 0x9e5590

with open(sys.argv[1], 'rb') as stream:
    elf = ELFFile(stream)
    assert any(n['n_desc'] == '3a0e156390b0e6f1e372051c24802c8493ae454a'
               for s in elf.iter_segments() if s['p_type'] == 'PT_NOTE'
               for n in s.iter_notes() if n['n_type'] == 'NT_GNU_BUILD_ID')
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

    def read(addr, size):
        s = next(s for s in loads if s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz'])
        stream.seek(s['p_offset'] + addr - s['p_vaddr'])
        return stream.read(size)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    # 1. the byte anchors, each a whole number of instructions
    checks = re.findall(r'\{(0x[0-9a-f]+), "([^"\n]+)", (\d+)\}', source)
    assert len(checks) == 13, len(checks)
    for address, data, size in checks:
        addr, size = int(address, 16), int(size)
        expected = bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-f]{2})', data))
        assert len(expected) == size, address
        assert read(addr, size) == expected, address
        assert sum(i.size for i in md.disasm(expected, addr)) == size, address

    # 2. the sixteen carrier-class calls
    def sites(name):
        block = re.search(r'kTint%sClassCalls\[\] = \{(.*?)\};' % name, source, re.S).group(1)
        return [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', block)]

    station, depot = sites('Station'), sites('Depot')
    assert len(station) == 11 and len(depot) == 5
    for site in station + depot:
        ins = next(md.disasm(read(site, 5), site))
        assert ins.mnemonic == 'call' and ins.size == 5, hex(site)
        assert ins.operands[0].imm == ADD_STYLE_CLASS, hex(site)

    # 3. the two context calls
    for site, target in ((0x10902e4, GET_COMPONENT), (0x1095525, GET_DATA_INDEX)):
        ins = next(md.disasm(read(site, 5), site))
        assert ins.mnemonic == 'call' and ins.size == 5 and ins.operands[0].imm == target, hex(site)

    # 4. no branch inside either icon function may land inside a redirected call
    redirected = set(station + depot) | {0x10902e4, 0x1095525}
    for start, stop in ((0x1090250, 0x1090ba0), (0x1093b30, 0x1096ef0)):
        for ins in md.disasm(read(start, stop - start), start):
            if not ins.operands or ins.operands[0].type != X86_OP_IMM:
                continue
            if not (ins.mnemonic.startswith('j') or ins.mnemonic == 'call'):
                continue
            target = ins.operands[0].imm
            for site in redirected:
                assert not site < target < site + 5, (hex(ins.address), hex(target))

    # 5. the typeinfo objects the tint hands to the type-index lookup
    relocs = {r['r_offset']: r['r_addend'] for r in elf.get_section_by_name('.rela.dyn').iter_relocations()}
    for rva, want in ((0x5a01c18, b'N3ecs9component11PlayerOwnedE\0'),
                      (0x5a01b98, b'N3ecs9component12StationGroupE\0'),
                      (0x5a025f0, b'N3ecs9component6PlayerE\0')):
        name = relocs[rva + 8]
        assert read(name, len(want)) == want, hex(rva)

# 6. the source still says what this file verifies
assert 'FlagSays("stationicon", "0")' in tint
assert 'RVA_ADD_STYLE_CLASS = 0x30550d0' in tint
ecs = (root / 'native/linux/src/slice/ecs_linux.h').read_text()
assert 'SLICE_STRIDE_PLAYEROWNED  = 4' in ecs and 'SLICE_STRIDE_STATIONGROUP = 24' in ecs
assert 'SLICE_TI_PLAYER       = 0x5a025f0' in ecs
assert 'Anchored(base, kTintContextChecks)' in tint
movement = (root / 'native/linux/src/slice/movement_linux.cpp').read_text()
assert 'SliceInstallCompanyTint(base, root, data)' in movement
# the company rename: only an entity with a Player component may be shipped
lines = (root / 'native/linux/src/slice/slice_lines.cpp').read_text()
assert 'SliceEcsIsCompany(c.rsi, entity)' in lines
assert 'VNAME %d %s' in lines and 'PercentEncode(name)' in lines

print('PASS: build-id, 13 complete anchor spans, 11+5 addStyleClass calls, '
      '2 context calls, no interior branch target, Player/PlayerOwned/StationGroup RTTI, '
      'company-rename gate')
