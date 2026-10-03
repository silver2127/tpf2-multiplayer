# Waiting at terminals, boarding and unloading: what order survives a load

Investigation 2026-09-27, build 35924 (Windows `TransportFever2.exe` SHA-256
`782b904a...175c`; native ELF build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`,
the local copy `C:\tools\tpf2_native\TransportFever2.elf`). Follows up the
"Not covered yet" item of [HOTJOIN_ORDER.md](HOTJOIN_ORDER.md).

Marks: **[D]** read from disassembly or decompilation of this build; **[G]** inferred and
not verified instruction by instruction.

## Result in one paragraph

The suspect named in HOTJOIN_ORDER.md is **not** where the divergence comes from. The
waiting queues in `SimEntityAtTerminalSystem` are not in arrival order while the game runs
and registration order after a load. They are kept **sorted by
`(SimEntityAtTerminal.arrivalTime, entity id)`** by a binary-search insert, and every field
of that key is saved. A running world and its reloaded copy therefore hold identical queues
[D]. The order that is **not** reproduced is in the containers next to them:

- **A.** `TransportVehicleSystem`'s list of vehicles standing AT_TERMINAL at a line stop.
  This is a `vector<Entity>` in append order, and `SimEntityAtTerminalSystem::Update`
  hands out waiting cargo and people to those vehicles in that order.
- **B.** `SimEntityAtVehicleSystem`'s per-(vehicle, stop, cargo) `deque<Entity>`. This is
  in boarding (push_back) order, and unloading takes entries from its front.

After a load, both are rebuilt in `EntityAdded` order. HOTJOIN_ORDER.md says that is the
engine's topological, mostly descending-id order. A running world holds them in
arrival or boarding history instead [D]. Container A matches tonight's symptoms directly:
when two or more trucks of a line stand at the same stop, the retained host and the joiner
give the waiting cargo to different trucks. The trucks leave with different loads at
different times and end up in different positions. The people, and later the entity ids,
follow from that. The fix is to sort both containers by entity id where the simulation
reads them (sites below).

## 1. The waiting queues (SimEntityAtTerminalSystem)

### Component `ecs::component::SimEntityAtTerminal` (0x20 bytes)

| off | type | name | evidence |
|---|---|---|---|
| +0x00 | Entity | `line` | [D] map key, Lua name list `line`, assertion `seat->line == lineEntity` |
| +0x04 | int | `lineStop0` | [D] map key (with `line`), string `seat.lineStop0 >= 0` |
| +0x08 | int | `lineStop1` | [D] |
| +0x10 | int64 | `arrivalTime` | [D] written by `SimCargoSimEntityAtTerminal` 0xa7bfa0 from its `__int64 arrivalTime` argument (assertion `place == -1 \|\| arrivalTime > 1`): 0 when the cargo got no place on the platform model, 1 when `GetEdge` flags the edge, otherwise the arrival time |
| +0x18 | Entity | `vehicle` | [D] -1 waiting, -2 gave up, otherwise the vehicle it was assigned to (`cc->vehicle == ecs::Entity(-1)` in Update) |

Saved: `ComponentManager::Load<SimEntityAtTerminal>` 0x192a80 streams the dense vector
through 0x1a1570, which reads int, int, int, int64 and int for each element: all five fields [D].

### System object (Windows offsets, `this` = system)

| off | what | evidence |
|---|---|---|
| +0x20 | `TransportVehicleSystem*` | [D] passed to 0xad5550 / `GetLineCargoInfo` 0xad54a0 |
| +0x28 | `SimEntityAtVehicleSystem*` | [D] passed to `GetVehicleSimEntitiesCount` 0xa85360 |
| +0x30 | int `m_numCargoTypes` | [D] assertions |
| +0x38 | `std::map<pair<Entity line,int stop>, vector<deque<Entity>>>` **`m_lineStop2cargo2simEntities`** (MSVC red-black tree, key at node+0x20, vector at node+0x28/0x30) | [D] assertion `it0 != m_lineStop2cargo2simEntities.end()` |
| +0x48 | `unordered_map<pair<Entity,int>, vector<int>>` per-cargo counts (`cargo2simEntityCount`) | [D] |
| +0x88..0x98 | component type indices (SimEntityAtTerminal, cargo-type helpers, TransportVehicle, Station) | [D] |

Each per-cargo container is an MSVC `std::deque<Entity>` (0x28 bytes: +0x08 map, +0x10
map size, +0x18 offset, +0x20 size, 4 entities per block; element `i` is
`map[((off+i)>>2) & (mapsize-1)][(off+i)&3]`) [D].

### Add, remove, rebuild

- **Add**: `SimEntityAtTerminalSystem::EntityAdded` 0xa80d60 (vtable 0x30d4f10 slot 3)
  finds or creates the map node, then calls 0xa80430. That function is a `lower_bound` over the
  deque with the key `(arrivalTime, entity)`, and it reads every element's component through
  `Engine` 0xc6040 (`comp.arrivalTime < key.time || (== && elem.id < key.id)`). 0xa82e00 then
  inserts at that position [D]. So the deque is always sorted ascending by `(arrivalTime, id)`.
- **Remove**: `EntityToBeRemoved` 0xa81110 calls 0xa7f310. That function runs the same
  lower_bound, checks for an exact match (`"erased"` assertion) and erases in place
  (0xd5b90). Empty map nodes are dropped [D].
- **Change**: the system has no ComponentAboutToBeChanged or ComponentChanged hooks (slots
  5-9 are the no-op 0x700e0) [D]. The only notified modification of the component is
  Update's (0xa806f0 / 0xa80940, called only from Update [D]), and it writes `vehicle`
  (+0x18) only. `NoteLineChanged` (cargo 0xa7a770, person 0xa94da0) re-keys a waiting entity
  with RemoveComponent + AddComponent and copies `arrivalTime` and `vehicle` unchanged [D].
  Nothing found writes `arrivalTime` in place [G: the check covered the notified paths and
  every function that references the type descriptor; an un-notified raw write through a
  mutable getter was not ruled out exhaustively].
- **After a load**: every entity with the component gets `EntityAdded`, in whatever order,
  and ends up at its sorted position. **The deque content and order are a pure function
  of saved component data [D]. This is not the divergence.**
- Native Linux: `EntityAdded` 0x16f5bb0 passes `(comp+0x10, entity)` to 0x16fac70 /
  0x16f95f0, which is the same halving search comparing `[comp+0x10]` then the id [D]. Vtable
  0x59c0c40 (Itanium slots 4/5/11 = add/remove/Update 0x16f6090).

People and cargo also hold a platform **place**: `SimPersonAtTerminal` has a bitset of
occupied places, and `GetRandomPlace` 0xa87890 draws among the free ones in index order.
`SimCargoAtTerminal` places are the first free slot (`GetPlace` 0xa77050). Both place
indices are stored in the (saved) component and re-registered by `EntityAdded`
(assertions `edgeInfo.places[scat.place].GetId() == -1`, `edgeInfo.places[spat.place] == false`) [D].
They are not order-dependent.

## 2. `SimEntityAtTerminalSystem::Update` 0xa81810: every consumer of order

The mt19937 is seeded once per call from the game time. It is the FNV-1a of the 4-byte time,
then `*0x1b3 + 0x979202d1 ^ 0x3ea6c361`, which is HashCombine with tag 13. The seed does not
depend on order, and Linux is corrected by `sim_seed_linux` (0x16f60ef) [D].

1. **Crowding pass** (map order, then cargo, then deque order): for each station group it
   counts entities with `arrivalTime == 0`, then subtracts `Station+0x40` (capacity). The
   result is a count, so order does not matter [D].
2. **Per line stop, per cargo type** (the map is a `std::map`, so line stops are walked in
   ascending key order [D]):
   - If the stop is not served for this cargo (the stop bit in the line's bitset,
     `GetLineCargoInfo` null, or its +0x6c == 0), every waiting entity, in deque order, draws
     `u = U[0,1)` (0x2374bf0). If `u <= 0.0025*dt`, it is queued with `vehicle = -1` for re-planning [D].
   - Otherwise, for each entity in deque order: if the station group is over
     capacity (counter >= 1) and `arrivalTime == 0`, it draws `u`. If `u <= 0.0025*dt`, it
     **gives up** (`vehicle = -2`, counter--). Everything else goes into the local
     **candidate list**, in deque order [D].
   - **Boarding**: `vehicles = TransportVehicleSystem::<vehicles at line stop>(line, stop)`
     (0xad5550, call at 0xa820ef). For each `v` **in that vector's order**: take
     `tv = TransportVehicle(v)` (0xa80790, a mutable getter with no change notification). Skip
     `v` if `tv.timeUntilNextLoad[c]` (`[tv+0x118]`) > 0 or if it is full
     (`GetVehicleSimEntitiesCount` against the capacity `[tv+0x58][c]`). Otherwise
     `n = min(free capacity, per-cargo allowance, floor(-timer/rate)+1)`, and the next `n`
     candidates, taken with **one running index shared by all vehicles**, are assigned
     `vehicle = v`. `timer += rate` for each assignment [D].
3. The assignments, then the give-ups and re-plans, are committed in list order inside
   one Begin/EndModification (0x23e4a60 / 0x23e4e10). The SimPerson and SimCargo systems
   receive the change notifications in that order [D].

Once the deques are canonical, the only order left to canonicalise is the **vehicle vector
in step 2**.

## 3. Container A: vehicles standing at a line stop (TransportVehicleSystem)

- Windows `TransportVehicleSystem+0x220` is a `phmap::flat_hash_map<pair<Entity line, int
  stopIndex>, vector<Entity>>` (slot 0x20: key 8 bytes, vector 0x18 bytes; ctrl at +0x220,
  slots at +0x228). The getter 0xad5550 returns `&slot.vector`, or a static empty vector
  when the key is missing [D].
- **Add**: `EntityAdded` (vtable 0x30ec8c8 slot 3, 0xad2ca0). When `tv.state == 2`
  (AT_TERMINAL, `[tv+0x90]`), it runs `vector.push_back(v)` under key `(tv.line [tv+0xa0],
  tv.stopIndex [tv+0xa4])` (0xacd2e0: a linear Contains check, then push_back) [D].
- **Remove**: `EntityToBeRemoved` (slot 4, 0xad3590) erases with 0xacd620, a linear find
  followed by a memmove erase that keeps the order [D].
- **Change**: `ComponentAboutToBeChanged` 0xad2850 and `ComponentChanged` 0xad29a0 call
  slot 4, then slot 3, for the TransportVehicle type. Every notified change of a vehicle's
  TransportVehicle component therefore removes the vehicle and appends it again [D].
- So while running, the order is the order of each vehicle's last notified TransportVehicle
  change while AT_TERMINAL at that stop (its arrival, in practice [G]). After a load, it is the
  load's `EntityAdded` order. **Not saved and not reproduced [D].**
- Consumers: `SimEntityAtTerminalSystem::Update` (above), and one Lua-binding caller (0xd7d8e0)
  that has no effect on the simulation [D]. The other TransportVehicleSystem maps checked here are
  only read by size: the terminal allocation at +0x250 (`FindNextFreeTerminal` reads it
  through 0xad3cd0, a least-loaded choice by `vector.size()`) [D].
- Native: getter 0x17718e0 (phmap near `tvs+0x1f0`), result stored at 0x16f6823. The loop
  reads the libstdc++ `vector<Entity>` at `[rbp-0xe20]` (begin `[v]`, end `[v+8]`) from
  0x16f6fe2 [D]. TransportVehicle is 0x168 bytes on both builds, and `+0x118` is `timeUntilNextLoad`
  on both [D].

## 4. Container B: entities riding a vehicle (SimEntityAtVehicleSystem)

- Component `SimEntityAtVehicle` is `{Entity line, int lineStop0, int lineStop1, Entity
  vehicle}` (Lua names; 4 ints loaded) [D]. **It has no boarding-time field.**
- `m_vehicleStop2cargo2simEntities` (Windows `SEAV+0x58`, an MSVC `unordered_map<pair<Entity
  vehicle,int stop>, vector<deque<Entity>>>`) [D, assertion strings].
  `EntityAdded` 0xa83e40 runs `deque.push_back(entity)`, inlined [D]. `EntityToBeRemoved`
  0xa844a0 runs a find and then an in-place erase (`it != entities.end()`, 0xd5b90) [D].
- `SimEntityAtVehicleSystem::Update` 0xa85460 sorts the vehicle ids (0x619eb0, MSVC sort) [D],
  so vehicle order is canonical. For each AT_TERMINAL vehicle and each cargo type, it walks
  the deque **from the front**, unloading one entry per `timeUntilNextUnload` tick
  (`[tv+0x130][c]`), and stops at the first entry the timer does not allow [D]. With more
  than one entity per tick, the deque order decides **who alights when**. That decides
  their next `arrivalTime` at a transfer stop, walk arrival ticks, and deliveries.
- While running, the order is the boarding order; after a load it is the `EntityAdded` order.
  No saved key reproduces the boarding order [D: the component has no time field].
- Native: vtable 0x59c0cb0 (add 0x16fd260, remove 0x16fc7b0, Update 0x16fd820). The unload
  loop at 0x16fe0ae walks a libstdc++ `deque<Entity>` at `r15` (+0x10 start.cur, +0x20
  start.last, +0x28 start.node, +0x30 finish.cur; 128 entities per 0x200 block) [D].

## 5. Is there a shared key for arrival order?

- Waiting queue: yes. `SimEntityAtTerminal.arrivalTime` at +0x10 (int64) is already the
  sort key, the tie-break is the entity id, and both are saved (section 1) [D]. Nothing to do.
- Vehicles at a stop: no field is known to be the arrival time.
  `TransportVehicle.doorsTime` (int64, in the Lua field list) might be one, but its
  semantics and offset were **not** verified [G]. Use the **entity id**.
- Entities on a vehicle: none (section 4). Use the **entity id**.

Sorting by id changes vanilla's tie-breaks: the lowest-id vehicle at a stop loads first, and
the lowest-id passenger or cargo alights first. It is a pure function of the entity set and
identical on Windows and native, which the canon needs. Every peer must run it, as with the
other sorts; the lobby's version gate already enforces that.

## 6. The fix

Sort each container ascending by entity id **in place**, immediately before the
simulation reads it. In-place sorting is safe: the owners only add after a linear
`Contains` (A) or by push_back (B), and they remove by linear find plus order-preserving
erase (sections 3 and 4) [D]. No other reader depends on the order. The next read re-sorts
whatever was appended since.

| batch | Windows site (steal) | container | native site (steal) | container |
|---|---|---|---|---|
| vehicles at line stop | 0xa820f4 `48 89 85 88 00 00 00` (`mov [rbp+0x88],rax`; right after `call 0xad5550`, once per line stop) | `rax` = MSVC `vector<Entity>*` (+0 begin, +8 end) | 0x16f6823 `48 89 85 e0 f1 ff ff` (`mov [rbp-0xe20],rax`; after `call 0x17718e0`) | `rax` = libstdc++ `vector<Entity>*` (+0 begin, +8 end) |
| entities on vehicle (unload) | 0xa85aa5 `4c 8b 44 24 48` (`mov r8,[rsp+0x48]`; run it first, before touching rsp) | deque = `r8 + r13`, MSVC `deque<Entity>` (+8 map, +0x10 mapsize, +0x18 off, +0x20 size, 4 per block) | 0x16fe0ae `49 8b 47 30 49 8b 5f 10` (`mov rax,[r15+0x30]; mov rbx,[r15+0x10]`) | `r15` = libstdc++ `deque<Entity>*` (block 0x200 = 128 ids) |

Site checks [D]:
- 0xa820f4: nothing branches into 0xa820f4..0xa820fa.
- 0xa85aa5: it is the target of `je` at 0xa85a76. That is fine because the hook replaces the
  first instruction; nothing branches into 0xa85aa6..0xa85aa9.
- 0x16f6823: no branch targets.
- 0x16fe0ae: it is the target of `jmp` at 0x16fe1df, again at the hook start; nothing
  branches into 0x16fe0af..0x16fe0b5.
- No stolen instruction is RIP-relative.
- None of the four overlaps an existing hook: the seed hooks are 0x16f60ef..0x16f6126
  (native) and at the Windows seed code near the start of 0xa81810.
- Alternative Windows vehicle site, per cargo: 0xa825aa `48 8b 85 88 00 00 00`
  (`mov rax,[rbp+0x88]`).

Helper, the same on both builds apart from the container walk: if the container holds fewer
than two ids or is already ascending, return (one scan). Otherwise copy the ids out,
`std::sort`, and write them back through the same walk (MSVC deque: the index formula above;
libstdc++ deque: `start.cur..start.last`, then `*++node` blocks until `finish.cur`). Keep
the entity ids unchanged; only positions move. Put the kill switch next to the others
(`hotjoinorder=0` / `TPF2MP_ORDER_CANON=0`).

Why both sides agree: A holds exactly the vehicles with `state == AT_TERMINAL` at `(line,
stopIndex)`, and B holds exactly the entities whose `SimEntityAtVehicle` names that vehicle
and stop. Both sets are functions of saved components. Sorting makes the order a function of
the set, so a retained world and a loaded one read identical sequences, and the per-tick
draws and assignments downstream follow.

## 7. Validation to run

- Lab (`hj-lab`, HOTJOIN_ORDER "Lab harness"): a save with a truck line whose trucks queue
  at a two-bay truck stop, and a busy bus stop. Retained host plus loaded joiner. Extend the
  probe to dump, per vehicle, `game.interface.getEntity(v)` state, stopIndex and cargo
  counts, and per stop the waiting counts. Expect a split within one load cycle with the sorts
  off and none with them on.
- Unit test (Unicorn, like `tools/hotjoin_order_bytes_test.py`): check the stolen bytes and
  that the helper leaves an already-sorted container untouched.
- HOTJOIN_ORDER.md "Not covered yet" should be corrected: the SEAT deques are sorted and
  saved. The open items are containers A and B.
