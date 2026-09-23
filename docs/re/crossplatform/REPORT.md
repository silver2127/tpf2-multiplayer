# Native Linux vs Windows simulation desync (build 35924)

Date: 2026-09-22. Binaries:
- Windows `TransportFever2.exe`, build 35924.
- Linux ELF, build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`. strelka and the VPS production copy have the same SHA-256.

## 1. Summary

The native Linux game and the Windows game do not produce the same random numbers from the same world. There are four independent reasons, all caused by the two toolchains' standard libraries (MSVC STL + UCRT vs libstdc++ + glibc):

1. **`std::hash`.** MSVC's is FNV-1a; libstdc++'s is the identity for integers. The game builds every per-batch `mt19937` seed as `HashCombine(std::hash(tag), std::hash(time/entity)...)`, so the seeds differ.
2. **`std::uniform_int_distribution` and `std::shuffle`.** MSVC maps a draw into a range with modulo plus rejection; libstdc++ uses bucket division, and its shuffle takes two indices per draw.
3. **`std::default_random_engine`.** It is `mt19937` on MSVC and `minstd_rand0` on libstdc++.
4. **`generate_canonical` / unit-float endpoint.** On a raw draw of 2^32-128 or more, MSVC returns 1.0 and libstdc++ clamps to nextafter(1,0). One `minstd` float path also rounds differently.

`origin/port/dev` (061343e) already corrects many sites, and production 0.7-native runs all of them. This work found **15 more simulation sites** still uncovered, all on paths that town growth and construction exercise heavily. It fixes all 15 in two new modules (`sim_seed_linux`, `engine_parity_linux`), and 65/65 soldier CTests pass.

libm also differs by 1 ulp, but only as a secondary risk: Proton's own math differs from Windows more than glibc does, and Windows–Proton sessions have held (§3.5).

Nothing was tested in a live game. §7 describes the lab run that must come next.

## 2. The live incident (production logs, read-only)

Sources: `/opt/tpf2mp/native/share/tpf2mp/data/tpf2_bridge.log` and the game's `stdout.txt`.

- The server loaded its save at t≈165.8. The Windows player joined from a save taken at t=177.2.
- The first comparable stamp shows the split:

  | Stamp | e (edges), server / joiner | z | t (town buildings), server / joiner |
  |---|---|---|---|
  | 252 | 12862 / 12859 | differs | 5864 / 5858 |
  | 336 | 13100 / 13093 | | |
  | 420 | | | 4813 / 4811 (`DESYNC (town buildings)`) |

- The gap keeps growing, and vehicle drift follows at t=1152.
- This is **town growth**: a world of about 12.8k edges in its first year, adding roughly 250 street edges in 170 game units.
- An earlier native session in the same log (4.5k edges, Windows peer, stamps t=6048..14400) had e/z agreeing at every stamp while its towns added 36 edges. The remaining gaps are on paths that heavy early growth exercises and slow growth rarely reaches.

## 3. Root causes, proved by executing the real code of both binaries (Unicorn)

The proof harnesses are in the scratchpad: `emu.py`, `emu_seed.py`, `emu_simseed.py`, `emu_engines.py`.

### 3.1 Seeds: std::hash inside HashCombine

Example: `SimPersonSystem::NoteWalkPersonsArrived`.
- **Windows 0x140a978a1..0x140a97901:** FNV-1a over the 4 bytes of x, then `sub 0x7caf5538ae6723f8; xor 0xed202288923b4a3f`. This equals `HashCombine(FNV(3)+0x9e3779b9, FNV(x))`; the constant 0xed202288923b4a3f is exactly FNV(int 3)+0x9e3779b9.
- **Linux 0x17175dd..0x17175f5:** `add rsi,0x2853a3c728; xor rsi,0x9e3779bc`. This is the same combine with the identity hash.

| x | Windows seed | Linux seed |
|---|---|---|
| 0 | 0x7d5da5c2 | 0xcd94be94 |
| 1 | 0x0b0a7953 | 0xcd94be95 |
| 1000 | 0xe1c9f91b | 0xcd94b2ac |

### 3.2 Integer ranges and shuffle

- **Windows 0x140955010:** `g % n`, with MSVC `_Rng_from_urng` rejection.
- **Linux 0x31b6070:** `g / (0xffffffff/n)`, rejecting `g >= n*scaling`.

From the same MT state, 119 of 800 draws agree (chance level). For example, `Random(mt,0,10)` with seed 77:
- Windows: `[5,5,8,6,7,...]`, which is `raw % 10`.
- Linux: `[9,0,6,3,7,...]`, which is `raw / 0x19999999`.

Shuffle: 1 of 15 shuffles agree. The 64-bit-range helper agrees on 44 of 192 draws; the std::mt19937 helpers on 19 of 120 and 15 of 80.

### 3.3 Unit-float endpoint

`generate_canonical<float,24>` is bit-identical on 8,000 draws. At raw draw 0xffffffc0, Windows returns 1.0 and Linux returns 0.99999994.

`AirConnectParts` (minstd float):
- Windows computes `(float(g)-1)/2^31`.
- Linux computes `float(g-1)*2^-31` and then clamps.
- 3.0% of draws differ.

### 3.4 default_random_engine

Each build's own signature shows it: the Windows functions take `std::mersenne_twister_engine<unsigned int,...>`, and the Linux ones take `std::default_random_engine` (`minstd_rand0`).

The live cases are:
- `TownBuildingTransformator::EmitModelInstances` (Windows 0x140b533b6, Linux 0x18296a0): `mt19937(id*31+7)` on Windows vs `minstd_rand0(id*31+7)` on Linux.
- Town-name selection.

`parcel_util::{anon}::CreateBuilding` and `GetSplitLine` are dead on both builds.

### 3.5 libm (secondary)

Bit comparison on game-like inputs: glibc 2.42 (VPS) against this PC's ucrtbase, and Proton 9.0-203's builtin ucrtbase run under emulation on the same inputs.

| function | MS vs glibc | MS vs Proton 9 |
|---|---|---|
| sinf / cosf | 1.4% / 1.2% | 0 / 0 |
| tanf | 0.03% | 13.7% |
| acosf / asinf | 1.9% / 6.4% | 7.4% / 1.7% |
| atan2f / atanf | 0 / 0 | 14.0% / 0.2% |
| expf / logf / powf | 0.07% / 0.04% / 0.08% | same as glibc |
| log2f / fmodf | 0 | 0 |

Every difference is 1 ulp. Proton differs from Windows at least as much as glibc does, and Windows–Proton sessions have stayed in sync, including the earlier production snapshots with zero bad stamps. So libm is not the leading cause. If a lab run still diverges after the RNG fixes, the next step is to route sinf/cosf/acosf/asinf through Windows-identical implementations.

## 4. Already fixed in port/dev and enabled in production

- `Random(boost mt,int,int)` → MSVC modulo.
- The 0x31b5fc0 float clamp.
- 10 SimPersonSystem seeds.
- 8 PathFactory and person-cost hash windows.
- TownSystem tag-21 seed.
- Tree RNG, animal RNG, resident hash.
- The building, person-map, target, network and index order modules.
- The hot-join canonical order (`TPF2MP_ORDER_CANON=1`).

## 5. Remaining gaps now fixed (all run during live play)

### 5.1 Seeds — `sim_seed_linux.cpp` (kill switch `TPF2MP_SIM_SEED=0`)

| Windows | Linux function / patch site | What | Fix |
|---|---|---|---|
| 0xa74070 | 0x16e4480 / 0x16e4987 (7 bytes) | SimBuildingSystem::Update2 (std::mt19937) | rax = FNV32(time) |
| 0xa69670 | 0x16d2c50 / 0x16d2e39 (6 bytes) | RunwaySystem, tag 11 | invert the native seed to the time, then apply the Windows hash |
| 0xaa4a90 | 0x172e140 / 0x172e21e (6 bytes) | StockListSystem::Update2, tag 12 | same |
| 0xa81810 | 0x16f6090 / 0x16f6126 (6 bytes) | SimEntityAtTerminalSystem::Update, tag 13 | same |
| 0xa7a770 | 0x16edd30 / 0x16ede8a (6 bytes) | SimCargoSystem::NoteLineChanged, tag 16 | Windows time+entity hash |
| 0xa7b070 | 0x16ed220 / 0x16ed2d8 (6 bytes) | SimCargo callback, tag 17 | same |
| 0xa78110 | 0x16ed5b0 / 0x16ed6b0 (5 bytes) | SimCargoAtTerminal callback, tag 18 | same |
| 0x95f730 | 0x154a480 / 0x154a5fd (8 bytes) | parcel_util, reached through the apply_command visitor | rax = FNV32(time) |

Proof: the native seed matches Windows at 0 of 6 inputs per site; with the fix it matches at 6 of 6. None of the hook addresses is a branch target, and no stolen instruction is RIP-relative.

These sites seed identically on both builds and need no fix: the constant-5489 seeds (the StockListUpdateHelper and SimEntityUpdateHelper destructors, apply_command visitors, SellVehicle, BuildConstruction), and the arithmetic seeds (ComputeEmittedCargo, AddSimPersons, building_distribution, street_developer, init_streets, streetloopfactory, RandomLocationFactory, TownDeveloper).

### 5.2 Distributions and engines — `engine_parity_linux.cpp` (kill switch `TPF2MP_ENGINE_PARITY=0`)

| Linux | Windows | What | Fix |
|---|---|---|---|
| 0x14ed9e0 | 0x140b6cf80 | 64-bit uniform int (boost MT): ConstructProposal 6-character string, CreateIndustries | whole-function MSVC replacement |
| 0x14edc70, 0x14ede20 | inline in 0x140915d90, 0x140914e80 | `std::shuffle`: StockListUpdateHelper dtor (every construction apply), FillTownCargoTypes | MSVC forward Fisher-Yates |
| 0xdb1290, 0x14e8ee0, 0xd25730 | 0x142374e10 + inline | uniform int on std::mt19937: RandomLocationFactory (industry founding), names, MakeSeq | MSVC replacement |
| 0x18296a0 (seed stores 0x182ac9d / 0x182cdc6) | 0x140b533b6 | TownBuildingTransformator: minstd → mt19937 | per-thread mt19937 bound to the engine slot |
| 0xc45010 → 0xc46c90 | 0x1402ac7f0 / 0x1402ab740 | town names: default engine + shuffle | MSVC mt19937 + shuffle |
| 0x2eccc54..0x2eccc87 | 0x14026ec60 | AirConnectParts minstd float | 51-byte rewrite to the Windows formula |
| 0x31b5f7e, 0x31b7759 | 0x14014e1a0 | std-MT float / boost double endpoint clamp | 2-byte NOPs |

Every MSVC model reproduces the golden values produced by the Windows instructions themselves.

Left unfixed:
- Map generation, UI and audio paths.
- The endpoint copies 0xa5fb70 and 0xdb4760, used by map generation and the ReplaceTerrain command (about 1 draw in 2^25).
- The StreetGenerator minstd draw (0x31b5c60), which is probably cosmetic.

## 6. Implemented and tested (scratch only; nothing committed or pushed)

- **Worktree:** `scratchpad/portdev`, detached at origin/port/dev 061343e.
  - New module sources: `src/sim_seed_linux.{h,cpp}`, `src/sim_seed_sites_linux.h`, `src/engine_parity_linux.{h,cpp}`, `src/engine_parity_sites_linux.h`, `src/windows_msvc_random_linux.{h,cpp}`.
  - Tests: `tests/sim_seed_test.cpp`, `tests/sim_seed_vectors.inc`, `tests/engine_parity_test.cpp`, `tests/engine_parity_golden_linux.h`.
  - The CMake and boot.cpp wiring is in `scratchpad/parity_integration.diff`.
- **Soldier SDK build** (`tools/linux/build_native.sh`, strelka `~/parity-scratch-20260922/combined`): **65/65 CTests pass**, including the new `sim_seed` and `engine_parity` tests.
- **Real-image check** (`real_image_install.cpp`): the real ELF's PT_LOAD segments are mapped into a test process, and all 16 installers run in boot order.
  - Every one returns OK, the two new modules included.
  - Every verified context byte matches the shipped binary.
  - The modules co-install without overlap.

## 7. Lab

No lab run was done.
- **VPS:** only about 1 GB of RAM was free (production used ~30 of 31 GB), so even a small save would have pushed the live server into swap.
- **strelka:** it has a native + Proton lab (`~/.local/share/tpf2mp-lab`) and 26 GB free, but it is the user's logged-in desktop and runs the porter.

Proposed test on strelka, with the user's go-ahead: install a boot library from `build-combined` into the native lab actor, load the same early-year save in the native and Proton actors, unpause at speed 4 with no commands for 2,000+ units with the hash forced every 84 units, and compare the e/z/t/n lanes per stamp. Repeat once with `TPF2MP_SIM_SEED=0 TPF2MP_ENGINE_PARITY=0` as the control.

## 8. Open questions

- **CRT `rand()`:** command handler 0x9d7c40 (Linux 0x15e6890) seeds an MT from `rand()` and calls `simulation_util::Develop`. glibc and UCRT `rand()` differ, and per-process `rand()` state is not lockstep-safe even between two Windows peers. The command it belongs to is not identified yet.
- **Transformator branch order:** one draw at 0x182ba5f sits on a branch whose order was not checked against Windows.
- **Tags 16/17/18:** the entity for each was inferred from the tag and the struct shape, not traced to the callers.
- **Remaining STL differences** (unordered-container iteration order, `std::sort` on equal keys): the ported order modules cover the known person paths. Any others would show up only in the lab run.
- **libm:** see §3.5.
- **bigmap/linux:** its patch sites were not checked for overlap with these addresses.

## 9. State-changing commands

- **VPS (root@76.13.109.115):**
  - Copied three analysis scripts to `/tmp/claude_*` and ran them read-only on the logs.
  - Ran a `nice -n 19` libm sampler for about a minute.
  - Deleted all four `/tmp/claude_*` files afterwards.
  - Touched no service, game or lab process.
- **strelka:**
  - Created `/tmp/tpf2_text.asm` (objdump of the ELF), `/tmp/lsites.py` and `/tmp/libm_cmp.py`.
  - Created `~/parity-scratch-20260922/` (source copies and soldier builds) and the `real_image_install` binary there.
  - Did not touch `~/tpf2-port` or `~/tpf2-multiplayer`.
- **Local:**
  - Created the detached worktree `scratchpad/portdev` (registered with the main repository's git worktree list; remove it with `git worktree remove`).
  - Copied the Linux ELF and Wine's ucrtbase.dll into the scratchpad.
  - Pushed nothing.
