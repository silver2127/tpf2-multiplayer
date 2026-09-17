"""Offline checks for station permissions (companies.lua CMOPEN), on Lua 5.2.

Which companies' vehicles may stop at a company's stations. Loads the real
companies.lua and checks:
  - the default is open to everyone; CMOPEN * 0 closes to nobody, CMOPEN * 1 reopens
  - allowing / denying one company edits an explicit set; naming every other company
    folds back to "everyone"
  - only someone playing the company may change it
  - the wire/dash/file codes ("*", "-", "1,3") round-trip, and the state survives
    cmSaveState / cmApplySaved
  - mp_company_perms.txt carries every company's player entity and its code, and is
    rewritten only when it changes
  - CM.cmLineStopsPermitted refuses a line whose stop belongs to a company not open to
    the line's company, and allows the owner's own, an open company's, and an unknown
    owner's stations
  - a dissolved company disappears from every set

    python tools/company_perms_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
COMPANIES = os.path.join(MP, "companies.lua")
SHARED = os.path.join(MP, "shared_infra.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


BASE = tempfile.mkdtemp().replace("\\", "/") + "/"
L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().SRC = open(COMPANIES, encoding="utf-8").read()
L.globals().SHARED = open(SHARED, encoding="utf-8").read()
L.globals().BASE = BASE
T = L.execute(r'''
local notes = {}
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = setmetatable({ INSTANCE = "a", BASE = BASE }, { __index = function() return nil end })
local CM = setmetatable({ ticks = 0 }, { __index = function() return function() return nil end end })
local log = function(s) end
assert(load(SRC, "@companies.lua"))()(CM, K, log)
CM.cmOwnerCapability = function() return true end
CM.cmGoLive = function() end
CM.cmEnsure = function() end
CM.cmEnsurePlayers = function() end
CM.cmNote = function(s) notes[#notes + 1] = s end
CM.cmMode, CM.cmMyCompany, CM.cmRoster = "companies", 1, { 1, 2, 3 }
CM.cmOriginCompany = { b = 2, c = 3 }
CM.cmCompanyPid = { [1] = 7, [2] = 8, [3] = 9 }
CM.cmPw = {}
CM.cmName = { [1] = "Ada Co", [2] = "Bob Co", [3] = "Cy Co" }
CM.cmNameOf = function(cid) return CM.cmName[cid] or ("company " .. cid) end
-- the engine side for the line check: PLAYER_OWNED per entity
local owners = { [500] = 7, [600] = 8, [700] = 9 }
api = { engine = { getComponent = function(id, t) if t == 1 and owners[id] then return { player = owners[id] } end return nil end },
        type = { ComponentType = { PLAYER_OWNED = 1, STATION_GROUP = 2 } } }
local T = {}
function T.open(origin, cid, who, on) CM.execCompanyCmd({ op = "CMOPEN", cid = cid, origin = origin, who = who, on = on }); return notes[#notes] end
function T.code(cid) return CM.cmOpenCode(cid) end
function T.text(cid) return CM.cmOpenText(cid) end
function T.isOpen(owner, user) return CM.cmStationOpen(owner, user) end
function T.file()
  local f = io.open(BASE .. "mp_company_perms.txt", "r")
  if not f then return nil end
  local s = f:read("*a"); f:close(); return s
end
function T.written() return CM.cmPermsWritten end
function T.clearWritten() CM.cmPermsWritten = nil; local f = io.open(BASE .. "mp_company_perms.txt", "w"); f:write(""); f:close() end
function T.writePerms() CM.cmWritePerms() end
function T.roundTrip()
  local st = CM.cmSaveState()
  local codes = {}
  for k, v in pairs(st.open or {}) do codes[#codes + 1] = k .. "=" .. v end
  table.sort(codes)
  CM.cmOpen = {}
  CM.cmSaved = st
  CM.cmMyCompany = 1
  CM.cmApplySaved()
  return table.concat(codes, ",") .. " -> " .. CM.cmOpenCode(1) .. "|" .. CM.cmOpenCode(2) .. "|" .. CM.cmOpenCode(3)
end
function T.line(userCid, ...) local ok, why = CM.cmLineStopsPermitted(userCid, { ... }); return ok, why end
function T.dissolve(cid)
  CM.peers = {}; CM.cmPlayersOf = function() return {} end; CM.cmMoveAssets = function() return 0 end
  CM.cmWallet = function() return 0, 0 end; CM.cmSetWallet = function() end; CM.cmPwOk = function() return true end
  CM.execCompanyCmd({ op = "CMDEL", cid = cid, origin = "a" })
end
function T.human() local h = nil; return h end
return T
''')

# stub what cmApplySaved needs to accept the state (the human entity is company 1's)
L.execute("api.engine.util = { getPlayer = function() return 7 end }")

check("default: open to everyone", T.code(1) == "*" and T.text(1) == "everyone" and T.isOpen(1, 2) is True)
T.open("a", 1, "*", 0)
check("CMOPEN * 0: nobody", T.code(1) == "-" and T.text(1) == "nobody" and T.isOpen(1, 2) is False and T.isOpen(1, 1) is True, T.code(1))
T.open("a", 1, "*", 1)
check("CMOPEN * 1: everyone again", T.code(1) == "*")
T.open("a", 1, "2", 0)
check("deny one from everyone: an explicit set of the rest", T.code(1) == "3" and T.isOpen(1, 2) is False and T.isOpen(1, 3) is True, T.code(1))
check("the text names the open companies", T.text(1) == "Cy Co", T.text(1))
T.open("a", 1, "2", 1)
check("allowing it back folds to everyone", T.code(1) == "*", T.code(1))
T.open("a", 1, "*", 0)
T.open("a", 1, "3", 1)
check("allow one from nobody: just that one", T.code(1) == "3" and T.isOpen(1, 3) is True and T.isOpen(1, 2) is False, T.code(1))
note = T.open("b", 1, "*", 1)
check("only a player of the company may change it", T.code(1) == "3" and "cannot set company 1's permissions" in note, note)
note = T.open("a", 1, "9", 1)
check("an unknown company is refused", "no company 9" in note, note)
T.open("b", 2, "*", 0)
check("b closes company 2's stations", T.code(2) == "-")

# the file
T.clearWritten()
T.writePerms()
f = T.file()
check("mp_company_perms.txt: player entities and codes", f == "pid 7 1\npid 8 2\npid 9 3\nopen 1 3\nopen 2 -\nopen 3 *\n", repr(f))
w1 = T.written()
T.writePerms()
check("unchanged state: not rewritten", T.written() == w1)

# the line check (a's company 1 is open to 3 only; company 2 is closed; 3 is open)
ok, why = T.line(2, 500)
check("company 2's line at company 1's station: refused", ok is False and "Ada Co's stations are not open to Bob Co" in str(why), why)
ok, why = T.line(3, 500)
check("company 3's line at company 1's station: allowed", ok is True)
ok, why = T.line(1, 500, 700)
check("own station and an open company's station: allowed", ok is True)
ok, why = T.line(1, 600)
check("company 1's line at closed company 2's station: refused", ok is False, why)
ok, why = T.line(1, 12345)
check("a station with no known owner: allowed", ok is True)
ok, why = T.line(None, 600)
check("no company on the command (coop): allowed", ok is True)

# save / load
r = T.roundTrip()
check("codes survive cmSaveState / cmApplySaved", r == "1=3,2=- -> 3|-|*", r)

# dissolve
T.dissolve(3)
check("a dissolved company leaves every set", T.code(1) == "-" and T.code(3) == "*", T.code(1) + " " + T.code(3))

print("FAILED: " + ", ".join(fails) if fails else "ALL PASS")
sys.exit(1 if fails else 0)
