"""Dedicated server mode (2026-09-18): the menu DLL hosts and loads by itself, the mod
pauses the empty world, the lobby keeps a stable code and lists as a dedicated server.

Source anchors for the C++ (the flags, the once-a-second tick from myPresent, the
--dedicated argument, the list label) and the Python (the argument, the kept secret,
the publisher kind, the master's label); the real Lua pause rule runs on Lua 5.2
against a stub game: alone -> paused once (remembering the speed), a hold owns the
clock, somebody in -> the speed comes back once; resync.lua resumes at the
remembered speed, not the 0 it finds.

    python tools/dedicated_mode_test.py
"""
import os
import re
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = open(os.path.join(REPO, "native", "src", "menu_hook.cpp"), encoding="utf-8", errors="replace").read()
LOBBY = open(os.path.join(REPO, "netpunch", "lobby.py"), encoding="utf-8", errors="replace").read()
MASTER = open(os.path.join(REPO, "netpunch", "masterserver.py"), encoding="utf-8", errors="replace").read()
PACING = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "pacing.lua"), encoding="utf-8").read()
RESYNC = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "resync.lua"), encoding="utf-8").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# ---- the menu DLL
for key in ("dedicated", "dedicated_save", "dedicated_lobby", "dedicated_name", "dedicated_password", "dedicated_public",
            "dedicated_companies", "dedicated_autosave_min", "dedicated_pause_empty", "dedicated_port"):
    check(f"ReadFlags parses {key}=", f'!strcmp(line, "{key}")' in MENU)
check("dedicated_save refuses path parts and quotes", 'strpbrk(v, "\\\\/:*?\\"<>|")' in MENU)
check("dedicated_password refuses blanks and quotes (it is an argument)",
      'if (strlen(v) < sizeof(g_flagDedPassword) && !strpbrk(v, "\\" \\t"))' in MENU)
tick = MENU[MENU.index("static void DedicatedTick()\n{"):]
tick = tick[:tick.index("\n}\n") + 3]
check("the tick does nothing unless dedicated=1", "if (!g_flagDedicated) return;" in tick)
check("the tick runs from myPresent after the stage watch", "    StageTick();\n    PollWorldGen();\n    DedicatedTick();" in MENU)
check("no lobby -> host one (StartLobby(0)) with the flags' names", "StartLobby(0);" in tick and "strcpy_s(g_lobbyName, g_flagDedLobby)" in tick)
check("a world without a lobby is left alone (the moment after a crash to the menu)", "if (world) return;" in tick)
check("lobby up, no world -> the configured save, else the newest, through the shared-save autoload",
      "newestSave(path, 600)" in tick and "doStartLoad(path)" in tick and "MarkSaveShared();" in tick)
check("no load while one is pending or the native side is busy",
      "InterlockedCompareExchange(&g_autoLoadPending, 0, 0) || NativeIo::Busy()" in tick)
check("world up -> the game's own autosave every dedicated_autosave_min, never during a native operation",
      "ForceAutosave()" in tick and "g_flagDedAutosaveMin * 60000ULL" in tick and "!NativeIo::Busy()" in tick)
check("the mod is told (mp_dedicated.txt: dedicated=1, pause_empty=)", 'L"%smp_dedicated.txt"' in tick and 'pause_empty=%d' in tick)
check("the host command line carries --dedicated", 'if (g_flagDedicated) wcscat_s(wpub, L" --dedicated");' in MENU)
check("and --local-port from dedicated_port (a box that also runs the relay)", 'L" --local-port %d", g_flagDedPort' in MENU)
check("the public list labels a dedicated game a dedicated server",
      '(!strcmp(r.type, "relay") || !strcmp(r.type, "dedicated")) ? L"dedicated server"' in MENU)

# ---- the lobby and the master
check("lobby.py takes --dedicated", 'ap.add_argument("--dedicated", action="store_true"' in LOBBY)
check("a dedicated game keeps its secret like the relay (a stable code)",
      'if getattr(args, "relay_only", False) or getattr(args, "dedicated", False):' in LOBBY)
check("the publisher announces type dedicated",
      '"relay" if args.relay_only else ("dedicated" if getattr(args, "dedicated", False) else "host")' in LOBBY)
check("the master server labels it", '"dedicated": "dedicated server"' in MASTER)

# ---- the Lua rule, for real
m = re.search(r"^function CM\.dedicatedPauseEmpty\(\)\n.*?\n^end\n", PACING, re.S | re.M)
m2 = re.search(r"^function CM\.dedicatedTick\(\)\n.*?\n^end\n", PACING, re.S | re.M)
check("pacing.lua defines CM.dedicatedPauseEmpty and CM.dedicatedTick", bool(m and m2))
check("paceTick asks the dedicated rule first", "\tif CM.dedicatedTick() then return end" in PACING)
m3 = re.search(r"if CM\.dedPaused and \(speed or 0\) == 0 and CM\.dedResume then speed = CM\.dedResume", RESYNC)
check("resync.lua resumes a paused-because-empty server at the remembered speed", bool(m3))

with tempfile.TemporaryDirectory() as td:
    base = td.replace("\\", "/") + "/"
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = (m.group(0) if m else "") + (m2.group(0) if m2 else "")
    L.globals().BASE = base
    T = L.execute(r'''
local K = { BASE = BASE, LOADGATE_MIN_TICKS = 100 }
local CM = { ticks = 0, peers = {} }
local T = { log = {}, speeds = {} }
local speed = 2
local others = false
game = { interface = { getGameSpeed = function() return speed end } }
CM.othersPresent = function() return others end
CM.setSpeed = function(v, why) speed = v; T.speeds[#T.speeds + 1] = v .. ":" .. why end
local function log(s) T.log[#T.log + 1] = s end
assert(load("local CM, K, log = ...\n" .. SRC, "@dedicated"))(CM, K, log)
T.CM, T.K = CM, K
function T.tick(n, o, hold)
  others = o; CM.resyncHold = hold
  local held
  for _ = 1, n do CM.ticks = CM.ticks + 1; held = CM.dedicatedTick() end
  return held, speed
end
function T.setSpeed(v) speed = v end
return T
''')
    # no file: an ordinary game, the rule is off
    held, s = T.tick(400, False, False)
    check("without mp_dedicated.txt nothing happens (an ordinary game)", held is False and s == 2 and len(T.speeds) == 0)
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\npause_empty=1\n")
    T.CM.dedCfgAt = None
    held, s = T.tick(1, False, False)
    check("alone at speed 2: paused once, the speed remembered", held is True and s == 0 and T.CM.dedResume == 2
          and len(T.speeds) == 1 and T.speeds[1] == "0:dedicated server: nobody is here", str(T.speeds[1]))
    held, s = T.tick(300, False, False)
    check("stays paused without repeating the command", held is True and s == 0 and len(T.speeds) == 1)
    held, s = T.tick(5, False, True)
    check("a world operation's hold owns the clock: the rule steps aside", held is False and len(T.speeds) == 1)
    held, s = T.tick(1, True, False)
    check("somebody in: the remembered speed comes back once", held is False and s == 2 and T.speeds[2] == "2:dedicated server: a player is in")
    held, s = T.tick(50, True, False)
    check("and is not touched again while they stay", len(T.speeds) == 2 and s == 2)
    # the player pauses the game on purpose while others are in: not ours to undo
    T.setSpeed(0)
    held, s = T.tick(50, True, False)
    check("a pause with others present is not ours to undo", s == 0 and len(T.speeds) == 2)
    T.setSpeed(1)
    held, s = T.tick(1, False, False)
    check("alone again from speed 1: paused, remembering 1", s == 0 and T.CM.dedResume == 1)
    # a fresh Lua state right after the load: below LOADGATE_MIN_TICKS the load gate owns the speed
    T.CM.ticks = 0; T.CM.dedPaused = False; T.setSpeed(3)
    held, s = T.tick(50, False, False)
    check("right after a load the load gate keeps the clock (no pause below LOADGATE_MIN_TICKS)", s == 3 and held is False)
    # pause_empty=0: never
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\npause_empty=0\n")
    T.CM.dedCfgAt = None; T.CM.ticks = 1000
    n = len(T.speeds)
    held, s = T.tick(400, False, False)
    check("pause_empty=0: the server keeps simulating while empty", s == 3 and len(T.speeds) == n)

if fails:
    raise SystemExit("FAIL: " + ", ".join(fails))
print("PASS: dedicated mode -- the menu hosts, loads and autosaves by itself; the lobby keeps a stable code and lists as a "
      "dedicated server; the mod pauses the empty world and gives the speed back")
