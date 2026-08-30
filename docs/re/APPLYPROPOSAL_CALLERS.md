# applyProposal callers — separating player actions from engine churn

Measured live on build 35924 with `bridge/src/probe_apply.cpp`. Image base
`0x140000000`; all values are RVAs of the *return address*, i.e. the instruction
after the call.

## The problem this solves

`applyProposal` (`0x9e76e0`) looked like a clean per-action choke point. It is
not: a 139-second window with a single player build logged **89 commits**,
spread evenly throughout with 1.6–16 s gaps. Towns build houses on their own and
that is a construction proposal like any other, so the engine calls this
function constantly. Suppressing it wholesale — the naive way to defer a
player's command — would cancel the engine's own construction work.

## Method

Two controls, same measurement:

1. **Script control** — 3 commands injected through the lockstep mod
   (`api.cmd.buildProposal`), 2 succeeded, 1 refused by terrain.
2. **UI control** — 3 roads built by hand in the same instance.

Caller identified by reading the return address off the relay stack at
`rsp+0xD8` (`applyrelay_probe.asm`). `RtlCaptureStackBackTrace` cannot do this:
x64 unwinding is table-driven and the relay is `VirtualAlloc`'d with no
`RUNTIME_FUNCTION` data, so it returns zero frames from inside a detour.

## Result

| caller (ret addr) | call site | function | script Δ | UI Δ | role |
|---|---|---|---|---|---|
| `9d6f93` | `9d6f8e` | `FUN_1409d6e20` | +2 | +3 | **once per SUCCESSFUL apply, both paths** |
| `21ed6df` | `21ed6da` | `FUN_1421ed420` | +2 | +3 | **once per SUCCESSFUL apply, both paths** |
| `985072` | `98506d` | `FUN_140984e30` | +3 | **0** | **script-only** (per attempt) |
| `962b23` | `962b1e` | `FUN_140962940` | +3 | +1 | mostly script |
| `a75497` | `a75492` | `FUN_140a74070` | 0 | +1 | ~~UI-only~~ — see correction below |
| `a7559b` | `a75596` | `FUN_140a74070` | 0 | +1 | ~~UI-only~~ — see correction below |
| `b6e06d` | `b6e068` | `FUN_140b6e040` | +10 | +10 | background (town growth) |
| `95d06f` | `95d06a` | `FUN_14095c810` | +10 | +15 | background (town growth) |

Script Δ counts 3 attempts / 2 successes; UI Δ counts 3 builds.

## Why this makes the lockstep hook viable

1. **Per-action detection exists.** `9d6f93` / `21ed6df` fire exactly once per
   successful apply. The action is separable from the background churn.
2. **The relay can discriminate at runtime.** It already reads the caller's
   return address, so it can suppress only calls arriving from a chosen site and
   let engine construction through untouched. No wholesale suppression.
3. **Origin is free.** `985072` fires for scripted commands and never for UI
   ones. Distinguishing "the player did this" from "our own replay did this" is
   a caller check — not the twelve signature tables and ten TTL constants the
   state-diff design needed for the same job.
4. **Suppression is mechanically cheap at the busy sites.** `b6e068`, `95d06a`,
   `21ed6da` all overwrite `rax` immediately after the call, so their return
   value is discarded and skipping them needs no fabricated result. Only
   `9d6f8e` consumes `rax` (it compares against a `+0xb00` end pointer, an
   iterator-style test), so a hook there would need care.

## Correction: what the DECOMPILED callers say

The table above assigns roles from runtime frequency alone. Decompiling the
calling functions afterwards corroborated some and refuted one — frequency
cannot distinguish causation from coincidence, and one label was coincidence.

| function | markers recovered in its code | verdict |
|---|---|---|
| `b6e040` | `BuildingTypeRep::Construct`, `ProposalData`, `street_util::StreetToolkit` | confirmed: town buildings |
| `95c810` | `street_util::StreetToolkit` | consistent: town roads |
| `21ed420` | `IHeightmap`, `terrain::HighResHeightMap`, `terrain_alignment_util::ExtendedTerrainModHeightMap` | it is TERRAIN MODIFICATION -- a consequence of building, not the build command |
| `a74070` | **`SimBuildingSystem::Update2`**, `component::SimBuilding`, `component::ParticleSystem`, `component::LogBook` | **NOT UI.** A per-tick simulation system. It scored "UI-only" because it happened to fire during the UI window and not the shorter script window |
| `962940` | `component::BaseEdgeStreet`, `component::Parcel` | street/parcel handling |
| `9d6e20`, `984e30` | none recovered | unidentified |

**Consequence: there is still no confirmed UI-specific caller.** The per-success
sites are shared between the script and UI paths, and at least one of them is a
downstream effect rather than the command. Identifying the player's build
command at this function is not yet solved.

Method lesson: runtime counts say WHICH code ran and how often; decompilation
says WHAT it is. Neither alone was sufficient here, and using only the first
produced a confident wrong answer.

## Caveats

- `FUN_140a74070` has three call sites (`a752de`, `a75492`, `a75596`) and only
  two fired, once each, for three builds. So it is UI-associated but not
  per-build — possibly per tool activation. Not yet characterised.
- One sample set per control. The correlations are clean (+2/+2 for two
  successes, +3/+3 for three) but a second run would harden them.
- Every measurement is road building. Rail, stations, demolish and terraform may
  route differently; M2 §5a already shows vehicles and lines use entirely
  separate factories.
