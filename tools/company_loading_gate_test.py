"""Company changes wait for everyone to load in (2026-09-16).

The menu DLL writes mp_loading.txt ("letter=name=stage" per player still
receiving the save, loading the world or catching up). companies.lua reads it;
inject.lua refuses to ship CMNEW / CMSWITCH / CMDEL while it is not empty (CMPW
still goes), with a note naming who is loading; the dashboard's buttons say the
same. This runs the real reader and the real inject branch, on Lua 5.2.

    python tools/company_loading_gate_test.py
"""
import os
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
COMPANIES = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
SHARED = open(os.path.join(MP, "shared_infra.lua"), encoding="utf-8").read()
INJECT = open(os.path.join(MP, "inject.lua"), encoding="utf-8", errors="replace").read()
LOCKSTEP = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua"),
                encoding="utf-8", errors="replace").read()
MENU = open(os.path.join(REPO, "native", "src", "menu_hook.cpp"), encoding="utf-8", errors="replace").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# the inject branch, lifted by its anchors: from the company-command test to the end of its block
start = INJECT.index('if o == "CMNEW" or o == "CMSWITCH" or o == "CMDEL" or o == "CMPW" then')
end = INJECT.index('elseif o == "CMOPEN" then', start)
branch = INJECT[start:end]

with tempfile.TemporaryDirectory() as td:
    base = td.replace("\\", "/") + "/"
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = COMPANIES
    L.globals().SHARED = SHARED
    L.globals().BRANCH = branch
    L.globals().BASE = base
    T = L.execute(r'''
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = setmetatable({ INSTANCE = "a", BASE = BASE }, { __index = function() return nil end })
local CM = setmetatable({ ticks = 0 }, { __index = function() return function() return nil end end })
assert(load(SRC, "@companies.lua"))()(CM, K, function() end)
local notes, scheduled, logs = {}, {}, {}
CM.cmNote = function(s) notes[#notes + 1] = s end
CM.scheduleLocal = function(op, c) scheduled[#scheduled + 1] = op .. " " .. tostring(c.cid) end
CM.cmNextId = function() return 9 end
CM.cmHashPw = function() return nil end
local function request(line)
  local w = {}
  for tok in line:gmatch("%S+") do w[#w + 1] = tok end
  local o = w[1]
  local run = assert(load("local CM, K, w, o, log = ...\n" .. BRANCH .. "\nend", "@inject-branch"))
  run(CM, K, w, o, function(s) logs[#logs + 1] = s end)
end
local T = {}
function T.loading(text)
  local f = assert(io.open(BASE .. "mp_loading.txt", "w")); f:write(text); f:close()
  local out = {}
  for _, p in ipairs(CM.cmLoadingPlayers()) do out[#out + 1] = p.letter .. ":" .. p.name .. ":" .. p.stage end
  return table.concat(out, "|")
end
function T.note(text)
  local f = assert(io.open(BASE .. "mp_loading.txt", "w")); f:write(text); f:close()
  return CM.cmLoadingNote(CM.cmLoadingPlayers())
end
function T.request(line) request(line); return (scheduled[#scheduled] or "-") .. " / " .. (notes[#notes] or "-") end
function T.reset() scheduled, notes = {}, {} end
function T.missing() os.remove(BASE .. "mp_loading.txt"); return #CM.cmLoadingPlayers() end
function T.count() return #scheduled end
return T
''')
    check("no file: nobody is loading", T.missing() == 0)
    check("an empty file: nobody is loading", T.loading("") == "")
    got = T.loading("b=bob=receiving save 40%\nc=cid=loading world\n")
    check("the file lists who is loading, by name and stage", got == "b:bob:receiving save 40%|c:cid:loading world", got)
    got = T.loading("a=me=loading world\nb=bob=catching up (12 s behind)\n")
    check("this game's own line is ignored", got == "b:bob:catching up (12 s behind)", got)
    n1 = T.note("b=bob=loading world\n")
    n2 = T.note("b=bob=loading world\nc=cid=receiving save 10%\n")
    check("the note names them", n1 == "company changes wait until bob has loaded in"
          and n2 == "company changes wait until bob, cid have loaded in", n1 + " / " + n2)

    T.loading("b=bob=loading world\n")
    r = T.request("CMSWITCH 2")
    check("a switch is refused while bob loads (nothing scheduled, a note says why)",
          r == "- / company changes wait until bob has loaded in", r)
    r = T.request("CMNEW")
    check("so is a new company", r.startswith("- /"), r)
    r = T.request("CMDEL 3")
    check("and a dissolve", r.startswith("- /"), r)
    r = T.request("CMPW 1 secret")
    check("a password change still goes", r.startswith("CMPW 1 /"), r)
    T.reset()
    T.loading("")
    r = T.request("CMSWITCH 2")
    check("once everyone is in, the switch ships", r.startswith("CMSWITCH 2 /") and T.count() == 1, r)

check("the menu DLL writes mp_loading.txt from the roster stages",
      'L"%smp_loading.txt"' in MENU and "if (g_stages[i].empty()) continue;" in MENU)
check("the dashboard's company buttons check it and show the note",
      "CM.cmLoadingPlayers()" in LOCKSTEP and "D.coLoadingNote" in LOCKSTEP)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
