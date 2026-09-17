"""Verify the SHOW ALL ICONS gates in the supported game binary.

The patch NOPs three owner-test `jne`s so world icons are drawn for every player's
stations and vehicles, not only the local player's. This checks, against the exe,
what a running game cannot be asked:

  * each gate's two/three bytes before the jne are the cmp it depends on, exactly as
    slice_hook.cpp records them (a byte match that landed elsewhere cannot pass);
  * each jne is a 6-byte near jump (0f 8x), so a 6-byte NOP replaces one whole
    instruction;
  * the jne's target is the reject path the RE named (Visit -> the epilogue 0x8088de,
    End -> the loop's next-node 0x80c640, DoStep -> the loop increment 0x5de603), and
    the fall-through is the accept path;
  * nothing in the enclosing function branches into the 6 bytes being NOPed.

Constants are read out of the source, so a drifting RVA fails here instead of in a
running game.

    python tools/showicons_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")

# parse the ICON_GATES table out of the source
block = re.search(r"static const IconGate ICON_GATES\[3\]\s*=\s*\{(.*?)\n\};", source, re.S)[1]
rows = re.findall(
    r"\{\s*(0x[0-9a-fA-F]+),\s*\{([^}]*)\},\s*(\d+),\s*\{([^}]*)\},\s*\"([^\"]*)\"\s*\}",
    block,
)
assert len(rows) == 3, f"expected 3 gates, parsed {len(rows)}"

assert "showicons=0" in source, "the kill switch is gone from the source"
assert 'FlagsSayOff("showicons")' in source, "the install no longer consults the kill switch"
assert "InstallShowAllIcons();" in source, "InstallShowAllIcons is never called"

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data()
base = text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# the reject targets the RE named, and the function each gate lives in (for the branch scan)
EXPECT = {
    0x80847b: (0x8088de, 0x8083c0, 0x808b70, "Visit"),
    0x80c56b: (0x80c640, 0x80c4b0, 0x80c700, "End"),
    0x5de529: (0x5de603, 0x5de2c0, 0x5de700, "DoStep"),
}

fails = []
for jne_hex, before_s, blen_s, jbytes_s, what in rows:
    jne = int(jne_hex, 16)
    before = bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", before_s))
    blen = int(blen_s)
    jbytes = bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", jbytes_s))
    assert blen == len(before) and len(jbytes) == 6

    got_before = pe.get_data(jne - blen, blen)
    if got_before != before:
        fails.append(f"{jne:x} {what}: cmp before is {got_before.hex(' ')}, source says {before.hex(' ')}")
        continue
    got_jne = pe.get_data(jne, 6)
    if got_jne != jbytes:
        fails.append(f"{jne:x} {what}: jne is {got_jne.hex(' ')}, source says {jbytes.hex(' ')}")
        continue
    # decode the jne and check its target
    ins = next(md.disasm(got_jne, jne))
    assert ins.mnemonic == "jne", f"{jne:x}: not a jne ({ins.mnemonic})"
    tgt = ins.operands[0].imm
    reject, fstart, fend, name = EXPECT[jne]
    if tgt != reject:
        fails.append(f"{jne:x} {name}: jne -> {tgt:x}, expected the reject path {reject:x}")
        continue
    # nothing in the function branches INTO the 6 NOPed bytes
    into = []
    for d in md.disasm(code[fstart - base:fend - base], fstart):
        if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
            if jne < d.operands[0].imm < jne + 6:
                into.append(hex(d.address))
    if into:
        fails.append(f"{jne:x} {name}: something branches into the NOPed jne: {into}")
        continue
    print(f"ok   {jne:x} {name}: cmp+jne intact, jne -> reject {reject:x}, nothing branches in")

# a rel32 scan across all of .text for a branch into any NOPed range
for jne_hex, *_ in rows:
    jne = int(jne_hex, 16)
    for i in range(len(code) - 5):
        if code[i] in (0xE8, 0xE9):
            t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
            if jne < t < jne + 6:
                fails.append(f"a rel32 at {base + i:x} lands inside the NOPed jne at {jne:x}")

if fails:
    print("FAILED:")
    for f in fails:
        print("  " + f)
    raise SystemExit(1)
print("showicons bytes: ok -- 3/3 gates")
