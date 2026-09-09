# STRICT lockstep for every replicated player action — engineering plan

> **Note (2026-09-08, after the split):** `lockstep.lua` line numbers below refer to
> the single-file version at commit `302974a`. The file has since been split into
> `res/scripts/mp/*.lua` (see the layout comment at the top of `lockstep.lua`);
> search by symbol or log string instead.

Build 35924, ImageBase `0x140000000`; every address below is an RVA. Line
numbers are against `native/src/slice_hook.cpp` (2678 lines) and
`mod/mp_lockstep_1/res/config/game_script/lockstep.lua` (10464 lines) as of
commit `302974a`. Written 2026-09-08 from the code, not the docs; where a doc
and the code disagree the code is cited.

Definitions used throughout:

- **S (strict)**: the originator's command is cancelled inside the engine
  before it applies, shipped with a stamp, and replayed at that stamp on every
  instance including the originator. Mechanism today: a factory hook arms
  `g_pendingCmd` (slice_hook.cpp:136) with the Command pointer, the
  `CommandList::Add` hook matches `r8 == g_pendingCmd` (:2012) and returns 1;
  the inject file carries `ARMED 1` (:1458-1468) and the Lua replays on the
  originator only when `K.STRICT_OPS[op]` and `armed == 1` (lockstep.lua:4094).
- **L (local-then-replay)**: originator applies at click time, peers replay at
  the stamp. Everything in L becomes S under this plan.

The owner's stance, which this plan is written to: **breaking the UI is
acceptable**. Cancel at the earliest uniform point; the UI fallout is a list
of follow-up fixes with a named mechanism each, not a reason to keep a
channel optimistic.

---

## 1. Principle: one cancel point

### 1.1 Every player command can be cancelled at `CommandList::Add` `0x9d2a00`

The Add hook already sees every command in the game (`ACTION_MAP.md`: 82
direct call sites, one queue for UI and script). It runs ~100/s from the Lua
bridge, so it cannot classify by itself cheaply — today it does a single
pointer compare (`r8 == g_pendingCmd`, :2012) and nothing else. Cancelling
there is uniform: `return 1` from `DeferHandler` skips the call, the caller's
out-handle is zeroed (`ZeroAddResult`, :1975-1984, the 2026-08-30 crash), and
the completion callback is optionally fired first (:2058-2074).

Two facts make the point genuinely uniform:

1. **The command type is readable at Add.** The depot window's buy callback
   (`C:\tools\ghidra_out\depotui\buy_cb_body.c`, `0x748250`) receives
   `Command const&` and does `lVar5 = *param_2; if (*(char*)(lVar5+0xb18) != 13) throw`
   — so `Command+0x00` is the payload pointer and `payload+0xb18` is the
   variant tag (13 = BuyVehicle, matching the dispatch table in
   `ACTION_MAP.md` "The apply side"). At the Add hook that is
   `tag = *(uint8_t*)(*(uint64_t*)r8 + 0xb18)`. This settles the "exact base is
   unconfirmed" note in ACTION_MAP. Confirm it once by logging the tag on
   the next road build (expect 15) and buy (expect 13) before relying on it.
2. **Origin is readable at Add.** The Lua bridge's Add caller is
   `cmd_interface.cpp` `0x1126f1a` (UI_CAPTURE_PATH.md measured 12,975 calls
   from it vs one from `0x459eb7`). `caller == 0x1126f1a` at Add is "our own
   replay, never cancel" — a second guard alongside the factory-side
   `0xcec000..0xcf2000` sol2-wrapper filter (:1592, :2123).

### 1.2 What the factory hooks are for once Add is the cancel point

Capturing **arguments before Add**, and nothing else:

- Some arguments exist only at the factory. BuyVehicle's config is a by-value
  struct on the caller's stack (`st[0] = [calleeRsp+0x28]`, :1573-1576,
  :1608-1611); UpdateLine's `component::Line` is *moved out of the caller's
  temp and destroyed inside the factory* (:1216-1220), so it must be read at
  entry. The Command payload is pointer-dense and its per-type layout is
  unmapped (`COMMAND_SERIALIZATION.md` "RESULT of the differential dump");
  decoding it at Add would be a fresh RE task per type. Keep the factory hooks
  as the capture point.
- The factory hook is where `g_pendingCmd` is armed (the factory's hidden
  return pointer `rcx` is exactly the `r8` the UI passes to Add, :128-135).
- Caller-RVA classification for the BuildProposal family (road `0x459e97`,
  upgrade `0x4790fc`, bulldoze `0x3eb227`, construction `0x419f62`) happens
  here because one factory serves twelve tools.

### 1.3 Which actions never pass through a hook we own today

Every player action goes `make_cmd::X` → `CommandList::Add`; "poll-detected"
means we chose not to use the hook, not that there is none.

| action | factory it passes through | RVA | hooked today? | caller RVA |
|---|---|---|---|---|
| construction demolish | `BuildProposal` | `0x9dc750` | yes, classified "construction-demolish shape" and **not cancelled** (:1288-1290) | `0x3eb227` (`UI::Bulldozer::Apply` return) |
| module add/remove (station edit) | `BuildProposal` | `0x9dc750` | yes, logged "UPGRADE-shaped ... left to the con poll" (:1286-1288) when via the bulldozer; the ModuleBuilder path lands in the "UNREPLICATED BuildProposal" branch (:2319) | `UI::ModuleBuilder::MousePressed` `0x42b810` / `addmodulecomp.cpp` `0x4b6f00` — **return-address RVAs not yet recorded**; the :2319 log line prints them |
| roadside stop (street terminal) | `BuildProposal` | `0x9dc750` | logged "UNREPLICATED" (:2319) | `streetterminalbuilder.cpp` `0x460a30` — return RVA not recorded |
| signals / waypoints / track modify | `BuildProposal` | `0x9dc750` | logged "UNREPLICATED" (:2319) | `UI::TrackModifier::Build` `0x478d70` — return RVA not recorded |
| terraform / paint / brush | `BuildProposal` | `0x9dc750` | logged "UNREPLICATED"; captured once as `caller=4311c6` (COMMAND_ARGS.md "Live capture") | `UI::ProposalAction` `0x4310d0` |
| loan take/repay | `Book` | `0x9dc5e0` (steal 18, COMMAND_MAP.md row 31) | **not hooked** | `financescomp.cpp` `0x53b660`, `0x53b8b0` (LIKELY, file only) |
| game speed | `SetGameSpeed` | `0x9de9e0` (steal 21; NO Engine arg, payload in `rdx.lo32`) | **not hooked** | clock/menu/camera sites (ACTION_MAP "Everything else") |
| stop/start vehicle | `SetUserStopped` | `0x9df070` (steal 20) | not hooked | `vehicle_button_util.cpp` `0x88aa90` |
| maintenance target | `SetVehicleTargetMaintenanceState` | `0x9df340` (steal 20; value in XMM3) | not hooked | `vehiclemanager.cpp` `0x74a490` |
| manual departure / depart now | `SetVehicleManualDeparture` `0x9df170`, `SetVehicleShouldDepart` `0x9df270` | steal 20 each | not hooked | sol2 wrapper `0xcee710` only found for ManualDeparture |

Nothing on this list is "not established" as a **hook point**. What is not
established is the **payload layout** of five of them (construction params,
edge objects, terrain deltas, JournalEntry, and the double-slip node bit);
each has its measurement in §7.

### 1.4 The one rule change that makes strict uniform

Today the Add hook can decline an armed cancel ("callback NOT fired --
letting the build run", :2090-2093) while `ARMED 1` was already written at
capture (:1597, :1614). That is the 7a29978 double-buy: an intention that
could be refused. Under this plan **an armed cancel is always honoured**: the
"let it run" branch is deleted, the hook suppresses whether or not the
callback fired, and logs which. `ARMED` then states an outcome by
construction. The UI consequences of a swallowed callback are the follow-up
items in §2, per action.

---

## 2. Per-action conversion plan

Order = easiest and highest determinism value first. Each item is one commit
behind its own flag (§6).

### 2.1 SellVehicle and SendToDepot (L → S)

Both are already captured, decoded and shipped; they just never arm.

**(a) Cancel point.** Factory hooks exist (`FACTORIES[]` :114-126, ids 3 and
5). In the factory dispatch (:2123-2163) `cancel` is set only for ids 10 and
6. Add `id == 3 || id == 5` → `cancel = CfgHas("strict_sell")` /
`CfgHas("strict_depot")`. `CaptureFactory` then arms `g_pendingCmd` with
`g_pendingNoCb = 1` (:1651-1670, `waitsForResult` is false for these ids).
The Add hook's NoCb branch suppresses without firing (:2050-2057).
**Measurement first** (per the memory rule "per-command verification is
mandatory"): with the flag on, sell a vehicle and confirm
`CANCEL fire-and-forget` + `cancelled=1` in `tpf2_slice.log`. If the sell's
Add is never reached the cancel never lands and the Lua replays on top of a
native sale.

**(b) Wire.** Unchanged: `VSELL <n> <id..>` / `VDEPOT <veh> <sell01>`
(:1470-1500) with local ids; the Lua converts to keys (`deferVehCap`,
lockstep.lua:3481-3520). Vehicles still exist on the originator at capture
time because the sale was cancelled, so key resolution is unaffected.

**(c) Lua.** `K.STRICT_OPS` (:753) gains `VSELL = true, VDEPOT = true`.
`shipVehCap` (:3510, :3537) and the VDEPOT reader (:9250) stop setting
`skipOrigin = 1` and carry `armed = CM.lastArmed or 0` like VLINE (:3496).
`execVehCmd`'s gate (:4094) already does the right thing once the op is in
STRICT_OPS. The VSELL `forgetVehicle` on the capture side (:3511) must move
to the replay callback (:4235, already there for `success and VSELL`) —
forgetting at capture would make the originator's own replay fail to resolve
the key. `drainVehCap`'s deadline path (:3540-3548) ships partial batches
with `skipOrigin=1`; under strict a partial batch must ship `armed` and the
unresolved ids are simply lost on every instance — log it as a lost action,
not a divergence.

**(d) UI.** The depot window's sell passes a callback
(`buy_ui_site.c:333-348`, lambda `04ca4019…`); nothing has been shown to wait
on it. Expected break: the vehicle stays listed in the depot / vehicle
window for ~3 sim steps until the replay lands, and any "sold" toast/refresh
driven by the callback does not appear. Follow-up: none required — the engine
refreshes the window from entity events when the replayed sell applies. If
the window holds a dangling selection and asserts, fire the callback with the
impl-pointer fix from §2.3 instead of the NoCb path.

**(e) Determinism.** Money lands on the same step everywhere (sell refund
was one of the coop balance-split sources). A vehicle whose key never binds
(`VEHCAP_WAIT`, :3540) is now lost on all instances rather than local-only.

**(f) Flags/test.** `strict_sell`, `strict_depot` default ON. Test:
`tools\soak.ps1 -Manual` with a sell and a send-to-depot; assert A5 (desyncs=0),
A7 (balance identical), A13 (every action executed on every other instance),
plus the new A14 in §6: count of `STRICT -- originator replaying` lines on
the originator equals the count of `ARMED 1` lines in its inject file.

### 2.2 Stop/start, maintenance, manual departure, depart-now (N → S)

Never replicated today — a stopped vehicle is an unconditional desync on
the spot. Same shape as Reverse, which is already strict.

**(a) Cancel point.** Append to `FACTORIES[]` (:114-126):
`{0x9df070,20,11,"SetUserStopped","vehicle"}`,
`{0x9df340,20,12,"SetVehicleTargetMaintenanceState","vehicle"}`,
`{0x9df170,20,15,"SetVehicleManualDeparture","vehicle"}`,
`{0x9df270,20,18,"SetVehicleShouldDepart","vehicle"}` (steal sizes from
COMMAND_MAP.md "Hook table rows", control-validated by PrologueBoundaries).
Arguments: `r8` = vehicle Entity by value; `r9` = bool (UserStopped /
ManualDeparture); maintenance **value arrives in XMM3** — `DeferHandler`
(:1986) receives only integer registers, so `deferrelay.asm` must spill xmm3
to a slot the handler can read (it already preserves all six volatile xmm
registers, CANCEL_POINT.md "Relay register damage"). Arm with
`g_pendingNoCb = 1`; extend the id ranges at :1591 and :2107 (`id >= 2 && id
<= 10` becomes a membership test on `FACTORIES[]`).

**(b) Wire.** `VSTOP <veh> <0|1>`, `VMAINT <veh> <float>`, `VMANUAL <veh>
<0|1>`, `VDEPART <veh>` in `WriteInjectVehicleCmd` (:1470).

**(c) Lua.** Readers next to VREV (:9242): resolve `vehKeyFor(id)`,
`scheduleLocal(op, {key=, v=, armed=CM.lastArmed or 0})`. `execVehCmd`
branches (:4148-4204) build `api.cmd.make.setUserStopped(id, v==1)`,
`setVehicleTargetMaintenanceState(id, v)`, `setVehicleManualDeparture(id,
v==1)`, `setVehicleShouldDepart(id)`. Add the four ops to `K.STRICT_OPS` and
to the `execute` dispatch (:5771).

**(d) UI.** Vehicle window buttons toggle their icon from entity state; the
icon flips ~3 steps late. No callback is waited on (same class as Reverse,
verified live for id 10). Follow-up: none.

**(e) Determinism.** Depart-now on a vehicle that has already left by the
stamp is a harmless no-op on all instances (same step everywhere).

**(f)** `strict_vehflags` default ON. Test: `vehicle_e2e.txt` plus manual
stop/start; A5, A13, A14; the VPOS drift lane (commit 9c8e822) must stay 0.00.

### 2.3 BuyVehicle (L → S; reverses 7a29978)

This is the 37 m plateau (2bebc5f). The decision to never arm is reversed;
the crash and the "callback cannot be fired" are both explained by the
decompile and each has a specific fix.

**Why the callback fire fails at `74fda9`.** The depot window's buy site
(`depotui/buy_ui_site.c:563-565`):

```
uVar9 = FUN_1409dca00(local_210, uVar9, in_stack_00000038, in_stack_00000040);  // make_cmd::BuyVehicle
FUN_1409d2a00(pvVar16, &local_510, uVar9, local_280);                            // CommandList::Add
```

`local_280` is declared `undefined1 local_280 [56]` (:122) followed by
`local_248` (:529, :559): a 64-byte MSVC `std::function` object. Its impl is
**heap-allocated** — `buy_cb_function_ctor.c` `0x73ed10` does
`operator new(0x60)` and stores the `_Func_impl_no_alloc<lambda_94d66e9a…>`
vftable in the new block — and the pointer to it is written to
`local_248 = local_280 + 0x38`, which is MSVC's `_Mystorage._Ptrs[7]`
(`_Getimpl()`). So at Add, `r9` is the `std::function`, not the impl. The
hook reads `vft = *(r9)` (:2062) — on the road path that works only because
UpdateEngine's 16-byte functor is small-buffer-stored inline at offset 0
(CANCEL_POINT.md "`r9` points straight at the impl"); on the buy path
`*(r9)` is the unused small buffer, `Readable(vft, 40)` or the `__try` fails,
and `fired = false` every time.

**Fix:** resolve the impl the way MSVC does: `impl = *(uint64_t*)(r9 + 0x38)`;
`impl == 0` means an empty function (nothing waits); otherwise `vft = *impl`,
`_Do_call = *(vft + 0x10)`, call `_Do_call(impl, r8)`. Verify before enabling:
log `*(r9)`, `*(r9+0x38)` and `*(*(r9+0x38))` at Add for one road build
(expect `*(r9+0x38) == r9`) and one buy (expect a heap pointer whose vftable
slot 2 is `g_base + 0x753820` = `buy_cb_docall`, which calls `0x748250(this+8,
cmd)`).

**Why the 2026-08-28 crash is probably not "the window waits".** The buy site
destroys the Add out-handle immediately after the call
(`FUN_142357910(&local_510)`, `buy_ui_site.c:566-568`) — this is exactly the
stale-slot destructor fault that `ZeroAddResult` (:1975-1984) fixed on
2026-08-30 for the plane's Reverse, two days *after* the buy crash. The
no-callback suppress was never retried with `ZeroAddResult` in place.

**What the callback does if fired on a cancelled command**
(`buy_cb_body.c` `0x748250`): checks the tag, reads `payload+0x38` (the result
vehicle entity), resolves it (`FUN_1423e27d0`), and only `if (local_88[0] !=
-1)` touches the window ("vehicle-manager" refresh / select). So firing it is
safe **iff** `payload+0x38 == -1` at Add time. Measure it; if it is not -1,
the hook writes -1 there before firing (the command is ours and will never
be applied).

**(a) Cancel point.** Factory id 2, `CaptureFactory` arms with
`g_pendingNoCb = 0` (`waitsForResult`, :1662) — keep that, but with the impl
fix the fire succeeds. Delete :2160-2163 (the strict_buy refusal). Two
ordered experiments, cheapest first: (1) NoCb suppress + `ZeroAddResult`
(one-line change: `waitsForResult = false`); (2) fire via the impl fix with
`+0x38 = -1`. Ship whichever keeps the depot window alive.

**(b) Wire.** `VBUY <depotChild> <config…>` (:1421-1437) unchanged. The
originator maps the VEHICLE_DEPOT child to its construction's position
(lockstep.lua:9096-9135); the construction is untouched by the cancel.
`purchaseTime`: under strict no vehicle exists to read it from, so the
`parkedBuys` wait (:3649-3685) is bypassed — the strict reader branch
(:9153-9163) already ships at once and `buildVehConfig` stamps
`math.floor(c.at*1000)` (:4258-4264), identical on every instance.

**(c) Lua.** Already built: `execVBuy` gate (:4296), key bound from
`res.resultEntity` on every instance (:4362-4372, commit c51e745), buys
serialized one per sim step by `notBeforeStep` (:10093-10104) with the
`buysThisTick` wedge cap (:10138-10145). Flip `K.STRICT_OPS.VBUY =
CM.cfgFlag("strict_buy", true)` (:7262). Remove the `else` parked-buy branch
(:9164-9171) once strict is default; keep it under `strict_buy=0`.

**(d) UI.** The depot window does not show the new vehicle until the replay
applies (~3 steps). With option (1) the "buy" button callback never runs, so
the window does not auto-select the purchase; with option (2) it runs and
no-ops. Follow-up if the window shows a stale vehicle count: it is
event-driven from the entity (the replayed buy is a normal command) — nothing
to do. If a batch-buy dialog holds a cursor on `resultEntity`, fire (2).

**(e) Determinism.** Multiple buys in one click: each gets its own
`notBeforeStep` one step apart (:10098-10101), identical on the originator,
so binding is unambiguous (`batch-buy-one-tick-is-the-desync-root`). A buy
whose depot the peer cannot find (:4320-4323) is now lost everywhere — the
originator too — which is the correct strict outcome.

**(f)** `strict_buy` default ON. Test: `buy_vehicle.txt` (single unit), then a
manual 5-vehicle batch; A1 (no crash), A5, A13, A14; VPOS lane 0.00 across
all three; dashboard `v` count equal.

### 2.4 ReplaceVehicle (L → S)

Same as 2.3: factory id 4 is hooked and ships `VREPL` (:1486-1499); it is in
the `cancel` exclusion by omission. Arm like the buy (`waitsForResult = (f.id
== 2 || f.id == 4)`, :1662), fire via the impl fix (the vehicle window reads
the replacement's result entity — SLICE_STATUS "the UI waits for the
replacement's result entity"; measure `payload+0x38` for tag 14 as for 13).
Lua: `execVReplace` (:4394) gains the `armed` gate; reader (:9083-9089) drops
`skipOrigin`, carries `armed`; `K.STRICT_OPS.VREPL = true`. Key re-registration
in the callback (:4411-4420) then runs on every instance. UI: vehicle window
shows the old consist for ~3 steps. Flag `strict_replace`, default ON.

### 2.5 SetName and SetColor (L → S)

Not sim-affecting, but uniformity is free: ids 13/14 are hooked and shipped
(:1519-1560). Arm with `g_pendingNoCb = 1`; the reader (:9226, :9230) drops
`skipOrigin = 1`, carries `armed`; `execSetName`/`execSetColor` (:4055,
:4073) replace the skipOrigin test with the STRICT_OPS/armed gate. The
originator's rename resolves the target by key/position at replay time, so
the entity id is never needed. UI: the name field reverts to the old name
until the replay lands (~0.6 s), then updates — the rename dialog does not
wait on a callback (SetName is fire-and-forget in `line_ui_util.cpp`
`0x7b8200`, ACTION_MAP). Flag `strict_cosmetic`, default ON.

### 2.6 UpdateLine and DeleteLine (L → S)

**(a) Cancel point.** Factory ids 8 and 9 are hooked but the dispatch
forbids arming 7/8 (:2113-2118) and never consults 9. The stated reason for
UpdateLine — "the next edit ships a stale snapshot" — is a *capture* problem:
`LUPDATE <lineId>` ships only the event (:1531-1533) and the Lua reads the
line back from the entity (`lineSnapshot`, lockstep.lua:3834; reader :9276-9281),
which under a cancel has not changed. Fix the capture, then arm both with
`g_pendingNoCb = 1`.

**(b) Wire.** Decode `component::Line` at the factory. UpdateLine: `r9 =
&Line` (read at entry only, :1216-1220). Layout, ground-truth EXACT from
COMMAND_ARGS.md "ecs::component::Line stop record": stops vector at Line+0x00
(`GtDumpLine`, :1145-1150; note COMMAND_ARGS.md also says "+0x18" in one
sentence — settle it from the existing `[gt] f8stopspan` line of a sweep),
stride 0xa8, `station` Entity @+0x04, `terminal` int @+0x08, wait float
@+0x2c, alternativeTerminals vector @+0x10, waypoints vector @+0x38;
`waitingTime` @+0x18, `vehicleInfo` @+0x1c (0x24 B). Ship
`LUPDATE <line> <wait> <n> {<station> <terminal> <minWait> <maxWait> <loadMode>…}`;
the Lua converts station entity → station-group position on the originator
(the station still exists) exactly as `lineSnapshot` does today (fields 8-9,
stops-edge-rebuild note). Fields not yet pinned (loadMode, min vs max wait,
vehicleInfo): one `GT line` sweep (`GtFactoryDumps` case 8, :1216) with
sentinel values settles them. DeleteLine: `r8 = Entity`, already shipped.

**(c) Lua.** `execLine` (:3994-3997) replaces the origin skip with the
STRICT_OPS/armed gate for LUPDATE/LDELETE; `retryLineDep` (:3980) already
retries deterministically. Readers (:9281, :9293) drop `skipOrigin`. The
`forgetLine(lid)` at capture (:9294) moves to the replay callback (:4033).

**(d) UI.** The line editor keeps its own model and issues a full Line per
edit (COMMAND_ARGS.md: "UpdateLine fires once per stop operation"), so
successive cancelled edits are each complete. Break: the route overlay and
stop list refresh from the entity ~3 steps late; the "stale overlay after
removing a stop" (SLICE_STATUS open item 3) becomes universal. Follow-up:
none needed for correctness; cosmetic.

**(e) Determinism.** Stop changes on a line with running vehicles re-route
them at the same step everywhere — that is the value. Risk: an LUPDATE
carrying a station that a peer has not yet built is retried
(`retryLineDep`) — on the originator that station always exists, so the
originator applies on the first try while peers retry: **different apply
steps**. Mitigation already exists for lines born in the same batch
(`LINE_MATERIALIZE_STEPS`, :10108-10126); extend `notBeforeStep` to lines
whose stations were created in the same batch (`newConStep`, same pattern).

**(f)** `strict_line_edit` default ON. Test `create_line.txt`,
`line_and_station.txt`; A5, A13, A14.

### 2.7 CreateLine (L → S) — last of the line family

Determinism value is nil until a vehicle is assigned (VLINE is already
strict), so this is done for uniformity and ordered last.

**(a) Cancel point.** Factory id 7, `g_pendingNoCb = 1`. The recorded hazard
(:2113-2116): the line editor takes `resultEntity` (-1 when cancelled) and its
next `UpdateLine(-1, …)` is a fatal assert (r8_analysis_lin.md, decompiled).
So the DLL must also swallow any UpdateLine whose `r8 == -1` arriving from
the UI (factory id 8, `r8 == 0xffffffff`): arm it, suppress at Add, ship
nothing. The stops the player then adds in that editor are lost until they
reopen the line — that is the accepted UI break.

**(b) Wire.** `CreateLine(std::string name @rdx, CVec3f colour @r8, Entity
player @r9, component::Line @st[0])` (:1191-1215 `GtFactoryDumps` case 7).
Ship `LCREATE <name%> <r g b> <Line…>` with the same Line encoder as 2.6.

**(c) Lua.** The key `origin:seq` binds on every instance from the replay's
`getLines()` delta (`pendingLineKeys`, :4005); `pollLineKeys` on the
originator (:3871) must no longer key a natively-created line.

**(d) UI.** The line editor opens on a dead line and must be closed;
follow-up: the GUI-state script (`guiUpdate`, SLICE_STATUS "Session tooling")
can watch `lockstep_status_<inst>.txt` for a "line created <key>" event and
prompt; there is no API to re-target the editor. Flag `strict_line_create`,
default ON per the stance, but keep it separately revertable.

### 2.8 Loan (poll → S)

**(a) Cancel point.** New factory row `{0x9dc5e0, 18, 31, "Book", "sync"}`.
Arguments (COMMAND_MAP.md row 0): `(playerEntity @r8, JournalEntry @r9 or
st[0], position optional)`. JournalEntry layout is **not decoded**; one
`GT` sweep through `api.cmd.make.bookJournalEntry(player, entry, pos)` with
sentinel amounts (the factory-only sweep never touches the world,
SLICE_STATUS "Ground-truth generator") pins amount/category/type. Filter
callers: the Lua path (`cmBookJournal` is our own, sol2 block) and
`modsbrowser.cpp` `0x6ca230`. Arm with `g_pendingNoCb = 1`.

**(b) Wire.** `BOOK <amount> <type>`; the Lua maps to its existing `LOAN`
op but as a *delta* with `armed`, not the polled absolute.

**(c) Lua.** `execLoan` (:5719-5724) currently skips the originator because
the poll shipped after the fact and the echo walked the loan to the cap;
under strict the originator's loan never moved, so it replays too — gate on
`armed`. `CM.pollLoan` (:7353) stays as a safety net for loans moved by
anything that is not the finances window, but must not ship while a strict
BOOK is pending (`CM.loanExpect` already does this, :7358-7376).

**(d) UI.** The finances window slider snaps back until the replay lands
(~0.6 s). Follow-up: none.

**(e) Determinism.** Interest is charged from the step the loan lands — this
is one of the coop balance-split sources.

**(f)** `strict_loan` default ON. Test: the soak's LOAN action (`-Quick`,
poll-driven today — it must become a real click under strict, per the soak
header's rule); A7.

### 2.9 Construction demolish (poll → S)

The strict-demolish ticket, built on the EDEMO path (faf98db).

**(a) Cancel point.** Already at the BuildProposal hook, caller `0x3eb227`
(:2200). `LogBulldoze` (:1261) classifies `nrem >= 1 && nadd == 0` as
"construction-demolish shape" (:1288-1290) and does nothing. `toRemove` is
`r8+0x1e0` (`vector<Entity>`), **UNVERIFIED by any sweep** (:1250-1252). Verify
with `dumpprop=1`: bulldoze one depot, confirm the vector at `r8+0x1e0` holds
exactly one id equal to the depot's construction entity. Then: write
`CDEMO <n> <id..>` and arm exactly as the edge-demolish does (:2219-2235,
`g_pendingNoCb = 0` — the bulldozer waits on its callback, CANCEL_POINT.md;
the road path proves the fire works for this tool's callback shape).

**(b) Wire.** Local construction ids; the Lua resolves each to
`fileName + transf[13..14]` (they still exist — the bulldoze was cancelled)
and ships `DEMOLISH x y file` with `armed`. This replaces the
removal-detection poll (`pollConstructionRemovals`, :8334-8378) for
player-initiated demolishes; keep the poll as a tripwire that logs — never
ships — when a tracked construction vanishes without a CDEMO.

**(c) Lua.** `execDemolish` (:3115-3118) replaces the origin skip with the
STRICT_OPS/armed gate; `expectedDemolish` marking (:3140) applies on the
originator too so the poll does not echo. `rearmSplitsNear` (:8364) moves
to the replay.

**(d) UI.** The bulldozer cursor stays armed; the building vanishes ~0.6 s
after the click (the callback fires, so the tool does not wedge). Follow-up:
none.

**(e) Determinism.** Refund and passenger/cargo removal on the same step
everywhere — the whole point of the ticket. Money: the coop reconciliation
(`cmTransferCost`) that today attributes the refund must be dropped for
strict demolishes or it double-counts.

**(f)** `strict_condemo` default ON. Test `station_edit.txt` end (demolish);
A5, A7, the people-count lane `n:` (05850df).

### 2.10 Roadside stops, signals and waypoints (poll → S, built 2026-09-08)

**Status: ADD is strict (`strict_stops=1`); REPLACE and DELETE stay on the poll.**

**(a) Cancel point.** ONE caller covers all three: `CALLER_STOPTOOL = 0x460e0b`
(`UI::StreetTerminalBuilder::commit` → `make_cmd::BuildProposal`; measured
shape `addEdges=1 rmEdges=1` — the edge rebuilt with the object — plus one
`edgeObjectsToAdd`). Armed with `g_pendingNoCb = 0` (fire the callback, the tool
waits). `STOPX` is written from the Add hook only when the cancel *lands*, else
dropped and the poll captures the native build (no double capture).

**(b) Wire.** The 0x100-B `edgeObjectsToAdd` record is now decoded
(`StashStopFromProposal`; built by `0x21ef7d0`, cross-checked against six live
records and the poll's read-back of the built objects):

| off | field |
|---|---|
| `+0x00` | edgeEntity (−1 = the rebuilt edge) |
| `+0x04` | 0 street stop, 2 track object (signal/waypoint) |
| `+0x08` | −1 (entity slot, unused for a fresh placement) |
| `+0x10` | modelId; `+0x14` Mat4f transf (x,y,z at `+0x44/48/4c`) |
| `+0xd0` | the commit's bool arg — **provisionally `oneWay`** (0 on every sample; the `[stop]` log line prints it, a one-way placement pins it) |
| `+0xd1` | **engine `left`** (poll side=0 ⇔ 1, three stops) |
| `+0xd8` | std::string name (SSO); `+0xf8` playerEntity |

The edge is the removedSegments entry (`r8+0x48`, entity at `+0x00`) — a real
id, still valid on the originator because the build never happened.
`STOPX <edge> <kind> <modelId> <x> <y> <z> <left> <oneWay> <player> name=<rest>`.

**(c) Lua.** The `STOPX` reader ships the poll's own `STOPADD` fields from the
local edge (ends, `u` by projection, model name via `modelRep.getName`, track,
street type) **without `skipOrigin`**, so the originator replays through
`nativeStopProposal` exactly like a peer. New fields `eleft` (the engine byte)
and `tx,ty` (the originator's unit tangent at the object): `execStopAdd` sets
`left = eleft`, flipped only when its matched edge's tangent dots negative.
That is what the poll path could not do — a track object's `left` is **not** the
geometric side of its model (two signals both geometrically left of their edge
carried 0 and 1), and the geometric fallback built signals facing the wrong
way. `pollStops` still runs; on a strict placement it sees the replay land
under `expectStop` and stays silent (it is the tripwire).

**(d) UI.** As planned: the object appears at the stamp; the callback is fired
so the tool survives.

**(e) Not strict yet.** REPLACE (a compatible stop dropped on an occupied side):
the DLL refuses the cancel when `edgeObjectsToRemove` is non-empty because the
engine re-points that stop's lines (`old2newEdgeObjects`), which a script
proposal cannot carry — the poll's `STOPREP` + `LUPDATE` re-ship stays. DELETE
goes through the bulldozer (a different caller) — still the poll's `STOPDEL`.

**(f) Open.** Pin `+0xd0` with a one-way signal; make replace strict by
shipping the line re-point (or refusing the replace under a line); the
bulldozer caller for stop delete.
### 2.11 Constructions — true cancel (see §3)

### 2.12 Module edit / station upgrade (poll → S)

Same payload problem as §3 (the new ConstructionEntity's params), plus the
shape: `toRemove` (old construction) + `toAdd` (new CE) — the branch logged
"UPGRADE-shaped" (:1286-1288) for the bulldozer's module removal, and an
as-yet-unrecorded caller RVA for `ModuleBuilder::MousePressed` `0x42b810` /
`addmodulecomp` `0x4b6f00`. Until the CE params decode (§3.3) is pinned the
channel keeps the `scanConstructionEdits` poll (:8381) with `CONU` (:2801)
replayed on peers only. Interim strictness without a true cancel:
delete-and-replay exactly as `conx_strict` does — originator applies
natively, reads params post-apply, then at the stamp `upgradeConstruction`s
its own entity to the same params again (a no-op upgrade re-runs the
template deterministically; the CONU diagnostic self-upgrade at :2837-2846
already proves the call). Flag `strict_module`, default ON once §3.3 lands.

### 2.13 In-place node edits (N → S): double slip, bridge/tunnel swap, crossing type

BuildProposal with `addNodes == rmNodes` at the same position; the node
differential dump (:2340-2380) already logs `flags`/`type` of the removed and
added record. Do one double-slip toggle and one bridge-type swap and diff the
`rmNode`/`addNode` lines: the changed field is the bit to ship. Then route
these callers through the upgrade path (`isUpgrade`, :2288) with the node
record's `flags`/`type` appended to `ROADE` as a per-node tail (same
"append after the legacy payload" trick as the bridge tail, :700-703), and
`execPolyline` sets them on `nodesToAdd`. Cancel as the upgrade does
(`g_pendingNoCb = 0`, :2526). Flag `strict_nodeedit`. Not established until
the differential is read; measurement is already in the log.

### 2.14 Terraform / paint / asset brush (N → S)

BuildProposal from `UI::ProposalAction` `0x4310d0` (caller `0x4311c6`). The
proposal has **no node vector** (COMMAND_ARGS.md "Still to capture") and
`construction_builder_util::Proposal` has no terrain field; the height delta
must live in the Context/ProposalData (`r9`) or a side structure. Measure:
`dumpprop=1` + `DumpVectors` over `r9` (0x480 B, :2233-2239) on one raise and
one lower at known coordinates; look for a heightmap patch (floats around
the click's x,y). Terrain edits are sim-affecting (town growth, vehicle z),
so this is real value, but the payload is unmapped. Not established.

### 2.15 Game speed (applied on arrival → unchanged, with a note)

`SetGameSpeed` does not change the sim step (0.2 units, `K.SIM_STEP`,
lockstep.lua:766); it changes wall-clock pacing only, and the pacer/barrier
already set it from the Lua path (`CM.setSpeed`, :9546-9563; `shareSpeed`
:9576-9591 broadcasts the player's change as `LSSPEED`). Making it strict has
no determinism value and would fight the pacer. Recommendation: leave as is,
document it as "not a sim input". If uniformity is wanted anyway: hook
`0x9de9e0` (steal 21, payload `rdx.lo32`), cancel NoCb, and have the peers'
`LSSPEED` handler apply at the stamp — but the pause case (speed 0) then
pauses ~0.6 s after the click on every instance, which the player will feel.

---

## 3. Constructions: replace bulldoze-and-rebuild with a true cancel

### 3.1 What `conx_strict` is today

Not a cancel. `CaptureFactory`'s construction branch (:2254-2286) lets the
placement proceed and ships `ROADC` (street vectors); `pollNewConstructions`
(lockstep.lua:8168) reads `fileName/params/transf/NAME/survivors` off the
**applied entity** (`queueConCapture`, :7005-7033), `shipConxPair` (:7041)
pairs and ships `CONX`. `execConX` (:4478) on the originator, `conx_strict`
(default ON, :4480, :4507): phase 1 bulldozes the native copy (:4553),
re-queues at +0.6 (:4576); phase 2 rebuilds through the scripted
`buildProposal` like a peer. Commits 02b42fc→302974a spent a week on the
collateral: the native build re-shapes the road, the heal over-demolished
(711a583, e7f970a, e8f5c26), so now the originator keeps its native split and
snaps the scripted split onto it (:4559-4573). Money is reconciled by balance
snapshots (`strictBalPre/Mid`, :4541, :4580). All of this exists because the
native build happened first.

### 3.2 Is the proposal fully formed at the cancel point?

Yes. `UI::ConstructionBuilder::MousePressed` `0x419aa0` calls
`make_cmd::BuildProposal` (return `0x419f62`) then `CommandList::Add`
consecutively (ACTION_MAP "Construction family"). At the factory, `r8` is
`construction_builder_util::Proposal` (760 B, PROPOSAL_STRUCTURE.md
"Construction linkage -- RESOLVED"):

| offset | content | decoded for the wire? |
|---|---|---|
| +0x000 / +0x018 | addedNodes / addedSegments (with tangents, street type, bridge type, ownership @+0x68/+0x70/+0x74) | yes (`DecodeNodes/DecodeEdges/DecodeEdgeType`, :387-556) |
| +0x030 / +0x048 | removedNodes / removedSegments | yes (:2259-2261) |
| +0x0e0 / +0x0f8 | edgeObjectsToRemove / ToAdd | no (§2.10) |
| +0x170 | frozen node indices | read by `MergeTemplateStreet` (:1786) |
| +0x1c8 | segmentTags (`__module_<slot>`) | no; not needed (template regenerates them) |
| +0x1e0 | toRemove `vector<Entity>` (buildings the placement demolishes) | UNVERIFIED offset (:1250) |
| +0x1f8 | toAdd `vector<ConstructionEntity>` stride 0x8e0 | **partially**: fileName @CE+0x00, description @+0x50, icon @+0x70, position Vec3f @+0x100 (`a3+0x368 - 0x268`, COMMAND_ARGS.md "The construction half"), frozenNodes @+0x768, segmentsBefore @+0x780; **params and name: not pinned** |

`r9` is the Context (`checkTerrainAlignment`, `cleanupStreetGraph`,
`gatherBuildings`, `gatherFields`, `player` — the Lua rebuilds it as
`CM.conxContext()`, :1220-1240).

**Rotation** is baked into the street nodes, no matrix in the proposal
(COMMAND_ARGS.md "There is no transform in the proposal"); for a
free-standing construction with no street pieces the transform must come
from the CE itself — its `transf` field is one of the unpinned CE offsets.
**Seed**: `make.buildProposal` from Lua needs `params.seed`
(`script-construction-proposals-need-seed`); the UI's proposal carries the
seed inside the CE params, so it ships with them. **Name**: goes in
`ce.name` (recipe item 9, SLICE_STATUS "CONX -- VERIFIED AGAIN"); the UI
names the construction in the proposal too, so it is in the CE.

### 3.3 The blocker: ConstructionEntity params

`params` (year, paramX/Y, modules map, seed, …) become
`getEntity(id).params` after apply, so they are carried in the CE record.
What is measured: the `params.modules` map is consumed by `MakeProposalAdd`
`0xa18ca0` into geometry and per-edge tags before the command exists
(construction-height memory note; M8 probe), and no `map<int, ModuleInfo>`
was found inline in `a3[0..0x800)`. What is **not** measured: whether the CE
record's params sub-object (a native param map behind pointers) is reachable
from the toAdd element. `ChaseStrings` did reach module model paths three
hops down (`PATH a3+3a8+010 -> …station_3_main_end_l.mdl`, COMMAND_ARGS.md),
which is evidence the tree is there.

Measurement that settles it (uses only existing tooling):

1. `groundtruth=1`; from Lua build a `SimpleProposal.ConstructionEntity` whose
   `params` contain a sentinel string key `GTSENT_<n>` and a sentinel numeric
   (`paramX = 900000+n`) plus a real modules map; call `api.cmd.make.buildProposal`
   only (no send). `GroundTruthSample` (:1050) → construction branch →
   `ChaseStrings` (:880) reports the offset path that reaches `GTSENT_<n>`;
   two samples by the same path = the params root and its element layout.
2. Read the decompiles already on disk: `decomp_ce/UI_UpdateConstruction_params.c`
   (`0x41c3e0`), `params_extract_264680.c` (`0x264680`),
   `CE_fill_from_construction.c` (`0x3e6900`), `scripting_Convert.c`
   (`0x20e72f0`, the sol2 → CE conversion — its writes into the CE ARE the
   layout).
3. Serialise the param map in C++ as the same `ser()` string the Lua uses
   today (`deserParams`, lockstep.lua:4491), so `execConX` is unchanged.

Until step 3 lands, constructions cannot be truly cancelled without
violating `never-cancel-on-a-failed-decode` (a wrong params decode rebuilds
the wrong station on every instance). This is the single largest RE item in
the plan.

### 3.4 The true-cancel design once params decode

**(a) Cancel point.** Construction branch (:2254): decode nodes/edges/
removals (exists), toRemove ids (verify +0x1e0), CE fields (file, params,
transf-or-position, name, player). Write `ARMED 1`, `ROADC` (exists) and a
new `CONE <file> <params%> <transf16> <name%> <nRemove> <id..>` line, arm
`g_pendingCmd` with `g_pendingNoCb = 0` (the ConstructionBuilder waits on its
callback like the StreetBuilder; fire it — same callback shape, road path
proves the fire). Failed decode → do not cancel, log "stays local" as the
road path does (:2492-2505).

**(b) Wire.** `CONX` as today (`shipConxPair`, :7041) but built from the
hook's `CONE` + `ROADC` instead of the applied entity; `survivors` are no
longer needed — the originator now runs the same survivor-diff replay as the
peers, and the UI's toRemove list (resolved to positions by the Lua while
the buildings still exist) replaces it exactly: every instance demolishes
precisely those buildings. `cost`/`bal` snapshots (:7058-7063) go away — the
charge happens once, at the stamp, everywhere.

**(c) Lua.** `execConX`: delete phase 1 (:4507-4577), `strictBal*` (:4541,
:4580-4583), `strictHealsSplit` (:4531), the self-rebuild `ignoreErrors=false`
special case (:4996, af8f67b) and the CONFAIL exemption for `origin ==
K.INSTANCE` (02b42fc): the originator is now a peer. Gate on
`K.STRICT_OPS.CONX and armed == 1`; `conx_strict` is retired.
`pollNewConstructions` keeps running as the tripwire ("a player construction
appeared with no CONE") and to prime saves.

**(d) UI.** The construction tool's callback is fired (`success` byte still
0): expect the "cannot build here" red flash / sound once per placement (the
SetLine false-toast class, `nopath-toast-is-our-setline-cancel`). Follow-up:
locate the success-byte test in `ConstructionBuilder::MousePressed`'s lambda
(the same decompile route as `depotui/buy_cb_body.c`) and either set the byte
before firing or fire nothing if the tool tolerates NoCb (measure: place two
stations in a row with NoCb; if the second capture never appears, the tool
wedged and the callback is required).

**(e) Determinism.** All instances run `construction_builder_util::Apply`
with identical inputs: identical z/grade (the construction-height desync),
identical demolish set, identical cost. The native build's road re-grade
(SLICE_STATUS open item (b)) disappears because there is no native build.
Residual: the split-node "0.01 m octree" collision (39b22f7) cannot occur —
the native split is never made.

**(f)** `strict_conx` default ON once (a) exists. Test: `station_edit.txt`,
a depot on a mid-span split, a depot on a junction node
(`depot-endpoint-weld-merge`), a station over buildings; A5, A8
(`egeo_*.txt` byte-identical), the town-building lane (76026fd).

---

## 4. The poll-detected actions — hook or keep the poll?

| action | today | plan | true cancel possible? |
|---|---|---|---|
| construction demolish | `pollConstructionRemovals` (:8334), `DEMOLISH`, originator skips (:3115) | hook exists (bulldoze caller :2200); ship `CDEMO` ids; verify `+0x1e0` | **yes** — §2.9; RVA `0x9dc750` caller `0x3eb227` |
| module edit | `scanConstructionEdits` (:8381), `CONU`, originator skips (:2802) | hook exists (UNREPLICATED branch); needs CE params decode | **yes after §3.3**; until then delete-and-replay interim (§2.12) |
| roadside stops | `CM.pollStops` (:7388), `STOPADD/DEL/REP` with `skipOrigin=1` | hook exists; needs edge-object record decode | **yes after the sweep** in §2.10 |
| signals/waypoints | not replicated | same hook and decode as stops, side 2 | yes after the sweep |
| loan | `CM.pollLoan` (:7353) | hook `Book` `0x9dc5e0`; JournalEntry sweep | **yes** — §2.8 |
| construction placement | `pollNewConstructions` + `conx_strict` delete-and-replay | true cancel after §3.3 | yes after §3.3 |

Where the poll is kept in the interim, the originator's native action is
*not* cancelled — there is no honest way to call that strict. The
delete-and-replay pattern (bulldoze then rebuild at the stamp) is the only
Lua-only approximation; its collateral history (§3.1) is the argument for
finishing the decode instead of extending the pattern to modules and stops.

---

## 5. Remove the exceptions

Every place the DLL refuses or degrades, and what replaces it:

| where | today | replacement |
|---|---|---|
| :2113-2118 + :2123-2143 | `cancel` only for id 10 (`cancel_vehicle`) and id 6 (`cancel_line`); "cancel_line must NEVER arm for CreateLine (7) or UpdateLine (8), regardless of cfg" | `cancel = CfgHas(flagFor(id))` for every id in `FACTORIES[]`; 7/8 gated by `strict_line_create` / `strict_line_edit` once their capture decodes the Line (§2.6-2.7) |
| :2144-2163 | BuyVehicle: "NOT cancellable … NOT arming a cancel we cannot honour" | delete; §2.3 impl-pointer fix + `+0x38 = -1`, or NoCb + `ZeroAddResult` |
| :1662 `waitsForResult = (f.id == 2)` | only the buy fires its callback | `waitsForResult = (id == 2 || id == 4)`; everything else NoCb |
| :2075-2094 | callback not fired → "letting the build run" (unless NoCb) | **delete the let-it-run branch**: suppress anyway, log `callback NOT fired -- suppressed regardless (strict)`. `ARMED` becomes truthful by construction (§1.4) |
| :2058-2064 | `vft = *(r9)` (impl assumed inline) | `impl = *(r9+0x38)`; `impl == 0` → nothing to fire; else `vft = *impl` |
| :2492-2494 `!et.ok` | type decode failed → stays local (L) | keep the rule (never cancel on a failed decode) but write `ARMED 0` **and** a `DESYNC`-class log line; the soak's A10 must fail on it. Reduce occurrences by finishing the decoders, not by cancelling blind |
| :2495-2505 upgrade `re < 1` / `re < m` | stays local | same as above; the `rmEdges[512]` cap (:2462-2465) is already sized so `re < m` should not occur for a real upgrade — if it does, it is a new shape to decode |
| :2254-2286 construction placement | "let it PROCEED untouched -- never cancelled" | §3.4 |
| :1288-1290 construction-demolish shape | classify only | §2.9 |
| :1286-1288 UPGRADE-shaped | "left to the con poll" | §2.12 |
| :2288-2380 UNREPLICATED callers | log only | §2.10, §2.13, §2.14 — each caller becomes a named constant with a decode |
| :1028 `CfgHas` no-cfg defaults | only `cancel_vehicle`, `merge` default on | every `strict_*` flag defaults ON; `suppress` stays the master (ReadCfg :219 already defaults it ON with no cfg) |
| lockstep.lua:4094 | `not K.STRICT_OPS[c.op] or armed == 0` → skip | keep the `armed` half (it is the no-session case, :8587 `soloDrop`); every op is in STRICT_OPS |
| :4296 `VBUY`, :4394 `VREPL`, :3994 `execLine`, :3115 `execDemolish`, :4480 `execConX` (`conx_strict`), :5719 `execLoan`, :2744 `execConP`, :2802 `execConU`, :4055/:4073 skipOrigin, :8139 `stopRun` skipOrigin, :1750 `ROADP` skipOrigin | per-op originator skips | one helper `CM.originatorMustReplay(c)` = `c.origin ~= K.INSTANCE or (K.STRICT_OPS[c.op] and tonumber(c.armed or 0) == 1)`; `skipOrigin` is written by no reader except the no-session ROADE case (:8909) |
| capture readers :3510, :3537, :9089, :9226, :9230, :9250, :9281, :9293, :7465, :7482, :7501 | `skipOrigin = 1` | `armed = CM.lastArmed or 0` |
| :7262 `K.STRICT_OPS.VBUY = cfgFlag("strict_buy", false)` | off | `true`; and STRICT_OPS becomes a full table built from cfg at :7262 (after `cfgFlag` exists) |

---

## 6. Ordering, flags and rollback

Each commit is independently revertable: the new path is behind its flag,
the old path stays until the soak proves the new one, then the old path is
deleted in a separate commit.

| # | commit | flag (default) | proves it |
|---|---|---|---|
| 0 | Add hook: read the tag (`*(*(r8)+0xb18)`) and the Lua-bridge caller `0x1126f1a` into the log; log `*(r9)`, `*(r9+0x38)` on every armed match. No behaviour change. | — | one road build + one buy: tag 15/13, impl pointer equal/heap as predicted (§2.3) |
| 1 | "an armed cancel is always honoured": delete :2090-2093; impl-pointer fix | `suppress` | road/rail/upgrade/bulldoze soak unchanged; A1, A5 |
| 2 | strict sell + send-to-depot | `strict_sell`, `strict_depot` (ON) | A5, A7, A13, **A14** |
| 3 | vehicle flags: stop/start, maintenance, manual, depart (xmm3 spill in the relay) | `strict_vehflags` (ON) | A5, A13, A14, VPOS 0.00 |
| 4 | strict buy (NoCb + ZeroAddResult first; fire path if the window needs it) | `strict_buy` (ON) | `buy_vehicle.txt`, 5-vehicle batch; A1, A5, A14, `v` equal |
| 5 | strict replace | `strict_replace` (ON) | manual replace; A1, A5 |
| 6 | strict name/colour | `strict_cosmetic` (ON) | A13 |
| 7 | Line decode at the factory + strict update/delete | `strict_line_edit` (ON) | `create_line.txt`, `line_and_station.txt`; A5, A13 |
| 8 | Book hook + strict loan | `strict_loan` (ON) | soak LOAN as a click; A7 |
| 9 | construction demolish via CDEMO (after the +0x1e0 check) | `strict_condemo` (ON) | `station_edit.txt`; A5, A7, `n:` lane |
| 10 | edge-object decode + strict stops/signals | `strict_stops` (ON) | manual; A5, A10, A13 |
| 11 | CE params decode + true construction cancel | `strict_conx` (ON; retires `conx_strict`) | station/depot matrix; A5, A8, town-building lane |
| 12 | module edits on the same decode | `strict_module` (ON) | `station_edit.txt`; A5 |
| 13 | in-place node edits | `strict_nodeedit` (ON) | manual double slip; A8 |
| 14 | strict line create | `strict_line_create` (ON) | `create_line.txt`; A1 (no UpdateLine(-1) assert) |
| 15 | terraform (after the payload is found) | `strict_terrain` | manual; A8 |

`tools\soak.ps1` additions (assertions are declared in its header, A1-A13):

- **A14 strict parity**: per instance, `count("ARMED 1")` in
  `lockstep_inject_<x>.txt` equals `count("STRICT -- originator replaying")`
  in its Lua log; any `callback NOT fired` line without a matching
  `suppressed regardless` fails.
- **A15 no local-only actions**: zero `stays local`, `NOT replicated`,
  `UNREPLICATED BuildProposal` lines in `tpf2_slice.log` after commit 10.
- The existing A13 (every action executed by every OTHER instance) becomes
  "by every instance", originator included.

Rollback per step is the flag; rollback of step 1 is `git revert` because it
changes the shared Add path.

---

## 7. Open questions / not established

Each with the measurement that settles it.

1. **`std::function` impl at `r9+0x38` at Add.** Log `*(r9)`, `*(r9+0x38)`,
   `*(*(r9+0x38))` for one road build and one buy. Expect: road `*(r9+0x38) ==
   r9`; buy: heap pointer whose vftable slot 2 == `g_base+0x753820`.
2. **`payload+0x38` for a cancelled BuyVehicle.** Log it at Add for tag 13.
   If not -1, write -1 before firing. Same for ReplaceVehicle (tag 14).
3. **Did the 2026-08-28 buy crash come from the stale out-handle?** Re-run the
   NoCb suppress with `ZeroAddResult` present; if the depot window survives
   five buys, the "window waits" theory is retired.
4. **Command tag base.** Log `*(uint8*)(*(uint64*)r8 + 0xb18)` on every armed
   match; expect 15 (BuildProposal) for road/upgrade/bulldoze, 13 buy, 7
   reverse, 12 sell, 14 replace, 28 colour, 31 book.
5. **`toRemove` at `r8+0x1e0`.** `dumpprop=1`, bulldoze one depot; the vector
   must hold exactly one id == the depot's construction entity.
6. **ConstructionEntity params layout.** The sentinel sweep in §3.3 step 1,
   read against `decomp_ce/scripting_Convert.c` writes.
7. **Edge-object record layout** (`+0xe0/+0xf8`, 0x100 B): factory-only sweep
   with sentinel `u`/model/side from Lua `edgeObjectsToAdd`.
8. **JournalEntry layout** for `Book`: sweep `bookJournalEntry` with sentinel
   amounts through the new factory hook (`GtFactoryDumps` case 31).
9. **Caller RVAs** for stop tool, TrackModifier, ModuleBuilder, addmodulecomp,
   terraform: already printed by the `UNREPLICATED BuildProposal from
   caller_rva=` line (:2319); perform each action once with the DLL attached.
10. **Double-slip / bridge-type bit**: read the `rmNode[i]`/`addNode[i]`
    flags/type differential (:2340-2380) after one toggle each.
11. **Terraform payload**: `DumpVectors` over `r9` on a raise and a lower;
    find the height patch.
12. **Does `ConstructionBuilder::MousePressed`'s callback wedge the tool if
    not fired**, and does firing it with success=0 flash an error? Two
    placements with NoCb, then two with fire.
13. **`component::Line` stops vector offset** (+0x00 per `GtDumpLine`
    vs "+0x18" in one COMMAND_ARGS.md sentence): read `[gt] f8stopspan` from
    an existing `GT line` sweep log.
14. **Line editor after a cancelled CreateLine**: does it issue
    `UpdateLine(-1)` immediately, or only on the next edit? Decides whether
    the DLL's `r8 == -1` swallow is enough or the editor must be closed.
15. **Industries / map-editor commands**: not in scope; `CreateTowns`,
    `RemoveTown`, `ReplaceTerrain`, `ConnectTownsAndIndustries` are "gated"
    rows in COMMAND_MAP.md and stay off.
