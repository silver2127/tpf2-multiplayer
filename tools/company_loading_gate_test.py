"""Company changes wait for everyone to load in (2026-09-16; registry 2026-09-27).

The menu DLL writes mp_loading.txt ("letter=name=stage" per player still
receiving the save, loading the world or catching up). companies.lua reads it;
the request path (CM.cmRequest, fed by inject.lua) refuses to ship CMNEW /
CMSWITCH / CMDEL while it is not empty (CMPW still goes), with a note naming who
is loading; the COMPANIES tab says the same. This runs the real reader and the
real request path, on Lua 5.2.

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
GUI = open(os.path.join(MP, "companies_gui.lua"), encoding="utf-8", errors="replace").read()
MENU = open(os.path.join(REPO, "native", "src", "menu_hook.cpp"), encoding="utf-8", errors="replace").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


with tempfile.TemporaryDirectory() as td:
    base = td.replace("\\", "/") + "/"
    with open(base + "tpf2_instance.txt", "w") as f:
        f.write("a\nentity_owner_v1=1\n")
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = COMPANIES
    L.globals().SHARED = SHARED
    L.globals().BASE = base
    T = L.execute(r'''
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = { INSTANCE = "a", BASE = BASE, IDENTITY_FILE = BASE .. "tpf2_instance.txt" }
local CM = { ticks = 0, peerSeen = true, escName = function(s) return s end, unescName = function(s) return s end }
assert(load(SRC, "@companies.lua"))()(CM, K, function() end)
CM.cmBooted, CM.cmJoined = true, true
local notes, scheduled = {}, {}
CM.cmNote = function(s) notes[#notes + 1] = s end
CM.scheduleLocal = function(op, c) scheduled[#scheduled + 1] = op .. " " .. tostring(c.cid) end
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
function T.request(line)
  local w = {}
  for tok in line:gmatch("%S+") do w[#w + 1] = tok end
  CM.cmRequest(w)
  return (scheduled[#scheduled] or "-") .. " / " .. (notes[#notes] or "-")
end
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
    r = T.request("CMNEW 3 1 Acme")
    check("so is a new company", r.startswith("- /"), r)
    r = T.request("CMDEL 3 1")
    check("and a delete", r.startswith("- /"), r)
    r = T.request("CMPW 1 secret")
    check("a password change still goes", r.startswith("CMPW 1 /"), r)
    T.reset()
    T.loading("")
    r = T.request("CMSWITCH 2")
    check("once everyone is in, the switch ships", r.startswith("CMSWITCH 2 /") and T.count() == 1, r)

check("inject.lua hands the tab's requests to CM.cmRequest", "CM.cmRequest(w)" in INJECT)
check("the menu DLL writes mp_loading.txt from the roster stages",
      'L"%smp_loading.txt"' in MENU and "if (g_stages[i].empty()) continue;" in MENU)
check("the COMPANIES tab checks it and shows the note",
      "CM.cmLoadingPlayers()" in GUI and "D.coLoadingNote" in GUI)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
