"""Offline checks for company names (companies.lua CMNAME), on Lua 5.2.

A company's name is replicated company state, not the player entity's NAME
component (a switch swaps the entities, so a NAME would follow the wrong
company). This loads the real companies.lua and checks:
  - an unnamed company reads as "Company N"
  - the origin playing a company may name it; the name arrives percent-escaped
    and is stored unescaped and trimmed
  - an origin playing another company may not name it
  - an empty name clears it; a dissolved company loses its name
  - the names ride in the save state and come back from it
  - the GUI's inject line and the dispatcher carry CMNAME (text anchors, as
    actions_off_test does for the dash file)

    python tools/company_name_test.py
"""
import os

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
COMPANIES = os.path.join(MP, "companies.lua")
SHARED = os.path.join(MP, "shared_infra.lua")
INJECT = os.path.join(MP, "inject.lua")
LOCKSTEP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().SRC = open(COMPANIES, encoding="utf-8").read()
L.globals().SHARED = open(SHARED, encoding="utf-8").read()
T = L.execute(r'''
local notes = {}
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = setmetatable({ INSTANCE = "a", BASE = "" }, { __index = function() return nil end })
local CM = setmetatable({ ticks = 0 }, { __index = function() return function() return nil end end })
local log = function(s) end
assert(load(SRC, "@companies.lua"))()(CM, K, log)
-- what execCompanyCmd needs around it
CM.cmOwnerCapability = function() return true end
CM.cmGoLive = function() end
CM.cmEnsure = function() end
CM.cmEnsurePlayers = function() end
CM.cmNote = function(s) notes[#notes + 1] = s end
CM.escName = function(s) return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end)) end
CM.unescName = function(s) return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
CM.cmMode, CM.cmMyCompany, CM.cmRoster = "companies", 1, { 1, 2, 3 }
CM.cmOriginCompany = { b = 2, c = 3 }
CM.cmCompanyPid = { [1] = 7, [2] = 8, [3] = 9 }
CM.cmPw = {}
local T = {}
function T.nameOf(cid) return CM.cmNameOf(cid) end
function T.name(origin, cid, name) CM.execCompanyCmd({ op = "CMNAME", cid = cid, origin = origin, name = CM.escName(name) }) return notes[#notes] end
function T.raw(cid) return CM.cmName[cid] end
function T.dissolve(cid) CM.peers = {}; CM.cmPlayersOf = function() return {} end; CM.cmMoveAssets = function() return 0 end; CM.cmWallet = function() return 0, 0 end; CM.cmSetWallet = function() end; CM.cmPwOk = function() return true end
  CM.execCompanyCmd({ op = "CMDEL", cid = cid, origin = "a" }) return notes[#notes] end
function T.roundTrip()
  local st = CM.cmSaveState()
  local names = {}
  for k, v in pairs(st.names or {}) do names[#names + 1] = k .. "=" .. v end
  table.sort(names)
  CM.cmName = {}
  CM.cmLoadState(st)
  CM.cmMyCompany = 1
  CM.cmApplySaved = CM.cmApplySaved   -- the real one
  -- api.engine.util.getPlayer() must equal the saved human pid for the state to apply
  api = { engine = { util = { getPlayer = function() return 7 end } } }
  CM.cmLocalSwitch = function() return true end
  CM.cmApplySaved()
  return table.concat(names, ","), CM.cmNameOf(1), CM.cmNameOf(2), CM.cmNameOf(3)
end
return T
''')

check("unnamed reads as Company N", T.nameOf(2) == "Company 2", T.nameOf(2))
n = T.name("b", 2, "  Acme & Sons  ")
check("the origin playing it names it (trimmed, unescaped)", T.raw(2) == "Acme & Sons", repr(T.raw(2)))
check("the note says who named what", "b named company 2" in n and "Acme & Sons" in n, n)
n = T.name("c", 2, "Hijack")
check("an origin playing another company may not", T.raw(2) == "Acme & Sons" and "cannot name" in n, n)
T.name("a", 1, "Host Rail")
check("the host names its own", T.nameOf(1) == "Host Rail", T.nameOf(1))
saved, n1, n2, n3 = T.roundTrip()
check("names ride in the save state", saved == "1=Host Rail,2=Acme & Sons", saved)
check("and come back from it", (n1, n2, n3) == ("Host Rail", "Acme & Sons", "Company 3"), f"{n1}|{n2}|{n3}")
n = T.name("b", 2, "")
check("an empty name clears it", T.nameOf(2) == "Company 2" and "unnamed" in n, n)
T.name("c", 3, "Gone Soon")
T.dissolve(3)
check("a dissolved company loses its name", T.raw(3) is None, repr(T.raw(3)))

inject = open(INJECT, encoding="utf-8", errors="replace").read()
lockstep = open(LOCKSTEP, encoding="utf-8", errors="replace").read()
check("inject.lua parses CMNAME cid name...", 'o == "CMNAME"' in inject and 'CM.scheduleLocal("CMNAME"' in inject)
check("CMNAME is never solo-dropped nor actions-off dropped", 'and o ~= "CMNAME" then' in inject and 'CMNAME = true' in inject)
check("the dispatcher routes CMNAME to execCompanyCmd", 'c.op == "CMNAME" then CM.execCompanyCmd(c)' in lockstep)
check("the dashboard reads the names and picks from a ComboBox", 'conames=' in lockstep and 'api.gui.comp.ComboBox.new()' in lockstep and 'CMNAME ' in lockstep)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
