# Per-terrain served ranges: dev fdfb79e8

## Windows change and native boundary

Windows replaces one grid's range array with four slots keyed by grid pointer
and record count. First writes increment each slot's applied count; duplicates
do not. Completion holds an exclusive lock, requires every record to be served,
and publishes that grid's cached ranges. Reset frees all slots. Slot exhaustion
falls back to alignment. The regression now completes two CTerrain versions
from one sidecar without losing the first version's ranges.

Linux has no runtime sidecar serving, served marks or cached-range completion
path. The existing pager and alignment batching do not establish sidecar
provenance. See [the prerequisite investigation](DEV_0871BFA6.md) and
[threaded capture investigation](DEV_C8DD5157.md). No Windows lock, heap API,
PE address or calling convention was copied into the native plugin.

## Fresh static attempt

This job read the actual lab ELF; readelf confirms GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` (build 35924).
`funcsig.csv` anchors publication at `0xcf56d0` using the CalcMinMaxHeight
assertion signature and GetTileCache at `0xcf5960`. Fresh objdump evidence:

- SysV GetTileCache takes terrain in RDI and packed coordinates in RSI.
  `0xcf5978`: `48 8b 47 18`, reads the grid at terrain+0x18.
- Publication reads grid at `0xcf5805` (`48 8b 43 18`) and scale at
  `0xcf5809` (`f3 0f 10 53 34`). Grid fields are origin +0/+4,
  width +8 and records +0x10. Index arithmetic at `0xcf5820..0xcf5839`
  derives a 40-byte record stride.
- Record+8 points to the vector; begin/end are +0/+8. Unsigned uint16
  minimum/maximum are computed at `0xcf5852..0xcf588a` and scaled.
- `0xcf58ad`: `f3 41 0f 11 44 24 18`, minimum to record+0x18;
  `0xcf58b4`: `f3 41 0f 11 4c 24 1c`, maximum to record+0x1c;
  `0xcf58bb`: `41 83 44 24 20 01`, increment version at record+0x20.

These observations reconfirm the metadata layout, not the number or lifetime
of grids during a native load. No research address became a production patch.
The existing ELF verifier passes all 30 guarded sites.

## Live attempt and missing evidence

Installed the candidate soldier libraries and merged Lua after cp -a backups
of both native actor payloads. Set autoload=1 and newgame_density=0. Ran the
prescribed lab launcher with a 170-second bound. It exited 1 immediately:
`bwrap: setting up uid map: Permission denied`.
No game process, menu, Vulkan device, loaded save, gdb probe or terrain behavior
was observed. No desktop input or Steam/security-policy changes were made.
Both payloads were restored; SHA-256 file and symlink-target manifests matched.
No save changed and no game remained running.

Evidence in this job's meta/live: terrain.asm, elf-id.txt,
signature-anchors.txt, verify-game.txt, launch.txt, restoration.json and
run-lab.py. Archived actor logs/data may be historical, not candidate gameplay.

Native per-grid range tracking and the complete-sidecar bypass remain unported.
Still missing: live CTerrain/grid identity and lifetime across both versions,
private writable tile eligibility, native sidecar capture/restore and served-mark
lifetime, synchronization with load completion/reset, and proof that every record
is covered before skipping alignment. A Windows log's two-version count cannot
prove the native contract. Linux retains its working alignment path.
