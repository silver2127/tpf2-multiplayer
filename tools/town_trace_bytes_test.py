"""Verify the TOWN DEVELOPMENT TRACE site (native/src/slice/town_trace.inl) in the
supported game binary, and that the Windows and native traces write one format.

- TT_EXPECT is the exe's argument setup + `call TownDeveloper::Develop`, the call
  is the last instruction, it reaches 0x91d910 (TownDeveloper::Develop), and
  r13 is the TownSystem update's context there
  (loaded from [rbp-0x28], which the prologue filled from rcx, and unchanged
  up to the call);
- the update reads the Town node vector through [r13+8] and the clock through
  0x287830([r13+0x20]), the getter TT_GETTIME_EXPECT pins (GameTime+0x34);
- the near stub stores r13 at stub+21 and jumps through stub+13;
- both platforms print TT/TF through native/src/town_trace.h only.

    python tools/town_trace_bytes_test.py
"""
from pathlib import Path
import re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from slice_source import slice_source

repo = Path(__file__).resolve().parents[1]
source = slice_source(repo)


def array(name):
    m = re.search(r'static const uint8_t ' + name + r'\[\] = \{(.*?)\};', source, re.S)
    assert m, name
    return bytes(int(b, 16) for b in re.findall(r'0x[0-9A-Fa-f]{2}', re.sub(r'//[^\n]*', '', m.group(1))))


def const(name):
    m = re.search(r'static const uintptr_t ' + name + r' = (0x[0-9a-f]+);', source)
    assert m, name
    return int(m.group(1), 16)


site, call, develop, gettime = const('RVA_TT_SITE'), const('RVA_TT_CALL'), const('RVA_TT_DEVELOP'), const('RVA_TT_GETTIME')
expect, getter = array('TT_EXPECT'), array('TT_GETTIME_EXPECT')
assert (site, call, develop, gettime) == (0xab2518, 0xab253e, 0x91d910, 0x287830)
assert site + len(expect) == call + 5

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
BASE = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

assert pe.get_data(site, len(expect)) == expect, pe.get_data(site, len(expect)).hex(' ')
assert pe.get_data(gettime, len(getter)) == getter
insns = list(md.disasm(expect, BASE + site))
assert sum(i.size for i in insns) == len(expect)
last = insns[-1]
assert last.address == BASE + call and last.mnemonic == 'call' and int(last.op_str, 16) == BASE + develop, last
assert insns[-2].mnemonic == 'mov' and insns[-2].op_str == 'rcx, qword ptr [r13 + 0x40]'
print(f'PASS: TownSystem Develop call {call:#x} -> {develop:#x}, argument setup {len(expect)} bytes as expected')

upd = 0xab1d20
code = pe.get_data(upd, 0x960)
fn = list(md.disasm(code, BASE + upd))
# r13: set from rcx at entry, spilled to [rbp-0x28], reloaded before the second loop, untouched to the call.
r13_writes = [i for i in fn if i.op_str.startswith('r13,') and i.mnemonic in ('mov', 'lea', 'pop', 'add', 'sub', 'xor')]
assert [(i.address - BASE, i.op_str) for i in r13_writes] == [(0xab1d73, 'r13, rcx'), (0xab23f0, 'r13, qword ptr [rbp - 0x28]')], \
    [(hex(i.address - BASE), i.op_str) for i in r13_writes]
spill = [i for i in fn if i.op_str == 'qword ptr [rbp - 0x28], rcx']
assert len(spill) == 1 and spill[0].address - BASE == 0xab1d76
assert not [i for i in fn if i.op_str.startswith('qword ptr [rbp - 0x28],') and i.address - BASE != 0xab1d76]
assert not [i for i in fn if 0xab23f0 < i.address - BASE < call and i.op_str.split(',')[0].strip() in ('r13', 'r13d')]
# The node vector through [r13+8], the clock through 0x287830([r13+0x20]).
assert any(i.op_str == 'rax, qword ptr [r13 + 8]' for i in fn)
# The seed (0xab1dab) and the stagger (0xab1ec0) both read the clock this way.
clock = {b.address - BASE for a, b in zip(fn, fn[1:])
         if a.op_str == 'rcx, qword ptr [r13 + 0x20]' and b.mnemonic == 'call' and int(b.op_str, 16) == BASE + gettime}
assert clock == {0xab1dab, 0xab1ec0}, sorted(map(hex, clock))
print('PASS: r13 is the update context at the call; [r13+8] = Town node vector, 0x287830([r13+0x20]) = the tick clock')

# The near stub: mov [rip+0x0e], r13 ; jmp [rip+0] -> slot at +21, target at +13.
head = re.search(r'static const uint8_t head\[\] = \{([^}]*)\}', source).group(1)
stub = bytes(int(b, 16) for b in re.findall(r'0x[0-9A-Fa-f]{2}', head)) + b'\x11' * 8 + b'\0' * 8
dec = list(md.disasm(stub[:13], 0x1000))
assert dec[0].mnemonic == 'mov' and dec[0].op_str == 'qword ptr [rip + 0xe], r13' and dec[0].address + dec[0].size + 0xe == 0x1000 + 21
assert dec[1].mnemonic == 'jmp' and dec[1].op_str == 'qword ptr [rip]' and dec[1].address + dec[1].size == 0x1000 + 13
assert 'memcpy(stub + 13, &wrap, 8)' in source and 'g_ttCtxSlot = (volatile uintptr_t*)(stub + 21)' in source
print('PASS: near stub stores r13 at +21 and jumps through +13')

# One format on both platforms (the native source exists only on port/dev).
linux_src = repo / 'native/linux/src/town_trace_linux.cpp'
texts = [source] + ([linux_src.read_text(encoding='utf-8')] if linux_src.exists() else [])
for text in texts:
    assert 'TownTraceFormatTT(' in text and 'TownTraceFormatTF(' in text
    assert '"TT ' not in text and '"TF ' not in text
print('PASS: %s format through native/src/town_trace.h' % ('both traces' if len(texts) == 2 else 'the Windows trace'))
