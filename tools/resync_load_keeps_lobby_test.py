"""A resync load must not read as "the player left the world" (2026-09-16).

Leaving a world for the title menu leaves the lobby (menu_hook.cpp, CreatePage
page 2 with a CGameUI pointer still set). A resync loads the host's snapshot
through the engine's own load path, which builds that same page between the old
world and the loading screen: every joiner sent LEAVE mid-resync and the host's
barrier failed with "Player disconnected or roster changed". The page-2 rule now
asks NativeIo::Loading() first, which is true from Load() until the new world's
CGameUI constructs (or the engine refused the load). Source anchors, no engine.

    python tools/resync_load_keeps_lobby_test.py
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "native", "src")
MENU = open(os.path.join(SRC, "menu_hook.cpp"), encoding="utf-8", errors="replace").read()
IO_CPP = open(os.path.join(SRC, "native_io.cpp"), encoding="utf-8", errors="replace").read()
IO_H = open(os.path.join(SRC, "native_io.h"), encoding="utf-8", errors="replace").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# the page-2 branch of the CreatePage detour
start = MENU.index("static void MyCreatePage(")
page2 = MENU[MENU.index("if (page == 2) {", start):MENU.index("else if (page >= 3)", start)]
leave = [m.start() for m in re.finditer(r"InterlockedExchange\(&g_leaveOnMenu, 1\)", page2)]
check("the page-2 branch sets g_leaveOnMenu in exactly one place", len(leave) == 1, str(len(leave)))
guard = page2[:leave[0]] if leave else ""
check("that place is gated on a lobby running and a CGameUI pointer still set",
      "g_gameUi != 0 && LobbyRunning()" in guard)
check("and on the load NOT being ours (NativeIo::Loading() decides before the flag)",
      "NativeIo::Loading()" in guard and guard.rindex("NativeIo::Loading()") > guard.rindex("LobbyRunning()"))
check("g_gameUi is still cleared on page 2 either way", "g_gameUi = 0;" in page2[leave[0]:] if leave else False)

# NativeIo::Loading covers the whole window: queued and in flight
check("native_io.h declares Loading()", re.search(r"^bool Loading\(\);", IO_H, re.M) is not None)
m = re.search(r"bool Loading\(\)\s*\{[^}]*\}", IO_CPP)
body = m.group(0) if m else ""
check("Loading() is true while a load is queued or in flight",
      "State::QueuedLoad" in body and "State::Loading" in body, body[:120])
check("Loading() takes the state lock", "lock_guard" in body)
# the window closes where the load completes or is refused, not at the old world's destructor
dtor = IO_CPP[IO_CPP.index("uintptr_t destructorHook("):IO_CPP.index("void execute()")]
check("the old world's destructor does not end a Loading state (the title menu is built after it)",
      "State::Loading" not in dtor and "State::QueuedLoad" not in dtor)
ctor = IO_CPP[IO_CPP.index("uintptr_t constructorHook("):IO_CPP.index("uintptr_t destructorHook(")]
check("the new world's constructor ends it", "state==State::Loading && accepted" in ctor and "state=State::Idle" in ctor)

# the leave-on-menu consumer still exists (the fix narrows the rule, it does not delete it)
check("myPresent still leaves the lobby when the flag is set",
      "InterlockedExchange(&g_leaveOnMenu, 0)" in MENU and "LeaveLobby();" in MENU)

if fails:
    raise SystemExit("FAIL: " + ", ".join(fails))
print("PASS: a resync's own load keeps the joiner in the lobby; a real quit to the title menu still leaves it")
