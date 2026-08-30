# The UI capture path — where a player's build actually originates

> **CORRECTION (later, from live measurement + `ACTION_MAP.md`).** The chain
> below claims `UpdateEngine` emits a `boost::signals2` signal and that the
> engine reaches `applyProposal` through async subscribers. **That is wrong.**
> `StreetBuilder::UpdateEngine` calls `make_cmd::BuildProposal` and then
> `CommandList::Add` (`0x9d2a00`) **directly** — no signal, no `packaged_task`.
> Measured: hooking `CommandList::Add` during one player road build logged
> exactly one call from `caller_rva=459eb7`, which is inside `UpdateEngine`
> itself, against 12,975 calls from the Lua bridge at `1126f1a`.
>
> What survives: `UpdateEngine` IS a valid interception point and the deferral
> hook built on it works (`M10_DEFERRAL_HOOK.md`). What was wrong was the
> mechanism — and with it my explanation for why static call graphs failed. They
> failed on the *other* legs of the search, not on this one.
>
> The better tap is `CommandList::Add`: one function for all 37 command types.
> See `ACTION_MAP.md`.

Image base `0x140000000`; addresses are RVAs.

## The chain

```
StreetBuilder::Step            0x4575c0   (1418 lines) per-frame while dragging
  -> StreetBuilder::UpdateEngine   0x459ce0   (150 lines)  pushes intent out
       -> boost::signals2 emit  signal<void(construction_builder_util::Proposal const&,
                                            construction_builder_util::ProposalData&)>
            emitters/connections around 0x3d07f0, 0x3e3270, 0x3e4180, 0x3e9510
            -> subscribers, dispatched ASYNC (std::packaged_task / std::future)
                 -> ... -> applyProposal 0x9e76e0
StreetBuilder::UpdateRenderer   0x45a0b0   (460 lines)  draws the preview
```

Evidence: `UpdateEngine`'s decompiled body calls four functions in the signal
region (`0x3d07f0`, `0x3e3270`, `0x3e4180`, `0x3e9510`) and **none** in the apply
region (`0x9e0000`–`0x9f0000`).

## How these were found

Not by call graph — a `signals2` emit sits between the UI and the engine, and
`getCallingFunctions()` cannot traverse an indirect dispatch. Two static walks
(depth 4 and depth 8, with UI and async markers) both returned 0 of 22.

They were found from **assert strings**, which name fields and locate the code
that touches them:

| assert text | function | recovered name |
|---|---|---|
| `m_tempProposalData.has_value()` | `0x4575c0` | `StreetBuilder::Step` |
| `!proposalData.proposal.proposal.` | `0x459ce0` | `StreetBuilder::UpdateEngine` |
| `proposalData.result.nodeLanes.si`, `.segmentLanes` | `0x45a0b0` | `StreetBuilder::UpdateRenderer` |

`m_tempProposalData` was the tell: an `m_` member holding a *temporary* proposal
is what a builder keeps while previewing a drag.

## Why applyProposal was the wrong interception point

1. **It is shared.** A 139-second window with one player build logged 89
   commits; `b6e040` (`BuildingTypeRep::Construct`) and `95c810`
   (`street_util::StreetToolkit`) fire constantly because towns build on their
   own. Suppressing there would cancel the engine's own construction.
2. **It is downstream of the decision.** By the time execution reaches it, the
   intent has fanned out into effects — one of the per-success callers,
   `0x21ed420`, turns out to be terrain modification
   (`terrain::HighResHeightMap`), a *consequence* of building rather than the
   command.
3. **Runtime frequency alone misleads.** `0x a74070` scored "UI-only" purely
   because it fired during the UI window and not the shorter script window;
   decompiling it showed `SimBuildingSystem::Update2` — a per-tick simulation
   system, not UI at all.

## VERIFIED LIVE — UpdateEngine discriminates perfectly

Hooked `0x459ce0` (21-byte steal) observation-only alongside `applyProposal`,
build 35924:

| phase | applyProposal commits | StreetBuilder::UpdateEngine |
|---|---|---|
| idle, 40 s | 29 | **0** |
| idle, 80 s | 55 | **0** |
| after ONE player road build | 115 | **1** |

```
[probe] UPDATE_ENGINE #1 t=37021421ms this=29c1466c490 caller_rva=40b7d6
```

One player action produces exactly one call. 115 engine-internal construction
commits produce none. `rcx` is the builder object; the caller is `0x40b7d6`.

This is the property the whole deferral design needed and `applyProposal` could
not provide: cancelling here cannot disturb town growth, because town growth
never comes through here.

## Why UpdateEngine is the right one

- It is **specific to the UI tool**, so hooking it cannot disturb town growth.
- It sits **before** the engine acts, which is what deferral requires: cancel
  here and nothing has happened yet.
- It is **small** (150 lines) and its parameter is the builder object, with
  field accesses at `0x38 0x50 0x58 0x68 0xe0 0xf8 0x1c0 0x1d0 0x228 0x229
  0x378 0x380 0x658 0x660 0x670 0x678 0xac0 0xac8`.
- The signal it emits carries `(Proposal const&, ProposalData&)` — the intent,
  in the engine's own vocabulary, already assembled.

## Open

- `construction_builder_util::Context` is the construction-side counterpart
  (`Context_CmdData::BuildProposal` ties it to the command payload). Its
  `UpdateEngine` equivalent has not been located yet.
- Whether cancelling at `UpdateEngine` is clean, or whether the builder assumes
  the emit succeeded and leaves `m_tempProposalData` in a bad state.
- M2 §5a lists four more taps for vehicles and lines, which are separate
  factories and unaffected by any of this.
