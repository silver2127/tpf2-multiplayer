# M3 RESULTS — Determinism Gate
### The experiment REPORT.md §7 calls "the make-or-break", finally run

**Verdict: no divergence observed. Lockstep is not ruled out.**

`REPORT.md` §7 rates M3 as the gate that decides the whole architecture and
estimates 4–12 weeks for it. It was never run to a conclusion, and the project
proceeded on a *third* path — Lua-level state-diff replication — that is neither
the preferred design (§5.3 lockstep) nor the documented fallback
(server-authoritative state sync). This closes that gap.

---

## 1. Result

| | |
|---|---|
| Overlapping samples compared | **59** |
| Identical | **59** |
| In-game days covered | 27617 – 27675 (58 days) |
| Vehicles in the hash | **79** |
| Transport lines in the hash | 26, with per-cargo counters |
| Snapshot size | ~7,850 chars, varying every sample |
| `mp_bridge` activity during the run | **0 lines** on both instances |

Two instances loaded the same save, received no player input, and ran side by
side on one machine. Their state hashes matched at every in-game day they both
sampled.

## 2. Why this run counts and the previous one did not

An earlier run reported "42/42 identical" and was nearly written up as a pass.
It was measuring almost nothing:

```
nv=0|nl=26|l22426.IRON_ORE=13732|...      len=754, constant after day 4
```

`nv=0`. The probe called `api.engine.system.transportVehicleSystem.getVehicles()`
inside a `pcall`. That system exposes no callable functions, so the call threw,
the `pcall` swallowed it, and the vehicle list came back empty. The hash covered
cargo counters, towns and balance — and not one moving object, which is exactly
the subsystem most likely to diverge.

`mpbridge.lua` had already hit this identical bug and documented it at its
`pollVehicles` (vehicle capture never fired once, on a save holding 78). The
working call is `game.interface.getEntities({radius=...},{type="VEHICLE"})`.

The probe now uses it and **prints a warning if the count is ever zero**,
because a blind probe and a passing probe produce indistinguishable output:

```
M3 WARNING: nv=0 -- vehicle query returned nothing. The hash is blind to
vehicles; a match proves much less than it appears to.
```

After the fix: `nv=79`, and snapshot length varies every sample (7842, 7846,
7840, 7861 …) rather than sitting flat at 754 — the signature of genuinely
moving state being hashed.

## 3. Threats to validity, and what was done about each

| threat | handling |
|---|---|
| Replication mod perturbing the worlds — towns grow, `mp_bridge` would capture and replay those constructions, i.e. inject input | `mp_bridge_1` removed from `mods/` entirely; confirmed 0 `[mpb-*]` lines on both sides for the whole run |
| Float drift rounded away by formatting | `num()` uses `%.17g` — full double precision |
| Engine hash-map iteration order masquerading as a desync | `sortedCopy()` fixes entity visit order before hashing |
| Weak hash colliding and reporting a false match | two Lehmer lanes with products kept under 2^53, so the arithmetic is exact (see the probe header) |
| Sample misalignment producing a false divergence | comparison aligns on **in-game day**, not sample ordinal — B launches second and runs ~40 samples behind A in wall-clock while covering the same days |
| Paused sim producing zero samples that look like a clean run | probe unpauses itself via `setGameSpeed`; comparison treats zero samples as INCONCLUSIVE, never a pass |

## 4. What this does NOT establish

Necessary, not sufficient. Untested:

1. **Cross-machine.** Both instances ran on one CPU. Floating-point differences
   between machines (different SSE/AVX paths, different microarchitecture) are
   the classic lockstep killer and are untouched by this run.
2. **Under player input.** The interesting lockstep case is a command applied at
   the same tick on every peer. This run had no input at all.
3. **Longer horizons.** 58 in-game days. Divergence can take much longer to
   surface, and once it does it compounds.
4. **Load-dependent scheduling.** §6 ranks a multithreaded ECS as the top risk:
   parallel system execution diverging on thread-scheduling order. 79 vehicles
   is real parallel work and it held, which is meaningful evidence *against*
   that risk — but a busier map may schedule differently.

## 5. Consequence for the architecture

The gate does not block lockstep. §5.3's design becomes the live plan:

1. Tap commands at the choke point — `applyProposal`, RVA `0x9e76e0`, re-verified
   firing on build 35924 and confirmed to catch **script-issued** builds too, so
   one hook sees both origins.
2. Broadcast, buffer for tick N + input delay.
3. Inject on every peer through the same path.
4. Gate `GameSim::Step` (already hooked, RVA `0x15aa00`) until all peers' inputs
   for the tick have arrived.
5. Detect desync with this probe's hash; recover via `savexfer.cpp`.

Note this makes the current `mpbridge.lua` state-diff replication a dead end
rather than a foundation: lockstep ships inputs and relies on determinism, so
the channels, the twelve echo-guard tables and the ten TTL constants all become
unnecessary rather than improved.

## 6. Reproducing

```
# both instances, save loaded, no input; mp_bridge must NOT be installed
powershell -File tools/autotest.ps1 -LaunchOnly -KeepOpen
powershell -File tools/m3_compare.ps1
```

Probe: `mod/m3_determinism_1/`. Comparison: `tools/m3_compare.ps1`.
