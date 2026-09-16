# Linux port checkpoint — 2026-09-15

## Current integration — 2026-09-16

Windows dev through `b5dade06` (0.5.7 plus later changes) is staged, uncommitted,
and partially ported. See [UPSTREAM_dev_b5dade06.md](UPSTREAM_dev_b5dade06.md)
for current coverage, tests and native recovery/in-world-load gaps. Historical
checkpoints below describe their own revisions, not current live validation.

## Upstream update — 0.5.3

Merged upstream `main` through `66da00db6a36791630d7ebb05b6bf81fb06e7959`
(release 0.5.3), retaining the native port and the desync fixes below. Resolved
lobby/mod-share conflicts while preserving Linux paths and orderly shutdown.
Ported protocol 5 and the bridge's world/lobby reset and readiness files.
The release Lua guard now checks all 28 files against 0.5.3, and lab creation
uses that same baseline instead of the historical Proton package manifest.

See [UPSTREAM_0.5.3.md](UPSTREAM_0.5.3.md). The new source builds and passes
isolated tests; a new native/Windows 0.5.3 game replay has **not** been run.
The running lab and published binaries were not replaced. Do not mix these
new Lua files with the running 0.4.22 libraries or lobby.

## Previous baseline — 0.4.22 replay with tree draw-order fix

The exact replay exposed missing public-transport/PathFactory hashes and two
missing traversal stages. Those adapters now match route choices and arrival
waiting times. Further allocation tracing found swapped scale/rotation draws
in town tree placement: Linux created three trees where Windows created four,
shifting subsequent building IDs. The native tree adapter now draws scale
first, then rotation, matching Windows and its rounded unit-float endpoint.

See [LINE_COST_DESYNC.md](LINE_COST_DESYNC.md) for evidence, addresses and tests.
All 29 SDK CTests pass, including inline ABI, RNG state and rollback checks.
All 24 Windows 0.4.22 Lua files remain byte-identical.
Current native boot SHA-256:
`78d89ebfdeb4edf650e50735995bed535fcd005d97dcc9bb43d6cdbf830655e8`.
Installed only in the lab. Exact replay confirms current person state through
t=3600 in 12 snapshots (excluding previous-frame interpolation history). The previously
mismatching town-growth event now produces identical records for all 13 checked
entities, including all four trees. The completed replay reports zero desyncs;
all vehicle samples have zero separation, and all common geometry hashes and
spatial person counts agree. Both lab instances are paused for further testing.
Evidence: `~/.cache/tpf2mp/resume-20260915/desync/tree-draw-validation/`.
The Linux port and these fixes are recorded together on the `linux-native` branch.
Published packages and the main installation remain unchanged.

## Earlier result — manual play test failed

The resumed candidate still desynchronizes. In the user's two-station/two-vehicle
play test, positions and sampled spatial person counts match through t=2124.
At t=2136 spatial person counts differ (native 346, Proton 347), while vehicle
positions still match. At t=2148 one vehicle differs by 11.89 m (mean 5.95 m),
triggering a desync on both peers; counts are native 343 versus Proton 346.
At t=2160 maximum vehicle separation reaches 54.61 m. Geometry verdict hashes
remain equal. No ordering-adapter error was found in the inspected native log.
Subsequent diagnosis identified a missing public-transport cost hash adapter,
as described above. The old aggregate samples do not prove full state parity.

The prior passing samples remain valid only for their recorded time range.
Logs from both actors were preserved under
`~/.cache/tpf2mp/resume-20260915/desync/`, with a SHA-256 manifest.
The user interrupted the requested commit after spotting the desync: changes
are staged, but no commit has been created. The later fix is under validation.

## Resumed session — 2026-09-15 22:40 UTC

Recovered Codex transcript `01a0a14c-7867-7d22-bf20-17322a9af73f`.
The interrupted persistent person-route index adapter existed in source but was
absent from CMake and boot initialization. It is now compiled and installed after
the target adapter, whose constructor signature must be checked first.

Added `network_index_order_test.cpp`: executes all ten installed hooks at both
stack alignments, including both loop branches; checks general/vector registers,
stack balance, relevant flags, insertion/duplicate/erase handling, incomplete
history fallback, instruction guards, and rollback at each installation stage.
All 29 soldier SDK CTests pass. All eleven index instruction contexts match the
installed native ELF. All 24 mod Lua files still match Windows 0.4.22 exactly.

Candidate boot SHA-256:
`4c63bbe2b6bb3b0c9bae3a35ed613caae18e4ec821103bbd1cb0273750f6950b`.
Installed only in the native lab. The prior lab boot is backed up under
`~/.cache/tpf2mp/resume-20260915/libtpf2mp_boot.before-index.so`.
The native game reaches its main menu, and startup logs confirm resident,
temporary-map, target, temporary-network and persistent-index adapters enabled.
Main installation and published assets were not updated.

**Subsequent manual play test:** the user built two stations and assigned two
vehicles. All 24 common samples from t=1800 through t=2076 have matching verdict
hashes and spatial person counts at equal sample times. At t=2076, both vehicle
positions match (mean/max difference 0.00 m); the logged 20-sample position trend
is flat at zero. Both peers reported zero desyncs at t=1920, and no ordering
adapter errors were found in the inspected native startup/runtime log.
See [VALIDATION_2026-09-15.json](VALIDATION_2026-09-15.json).

**Still pending:** exact first-terminal reproduction and full person-state
snapshots. This manually driven test does not establish complete simulation
parity or complete capture of every index history. The user is continuing play.
Evidence and build output are under `~/.cache/tpf2mp/resume-20260915/`.

## Previous checkpoint

The `linux-native` branch now uses pushed Windows **release 0.4.22**, commit
`7990a86`, as its baseline. The previous Linux edits were preserved while
fast-forwarding to that release. At that earlier checkpoint, source changes were uncommitted. The experimental native build has been
published as `v0.4.22-linux-dev.1`, with a curated source overlay and provenance;
the release tag alone points to the Windows baseline, not the Linux source.
The frozen `.desync.5` artifacts are now published as `v0.4.22-linux-dev.2`:
https://github.com/silver2127/tpf2-multiplayer/releases/tag/v0.4.22-linux-dev.2 .
All eight uploaded assets have matching GitHub SHA-256 digests; release notes
and prerelease status were verified at 2026-09-15 02:37:28 UTC. That tag identifies baseline commit
`7990a86edd941dd65fe0350a22d28fbc5757abab`, with the uncommitted Linux sources
supplied by the curated source overlay.

**Current known failure after resuming that validated world:** after four street
terminals, a road depot, a line and two coaches were created, spatial person
counts first differ at t=1896 (native 325, Proton 324). Both coach positions still
match at that sample; the first coach-position difference is t=1908. Geometry
verdict hashes remain equal. All 12 player commands applied at the same stamps
on both peers with zero replay lag, including coach IDs 28361/28362 assigned to
line 20125 at t=1859.4. The exact t=2100 and 2124 movement snapshots both complete
with zero capture errors and confirm persistent person and position differences.
Both populations are 720, but three new residents of home 25816 reuse different
entity IDs. Pairing all 720 people by home and personal attributes still leaves
48 differing people at t=2100 and 55 at t=2124; this is not solely an ID-label
difference. Person 25873 has the same car path and destinations but different
progress. Removal/free-ID order and the transport differences are under
investigation; their causal relationship is not yet established. Both worlds
were saved around t=2124 and paused by the root operator. Evidence is under
`~/.cache/tpf2mp/desync-fix-20260914/post-release-desync/`, including
`command-timeline.json` and `movement-live-2/`. The earlier 30-minute baseline
proof below remains valid within its stated scope; this later play test fails.

A fresh reproduction now shows that the **first terminal alone** is sufficient.
Only original CONX sequence 1 was replayed at t=1814.6, with identical wire and
execution stamps. Request `05233c5df947` has 12 complete snapshots and zero capture
errors. Full state matches at 1800, 1814, 1815 and 1824. At 1832/1833, three new
residents of home 25816 have permuted recycled IDs; their logical records still
match after pairing. At 1842, two existing residents, 20869 and 26507, differ in
leave/stay movement state beginning at 1840.6. Those two logical differences
persist through 1908. No buses or subsequent player commands were queued.

A bounded debugger trace captures identical building inputs and removed-ID
multisets, but different resident-container and temporary-map iteration orders.
The first 22 engine removals match; the 23rd is native 20852 versus Windows 20853.
Both games append retired IDs in removal order. Evidence is in
`post-release-desync/first-terminal-trace/` and `first-terminal-repro-live/`.
The five temporary Windows maps' order has been independently reproduced against
original executable instructions (1,088 cases / 575,042 input items).
The resident-hash adapter independently matches original Windows instructions
for 100,029 entity hashes and 300 complete per-operation container states,
including growth, erasure, reinsertion and tombstone compaction. Full control
bytes, occupied slots, iteration order and growth allowances match. Reproducible
oracles and reports are in `removal-order-audit/resident-oracle/`. Resident-hash
and temporary-map traversal adapters passed 25 SDK tests and were installed in
the native lab as candidate `c307f074...`; the main installation and published
release retain `331abcbd...`. A second exact first-terminal replay still fails:
full state matches through 1824, recycled IDs differ at 1832, and real movement
differences appear by 1842. All snapshots complete without capture errors.

The extended trace identifies two further ordering stages. The persistent
`GetSimPersonsForTarget` set already supplies different first-insertion order.
Separately, the temporary affected-network set receives vector batches with
identical members but different order. Those batches preserve the traversal of
another persistent index at `SimPersonSystem+0x90`, keyed by transport-node
pairs. Thus correcting only the temporary set is insufficient. Target-set and
temporary-network adapters have focused tests and Windows instruction oracles;
the upstream persistent-index correction and full-chain live validation are
pending. No later adapter is installed in the current lab. The V3 debugger has
been detached, with all 24 breakpoint locations restored; both worlds are paused
at 1908. Evidence is in `network-person-order-audit/` and
`network-input-v3-live/`. The dev.2 release notes disclose the failing play test;
its eight downloadable assets are unchanged.

The current combined lab candidate, boot SHA-256
`331abcbdc5bd11d0d342d7ac5ced9902323135fc06f0097a68dcd0f7a549135c`,
passes all 21 SDK CTests and the independent Windows machine-code oracles below.
It adds scoped animal RNG compatibility, the inlined tree-choice distribution
and Windows building-registration ordering. It is installed in the lab and was
used for the following fresh-baseline test. Request `84f6725790e8`, schema 3, captures
t=300,600,744,756,780,900,1200,1800 under
`~/.cache/tpf2mp/desync-fix-20260914/people-creation-live/`.
The t=300 and 600 captures complete without errors: all 718 people, current path
state and world transforms match (440 and 364 moving respectively). The full
comparison fails because previous-state fields differ (`MOVE_PATH.dyn0` and
`MODEL_PERSON.dist0`). Exhaustive leaf comparisons find no differences outside
those two explicit history fields. Both checkpoints ran at speed 4. Root set
speed 1 before t=744 to check frame-history grouping; `speed-phases.json` records
the observations. At t=744,756,780,900,1200,1800 the complete snapshots match exactly, including
previous state, with zero capture errors. At t=780 both games have 721 people:
new residents 28228–28230, home 28224, and all their personal attributes match.
The first growth/birth event now passes. All 151 common game-hash stamps from
0 through 1800 match, including spatial person counts at equal sample times.
The raw report preserves the earlier speed-4 history differences. This verifies
the captured state through 30 simulation minutes; broader play and complete
simulation coverage remain unproven. The three new residents stayed home in
every captured checkpoint, so their first journeys were not exercised.

Preceding town-seed test: that candidate passed all 17 SDK CTests and was
installed in the native lab actor only; at that checkpoint the main native
installation retained the earlier combined candidate. In that clean-baseline run, game hashes matched
through t=780, including the previously failing t=756 town-growth event. Complete
exact-time snapshots at t=600,744,756 match all 718 people, their captured path
progress and world transforms, with zero capture errors. At t=780 the original
718 records still match, but the three newly created residents have different
entity IDs, home IDs and personal attributes. Both populations are 721 and both
moving counts are 343. That entity-allocation difference motivated the animal/tree/building changes
now verified for the first growth event in the current candidate. Extended
deterministic play and complete simulation coverage remain unestablished.

The Windows-compatible Lua decimal tie formatter passes the clean baseline and
earlier rail comparison. Both lab actors discover the same 27 mods, with Legacy
Vehicles disabled, and retain the exact 24 Windows 0.4.22 Lua files. The native
compatibility changes now include the common Boost MT integer distribution, ten
verified person seed callbacks, the unit-float endpoint behavior, five person
travel-cost hash sites and the town-development time/tag seed. Both games run in
Steam Offline Mode. The previous combined candidate's six exact person snapshots
through t=600 remain valid historical evidence; the new run uses the corrected
movement diagnostic and is recorded below.
The earlier Steam `Logged In Elsewhere` shutdowns
and the temporary offline settings are recorded below.
The earlier departure-only diagnostic `bd88d4ab2a68` completed
t=30,60,72,144,300,600 and is archived under
`~/.cache/tpf2mp/desync-fix-20260914/people-departure-live`.

The recovered Claude Code session was
`57612d9e-d5c4-42f7-8507-30775a5403ac`. Its interrupted workflow had 50 completed
and 15 failed tasks. The continuation uses its RE evidence and implementation,
with new integration, decoder, packaging and review work described below.

## Implemented

- All five loader components build: boot, bridge, Vulkan menu, slice and plugin
  host. Menu startup wires autoload, hot join, forced autosave and pre-start mod
  enablement. The `slot=0..7` menu position setting matches Windows.
- Vehicles, lines, time, road/track proposals, construction, terrain and assets
  have Linux decoders. Cancellation commits the serialized action only after
  the core validates the command and completion callback. Capture failure blocks
  a known player action in multiplayer; it cannot fall back to native execution.
  The existing Windows 0.4.22 capture format and replay fields remain the contract.
- Strict line creation preserves the editor callback until its own scheduled
  replay using the unchanged Windows single-integer claim file and existing
  payload. Claims are consumed once and matched against held commands. Clock
  hooks preserve nested calls and foreign-runtime unwinding.
- Terrain/assets replay allocates through the game's allocator, initializes
  objects at their final addresses and validates the actual construction
  entry constructor. Tool holds are invalidated before destruction and checked
  again after callbacks, including address reuse.
- Road/track handling includes release-0.4.22 mixed crossings, in-place bridge
  replacements and split-parent omission. Shared Lua includes the release-0.4.22
  previews, simulation fixes and experimental Natural Town Growth support.
- Every game-code patch uses the shared `/proc/self/mem` writer without changing
  game page permissions. The hook decoder handles 16-bit immediates. Short
  near-jump hooks preserve subsequent RIP-relative instructions. Actual hook
  success is distinct from a retained trampoline after an uncertain write.
- Lobby paths, child environments, save/mod/log discovery and cleanup work with
  Linux and Snap layouts. The relay leadership status follows release 0.4.22.
- Native boot publishes Linux paths through the environment expected by the
  unchanged Lua. Host/join and queued game starts require successful cancellation
  hook installation for this exact game process; missing or failed hooks refuse
  startup. This checks installed hooks, not coverage of every game action.
- Native boot now matches the Windows game's decimal halfway rounding at the
  verified build-35924 Lua floating-format call. The caller-gated
  `__sprintf_chk` adapter corrects exact ties for single `%f`/`%F` conversions
  under `FE_TONEAREST`; numeric game state, Lua, hash logic, buffer lengths and
  the floating-point environment are unchanged. Other callers, formats,
  unknown builds and directed rounding modes retain libc behavior.
- The verified common Boost `mt19937` integer overload at RVA `0x31b79c0` now
  uses Windows modulo/rejection sampling and consumes no draw for singleton
  ranges. This covers every caller
  of that overload, extending the earlier two-caller destination adapter while
  retaining the game's original MT state and raw draw implementation.
  This does not replace every random-engine overload. The verified animal
  LCG paths are separately adapted to Windows standard MT below; other unbound
  standard-MT and LCG consumers retain their native behavior.
- Ten verified person callbacks now use Windows time/tag/entity seed hashing,
  covering departure, walking/idle transitions, line changes, vehicle changes,
  terminal changes and path changes. Other constructor callers retain their
  original seed. The verified unit-float sampler keeps a rounded result of 1.0,
  matching Windows; only the native clamp branch is replaced with two NOPs.
- Five verified travel-cost sites now use the Windows person-ID/salt hash for
  reachable walking/driving and actual path costs, including both drive-year
  branches. The original modulo and SSE arithmetic remain in place. The stubs
  preserve live registers, flags and stack alignment; all instruction contexts
  are checked before any site is patched. These compatibility changes modify
  native execution, without changing installed Lua or the capture protocol.
- The verified town-development update now seeds its inline MT initializer with
  the Windows tag-21/time hash. The adapter changes the six-byte seed store at
  RVA `0x1746854`, checks the complete surrounding instruction context and
  preserves the original initializer, registers and flags. The separate local
  town-developer MT uses the raw town entity ID on both platforms and is unchanged.
- The current combined candidate uses Windows animal worker seed hashing and a scoped
  standard-MT sidecar for the verified worker/spawn draw sites. The scope is tied
  to each invocation and exact local engine address, including nested calls,
  threads and foreign-runtime unwinding. Other RNG consumers retain their
  existing routing. The complete escape audit identifies five float and three
  integer consumption sites in these animal paths.
- Tree model selection now handles the inlined sampler inside
  `CreateBuildingAssets`, which bypasses the common integer overload. It uses
  Windows modulo/rejection while retaining the original native MT raw draw and
  twist, including no draw for singleton lists.
- Before a fresh `BuildingTypeRep` registers its indices, the current candidate
  restores ascending construction IDs and applies the Windows sort's exact
  priority/year ordering, including its equal-key permutations. Native and
  Windows had the same construction/model catalogues but different internal
  building order: one residential subset was native `[01,04,03,02]` versus
  Windows `[01,02,03,04]`. This explains different house variants at the same
  random index. The adapter changes that constructor list before registration.
- Portable native builds use a checksum-pinned Steam Runtime soldier SDK and
  require glibc no newer than 2.31. The frozen lobby uses pinned Python and
  manylinux dependencies; its current maximum requirement is glibc 2.17.
- Packaging produces a `.run` installer and tarball, verifies checksums,
  installs a durable Steam launch wrapper and preserves user data on upgrade
  and uninstall. `--patch-runsh` is an explicit legacy option.

## Coverage limits

The strict native guard and construction replay implementation are in place.
Connected/snapped placements, internal station companions, ModuleBuilder/bulldozer
upgrades and Linux `MergeTemplateStreet` use Windows 0.4.22's existing
`ROADC`/`CONXP`/`CONUP` protocol and pass offline tests. A rejected capture is blocked;
the implemented paths still need live validation.
See [CONSTRUCTION_TERRAIN_IMPLEMENTATION.md](CONSTRUCTION_TERRAIN_IMPLEMENTATION.md).

All 24 Lua files now match Windows release 0.4.22 byte for byte. The proposed
capture expansion, generic proposal protocol, Linux Lua path changes and optional
line token were removed at the user's direction. `tools/linux/verify_lua_release.py`
checks source and staged package contents against commit `7990a86edd94`.
Native boot supplies the environment expected by that unchanged Lua.

The unchanged Lua's name/color and loan handlers always skip the origin. Windows
applies those changes locally first; loans are subsequently polled and replicated.
Until native origin-replay adapters are proven, those controls remain blocked in
strict sessions. Cancelling and shipping them as normal would update peers alone.
See [STRICT_REPLAY_GAPS.md](STRICT_REPLAY_GAPS.md) for evidence and next steps.

Vehicle stop/maintenance, loans and other uncovered known player controls remain
blocked in multiplayer. The guard recognizes verified native player call sites;
arbitrary Lua-issued commands still share the permitted script replay sinks.
Distinguishing synchronized replay from other script commands remains an
implementation gap. The port does not yet meet complete strict-only coverage.

The native 3D preview plugin is implemented and packaged, but defaults off
pending live GPU/scene validation. It verifies all eight hooks, uses the game's
exception runtime for native evaluation, and waits for a matching GUI/render
thread before announcing readiness. Any unsupported proposal, thread mismatch
or native failure leaves shared Lua previews available. See
[PREVIEW_PLUGIN.md](../re/linux/PREVIEW_PLUGIN.md) for layout evidence and the
opt-in setting.

A Windows/Linux LAN test completed host/join, save transfer and map load.
The peers already disagreed on edge geometry and height hashes at the first
loaded-world hash, before either player built rail. The first rail command
was cancelled, scheduled and replayed successfully on Linux; both peers gained
five edges and charged the same amount, but the baseline mismatch remained.
The desync warning appeared after the repeated-mismatch threshold. These early
results preceded the compatibility fixes and controlled passing checkpoints
documented below. They do not validate the current build over a physical
Windows/Linux LAN; late join, resync and extended play also remain unvalidated.

Background saving and Esc without pausing are future Linux proposals, not
features shipped in Windows 0.4.22. They remain outside this port baseline;
see [MENU_BRIDGE_PARITY.md](MENU_BRIDGE_PARITY.md).

## Validation

- Current `.desync.5` candidate: all 21 soldier SDK CTests and the complete
  installer regression pass. Six normal-speed schema-3 snapshots through
  t=1800 match complete captured person, path/current-and-previous-state and
  model-transform data. Two 4x snapshots retain history-only differences.
  All 151 common game-hash samples through t=1800 match. The exact protocol
  and newborn-travel limitation are recorded above and in the final table.
- Baseline validation before the formatter fix: Host Release and Steam Runtime
  soldier builds compiled/linked the five
  components plus the native preview plugin; dynamic exports are restricted.
  Ten host CTests passed. CTest covers actual detour and
  trampoline execution, callback validation/ownership, proposal serialization,
  terrain/construction ownership, and command codecs. The host also runs the
  Lua wire-format check and the actual preload environment against unchanged Lua;
  the SDK has no Lua executable. Eight SDK CTests passed at that checkpoint.
- Formatter-fix validation at that checkpoint: all ten SDK CTests and three focused host
  checks pass. The complete 718-edge fixture matches the observed Windows
  geometry/height hashes after correcting the four exact halfway values, and
  a real additional height difference still changes the hash. All 2,062 Windows
  formatter oracle cases match under `FE_TONEAREST`. Passthrough checks preserve
  fortified bounds and unrelated calls. The local
  `0.4.22-linux-desync.1` package passes the full installer regression.
- Destination-RNG validation: all 12 SDK CTests pass, including the two new
  checks. The compatibility helper matches 3,978 calls through the original
  Windows machine code, including draw counts, and a 25,000-case MT oracle.
  These are offline results; the first exact t=144 live snapshot still shows
  residual destination differences, so the helper oracle is not full-world proof.
- Combined people-candidate validation: all 16 SDK CTests pass. The cost helpers'
  full 64-bit hashes and final float bits match the original Windows instructions
  in 220,006 cases per helper. The actual five patched stubs pass 200 isolated
  executions with both stack alignments, all 15 general registers, all 16 XMM
  registers, flags, MXCSR and stack restoration checked. Signature rejection and
  both successful and failed rollback paths are covered. These are offline and
  startup results; the combined world comparison also passes all six exact
  snapshots through t=600, with all 718 captured person records and capacity
  callback order equal. Later schema-3 path-progress/model-transform results
  are recorded in the final table below.
- People-probe validation: eight isolated regressions pass with real Lua5.2,
  covering unchanged clock returns, multiple targets, self-restoration,
  world-clock-reset cleanup, late/duplicate rejection, JSONL output, exact-time comparison and dry-run
  isolation. The diagnostic uses existing EVAL and changes no installed Lua.
- Sandbox mod-layout validation: six isolated regressions pass, including real
  bubblewrap discovery through ordinary directories, read-only shared contents,
  refusal to migrate an active actor, and cross-actor exclusions through the
  Legacy removal watcher's transition. Both restarted games now report 27 mods.
- Native command tests include a dynamic-runtime foreign exception crossing a
  hidden-static-runtime detour. Terrain/construction tests additionally map
  the actual game ELF and validate the game constructor and byte gates.
  Decoder and preview suites were run with ASan and UBSan. Preview tests cover
  draw/ACK behavior, scene/thread readiness, every partial hook-install failure,
  terrain composition, disposal and foreign game exceptions.
- Release-0.4.22 offline regressions passed for boot resilience, bridge
  companions, clone-buy, crossings, deterministic scripts, edge demolition,
  large-map hashing, heal step locks, land replay, strict/rapid line edits,
  shared previews and rendezvous. Natural Town Growth tests used the installed
  Workshop script instead of the upstream test's Windows default path.
- The rebuilt 0.4.22 lobby passed its five local self-tests: direct, relay,
  mesh, mods and transfer. A local delayed-rendezvous regression reproduced
  shutdown taking 6.23 s against the launcher's 3.5 s budget; the Linux daemon
  poll no longer blocks cleanup, and the same scenario finishes in 3.22 s with
  listing removal and mapping cleanup complete. All 16 lifecycle assertions and
  the upstream rendezvous tests pass.
- A real build-35924 game launched under `snap run --shell steam`, using
  isolated development libraries and data. All 15 factory hooks and three
  auxiliary slice hooks installed. The menu frame hooks and Vulkan overlay
  initialized, and the title screen displayed MULTIPLAYER. The process closed
  through its normal window-close event. This test used no saved game and did
  not install files into the user's game. A second startup loaded the enabled
  native preview plugin, installed all eight of its hooks and returned OK. The
  sample plugin verified the real translation-function bytes and returned OK.
  This second process also closed normally without loading a world.
- A third real startup used the rebuilt strict-only guard, unchanged-Lua boot
  environment and startup readiness publication. All 15 requested factory hooks,
  three auxiliary hooks and the Add cancellation hook installed; the live log
  reported `multiplayer hook readiness: ready`. The MULTIPLAYER title entry was
  visible. No world or peer was loaded in this check.
- Installer checks exercise extraction, checksums, dry run, wrapper environment,
  upgrade, legacy migration, uninstall and preservation of user files. The
  expanded check compares every packaged file through upgrade and uninstall,
  retains internal symlink aliases and rejects malformed loading upgrades
  before modifying any installed file. It passed against the complete 19 MiB
  `.run` artifact, including the preview plugin and frozen lobby.

## Development artifact

`dist/linux/tpf2mp-linux-0.4.22-linux-dev.run` and the matching `.tar.gz` contain
all five libraries, the optional native preview plugin, the latest shared mod,
the frozen lobby, installation scripts and checksums. This experimental test
build is published at https://github.com/silver2127/tpf2-multiplayer/releases/tag/v0.4.22-linux-dev.1 . It has been rebuilt
with the strict guard, native boot compatibility and startup readiness check.
Source and packaged Lua both match the Windows release byte for byte, and the
rebuilt installer passes its extraction/install/upgrade/uninstall checks.

The newer local formatter-fix package is
`~/.cache/tpf2mp/desync-fix-20260914/package/tpf2mp-linux-0.4.22-linux-desync.1.run`,
with its matching tarball and staging directory alongside it. This package has
passed the full installer regression and has not replaced the published
`v0.4.22-linux-dev.1` artifact. Only the lab native actor's boot library was
updated for that formatter comparison; its SHA-256 was
`4d894c237c22f2f1e0af301e6778ebe3d34e80d01340227844abeb6e3e97a9f7`.
The first integer-RNG-validation native boot SHA-256 was
`383ba72ec24743d7a86e34a5ffe296efd0f2d8dae978a3fb034ae448814b6cec`;
that process logged both destination callers enabled. The preceding combined
candidate boot SHA-256 is
`77e948f4efd949d15219778a1e9b256f91cbf466ea5e600e57dc23cc70d81515`,
which was installed at `~/snap/steam/common/.local/share/tpf2mp/libtpf2mp_boot.so`
before the final creation-fix update.
It passed all 16 SDK CTests. The frozen combined package is
`~/.cache/tpf2mp/desync-fix-20260914/package-desync.3/`, containing the
`0.4.22-linux-desync.3` installer and tarball. Its packaging metadata accurately
records live validation as pending at packaging; the adjacent
`LIVE_VALIDATION.json` records the subsequent six-checkpoint live result without
changing the frozen archives. This local package has not been published.
The preceding town-seed candidate boot SHA-256 is
`53ff332c01f09a0bdea5a4591324cb3d018a3f65dc2dac9bd5b7dcecfa38173e`.
It passed all 17 SDK CTests and was installed only at
`~/.local/share/tpf2mp-lab/native/share/tpf2mp/libtpf2mp_boot.so` for the new live
comparison. The frozen `desync.3` package does not contain this later town fix.
The lab now uses `331abcbdc5bd11d0d342d7ac5ced9902323135fc06f0097a68dcd0f7a549135c`
with the animal/tree/building changes and 21 passing SDK tests. Its fresh live
comparison is recorded at the top and in the final validation table below.
The immutable `.desync.5` package is in
`~/.cache/tpf2mp/desync-fix-20260914/package-desync.5/`. It includes the `.run`,
tarball, 152-file curated source overlay and checksums. All six native libraries
match the frozen SDK build, the complete lobby payload matches `.desync.4`, and
all 24 Lua files match Windows 0.4.22. Embedded notes say live validation was
pending when packaged; the external release notes, `LIVE_VALIDATION.json` and
`PACKAGE_VERIFICATION.json` record the completed test without altering archives.
The published `v0.4.22-linux-dev.2` contains these exact archives, the final
validation/provenance metadata and checksums. `GITHUB_PUBLICATION.json` beside
the local archives records the verified server digests; `github-dev.2/` contains
the exact uploaded files. The earlier release and its assets remain available.
After the final t=1800 pass, the main native boot was atomically updated to
`331abcbd...` at 2026-09-15 02:32:35 UTC. All six installed libraries match the
frozen SDK candidate. The previous main boot is retained as
`main-native-boot-before-creation-fixes.so`;
`main-native-creation-fixes-install.json` records the installation. All 24 Lua
files match the baseline in the repository, main install, shared game, native
lab and Proton lab. The protected-file audit found 40/41 unchanged; only expected
Steam appmanifest metadata differed. Both comparison games remain paused.
The previous boot library and pre-restart comparison data are retained under
`~/.cache/tpf2mp/desync-fix-20260914/`.
The main native update was installed atomically after the six-checkpoint pass;
the previous main boot (`e05e87ce0c7656a8306391ac1c24ec87829006a8ac0ffe06e2372bb973acec22`)
is backed up as `main-native-boot-before-people.so`, with manifest
`main-native-people-install.json` in that cache directory. No active process
mapped the old main boot, and the running lab PID 374454 was unchanged. Other
main native libraries matched the lab; installed Lua was not changed.

Earlier, at the user's request, the `0.4.22-linux-dev` package was installed into
the default Snap Steam game/data folders and launched through Steam. The existing `run.sh` preload
setup was upgraded. All libraries loaded, 15/15 factory hooks and 3/3 auxiliary
hooks installed, and cancellation readiness reported ready. Automod added
`mp_lockstep` to activeMods; installed Lua also passed byte-for-byte verification.
The subsequent LAN test found a UDP firewall block; a rule scoped to the
Windows peer and lobby port allowed joining and save transfer. The geometry
hash mismatch described above was then observed. The user then switched Steam
to Proton 11 and downloaded the Windows game depot for the following test.
The previous local installation and settings were backed up under
`~/.cache/tpf2mp/before-live-install-ho97y4gw`.

## Proton comparison

The installed Windows executable matches release 0.4.22 hook guards. The
official MSI has been downloaded and verified against the release SHA-256;
its 24 Lua files exactly match commit `7990a86`. The Proton setup in `tools/proton/setup.py` installed that Windows payload,
preserving stock audio and exposing Steam userdata/library paths in the prefix.
All 33 payload files verified, including the unchanged Lua. The bridge, menu,
slice and preview plugin loaded successfully with no DLL override needed.
The user joined a Windows host, received and loaded its save, and reported no
desync. Captured logs show SYNC and successful remote proposal replay. All seven
common hash stamps from t=0 through t=144 match, including geometry after
remote proposals at t=126 and t=129.8. The original Windows
lobby failed hosting before publishing a code, exiting with `0xc0000005`
after its initial observing-NAT status. An isolated reproduction confirms the bundled miniupnpc DLL has 64 duplicate
DIR64 base relocations. Wine applies the relocation delta twice to its TLS
index pointer, exactly reproducing the crash address. The pinned repair in `tools/proton/fix_lobby_relocations.py` neutralizes only
those duplicate records and repackages the lobby. All other 73 archive member
contents are preserved exactly. Its isolated Proton host test completed NAT
discovery, published a code/roster and exited cleanly. The repaired lobby is now
installed via `setup.py --repaired-lobby`; all 33 installed files verify against
the selected manifest, including the 24 unchanged Lua files. See
`docs/proton/NAT_CRASH.md` and `docs/proton/INSTALL.md`. This tests
Windows executable parity; it does not prove determinism under Wine's CRT.
Stock Windows DLLs also retain native fallback paths and do not inherit the
Linux cancellation guard. Complete strict-only coverage remains unresolved.

## Native/Proton sandbox lab

At the user's request, `tools/sandbox/tpf2mp-lab` now prepares two filesystem-isolated
instances at `~/.local/share/tpf2mp-lab`, sharing installed assets read-only and
keeping saves, settings, multiplayer data, caches, and Proton prefix separate.
The exact native depot `1066784` / manifest `2694881574364695876` was downloaded
through Steam's console without switching the installed Windows game. Its ELF
build ID is `3a0e156390b0e6f1e372051c24802c8493ae454a`, matching the native hook build.

Both build-35924 games reached their multiplayer menus concurrently, closed normally,
and restarted through the desktop launcher wrapper. Native reports all 15 factory
hooks, three auxiliary hooks, Add cancellation, and readiness; the bridge ports
are `a/7771` and `b/7772`. Proton loads the Windows modules and the repaired lobby.
The installed shortcuts are **TpF2 Test — Native Linux** and **TpF2 Test — Proton**.
Native was positioned left and Proton right for the initial session. The later
joined-world comparison and its findings are recorded below; the post-fix clean
test is now in progress.

Both actor copies retain the exact 24 Windows Lua files. Setup seeded them with
the original 18 MB `New Game.sav` used for the failed native comparison. Existing `dump_egeo=1`
diagnostics are enabled only in their private game configs. Old copied native
lobby files were archived outside the lab; fresh setup copies only the lobby
executable and dependencies. Original saves, settings, and monitored game/mod
files still match the pre-lab checksums; the outside Steam client updates its
play metadata, with Windows depot selection unchanged. Setup uses Steam's own
container helper, private runtime locking files, and private shader-cache mounts;
no host AppArmor or namespace policy was changed. Details and test procedure are
in `docs/sandbox/INSTALL.md`.

The user subsequently joined the two live sandbox instances. Read-only evidence
at `~/.cache/tpf2mp/lab-live-geometry-20260914-231233/` captures both at t=156 with
718 edges. The only eight differing geometry strings involve four shared nodes
whose saved float32 heights are exactly 69.25, 8.25, 7.25, and 2.25. Native text
rounds them to .2 while Proton text rounds them to .3. Replacing only those four
node-height representations in an offline copy makes all 718 strings, and both
geometry/height hashes, match Proton exactly. These hash values also match the
original native-versus-Windows failure. Both saved files from that comparison are byte-identical.
This strongly identifies decimal formatter tie rounding as the geometry hash
cause; live raw engine coordinates have not been read, and the separate people
count began diverging at t=72. The formatter fix above is now installed in the
native lab actor. The subsequent clean live comparison confirms the geometry
fix; the separate people movement divergence persists with the same catalogue.

The initial native session loaded Legacy Vehicles while Proton omitted it.
The requested removal completed after native PID 293884 exited:
`native/legacy-removal.json` now reports `removed`, finished at
`2026-09-14T23:35:09.097581+00:00`. Its former
`game/mods/urbangames_legacy_vehicle_pack_1` link is preserved under
`disabled-mods/`; the original shared assets and old save dependencies remain.
Both old game sessions have now exited. Their saved world and logs were preserved
under `~/.cache/tpf2mp/desync-fix-20260914/live-before-restart/` before the clean
**Desync baseline** test was prepared.

The mod discovery mismatch is also corrected: Windows skipped stock-mod
directory symlinks, producing six catalogue entries versus native's 28. The
launcher now creates ordinary directories and supplies each stock mod through a
read-only bind mount, without copying its assets. Migration runs only while that
actor is inactive. Disabled-mod markers and the pending Legacy removal request
apply across both actors, so discovery repair cannot restore Legacy on Proton.
Both restarted games now discover **27 mods**, with Legacy absent from both.
All 24 multiplayer Lua files remain byte-identical to Windows 0.4.22.

The clean comparison now passes all 26 geometry/verdict samples through t=300.
The user's Proton rail build (`ROADP` seq=1, origin=b) replayed on both actors
at exactly t=114.8, lag=0, adding two edges. Both reported success and matching
post-build hashes (`1097537380-0793652002`). Evidence is archived at
`~/.cache/tpf2mp/fixed-desync-independent-20260914/`. Closing the title lobby
panel inadvertently invoked LeaveLobby, so this local comparison uses direct
loopback bridges a/7771 and b/7772; it does not revalidate the lobby relay.

Both people snapshots now cover exactly t=600, with post-read clocks still at
600. Global `simPersonSystem.getCount()` and component enumeration report the
same 718 people, with identical IDs, names, speed, person model/color and home
destinations. Native has 352 moving people and 366 inside buildings; Proton has
364 moving, 352 inside buildings and two idle. Spatial `n` equals only the moving
set on both. Shopping destinations differ for 667 people and work destinations
for 673; 708 people differ in at least one destination. `lastDestinationUpdate`
differs for only two people. This confirms destination/movement divergence,
not population loss or a rendered-person count. The early spatial trajectory
repeats within each platform, first differing at t=72 (158/159).

The defect is confirmed in the integer RNG mapping: native `Random` at RVA
`0x31b79c0`, through inclusive helper `0x31b6070`, maps draws using bucket division;
Windows `Random` at `0x2374d60`, through `0x955010`, uses modulo/rejection and draws
nothing for a singleton range. The initial native `DestinationRandom` adapter was gated
to return PCs `0x150395b` (seed) and `0x14fe99f` (weighted choice), obtaining raw MT
draws through the unchanged native helper's full signed-range branch. Its
Windows machine-code/draw-count oracle and all 12 SDK tests passed. Its live
results follow below; neither matching population
nor matching geometry alone proves full cross-platform simulation determinism.

The pre-RNG-fix exact snapshots, hashes and diagnostic handling notes are archived
under `~/.cache/tpf2mp/desync-fix-20260914/people-600/`. Both temporary clock wrappers
restored themselves before reading; they returned the original clock values
and sent no simulation commands. The reusable multi-target diagnostic and
comparison are `tools/sandbox/people_probe.py`, with procedure in
[the sandbox instructions](../sandbox/INSTALL.md). The fresh run's existing hash
records show matching spatial counts at t=60 (133/133) and t=72 (159/159), but a
remaining t=84 difference (188/192). The first reusable probe rejected a new
global marker under the game's strict globals policy before installing a
wrapper; the tool now uses only closure/upvalue checks, covered by its Lua5.2
regression. A subsequent late t=60 arm also rejected without changing the clock.
Request `f36960695425` completed t=144, 300 and 600 on both actors, with report
artifacts under `~/.cache/tpf2mp/desync-fix-20260914/people-rng-fixed-future/`.
The completed t=144 snapshot has exact before/after clocks and identical 718
person IDs and destination-update times. Destinations still differ for 325
people (312 shopping, 314 work, no residence differences); movement is
285 native versus 287 Proton. The remaining destination-selection discrepancy
is being investigated before claiming a successful people-state fix. At t=300,
both still have 718 people, but 708 have different destinations (686 shopping,
674 work), with movement counts 442/440 and only three differing update times.
Further binary comparison identified another destination RNG difference:
`NoteAtBuildingPersonsLeave` seeds the native MT via identity time/tag values,
while Windows hashes their bytes with FNV. That seed compatibility change became
the subsequent departure-only candidate; the integer-range adapter alone had not
fixed the world.
At t=600 the same 708 people still differ in destinations, with movement
368/364 and global count 718/718. Names, personal speed, person model/color,
home, cargo type, travel times and reachable-line counts match for all people.
Car models differ for 206 (178 unset/set pairs and 28 different selected models).
Of the ten matching destination triples, seven remain uninitialized; the other
three still differ in trip phase/target, so destination equality alone is not a
complete movement check. A final read-only EVAL on both actors confirms
`restored=true` and no Lua upvalues on the original clock function. No pending
people probe remains from this run.

The departure-only candidate subsequently matched all 718 destinations and
moving-person counts at exact t=30,60,72. At t=30, 23 people nevertheless differed
in 24 walking/driving choices, with 12 last-move-mode and 14 car-model differences.
Later snapshots again diverged in movement and destinations. The complete run is
archived under `people-departure-live`; it did not establish people-state parity.

Further binary comparison confirmed identity-versus-FNV differences in the other
person seed callbacks and in per-person walking/driving cost multipliers. The
cost helpers are shared by reachability estimation and actual path creation;
native inlines them five times, including a separate 1900–1999 drive branch.
The combined candidate above corrects these sites together with the common
integer sampler and rare unit-float clamp difference. All five cost contexts and
replacement register sets were independently checked against the original ELF.
The inventory is `~/.cache/tpf2mp/native-access-order/cost-sites.json`.

The combined candidate's first startup, native PID 360617, logged all adapters
enabled. Steam's `logs/connection_log.txt` records
`RecvMsgClientLoggedOff('Logged In Elsewhere')` at 20:50:29 and again on retry at
20:52:39 local time. The client then shut down and the games lost their Steam IPC
connection before loading the comparison world. The first shutdown also produced
a depot HTTP assertion and segmentation fault at 20:50:42–43 during teardown;
those later errors were initially mistaken for the initiating cause. The observed
trigger is Steam's account-session logout, not a demonstrated native hook crash.
The Windows same-account session is being checked. These interrupted startups
supply no combined simulation result.

While Steam was inactive, its existing `config/loginusers.vdf` was backed up to
`~/.cache/tpf2mp/desync-fix-20260914/steam-offline-test/loginusers-before.vdf`
with mode 0600. Only `WantsOfflineMode` and `SkipOfflineModeWarning` were changed
from 0 to 1 for a temporary local offline trial, then Steam was started with
`snap run steam -silent`. After the final comparison, only those two preferences
were restored from 1 to their original 0 values at 02:33 UTC; other VDF bytes were
preserved and the private backup has mode 0600. Evidence is
`steam-offline-test/preference-restoration.json`. The current Steam process
remains offline because it has not been restarted; both games remain paused. [Valve's Offline Mode instructions](https://help.steampowered.com/en/faqs/view/0E18-319B-E34B-B2C8)
require cached account credentials and completed game initialization. The lab
launchers share the local Steam client but use their own UDP loopback bridge,
so direct local networking does not require Steam's online connection. Offline
SteamAPI/game startup succeeded: both games reached their title menus at 21:00
local time and loaded the same clean baseline at 21:02.
The post-install audit
`~/.cache/tpf2mp/desync-fix-20260914/combined-people-integrity-77e948f4.json`
confirms the expected native boot hash, 24/24 unchanged Lua files on each actor,
and 40/41 unchanged original protected files. The sole changed original file is
Steam's `appmanifest_1066780.acf`; its play metadata changed, while the original
game build/depot selection was separately verified unchanged.

The combined live comparison is now running with native PID 374454, image base
`0x650317ca0000`, and all five boot compatibility features logged enabled. Native
is `b/7772`, Proton is `a/7771`, and their direct loopback bridges report two
players with leader `a`. Diagnostic request `0373d10b0a8c` was armed at native
t=18.8 and Proton t=18.6, with artifacts under
`~/.cache/tpf2mp/desync-fix-20260914/people-combined-live/`.

| Exact time | Global people, native / Proton | Moving, native / Proton | Captured person records and capacity callback order |
| --- | --- | --- | --- |
| 30 | 718 / 718 | 53 / 53 | All match |
| 60 | 718 / 718 | 133 / 133 | All match |
| 72 | 718 / 718 | 159 / 159 | All match |
| 144 | 718 / 718 | 287 / 287 | All match |
| 300 | 718 / 718 | 440 / 440 | All match |
| 600 | 718 / 718 | 364 / 364 | All match |

The complete `people_probe.py compare` returned exit 0 and `allEqual: true` for
all six targets, with no destination or other captured-field differences. Its
report is `people-combined-live/report.json`. This verifies the observed person
fields, component-state membership and callback order at those exact simulation
times; it does not include position or complete path-progress comparisons.
The adjacent `hash-report.json` additionally records 60 matching common hash
stamps from 0 through 708, with equal sample times and spatial person counts.
The initial stamp 0 was sampled at simulation time 2.6 on both actors.
After t=600, movement diagnostic request `5f895ab0d515` was queued for t=720,780
to read path progress and model transforms. At t=720, all 360 moving persons'
world-transform matrices (16 floats each) match exactly, and geometry capture is
complete. All 48 present `MOVE_PATH` records and captured `SIM_ENTITY_MOVING`
fields also match. Simulation capture is incomplete because 312 walkers lack
`MOVE_PATH`, and the optional `speed0`/`pathPos0` properties were nil on both
actors, producing 192 diagnostic errors. These are probe-schema limitations,
not observed game-state mismatches. The corrected schema was subsequently used
in the new town-seed run described below; this earlier probe is retained as
historical evidence.

A later, actual town-growth divergence is distinct from the movement probe's
schema limitations. Hash stamp 744 still matches; at 756, native has 718 edges
and 581 buildings while Proton has 719 edges and 580 buildings. Both report
money 4,950,008 and 363 moving people. All original 718 edges match exactly;
Proton alone adds the street from `(-3073,-3462.3,44.2)` to
`(-3112.2,-3541,42.3)`. At 780 both have 582 buildings, while that extra street
persists. Binary comparison identifies another seed-hash difference in the town
development update: native RVA `0x1746790` combines tag 21 and time using
identity hashes before an inline MT initializer; Windows RVA `0xab1d20` hashes
the same inputs with FNV. This initializer bypasses the ten person constructor
hooks. The new `Tpf2mpWindowsTimeSeed(21, timeBits)` helper matches the original
Windows instructions in 100,017 boundary/random cases, including negative time
bit patterns. Evidence is `~/.cache/tpf2mp/town-seed-audit/windows-tag21-oracle.json`.
The scoped adapter is implemented in the new `53ff332c...` candidate. Its fixture
checks 44 register/stack cases and 1,035 actual native inline MT initializations,
including 646,875 matching MT outputs. All 17 SDK tests pass. Windows' separate
town-developer helper `0x91d580` directly seeds its local MT from the raw town ID
at `0x91d671`, matching the native raw-ID initialization at `0x14f60a4`; this
second seed path required no change.

Both actors loaded another clean baseline at 21:28 local for this candidate.
Native PID 388849 uses the new lab boot; the main native installation remains
on `77e948f4...`. Warm-up ran at speed 4 and returned to speed 1 near t=588.
Movement diagnostic schema 3, request `2648cc32ed4b`, captures complete person,
walker, car-path and model-transform fields at exact targets
600,744,756,780,900,1200. Evidence is under
`~/.cache/tpf2mp/desync-fix-20260914/people-town-live-600/`.

| Target | People native / Proton | Moving (walkers + car paths) | Complete comparison |
| --- | --- | --- | --- |
| 600 | 718 / 718 | 364 (299 + 65) | Exact, zero capture errors |
| 744 | 718 / 718 | 359 (313 + 46) | Exact, zero capture errors |
| 756 | 718 / 718 | 363 (308 + 55) | Exact, zero capture errors |
| 780 | 721 / 721 | 343 (290 + 53) | Original 718 exact; three new residents differ |
| 900 | 721 / 721 | 324 (249 + 75) | Original 718 exact; same three new IDs differ |
| 1200 | 721 / 721 | 329 (261 + 68) | Original 718 exact; same three new IDs differ |

Game hashes match through 780, so the preceding run's street/building geometry
divergence at 756 is no longer observed. At 780, native's new person IDs are
28236–28238 with home 28231; Proton's are 28228–28230 with home 28224. Their
names, speeds, models and colors also differ. All are still at home with
`lastDestinationUpdate=-1`; capture completed without errors. These new entity
allocations are the next unresolved difference, despite equal geometry hashes
and population counts. The earlier six-checkpoint person proof remains valid
for its observed interval and does not establish parity for new residents.

A later read-only allocation inventory at native t=1890.4 / Proton t=1891.2
adds evidence about static entities; these are not exact-time movement samples.
The street's node/edge IDs already differ by eight: native 28226/28227 versus
Proton 28218/28219. The preceding salmon allocation contains 41 entities on
native (28185–28225) and 33 on Proton (28185–28217), after matching wolf/gull
ID ranges. The new house is
`res_1_3x4_04.con` on native and `res_1_3x4_02.con` on Proton, despite equal
parcel parameters, capacity and construction seed -33461. Native has three
particle-system entities for that house, while Proton has two. The birth helper
seeds resident attributes from the home building ID plus resident index before
allocating the person entity, explaining why differing home IDs also alter
names, models, colors and speeds. The allocator itself uses FIFO retired-ID
reuse or vector append on both platforms. Animal creation and building-type
selection were investigated next, producing the fixes tested in the subsequent
candidate below. A separate tree-choice defect was proven:
native `CreateBuildingAssets` inlines bucket-division sampling around
`0x1546a18–0x1546a97`, bypassing the common integer hook; Windows' corresponding
`0x95cd16–0x95cd76` uses modulo/rejection and skips singleton draws. No further
fix is claimed in this candidate. Evidence is
`town-allocation-audit/` alongside the live comparison, with binary/row evidence
under `~/.cache/tpf2mp/town-seed-audit/`.

The subsequent animal/tree/building candidate addresses these three proven
differences and passes 21 SDK tests. Independent oracles execute the original
Windows instructions in private mappings; they do not invoke or modify a game:

- Animal unit-float outputs match the actual helper for 320,000 draws from 128
  seeds across repeated twists, plus 8,064 forced raw-value checks across all
  four rounding modes. The original Windows seed initializer and complete
  twist/temper/float arithmetic are retained in the oracle. The fixed prelude
  computes one draw from `ceil(24 / log2(2^32))`.
- Original worker seed hashing matches 100,042 boundary/random cases. The
  Windows standard-MT integer adapter matches the shared Windows inclusive
  distribution in 64,000 cases. Evidence:
  `~/.cache/tpf2mp/animal-rng-audit/oracle-report.json`.
- The original Windows tree sampler and complete Boost twist match the actual
  new helper's selected index and all 2,504 MT state bytes for 128,000 seeded
  cases; 118 forced-tail cases also match draw counts, including singleton and
  wrapped full-range counts. Evidence: `tree-oracle-report.json` in
  `~/.cache/tpf2mp/town-seed-audit/`.
- The original Windows building sort matches 1,928 exact ID permutations over
  520,827 items, including 486 forced heap-sort cases, duplicate keys, signed
  boundary priorities/years and insertion/partition threshold sizes. This
  verifies the actual Windows tie ordering rather than assuming a stable sort
  or filename tie-break. Evidence: `building-oracle-report.json` in that folder.

The resident-creation seed itself already matches: native
`0x1549c88–0x1549ca1` and Windows `0x9595b0–0x9595b9` both use raw
`uint32(homeEntityId + existingResidentCount + birthIndex)` before the inline
MT initializer. Attribute generation precedes person allocation at native
`0x154a0da` / Windows `0x959c4a`. No additional birth seed-hash conversion is
warranted by this evidence. The new combined candidate's fresh live comparison
completed through t=1800, covering animal creation, town growth and resident
births on this baseline. Its raw
reports and immutable archived snapshots are under `people-creation-live/` in
the desync-fix cache. Separate `nested-differences-*.json` artifacts classify
the exact differing field paths without changing the comparison or snapshots.

| Target | Population | Moving | Current fields / transforms | Full snapshot |
| --- | --- | --- | --- | --- |
| 300 (speed 4) | 718 / 718 | 440 / 440 | Exact | History differs: 76 car paths, 362 walkers |
| 600 (speed 4) | 718 / 718 | 364 / 364 | Exact | History differs: 64 car paths, 296 walkers |
| 744 (speed 1) | 718 / 718 | 359 / 359 | Exact | Exact, including previous state |
| 756 (speed 1) | 718 / 718 | 363 / 363 | Exact | Exact, including previous state |
| 780 (speed 1) | 721 / 721 | 343 / 343 | Exact | Exact, including all three new residents |
| 900 (speed 1) | 721 / 721 | 324 / 324 | Exact | Exact, including previous state |
| 1200 (speed 1) | 721 / 721 | 329 / 329 | Exact | Exact, including previous state |
| 1800 (speed 1) | 721 / 721 | 326 / 326 | Exact | Exact, including previous state |

All eight completed captures have zero geometry/simulation capture errors. The
current-field classification for the two speed-4 checkpoints excludes only
`movePath.value.dyn0` and `walker.value.dist0`; the cumulative raw comparison
retains those failures. The speed-1 checkpoints require no exclusions.
At 780, both actors create Beatrice Kelly, Gracie Williams and Luca Jackson with
the same IDs, home, models, colors, speeds and other captured fields. This
directly resolves the preceding run's first newborn mismatch for this baseline.
All three remain at home with `lastDestinationUpdate=-1` and no `MOVE_PATH`
through t=1800, so newborn movement has not yet been exercised. All 151 common
game-hash samples from stamp 0 through 1800 match both geometry and the sampled
moving-person count. The final 1200/1800 checkpoints ran at 1x after intervening
4x intervals; this was not a continuous 1x run. Both games were paused after
the final checkpoint.
At 1800, 279 walkers and 47 car paths match completely. All three new residents
remain at home with `lastDestinationUpdate=-1` and no movement/path component
in every captured post-birth sample; creation and attribute parity are verified,
but their first trips are not. `newborn-state-evidence.json` records these rows.
The final `hash-report.json` passes all 151 common stamps from 0 through 1800,
including spatial person counts at matching sample times.

`validation-summary.json` records these scopes and source-file hashes, including
the six complete snapshot passes and the two speed-4 history-only differences.
The raw movement command exits 1 because those differences are retained; the
hash comparison exits 0. `extended-speed-phases.json` records speed 4 between
the later checkpoints, returning to speed 1 before 1200 and 1800. Both final
snapshot logs show exact t=1800 and zero errors; the diagnostic restores the
original clock function before its final capture and does not re-arm afterward.

## Newly pushed Windows releases

After the user pointed out newer multiplayer mod handling, remote refs were
refreshed on 2026-09-14. Windows releases 0.4.29, 0.4.31, and 0.5.0 were pushed
during this session. The newest release found during that refresh was `v0.5.0` / `dcf7eaaf96114900eeb74b8e952a492c0e425274`,
published at 22:52 UTC. The live sandbox and native port still use 0.4.22.

The mod-download changes (`8268f5a`, merged before 0.4.29) already implement the
required feature: share-mods defaults on, hosts advertise save dependencies,
joiners check both installed files and the engine catalogue, downloads require
consent, and save loading waits for a matching catalogue refresh receipt. Reuse
`netpunch/modshare.py`, `netpunch/lobby.py`, the menu integration, and native
`workshop_register.cpp` rather than inventing another mod-management protocol.
The official package includes `tpf2_workshop_register.dll`. Legacy Vehicles is
not excluded by its DLC filter. These checks would detect its missing catalogue
entry, but the registrar handles numeric Workshop IDs; it does not repair local
mod-directory symlinks, and the downloader preserves existing installed folders.

This requires a coordinated upgrade: 0.5.0 enforces identical peer release
versions, ships 27 Lua files instead of 24, adds a build-specific native catalogue
registrar, and expects new bridge ownership capability `entity_owner_v1=1`.
The Linux equivalents must be ported before adopting that shared Lua. The user's
explicit Windows-0.4.22-Lua-unchanged baseline remains in place; this comparison
did not change installed files, Lua, or live sessions. The subsequent formatter
and sandbox discovery fixes above retain that baseline; the Legacy removal is
complete. No upgrade to Windows 0.5.0 has been applied.

## Reproduce

```sh
tools/linux/build_native.sh --build-dir /tmp/tpf2mp-soldier-build
tools/linux/build_netpunch.sh --test
tools/linux/build_release.sh --version 0.4.22-linux-dev
python3 tools/linux/test_installer.py dist/linux/tpf2mp-linux-0.4.22-linux-dev.run
```

Developer notes: [SLICE_COMMANDS.md](SLICE_COMMANDS.md),
[PLUGIN_HOST.md](PLUGIN_HOST.md), [MENU_BRIDGE_PARITY.md](MENU_BRIDGE_PARITY.md),
and the detailed binary maps under `docs/re/linux/`.

Recovery evidence remains in the original Claude session directory. The
pre-merge tracked Linux diff was backed up to
`/tmp/tpf2mp-pre-0.4.22-linux.patch`; the named pre-merge stash was retained.
