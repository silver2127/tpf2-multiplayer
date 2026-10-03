# Windows dev ba95f609 integration: automatic Fantasia buffer reuse

## Merged

Target `ba95f6094f85fdecd8b09487f77ac2b660388350`, Linux parent
`59faedb` (after [4617fb6f](UPSTREAM_dev_4617fb6f.md)). Both incoming commits
are retained: `8aed984` adds the Fantasia low-memory mod and `ba95f609`
replaces its separate generator entries with Fantasia's own filenames and
applies reuse only above 32768² square metres. No merge conflicts.
The merge remains staged and uncommitted; release version stays 0.7.0.7.

## Ported (what and how)

The runtime change is shared Lua, retained byte-for-byte from the target.
The wrapper locates Fantasia through package.searchpath, normalizes path
separators, loads the original generator in a private environment, and wraps
its updateFn. Names, climate, parameters and operation order stay unchanged.
Maps at or below the area threshold return the original pipeline unchanged;
larger maps use the existing bigmap_memory optimizer. No ELF address, native
hook, struct layout, ownership contract or ABI changes are introduced. The
Linux organization and existing RE records were reviewed; no new RE is needed
for these two commits.

On Linux the Python installer now requires an explicit `--mods` directory;
it cannot silently create a Windows-shaped path in the working directory.
Windows retains its existing default. Tests accept `--fantasia-res` and
`--game-res`, retaining their Windows defaults. Added upgrade cleanup coverage
and expanded unchanged-at-32-km coverage to all three climates. The native Lua
verifier and package BUILDINFO now name this target, preserving the existing
origin-replay exception; the package includes this integration record.

From the repository root, with the game closed:

```sh
python3 bigmap/tools/install_fantasia_low_memory.py --mods '/path/to/Transport Fever 2/mods'
# Uninstall using the same explicit directory:
python3 bigmap/tools/install_fantasia_low_memory.py --mods '/path/to/Transport Fever 2/mods' --remove
```

Enable Fantasia and then **Fantasia Map Generator (low memory)** below it in
the mod list. Choose the normal Fantasia generator. Its `buffer reuse armed`
log confirms that the stand-in won the resource load order. This remains a
separate opt-in source installer, as upstream; it is not automatically enabled
by the multiplayer package. Fantasia Workshop files are not changed.

## Not ported

None from these two commits. Earlier native feature gaps remain, including
terrain sidecars and cargo-filter capture/replay; this batch does not address
them. Symbolic pipeline equivalence does not prove actual native engine
buffer aliasing, rendered terrain equivalence or peak memory savings. Those
live performance checks remain unvalidated, rather than omitted code ports.

## Live testing

No game was launched, debugger attached or desktop input sent. The actor,
installed mods, saves and Steam were not modified; no backup or restoration
was necessary. No GPU, load-order UI, terrain output, memory peak or multiplayer
result is claimed. The actual installed Fantasia and game Lua resources were
read only by an offline Lua 5.2 harness. All temporary installations and Python
dependencies were confined to this clone's `.git/port-tmp` and `.git/port-venv`.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 71/71 CTests and
  glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception, 32 exact HUD glyphs. Manifest SHA-256:
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- Lua 5.2 Fantasia suite with explicit resource paths: PASS. At 40 km,
  temperate 25,775 layers / 325 -> 10 names; dry 25,846 / 328 -> 10;
  tropical 25,787 / 325 -> 10. Symbolic replay preserved pipeline semantics.
  At 32 km all three pipelines were exactly unchanged (16,809 / 16,880 /
  16,821 layers). Missing Fantasia refuses generation; install idempotence,
  obsolete-entry removal, foreign-directory refusal and uninstall pass.
- Linux CLI fixture: PASS, missing `--mods` rejected before writing, paths
  with spaces supported, repeated install/remove idempotent.
- Release shell syntax, staged whitespace, runtime Lua identity against the
  target, and unresolved-index/merge-state checks pass.

Logs: `.git/port-ba95f609-{build,lua,fantasia,deps}.log`. No publication,
commit, merge abort or system-wide installation occurred.
