# Windows dev 0871bfa6: skip alignment after a complete sidecar load

## Merged

Windows target: `0871bfa60763f9bad9900bf318645f495f3a7c56`.
Linux parent: `05953d5294075a0b686191064f5aa9178defdd55`.
One Windows commit, no conflicts. Merge remains staged and uncommitted.
All six incoming Windows files are retained byte-for-byte: configuration,
alignment documentation, alignment batching, plugin initialization, terrain
serving and its regression test. Release remains **0.7.1.1**.

## Ported (what and how)

Recorded the native feature boundary in README, installation guidance, Big Maps
port notes and native configuration comments, and packaged this integration
record. Existing native alignment batching and pager remain unchanged. No new
native hook, patch or inert enable switch is introduced. Lua remains unchanged
and uses the existing exact dev 2b8505c7 verification baseline.

## Not ported

Native `alignment_skip_served`, per-served-tile range caching, all-record
validation, replacement min/max/version publication and skip/completion
accounting remain unported. Their prerequisite native sidecar capture/serving
and served-state lifetime are already absent. Fresh static analysis located
the native metadata stores and unsigned range computation, but a real lab
launch failed before startup, so the remaining ownership/load contracts could
not be proved live. See [RE evidence and missing proof](../re/linux/DEV_0871BFA6.md).
Linux continues running alignment; the upstream 44–75 second saving is not a
native measurement or a demonstrated Windows live result in this job.

## Live testing

Installed the soldier build and merged Lua into the native actor after `cp -a`
backups of both payload directories. Set `autoload=1` and `newgame_density=0`.
Ran the prescribed `tools/sandbox/tpf2mp-lab run native --root
~/.local/share/tpf2mp-lab`, bounded by a 170-second timeout. Immediate exit 1:
`bwrap: setting up uid map: Permission denied`.
No game process, renderer/GPU selection, menu, loaded world, gdb probe or
terrain behavior was observed. No desktop input was used. Restored both
payload directories in a finally block and verified file SHA-256s and symlink
targets. No save changed, no game remains running, and Steam was untouched.
Evidence is in this job's `meta/live/`; archived actor logs/data may predate
this attempt and are not candidate gameplay evidence.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, **75/75 CTests**
  (59.08 seconds), glibc <=2.31 checks. Existing native alignment and terrain
  pager tests included; no native runtime change requiring new fixture tests.
- `python3 tools/linux/verify_lua_release.py`: PASS, **31 exact Lua files**,
  zero exceptions, **32 HUD glyphs** and **2 toolbar textures**.
  Manifest SHA-256: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id
  and **30/30 guarded sites**.
- Release shell syntax, CRLF-aware staged whitespace, all six incoming files
  identical to Windows target, empty unmerged index and retained MERGE_HEAD
  checked. Package archive was not built.
- Windows terrain-serve tests retained, not executed: they require Windows
  APIs and a hardcoded PE input. No native sidecar behavior is claimed from
  the existing native test suite.

Final status: **PARTIAL**. No commit, abort or publication.
