# Terrain-sidecar release investigation (dev 363c38cc)

Target: `363c38cca1a979597144921a27ba6cd4aeabc7be`.
Actual lab ELF: Steam 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, confirmed with readelf.
Evidence from this job is in `meta/live/` beside the disposable repository.

## Changed contract and native implementation

Windows `TerrainSidecar` now protects the loaded file/index with an SRW lock:
readers hold it shared across decoding, begin/end take it exclusive, and end
moves ownership into a local destroyed after unlocking. `EndApply()` returns
true only to the caller that removed a loaded file; only that caller logs.
The upstream test races four decoders with eight releasers over 200 rounds.

Native `bigmap/linux` has no sidecar file/index, AddTile serving hook, served
publication suppression or alignment-done callback. Its existing
`AlignmentUpdate` calls `linux_alignment::Run`, which owns only temporary map
views and then calls the original native update. Thus adding a mutex there
would not port the sidecar fix. The native userfaultfd pager is a separate
implementation; no Windows SRWLOCK or MSVC object layout was copied into it.
The [earlier sidecar gap](DEV_60D237C5.md) remains, although native experimental
alignment batching is implemented (default off), as documented in
[Big Maps PORT.md](../../../bigmap/docs/linux/PORT.md#alignment-batching-implemented-experimental-default-off).

## Fresh static investigation

Searched the Linux signature export for CTerrain/GetTileCache and
UpdateSubterrains; retained matches in `terrain-signatures.txt`. Disassembled
the real ELF, revisiting the existing native batching and tile allocation sites:

- `0x173dae0` saves RDI to R14 (`49 89 fe` at `0x173daec`) and RSI to R12
  (`49 89 f4` at `0x173daf3`): system and const libstdc++ map respectively.
  `0x173db00` loads RDI from system+8, then calls the signature-anchored
  `CTerrain::GetTileCache` at `0xcf5960`. This provides a terrain candidate,
  not evidence that it is the surviving saved-world owner.
- The signature maps `0x173cbe0` to ThreadPool::LoopImpl for
  TerrainAlignmentSystem::UpdateSubterrains. The update calls that worker at
  `0x173e0a6` (`e8 35 eb ff ff`). At `0x173e168`, bytes
  `49 8b 7e 08 4c 89 ee e8 5c 75 5b ff` load the terrain into RDI, put R13
  in RSI and call publication at `0xcf56d0`. Subsequent delete calls at
  `0x173e180`, `0x173e1a9`, `0x173e1b7`, `0x173e1d4` concern engine-local
  storage, not a native mod-owned sidecar. No global load-completion ownership
  can be inferred from reaching one such return.
- In the existing AddTile allocation neighborhood, `0xcf7409` computes
  index*5, `0xcf7411` addresses base+index*40, and `0xcf7419`
  (`45 89 2c 24`) stores R13D in the record. At `0xcf75e4` the code reads
  record+0x10 and tests its reference count, branching at `0xcf75f9` to
  `0xcf7720` when shared. At `0xcf75ff` it reads record+8 and at
  `0xcf7604/08` reads the vector end/begin. These observations narrow the
  record/detachment investigation; they do not prove writable eligibility,
  the identity of every register at entry, or survival across reloads.

Transcripts: `alignment-disassembly.txt`, `alignment-completion.txt`,
`addtile-disassembly.txt`, `record-store.txt`, `elf-notes.txt`. The broad AddTile
range starts mid-instruction at `0xcf7400`; the focused record-store transcript
starts at the valid `0xcf7407` boundary. No claim relies on its misdecoded prefix.
The existing independent checker passes all 29 guarded sites. No new address,
byte pattern, object offset or hook is introduced into production code.

## Live attempt and remaining proof

Backed up both native actor payload directories to `.before-port-363c38c`
siblings, installed this job's soldier libraries, Big Maps plugin/config and
merged Lua, set `autoload=1` and `newgame_density=0`, then ran the prescribed
lab launcher with a 170-second limit. It exited 1 immediately with
`bwrap: setting up uid map: Permission denied`. No game process, Vulkan device,
menu, loaded world, gdb attachment or reload race was observed. No Steam or
host security policy was changed. The actor directories were restored and
compared equal using `diff -qr`; no save was changed or game left running.

`launch.txt` and `restoration.txt` record this attempt. `actor-logs/` and
`actor-data/` are copied actor state and may include historical runs; they are
not evidence of successful gameplay in this job.

Still needed: live CTerrain ownership across load/reload, capture freeze and
save fingerprint binding, eligible private tile vectors, pager served-state
lifetime and publication suppression, and the complete set of concurrent
completion/read paths. A native sidecar must establish these before adopting
the same shared-decode/exclusive-release contract. The sidecar and therefore
this native release fix remain **not ported**; no speculative hook is enabled.
