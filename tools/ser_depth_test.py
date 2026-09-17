"""Offline checks of the construction-params codec (cons.lua CM.ser / CM.deserParams), on Lua 5.2.

Until 2026-09-16 CM.ser replaced every table below depth 8 with `{}` -- silently,
so a modded module's nested params were applied EMPTY on the peers while the
edit looked identical on both sides. There is no depth cap any more: the only
guard is a cycle check that logs loudly when it trips. This loads the real
cons.lua into a lupa.lua52 runtime with a stub API and checks:
  * a 60-deep params tree round-trips through ser -> deserParams unchanged;
  * the same table under two keys (a DAG, not a cycle) serialises in full twice;
  * a cycle is logged with its key path and counted, never a quiet {};
  * a literal the reader cannot parse is refused loudly (nil + log), never {}.

    python tools/ser_depth_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "cons.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().CONS_SRC = open(CONS, encoding="utf-8").read()
H = L.execute(r'''
local logs = {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = sink()
game = sink()
local K = setmetatable({ INSTANCE = "a" }, { __index = function() return nil end })
local CM = setmetatable({ expectedCons = {}, cmExpectedCompany = {}, cmExpectedBal0 = {}, expectedDemolish = {} },
  { __index = function() return nil end })
function CM.gameTime() return 100 end
function CM.scheduleLocal() end
function CM.cmLog() end
function CM.cmBalance() return nil end
local log = function(s) logs[#logs + 1] = s end
assert(load(CONS_SRC, "@cons.lua"))()(CM, K, log)
local H = { CM = CM }
function H.logs() return table.concat(logs, "\n") end
function H.clear() logs = {} end
-- a params tree `depth` levels deep, a modular-station shape at the top
function H.deep(depth)
  local leaf = { name = "leaf", variant = 3, flag = true, list = { 1, 2, 3 } }
  local t = leaf
  for i = depth - 1, 1, -1 do t = { ["lvl" .. i] = t, n = i } end
  return { seed = 42, modules = { [1] = { name = "station/rail/x.module", variant = 0, params = t } } }
end
-- walk `modules[1].params` down `depth`-1 levels and return the leaf's name
function H.leafOf(p, depth)
  local t = p.modules[1].params
  for i = 1, depth - 1 do t = t["lvl" .. i]; if not t then return nil end end
  return t.name, t.list and #t.list
end
function H.roundtrip(depth)
  local p = H.deep(depth)
  local s = CM.ser(p)
  local back = CM.deserParams(s)
  local name, n = H.leafOf(back, depth)
  return #s, name, n, CM.ser(back) == s
end
function H.dag()
  local shared = { x = 1, y = { z = 2 } }
  local s = CM.ser({ p = shared, q = shared })
  return s, CM.serCycles
end
function H.cycle()
  local t = { a = 1, inner = {} }
  t.inner.back = t
  local s = CM.ser(t)
  return s, CM.serCycles
end
function H.deser(s)
  local v, err = CM.deserParams(s)
  return v ~= nil, tostring(err)
end
return H
''')

# 1. depth: 60 levels, far past the old cap of 8
n, name, listn, stable = H.roundtrip(60)
check("60-deep params round-trip keeps the leaf", name == "leaf" and listn == 3, f"leaf={name} list={listn} bytes={n}")
check("60-deep: ser(deser(ser(p))) == ser(p)", stable)
n8, name8, _, _ = H.roundtrip(9)
check("9-deep (first depth the old cap emptied) keeps the leaf", name8 == "leaf", f"leaf={name8}")

# 2. a DAG serialises in full under both keys and is not a cycle
s, cycles = H.dag()
check("DAG: shared table written in full under both keys", s.count('["z"]=2') == 2, s)
check("DAG: cycle guard did not trip", cycles == 0, str(cycles))

# 3. a cycle: loud, counted, never a quiet {}
H.clear()
s, cycles = H.cycle()
logs = H.logs()
check("cycle: written as {} at the back-reference only", s == '{["a"]=1,["inner"]={["back"]={}}}', s)
check("cycle: counted", cycles == 1, str(cycles))
check("cycle: logged loudly with its key path", 'ser: CYCLE at params["inner"]["back"]' in logs, logs[-200:])

# 4. the reader refuses loudly
H.clear()
ok, err = H.deser("{[1]=")
check("unparseable literal: refused (nil)", not ok)
check("unparseable literal: logged as REFUSED", "does not parse" in H.logs() and "REFUSED" in H.logs(), H.logs()[-200:])
H.clear()
ok, err = H.deser("7")
check("non-table literal: refused (nil)", not ok)
check("non-table literal: logged as REFUSED", "did not evaluate to a table" in H.logs(), H.logs()[-200:])
H.clear()
ok, err = H.deser('{["modules"]={[1]={["name"]="a"}}}')
check("a good literal still reads", ok and H.logs() == "", H.logs()[-200:])

print("FAILED: " + ", ".join(fails) if fails else "all passed")
sys.exit(1 if fails else 0)
