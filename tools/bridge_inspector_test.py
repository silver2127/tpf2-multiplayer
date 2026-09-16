"""Verify the measured bridge inspector call site in the supported game binary.

Run together with bridge_companion_test.py, which exercises real Lua conversion
and verifies an upgrade retains its changed bridge model and edge removal.
"""
from pathlib import Path
import re
import struct
import pefile

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")
caller = int(re.search(r"CALLER_BRIDGE_UPGRADE\s*=\s*(0x[0-9a-f]+)", source)[1], 16)
game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675abcc6
def call_target(at):
    data = pe.get_data(at, 5)
    assert data[0] == 0xe8, f"Expected call at {at:x}"
    return at + 5 + struct.unpack("<i", data[1:])[0]
assert call_target(caller - 5) == 0x9dc750, "Inspector no longer calls BuildProposal"
assert call_target(0x89869e) == 0x9d2a00, "Inspector no longer submits through CommandList::Add"
assert "caller == CALLER_UPGRADE || caller == CALLER_BRIDGE_UPGRADE" in source
print("PASS: measured bridge inspector uses the supported BuildProposal/Add path and is routed as an upgrade")
