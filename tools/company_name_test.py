"""Offline checks for company names (companies.lua CMNAME), on Lua 5.2.

A company's name is replicated company state, not the player entity's NAME
component (a switch swaps the entities, so a NAME would follow the wrong
company). This loads the real companies.lua and checks:
  - an unnamed company reads as "<founder>'s company", "<founder>'s 2nd
    company", ... after the player who founded it (the lobby's assignment, or
    CMNEW), whoever plays it now; "Company N" without a founder name
  - the game's own company window is the rename: a SetName captured on our
    company's player entity becomes CMNAME (inject.lua); the dashboard has no
    name field
  - the origin playing a company may name it; the name arrives percent-escaped
    and is stored unescaped and trimmed
  - an origin playing another company may not name it
  - an empty name clears it; a dissolved company loses its name
  - the names ride in the save state and come back from it
  - mp_company_map.txt (for the Big Maps minimap) lists me= and cid=pid=name,
    percent-escaped, rewritten only on change, emptied outside companies mode
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
-- the engine side: NAME components per entity and the setName commands issued
local ename, sets = {}, {}
api = { engine = { getComponent = function(id, t) return ename[id] and { name = ename[id] } or nil end },
        type = { ComponentType = { NAME = 9 } },
        cmd = { make = { setName = function(id, n) return { id = id, n = n } end },
                sendCommand = function(c, cb) ename[c.id] = c.n; sets[#sets + 1] = c.id .. "=" .. c.n; if cb then cb(nil, true) end end } }
local realSwitch = CM.cmLocalSwitch
local T = {}
function T.nameOf(cid) return CM.cmNameOf(cid) end
function T.entityNames() local out = {} for _, id in ipairs({ 7, 8, 9 }) do out[#out + 1] = tostring(ename[id]) end return table.concat(out, "|") end
function T.sets() local n = #sets; sets = {}; return n end
function T.swapSwitch()
  -- a local switch 1 -> 2 in the real code swaps the ENTITIES (7 <-> 8 own each other's assets);
  -- the names must stay with the companies: stub the swap, keep the real re-naming
  CM.cmOwnedEntities = function() return {} end; CM.cmHandOver = function() end; CM.cmSwapWallets = function() return true, 0, 0, 0, 0 end
  realSwitch(2)
  return T.entityNames() .. " mine=" .. tostring(CM.cmMyCompany) .. " pid1=" .. tostring(CM.cmCompanyPid[1]) .. " pid2=" .. tostring(CM.cmCompanyPid[2])
end
function T.name(origin, cid, name) CM.execCompanyCmd({ op = "CMNAME", cid = cid, origin = origin, name = CM.escName(name) }) return notes[#notes] end
function T.raw(cid) return CM.cmName[cid] end
function T.map(dir)
  K.BASE = dir
  CM.cmWriteCompanyMap()
  local f = io.open(dir .. "mp_company_map.txt", "r"); local s = f and f:read("*a") or "?"; if f then f:close() end
  local stamp = CM.cmMapWritten
  CM.cmWriteCompanyMap()                      -- unchanged content: no rewrite
  local same = CM.cmMapWritten == stamp
  local mode = CM.cmMode; CM.cmMode = "coop"; CM.cmWriteCompanyMap(); CM.cmMode = mode
  local f2 = io.open(dir .. "mp_company_map.txt", "r"); local empty = f2 and f2:read("*a") or "?"; if f2 then f2:close() end
  K.BASE = ""
  return s, same, empty
end
function T.defaults()
  -- the lobby handed out 1 to a, 2 to b, 3 to c; only a and b have names
  CM.playerNames = { a = "Ada", b = "bob" }
  CM.cmLobbyOrigin = { a = 1, b = 2, c = 3 }
  local r = { CM.cmNameOf(1), CM.cmNameOf(2), CM.cmNameOf(3), CM.cmDisplayName(2, "Acme", "bob", 1), CM.cmDisplayName(4, nil, nil, 1) }
  return table.concat(r, "|")
end
function T.found(origin, cid) CM.execCompanyCmd({ op = "CMNEW", cid = cid, origin = origin, sw = 0 }); CM.cmCompanyPid[cid] = 100 + cid; return CM.cmNameOf(cid) end
function T.founders()
  -- a founds 4 and 5, b founds 6; a switch changes nothing; a dissolve keeps the numbering
  local r = { T.found("a", 4), T.found("a", 5), T.found("b", 6) }
  CM.cmOriginCompany.b = 4          -- b now plays Ada's 2nd company
  r[#r + 1] = CM.cmNameOf(4) .. "/" .. CM.cmNameOf(2)
  CM.cmOriginCompany.b = 2
  T.dissolve(5)
  r[#r + 1] = T.found("a", 7)
  r[#r + 1] = CM.cmOrdinal(11) .. CM.cmOrdinal(12) .. CM.cmOrdinal(13) .. CM.cmOrdinal(21) .. CM.cmOrdinal(22) .. CM.cmOrdinal(23) .. CM.cmOrdinal(24)
  local st = CM.cmSaveState()
  local f = {}
  for k, v in pairs(st.founded or {}) do f[#f + 1] = k .. "=" .. v.o .. v.n end
  table.sort(f)
  r[#r + 1] = table.concat(f, ",")
  for _, cid in ipairs({ 4, 6, 7 }) do CM.execCompanyCmd({ op = "CMDEL", cid = cid, origin = "a" }) end
  CM.cmFounded, CM.cmFoundedCount = {}, {}
  CM.playerNames, CM.cmLobbyOrigin = {}, nil
  return table.concat(r, "|")
end
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
  api.engine.util = { getPlayer = function() return 7 end }
  CM.cmLocalSwitch = function() return true end
  CM.cmApplySaved()
  return table.concat(names, ","), CM.cmNameOf(1), CM.cmNameOf(2), CM.cmNameOf(3)
end
return T
''')

check("unnamed reads as Company N with no player names", T.nameOf(2) == "Company 2", T.nameOf(2))
d = T.defaults()
check("unnamed reads as <founder>'s company; a given name wins; an unknown founder name falls back",
      d == "Ada's company|bob's company|Company 3|Acme|Company 4", d)
d = T.founders()
check("a founder's later companies are their 2nd, 3rd...; a switch renames nothing; a dissolve keeps the numbering; founders ride in the save",
      d == "Ada's 2nd company|Ada's 3rd company|bob's 2nd company|Ada's 2nd company/bob's company|Ada's 4th company|11th 12th 13th 21st 22nd 23rd 24th |1=a1,2=b1,3=c1,4=a2,6=b2,7=a4", d)
n = T.name("b", 2, "  Acme & Sons  ")
check("the origin playing it names it (trimmed, unescaped)", T.raw(2) == "Acme & Sons", repr(T.raw(2)))
check("the note says who named what", "b named company 2" in n and "Acme & Sons" in n, n)
n = T.name("c", 2, "Hijack")
check("an origin playing another company may not", T.raw(2) == "Acme & Sons" and "cannot name" in n, n)
T.name("a", 1, "Host Rail")
check("the host names its own", T.nameOf(1) == "Host Rail", T.nameOf(1))
check("every company's entity carries its name (finances window)", T.entityNames() == "Host Rail|Acme & Sons|Company 3", T.entityNames())
import tempfile as _tf
with _tf.TemporaryDirectory() as _td:
    _s, _same, _empty = T.map(_td.replace("\\", "/") + "/")
    check("mp_company_map.txt: me= and cid=pid=name, percent-escaped",
          _s == "me=1\n1=7=Host%20Rail\n2=8=Acme%20%26%20Sons\n3=9=Company%203\n", repr(_s))
    check("the map is rewritten only on change and emptied outside companies mode", _same and _empty == "", (_same, repr(_empty)))
T.sets()
T.name("a", 1, "Host Rail")
check("an unchanged name issues no setName", T.sets() == 0)
saved, n1, n2, n3 = T.roundTrip()
check("names ride in the save state", saved == "1=Host Rail,2=Acme & Sons", saved)
check("and come back from it", (n1, n2, n3) == ("Host Rail", "Acme & Sons", "Company 3"), f"{n1}|{n2}|{n3}")
n = T.name("b", 2, "")
check("an empty name clears it", T.nameOf(2) == "Company 2" and "unnamed" in n, n)
T.name("c", 3, "Gone Soon")
T.dissolve(3)
CM_names_after_switch = T.swapSwitch()
check("after a switch the entities swap but the names stay with the companies", CM_names_after_switch == "Company 2|Host Rail|Gone Soon mine=2 pid1=8 pid2=7", CM_names_after_switch)
check("a dissolved company loses its name", T.raw(3) is None, repr(T.raw(3)))

inject = open(INJECT, encoding="utf-8", errors="replace").read()
lockstep = open(LOCKSTEP, encoding="utf-8", errors="replace").read()
check("inject.lua parses CMNAME cid name...", 'o == "CMNAME"' in inject and 'CM.scheduleLocal("CMNAME"' in inject)
check("CMNAME is never solo-dropped nor actions-off dropped", 'and o ~= "CMNAME"' in inject and 'CMNAME = true' in inject)
check("the dispatcher routes CMNAME to execCompanyCmd", 'c.op == "CMNAME"' in lockstep and 'then CM.execCompanyCmd(c)' in lockstep)
check("the dashboard reads the names and picks from a ComboBox", 'conames=' in lockstep and 'api.gui.comp.ComboBox.new()' in lockstep)
check("the dashboard has no name field: the game's company window renames", 'name mine' not in lockstep and 'coNameInput' not in lockstep)
check("a rename of our company's player entity becomes CMNAME",
      'CM.scheduleLocal("CMNAME", { cid = CM.cmMyCompany, name = w[3] })' in inject and 'id == myCompanyPid' in inject)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
