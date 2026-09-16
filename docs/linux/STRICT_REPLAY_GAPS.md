# Strict replay gaps with unchanged Windows 0.4.22 Lua

Read-only feasibility audit, 2026-09-14. The shared Lua files must remain
byte-for-byte Windows 0.4.22. No proposed adapter below is installed. A blocked
multiplayer action is an incomplete feature, not completed strict replay.

Binary evidence uses Linux build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. RVAs are relative to that image.
Lua line references refer to `mod/mp_lockstep_1/res/` in the unchanged release.

## Name and colour: the existing wire is usable, but origin replay is skipped

The relevant sequence is concrete:

1. `scripts/mp/inject.lua:1026` parses VNAME/VCOLOR using a local entity ID,
   resolves a vehicle/line/construction key, and can drop an untracked entity or
   a colour echo. Lines 1068 and 1075 always schedule `skipOrigin = 1`.
2. `scripts/mp/net.lua:349` constructs the scheduled table, copies those args,
   and appends that table to `CM.queue` at line 354. Only afterwards does it
   encode/broadcast the wire at lines 359–366.
3. `scripts/mp/vehicles.lua:441` and `:459` return immediately when
   `skipOrigin == 1` and `origin == K.INSTANCE`.
4. Windows `native/src/slice_hook.cpp:3874` explicitly documents SetColor/SetName
   as never cancelled, and `strictId` excludes IDs 13/14. Thus this Lua behavior
   matches the original Windows native behavior; it is not an overlooked ARMED
   flag in the Linux parser.

Cancelling the Linux UI action and merely writing the existing VNAME/VCOLOR
record changes peers but leaves the origin unchanged. Rewriting only an outgoing
network file cannot fix that: the origin's queue already contains its own table.
The current Linux native barrier blocks those clicks pending a working adapter.

## An existing synchronous execution point

`config/game_script/lockstep.lua:1014` prints:

```
APPLY <op> seq=<seq> origin=<origin> at=... now=... lag=... step=... late=...
```

Line 1017 immediately calls `execute(c)`. This happens after the command becomes
due, after the buy/binding guards, and after its `(origin,seq)` is entered into
the executed set. `log` at line 430 synchronously calls `print` before dashboard
bookkeeping. The message occurs for origin VNAME/VCOLOR too, before their early
return in the functions above.

The actual Linux ELF provides a native observation point for this existing call:

| Evidence | RVA / contents |
|---|---|
| Base-library `print` name | string `0x3f1f975` |
| Registration name/function pair | `0x59a8a10 -> 0x3f1f975`, `0x59a8a18 -> 0x986f30` (`R_X86_64_RELATIVE`) |
| Adjacent base-library entries | `pairs`, `pcall`, `print`, `rawequal`, `rawlen` |
| Native print function | `0x986f30`, 428 bytes; receives `lua_State*` in rdi |
| First 14 bytes | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` |
| Current argument count | print calls `0x982100` at `0x986f63`; computes `(L.top - (L.ci.func + 16)) / 16` |
| String extraction | print calls `0x982770` at `0x987012`; index resolver is `0x981e40` |

These are reads of the actual game ELF, not a stock Lua ABI assumption.
`0x981e40` proves a positive stack index is `L.ci.func + index*16`, bounded by
`L.top`. `0x982770` checks the value tag's low nibble for string (4), then reads
string length at `TString+0x10` and bytes at `+0x18`. Its non-string branch can
convert/allocate; an observer must first require an existing string and use
bounded guarded reads. Calling it indiscriminately is not an allocation-free
probe. The loaded state's print binding still needs runtime verification.

### A possible minimal native origin adapter

A narrowly scoped native adapter could observe only a provenance-verified
VNAME/VCOLOR `APPLY` belonging to this instance, obtain that exact existing `c`
and its module state, and invoke the unchanged `CM.execSetName` or
`CM.execSetColor` through the game's protected Lua API. A native-created copy of
`c` with its existing `skipOrigin` field set to zero would let the existing Lua
resolve the shared key, construct the command and install its normal callback.
The original `c` would remain unchanged and its subsequent normal execution
would still take the origin skip. This requires no new capture/wire field and no
replacement Lua function or source edit.

That is a candidate, not an enabled feature. Remaining proof/implementation:

- Identify the exact loaded 0.4.22 prototype/call frame and current `c`, `CM`, and
  `K`; do not authorize actions based on matching printed text alone.
- Correlate it with a native action whose cancellation and capture commit both
  succeeded. Company automatic VCOLOR, another mod's prints and stale queues
  must not cause an additional origin action.
- Verify upvalue/local access, stack preservation and protected reentrant Lua
  calls in the game's actual runtime. No game exception or Lua error may cross
  the DLL's foreign C++ cleanup frames. Lua callbacks and state teardown require
  correct lifetimes.
- Resolve targets at application time using the existing key semantics. A held
  raw entity ID may have been removed/reused while waiting.
- Deduplicate by the existing command identity and clear native bookkeeping on
  state/instance changes. Prove failure handling before enabling cancellation.
- Validate names, colour float precision, same-payload repeated clicks, key
  drops, colour echoes, delayed commands, reloads and callback failures with a
  loaded-world corpus.

An alternative retaining a game-owned native Command until its matching APPLY
would need additional command-copy/refcount, dispatch and exception-boundary
proof. It also needs the same target/correlation checks. The existing source
return-address filter alone does not provide those guarantees.

### Why a FIFO or a next-Add token is insufficient

Native captures do not have a one-to-one positional relationship with scheduled
commands. The unchanged inject parser can discard untracked names/colours or a
colour echo; a partial file record can fail parsing; other channels schedule
commands between captures; command dependencies delay execution; replay can be
retried or fail before issuing any native command. A captured FIFO entry can
therefore attach to a different later sequence. A correct mapping must use the
actual scheduled table and existing identity/payload, or first establish and test
all ordering/drop assumptions.

APPLY announces a queue dispatch, not exactly one native call. Construction,
stop and line executors have deferred queues; vehicle commands can fan out;
callbacks can issue subsequent commands. Pacing and company compensation also
issue commands outside this dispatcher. Thus observing any APPLY and authorizing
the next arbitrary Add would admit unrelated work and miss legitimate followups.
The proposed print observation is only a candidate trigger for the two known
origin-skip channels, with exact identity and provenance checks.

## Script provenance is a separate unresolved boundary

Independent proposal-area inspection found a more appropriate observation point
for commands that actually reach `api.cmd.sendCommand`: its registered C closure
at `0x197cf00` receives `lua_State*` in rdi and synchronously reaches the sink call
at `0x197add9`. It can observe Lua provenance before propagating a scoped identity
to native sinks. It cannot by itself recover origin VNAME/VCOLOR, because those
return before `sendCommand`.

Modern script Add returns are `0xa2f5c2` and `0x11225a9`; the third sink at
`0xa2d650` directly calls apply `0x15e2e70` without Add. The legacy script Add
return is `0x1d873b2`. Current `SlicePlayerAddSite` does not cover those script
sinks. AddDispatch counts script calls but its generic block tests the player
site set, so it does not establish that arbitrary script work is synchronized.
Maker and sink Command addresses also differ after moves; a maker-pointer cancel
arm is insufficient for these paths.

The game's Lua debug API is available at verified internal addresses:

| Function | RVA | Binary evidence |
|---|---|---|
| `lua_getstack` | `0x98b0d0` | calls from `debug.getinfo` at `0x98a35f`, `debug.getlocal` at `0x98a1f5` |
| `lua_getinfo` | `0x98b260` | call at `0x98a375` |
| `lua_getlocal` | `0x98b120` | call at `0x98a207` |
| `debug.getinfo` wrapper | `0x98a2d0` | registry `0x59a8cd0` name `0x3f1fcbf` (`getinfo`) |
| `debug.getlocal` wrapper | `0x98a170` | next registry entry `0x59a8ce0`, name `0x3f1fcc7` |

**The game's `lua_Debug` is not the stock header layout.** It has
`short_src[0x10e]` (270 bytes), total size `0x150`, and `i_ci` at `+0x148`.
`lua_getstack` writes that last field. `getinfo("S")` writes source pointer
`+0x20`, definition lines `+0x2c/+0x30`, and short source `+0x38` with length
`0x10e` at `0x98b4aa`; `getinfo("l")` writes current line `+0x28` at `0x98b423`.
Using a normal 60-byte `LUA_IDSIZE` header would overflow the structure.

The next justified step is observation-only source/prototype/line tracing at
sendCommand, with asynchronous construction/stop/line callbacks, pacing,
company bookkeeping and legacy interface paths included. Known source alone
still does not prove an agreed command/stamp. No broader script barrier should
be enabled until the permitted chains and failures are established.

## Vehicle stop, maintenance and loans

Here “stop” means the vehicle's start/stop control, not roadside stops/signals.

| Action | Existing Windows 0.4.22 path | Consequence for unchanged Lua |
|---|---|---|
| Vehicle user-stop | No capture factory or executor found. Windows FACTORIES contains IDs 2–10 and 13–17, omitting stop/maintenance; its capture branch excludes 11/12. Lua `execVehCmd` dispatches VSELL/VDEPOT/VREV/VLINE only. | No existing stop wire operation to resume through Lua. |
| Target maintenance | No capture/executor/poll found. `vehicles.lua:700–701` sets initial purchase `maintenanceState=1`, `targetMaintenanceState=0`; this is not maintenance-edit replay. | No existing maintenance wire operation. |
| Loan | `stops.lua:172` polls the local player's loan; `lockstep.lua:840` calls it every 15 ticks. A changed value schedules existing `LOAN {v=absolute}`. `conx.lua:1402` executes it on peers by booking the difference. | It is native read-back replication. `conx.lua:1414` explicitly skips the origin; a cancelled loan causes no polled change and therefore no capture. |

Linux binary evidence confirms that vehicle stop/maintenance have real command
paths; they are not merely transient UI settings:

- `SetUserStopped` factory `0x15ebf00`, CmdData tag `0x08`; UI calls
  `0x14310fd` (false) and `0x14311ff` (true), followed by Add at
  `0x1431124` / `0x1431226`. The registered Lua maker returns at `0x196c234`.
- `SetVehicleTargetMaintenanceState` factory `0x15ec000`, tag `0x09`; UI call
  `0x126f376` passes entity in edx and float state in xmm0, followed by Add
  `0x126f39e`. Its Lua maker returns at `0x196b43c`.
- `Book` factory `0x15ecd80`, tag `0x1f`, reads stack arguments; the Lua maker
  returns at `0x1973034`. Its non-generic ABI is documented in
  `docs/re/linux/SLICE_CORE.md` (C-FAC-4).

There is no hidden .22 stop/maintenance channel found in the full shared Lua or
Windows slice. Substituting reverse, depot or vehicle replacement would change
the requested action and is not a valid port. Supporting them requires a native
capture/transport/replay path, or an explicitly approved protocol change; it
cannot be claimed complete by removing a guard.

Loan has an existing wire operation that a future native Lua adapter could
schedule using its current `v`/company fields, but capture must read the intended
UI booking before cancellation, maintain the correct absolute target across
pending changes, and provide origin execution that bypasses only this known
origin-skip behavior. Polling the unchanged world after cancellation cannot
supply the missing intended amount. No such adapter is installed.
