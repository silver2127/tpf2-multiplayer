"""Offline check (Lua 5.2, real net.lua): a command that arrives more than once is queued once.

19:30, 2026-09-16: a's eight buses. Every command now goes out three times (scheduleLocal),
and the apply loop's pre-pass hands each QUEUED buy a step target chained one after the
previous queued buy -- copies included. b, holding three copies of each, chained 803, 804,
805, 806... across them and created its buses on steps 806, 808, 811...; a, which never
receives its own copies, used 804, 805, 806...: the towns split within a minute. So net.lua
queues one copy per (at, origin, seq); the apply loop frees the key when it executes, and a
retry re-registers it so a copy arriving mid-retry is not queued beside it.

    python tools/dup_arrival_test.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_src = open(os.path.join(HERE, "delay_hold_test.py"), encoding="utf-8").read()
_ns = {"__file__": os.path.join(HERE, "delay_hold_test.py"), "__name__": "delay_hold_harness"}
exec(compile(_src[:_src.index("# ---- round trip smoothing ----")], "delay_hold_test.py", "exec"), _ns)
runtime, check, fails = _ns["runtime"], _ns["check"], _ns["fails"]


def lua_list(t):
    return [t[i] for i in range(1, len(t) + 1)]


L, h = runtime()
CM = h.CM
# the stub lacks what the LSCMD branch touches beyond the queue
L.execute("local CM = ...; CM.cmReadConfig = function() CM.cmOriginCompany = {} end; CM.histPush = function() end; CM.lateCount = 0; CM.recovered = 0", CM)
h.setNow(50.0)
line = "LSCMD op=VBUY at=51.2000 origin=b seq=7 x=1"
h.feed(line)
h.feed(line)
h.feed(line)
q = lua_list(CM.queue)
check("three arrivals of one command queue it once", len(q) == 1 and q[0].seq == 7, [(c.op, c.seq) for c in q])
check("the drops are counted", CM.dupDropped == 2, CM.dupDropped)
check("the key is registered while queued", CM.queuedKeys["51.2|b|7"] is True)
# a different command still queues
h.feed("LSCMD op=VBUY at=51.4000 origin=b seq=8 x=1")
check("a different seq queues beside it", len(lua_list(CM.queue)) == 2)
# the apply loop frees the key when it takes the command: modelled here by what lockstep.lua does
CM.queuedKeys["51.2|b|7"] = None
CM.queue = L.table()
h.feed(line)
check("once executed and freed, a late resend queues again (the apply loop's executed[] then drops it)", len(lua_list(CM.queue)) == 1)

# ---- wiring in lockstep.lua ----
LS = os.path.join(os.path.dirname(HERE), "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
src = open(LS, encoding="utf-8", errors="replace").read()
i_exec = src.index("if not executed[k] then")
check("lockstep.lua frees the queued key where it marks a command executed",
      "CM.queuedKeys[k] = nil" in src[i_exec - 400:i_exec])
i_retry = src.index("for _, rc in ipairs(CM.retryQueue) do")
check("lockstep.lua re-registers the key of a retried command",
      "CM.queuedKeys[CM.cmdKey(rc)] = true" in src[i_retry:i_retry + 400])

print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
