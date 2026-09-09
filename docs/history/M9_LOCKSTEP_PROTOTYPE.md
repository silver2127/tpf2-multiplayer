# M9 RESULTS — Lockstep prototype
### Two instances executing the same command at the same simulated moment

**Verdict: PASS.** First working lockstep loop in this project.

```
[ls-a] SCHED ROAD seq=1 at=55248 (now=55244)      A stamps it, does NOT execute
[ls-b] RECV  ROAD seq=1 at=55248 from a           crosses the wire, same stamp
[ls-a] EXEC  ROAD seq=1 origin=a at=55248 success=true
[ls-b] EXEC  ROAD seq=1 origin=a at=55248 success=true
RESULT: PASS   (desyncs A=0 B=0)
```

## 1. Why this is lockstep and not the old replication

The state-diff design in `mp_bridge` also made a road appear on both sides. It
still drifted, because each instance applied a *different* thing at a
*different* moment: the originator built immediately and shipped a description
of the result, which the peer re-derived later. Two worlds, two timelines.

Here the originator does not execute early. It stamps the command for a future
game time, ships it, and queues it exactly as the peer does. Both then execute
at the same stamp — the same simulated moment, not merely "eventually".

## 2. Design

| concern | choice | why |
|---|---|---|
| Clock | `getGameTime().time` | Wall clock is useless (the processes are never in step, one may be paused) and a per-instance tick counter counts frames since load, so equal tick numbers mean different world states. Game time is part of the simulation, loaded from the same save, and M3 showed it advances identically. |
| Input delay | `EXEC_DELAY = 4` (~2 game days) | Must exceed worst-case delivery latency. The file relay is sub-second, so this is enormous margin at well under a minute of wall clock. |
| Ordering | sort by `(at, origin, seq)` | Two commands due at the same stamp must apply in the same sequence on every peer, or the worlds diverge despite "executing the same commands". |
| Barrier | pause when >10 units ahead | The sim cannot be blocked from Lua, but it can be paused. Only whoever is *ahead* pauses, so it cannot deadlock. Verified live: `BARRIER hold: 13 s ahead` → `BARRIER release: 4 s ahead`. |
| Desync detection | M3's two-lane Lehmer hash | A weaker hash collides and reports agreement between genuinely different worlds — the worst possible failure mode for a desync detector. |
| Placeholder entity ids | derived from `(origin, seq)` | `mptest` uses `-100000 - ticks`, which differs per instance. Both peers must compute identical ids for the same command. |

## 3. Units — a trap worth recording

`getGameTime().time` is **not seconds**. Comparing a live reading (`t=55234`)
against the M3 probe's day counter (`day=27617`) puts it at ~2 units per in-game
**day**. The first draft's `EXEC_DELAY = 30` therefore meant 15 game days —
roughly seven minutes per command — and desync checks would have been 100 days
apart, i.e. never during a test.

## 4. Bugs found building it

- **Inject offset primed to end-of-file.** `injectOffset = -1` means "skip
  history", which is right for peer traffic (replaying commands whose stamps
  have passed would be wrong) but wrong for the injection file: while the file
  did not exist the offset stayed `-1`, so the poll that finally opened it
  seeked straight past the line it was meant to read. Now primed to the file's
  actual size at startup. This is the **third** variant of this same bug in the
  project — `mpbridge` records the joiner priming to end-of-file and reporting
  `consumed=0` while holding every line.
- **Proposal construction written from memory.** Missing `comp.type` /
  `comp.typeIndex`, and `streetEdge` needs constructing before assignment.
  Corrected against `mptest`'s proven path.

## 4b. Second run — three command types, and an id result

```
A: SCHED ROAD seq=1 / RAIL seq=2 / CON seq=3    all at=55248
B: RECV  ROAD seq=1 / RAIL seq=2 / CON seq=3    all at=55248
both: EXEC all three at 55248, success=true
both: SYNC t=55244 hash=2114130696-1613317288
executed A=3 B=3 of 3 | desyncs 0 | RESULT: PASS
```

**Entity ids match across peers.** The construction came back as `id=281697` on
*both* instances. `execDemolish` was deliberately written to target by position
rather than id, on the grounds that cross-peer id equality was an unverified
assumption; this is direct evidence it holds under an identical command history.
Future work can reference entities by id, which is far cheaper and less
ambiguous than nearest-by-position. Worth re-checking once commands originate
from both sides rather than only from A.

**The desync detector became bidirectional.** It previously compared only at the
moment a side computed its own hash, so whichever instance ran slightly ahead
always looked before the peer's hash arrived and never revisited the stamp --
measured A=0, B=1, i.e. "0 desyncs" mostly meant "0 comparisons". Comparing on
arrival as well fixed it: A=1, B=1 with identical hashes.

## 5. What this does NOT do

1. **No UI capture.** Commands enter through a file. Cancelling a command the
   player issues by clicking requires the native deferral hook, which is blocked
   on `applyProposal`'s signature — Ghidra recovered none
   (`undefined FUN_1409e76e0(void)`), and the known-good `buyVehicle_factory`
   control came back equally unsigned, so that is the baseline analysis quality
   rather than a problem with that function.
2. **One command type.** `ROAD` only. Per `M2_RESULTS.md` §5a the full set needs
   five taps: `applyProposal` (build/demolish/track/terraform), plus separate
   factories for buy vehicle (already hooked), create line, line stops, sell
   vehicle.
3. **One machine.** Cross-machine floating point is untested.
4. **Thin desync evidence.** The passing run compared a single hash checkpoint.

## 6. Risk profile change

Under state-diff replication a missing channel meant a feature did not sync —
annoying but contained. Under lockstep **any uncaptured local action is an
immediate, permanent desync**. That is worse in principle, but paired with two
things that make it manageable: the detector fires within one checkpoint instead
of corruption festering for hours, and the surface is bounded at five taps
rather than an open-ended list of semantic channels each needing echo guards and
TTLs.

## 7. Reproducing

```
# both instances: enable "MP Lockstep", disable "MP Bridge" and "M3 determinism probe"
powershell -File tools/autotest.ps1 -LaunchOnly -KeepOpen
powershell -File tools/lockstep_test.ps1
```

Mod: `mod/mp_lockstep_1/`. Test: `tools/lockstep_test.ps1`.
