"""The lobby panel stays up while a world loads (2026-09-18, user).

With a lobby running, the loading screen (CreatePage 16) keeps the multiplayer
panel on the lobby view, so every player sees the roster and what each member is
doing (receiving the save, loading the world, catching up). It is drawn even
during a native save/load -- the frozen join is one -- but takes no clicks then,
and once the world is up it hands over to the in-game panel, open. Source
anchors in menu_hook.cpp, no engine.

    python tools/loading_panel_test.py
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = open(os.path.join(REPO, "native", "src", "menu_hook.cpp"), encoding="utf-8", errors="replace").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


check("a g_loadingPanel flag exists", "static volatile LONG g_loadingPanel = 0;" in MENU)

start = MENU.index("static void MyCreatePage(")
p16 = re.search(r"else if \(page == 16 && LobbyRunning\(\)\) \{(.*?)\n\s*\}", MENU[start:], re.S)
check("CreatePage 16 with a lobby running keeps the overlay on the lobby view", bool(p16))
body = p16.group(1) if p16 else ""
check("  ... showing the overlay, raising the flag, on the lobby page (uiState 2), repainted",
      "InterlockedExchange(&g_showOverlay, 1)" in body and "InterlockedExchange(&g_loadingPanel, 1)" in body
      and "InterlockedExchange(&g_uiState, 2)" in body and "InterlockedExchange(&g_panelDirty, 1)" in body)
p3 = MENU[start:][MENU[start:].index("else if (page >= 3)"):][:200]
check("any other in-game page drops both the overlay and the flag",
      "g_showOverlay, 0" in p3 and "g_loadingPanel, 0" in p3)
page2 = MENU[MENU.index("if (page == 2) {", start):MENU.index("else if (page == 16", start)]
check("the title page (2) clears the flag", "InterlockedExchange(&g_loadingPanel, 0)" in page2)

# the draw gate in myPresent
gate = MENU[MENU.index("const bool loadingPanel = InterlockedCompareExchange(&g_loadingPanel, 0, 0) != 0;"):]
gate = gate[:gate.index("__except")] if "__except" in gate else gate[:3000]
check("the world coming up hands the loading view over to the in-game panel, open",
      "if (loadingPanel && WorldLoaded())" in gate and "InterlockedExchange(&g_ingameOverlay, 1)" in gate
      and gate.index("if (loadingPanel && WorldLoaded())") < gate.index("InterlockedExchange(&g_ingameOverlay, 1)"))
# OverlayWanted(bool& quiet), shared by both renderers since 2026-09-21
check("quiet = no recovery world I/O and no native I/O", "quiet = !g_recoveryWorldIo && !NativeIo::Busy();" in gate)
check("the panel is drawn when quiet OR while the loading view is up", "(quiet || loadingPanel)" in gate)
check("clicks are taken only when quiet", "DrawButton(q, idx); if (quiet) PollClick();" in gate)
done = MENU[MENU.index('Back to the LOBBY VIEW when the panel is open'):][:900]
check("a completed world operation returns an open panel to the lobby view (2), collapses only a closed one",
      "InterlockedExchange(&g_uiState, open ? 2 : 0);" in done and "InterlockedCompareExchange(&g_ingameOverlay,0,0) || InterlockedCompareExchange(&g_showOverlay,0,0)" in done)

if fails:
    print("FAIL:", len(fails), "check(s):", "; ".join(fails))
    raise SystemExit(1)
print("PASS: the lobby panel stays up through the loading screen and hands over to the in-game panel")
