"""Offline checks that the multiplayer mod never stops the game's own load, on Lua 5.2.

A player with 503 mods could not create a new game: lockstep.lua's data-folder lookup failed
with the error value "file (0000000000000000)" (io.open or close threw a file handle -- the
game script state is shared with every mod's game scripts) and the mod then called error(),
which the game reports as "Exception during init" and aborts (2026-09-12).

This loads the real mod/.../config/game_script/lockstep.lua in a bare lupa.lua52 runtime:
  - io.open throws a file handle: the lookup catches it, falls back to LOCALAPPDATA, and the
    chunk completes with data() defined
  - no LOCALAPPDATA and no TPF2MP_DATADIR: the lookup fails, the chunk does NOT raise, data()
    returns an empty game script and stdout says multiplayer is off
  - a module that cannot be required (none resolve in this runtime): the chunk does not raise,
    data() returns an empty game script, and the reason names the module

    python tools/boot_resilience_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def run(setup):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().LS_SRC = open(LS, encoding="utf-8").read()
    return L.execute(r'''
local out = {}
print = function(...) local t = {} for i = 1, select("#", ...) do t[#t + 1] = tostring(select(i, ...)) end out[#out + 1] = table.concat(t, " ") end
''' + setup + r'''
-- no module resolves here, exactly like a broken require
package.path = ""; package.cpath = ""
local ok, err = pcall(function() assert(load(LS_SRC, "@lockstep.lua"))() end)
local d = nil
if type(data) == "function" then d = data() end
local n = 0
if type(d) == "table" then for _ in pairs(d) do n = n + 1 end end
return { ok = ok, err = tostring(err), hasData = type(data) == "function", dataType = type(d), dataFields = n, out = table.concat(out, "\n") }
''')


# 1. io.open throws a file handle (what the player's game reported)
r = run(r'''
os.getenv = function(k) if k == "LOCALAPPDATA" then return "C:/Users/x/AppData/Local" end end
local realOpen = io.open
local fh = realOpen and io.tmpfile and io.tmpfile() or {}
io.open = function() error(fh) end
''')
check("io.open throwing a file handle: the chunk does not raise", r.ok, r.err)
check("io.open throwing a file handle: data() exists", r.hasData)
check("io.open throwing a file handle: falls back to LOCALAPPDATA",
      "data folder C:/Users/x/AppData/Local/tpf2mp/data/" in r.out, r.out[-300:])

# 2. nothing readable: no LOCALAPPDATA, no TPF2MP_DATADIR
r = run(r'''
os.getenv = function() return nil end
''')
check("no data folder: the chunk does not raise", r.ok, r.err)
check("no data folder: data() returns an empty game script", r.hasData and r.dataType == "table" and r.dataFields == 0,
      f"{r.dataType} {r.dataFields}")
check("no data folder: stdout says multiplayer is off", "multiplayer is OFF for this game" in r.out, r.out[-300:])

# 2b. a non-ASCII profile folder and no TPF2MP_DATADIR from the DLLs: off, with the reason
r = run(r'''
os.getenv = function(k) if k == "LOCALAPPDATA" then return "C:/Users/profile\233\252/AppData/Local" end end
''')
check("non-ASCII profile without the DLLs: the chunk does not raise", r.ok, r.err)
check("non-ASCII profile without the DLLs: empty game script", r.hasData and r.dataType == "table" and r.dataFields == 0)
check("non-ASCII profile without the DLLs: says why", "non-ASCII characters" in r.out and "multiplayer is OFF" in r.out, r.out[-300:])

# 3. the data folder resolves but a module cannot be required
r = run(r'''
os.getenv = function(k) if k == "LOCALAPPDATA" then return "C:/Users/x/AppData/Local" end end
''')
check("module load failure: the chunk does not raise", r.ok, r.err)
check("module load failure: data() returns an empty game script", r.hasData and r.dataType == "table" and r.dataFields == 0,
      f"{r.dataType} {r.dataFields}")
check("module load failure: stdout names the module and says it is off",
      "require('mp.hash') failed" in r.out and "is OFF for this game" in r.out, r.out[-400:])
check("module load failure: later modules are not attempted", "loading mp.io" not in r.out)


# Reach the Fences adapter with other module factories stubbed. Exercise the
# real game-script boot guard, including Linux's loader-provided environment.
check("earlier boot failure skips Fences", "loading mp.fences_compat" not in r.out)
for label, behavior, reason in (
    ("missing module", "error('fences unavailable')", "fences unavailable"),
    ("wrong export", "return false", "returned a boolean without bind"),
    ("missing bind", "return {}", "returned a table without bind"),
    ("invalid bind", "return {bind=true}", "without bind"),
    ("bind error", "return {bind=function() error('fences bind failed') end}", "fences bind failed"),
):
    r = run(r'''
local getenv = function(k)
    if k == "LOCALAPPDATA" then return "/fixture/.local/share" end
    if k == "TPF2MP_DATADIR" then return "/fixture/.local/share/tpf2mp/data/" end
end
os.getenv = getenv
require = function(name)
    if name == "mp/fences_compat" then
''' + behavior + r'''
    end
    return function() return {} end
end
''')
    check("Fences " + label + ": chunk completes with empty game script",
          r.ok and r.hasData and r.dataType == "table" and r.dataFields == 0, r.err)
    check("Fences " + label + ": diagnostic identifies failure",
          "loading mp.fences_compat" in r.out and reason in r.out
          and "is OFF for this game" in r.out, r.out[-400:])
    check("Fences " + label + ": later modules skipped", "loading mp.stats" not in r.out)

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("all passed")
