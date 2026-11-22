# Windows dev 63a3b8df integration

## Merged

Windows target: `63a3b8dfc0384b761d336f121de0986bb918debc`.
Linux parent: `d97e711d6510626ce63526826be7479c10af472b`.
One incoming commit: `bigmap docs: octree depths 12/13 are tested in play and work fine`.
No conflicts. Merge remains staged and uncommitted.

Retain upstream changes to `bigmap/README.md`, `bigmap/cfg/tpf2_bigmap.cfg`
(comments only), and `bigmap/docs/octree-depth12.md`. Upstream reports depth-13
creation, play, save/reload and multiplayer with Windows players and a native
Linux dedicated server, without duplicate-node repairs, assertions or missing
objects. The GOG observation covers the depth-11 fallback. Every peer needs
matching `octree_depth`.

## Ported (what and how)

Update the native README scope note, Linux configuration comments, installation
instructions and Big Maps port record to attribute the new gameplay evidence
to upstream instead of claiming that only offline evidence exists. Preserve the
old octree section anchor for existing links. Link this record from README and
include it in Linux release packaging, following prior integrations.

Native depth 12/13 already exists from [bd69b864](UPSTREAM_dev_bd69b864.md).
No code, address, byte pattern, ABI, offset or lifetime contract changed, so
new static/live reverse engineering is unnecessary. Read-only verification of
the actual lab Steam ELF passes its GNU build-id and all 29 patch sites.
[Existing native evidence](../re/linux/DEV_BD69B864.md#octree) remains applicable.

Keep native defaults `octree_depth=11`, `max_tiles=512` and the menu's placement
diagonal bound. The upstream gameplay report does not implement the missing
native placement-distance widening or prove every boundary/culling scenario.
Historical integration/RE records retain their original observations. Runtime
log strings and Windows code paths are unchanged. Version and Lua baseline
remain unchanged.

## Not ported

None for this commit: its changes are documentation and configuration comments.
Existing native feature gaps remain as documented; this integration does not
claim to resolve placement-distance parity or other prior limitations.

## Live testing

No game launched, gdb attached, or actor payload installed. No local creation,
rendering, save/reload or multiplayer observation is claimed. Upstream gameplay
results were not repeated locally. Lab actors, saves, Steam and the user's
installed mod were untouched; no backup/restoration was necessary.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 66/66 CTests and glibc
  <=2.31 compatibility checks passed, including existing Big Maps stub,
  installer, guard, rollback and depth/ceiling coverage.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception) and 32 HUD glyph textures passed. Manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `python3 bigmap/tools/linux/verify_game.py ~/.local/share/tpf2mp-lab/native/game/TransportFever2`:
  GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a` and all 29 sites passed.
- `bash -n tools/linux/build_release.sh`, whitespace checks, unchanged active
  configuration values, and merge-state checks passed. No release package built.
- No new tests for comment/documentation changes; existing area tests passed.

Logs: `.git/port-63a3b8df-{build,lua,elf}.log` in this disposable clone.
