# M2 RESULTS — Live-Instrumentation Findings
### Dynamic analysis milestone: empirical map of TpF2's player-action paths

Method: 5 probe DLLs injected into a live game (pid 29056) via our own
LoadLibrary injector; 14-byte absolute-jump detours with generated
blob+relay stubs (no external hook library). ~25M captured events, zero
crashes, zero game instability across the whole session.
Probes chain cleanly (each new DLL detours over the previous patch).

---

## 1. THE headline finding: M1's "one choke point" was wrong — there are three

| Player action | Path taken | Evidence |
|---|---|---|
| **Construction** (road, depot, station, track) | `applyProposal` — `0x9e76e0` (24KB main) + `0x9ee4f0` (secondary), same proposal ptr passed `rdx→rcx` | 102 paired hits per depot build; zero CommandList activity |
| **Vehicle purchase** | Command system factory `0x9dca00` (make_command.cpp) | exactly 1 hit per bus purchased |
| **Script actions** (`api.cmd.*`, incl. the hotseat mod) | sol2 trampoline `0x83110` → CommandList | 25M events (mostly renderer/UI script queries: `renderables`, `marker`) |

`CommandList`/`apply_command`/`make_command` is the **script-facing** command
API. The game's own UI bypasses it for construction and goes straight to the
proposal system (`Game\construction\apply_proposal.cpp`). The 11
apply_command + 19 make_command hooks stayed silent through a full build
spree — empirically disproven as the UI tap.

## 2. What this means for the MP design (good news, actually)

The replication layer needs **two taps**, not one:

1. **Proposal tap** (`0x9e76e0`): captures every construction action as a
   self-contained proposal object. Better replication unit than raw commands
   — a proposal IS "build X at Y" in one object (the engine exposes the same
   abstraction to Lua as `SimpleProposal`).
2. **Command tap** (the 19 factories, only some of which the UI uses):
   captures vehicle purchases and other non-construction actions. Each
   factory = one command type = free type identification.

Lockstep is still the architecture; the unit of replication just changed.

## 3. Proven machinery (reusable for all future milestones)

- `injector.exe` — reliable LoadLibrary injection (5/5 successful).
- `hook.cpp` — 14/15/17/18/19/20/21-byte steals, trampoline allocation.
- blob+relay stub (`applyrelay.asm` + generated per-target blobs) —
  validated at 25M events; preserves full register + xmm0-5 state.
- Chain-injection works: 4 DLLs stacked on the trampoline target without
  instability.
- Log files opened with `FILE_SHARE_READ` are live-readable from outside.

## 4. Captured data (starting point for struct mapping)

- `applyProposal`: proposal object at `rdx` (id=1) / `rcx` (id=3).
  Heap region `0x225f…` / `0x2260…`; dumps show vector triplets
  (begin/end/cap) inside.
- Vehicle factory `0x9dca00`: params at `rdx`; contains two vector triplets
  + a pointer near the engine heap.
- Both hooks fire with **repeated identical args across adjacent ticks** —
  consistent with preview/validation passes before the real commit.

## 5. Open items (next milestone inputs)

1. ~~Preview vs. commit discriminator~~ — **RESOLVED (probe6, live test)**:
   `applyProposal` (0x9e76e0) fires ONLY on commit. Depot build = exactly
   1 call; drag-ghost + ESC = 0 calls. The ~102 calls in the first session
   were multiple separate constructions misattributed as one. No
   discriminator needed — every call is a build event. r8/r9 captured for
   the record: r8 = stack-ish context ptr, r9 = heap ptr; proposal object
   at rdx (vector triplets + float/int header fields, 256B window logged).
2. ~~Action-path map completion~~ — **COMPLETE**, see §5a.

## 5b. M4 recon: where do build PARAMETERS live? (probes 6-7)

Goal: find the canonical "build intent" packet for reconstruction-style
replication (send params, re-run make_proposal remotely).

- probe7 hooked all 7 `ConstructionBuilder.cpp` UI functions. Findings:
  - `0x415e60` (id=1), `0x417410` (id=2), `0x41a9d0` (id=4): per-frame
    mouse/update handlers — spam while the construction tool is active.
  - `0x41c3e0` (id=5): **the click/commit UI handler** — fires only on
    click (2× = press/release, identical args).
  - Diffing the id=5 param struct (`rdx`, 56B) across builds of the same
    depot at different locations AND across depot-vs-road: **byte-identical**
    → rdx is a UI event context, NOT build params. Values seen (1000, 391,
    254) are likely UI/sound ids. The `1000` overlap with the proposal dump
    is probably coincidental (could be a price).
  - The builder object itself (`rcx`) DOES change per build → build state
    (position etc.) lives in builder member state, set by the per-frame
    handlers.
- Conclusion: UI layer is the wrong boundary for param capture. The clean
  boundary is `make_proposal` (`Game\construction\make_proposal.cpp`, 17
  functions mapped in recon) — where resolved params become a proposal,
  right before `applyProposal`. NEXT PROBE: hook the non-rip-rel
  make_proposal functions, build varied constructions, diff arguments.

## 5c. M4 capture design — SOLVED (probe8, live test)

probe8 hooked 11 non-rip-rel make_proposal.cpp functions during one depot
build (732 events: 146 preview frames × 5 functions + 2 misc):

- `0xa152e0` (id=4): rcx = **placement transform** — identity rotation
  matrix (1.0f diagonal floats) + position vector. Pure geometry.
- `0xa182a0` (id=8): rdx = **construction params block** — small ints
  (1000, 391, 254, 26, 31, 7 observed); type/variant identifiers, same
  values as the UI click handler's context.
- Per-frame chain: transform → helpers → params → assembly; previews
  recompute every frame (~146/build), `applyProposal` fires once at commit.

**Replication protocol (M4):** on each frame, snapshot the latest
params+transform; when `applyProposal` fires, ship the most recent pair to
peers; remote side re-runs make_proposal→applyProposal at the aligned tick.
No struct cloning, no pointer shipping, no preview noise.
3. **Proposal struct layout** — static: RTTI for `SimpleProposal` /
   `Proposal` classes; dynamic: dump bigger windows around the proposal ptr.
4. **Tick alignment** — command/proposal application cadence vs. GameSim
   tick (needed for lockstep scheduling).
5. **Determinism** — untouched; still the project's main risk (REPORT.md §6).

## 5a. Final action-path map (live-tested, all 8 action types)

| Action | Tap | Notes |
|---|---|---|
| Build construction (road/depot/station) | `applyProposal` `0x9e76e0` + `0x9ee4f0` | ~102 paired calls/build (previews+segments) |
| Demolish | same | +3 calls; simpler, no segments |
| Lay rail track | same | +1 call per segment |
| Terraform | same | +1 call |
| Buy vehicle | factory `0x9dca00` | exactly 1 call per purchase |
| Create line | factory `0x9dcde0` | payload contains line name in cleartext ("Line 2") |
| Add/edit line stops | factory `0x9df4e0` | 1 call per stop operation |
| Sell vehicle | ~~factory `0x9de8a0`~~ → **CORRECTED: `0x9de380`** (see note below) | first observed full create→apply cycle |

Param notes: the engine recycles a per-operation context struct
(same scratch address seen across unrelated actions) — key on content,
not addresses.

Log hygiene lesson: the always-on trampoline validation hook wrote ~56 GB
of renderer noise across the session. Future probes hook only meaningful
targets; logs must be sampled or rate-limited.

## 6. Session inventory

| Probe | Targets | Result |
|---|---|---|
| probe1 | executor `0x9d2420`, trampoline | executor silent; log not share-readable (superseded) |
| probe2 | same + share-mode log | trampoline fires ~continuously (renderer script queries) |
| probe3 | 11 apply_command funcs | silent during real builds → ~~CommandList disproven for UI~~ **WRONG, see note below** |
| probe4 | + 19 factories + trampoline validation | vehicle buy → factory `0x9dca00`; relay validated |
| probe5 | 4 apply_proposal funcs + trampoline | **construction found**: `0x9e76e0` + `0x9ee4f0` |

Build scripts `build.bat` … `build5.bat`; sources in `src/`; all RVAs are
relative to the module base (ASLR-safe).

---

## CORRECTIONS (added after the ACTION_MAP work, verified live)

Two claims above are wrong. Both are struck through in place; the detail is here.

### 1. "CommandList disproven for UI" — WRONG

Section 1 and the probe3 row conclude that `CommandList` is script-facing only,
because 11 `apply_command` and 19 `make_command` hooks stayed silent through a
full build spree. The conclusion does not follow: the set that was probed did not
include the function that actually matters.

**`CommandList::Add` at `0x9d2a00` is the universal tap for player actions.** It
has ~81 direct call sites which are, bar two engine-internal ones, exactly the
player-action list — and `api.cmd.sendCommand` converges on the same function, so
UI and script commands are the same objects in the same queue.

Measured live on build 35924 by hooking it directly:

| | calls |
|---|---|
| 45 s idle (no player action) | 4,583 — **all** from `caller_rva=1126f1a`, the Lua bridge |
| one player road build | **1**, from `caller_rva=459eb7` (inside `StreetBuilder::UpdateEngine`) |

So the function is busy, but the busy-ness is monolithic and comes from one call
site. Player actions are separable by caller RVA alone.

The probe3 result was real; the inference from it was not. A silent hook proves
the hooked function was not called, never that the family is uninvolved.

### 2. "Sell vehicle → `0x9de8a0`" — WRONG

`0x9de8a0` is `make_cmd::SetColor` (recolour a vehicle). Sell is **`0x9de380`**.

### Consequence

`applyProposal` (`0x9e76e0`) is **not** on the command path. It is one step
inside the handler for a single command type, and most of its callers are town
growth, industry simulation, savegame loading and scripting — which is why a
139-second window with one player build logged 89 commits there.

The correct interception point for lockstep is `CommandList::Add`, filtered by
caller: suppress commands arriving from a UI call site, let commands arriving
from the Lua bridge (`1126f1a`) through, since those are our own replays. Echo
suppression becomes a caller check rather than the twelve signature tables and
ten TTL constants the state-diff design needed.

See `docs/re/ACTION_MAP.md` for the full per-action map.
