"""A player construction is one with an owner, whoever the owner is (2026-09-16).

The hash's town lane counts every construction that is not a player's. That
classification used to accept the human plus the company entities the save's
state fills in lazily; a joiner's first samples after the load gate ran before
that state was applied, so the other companies' stations counted as town
buildings for two stamps and a false "DESYNC town +12" was declared (t=6668
and 6672, c1 t:696 against the host's c13 t:684). This runs the real
CM.isPlayerConstruction from cons.lua against a mocked engine, and checks that
worldHash applies the companies state before sampling.

    python tools/player_construction_owner_test.py
"""
import os
import re

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
CONS = open(os.path.join(MP, "cons.lua"), encoding="utf-8", errors="replace").read()
HASH = open(os.path.join(MP, "hash.lua"), encoding="utf-8", errors="replace").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


m = re.search(r"^function CM\.isPlayerConstruction\(id, fileName\).*?^end$", CONS, re.S | re.M)
assert m, "isPlayerConstruction not found"
L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().FN = m.group(0)
T = L.execute(r'''
local CM = {}
local owners = {}
api = { type = { ComponentType = { PLAYER_OWNED = 1 } },
        engine = { util = { getPlayer = function() return 7 end },
                   getComponent = function(id, kind) local o = owners[id]; if o == nil then return nil end; return { player = o } end } }
assert(load("local CM = ...\n" .. FN, "@cons-fn"))(CM)
local T = {}
function T.set(id, owner) owners[id] = owner end
function T.mode(mode, pids) CM.cmMode = mode; CM.cmCompanyPid = pids end
function T.owned(id, file) return CM.isPlayerConstruction(id, file) end
function T.broken() api.engine.getComponent = function() error("no engine") end end
return T
''')

T.set(1, 7)          # mine
T.set(2, 9001)       # another company's AI entity, not yet in cmCompanyPid
T.set(3, None)       # a town building: no PLAYER_OWNED at all
T.set(4, -1)         # an owner slot with nobody in it
T.mode("coop", L.table())
check("mine: a player construction", T.owned(1, "station/rail/x.con") is True)
check("another company's, before the companies state is applied: still a player construction", T.owned(2, "station/rail/x.con") is True)
check("a town building (no owner) is not", T.owned(3, "building/era_b/res_1.con") is False)
check("an empty owner slot is not", T.owned(4, "station/rail/x.con") is False)
T.mode("companies", L.table({7, 9001}))
check("the same answers once the companies state is there", T.owned(2, "station/rail/x.con") is True and T.owned(3, "x") is False)
T.broken()
check("no engine: falls back to what a player can place", T.owned(5, "depot/rail/x.con") is True and T.owned(5, "building/x.con") is False)

check("worldHash applies the companies state before sampling",
      re.search(r"local function worldHash\(now\)\s*\n(?:\s*--[^\n]*\n)*\s*if CM\.cmEnsure then pcall\(CM\.cmEnsure\) end", HASH) is not None)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
