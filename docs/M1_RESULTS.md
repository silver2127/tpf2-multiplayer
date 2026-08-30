# M1 RESULTS — Hook-Point Symbol Map for TransportFever2.exe
### Static analysis milestone: from 138,112 anonymous functions to named hook targets

Method: pure static analysis (no debugger, game never launched).
Pipeline: PE parse → .pdata RUNTIME_FUNCTION table (138,112 exact function
bounds) → string→VA mapping → vectorized RIP-relative xref scan (numpy) →
capstone-annotated disassembly → caller walk (E8/E9 + address-taken scan).
Tools: `m1/find_hooks3.py`, `m1/walk_up.py` (venv: capstone/pefile/numpy).

All addresses are VAs for ImageBase `0x140000000` (ASLR: rebase at runtime).

---

## 1. Confirmed hook points (the money table)

| Role | Address | Evidence |
|---|---|---|
| **`SetupCommandInterface`** (registers whole `api.cmd` Lua table) | `0x140d042e0` (size ~11 KB, ends `0x140d06f5d`) | contains LEAs to `'SetupCommandInterface'`, `'sendCommand'`, all 33 command-name strings |
| **sol2 shared command trampoline** (every `api.cmd.make.*` call enters here) | `0x140083110` | sole target of all 33 registration LEAs in SetupCommandInterface |
| **`CommandList::Swap(std::vector<Command>&)`** | `0x1409d2d5f` | disasm swaps 3 qwords (vector begin/end/cap), asserts `'&commands != &m_data->commands'` (line 54), `'m_data->commands.empty()'` (line 63) in `CommandList.cpp` |
| **Command-by-index executor** (`CommandList` apply path) | `0x1409d2420` | assert `'idx >= 0 && idx < (int)commands.size()'`; indexes with `imul rdx, rax, 0x38` |
| **`sizeof(Command)`** | **`0x38` (56 bytes)** | from the executor's index multiply |
| **GameSim big update function** | `0x140157390` (size ~5.6 KB) | 3 GameSim.cpp asserts; called once from `0x14014f8b0` |
| **Frame/update driver** | `0x14014f8b0` → called from `0x140117f90` | caller chain toward the main loop |

Supporting maps:
- `apply_command.cpp` — 15 functions (`0x1409d54a0`, `0x1409d5620`, `0x1409d5720`,
  `0x1409d5770`, `0x1409d6280`, `0x1409d6bc0`, `0x1409d70c0`, `0x1409d8110`,
  `0x1409d8c90`, `0x1409d98a0`, `0x1409d9e10`, …) — the per-command-type apply handlers.
- `make_command.cpp` — 21 factory functions (`0x1409dc5e0` … `0x1409df4e0`).
- `GameSim.cpp` — 11 funcs; `GameTime.cpp` — 10 funcs (incl. `0x1402872f0` with
  12 callers = likely the time-advance); `GameState.cpp` — 31 funcs;
  `Serializer.cpp` — 7 funcs (save/load; relevant for resync).
- Full annotated disassembly: `M1_map.txt`; caller walks: `M1_walk.txt`;
  registration pairs: `api_cmd_map.txt`.

## 2. The 33 registered `api.cmd` commands

`bookJournalEntry, buildProposal, buyVehicle, connectTownsAndIndustries,
createLine, createTowns, deleteLine, developTown,
instantlyUpdateTownCargoNeeds, removeField, removeTown, replaceTerrain,
replaceVehicle, reverseVehicle, sellVehicle, sendToDepot, sendScriptEvent,
setAnimalState, setCalendarSpeed, setColor, setDate, setGameSpeed, setLine,
setName, setVehicleManualDeparture, setTownInfo, setUserStopped,
setVehicleTargetMaintenanceState, setVehicleShouldDepart, spawnAnimal,
updateLine` (+ `sendCommand` itself and the `make` subtable).

**Caveat (honest):** all 33 register the same C-callable trampoline
(`0x140083110`) — sol2 stores the per-command C++ functor as a closure
upvalue. Resolving name→functor statically needs deeper data-flow analysis;
dynamically it's a 10-minute job (breakpoint the trampoline, dump upvalues).
This does NOT block hooking: the trampoline is a *better* single hook point —
every scripted command funnels through it.

## 3. Architecture confirmations from disassembly

- `Command` objects are 56-byte structs in a `std::vector` inside
  `CommandList::m_data` (double-buffered: `Swap` exchanges the pending and
  active vectors and flips a flag byte at `m_data+0x18`).
- `Swap` at `0x1409d2d5f` has **no direct callers and is never address-taken**
  → it's a COMDAT out-of-line copy; live call sites inlined `Swap` into their
  own bodies. Hooking strategy must therefore target the **executor**
  (`0x1409d2420`, called through tail-call thunk `0x1409d34d0`) or the
  trampoline (`0x140083110`), not `Swap` itself.
- `0x1409d34e0` (right after the thunk) dispatches on a command-type argument
  (`cmp r8d, 4`) — candidate for the per-type apply switch.
- Scripting init chain: `SetupCommandInterface` ← `0x141126570` (one caller;
  the scripting-environment setup).

## 4. What M1 does NOT yet have (next milestone inputs)

1. **Exact per-tick apply site.** The chain `0x140117f90 → 0x14014f8b0 →
   0x140157390` is frame-driven, but the instruction that walks the swapped
   command vector each tick wasn't pinned (it's one of the inlined `Swap`
   callers). Found fastest dynamically: breakpoint `0x1409d2420`, look at the
   call stack.
2. **Command struct layout** (56 bytes: type tag offset? payload union?) —
   one debugger session dumping live commands settles it.
3. **Per-command functor mapping** (see §2 caveat).
4. **Determinism surface** (RNG state locations, thread scheduler) — untouched;
   remains the project's biggest risk per REPORT.md §6.

## 5. M2 hook design (updated with M1 data)

A DLL detour needs exactly two hooks to see every command in the game:

1. `0x140083110` (trampoline) — captures every **Lua-originated** command
   with its registered name resolvable via upvalue.
2. `0x1409d2420` (executor) — captures every command **at apply time**
   (UI- and script-originated alike), already marshalled as 56-byte structs
   in tick order. This is the primary replication tap.

Injection of remote commands: call the same executor (or the enclosing apply
loop) with fabricated 56-byte structs — requires §4.2 layout knowledge first.

## 6. Reproducibility

```
venv/Scripts/python m1/find_hooks3.py   # xref map + disasm   -> M1_map.txt
venv/Scripts/python m1/walk_up.py       # caller walk         -> M1_walk.txt
# api_cmd_map.txt regenerated by the parse step (see session notes)
```

Re-run against any future exe build; addresses will shift, method won't.
ASLR note: all VAs assume ImageBase `0x140000000`; at runtime compute
`actual = VA - 0x140000000 + GetModuleHandle(NULL)`.
