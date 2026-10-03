# Native Big Maps memory revisit — 2026-09-22

No merge: `port/dev` already contains Windows through `0b249268ab`.
Windows code and shared Lua are unchanged.

## Ported

Windows' automatic terrain hot-budget calculation is now native:
`terrain_cache_hot_mb=0` uses installed MiB / 30, bounded to 256..4096 MiB.
Explicit positive settings preserve the existing native limits. The native
candidate initialized userfaultfd and logged 1022 MiB on the lab machine.

An experimental Linux alignment batch implementation adapts the Windows
algorithm to libstdc++ map headers, node links and SysV arguments. It verifies
seven sites and retains the stock call on mismatch. `alignment_batch_tiles`
defaults to zero until loaded-world validation is possible; 512 enables the
prototype. Tests cover real libstdc++ iteration, partial batches, source vector
ownership, config disablement and all seven guard failures.

Full addresses, bytes, static derivation and live evidence:
[Big Maps port record](../../bigmap/docs/linux/PORT.md#memory-revisit-2026-09-22-partial).

## Not ported

Production alignment batching/load-peak acceptance remains open. Dedicated
compression default-on, dynamic headroom/load throttling, material paging,
terrain COW/dedup, small paging and instance shrink also remain open. Depth
12/13 remains gated on those items. This record does not supersede unrelated
multiplayer backlog records.

These received a shared live startup attempt, with mapped-code inspection and
probes as detailed in the Big Maps record. The lab could not initialize Steam;
no world loaded, so ownership/lifetime contracts remain unproven. In particular,
requesting dedicated rendering suppression is insufficient when its existing
Vulkan slot verification can refuse activation. Default compression remains
opt-in until the successful-suppression contract is established.

## Live testing

The normal helper failed UID-map setup. The earlier documented system-bwrap /
no-inner-runtime workaround preserved the lab filesystem overlays. The new
plugin initialized successfully and patched its verified alignment call with
batching enabled. Ordinary runs then exited 53 with SteamAPI reporting no
running Steam instance. No steam/steamwebhelper process was visible. Steam was
not started or changed. The final hardware-breakpoint run reported application
load error T:0000067431 and exited 84 before reaching any target.

No map generation/load, GPU selection, RSS comparison or two-peer gameplay
result is claimed. NewMPSAVE was absent from the supplied saves. Both actor
payload directories were restored byte-for-byte against their pre-run backups;
all game processes exited. Logs are in the job's `meta/live/` directory.

## Tests

- `tools/linux/build_native.sh`: soldier build, 63/63 CTests and glibc baseline passed.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: build-id and 27 sites passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures passed.
- `git diff --check`: passed.
