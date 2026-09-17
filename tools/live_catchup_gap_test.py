"""Offline check (Lua 5.2, real net.lua): a gap below a LIVE catch-up's history feed stays owed.

The rig, 2026-09-16 (a desync with 25 planes on the host and 24 on the joiner): the joiner
received a's seq 63, learned of seq 64 from a's heartbeat (hi=64), lost the line itself,
held for it, fell 8.6 units behind and entered catch-up. The host served the history
AFTER the joiner's clock (574.4), which is exactly seq 64's stamp, so the feed was seq
65..66. That feed arrived within the 15-tick NACK grace, and the "between two runs means
not owed" rule (meant for an origin whose sequence restarted after a rejoin, in a
catch-up FROM A SAVE) wrote seq 64 off: no NACK ever went out, the hold "released", the
plane was never bought on the joiner. This drives the real rxNote/rxAdvertise/
rxHistRange/nackScan/rxGaps/histServe through that sequence and checks:
  - live catch-up: seq 64 stays owed, is NACKed after the grace, and counts as a gap
  - the hold's release line says WRITTEN OFF / arrived truthfully
  - the host serves a command stamped exactly at a live requester's clock (>=), and
    still strictly after a save's stamp
  - save-based catch-up: the between-runs write-off still applies (a restarted origin)

    python tools/live_catchup_gap_test.py
"""
import os
import sys

# The harness (runtime/check/lua_list) lives in hist_retention_test.py, whose module body
# runs its own checks and ends with SystemExit: take its definitions only.
HERE = os.path.dirname(os.path.abspath(__file__))
_src = open(os.path.join(HERE, "hist_retention_test.py"), encoding="utf-8").read()
_ns = {"__file__": os.path.join(HERE, "hist_retention_test.py"), "__name__": "hist_retention_harness"}
exec(compile(_src[:_src.index("# ---- the history has no count cap ----")], "hist_retention_test.py", "exec"), _ns)
runtime, check, fails, lua_list = _ns["runtime"], _ns["check"], _ns["fails"], _ns["lua_list"]


def gap_case(live):
    L, h = runtime(instance="b", leader=False)
    CM = h.CM
    # a's seq 63 arrives live; the heartbeat then advertises hi=64; the line itself is lost
    h.CM.ticks = 1000
    CM.rxNote("a", 63)
    CM.rxAdvertise("a", 64)
    n, oldest, who, seq = CM.rxGaps()
    check(f"[{'live' if live else 'save'}] seq 64 is a known gap once advertised", n == 1 and who == "a" and seq == 64, f"n={n} who={who} seq={seq}")
    # the joiner had loaded a save earlier: the load gate's feed announced a's run 1..1 then
    CM.rxHistRange("a", 1, 1)
    # the catch-up's history feed answers within the NACK grace: a run 65..66 (the host served > 574.4)
    CM.histLive = live
    h.CM.ticks = 1005
    CM.rxHistRange("a", 65, 66)
    n2 = CM.rxGaps()[0]
    logs = h.logs()
    if live:
        check("[live] seq 64 stays owed after the feed", n2 >= 1 and "still owed (live catch-up)" in logs, f"gaps={n2}")
        # the NACK scan fires once the grace has passed
        h.CM.ticks = 1000 + 16
        CM.nackScan()
        sent = lua_list(h.sent())
        check("[live] seq 64 is NACKed after the grace", any("LSNACK o=a seq=64" in s for s in sent), str(sent[-3:]))
    else:
        check("[save] seq 64 is written off between two runs (restarted origin)", "not owed" in logs and not any("seq=64" in s for s in lua_list(h.sent())), logs[-200:])
        h.CM.ticks = 1000 + 16
        CM.nackScan()
        check("[save] no NACK for a not-owed seq", not any("LSNACK o=a seq=64" in s for s in lua_list(h.sent())))
    # the hold release line tells the truth
    CM.gapHold = L.table(o="a", seq=64, since=900)
    L.globals().H2 = h
    h.CM.ticks = 1100
    L.execute('local CM = H2.CM; CM.gapHoldNeed = function() return nil end; CM.gapHoldTick(50.0)')
    logs = h.logs()
    if live:
        # in the live case the seq is still a gap so the hold would not release; force it to check the wording on a seen seq
        CM.rxNote("a", 64)
        CM.gapHold = L.table(o="a", seq=64, since=900)
        L.execute('local CM = H2.CM; CM.gapHoldTick(50.0)')
        logs = h.logs()
        check("[live] hold release says arrived for a seq we hold", "seq=64 arrived" in logs, logs[-160:])
    else:
        check("[save] hold release says WRITTEN OFF for a seq we do not hold", "seq=64 WRITTEN OFF" in logs, logs[-200:])


gap_case(True)
gap_case(False)

# ---- the host: a live request is served FROM its stamp, a save's request AFTER ----
L, h = runtime(instance="a", leader=True)
CM = h.CM
h.CM.ticks = 2000
CM.histPush("LSCMD op=VBUY at=574.4000 origin=a seq=64 x=1", 574.4)
CM.histPush("LSCMD op=VBUY at=581.8000 origin=a seq=65 x=1", 581.8)
# the lines themselves go out over the feed's tick budget; the run announcement is synchronous
h.clearSent()
CM.histServe(574.4, "b", True)
runs = [s for s in lua_list(h.sent()) if "LSHIST for=b" in s]
check("a live request at 574.4 is served FROM 574.4: the run announced is seq 64..65", any("from=64 to=65" in s for s in runs), str(runs))
h.clearSent()
CM.histServe(574.4, "c", False)
runs = [s for s in lua_list(h.sent()) if "LSHIST for=c" in s]
check("a save's request at 574.4 is served AFTER it: the run announced is seq 65..65", any("from=65 to=65" in s for s in runs), str(runs))

# ---- wiring: pacing marks which kind of request it sends ----
PACING = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mod", "mp_lockstep_1", "res", "scripts", "mp", "pacing.lua")
src = open(PACING, encoding="utf-8", errors="replace").read()
check("pacing marks the live catch-up request (CM.histLive = true before LSNEED without save=1)", "CM.histLive = true" in src and src.index("CM.histLive = true") < src.index('LSNEED t=%.4f o=%s", now, K.INSTANCE'))
check("pacing marks the load-gate request (CM.histLive = false before LSNEED save=1)", "CM.histLive = false" in src and src.index("CM.histLive = false") < src.index('LSNEED t=%.4f o=%s save=1'))

print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
raise SystemExit(1 if fails else 0)
