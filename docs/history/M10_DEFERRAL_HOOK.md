# M10 RESULTS — Deferral hook
### Intercepting and cancelling a player's build before the engine acts

**Verdict: WORKING.** A player's build can be cancelled in-flight. This is the
capability Lua could not provide at all, and it was the last missing mechanism
for real lockstep.

```
[defer] #1 observed  builder=29c1466c490 caller_rva=40b7d6 (original ran)
[defer] #2 SUPPRESSED builder=29c1466c490 caller_rva=40b7d6 -- the build did NOT happen locally
[defer] #3 SUPPRESSED ...
[defer] #4 SUPPRESSED ...
[defer] alive: seen=4 suppressed=3 suppress_flag=1
```

Three player builds intercepted and cancelled. Both instances stayed responsive;
town growth continued unaffected.

## Target

`StreetBuilder::UpdateEngine`, RVA `0x459ce0`. See `docs/re/UI_CAPTURE_PATH.md`
for how it was found and why `applyProposal` is the wrong place.

Discrimination, measured live on build 35924:

| event | hits |
|---|---|
| 243 engine-internal `applyProposal` commits (towns building) | 0 |
| dragging a build preview | 0 |
| cancelling with X | 0 |
| one completed player build | 1 |

## Mechanism

`bridge/src/deferrelay.asm` — the first relay here that can skip the original.
The handler returns 0 (proceed, jump to trampoline) or 1 (suppress, return
straight to the game). Suppression drops the blob's three stack slots so `rsp`
lands on the game's return address, then `xor eax,eax / ret`.

Returning zero is sufficient because **the caller does not read the result**:
after `call rax` at `0x40b7d4` the next instruction is `cmp byte ptr [rbx],0`,
which touches memory and not `rax`. Verified by disassembly.

Hook: 21-byte steal. Prologue boundaries are 3,4,5,7,14,21,30 with no
RIP-relative bytes, and 21 is a size `hook.cpp` supports. The function opens
`mov rax, rsp`, which the trampoline re-executes — safe only because the relay
restores `rsp` to its entry value before jumping there.

## Safety properties

- **Off by default.** Suppression requires `tpf2_defer.cfg` to contain
  `suppress=1`. Re-read per event, so it flips without a rebuild or restart —
  which matters because an injected DLL cannot be replaced while the process
  lives.
- **Fails open.** If the handler faults it returns "proceed". A bug there costs
  an un-deferred build, never a lost one.
- **Single instance.** Refuses to install if a copy is already loaded. Injecting
  twice does not fail loudly — the newer DLL patches an address the older one
  already detoured and their log handles clobber each other, so the output looks
  exactly like the hook never firing. That cost three measurement cycles.
- **Cancelling is a state the game already produces.** Pressing X discards a
  pending proposal without notifying the engine, so a suppressed call leaves the
  builder in a situation it reaches routinely on its own.

## What this does NOT do

It proves the CANCEL half of deferral. It does **not** re-issue the command.
Extracting the proposal geometry from the builder object is a separate unsolved
problem, so with suppression on a build is simply lost. That is why the flag
defaults off and why this is a mechanism test rather than a feature.

## Next

1. Extract the proposal from the builder (`rcx`) so a cancelled build can be
   described. Field accesses in `UpdateEngine` run to `0xac8`; `0x228`/`0x229`
   look like a `std::optional` payload plus its engaged flag
   (`m_tempProposalData`).
2. Feed that into the lockstep scheduler (`mod/mp_lockstep_1`), which already
   executes commands at an agreed game-time stamp on both peers.
3. Locate the equivalent for stations/depots
   (`construction_builder_util::Context`) and for vehicles/lines, which M2 §5a
   puts on four separate factories.
