# Town street desync, native Linux server vs Windows players: root cause

Date: 2026-09-23. Public copy; host names, local paths and the capture files are omitted. Build 35924 (Windows exe; native ELF build-id `3a0e1563…454a`, sha256 `59dc5c9d…`, the
same file on the dedicated server and the Linux build box). Labels: **MEASURED** (seen in a log, a dump or a test run),
**DERIVED** (follows from measured facts plus the decompiled code), **GUESS** (plausible, not shown).

## 1. Answer

**The native family canon sorts 1 of the 28 ECS node lists; every Windows peer sorts all 28.**

- 0.7 added a per-iteration sort of every ECS family's node list (Windows `hotjoin_order.inl` site
  "step", dev 0115785) so that a world's node-list order no longer depends on its history. The
  TownSystem needs it: it develops town `i` of its node list when `t % 120 == (i % 30) * 4`, and hands
  every town of that tick **one** mt19937 seeded from `t`.
- The native port recognises a family's node list by **one** GetNodeList address, `0xa914c0`. MSVC
  folds every `ComponentGroupFamily<…>::GetNodeList` into one function, so one address is right on
  Windows. **GCC does not fold them.** The native ELF has 28 node-list getters and 35 no-list getters,
  one per family template, and only PersonCapacity uses `0xa914c0`.
- The live server has logged this at every iteration of every 0.7 session:

  ```
  [order-canon] step engine=0x73d43a8a0070 calls=63000 families=63/63 lists=1 reordered=0 moved=0 unknown/refused=61
  ```

  The Windows client in the same session logs `63 families, 28 node lists (largest 28834), 0 not
  understood; 19 lists put in entity order`. The server's boot line nevertheless says "all ECS families
  sorted per iteration".
- So the Town list (and TownBuilding, Construction, BaseEdge, TownConnection, SimBuilding, StockList,
  Station, the vehicle lists …) is in entity order on the Windows player and in load/history order on
  the server. The towns are then developed at different ticks, with different draws from the shared
  generator.

The fix (Linux only) is a generated table of all 63 getters, verified byte for byte at install. It is
proven offline against the production binary (§2.1, §4). It has **not** been run in a live session yet.

| # | cause | confidence | status |
|---|---|---|---|
| 1 | Native family canon inert for 27/28 node lists (Town stagger + shared per-tick MT) | **high**. The gap is MEASURED on the live server, reproduced exactly from the real ELF, and matches the session history. That it is the *first* divergence in this session is DERIVED. | fixed (scratch commit), tests pass |
| 2 | Remaining history-dependent town state a live join does not reset (per-town building sets in TownSystem data, 991800's float sum over them, RepopulateParcels' set order) | low to medium, GUESS | the new trace names it if it shows up after the fix |
| 3 | Remaining libm (asinf, powf/pow, exp, one double atan2) and sort ties (previous reports) | low | not changed |

## 2. Evidence

### 2.1 The gap, on the live server and in the binary (MEASURED)

- **Server** (`tpf2_proxy.log`, native boot library): `families=63/63 lists=1 … unknown/refused=61` at
  every logged iteration since 0.7. At the first iteration after each load it reads `lists=1
  reordered=1 moved=5323` (5,271–5,451 in other loads). The one list it does handle, PersonCapacity,
  is therefore **not** in entity order after a native load.
- **Windows client** (`tpf2_slice.log`): `step: engine … has 63 families, 28 node lists …, 0 not
  understood; 19 lists put in entity order (98132 nodes moved)` at load. After that it reorders at every
  step, averaging 221 µs.
- **Static inventory** (`tools/linux/gen_family_getters.py`, reading the ELF's own vtables): 63 family
  vtables. 28 have a slot-2 getter `endbr64; lea rax,[rdi+8]; ret` and 35 have `endbr64; xor eax,eax;
  ret`. All 63 addresses are distinct, and the Windows count is the same (63/28).
  - Town's getter is `0xa917d0` and TownBuilding's is `0xa917c0`.
  - The canon accepted only `0xa914c0` (PersonCapacity) and `0xa914e0` (VehicleOrder).
- **Reproduction on the real binary** (`native/linux/tests/family_canon_elf_test.cpp`):
  - The test maps the production ELF, applies its RELATIVE relocations, and builds an engine from the
    binary's 63 family vtables, each list out of order.
  - With the **old** check, the canon prints `families=63/63 lists=1 reordered=1 moved=3
    unknown/refused=61`, the server's line.
  - With the fix, it prints `lists=28 reordered=28 … unknown/refused=0`.

### 2.2 Session history (MEASURED, all server bridge logs incl. snapshots)

| era | step canon | join | outcome |
|---|---|---|---|
| Proton dedicated server (Windows exe), 09-18 … 09-22 | Windows code on both sides | frozen (all load) | all stamps EQ (e.g. world 35b08ebc: 36 → 1296, 12 stamps) |
| native 0.6.1.28, 09-22 before 18:41 | none on either side (added in 0.7) | frozen (the server's `tpf2mp_live_join.txt` was written at 18:42, after this session; native live join is opt-in) | world bf7d0193: 25 stamps EQ over 1,800 units, 36 new street edges; later 14400 and 43776–44208 EQ |
| native 0.7+, 09-22 23:42 onward | Windows: 28 lists, native: 1 | live join | **every** session NE within 1–3 stamps of a (re)join: 95c66a8f, 99890675, c6c86157, d5aed474, c6478d25 (EQ ×3 then NE), dbcdda9b (NE; re-join → EQ at 3456 → NE at 3840), b1c5479f (EQ 13056 → NE 13248; re-join at 18618 → NE at 19008) |

Every desync is in town growth: the e/z lanes, the t (town buildings) and n (people) lanes, and later
vehicle drift. The c (player constructions) and r lanes always match.

### 2.3 This session, both samples

- **Join mode (MEASURED).** Both joins were live joins. The server lobby logged `live join for
  '<player>': the session keeps running`, and `tpf2mp_live_join.txt` = 1 on the server. The joiner
  loaded the server's hot-join save.
  - Sample 1: the save was at t = 12773.2. The first stamp, 13056, was EQ, including t and n. At 13248
    the e lane differed (14272 edges on both sides), and so did t (6643 vs 6641) and n (3213 vs 3211).
  - Sample 2 (the coordinator's): the save was at t = 18618.4. By the first stamp, 19008, e (14308 vs
    14307), t (6957 vs 6959) and n already differed. Then came the town lane DESYNC at 19584 and the
    world DESYNC at 20160.
- **Same streets, different times (MEASURED, dumps at 13824 and 17856).**
  - The server-only edge 16171.1,9039.2→16225.8,8970.3 (built before 13248) exists on the Windows side
    by 17856, with identical geometry at 0.1 m. The server then extended it again (16225.8→16294.7).
  - Seventeen further street edges were built identically on both sides between 13824 and 17856.
  - So the street *decision* is the same on both builds; *when* it is made differs by town. That is a
    scheduling difference (which tick, which draws), not a float or validity flip.
- **Reproducible per-town outcome (MEASURED, dumps at 17856 and 20160).**
  - Town near (15300, 33200): after each of the two independent joins, the Windows side replaced the
    server's two edges 15253.6,33336.1→15310.3,33268.8 and 15326.6,33179.8→15378.2,33251.1 with the
    same 8-edge junction layout (new nodes 15295.0,33287.2 and 15353.5,33217.5). The server did not do
    this by 17856 and did not do it again by 20160.
  - The edge −8790.6,4130.2→−8876.3,4110.2 is Windows-only in both samples.
  - A deterministic, repeatable difference from the same starting save is what a different processing
    order and generator stream gives. Random ulp noise would not repeat.
- **Why "one extra street per side, 30 km apart" (DERIVED).**
  - The towns processed in one tick share one generator, and which towns share a tick depends on the
    node-list index.
  - With the list in a different order, every town has its own development schedule on each side.
    Whichever town first reaches a slot-sensitive decision diverges, independently on each side.
  - At 13248 that happened to be one town per side. By 17856 it is 16 server-only and 32 Windows-only
    edges scattered over many towns, and in sample 2, 10 and 20 edges.
- **Why the first stamps match (DERIVED, GUESS for the exact split).**
  - The town-centre street developer seeds its per-node generator from the node position
    (`trunc(x)+trunc(y)`). The tick's shared MT is not involved, so the streets it builds do not depend
    on the tick.
  - Building placement does use the shared MT: `TownDeveloper::Develop` random picks, removal,
    upgrades, and the build-state loop with jitter angles.
  - The world hash counts town buildings (t) but does not hash their positions. Buildings can therefore
    land differently with equal counts first. Streets follow once a street decision depends on them, or
    on a `simulation_util::Develop` position drawn from the shared MT.

### 2.4 Things checked and not the cause here (MEASURED)

- **Mods.** The `mp_lockstep_1` Lua is identical on both sides, including `deterministic_script.lua`
  at 0f4614f0, except `inject.lua`, which is UI replication and not the sim. Only mp_lockstep, NTG
  (1954591986) and the legacy vehicle pack are active. The Windows-only `mp_bridge_1` folder is not
  active.
- **Pausedtick.** Installed on both sides.
- **The other order sites.** candidates, departures, arrivals, idle, capacity maps and freed ids show no
  refusals on the server. Ship and air order are "measured only" on both builds.

## 3. What decides "build this street here" (static RE, both binaries)

| input | where | parity before this work | how known |
|---|---|---|---|
| which tick develops a town | TownSystem 0xab1d20 / 0x1746790: town `i` when `t%120 == (i%30)*4`, `i` = Town node-list index | **broken on native**: list not canonicalised | §2.1; fixed |
| shared per-tick MT seed | FNV(t) tag 21 | fixed (town_seed_linux, live "enabled") | REPORT.md |
| draws from the shared MT | Develop: GetRemove, upgrade and build-state picks, `Random(0,n)`, jitter angle `(int)(cos θ·100)` | uniform int / float endpoint fixed (engine_parity); the **order of towns** sharing the stream depends on the Town list | REPORT.md; §2.1 |
| Town +0x0c/+0x10 growth factor | TownSystem from 0x991c50 (±0.02 thresholds) | not verified; container-order float sums inside (GUESS) | decompile only |
| Town +0x64 | 0x991800: weighted float mean over the town's building **set** (list order) | order-dependent float sum; history-dependent on a live join, platform-dependent (MSVC vs libstdc++ set) | decompile; consumer not traced |
| building lists (S2) | 0x91ca10 → sort by float d² | order ties rare (sort_parity report) | SORT_PARITY_REPORT |
| street candidate order (S1, S3) | street developer / parcel_util sorts | no ties in this world; patch exists | SORT_PARITY_REPORT |
| per-node street MT | seeded `trunc(x)+trunc(y)` | identical | STREET_LIBM_REPORT |
| street geometry libm | sinf/cosf/tanf/acosf/atan2f | fixed (libm_parity, live "enabled") | STREET_LIBM_REPORT |
| remaining libm | asinf, powf/pow, exp, one double atan2 | open | SORT_PARITY_REPORT §9 |
| parcel repopulation | RepopulateParcels 0x952730 / 0x1536e20: int-key sort over a set's order, first come first served | open (platform), possibly also history | SORT_PARITY_REPORT |
| NTG capacities | Lua, deterministic wrapper; inputs from `getTownCargoSupplyAndLimit` / `getTownReachability` | wrapper identical; engine inputs not compared | mod hashes |

## 4. Fix (scratch commits, not pushed)

Delivered as `linux_family_getters_town_trace.patch` in this directory, against port/dev d855414.

- **The fix** (first commit of the patch):
  - `native/linux/src/family_getters_linux.h`: 28 + 35 getter RVAs, generated from the ELF by
    `tools/linux/gen_family_getters.py`. Its `--check` mode verifies the header against the ELF.
  - `family_canon_linux.inl`: a family is classified by that table; unknown getters are still refused,
    and nothing is ever called.
  - `order_canon_linux.cpp`: the install guard checks all 63 getters byte for byte.
  - `tests/order_canon_test.cpp`:
    - every getter is recognised at widths 1–5;
    - no-list families are skipped without counting as refusals;
    - unknown and mid-function addresses are refused untouched;
    - a flipped byte in any of the 63 refuses the install with no write.
  - `tests/family_canon_elf_test.cpp` (manual, needs the ELF): the real-image test of §2.1.
  - `tools/linux/verify_order_canon_elf.py`: now also compares the table with the ELF's inventory.
  - Docs: a section in `docs/re/HOTJOIN_ORDER.md` and a correction in
    `docs/re/linux/DEV_0115785C.md`.
- **Test runs:**
  - soldier SDK (GCC 8.3): **67/67 CTests pass**, including the new `town_trace` (built
    on ddb493f; d855414 changes no native code);
  - the real-ELF test PASS; `verify_order_canon_elf.py` PASS;
  - mutation check: reverting to the single-address check fails the unit test, and on the real ELF it
    reproduces `lists=1 … refused=61`.
- **Behaviour change.** The native peer now orders all 28 node lists as Windows does. Two native peers
  must run the same build: the lobby's exact version gate covers this at release.
- **Cost.** Expect the per-iteration canon cost to rise towards the Windows figure: 221 µs on average
  there, with ~100k nodes moved at the first iteration after a load. The server's current line reads
  avg 512 µs, mostly spent walking the families. **This has not been measured on the server.**
- **Publishing path.** dev carries no `native/linux`: the native half is the porter patch in this
  directory; the Windows half of the trace is in dev.

## 5. Diagnostic: town development trace (off by default)

**Second commit of the patch, plus the Windows slice part in dev.** Both builds write `tpf2_towntrace.txt` in the data dir:

```
TT t=<GameTime+0x34> town=<id> i=<index in Town node list> n=<len> list=<fnv32 of list order> mt0=<fnv64 MT before> mt1=<after> e=<engine>
TF t=<t> e=<engine> lists=<k> <count>:<fnv32> …        (every 600 iterations: every node list as the systems see it)
```

`python tools/town_trace_diff.py A B` names the first difference in the window the two files share:

| field that differs | what it means |
|---|---|
| `list` | node-list order: this bug class |
| `mt0` | an earlier town of the same tick drew differently |
| `mt1` with equal inputs | this town decided differently: trace deeper into Develop |
| a line on one side only | development gating: Town +0x61, capacity |
| TF tokens | which node lists differ at all |

**Windows.** `towntrace=1` in `tpf2_slice.cfg`, read at attach.
- The call `0xab253e` → `TownDeveloper::Develop` is pointed at a near stub that stores r13 (the
  update's context) and jumps to a wrapper. The wrapper reads the tick clock through the game's own
  getter 0x287830 and the Town vector through `[r13+8]`, then calls Develop with its arguments
  unchanged.
- TF lines come from the step canon.
- `tools/town_trace_bytes_test.py` PASS against the exe: the 43-byte guard, r13's only writes, the
  clock reads at 0xab1dab and 0xab1ec0, and the stub layout.
- `build.bat slice` compiles with no new warnings.

**Native.** `TPF2MP_TOWN_TRACE=1`.
- The same call `0x1747917` goes through `Tpf2mpRedirectCall`, behind a 70-byte guard.
- The clock and context come from the existing town-seed hook, `[rbp-0xbf8]`, and TF lines from the
  family canon.
- CTest `town_trace` covers:
  - the FNV references and the exact line format;
  - the wrapper's argument pass-through;
  - engine indexing and the TF cadence;
  - refusals: env values, build id, guard bytes, unwritable file, double install;
  - the stub target.
- The real-ELF test installs it against the production image.

**Off (the default).** Nothing is patched and no file is opened. The only per-update cost is one flag
check.

**On.**
- Size: at most one TT line per developed town per 120 iterations. At 4x this is roughly 30 MB an hour,
  an estimate.
- Not covered yet: per-candidate and validity-test detail inside the street developer, and the Town
  component fields. On the native side the signatures of those inner functions are not pinned; a
  generic hook there would risk the unwind path. If the TT diff points inside a single town's
  Develop, that is the next layer to add.

## 6. What you need to do (no lab needed first)

1. **Port** `linux_family_getters_town_trace.patch` to port/dev, then build and install the native
   release on the server. Windows players need no new build for the fix; they need one only for the trace.
2. **Verify on the server at load:** `grep "order-canon] step" …/data/tpf2_proxy.log` must show
   `lists=28 … unknown/refused=0`, not `lists=1 … 61`. Note the `avg=` µs for cost.
3. **One normal session with a Windows join**, with the trace on both sides to prove it:
   - server: `TPF2MP_TOWN_TRACE=1` in `/etc/tpf2mp/server.env`;
   - Windows: `towntrace=1` in `tpf2_slice.cfg`;
   - keep `dump_egeo=1` on both.

   Play or idle at least 2,000 units after the join; sample 2 split within 390.
   - **Expected with the fix:** e/z/t/n lanes EQ throughout, and `town_trace_diff.py` reports that every
     Develop call agrees.
   - **If it still splits,** the diff names the first town and tick, and which of the three kinds it is.
     Copy both `tpf2_towntrace.txt` files and both `egeo_*.txt` files.
4. **Control, only if step 3 still splits with `list` equal:** set `tpf2mp_live_join.txt` to `0` on the
   server so the server reloads at a join. That separates "history the canon does not cover" (cause
   2) from a platform difference (cause 3).
5. Turn the trace off afterwards (unset the env var, remove the cfg line). Both take effect at the next
   game start.
