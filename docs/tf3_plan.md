# Transport Fever 3 — multiplayer mod plan

Written 2026-09-03, before TF3 exists. Everything here is a design position taken
from what the TpF2 project learned, not from the TF3 binary. Revise hard once the
binary lands.

## Known facts

| | |
| --- | --- |
| Release | **2026-09-29** (26 days from writing) |
| Platforms | PC, Mac, Linux day one (Steam / Epic / GOG) + PS5, Xbox Series X\|S |
| Publisher | Paradox Interactive (TpF2 was self-published by Urban Games) |
| Official multiplayer | **Not announced.** UG's line: focused on single-player, "the timing isn't right" |

So the mod is still needed, and there is most of a month to prepare before the
binary exists.

Sources: [Paradox press release](https://www.paradoxinteractive.com/media/press-releases/press-release/transport-fever-3-gets-on-track-for-september-29-launch-on-pc-and-consoles-deluxe-and-collectors-editions-revealed-as-pre-orders-open),
[Will TF3 have Multiplayer? — Steam](https://steamcommunity.com/app/3493540/discussions/0/599662086417818869/),
[transportfever3.com — release date announced](https://www.transportfever3.com/news/rda/).

---

## The one structural change: invert the tooling/mod ratio

The most valuable thing in the TpF2 repo is not `lockstep.lua` — it is `tools/`
(`tpfdis.py`, `funcsig.py`, `find_hooks3.py`, `src_ranges.py`, the Ghidra
pipeline). The biggest liability is **77 hardcoded RVAs** behind a build guard
that goes inert on mismatch.

TpF2 got away with that because the game **froze at build 35924 in December
2024** — no updates in 2025 or 2026. TF3 will be patched actively for years,
across Steam/Epic/GOG builds that may not be byte-identical, on three desktop
platforms.

**Design rule: zero hardcoded addresses in the shipped DLL.** Build a signature
resolver, and build it first.

### Why this is tractable, not aspirational

`tools/funcsig.py` exists because TpF2 embeds `__FUNCSIG__` strings in the
binary — that is how `make_cmd::CreateLine`'s argument order was recovered
without decompiling it. If TF3 does the same (same engine lineage, same assert
macros — likely), functions can be resolved **by name at runtime** from the
embedded signature strings and their cross-references:

```cpp
Resolve("make_cmd::BuildProposal")   // not 0x9dc750
```

That is near-symbolic access to a stripped binary. Roughly 300 lines, and it is
the difference between a mod that survives patch day and one that needs a manual
Ghidra session every time.

**Check whether TF3 embeds those strings in the first hour you have the binary.**
It is the single highest-leverage fact about the whole project.

---

## Ordered plan

### 1. Measure determinism before building anything on it

Run the M3 experiment — two instances, same save, hash the world for N in-game
days — as the **first** thing, not the third. It decides the architecture:

- deterministic fixed-step sim → coop lockstep is on the table
- not → companies mode is the only option

Find that out in a day rather than after 9,000 lines.

The console port cuts both ways. Consoles push toward a fixed-step, tightly
budgeted sim, which favours us. But an engine rework may have added threaded
pathfinding with work-stealing, which is nondeterministic by construction and
kills coop lockstep outright. This cannot be reasoned out — measure it.

### 2. Pick companies mode

Even if determinism holds. Everything downstream gets simpler: no barrier, no
pacer, no speed sharing, no hash-as-gate, and cross-platform play becomes
conceivable instead of impossible.

TpF2 has AI competitors, so the engine already models multiple players — that is
why `game.interface.addPlayer` works at all. TF3 will too. Let the engine do the
work.

Coop lockstep is the more impressive engineering result. Companies mode is the
one people will actually play.

### 3. One hook at `CommandList::Add`, not fourteen factories

Everything the player does funnels through Add. TpF2 hooks 14 factories **plus**
Add only because the Command objects are not decoded — it needs the factory
arguments. Decode the Command types instead and you hook Add alone, dispatching
on the vftable pointer to identify the concrete type.

This collapses `slice_hook.cpp`'s 2,400 lines to a dispatcher plus per-type
serializers, and it changes the failure mode from silent to loud: an unknown
vftable is a command type not yet reversed, so refuse to cancel it and log it.

Compare TpF2, where the street-upgrade tool landed in the "not the road path —
ignored" branch and silently did not replicate until someone noticed.

### 4. Serialize the Command; do not invent a schema

TpF2's wire is `ROADE x0 y0 z0 x1 y1 z1 stype=16 ...` — hand-built and lossy. It
grew a field every time a bug taught us something:

- tangents (curves replicated as polygons of their control points)
- `btype` / `bidx` (bridges came out as embankments)
- catenary

Each was a shipped bug first. Serializing the object is the same RE work, but the
wire is complete **by construction**. A new command type is one serializer, not a
new opcode plus a Lua exec path plus a settle path.

### 5. Capture and replay through the same path

The deepest lesson in the TpF2 codebase. The CONX z-desync exists because capture
is native (`StreetBuilder::UpdateEngine`) and replay goes through Lua's
`api.cmd.make.buildProposal`. Two paths, two proposals, 0.1 m apart.
`K.CONX_UI_CONTEXT` and the `conx_terrain_align` experiment are both attempts to
make path B imitate path A.

For TF3, replay natively from the start: reconstruct the Command and hand it to
Add, exactly as the UI did. That bug class never exists.

### 6. The clock is the sim step, and nothing else

Hook the step function on day one (`simhook.cpp` is the template). Stamps are step
integers.

This deletes `K.EXEC_DELAY` in fractional game-time units, `stepOf()`,
`notBeforeStep`, `K.BIND_GUARD_STEPS`, the applylag metric, and the whole "first
`update()` past the stamp is a different sim step on each instance" problem — all
of which are compensation for scheduling from a per-frame Lua callback.

---

## What carries over free

Genuinely a lot, and it changes the effort estimate:

- **All of `netpunch/`** — punch, lobby, mesh, swarm, seal, save transfer.
  Game-agnostic, zero changes.
- **`net.cpp`** — reliable-ordered UDP.
- **The geometry math** — hermite, split-at-u, `findNodeNear`,
  `findEdgeContaining`, and the tuned tolerances (`SPLIT_EPS = 5.0` because rail
  sits ~4.5 m off a road's centreline). Pure math, ports as C++.
- **The hash design** — "hash by geometry and content, never by id" is a general
  lesson, not a TpF2 fact.
- **The two-instance rig** — autotest, snapshot-logs discipline, the
  HOST/JOIN letter-election requirement.

## What to drop

- The Lua mod, if native replay works.
- The file IPC (capture/events/inject/status/dash files and their byte offsets).
- The `mp_bridge` state-diff lineage.
- Hardcoded RVAs.

---

## Pre-launch checklist (now → 2026-09-29)

Nothing here needs the binary:

- [ ] Port `netpunch/` verbatim; confirm the lobby, seal and save transfer still
      pass selftest standalone.
- [ ] Extract the geometry math from `lockstep.lua` into a standalone C++ library
      with unit tests. It is pure math and currently only testable in-game.
- [ ] Write the determinism harness against an abstract "enumerate entities"
      interface so only the back end changes on day one.
- [ ] Decide the platform policy **up front** this time (native Linux vs Proton
      vs both) rather than retrofitting it — see `audit.md` and the TpF2 Linux
      findings.
- [ ] Read Paradox's modding documentation the day it appears (below).

## Day-one checklist (2026-09-29)

In order, before writing any mod code:

1. Does the binary embed `__FUNCSIG__` / assert strings? → decides pattern
   resolution vs manual RE.
2. Do the Steam / Epic / GOG builds differ byte-for-byte? → another argument for
   pattern resolution, cheap to check once.
3. Is the sim single-threaded and fixed-step? → run the determinism harness.
4. Does `CommandList::Add` (or its equivalent) still exist as a single funnel?
5. Is there an official mod/scripting API richer than TpF2's Lua?

---

## Two things to watch

**Whether Paradox ships a richer official mod API.** Paradox is mod-friendly, and
a console port often forces a cleaner sim/render split — exactly what a lockstep
mod wants. If TF3 exposes anything like a command-stream or replay API, the entire
capture half of this project evaporates. Read the modding docs before committing
to a hooking design.

**Whether the engine changed enough that "similar architecture" fails.** The
pattern resolver and the determinism harness both survive that. The accumulated
RE knowledge does not.

---

## Honest summary

The transport, lobby, security and harness half of this project ports for free.
The RE half is a rebuild, not a port — but done pattern-first against a
live-service game, it should be a **durable** rebuild rather than one that needs
re-doing every patch.
