# Hot join without a host reload: node-list order

Investigation 2026-09-22, build 35924 (Windows exe and the native Linux ELF,
GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`).

## The question

A hot join today is a recovery round in which **every** member, the host
included, loads the snapshot (`sync_runtime.py`, "THE HOST LOADS TOO"). Skipping
the host's load was tried on 2026-09-16 and again on 2026-09-22
(`linux-load-perf:docs/re/linux/RETAINED_WORLD_JOIN.md`): the paused
fingerprints matched, the main hash kept matching, and the people count split
~100 game units later. What does a running world hold that its own save does
not reproduce?

## Measurement

Private native lab pair on the VPS (`/opt/tpf2mp-linux-parity-20260921`, see
[Lab harness](#lab-harness)): host `lab` keeps its world at the join
(`TPF2MP_EXPERIMENT_RETAIN_HOST=1`), `peer` loads the snapshot. A Lua probe
(EVAL) dumps every SIM_PERSON with `game.interface.getEntity` at each whole game
unit on both sides: destinations, target, move modes, speed, travel times,
reachability.

| run | host | first differing person | paired dumps equal |
|---|---|---|---|
| control (host reloads too) | reloads | none | 25 / 25 |
| retained, vanilla | keeps its world | **2-5 game units after release** | 2 / 25 |
| retained, with the sorts below (3 sites) | keeps its world | none | 59 / 59 over ~290 game units, 55 / 55 hash stamps |
| busy world (the dedicated server's save: 10 ships, ~130 cargo, ~2,800 people out), host alone 5 min first, live-join lobby path, 4 sites | keeps its world | ~120 units: two people swap a destination slot at one building (capacity maps, below) | 23 hash stamps equal, persons/destinations differ from +120 |
| busy world, + capacity maps relinked | keeps its world | ~280 units: one new cargo gets another recycled id; a town building's id and residents follow (freed ids, below) | 72 hash stamps equal, 174/175 dumps |
| busy world, + freed-id batches sorted | keeps its world | see the run log | |

What differs first in the vanilla run is a person's **choice of destination**
(`destinations[2]` or `[3]`, and the move mode with it), everything else equal;
a few units later different people are out travelling (the `n:` lane counts only
travelling people, which is why the count split showed so late). Entity ids are
NOT the cause: new people appear with identical ids on both sides (the free-id
deque is saved, `Engine::Load` 0x23df8f0).

## Mechanism

ECS node lists (`NodeList<N>::Add` 0x21d660 push_back, `Remove` 0x241030
swap-with-last; component-group family node vectors behave the same) are not
saved. After a load, 0x23dc1d0 collects every live entity, topologically sorts
them (0x23e1350) and fires `EntityAdded` iterating the result backwards: mostly
descending id order. A running world holds its lists in add / swap-remove
history. Same contents, different order. The person sim consumes three batches
in that order:

1. **candidates** -- `destination_util::GetTargetsByLandUse` 0x9279f0 copies the
   PersonCapacity family node list into a local `vector<Entity>`;
   `PickTarget` 0x928370 accumulates free capacity over it in that order, draws
   one `r` and binary-searches. Same `r`, another order: another building.
2. **departures** -- `SimEntityAtBuildingSystem::Update2` 0xa7c920 collects the
   people whose stay ran out in node order and signals
   `SimPersonSystem::NoteAtBuildingPersonsLeave` 0xa93610, which draws "recompute
   the destination?" and stay durations from one time-seeded mt19937 in batch
   order, then seeds the destination batch (0x927877).
3. **arrivals** -- `PersonMoveSystem::Update2` 0xa59450 collects walk arrivals for
   `NoteWalkPersonsArrived` 0xa97820 (one tag-3 mt19937, stay durations
   U(5,300) in batch order).
4. **idle** -- `SimEntityIdleSystem::Update` 0xa86760 keeps its pending list
   (system+0x18) in insertion order; `PathFactory::Compute` 0x90f320 seeds one
   mt19937 per chunk (time + chunk start) and draws per item, so each trip's path
   and mode depend on the person's place in that list. Not seen diverging in the
   lab (3-site runs stayed equal), sorted anyway: it runs for every trip.

## The fix

Sort each batch ascending by entity id just before the engine reads it. Every
peer of a session must run it (a draw lands on another building than vanilla's);
the lobby's exact version gate guarantees that.

| batch | Windows site (steal) | vector | native site (steal) | vector |
|---|---|---|---|---|
| candidates | 0x927df6 `c7 45 87 01 00 00 00` | `[rbp-0x71]` | 0x1502918 `48 c7 03 00 00 00 00` | `[rbp-0x90]` |
| departures | 0xa7c9fd `48 8d 54 24 28` | `[rsp+0x28]` | 0x16f0bcb `48 8b 7a 08 48 85 ff` | `[rbp-0x50]` |
| arrivals | 0xa59928 `48 8d 54 24 68` | `[rsp+0x68]` | 0x16b5dc6 `48 8b 78 08 48 85 ff` | `[rbp-0x88]` |
| idle | 0xa867ce `49 8b 55 20 49 2b 55 18` | `[r13+0x18]` (the system's own list, sorted in place) | 0x17005cc `48 8b 8d 80 fe ff ff` | `[[rbp-0x180]+0x18]` |
| capacity maps | 0x21234de `49 8b bd 20 01 00 00` | 9 MSVC lists relinked (D = `[r13+0x120]`) | 0x2e6e6c3 `48 8b 85 28 f4 ff ff` | 9 libstdc++ lists relinked (D = `[[rbp-0xbd8]+0x120]`) |
| freed ids | 0x23de385 `49 8b 04 24 48 8b 50 08` | `[[r12]]` (engine+0x200) | 0x3256930 `49 8b 85 08 02 00 00` | `[[r13+0x208]]` |

5. **capacity maps** -- every construction build, replace or demolish (town
   growth included) runs `SimEntityUpdateHelper`: the affected people and cargo
   go into 5 + 4 temporary `unordered_map`s, and its destructor 0x2122fd0 seeds
   one mt19937 with 5489 while `ApplySimPersonData` 0x2125a90 / `ApplySimCargo`
   0x2124030 walk the maps in list order (stay draws, freed ids). The lists are
   relinked in ascending key order after the seed; nothing reads the buckets
   again (the maps are walked head to tail and destroyed by walking the ring).
   On Linux `person_map_order_linux.cpp` redirects the person walks to Windows
   order: [`hotjoin/person_map_order_canonical.patch`](hotjoin/person_map_order_canonical.patch)
   makes it ascending.
6. **freed ids** -- `Engine::EndModification` 0x23de130 appends each batch of
   removed ids to the FIFO free-id deque in removal order; `AddEntity` 0x23dca30
   pops the front. Each batch is sorted before the append, so the deque depends
   only on which ids each batch removed (identical in lockstep); the replicated
   second engine replays the same removals through its own EndModification.

- Windows: `native/src/slice/hotjoin_order.inl` + `native/src/hotjoinrelay_slice.asm`,
  verified by `tools/hotjoin_order_bytes_test.py`. Kill switch `hotjoinorder=0`.
- Native Linux: [`hotjoin/order_canon_linux.cpp`](hotjoin/order_canon_linux.cpp)
  / `.h`, a boot-library module in the style of `target_order_linux.cpp`
  (installed from `boot.cpp` after the person-order modules; add the .cpp to
  `tpf2mp_boot`'s sources). Kill switch `TPF2MP_ORDER_CANON=0`. This is what
  the lab measured.

### Linux default: ON (required since 0.7, the owner's call, 2026-09-22)

The sorts change what the simulation decides, so every peer of a session must
run the same set: a Windows player (sorts always on unless `hotjoinorder=0`) and
a Linux peer with the sorts off decide apart and desync, hot join or not. The
native port shipped them opt-in (`TPF2MP_ORDER_CANON=1`, "pending live
validation"); the 0.7 dedicated server only matches Windows because its
server.env sets that variable. The Linux build must default ON, exactly as the
reference copy [`hotjoin/order_canon_linux.cpp`](hotjoin/order_canon_linux.cpp)
does:

- `native/linux/src/order_canon_linux.cpp`, the install gate: unset (or any value
  but `0`) = on; `TPF2MP_ORDER_CANON=0` = off, status
  `"off (TPF2MP_ORDER_CANON=0)"`.
- `native/linux/src/person_map_order_linux.cpp`, `CanonicalMaps()`: the same rule
  (on unless the variable is `0`), so the capacity-map walk and the order sorts
  are never split.
- The docs that call it an experimental opt-in (`docs/linux/INSTALL.md`, the
  UPSTREAM notes going forward) say it is on by default, `=0` turns it off, and
  that it must match the Windows players.

### Linux `Readable()` costs a /proc/self/maps parse: 32% of the sim thread (measured 2026-09-22)

The native dedicated server, profiled with `perf -t` on its simulation thread
while a two-player session ran (49,000-tile world, 1x, pager budget 6 GiB), spent
**a third of that thread reading and parsing `/proc/self/maps`**:

```
6.07% [k] mangle_path      <- seq_path/show_map_vma/show_map/seq_read/vfs_read/read
4.76% [.] __vfscanf_internal   <- _IO_fgets <- (anonymous namespace)::FamilyMappings()
3.26% [k] strchr    3.25% [k] seq_put_hex_ll   1.79% [k] seq_putc
1.74% [k] show_map_vma   1.65% [k] lock_next_vma   1.47% [k] show_vma_header_prefix
2.00% [.] __GI_____strtoull_l_internal ...          (~24% kernel + ~8% libc in total)
3.52% [.] Tpf2mpOrderCanonDispatch   3.45% TargetErase   2.95% TargetInsert
```

`libtpf2mp_boot.so`'s `FamilyMappings()` opened `/proc/self/maps` 5.6 times a
second -- once per simulation batch, for `family_canon.h`'s two `Readable()`
calls -- and each parse walked **50,000 mappings** (48,000 of them lavapipe's
per-frame memfd buffers, see `docs/DEDICATED_SERVER.md`), about 4.5 MB of
kernel-generated text at roughly 57 ms a time. On Windows the same contract is
one `VirtualQuery`, about a microsecond (`native/src/slice_hook.cpp`).

FOR THE LINUX PORT -- `Readable(p, n)` must be O(1). It is called from the sim
thread and from every slice validation (`station_weld.h`, `trainorder.h`,
`moveorder.h`, `roadspace.h`, `family_canon.h`), not only once a batch:

- Cheapest correct probe, no enumeration at all: `process_vm_readv` on self for
  the first and last byte of the range (the port already links it) -- `EFAULT`
  means "not mapped", which is exactly what `Readable` answers. `msync(addr,
  len, MS_ASYNC)` returning `ENOMEM` says the same thing in one syscall.
- If a mapping table is kept, parse it once into a sorted array of
  begin/end/prot and re-parse only when a lookup misses: the engine's heap
  ranges are stable for a whole session, and a miss is the only event that can
  mean "a new mapping appeared".
- Read the file with `read()` into one buffer and scan it; never `fgets` plus
  `sscanf` per line, which is the 8% of libc above (glibc's `%x` conversions
  dominate it).
- Whatever the implementation, it must not allocate or take a lock the render
  thread holds: it runs inside the sim step.

That one change should give the server's simulation about 1.4x the headroom it
has now. The order sorts themselves (dispatch plus target insert/erase, ~10% of
the thread) are the next item down and are inherent to the canon.

### The person target-set order costs 20% of the sim thread (measured 2026-09-23)

The native dedicated server, one player in, the session voting 3x on the
49,000-tile world: it delivered **2.02x** (its clock advanced 2.02 units a second;
one unit is one second at 1x), and the engine agreed -- `tpf2_engine_pace.txt`
read `base=380000`, i.e. it needed 380 ms batches where the nominal is 200, and
3 x (200/380) = 1.58. Nothing was saturated: 1.7 of 8 cores busy, the frame pacer
at exactly 30 sleeps a second (good for 6x), `memory.pressure` 0.00. The limit is
how long one batch takes, and a 15 s `perf` of the simulation thread says where
that goes:

```
38.8% [kernel]   25.8% TransportFever2   20.9% libtpf2mp_boot.so   11.2% libc
10.43% [.] (anonymous namespace)::TargetInsert(unsigned int const*, int const*, void*)
 9.90% [.] (anonymous namespace)::TargetErase(unsigned int const*, int const*, void*)
 2.82% [.] _int_malloc      2.19% [.] process_vm_readv      1.09% [.] malloc_consolidate
 1.88% [k] mt_find   1.39% [k] mod_node_page_state   1.28% [k] __radix_tree_lookup
```

**FOR THE LINUX PORT: `TargetInsert` and `TargetErase` are 20.3% of the
simulation thread** -- the boot module that gives the person target sets Windows'
order ("Windows person target set order: enabled" in the boot log). The world had
2,258 sim persons at the time, and the pair is called per person per step, so an
O(n) scan inside either one is O(n^2) a step. Suggestions, cheapest first:

- If a position in the Windows order is found by scanning a vector, add an index
  (key -> slot) and keep it with the vector: insert and erase become O(1) plus
  the memmove, and `_int_malloc`/`malloc_consolidate` above (4%) suggests a
  container that also reallocates per operation -- reserve it once.
- If the order is only ever *read* in bulk (a traversal at the end of the step),
  do not maintain it per operation at all: keep the engine's own container and
  produce the Windows order once, where it is read, as
  `family_canon.h` was changed to rebuild only the tail after the first displaced
  node (dev b446547).
- Time it in the lab the way the rest of the canon is timed, and put the cost in
  the boot log so the next profile does not have to be a `perf` run on the live
  server.

**The slow patches are the same two functions.** The server's clock does not run
evenly: in a 120 s window it advanced at 2.02 units a second overall but twice
crawled for ~5 s. A capture triggered on the stall itself (watch the dash at
20 Hz, profile the moment the clock has not moved for 1.6 s) caught two of them,
and both read the same:

```
9.70% TargetInsert  8.93% TargetErase  5.31% exe+0xa6139c  4.18% _int_malloc
10.72% TargetErase  10.52% TargetInsert  3.24% exe+0xa6139c  2.61% process_vm_readv
```

So there is no separate hitch to chase: the clock slows when the person target
churn spikes, and the same pair dominates. The libc share beside them (4-6%
`_int_malloc`, `__libc_malloc2`, `malloc_consolidate`) most likely belongs to the
same containers, so a fix that stops reallocating per operation takes that too:
about 25-27% of the thread in total.

The other measured cost on that thread, for scale: the `Readable` probe's
`process_vm_readv` plus its kernel iovec and radix paths is ~5%, and it replaced a
`/proc/self/maps` parse that was 32%, so it is already the cheap version. The
mod's own world hash is NOT a factor at the live cadence: the leader had stamped
`HASHEVERY every=576` with `hc=3089` in the heartbeat, which is 3.1 s every 576
game units -- about 1% of wall time at 2x and 2% at 4x. (The 84-unit interval the
log prints at load is only the starting value, before the cost ladder lands.)

## Not an order bug: the render clock stepped back (speed hook)

The retained host of the busy-world run with the freed-id sort asserted
`ShipFoamRenderer.cpp:137 startAge >= 0` ~210 units after the join (all dumps
and hash stamps equal until then). A ship wake's first point was later than the
render time: the interpolated clock `prev + alpha * (cur - prev)` had stepped
back. Vanilla keeps `guiFrameTime` constant inside a batch (`CGame::Sync` writes
it once per batch, and `CGame::Step` computes `alpha = (totalTime - lastSync) /
guiFrameTime` right after); `speedhook.cpp` re-imposed its interval (the
dedicated server's 200 ms pin, fractional speed) at the NEXT frame's Step entry,
so each batch's first frame used the engine's estimate and the rest ours. Fix:
the override is imposed right after Sync returns (Windows: Step's call at
0x118ee6 redirected to `SyncWrap`; native: `CGame::Sync` 0xa30cc0 hooked at its
entry, [`hotjoin/speedhook_batch_boundary.patch`](hotjoin/speedhook_batch_boundary.patch))
and nowhere else. Any dedicated server or fractional-speed session with ships
could hit it; entity ids and the order sorts cannot (ship traces are keyed by id
AND revision).

## Live join in the lobby

`sync_operation.SyncOperation` carries `retain`: in a `join` round the host and
everyone already playing keep the world they are paused in; only the newcomers
load (`sync_runtime`: a kept member pauses its world -- only the one the round
found, held, paused, engine idle -- and acks `kept=True`). The paused
fingerprints still decide: a difference empties `retain` and repeats `loading`
once under a fresh epoch, i.e. the frozen join everyone knows; a difference after
that is the ordinary error. A retry is always the plain round. Windows 0.7 enables live join by default;
`tpf2mp_live_join.txt` = `0` turns it off. Native Linux keeps explicit opt-in
(`1`, `on`, `yes`) until the canonical-order lifetime checks below pass.
Read at each join.

Nobody waits for members still loading (0.7, `pacing.lua` load gate): the roster
hold is gone (`loadgate_roster=1` in tpf2_slice.cfg brings it back locally). A
joiner still waits for the leader and the command history since its save.

**The bridge's world id must reach it on every platform.** Each game's bridge
drops datagrams from another world (`other-world=` in tpf2_bridge.log); its world
id is the lobby nonce, which the menu writes into `tpf2_bridge_ctl.txt` as
`lobby=<32 hex>` when the lobby emits `transport_lobby`. On the native Linux
dedicated server (0.7-native, 2026-09-22) the ctl file had no `lobby=` line, so its
bridge stayed in world `00000000`: a live joiner's game and the server dropped each
other's frames and both held (joiner: "the leader (a) has not been heard"). Writing
the line by hand joined them at once. The Linux menu (`native/linux/`) now writes
`lobby=` like `native/src/menu_hook.cpp` (the `bridge ctl` writer), also
when the `transport_lobby` event arrived before the menu first wrote the file.
The nonce is retained in the session model and cleared for a new session; see
[the native integration](../linux/UPSTREAM_dev_0a35d0a8.md). Tests: `tools/test_sync_operation.py`,
`tools/test_sync_runtime.py` (live-join cases).

## Every family's node list, in entity order at every sim iteration

The node-list consumers the batch sorts above do not reach (vehicles claiming
terminals, industries, stock lists, ship/aircraft reservations, animal chunk
seeds, scaffolds, the town stagger) all read an ECS family's node vector in its
order. A live-joined pair on the user's machine (2026-09-22) matched every hash
from t=36 to 720 and then split its town streets at 724 (e lane only: same edge
count, same heights, other endpoints; no player command): the town stagger.

Fix (Windows, site `step` in `hotjoin_order.inl`): at the entry of
`Engine::Update` (0x23e1850 `40 57 41 54 41 57`, rcx = the engine), every
family's node list is brought to ascending entity order and its entity->position
index rewritten, before any system runs. Engine::Update has one caller,
`GameSim::Step`'s iteration loop (0x15abbb), so this is once per sim iteration
on each of the peer's two engines -- NOT once per Step: Step runs `speed`
iterations, and a peer catching up runs at another speed than the host.
The order is then a pure function of the entity set at every iteration, on both
engines of every peer, whatever history they have.

- Families: Windows MSVC `unordered_map<type_index, IFamily*>` at engine+0x148
  (list node {next, prev, type_info*, IFamily*}, size at +0x150); native
  libstdc++ hashtable at engine+0x160 (first node `*(engine+0x170)`, node
  {next, type_info*, IFamily*}). GetNodeList: Windows vtable slot 1 (0xba990
  `lea rax,[rcx+8]`, 0xbdff0 = none); native slot 2 (0xa914c0). N from the node
  list's vtable (Windows 0x2f47a38 + 0x10*(N-1), native 0x59ac260 +
  0x20*(N-1)); node stride 4 + 4N.
- Node list: +8/+0x10/+0x18 vector; +0x20 phmap entity->position (ctrl +0,
  slots +8 {int32 entity, int32 pos}, size +0x10, capacity +0x18). Read only by
  the five `NodeList<N>::Remove` functions; systems hold the node vector only
  during their own call.
- `native/src/family_canon.h` (pure, `tools/family_canon_test.cpp`): a sorted
  list costs one scan; a disturbed one (Remove swaps the last node into the
  hole) is repaired in O(n) -- the displaced nodes are pulled out, sorted and
  merged back -- and each full index slot is rewritten through the old->new
  position map after checking that every slot names its node. Anything else is
  refused and left untouched (logged).
- Native Linux: guarded implementation, enabled by default since dev ad3d66e4.
  Static/fixture evidence is in [linux/DEV_0115785C.md](linux/DEV_0115785C.md);
  loaded-game lifetime validation remains outstanding. Site: `Engine::Update` 0x32515b0 entry
  (`f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54`, rdi = the engine), same
  walk over the libstdc++ family map; family_canon.h is portable.

### Native: GCC keeps one GetNodeList per family (fixed 2026-09-23)

The native step canon recognised a family's node list by ONE getter address,
`0xa914c0` (`endbr64; lea rax,[rdi+8]; ret`), and a list-less family by
`0xa914e0`. That is the Windows design, where MSVC's identical-code folding
makes every `ComponentGroupFamily<...>::GetNodeList` one function (0xba990).
GCC does not fold them: build 35924 has **28** node-list getters and **35**
no-list getters, one per family template instantiation
(`tools/linux/gen_family_getters.py` lists them from the ELF's own vtables).
Only PersonCapacity used `0xa914c0` and only VehicleOrder `0xa914e0`, so every
native peer since 0.7 sorted 1 of its 28 node lists and refused the other 61
families -- Town, TownBuilding, Construction, BaseEdge, TownConnection,
SimBuilding, StockList, Station, the vehicle families -- while every Windows
peer sorted all 28. The production server logged it at every iteration:

```
[order-canon] step engine=... families=63/63 lists=1 reordered=0 moved=0 unknown/refused=61
```

(Windows: `63 families, 28 node lists ..., 0 not understood`.) The native
lists therefore stayed in load/history order while the Windows lists were in
entity order: the TownSystem staggers town `i` of its node list to
`t % 120 == (i % 30) * 4` and hands every town of a tick one mt19937 seeded
from `t`, so a list in another order develops towns at other ticks with other
draws (the native load order is not entity order: the one list it did sort,
PersonCapacity, needed ~5,300 nodes moved at the first iteration after every
load). Every native session since 0.7 split in town growth within one to
three hash stamps of each join (for example 390 and 475 game units after the
joiner's load on 2026-09-23), in several towns at once; the
0.6.1.28-native sessions, which had no step canon on either side, held
(1,800 units and 36 new street edges, 2026-09-22).

Fix: `native/linux/src/family_getters_linux.h`, generated from the ELF,
lists all 63 getters; install verifies each byte for byte; the canon
classifies a family by that table and still calls none of them.
`tests/family_canon_elf_test.cpp` (manual, needs the game ELF) maps the real
image, applies its relocations, gives an engine the binary's own 63 family
vtables with unsorted lists and runs one iteration: 28 lists sorted, 0
refused; with the old check it prints the server's `lists=1 ...
unknown/refused=61`. After deployment the server's log must read
`lists=28 ... unknown/refused=0`.

## Not covered yet

Order-sensitive consumers the lab world did not exercise (it has two vehicles
and no player activity between the host's load and the join), found by RE:

- **SimEntityAtTerminalSystem::Update** 0xa81810: waiting people sit in per-terminal,
  per-cargo deques -- arrival order while running, registration order after a
  load; one time-seeded mt19937 draws "give up waiting" per person in deque
  order, and boarding takes them in deque order up to capacity. Needs an arrival
  key to canonicalise, not a sort by id.
- Whatever else consumes a node list the same way in vehicle, cargo and industry
  systems; see the sections added below as they are measured.

The acceptance gate stays the one in `RETAINED_WORLD_JOIN.md`: a retained-host
join must match a loaded joiner past the previously observed failure interval,
with vehicles, cargo, construction and later joins exercised, before any
production join skips the host's load.

## Lab harness

On the VPS under `/opt/tpf2mp-linux-parity-20260921/hj-lab/` (sources in
[`hotjoin/lab/`](hotjoin/lab/)): `hj_run.py` restarts the private pair, waits for
the host world, joins the peer through the lobby, installs the person probe on
both, collects paired dumps and compares; `--control` makes the host reload too,
`--save NAME` picks the host's world, `--prejoin S` lets the host run alone
first. `hj_watch.py` follows a long run (hash lanes + dumps), `hj_long.py` /
`hj_compare.py` compare. Production (`tpf2mp-game`) is never touched.

Native integration: [dev 7cacbaaf](../linux/UPSTREAM_dev_7cacbaaf.md) replaces
family mapping snapshots with permission-aware PROCMAP_QUERY on Linux 6.11+;
older kernels keep a buffered snapshot fallback. Endpoint-only probes and
refresh-on-miss caches do not preserve the full range/write-permission contract.
