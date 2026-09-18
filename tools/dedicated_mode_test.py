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
            "dedicated_companies", "dedicated_autosave_min", "dedicated_empty_speed", "dedicated_pause_empty", "dedicated_port", "dedicated_render"):
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
check("the mod is told (mp_dedicated.txt: dedicated=1, empty_speed=)", 'L"%smp_dedicated.txt"' in tick and 'empty_speed=%d' in tick)
check("the tick runs by the clock, not per present", "if (now - lastTick < 1000) return;" in tick and "% 60" not in tick)
sub = re.search(r"static VkResult VKAPI_CALL mySubmit\(.*?\n\}\n", MENU, re.S)
check("dedicated_render=0: vkQueueSubmit is intercepted and its command buffers taken out", bool(sub)
      and "copy[i].commandBufferCount = 0;" in sub.group(0) and "return g_origSubmit(q, n, copy, fence);" in sub.group(0))
check("  ... the fence and semaphores still reach the real submit (a straight copy of each VkSubmitInfo)", bool(sub) and "copy[i] = pSubmits[i];" in sub.group(0))
check("  ... hooked through the device proc-addr interceptor", 'if (strcmp(name, "vkQueueSubmit") == 0) {' in MENU and "return (PFN_vkVoidFunction)mySubmit;" in MENU)
check("  ... query results read as zero, available at once", "static VkResult VKAPI_CALL myQueryResults(" in MENU and 'strcmp(name, "vkGetQueryPoolResults") == 0' in MENU)
check("  ... the panel is not drawn while not rendering", "if (!NoRender() && (InterlockedCompareExchange(&g_showOverlay, 0, 0)" in MENU)
check("  ... the swapchain is never acquired from or presented to: acquire answered here (round robin + empty signal submit)",
      "static VkResult NullAcquire(VkSemaphore sem, VkFence fence, uint32_t* pIndex)" in MENU
      and 'strcmp(name, "vkAcquireNextImageKHR") == 0' in MENU and 'strcmp(name, "vkAcquireNextImage2KHR") == 0' in MENU)
check("  ... present consumes its wait semaphores and returns without the window system, paced to 60 frames/s",
      "NullSignal(q, VK_NULL_HANDLE, VK_NULL_HANDLE, pi->waitSemaphoreCount, pi->pWaitSemaphores);" in MENU
      and "static const double NORENDER_FPS = 60.0;" in MENU and "NullPace();" in MENU
      and MENU.index("NullPace();") < MENU.index("return g_realPresent(q, pi);"))
check("  ... on by default in dedicated mode, dedicated_render=1 turns drawing back on",
      "static int   g_flagDedRender = 0;" in MENU and "if (g_flagDedicated && !g_flagDedRender) InterlockedExchange(&g_noRender, 1);" in MENU)
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
check("the publisher does not coerce the type back to host (it did: listed as player hosted, 2026-09-18 16:25)",
      'kind if kind in ("relay", "host", "dedicated") else "host"' in LOBBY)

# ---- the Lua rule, for real
m = re.search(r"^function CM\.dedicatedPauseEmpty\(\)\n.*?\n^end\n", PACING, re.S | re.M)
m2 = re.search(r"^function CM\.dedicatedTick\(\)\n.*?\n^end\n", PACING, re.S | re.M)
check("pacing.lua defines CM.dedicatedPauseEmpty and CM.dedicatedTick", bool(m and m2))
check("paceTick asks the dedicated rule first", "\tif CM.dedicatedTick() then return end" in PACING)
check("resync.lua resumes a dedicated server at the players' vote, never 0 (CM.dedicatedResumeSpeed)",
      "speed = CM.dedicatedResumeSpeed(speed)" in RESYNC and RESYNC.count("dedicatedResumeSpeed") >= 2)
check("a dedicated server casts no vote and adds no own speed to the mean",
      "if mine and not CM.dedicated then" in PACING and "elseif not CM.dedicated then" in PACING)
check("players in, nobody voted yet: 1x", 'if CM.dedicated and CM.othersPresent and CM.othersPresent() then return 1, "", 0 end' in PACING)
m4 = re.search(r"^function CM\.dedicatedResumeSpeed\(found\)\n.*?\n^end\n", PACING, re.S | re.M)
check("pacing.lua defines CM.dedicatedResumeSpeed", bool(m4))
check("the leader's lever detector is off on a dedicated server (no hand at its lever)", "if CM.isLeader() and not CM.dedicated then" in PACING)
check("a dedicated server's 0 ceiling never pauses the session; the votes run it",
      "if CM.myCeiling <= 0 and not CM.dedicated then" in PACING and '"dedicated server, no votes"' in PACING)

with tempfile.TemporaryDirectory() as td:
    base = td.replace("\\", "/") + "/"
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = (m.group(0) if m else "") + (m2.group(0) if m2 else "") + (m4.group(0) if m4 else "")
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
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\nempty_speed=0\n")
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
    # empty_speed=1 (the default): alone, the world runs at 1x, whatever the lever read
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\nempty_speed=1\n")
    T.CM.dedCfgAt = None; T.CM.ticks = 1000; T.CM.dedPaused = False
    n = len(T.speeds)
    held, s = T.tick(1, False, False)
    check("empty_speed=1: alone at 3, the world goes to 1x once, 3 remembered",
          held is True and s == 1 and T.CM.dedResume == 3 and len(T.speeds) == n + 1 and T.speeds[n + 1] == "1:dedicated server: nobody is here, 1x", str(T.speeds[n + 1]))
    held, s = T.tick(300, False, False)
    check("and stays there without repeating the command", s == 1 and len(T.speeds) == n + 1)
    T.setSpeed(0)   # something else moved the lever (a released hold, a load)
    held, s = T.tick(30, False, False)
    check("a lever moved under it goes back to 1x", s == 1 and len(T.speeds) == n + 2)
    T.CM.voteSpeed = L.eval("function() return 2 end")
    held, s = T.tick(1, True, False)
    check("somebody in: the votes' speed", held is False and s == 2 and T.speeds[n + 3] == "2:dedicated server: a player is in")
    T.CM.voteSpeed = L.eval("function() return nil end")
    # the older DLL's file: pause_empty=1 is empty_speed 0, pause_empty=0 is 1
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\npause_empty=1\n")
    T.CM.dedCfgAt = None; T.CM.dedPaused = False; T.setSpeed(3)
    held, s = T.tick(1, False, False)
    check("an older DLL's pause_empty=1 still pauses", s == 0 and T.CM.dedEmptySpeed == 0)
    open(os.path.join(td, "mp_dedicated.txt"), "w").write("dedicated=1\npause_empty=0\n")
    T.CM.dedCfgAt = None; T.CM.dedPaused = False; T.setSpeed(3)
    held, s = T.tick(1, False, False)
    check("an older DLL's pause_empty=0 runs at 1x", s == 1 and T.CM.dedEmptySpeed == 1)
    # the resume speed: a found 0 becomes the vote, else the remembered speed, else 1; a found speed stands
    T.CM.dedicated = True; T.CM.voteSpeed = L.eval("function() return nil end"); T.CM.dedResume = None
    check("no vote, nothing remembered: 1", T.CM.dedicatedResumeSpeed(0) == 1)
    T.CM.dedResume = 3
    check("no vote, remembered 3: 3", T.CM.dedicatedResumeSpeed(0) == 3)
    T.CM.voteSpeed = L.eval("function() return 2.5 end")
    check("a vote of 2.5: 3 (whole lever)", T.CM.dedicatedResumeSpeed(0) == 3)
    check("a found speed 2 stands", T.CM.dedicatedResumeSpeed(2) == 2)
    T.CM.dedicated = False
    check("not dedicated: whatever was found, 0 included", T.CM.dedicatedResumeSpeed(0) == 0)

if fails:
    raise SystemExit("FAIL: " + ", ".join(fails))
print("PASS: dedicated mode -- the menu hosts, loads and autosaves by itself; the lobby keeps a stable code and lists as a "
      "dedicated server; the mod pauses the empty world and gives the speed back")
