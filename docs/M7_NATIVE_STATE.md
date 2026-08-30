# M7 — reading game state natively instead of through Lua

*2026-08-06. Recon only — nothing implemented.*

## Question

Can the bridge read game state directly from the exe rather than going through
the Lua scripting API?

## Answer: yes, and the entry point is a single function

`0x140fa6850` (4836 bytes) is the sol2 registration function for the
`api.engine` surface. It references **every** `ComponentType` name *and* the
bound accessors, so disassembling it yields both halves of what native reads
need:

- the `ComponentType` name → integer id mapping (sol2 enum registration pairs)
- the native functions behind `getComponent`, `entityExists`, `getRevision`,
  `forEachEntity`, `forEachEntityWithComponent`

Supporting symbol: `ecs::ComponentManager::GetComponentTypeIndex(const std::type_index&)`
— the component registry itself.

Found with `tools/find_sym.py forEachEntityWithComponent getComponent`.

## Why bother — the native surface is strictly larger

The `ComponentType` enum has ~70 entries. Ones the Lua API does not usefully
expose:

| Component | What it gives |
|---|---|
| `SIM_PERSON` | individual citizens |
| `SIM_PERSON_AT_TERMINAL`, `SIM_PERSON_AT_VEHICLE` | where each person currently is |
| `SIM_ENTITY_MOVING`, `SIM_ENTITY_IDLE`, `SIM_ENTITY_AT_BUILDING` | movement state |
| `MOVE_PATH`, `MOVE_PATH_AIRCRAFT` | the actual path a mover is following |
| `RAIL_VEHICLE`, `ROAD_VEHICLE`, `TRANSPORT_VEHICLE` | full vehicle state |
| `SIM_CARGO`, `SIM_CARGO_AT_TERMINAL` | cargo units |
| `PLAYER`, `ACCOUNT`, `PLAYER_OWNED` | the ownership layer on the roadmap |
| `GAME_TIME`, `GAME_SPEED` | clock, without the Lua round-trip |

This matters for M3 specifically: the determinism probe can only hash "what the
scripting API exposes", which is why it cannot see citizens at all. Native reads
would close that gap.

`forEachEntityWithComponent` is also a real iteration primitive — the Lua side
currently fakes it with `getEntities({radius = 999999})` full-world scans.

`getRevision` is worth a look on its own: an ECS revision counter would replace
the mod's poll-and-diff capture (`known[]` / `knownEdges[]` tables re-scanned
every 5 ticks) with proper change detection.

## The real catch is threading, not feasibility

REPORT.md §6 already flags the multithreaded ECS as a determinism risk. The same
fact bites here: **reading component memory from the bridge's own thread while
the sim mutates it is a data race.** Torn reads would not usually crash — they
would produce plausible-but-wrong values, which is the worst failure mode for
something whose whole job is detecting divergence.

The Lua path is safe precisely because `update()` runs on the game thread at a
defined point in the frame. Native reads need the same guarantee, so the
prerequisite is a **game-thread tick hook**, not the reader itself. M2 already
scouted candidates — `walk_up.py` probes "GameSim update caller (frame/tick
candidate)" at `0x14014f8b0` and the command-apply loop around `0x1409d34d3`.

Order of work, therefore:

1. Establish a per-tick game-thread callback.
2. Resolve `getComponent` / `forEachEntityWithComponent` at init.
3. Read from inside the tick callback only.

Doing 2 before 1 produces a racy reader that manufactures false desyncs.

## The tick hook — located

Found, and it resolves the threading problem.

| Symbol | RVA | Notes |
|---|---|---|
| **`GameSim::Step(__int64 frameTime, int)`** | **`0x15aa00`** | 119 b. **The per-tick hook target.** |
| `CGame::RunGameSimLoop()` | `0x1184d0` | 1164 b; calls Step at `0x1401185a0` |
| Apply-command (sim thread) | `0x9da290` | 694 b; opens with the literal `"Simulation Thread: Apply Command"` |

`GameSim::Step` was not findable from its own name string — MSVC outlined its
cold paths into separate 33–39 byte chunks around `0x14015ac20`, and those carry
the name. Identified instead from the sim loop, then confirmed by its own
assertion: `cmp rdx, 0x3e8 / jl <stub>` where the stub's message is
`frameTime >= 1000`. So `rcx` = `GameSim*`, `rdx` = frameTime in millis.

### Why this settles the data-race question

Three facts line up:

1. `"Simulation Thread: Apply Command"` — the sim genuinely runs on its own
   thread, so background reads from the bridge thread really would race.
2. `CGame::RunGameSimLoop` asserts on
   `m_data->gameStates[m_data->simIdx]` and `m_data->simIdx == 1 - oldSimIdx`
   — the game **double-buffers game state** and flips `simIdx` each step.
3. Hooking `GameSim::Step` puts our code *on the sim thread at a defined point
   in the frame*, which is the same guarantee Lua's `update()` enjoys.

So native reads should happen inside the Step detour. The double buffering is a
bonus worth investigating: it may allow reading the inactive buffer off-thread,
but that needs proving before relying on it.

### Hooking notes

The prologue is relocatable verbatim — no rip-relative operands in the first
21 bytes:

```
0x14015aa00  push rbx            ; 1
0x14015aa02  push r14            ; 2
0x14015aa04  sub  rsp, 0x68      ; 4
0x14015aa08  mov  rbx, rdx       ; 3
0x14015aa0b  mov  r14, rcx       ; 3
0x14015aa0e  cmp  rdx, 0x3e8     ; 7   -> steal ends 0x14015aa15
```

A 14-byte steal would land mid-instruction; steal **21 bytes** (through the
`cmp`, stopping before the rel32 `jl`). `hook.cpp` already supports
14/15/17/18/19/20/21-byte steals, and M2 validated the blob+relay stub at 25M
events, so no new hooking machinery is needed.

Because Step runs every tick, the detour must be cheap and must not log
unconditionally — M2's "56 GB of renderer noise" lesson applies directly.

## Threading model (and what it means for determinism)

Evidence-based revision of REPORT.md §6, which was written before disassembly.

### How it actually works

Three named long-lived threads: **`Simulation Thread`**, **`Render Thread`**,
**`Game Init Thread`**. Plus a general `ThreadPool` (`Lib/Util/ThreadPool.cpp`),
sized from `_Thrd_hardware_concurrency`.

| Symbol | RVA | Notes |
|---|---|---|
| `ThreadPool::ThreadPool(const std::string& name, int numThreads, bool)` | `0x2381ef0` | asserts `numThreads > 0`; **pools are named** |
| Apply-command | `0x9da290` | `"Simulation Thread: Apply Command"` |

The overwhelming majority of `ThreadPool` use is the renderer
(`CRenderer::Prepare`, `CRenderer::NewUpdate`) and `GlobalSettings::Save` —
all determinism-irrelevant.

The one that matters is **`ForEachEntityLoopParallel`**, which enqueues ECS
entity iteration onto the pool, accumulating a `std::array<int,5>` per chunk.
So entity updates *can* run across pool threads.

State is double-buffered: `gameStates[simIdx]`, `simIdx == 1 - oldSimIdx`.

### Corrections to earlier assumptions

- **`BaseParallelStripSystem` and `construction_util_parallel.cpp` are false
  friends.** "Parallel strips" means parallel *roads/tracks* — geometry, not
  execution. The ECS is not a general parallel system graph; there is exactly
  one parallel mechanism.
- **RNG concern (§6 #3) is resolved, and better than feared.** REPORT.md
  guessed "no shared sim RNG infrastructure is exposed; each system may carry
  its own state". Wrong: the sim uses
  `boost::random::mersenne_twister_engine` (mt19937) passed **explicitly by
  reference** through the call graph, e.g.
  `TownDeveloper::Develop(..., mt19937&, ...)`,
  `SimPersonAtTerminalSystem::GetRandomPlace(..., mt19937&)`,
  `SimEntityAtBuilding(..., mt19937&, ...)`,
  `CargoAtTerminalGoIdle(..., mt19937&, ...)`,
  `parcel_util::CreateBuilding(..., mt19937&, ...)`,
  `town_util::CalcBuildingCargoTypes(..., mt19937&)`.
  An explicit parameter is far easier to control than hidden global state.

### Ranked determinism risks, with evidence

1. **Parallel entity iteration drawing from a shared RNG.** The generator is
   passed by reference into per-entity work that `ForEachEntityLoopParallel`
   may be distributing across threads. If several threads draw from one
   engine, the sequence any given entity receives depends on scheduling —
   divergence, plus a data race on the generator itself. *Unconfirmed:* whether
   each chunk gets its own deterministically-seeded generator. **Check this
   first — it decides everything.**

2. **Hash-map iteration order in pathfinding.** `simulation_util::path_finder::FindPathLines`
   takes both `std::unordered_map<transport::EdgeId, ...>` and
   `phmap::flat_hash_map<transport::NodeId, ...>` — *and* an `mt19937&`.
   Iteration order over these depends on hash values and insertion history, so
   any difference in insertion order changes traversal order and can change the
   chosen path. Pathfinding decides where every vehicle and person goes, so
   this is worse than §6 #4 assumed: it is in the hottest decision path.

3. **Float** — the least of the four now. Same binary, same SSE2, no `/fp:fast`
   evidence. Cross-machine x86-64 FP is generally stable.

### The patch lever

`ThreadPool::ThreadPool` takes the pool **name** as its first argument. So
"force single-threaded", REPORT.md's proposed mitigation, does not have to be
blunt: hook the constructor and clamp `numThreads` to 1 **only for the sim
pool**, leaving the renderer's pools at full width so framerate survives.

Open: confirm which pool `ForEachEntityLoopParallel` submits to, and whether
per-chunk RNG seeding already makes it order-independent.

## Second catch: brittleness

Every hardcoded VA breaks on a game patch, and a native ECS reader multiplies
the project's existing exposure. Mitigation: resolve addresses **at runtime**
from string cross-references — exactly what `tools/find_sym.py` does offline —
rather than baking constants into the DLL.

## Prototype status (2026-08-06)

The tick hook is **implemented and tested offline**. Not yet run in the game.

- `bridge/src/simhook.{h,cpp}` — installs a 21-byte detour on `GameSim::Step`
- `bridge/src/simsteprelay.asm` — the relay. Simpler than probe3's blob+relay
  because the patch is a bare `jmp [rip+0]`, so the stack at entry is untouched.
  Preserves only `rax/rcx/rdx/r8/r9`: everything else volatile is already
  Step's to clobber, and this runs every tick.
- Enabled with `sim_hook=1` in the bridge cfg. **Off by default in code** —
  it patches game memory, so it should be opted into.

### Safety

Three guards, all exercised:

1. **Prologue verification.** The exact 21 bytes
   (`40 53 41 56 48 83 EC 68 48 8B DA 4C 8B F1 48 81 FA E8 03 00 00`) are
   compared before patching. A game update shifts the function, the bytes stop
   matching, and we refuse instead of writing a jump into whatever now lives
   there.
2. **Host check.** Skips unless the containing process really is
   `TransportFever2.exe`.
3. **Bounds + readability check** on the target range.

Guards 2 and 3 exist because the first version had neither: loading the bridge
into a test harness put `base + 0x15aa00` outside the image, and the `memcmp`
faulted and killed its own thread with no log line. Caught by the offline smoke
test, not in the game.

### Offline test

`scratchpad/fakestep.asm` provides a stand-in carrying the *byte-identical*
prologue and a body returning `rcx + rdx`, so a corrupted argument register
shows up as a wrong answer. Results:

- unhooked baseline correct
- mismatched prologue refused, target left intact
- hooked function still returns the right value
- handler fires exactly once per call, sees both arguments
- **20,000 calls with varied arguments, all correct**; tick count exact

That covers the steal size, the trampoline, and register preservation — the
three things that would otherwise have been discovered by crashing the game.

### Reporting

The handler does nothing but store three values: no allocation, no locks, no
I/O. A separate thread in the bridge logs the tick count every 10 s. M2's
always-on hook wrote ~56 GB of noise in one session; a per-tick hook is exactly
where that repeats.

### Next

Resolve `getComponent` / `forEachEntityWithComponent` from the sol2 binding at
`0x140fa6850` and call them from inside the handler. Everything above exists to
make that step safe.

## Scope note

This is about reads. Writes should still go through the command system:
commands are the replication unit, they are already captured and replayed, and
poking component memory directly would bypass the engine's own invariants.
