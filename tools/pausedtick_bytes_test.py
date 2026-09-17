"""Verify the PAUSED TICK patch's sites in the supported game binary.

The patch NOPs one call: the paused branch of GameSim::Step advancing GameTime+0x30
once per render batch. This checks what a running game cannot be asked right now:

  * the 22 bytes slice_hook.cpp guards are still `mov rcx,[r14+8] ; xor r8d,r8d ;
    mov edx,[rcx+0x210] ; mov rcx,[rcx+0x28] ; call`, with r8d (the "stepped" bool)
    cleared -- so this is the PAUSED call, not the running one -- and the call
    resolves to the advance function;
  * that function is the one whose body is `inc [rax+0x30]` unconditionally and
    `inc [rax+0x34]` only when the bool is set, so NOPing the paused call changes
    +0x30 and nothing else;
  * the running branch's call (bool = 1) is elsewhere in the function and untouched;
  * the 5 bytes replaced are exactly one instruction, and nothing branches into them;
  * the town developer, town creation, the street proposal, the account system and
    the train system still read the counter through the accessor 0x2877c0 -- the
    readers the finding named -- so the patch's reason still holds.

Constants are read out of the source, so a drifting RVA fails here instead of in a
running game.

    python tools/pausedtick_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


site = const("RVA_PAUSED_TICK_SITE")
call = const("RVA_PAUSED_TICK_CALL")
advance = const("RVA_GAMETIME_ADVANCE")
inc = const("RVA_GAMETIME_ADVANCE_INC")
expect = byte_array("PAUSED_TICK_EXPECT")
adv_expect = byte_array("GAMETIME_ADVANCE_EXPECT")

assert "pausedtick=0" in source, "the kill switch is gone from the source"
assert 'FlagsSayOff("pausedtick")' in source, "the install no longer consults the kill switch"
assert "InstallPausedTick();" in source, "InstallPausedTick is never called"
assert call == site + 17, "the call is the last instruction of the guarded window"
assert len(expect) == 22

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"

got = pe.get_data(site, len(expect))
assert got == expect, f"paused branch changed: {got.hex(' ')} != {expect.hex(' ')}"
assert expect[4:7] == b"\x45\x33\xc0", "r8d is no longer cleared: this is not the paused call"
assert expect[17] == 0xE8
rel = struct.unpack("<i", expect[18:22])[0]
assert call + 5 + rel == advance, f"the call resolves to {call + 5 + rel:x}, not the advance {advance:x}"

got = pe.get_data(inc, len(adv_expect))
assert got == adv_expect, f"the advance's body changed: {got.hex(' ')}"
assert advance < inc < advance + 0x100, "the increments are not inside the advance function"

# the whole of GameSim::Step: the running branch's call (r8b = 1) is still there,
# the paused one is the only other, and nothing branches into the 5 NOPed bytes
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data()
base = text.VirtualAddress
FUNC, FUNC_END = 0x15AA00, 0x15B200
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
calls, prev = [], []
for ins in md.disasm(code[FUNC - base:FUNC_END - base], FUNC):
    if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM and ins.operands[0].imm == advance:
        calls.append((ins.address, prev[-4:]))
    if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X86_OP_IMM:
        assert not (call < ins.operands[0].imm < call + 5), f"a jump lands inside the NOPed call at {ins.address:x}"
    prev.append((ins.address, ins.mnemonic, ins.op_str))
    if ins.mnemonic == "ret" and ins.address > 0x15AC00:
        break
addrs = [a for a, _ in calls]
assert call in addrs, f"the paused call is not among the advance calls in GameSim::Step: {[hex(a) for a in addrs]}"
assert len(addrs) == 2, f"expected the paused and the running call, found {[hex(a) for a in addrs]}"
running = next(p for a, p in calls if a != call)
assert any(m == "mov" and o == "r8b, 1" for _, m, o in running), f"the other call does not set r8b=1: {running}"

# every rel32 call/jmp in .text that lands inside the replaced bytes
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        tgt = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (call < tgt < call + 5), f"something at {base + i:x} branches into the NOPed call"

# the readers of the skewed counter, through the accessor 0x2877c0
acc = 0x2877C0
readers = set()
for i in range(len(code) - 5):
    if code[i] == 0xE8 and base + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == acc:
        readers.add(base + i)
for want, what in ((0x943C7D, "TownDeveloper::Develop"), (0x9372D6, "town creation"), (0x987F89, "street_developer_util"),
                   (0xA1A60F, "MakeStreetProposal"), (0xA26AF1, "AccountSystem::Update"), (0xABE035, "TrainMoveSystem::Update2")):
    assert want in readers, f"{what} no longer reads the counter at {want:x}"

print(f"pausedtick bytes: ok -- the paused call at {call:x} -> {advance:x}, the running call at "
      f"{[hex(a) for a in addrs if a != call][0]}, {len(readers)} readers of the counter in .text")
