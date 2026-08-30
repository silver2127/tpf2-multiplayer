# Where a player's build can be cancelled — and where it cannot

Measured 2026-08-14 on build 35924.

## Result

Cancelling at **`CommandList::Add` `0x9d2a00` wedges the build tool.**
Cancelling at **`StreetBuilder::UpdateEngine` `0x459ce0` does not.**

## Evidence

`slice_hook` captured a road at `make_cmd::BuildProposal`, suppressed the
following `CommandList::Add`, and the lockstep engine replayed it successfully
on both peers:

```
[slice] #1 captured road, 3 nodes ... CANCEL local build (caller_rva=459eb7)
[ls-a]  EXEC ROADN seq=4 nodes=3 streetType=25 success=true
[slice] alive: captured=1 cancelled=1 addHits=1
```

The player then attempted a second road. **No second capture was ever logged** —
the attempt never reached `BuildProposal`, so the tool was already dead. The
counter stays at `captured=1` no matter how many times the player clicks.

This reproduced across two sessions. It first surfaced as the report "I can't
build more than 1 road", and earlier as "I can't build anything" after a rail
build had also been cancelled — one wedge per tool, which is the same effect
seen twice.

By contrast `defer_hook`, which suppresses `StreetBuilder::UpdateEngine`,
cancelled **three consecutive player builds** in its original test. The tool
survived each one.

## Why

`CommandList::Add`'s 4th argument is the completion callback:

```
CommandList::Add(CommandList*, ?, Command* cmd,
                 std::function<void(Command const&)>* callback, ?*)
```

Every UI call site materialises a
`std::_Func_impl_no_alloc<<lambda_…>, void, Command const&>` for it — the same
`function(result, success)` the Lua API exposes. The build tool passes that
lambda and waits to be told the command finished.

Suppressing the call swallows the callback, so the tool waits forever. This is
not a bug in the hook; it is a **contract violation**. `Add` promises to call
back, and the suppress path silently breaks that promise.

`UpdateEngine` has no such contract — it constructs the callback internally and
nothing outside is waiting on it — which is why suppressing there is clean.

## What this rules out

Three candidate causes were eliminated first, by experiment rather than
reasoning:

1. **The replay code.** `execPolyline` was driven directly through the inject
   file with no hook loaded: a 2-node road, a 3-node multi-edge road with shared
   nodes, and a follow-up build all returned `success=true`, and the world kept
   accepting builds. Invented placeholder ids (`-1001001`) and shipped `z`
   values are both fine.
2. **The game or save.** A control launch with no DLL injected at all built
   roads normally.
3. **Relay register damage.** `deferrelay.asm` preserves `rcx/rdx/r8/r9/r11`,
   all six volatile `xmm` registers, `rax` and `r10`, and restores `rsp` to its
   entry value before the trampoline.

## RESOLVED — option A works

Firing the callback before suppressing fixes the wedge. Measured: nine
consecutive player builds captured and cancelled, `captured=9 cancelled=9`,
tool responsive throughout, where previously the count stuck at 1 forever.

The vftable slot was verified against the binary rather than assumed. The
callback object is built at `[rsp+0x78]` in `UpdateEngine` as
`{ vftable*, captured this }`, so `r9` points straight at the impl:

| slot | RVA | code | identification |
|---|---|---|---|
| 0,1 | `45b010` | `lea rax,[vft]; mov [rdx],rax; mov rax,[rcx+8]; mov [rdx+8],rax` | `_Copy`/`_Move` — 16-byte impl |
| 2 | `45b540` | full prologue, EH frame | **`_Do_call`** |
| 3 | `45c580` | `lea rax,[rip+X]; ret` | `_Target_type` |
| 4 | `b8790` | `test dl,dl; mov edx,0x10; jmp operator delete` | `_Delete_this`, frees 16 bytes |

`_Delete_this` freeing exactly the size `_Copy` implies, plus a two-instruction
RTTI getter at slot 3, pins the standard MSVC order and puts `_Do_call` at
vftable+0x10. The call is `_Do_call(this, Command const&)` → `rcx = r9`,
`rdx = r8`.

If the callback cannot be fired, the hook **declines to cancel** and lets the
build run. A local build that also replicates is a visible, recoverable desync;
a dead build tool is not.

## Options as they were assessed

**A — Invoke the callback before suppressing.** Preserves strict lockstep. Needs
the MSVC `std::function` layout and the `_Do_call` vftable slot. A wrong slot
crashes the game inside the UI path, so this must be *verified against the
binary*, not guessed. Three single-sample inferences already failed this session.

**B — Cancel at `UpdateEngine` instead.** Proven clean, but `UpdateEngine` is
what *calls* `BuildProposal`, so suppressing it means the geometry is never
built and there is nothing to capture. Would require decoding the geometry out
of the `StreetBuilder` object at entry — a fresh decode task.

**C — Stop cancelling.** Let the local build happen and replicate to peers only.
Never wedges the UI, never destroys work, and is the smallest change. Gives up
strict lockstep on the originator: it executes at `T0` while peers execute at
`T`, so entity ids may diverge — which matters because every non-construction
command (`SetLine`, `SellVehicle`, `BuyVehicle`) addresses entities *by id*.

There is no patch that makes option A safe without first reading the binary. The
choice between A, B and C is a design decision about how much strictness the
lockstep model actually needs.
