# Native investigation for dev 5a9b3ae0

Target `5a9b3ae0641478d4cda6b46e846f585866c50184`, Steam Linux build
35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
All addresses below are ELF virtual addresses; add the loaded PIE base.
The incoming Linux research files are leads, not local live evidence.

## Freed IDs

`functions.csv` gives `0x32567a0`, size 1315; `funcsig.csv` identifies it
through `void ecs::Engine::EndModification()` and `Lib/ecs/Engine.cpp`.
Fresh objdump of the actual lab executable confirms:

- `32567c7` reads `[rdi+208]`; `32567e9` preserves SysV `this` in r13.
- Component EndModification callbacks finish before `3256930`.
- `3256930`: `49 8b 85 08 02 00 00`, `mov rax,[r13+208]`.
  This is the seven-byte displaced instruction, with no RIP-relative operand.
- `3256937` forms the destination deque address `[r13+e0]`; `325693e`
  loads the source end from `[rax+8]`, `3256942` its begin from `[rax]`, and
  `3256952` divides the byte difference by four. These are native 32-bit
  entity IDs, not the MSVC engine offset +200.
- `3256999` passes begin in rsi; `32569b1` passes end in rdx to the range-copy
  helper at `325b850`. The destination finish iterator is updated afterwards.
- `32569e0` retrieves the payload, then `32569e7` clears engine+208 before
  cleanup. The sort therefore operates on the batch before append and cleanup.

Full guard: `49 8b 85 08 02 00 00 49 8d b5 e0 00 00 00 4c 8b`.
`verify_order_canon_elf.py` independently checks the build-id, all six guards,
whole-function instruction boundaries, and direct branches for interior
trampoline targets. The existing register-preserving relay now passes saved
r13 to CanonSort and has a sixth stub. Existing build checks, vector bounds,
index validation, activation gate and installation rollback are preserved.

The fixture executes all six actual shims with both stack alignments, checking
GP/XMM registers, flags, MXCSR and stack position, including the new displaced
rax load. It also covers absent/empty/malformed removed-ID payloads, duplicate
IDs, already sorted batches, all six guard refusals and installation rollback.
It does not prove live engine ownership, concurrency or replicated-world parity.

## Batch-boundary pacing

`funcsig.csv` identifies `0xa30cc0`, size 3940, as
`bool CGame::Sync(const std::function<void()>&)`, anchored in `Game/Game.cpp`.
The actual ELF confirms its 15-byte prologue:
`f3 0f 1e fa 55 48 89 e5 41 57 49 89 ff 41 56`.
These complete instructions preserve incoming rdi in r15. At `a30cd8`, rsi
(the callback reference) is saved in `[rbp-68]`. The native signature is
`bool(void* this, const void* callbackReference)`; no libstdc++ object is copied.

`a310be` reads m_data at CGame+160 and `a310c5` stores the fresh int32 batch
interval at m_data+1a8. `a313fe` sets ebx=1 on success; the shared epilogue at
`a30e3a` moves ebx to eax. Failure sets ebx=0 at `a30e2b`.
In CGame::Step (`a31c30`, 322 bytes), `a31c60/63` set rsi/rdi and `a31c66`
calls Sync. `a31c6b` tests al. The next batch test reads the interval at
`a31c8f`; interpolation reads it at `a31cd2`, after the call returns.

The native entry detour calls the original trampoline, imposes the interval
only on a true return, and returns that bool unchanged. Step's detour only
forwards. A fresh engine estimate is always published, even if equal to the
last override; controls take effect at the next successful Sync. GetSpeed's
existing lever/frame observation remains intact. All prologues and GetSpeed
calls are checked before any installation. Sync installs last, so earlier
installation failure cannot activate pacing writes. A Sync failure leaves
only the existing pass-through hooks.

`verify_speedhook_elf.py` checks the actual guard bytes, function boundaries,
interior branches, calls, argument setup, bool test and field accesses.
The pacing fixture tests pins/fractional targets, baseline updates and equal
estimates, successful/failed Sync returns, callback forwarding, and repeated
Step calls between control changes and the next batch. This is off-game
regression evidence, not a ship-rendering soak result.

## Local live attempt, 2026-09-22

Backed up the native actor's share/tpf2mp, game/mods/mp_lockstep_1 and userdata
as `.before-port`. Installed the soldier libraries and merged Lua only into
that actor, requested `autoload=1`, opted into canonical ordering, and
requested RADV with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json`.
The normal lab launcher failed immediately with
`bwrap: setting up uid map: Permission denied`.

A second attempt retained the lab's outer mounts and private data environment,
using `/usr/bin/bwrap` and bypassing only the nested pressure-vessel command,
as in earlier integration attempts. It exited 53:
`SteamAPI_IsSteamRunning() did not locate a running instance of Steam`.
The game/Steam API then attempted Steam startup automatically; its steam.sh
output reported read-only logger paths and missing 32-bit libGL.so.1. No
Steam management command was issued by this integration. The Steam tree was
read-only in the lab; a subsequent process scan found no Steam, game, bwrap
or pressure-vessel process. No further launch was attempted.

Neither attempt reached the title menu, selected a Vulkan device, loaded a
save or permitted a gdb probe. No XTEST input, live ABI measurement, free-ID
recycling observation, render-clock observation or cross-peer test is claimed.
All three actor trees were restored. Copied actor logs may include older runs;
only live-launch.log and live-fallback.log describe these attempts.

Job evidence in `meta/live/`: freed-disassembly.txt, sync-disassembly.txt,
anchors.txt, canon-elf.log, speed-elf.log, live-launch.log, live-fallback.log,
fallback-command.txt, process-scan.json, restored.txt, actor-logs/, actor-data/,
build.log and lua-verification.log. The exact attempt script is live-port.py.

## Remaining evidence and activation decision

The six-sort module remains default-off (`TPF2MP_ORDER_CANON=1` is the existing
experimental opt-in). Enabling it by default remains unported: this run could
not settle the earlier capacity-map stale-bucket/nested-consumer lifetime
contract or observe the removed-ID vector during real EndModification calls.
Next successful lab run needs gdb captures before/after append and cleanup,
capacity-map lifetime probes, and retained-host/loaded-joiner comparisons past
the upstream failure interval. Also exercise ships with a 200 ms pin and
fractional pacing, checking the clock throughout successive batches.
Upstream busy-world observations in HOTJOIN_ORDER.md are not reproduced here.
