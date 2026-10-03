# Native terrain sidecar: dev 2b4fd093

## Scope and decision

The incoming Windows-branch commit itself supplies Linux source, nine new byte
checks and a synthetic hook-flow test. No Windows runtime source changes.
Retain this implementation as experimental, **terrain_sidecar=0 by default**
in both compiled fallback and shipped config. Static analysis confirms the ABI
and fields below; live ownership and completion cannot be inferred from them.
This supersedes earlier records saying the native implementation is absent,
without promoting their outstanding lifetime questions to proven contracts.

## Independent ELF verification

Read the actual lab Steam build 35924 ELF, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. All 39 manifest sites match.
Job `meta/live/` retains `elf-id.txt`, `verify-game.txt`, signature exports and
fresh objdump transcripts (`addtile.asm`, `save.asm`, `load.asm`,
`publication.asm`, `alignment.asm`). Exact bytes are in
[sites.json](../../../bigmap/linux/sites.json), checked at runtime before hooks.

- Serializer.cpp signatures anchor SaveGame `0xc7ec00` and LoadGame
  `0xc7ca40`. Save receives six reference arguments in SysV registers and
  SaveGameId as argument seven: `0xc7ec2a: 4c 8b 65 10` loads `[rbp+0x10]`.
  At `0xc7ec5c` the path length +8 must be zero; `0xc7ec95` reads name
  data/length +0x20/+0x28. This is libstdc++'s 32-byte string, three per ID.
  Save's return at `0xc7fd9d` loads EDX from rbp-0x488, then RAX from
  rbp-0x490 at `0xc7fda3`, supporting the two-register return shim.
- Load has a hidden unique_ptr result in RDI (saved at `0xc7ca7e`), context
  RSI, ModRep ownership wrapper RDX, ID RCX (saved in R13 at `0xc7ca55`),
  then R8/R9 and five stack slots `[rbp+0x10..0x30]`. The retained shim
  forwards these machine slots without constructing MSVC objects.
- AddTile `0xcf71d0`: RDI saved in R15, ESI entity in R13D. Inlined ECS
  assertions anchor component lookup, followed by tile coordinate accesses.
  `0xcf73f2` loads terrain+0x18 into RSI, computes
  `(y-y0)*nx+(x-x0)` and uses LEAs to multiply by 40. At `0xcf7419`
  R13D is stored at record+0, supporting the entity scan used by the hook.
  Record+8 is the vector pointer. `0xcf75ee..0xcf75ff` checks the shared_ptr
  control block and branches to copy-on-write for refcount >1; following code
  sizes the vector and bumps version at `0xcf762f`. This is static evidence
  of an ownership path, not proof that the selected live cache remains private.
- Publication is anchored by Terrain.cpp's CalcMinMaxHeight signature.
  `0xcf5805` loads grid+0x18 and scale+0x34 from terrain. Unsigned uint16
  minima/maxima are converted to floats and scaled. `0xcf58ad/0xcf58b4`
  store record+0x18/+0x1c, `0xcf58bb` increments record+0x20. The native
  replacement uses an unsigned increment to match the machine's wraparound.
- Alignment `0x173dae0` saves self/map in R14/R12; publication call
  `0x173e168` obtains terrain from self+8. Caller `0x173e443` is the existing
  guarded batching redirect. With batch zero and no complete sidecar it
  calls the stock update. The proposed bypass requires every record's served
  range and a still-loaded matching file; small passes do not skip.
- Stolen lengths 13/24/24 end on instruction boundaries with no RIP-relative
  operands. The 13-byte AddTile hook uses the host's supported near stub.
  Original function pointers are now passed directly to installHook so the
  host publishes them before changing entry bytes. A separate atomic gate
  enables sidecar effects only after all three hooks succeed. Failed installs
  leave any published callbacks forwarding only; no rollback frees code that
  might have been entered. Tests inject failure at each hook.

## Live attempt and unresolved contracts

Twice installed candidate soldier libraries and merged Lua into cp -a backed-up
native actor payloads. The final candidate used terrain_sidecar=1, autoload=1
and newgame_density=0 in the actor only. Both prescribed lab launches exited 1
immediately: `bwrap: setting up uid map: Permission denied`. No game process
existed for gdb. No menu, Vulkan device, save load, terrain memory, performance
or visible result was observed. Steam and host policy were untouched.

The second log-copy attempt hit an existing symlink in the evidence directory;
restoration was then performed explicitly. Final SHA-256/symlink manifests
match both the first and final backups for both payload directories. Saves
were untouched and no game remained. `launch.txt`, `launch-result.txt`,
`restoration.json`, and archived actor logs/data are retained; actor logs/data
are historical and do not establish candidate gameplay.

Still needed before default enablement: live CTerrain identity/lifetime
through AddTile, save capture and two load versions; whether LastTerrain is
still the saved world's terrain; worker/pager synchronization and writable
cache ownership; served-mark validity through resets and edits; and proof
that first large-pass completion may release the shared sidecar safely.
Compare full, partial, absent and changed-fingerprint sidecars in the game,
including metadata and subsequent terrain edits. Native fixture coverage
cannot settle these contracts. The end-to-end production port remains partial.
