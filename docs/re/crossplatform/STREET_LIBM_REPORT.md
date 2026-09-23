# Town street extension: native Linux vs Windows (build 35924)

Date: 2026-09-23. Follows `docs/re/crossplatform/REPORT.md` (origin/dev).
Binaries: Windows `TransportFever2.exe` 35924; Linux ELF build-id `3a0e1563…93ae454a`.
Scratch worktree: `scratchpad/libmwt` (detached at origin/port/dev 0a15250). Patch: `scratchpad/libm_parity.patch`. Nothing is committed or pushed.

## 1. Summary

1. **The Windows player and the server did not run the same mod.** Three Lua files in the player's `mods/mp_lockstep_1` differ from the server's copy. One of them is `deterministic_script.lua`, the Natural Town Growth wrapper.
   - The player had the version from 87324c7. That version applies every per-frame state load.
   - The server had today's fix (9125a2d), which ignores same-clock loads.
   - The logs confirm it:

     | | Player's stdout | Server's stdout |
     |---|---|---|
     | Loads applied | 13,315 | — |
     | Same-clock loads ignored | — | ~16,000 |

   - NTG's state sets the town capacities, and the capacities gate town development. So this mismatch alone can make the towns grow differently.
   - The other two files, `inject.lua` and `pacing.lua`, are also older on the player's side.
   - **Match the mods before blaming native code.** This is the cheapest and most likely explanation for a divergence that affects town growth only, goes one way, and then holds steady (§5).
2. **The native code does have a real difference on this path.**
   - The street developer and TownDeveloper feed `sinf`/`cosf`/`tanf`/`acosf`/`atan2f` results into three things:
     - street geometry (curve-handle lengths and forced minimal angles),
     - the branch-angle map,
     - an integer-truncated retry position.
   - glibc 2.42 and ucrtbase disagree in these fractions of calls: `sinf` 1.36%, `cosf` 1.19%, `acosf` 1.94%, `tanf` 0.03%.
   - GCC also calls the **double** `atan2` at three sites where MSVC calls `atan2f`.
   - Geometry is hashed at 0.1 m, so these ulp differences stay invisible until a knife-edge decision flips.
   - In one of the three places, the new street D1→D2 is exactly **88.0 m**, which is the developer's straight-extension length. It lands exactly on an existing dead end, which is such a knife edge.
3. **Implemented fix (scratch only):**
   - Bit-exact C++ models of ucrtbase's `sinf`, `cosf`, `tanf`, `acosf` and `atan2f`:
     - `sinf`/`cosf`/`tanf`/`acosf` are identical over all 2^32 inputs;
     - `atan2f` is identical on 2^31 random pairs plus an edge grid.
   - A boot module, `libm_parity_linux`, points the game's `.got.plt` entries at the models and moves the three `atan2` sites onto them.
4. **Configuration mismatches found** (they do not cause these 5 edges, but they are parity gaps):
   - The player runs Big Maps with `octree_depth=13` (EXPERIMENTAL); Linux only supports 11.
   - The player's `placement_attempts=50` and the 64-bit placement-spacing fix have no Linux counterpart. This changes where industries and towns are founded.
5. **ucrtbase is CPU-dependent.** Its AVX2+FMA and SSE2 paths differ for `sinf`, `cosf`, `acosf` and `asinf`. A Windows peer without AVX2/FMA3 computes differently from other Windows peers.

## 2. The deciding code path

| Step | Windows RVA | Linux RVA | Notes |
|---|---|---|---|
| TownSystem update, towns with flag +0x61 | 0xab1d20 | — | calls TownDeveloper::Develop |
| `TownDeveloper::Develop` | 0x91d910 | 0x14f5b80 | positions; on failure: `θ=Random(0,a)`, `pos += (int)(cosf θ·100)`, `(int)(sinf θ·100)` |
| `simulation_util::Develop` | 0x97e110 | 0x15728f0 | blocked-node set is **nullptr** at runtime |
| street developer wrapper | 0x985e50 | 0x157c2f0 | two passes over the candidate nodes |
| collect candidate nodes | 0x985160 → octree query 0x9829d0 | inlined | box pos±R, then `std::sort` (0x9847a0 MSVC introsort; Linux libstdc++ 0x1578370 / 0x1577ea0 / 0x1577d60), comparator = predicate 0x987450, then squared 2D distance |
| **per-node extension attempt** | **0x986070** | **0x1579a20** | local MT seeded `trunc(x)+trunc(y)`; returns after the FIRST street built |
| Expand (candidate tangents) | 0x986cc0 | inlined | lengths 88/120 (0x14309ec60, 0x142f72544); filter 0x9855a0: `acosf(dot)` < 1.0367 / 0.6283 → drop |
| CheckBranches | 0x985310 | inlined at 0x157a4ac | `atan2f(t)` + upper_bound in the angle map; `cosf/sinf(±π/6)` rotations |
| CheckBranchesRec | 0x985800 | 0x1578da0 | `atan2f`, sqrtf distance budget |
| CreateAngle2SegMap | 0x985ba0 | 0x15789c0 | `map<float,Entity>` keyed by `atan2f(-t)` |
| jitter (inactive: `townMajorStreetAngleRange = 0`) | 0x986839.. | 0x157bf67.. | `atan2f` + Random + `sinf/cosf` |
| SnapOrCreatePoint | 0x987850 | inlined | endpoint = node + 88·t, terrain height, CompCurveHandleDistance |
| terrain test | 0x33d920 | | floorf grid test |
| build + validate | 0x987d50 → 0x984e30 | inlined, Apply 0x15fab90 | CheckWaterCollisionHack; ForceMinimalAngle 0x4a7290 (Linux 0xf08fb0: `acosf/cosf/sinf`); angle 0x3ff410 (`acosf`); CompCurveHandleDistance 0x22353d0 (`sinf`, `tanf`) |

Every float step of the candidate generation and checks that I compared uses the same operations in the same order on both builds. For example:
- the normalisation `sqrtf(x²+y²)` then `1/l`;
- the endpoint `node + len·t`;
- the rotation `x·c − y·s`.

Only the libm calls and the `atan2` overload differ.

## 3. Proven native differences on the path

### 3.1 libm results

The table is the 200k-input comparison: glibc 2.42 (production VPS) against ucrtbase 10.0.26100.

| Function | Differ | Example: x → UCRT / glibc |
|---|---|---|
| `sinf` | 1.355% | 0xc117f700 → 0x3d956bfe / 0x3d956bff |
| `cosf` | 1.192% | 0xc105e3c7 → 0xbefbca0b / 0xbefbca0a |
| `acosf` | 1.941% | 0xbf50c016 → 0x40218da2 / 0x40218da3 |
| `tanf` | 0.032% | 0xbf0dd22e → 0xbf1e5cfa / 0xbf1e5cfb |

Where these values go:
- Every town street built through 0x987d50 gets its end tangents from the calls below. So new streets differ in their low bits between the platforms.
  - `acosf` (0x3ff410 angle);
  - `sinf`/`tanf` (CompCurveHandleDistance 0x22353d0: `(d/2)/sinf(a/2) · 4 · tanf(a/4)`, returning d when a < 0.001);
  - `acosf`/`sinf`/`cosf` in ForceMinimalAngle.
- The next decisions that measure against those streets then inherit those low bits:
  - CheckBranchesRec distances and angles;
  - snapping and collision in validation.
- The TownDeveloper retry step `(int)(cosf(θ)·100 + x)` also flips with a small probability. That probability is about 1e-5 for each call whose `cosf` differs.

### 3.2 atan2 overload

- In `street_developer_util` the source calls `::atan2` on floats. MSVC resolves this to `atan2f` (imports at 0x140985d53, 0x1409859d4 and in 0x985310).
- GCC resolves it to the double `atan2` (PLT 0x6dbc60) at three places:
  - 0x1578b3b (CreateAngle2SegMap);
  - 0x1579031 (CheckBranchesRec);
  - 0x157a4ac (CheckBranches, inlined).
- The result differs from ucrtbase's `atan2f` in 42 of 67M unit vectors.
- The ELF holds 7 more such "float-semantic double" `atan2` sites. They are in init_streets Search 0x15138f8, StreetGenerator, catenary and one unnamed function. The patch leaves them alone, because their Windows counterparts were not checked.

### 3.3 Sort ties (possible, but probably not these 5 edges)

- The candidate list is sorted with MSVC `std::sort` on Windows and libstdc++ `std::sort` on Linux. These order equivalent elements differently.
- The per-call loop keeps going until the first success. So a different tie order would build a *different* street on Windows, and that would show up as a Windows-only edge.
- There are none, and the gap does not grow. That argues against ties being the cause here.

### 3.4 Octree depth (probably neutral, unvalidated)

- Both configurations use 128 m leaves, and the cell grids align below 131 km. A correct octree returns the same set, and the list is sorted afterwards.
- Depth 13 is flagged EXPERIMENTAL and has not been validated in game.

## 4. Deployment and configuration mismatches in this session (read-only evidence)

- **mp_lockstep_1 Lua.** Hashes are sha256 after stripping CR:

  | File | Player's game dir | Server's game dir |
  |---|---|---|
  | `deterministic_script.lua` | e0381dc2… (87324c7) | 0f4614f0… (9125a2d) |
  | `inject.lua` | older (c0dde37) | newer |
  | `pacing.lua` | older (2b466e3) | newer (0a35d0a…) |

  - The player's stdout shows the old wrapper's "echoing our own state" message and 13,315 applied loads.
  - The server's stdout shows ~16,000 "state sync, not a rewind … ignored" messages, at x1..x16000 with clocks 3755..12341.
- **Big Maps.**

  | Setting | Player | Server |
  |---|---|---|
  | `octree_depth` | 13 | 11 |
  | `placement_attempts` | 50 (log: "fast placement: 50/200 attempts") | — |
  | 64-bit placement-spacing fix | yes | — |
  | `street_raster`, budget 1.5e9 | same | same |
  | travel_time | 0 | 0 |

## 5. What the 5 edges say

The divergence appeared once and then held:

| Stamp | Server edges | Joiner edges |
|---|---|---|
| 6336 | 13134 | 13134 (no growth for 1152 units) |
| 6912 | 13210 | 13204 |
| 7488 | 12996 | 12991 |
| 8064 | 13018 | 13013 |

- Three towns about 40 km apart diverged in the same window, and nothing else diverged in the next 1152 units.
- All the extra geometry is 88 m straight extensions of dead ends: the Expand straight candidate.
- At (−12.6k, −9.1k), D1→D2 is exactly 88.0 m and closes onto an existing dead end: a snap knife edge.
- The other two new endpoints have no street within 88 m.

This pattern fits a capacity or timing difference best: Linux towns were allowed to develop a little earlier. The NTG wrapper mismatch produces exactly that kind of difference. A libm knife edge explains the D1→D2 connection, but random ulp flips would not normally fall all one way.

## 6. Fix design (Linux only; Windows unchanged)

The new module is `src/libm_parity_linux.{h,cpp}` plus `libm_parity_sites_linux.h`. It runs in `BootInit` after engine parity. The kill switch is `TPF2MP_LIBM_PARITY=0`.

1. **GOT redirection.**
   - The ELF is `DF_BIND_NOW`. Its `.got.plt` lies in RELRO and is written through `Tpf2mpCodeWriteSelf`.
   - These slots are pointed at the models:

     | Function | Slot RVA |
     |---|---|
     | `sinf` | 0x5a471b8 |
     | `cosf` | 0x5a471c8 |
     | `sincosf` | 0x5a470c0 (= sinf + cosf) |
     | `tanf` | 0x5a470d8 |
     | `acosf` | 0x5a472b8 |
     | `atan2f` | 0x5a472b0 |

   - Each slot must first resolve to libm's function of that name (dlsym, or dladdr name + libm).
   - The .text has no direct GOT references to these slots, and `.rela.dyn` has no GLOB_DAT for them. So every game call goes through the PLT and is covered.
2. **Call sites.**
   - The `call atan2@plt` at 0x1578b3b, 0x1579031 and 0x157a4ac is redirected, through a near stub, to `Tpf2mpUcrtAtan2FromFloats(double,double)`, which returns `(double)atan2f((float)y,(float)x)`.
   - Each site checks a guard of 20–24 bytes: the conversions, the call and the narrowing that follows.
3. **Failure handling.**
   - Everything is verified before any write.
   - A failed GOT write rolls back what was already written.
   - A site that cannot be redirected leaves the status at "partial".
4. **Models.** `src/windows_ucrt_math_linux.cpp`, compiled with `-ffp-contract=off`:
   - `sinf`/`cosf`: AVX2+FMA paths 0xac36d / 0xa7f7d, with the Payne-Hanek reduction and cosf's double-double reduction from 0xab710;
   - `tanf`: 0xad420;
   - `acosf`: 0x738ac;
   - `atan2f`: 0x519c0.
   - Its tables are generated from their mathematical definitions by `gen_ucrt_tables.py`: floor(2/π·2^1202) and RZ(atan(k/256)). The generator checks them against the DLL.

## 7. Implemented and tested

- **Exhaustive Windows check.**
  - `tests/windows_ucrt_math_check.cpp` (MSVC) compares the models with ucrtbase.dll.
  - Result: `sinf`, `cosf`, `tanf` and `acosf` agree on all 2^32 inputs; `atan2f` agrees on 2^31 pairs plus an edge grid, with 0 mismatches.
  - It also writes `tests/windows_ucrt_math_golden_linux.h`.
- **`tests/libm_parity_test.cpp`** (CTest `libm_parity`) covers:
  - stride hashes against that golden header;
  - 18 vectors, 16 of them where glibc differs;
  - the installer: build-id check, kill switch, foreign slot, guard mismatch, success, double install, and rollback after a failed write.
- **Test run.** Built with MSVC against a scratch POSIX shim, it passed every check.
- **Not done: the GCC/soldier build and run.** strelka was offline from about 00:50 and I did not compile on the production VPS.
  - Run `ctest -R libm_parity` in `native/linux`.
  - On glibc it should report that "this libm differs on 16 of 16".
- **Compiler bug found on the way.** MSVC 14.44 /O2 turns `memcpy(float ← (uint32_t)u64 counter)` into an int→float conversion. The models and the test use `std::bit_cast` where it is available.

## 8. Open questions

- **Is the NTG wrapper mismatch the cause?** Re-run with identical mods on every peer (see Lab).
- **The other libm functions:**
  - float: `asinf`, `atanf`, `expf`, `logf`, `powf`, `log2f`, `log10f`, `tanhf`;
  - double: `sin`, `cos`, `pow`, `exp`, `log`, `atan2`, …;
  - the other 7 double-`atan2` float sites.

  None of these is modelled yet.
- **Sort-tie order** in the candidate list and in other sorts: an MSVC `std::sort` port for these call sites is not done.
- **The octree depth-13 patch** has not been checked for query equivalence.
- **Big Maps placement** (`placement_attempts`, 64-bit spacing) needs a Linux port or a lobby parity gate.
- **The lobby gate** accepted peers whose mod Lua differs. It should compare hashes of the mod files.

## 9. Lab plan (strelka, needs your go-ahead)

1. Deploy the same mod build (9125a2d or later) to both actors.
2. Add a debug lane that hashes the raw float bits of node positions and edge tangents, not the 0.1 m strings.
3. Add a Lua lane with NTG's per-town capacities and `getTownReachability` / supply inputs.
4. Run A/B from the same save with no commands for 2,000+ units:
   - with `TPF2MP_LIBM_PARITY=0`, the raw-bit lane should split soon after the first town street, while the 0.1 m lanes may hold;
   - with the module on, it should hold.
5. The capacity lane tells whether NTG inputs differ independently.

## 10. State-changing actions

- **Local:**
  - created the detached worktree `scratchpad/libmwt` (remove it with `git worktree remove`);
  - created scratch builds in `scratchpad/ucrtbuild` and `scratchpad/winshim`;
  - ran Ghidra DecompileTargets on the existing `C:\tools\ghidra_proj`. The project was saved, and the output is in `scratchpad/sx/decomp`.
- **VPS:**
  - read-only greps and hashes of logs, cfgs and mod files, plus `/proc/<pid>/maps` and environ;
  - one ~1 s `python3 -c` evaluating 8 `sinf`/`cosf` values;
  - nothing was written and no service was touched.
- **strelka:** unreachable after the first scp of `~/tpf2-re/linux/{funcsig,functions}.csv` into the scratchpad.
- **Pushed:** nothing.
