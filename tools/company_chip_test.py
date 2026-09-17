"""The lobby's company chip ring (native/src/menu_hook.cpp, OnHit case 20..35).

A left click increases: the next company id somebody already uses, then one
brand-new id. A right click decreases: the previous used id, and from the
lowest round to the highest in use; it never creates a company. This lifts the
two rules out of the source by text anchor and runs them in Python over
rosters, and checks that the mouse hook passes the button through.

    python tools/company_chip_test.py
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(REPO, "native", "src", "menu_hook.cpp"), encoding="utf-8", errors="replace").read()
MAX_COMPANIES = 200

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def ring(cur, companies, button):
    """The C++ rules, transcribed; the anchors below pin the transcription to the source."""
    used = {c for c in companies if 1 <= c <= MAX_COMPANIES}
    max_used = max(used) if used else 0
    fresh = max_used < MAX_COMPANIES
    nxt = 0
    if button == 2:
        for c2 in range(cur - 1, 0, -1):
            if c2 in used:
                nxt = c2
                break
        if not nxt:
            nxt = max_used
    else:
        for c2 in range(cur + 1, max_used + 1):
            if c2 in used:
                nxt = c2
                break
        if not nxt:
            nxt = max_used + 1 if (cur <= max_used and fresh) else 1
    return max(nxt, 1)


# the source carries exactly these rules
check("left: next used id, then a brand-new id, then 1",
      "for (int c2 = cur + 1; c2 <= maxUsed; c2++) if (used[c2]) { next = c2; break; }" in SRC
      and "if (!next) next = (cur <= maxUsed && fresh) ? maxUsed + 1 : 1;" in SRC)
check("right: previous used id, from the lowest round to the highest in use; never a new one",
      "for (int c2 = cur - 1; c2 >= 1; c2--) if (used[c2]) { next = c2; break; }" in SRC
      and "if (!next) next = maxUsed;" in SRC and "if (next == cur) break;   // the only company there is" in SRC)
check("the mouse hook captures right clicks on the panel and records the button",
      "(wp==WM_LBUTTONDOWN || wp==WM_RBUTTONDOWN)" in SRC and "InterlockedExchange(&g_panelClickButton, wp==WM_RBUTTONDOWN ? 2 : 1);" in SRC
      and "wp==WM_RBUTTONUP && capturedR" in SRC)
check("PollClick hands the button to OnHit; a right click acts on chips only",
      "OnHit(hh.id, button)" in SRC and "if (button == 2 && !(id >= 20 && id <= 35)) return;" in SRC)
check("the legend says so", "Left-click a chip for the next company, right-click for the previous." in SRC)

# players on 1, 1, 3: left climbs 1 -> 3 -> 4 (new); right descends 3 -> 1 -> 3 (round), never to a new id
roster = [1, 1, 3]
check("left: 1 -> 3 -> 4 (a new company)", [ring(1, roster, 1), ring(3, roster, 1)] == [3, 4])
check("right: 3 -> 1, and from the lowest round to the highest", [ring(3, roster, 2), ring(1, roster, 2)] == [1, 3])
# right undoes a left that landed on an existing company, for several rosters
for roster in ([1, 1], [1, 2], [2, 2], [1, 1, 3], [1, 4, 6], [1, 2, 3, 4, 5], [3, 5]):
    used = sorted(set(roster))
    bad = [(p, ring(ring(p, roster, 1), roster, 2)) for p in used[:-1] if ring(ring(p, roster, 1), roster, 2) != p]
    check(f"roster {roster}: right undoes left below the top", not bad, str(bad))
    check(f"roster {roster}: right never leaves the used ids", all(ring(p, roster, 2) in used for p in used))
    check(f"roster {roster}: left from the top makes a new company", ring(used[-1], roster, 1) == used[-1] + 1)
check("alone in company 1, a right click changes nothing", ring(1, [1], 2) == 1)
check("at 200 companies a left click wraps to 1", ring(200, [1, 200], 1) == 1)

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
