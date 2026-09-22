"""A cancelled CommandList::Add must hand its caller a usable handle.

Add writes a std::unique_ptr to a 16-byte {result*, control*} weak reference into its
second argument; the caller owns it. When the slice cancels the command it has to put
something valid there, and "null" is not enough: the NEW LINE button in a vehicle's line
picker copies the handle right after Add and the copy dereferences it. That crashed a
player's game on every press (2026-09-22, exe+0x235780f, read of address 0).

Checked against the supported exe, so a drifting RVA fails here and not in someone's game:

  * the vehicle line picker's New Line (linelist.cpp) calls Add and, at the return address
    the slice logs, COPIES the handle (0x23577f0) before destroying it -- and that copy
    reads *handle with no null check, as does the release the UI runs later (0x9d3210
    reads handle[1]);
  * the Line manager window's New Line (linemanager.cpp) only destroys it (0x2357910),
    which is why that button lived;
  * the game's operator new, whose bytes the slice verifies before calling it, is a real
    function at the recorded RVA;
  * the slice's cancel path allocates an empty handle from it instead of writing null.

    python tools/add_handle_test.py
"""
from pathlib import Path
import re
import struct
import sys

import pefile
from slice_source import slice_source

repo = Path(__file__).resolve().parents[1]
source = slice_source(repo)
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


RVA_ADD = 0x9D2A00            # CommandList::Add
RVA_COPY = 0x23577F0          # copy the handle (allocates 16 bytes, reads *handle)
RVA_DESTROY = 0x2357910       # destroy the handle
RVA_RELEASE = 0x9D3210        # what the UI's callback runs per handle (reads handle[1])
LINE_LIST = 0x610380          # a vehicle's line picker, New Line: Add, COPY, hand to the UI, destroy
LINE_MANAGER = 0x618FF0       # the Line manager window, New Line: Add, destroy

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
if not game.exists():
    print("SKIP: the game is not installed here")
    sys.exit(0)
pe = pefile.PE(str(game), fast_load=True)
check("the supported build", pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "these RVAs were measured on 35924")

try:
    import capstone
except ImportError:
    print("SKIP: pip install capstone for the disassembly checks")
    sys.exit(0)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
BASE = 0x140000000


def calls(rva, length=0x160):
    """(return address, target) of every direct call in a function, in order."""
    out = []
    for ins in md.disasm(pe.get_data(rva, length), BASE + rva):
        if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            out.append((ins.address - BASE + ins.size, int(ins.op_str, 16) - BASE))
        if ins.mnemonic == "ret":
            break
    return out


list_calls = calls(LINE_LIST)
mgr_calls = calls(LINE_MANAGER)
after_add = [ret for ret, tgt in list_calls if tgt == RVA_ADD]
check("the vehicle line picker's New Line calls Add", len(after_add) == 1, "returns to %#x" % after_add[0] if after_add else "not found")
seq = [tgt for _, tgt in list_calls]
check("... then COPIES the handle, hands it to the UI, and destroys it",
      RVA_ADD in seq and RVA_COPY in seq and RVA_DESTROY in seq
      and seq.index(RVA_ADD) < seq.index(RVA_COPY) < seq.index(RVA_DESTROY), str([hex(t) for t in seq]))
check("the Line manager window's New Line only destroys it",
      RVA_ADD in [t for _, t in mgr_calls] and RVA_COPY not in [t for _, t in mgr_calls]
      and RVA_DESTROY in [t for _, t in mgr_calls], str([hex(t) for _, t in mgr_calls]))

# the copy dereferences the handle with no null check: mov rdi,[rdx] ... mov rdx,[rdi]
copy = list(md.disasm(pe.get_data(RVA_COPY, 0x40), BASE + RVA_COPY))
text = [(i.address - BASE, i.mnemonic + " " + i.op_str) for i in copy]
check("the copy reads *handle and then handle->ptr, unguarded",
      any(t[1] == "mov rdi, qword ptr [rdx]" for t in text) and any(t[1] == "mov rdx, qword ptr [rdi]" for t in text)
      and not any(t[1].startswith("test rdi") for t in text[:6]), str(text[:8]))
# ... and the UI's release reads handle[1], so a null handle would fault there too
rel = list(md.disasm(pe.get_data(RVA_RELEASE, 0x30), BASE + RVA_RELEASE))
check("the release reads handle[1] as well", any(i.mnemonic == "mov" and i.op_str.endswith("qword ptr [rcx + 8]") for i in rel))

# the allocator the slice calls
rva_new = const("RVA_OPERATOR_NEW")
expect = byte_array("OPERATOR_NEW_BYTES")
got = pe.get_data(rva_new, len(expect))
check("the game's operator new is where the slice expects it", got == expect, "%s vs %s" % (got.hex(" "), expect.hex(" ")))

# the cancel path itself
zero = re.search(r"static void ZeroAddResult\(uint64_t rdx\)\s*\{(.*?)\n\}", source, re.S)[1]
check("the cancelled Add writes an allocated empty handle, not null",
      "GameNew16()" in zero and "(uint64_t)empty" in zero and "*(volatile uint64_t*)rdx = 0;" not in zero)
check("the allocator's bytes are verified before it is called",
      "memcmp((const void*)fn, OPERATOR_NEW_BYTES" in source and "cancelled Adds hand back a null handle" in source)
check("the allocation is zeroed (an empty weak reference)", "memset(p, 0, 16)" in source)

print()
if fails:
    print(f"{len(fails)} FAILED")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("PASS: a cancelled Add hands back an empty 16-byte handle from the game's allocator; the vehicle line picker's copy and the UI's release both survive it")
