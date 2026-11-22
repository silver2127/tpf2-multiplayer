"""Offline check that the faster world-hash byte loop gives exactly the old hashes, on Lua 5.2.

hash.lua's hashStr walked every byte with one string.byte call each, and the world hash joined
each lane with table.concat first (the edge lane alone ~1.5 MB on a 27k-edge map). Since
2026-09-23 it fetches eight bytes per call (hashFeed) and hashList feeds the list items and
their separators straight in. The desync detector must not change by a single bit: two peers
on different builds would call every stamp a desync, and a hash that differs from the old one
in some corner would be a different detector.

This loads the REAL hash.lua and compares CM.hashStr / CM.hashList with the original
implementation, kept verbatim below as the oracle:
  - every length 0..80 (all eight-byte tails), random bytes 0..255, NUL and 0xFF runs
  - hashList against hashStr(table.concat(list, sep)): empty lists, empty items, numbers
    (integral and fractional), empty and multi-byte separators
  - a 30,000-item edge-like lane (~1.5 MB) and its timing, old vs new
  - hashList refuses a value table.concat refuses (no silent tostring of a table)

    python tools/hash_stream_test.py
"""
import os
import random
import sys
import time

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HASH = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "hash.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


ORACLE = r"""
local M1, A1 = 2147483647, 48271
local M2, A2 = 2147483629, 40692
return function(s)
	local h1, h2 = 2166136261 % M1, 2166136261 % M2
	for i = 1, #s do
		local b = s:byte(i)
		h1 = (h1 * A1 + b) % M1
		h2 = (h2 * A2 + b) % M2
	end
	return string.format("%010d-%010d", h1, h2)
end
"""

lua = lupa.LuaRuntime(unpack_returned_tuples=True, encoding=None)
with open(HASH, "rb") as f:
    src = f.read()
factory = lua.execute(b"return function(src) return load(src, '=hash.lua') end")(src)
if factory is None:
    print("FAIL hash.lua does not parse")
    sys.exit(1)
CM = lua.eval("{}")
K = lua.eval("{}")
factory()(CM, K, lua.eval("function() end"))
oracle = lua.execute(ORACLE)
new_str = CM[b"hashStr"]
new_list = CM[b"hashList"]
check("hash.lua exports hashStr and hashList", new_str is not None and new_list is not None)
if fails:
    sys.exit(1)

# Lua-side helpers: build strings/lists from bytes without Python<->Lua string surprises
mk_list = lua.eval("function(...) return {...} end")
concat = lua.eval("function(t, sep) return table.concat(t, sep) end")
pcall_list = lua.eval("function(f, t, sep) return pcall(f, t, sep) end")
pcall_concat = lua.eval("function(t, sep) return pcall(table.concat, t, sep) end")

rnd = random.Random(20260923)

# 1. hashStr: every tail length, random bytes
bad = 0
for n in range(0, 81):
    for _ in range(40):
        s = bytes(rnd.randrange(256) for _ in range(n))
        if oracle(s) != new_str(s):
            bad += 1
check("hashStr equals the old loop for every length 0..80 (3,240 random strings)", bad == 0, f"{bad} differ")

special = [b"", b"\0", b"\0" * 9, b"\xff" * 17, b"\xff\0" * 33, b"|", b"a|b", bytes(range(256)) * 3]
bad = sum(1 for s in special if oracle(s) != new_str(s))
check("hashStr equals the old loop on NUL / 0xFF / all-byte strings", bad == 0, f"{bad} differ")


def rand_item():
    r = rnd.random()
    if r < 0.1:
        return b""
    if r < 0.2:
        return rnd.randrange(-10 ** 6, 10 ** 6)
    if r < 0.3:
        return rnd.uniform(-1e5, 1e5)
    return bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 40)))


# 2. hashList == hashStr(table.concat(list, sep))
bad = 0
cases = 0
for sep in (b"|", b"", b",;", b"\0", b"\xff|\xff"):
    for count in list(range(0, 12)) + [31, 64]:
        for _ in range(25):
            items = [rand_item() for _ in range(count)]
            t = mk_list(*items)
            want = oracle(concat(t, sep))
            got = new_list(t, sep)
            cases += 1
            if want != got:
                bad += 1
check(f"hashList equals hashStr(table.concat(...)) ({cases} lists, 5 separators, numbers and empty items)", bad == 0, f"{bad} differ")

# 3. a big edge-like lane, and what it costs
edges = [f"{rnd.uniform(-8000, 8000):.1f},{rnd.uniform(-8000, 8000):.1f}>{rnd.uniform(-8000, 8000):.1f},{rnd.uniform(-8000, 8000):.1f}#{rnd.randrange(40)}".encode()
         for _ in range(30000)]
t = mk_list(*edges)
joined = concat(t, b"|")
t0 = time.perf_counter()
want = oracle(joined)
t_old = time.perf_counter() - t0
t0 = time.perf_counter()
got_str = new_str(joined)
t_str = time.perf_counter() - t0
t0 = time.perf_counter()
got_list = new_list(t, b"|")
t_list = time.perf_counter() - t0
check(f"a 30,000-edge lane ({len(joined):,} bytes) hashes the same, joined and streamed", want == got_str == got_list)
print(f"     timing: old loop {t_old * 1000:.1f} ms, new hashStr {t_str * 1000:.1f} ms, hashList (no join) {t_list * 1000:.1f} ms")

# 4. refuses what table.concat refuses
t_bad = lua.eval("{'a', {}, 'b'}")
ok_c = pcall_concat(t_bad, b"|")[0]
ok_l = pcall_list(new_list, t_bad, b"|")[0]
check("hashList refuses a table item, like table.concat", ok_c is False and ok_l is False)

if fails:
    print(f"\n{len(fails)} check(s) FAILED")
    sys.exit(1)
print("\nall checks passed")
