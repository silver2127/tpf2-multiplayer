# Complete-sidecar alignment bypass: dev 0871bfa6

Actual lab ELF: Steam build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. Fresh disassembly and signature
exports are preserved in this job's `meta/live/`.

## Windows contract and native gap

Windows records unsigned sample minima/maxima at AddTile, before paging can
make the cache cold. For a batched load pass it first validates every terrain
record against the still-loaded sidecar and served marks. Only then it writes
scaled min/max and increments each version, skips computation, resets timing
and releases the loaded file. Partial, released, unmarked or missing-range
cases retain the original pass; `alignment_skip_served=0` disables the bypass.
The upstream guard also retains stock behavior for disabled batching, small
passes and implausibly large sets.

Linux has experimental libstdc++ map batching, but no sidecar capture/serve
state, served marks, cached ranges or completion callback. Its pager is not a
proof that a tile came from a matching finished sidecar. The inherited gap is
recorded in [DEV_363C38CC.md](DEV_363C38CC.md). No new production hook, offset,
ABI assumption or configuration switch was added for an unavailable feature.

## Fresh static derivation

`funcsig.csv` anchors `0xcf56d0` through the inlined assertion signature
`CVec2f {anonymous}::CalcMinMaxHeight(float, const std::vector<short unsigned int>&)`.
The referenced string at `0x3fdc2a0` was independently dumped from `.rodata`.
The containing publication function is called from the native alignment update:

- `0x173dae0` receives system in RDI and map in RSI, saved in R14/R12.
  It obtains terrain from system+8 for `GetTileCache` at `0xcf5960`.
- At `0x173e168`, `49 8b 7e 08 4c 89 ee e8 5c 75 5b ff` loads terrain
  into RDI, R13 into RSI and calls `0xcf56d0`. This is SysV, not the Windows
  calling convention or MSVC set layout.
- Publication saves RDI in RBX at `0xcf56ed`. At `0xcf5805` it loads grid
  from terrain+0x18, and at `0xcf5809` loads height scale from terrain+0x34.
  Grid origin is +0/+4, width +8 and record storage +0x10. The index calculation
  at `0xcf5820..0xcf5839` gives 40-byte records.
- Record+8 points to a vector with begin at +0 and end at +8. The loop at
  `0xcf5852..0xcf588a` zero-extends uint16 samples and compares unsigned
  values (`jb`/`cmovb`), deriving the true minimum and maximum.
- `0xcf5894..0xcf58a0` converts the results to float and multiplies by scale.
  `0xcf58ad`: `f3 41 0f 11 44 24 18`, stores minimum at record+0x18.
  `0xcf58b4`: `f3 41 0f 11 4c 24 1c`, stores maximum at record+0x1c.
  `0xcf58bb`: `41 83 44 24 20 01`, increments the 32-bit version at +0x20.

These are research observations verified against this ELF, not newly enabled
patch sites. They establish the native counterpart of the metadata update,
but do not prove that skipping all preceding computation is valid for a live
load. Existing guarded patch verification passes all 30 sites.

## Live attempt and missing evidence

Backed up both actor payloads to `.before-port-0871bfa` siblings with `cp -a`,
installed this build and merged Lua, set `autoload=1`, and disabled resource
rewriting with `newgame_density=0`. The prescribed lab launcher exited 1 with
`bwrap: setting up uid map: Permission denied`. No game started, so no Vulkan
device, title menu, save load, gdb attachment or live register/memory evidence
was available. No Steam or host security settings were changed.

Both payloads were restored and their file hashes and symlink targets verified.
No save changed and no game remains running. `launch.txt`, `restoration.json`,
static transcripts and actor logs/data are retained in `meta/live/`; copied
historical actor logs are not observations from this attempted run.

Still required: live saved-world terrain ownership, sidecar fingerprint and
capture/restore lifecycle, private writable cache eligibility, pager served-mark
lifetime, load completion synchronization, and proof that the loaded sidecar
covers every record before bypassing publication. A full/partial/released
sidecar loaded-world comparison must verify metadata and subsequent terrain
edits. Native complete-sidecar alignment bypass remains **not ported**.
