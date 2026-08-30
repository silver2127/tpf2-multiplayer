# M8 — applyProposal probe: is the intent tap viable on build 35924?

Probe source `bridge/src/probe_apply.cpp`, build `bridge/build_probe_apply.bat`,
scenario `tools/scenarios/probe_applyproposal.txt`. Standalone DLL, injected
with `injector.exe`; links none of the live bridge's networking, so it can only
observe.

## Why this exists

The shipped replication captures **state**: poll the world, diff a cached
snapshot, infer what the player must have done. Every hard bug in the project is
that one decision — intent is discarded at capture and reconstructed by
heuristic. The current defence is **12 shadow-state tables and 10 tuning
constants** across 2,512 lines of `mpbridge.lua` (`remoteDemolished`,
`remoteConMod`, `conRewrite`, `conEdgeGrace`, `remoteEdgeSigs`,
`remoteEdgeDelSigs`, `EDGE_SIG_TTL`, `REPLAY_QUIET_TICKS`, …). Each exists to
recover a fact that was free at the instant the player clicked.

`REPORT.md` §5.3 already specified the alternative — tap where intent exists,
broadcast, inject through the same path with a remote flag. `M2_RESULTS.md`
found the point: **`applyProposal`, RVA `0x9e76e0`**. This probe checks whether
that still holds, and what it costs to use.

## Results

### 1. The choke point is intact on 35924 — CONFIRMED

```
[probe] hooked make_proposal.transform  rva=a16d00
[probe] hooked make_proposal.params     rva=a18ca0
[probe] hooked applyProposal.COMMIT     rva=9e76e0
[probe] 3/3 hooks installed
```

All three prologues still match; no re-RE needed to install.

### 2. Script-issued builds reach the same hook — CONFIRMED, and this is the important one

Every action in the run came from Lua (`game.interface` / `api.cmd`) — the exact
path our own replay uses — and `applyProposal` fired for them. One tap therefore
sees both UI and script origins, which means **echo suppression collapses to a
single "this apply is remote" flag** instead of the signature tables above.

Caveat: the two `make_proposal` hooks barely fired (`transform=1 params=2` over
the whole run). They are UI-preview functions, so a script build skips them —
the transform/params context they provide is NOT available for script-origin
commits. Consistent with M2 calling them UI-path.

### 3. The proposal identifies what was built — CONFIRMED by correlation

A single-level pointer scan found **zero** resource paths: the proposal is a
graph of nested `std::vector`s, and a `.con`/`.module` path exceeds the 15-char
SSO limit so it sits behind a further indirection. A depth-3 chase reaches it.

The correlation is what makes this trustworthy rather than a lucky string hit.
Four commits, segmented:

| commit | proposal ptr | paths reached |
|---|---|---|
| #1 | `29b15b37d70` | 5, all noise |
| #2 | `29b15b37d70` | 5, **identical set to #1** |
| #3 | `29b15b385f0` | 4, all noise |
| #4 | `29b15b385f0` | 4 noise + **2 more** |

The two extra paths in #4, and only in #4:

```
+0xe8+0x40+0x38 -> "_era_a.module"
+0xe8+0x40+0x40 -> "tion/rail/modular_station/platform_cargo_era_a.m"
```

Commit #4 is the `SELFUPGRADE` of the modular rail station. **`+0xe8` tracks the
construction content**; everything else is heap noise, provably so because it is
byte-identical across commits that share a pooled proposal pointer.

## What is still unknown

- **Strings are reached mid-buffer.** `"tion/rail/..."` is missing its `sta`
  prefix, `"_era_a.module"` is a fragment. The chase lands on pointers into the
  middle of buffers, so these offsets are not string starts. The container at
  `+0xe8` needs mapping properly (presumably a vector of construction entries),
  not string-scraping.
- **Action→commit is not 1:1.** Five actions (road, rail, depot, upgrade,
  demolish) produced **four** commits. Unexplained.
- **Proposal pointers are pooled and reused** (`29b15b37d70` twice, then
  `29b15b385f0` twice), so the pointer is not an identity.
- **n=1.** One run, one modular station. The correlation is strong but has not
  been repeated across construction types.

## Assessment

Cheaper than the first run suggested, dearer than "an afternoon". The hook is
free; decoding `+0xe8` into something shippable is real struct-mapping work.

A middle option the data suggests, which needs **no struct decoding at all**:
use the hook purely as a **trigger** — "a commit happened, and here is whether it
was ours" — and let the existing Lua capture take a targeted diff at that
instant instead of polling blindly. That kills the echo class (a commit we
caused is flagged at its source) and the ordering class (one commit is one
atomic batch, shipped as a unit), which together account for most of the bugs
this project keeps hitting, while reusing every replay function that already
works. It does not solve construction-owned edges, which stays a Lua concern.

## Reproducing

```
cmd /c "call <repo>\bridge\build_probe_apply.bat"
# launch instances, then:
injector\injector.exe <A-pid> <repo>\bridge\out\tpf2_probe_apply.dll
tools\run_mptest.ps1 -Scenario tools\scenarios\probe_applyproposal.txt -Target a -WaitSeconds 100
# read out\tpf2_probe_apply.log
```

Note `_fsopen(..., _SH_DENYWR)` in the probe, not `fopen_s`: MSVC's `fopen_s`
opens exclusively, which made the log unreadable while the game held it.
