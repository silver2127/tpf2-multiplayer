# Windows dev 4616c16a integration

## Merged

Windows target: `4616c16ac73944c80fd2544de349646155644fb1`.
Linux parent: `5a49720367ed689de06acb7ae120025fb7c45a2f`.
One incoming commit: `bigmap: README lists every performance optimization and how it works`.
Only `bigmap/README.md` changes upstream. No conflicts; merge remains staged
and uncommitted.

Retain the performance catalog: memory and load-time/CPU optimizations,
Windows switches/defaults, mechanisms, measured gains and design-document
links, plus the retired/experimental feature notes.

## Ported (what and how)

Add a native scope note immediately above the catalog, checked against
`bigmap/linux/tpf2_bigmap.cfg`, `bigmap/linux/bigmap_linux.cpp` and
[the native port evidence](../../bigmap/docs/linux/PORT.md). Native terrain
compression, min/max scanning and faster saves default on; alignment batching
is experimental and defaults off. The native min/max feature omits the Windows
block-copy optimization. Other catalog entries are not implemented as native
features. Attribute the gains and multiplayer claims to upstream rather than
claiming new native measurements or cross-platform validation.

Link this integration from the root README and include its record in Linux
release packaging, following earlier integrations. Windows documentation is
preserved; runtime code, native defaults, release version and the shared Lua
verification baseline are unchanged.

No new hook, address, byte pattern, ABI, struct offset or lifetime contract is
introduced. New static/live reverse engineering is unnecessary for this
catalog-only change. Read-only verification of the existing ELF patch sites
is recorded below; earlier RE evidence, including
[dev bd69b864](../re/linux/DEV_BD69B864.md), remains applicable.

## Not ported

None from this commit: it adds documentation, not implementations or a request
to change native behavior. Existing native feature gaps remain documented in
[Big Maps PORT](../../bigmap/docs/linux/PORT.md); this integration does not
claim to close them or repeat their historical static/live investigations.

## Live testing

No game launched, gdb attached or actor payload installed. No local gameplay,
rendering, performance or multiplayer observation is claimed. Lab actors,
saves, Steam and the user's installed mod were untouched; no backup or
restoration was needed.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 66/66 CTests and glibc
  <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception) and 32 HUD textures passed. Lua manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `python3 bigmap/tools/linux/verify_game.py ~/.local/share/tpf2mp-lab/native/game/TransportFever2`:
  GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a` and all 29 sites passed.
- `bash -n tools/linux/build_release.sh`, whitespace checks, preservation of
  upstream additions and merge-state checks passed. No release package built.
- No new tests added for documentation-only changes; existing area tests passed.

Logs: `.git/port-4616c16a-{build,lua,elf}.log` in this disposable clone.
