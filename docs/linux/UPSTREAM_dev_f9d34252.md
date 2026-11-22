# Windows dev f9d34252 integration: stop cargo bit vectors (partial)

## Merged

Windows target: `f9d34252db190556a99423a813182e228a07e3a3`.
Linux parent: `e96c0115c51268428d7b2f1f662e0adc25000017`.
One incoming commit, no conflicts. Windows `native/src/slice/lines.inl`
remains byte-identical to upstream, including its MSVC packed-bit reader.
The merge is staged and deliberately uncommitted.

## Ported (what and how)

Shared Lua already supports the corrected per-cargo numeric 0/1 wire flags.
Updated its line-wire regression from the old cargo-index examples to flags
and extended create/update, codec, replay and snapshot checks over 2, 30,
33, 65 and 1024 flags. Leading/trailing zeros and an all-zero unload list
survive the round trip with fractional maxLoad and waypoints. These are
mock-API results, not engine userdata or native-memory decoder tests.

Advanced Lua verification and package BUILDINFO provenance to this target,
retained the cumulative native origin-replay exception, and included this
record in packaging. Release/handshake version remains 0.7.0.5.
Read README, Linux install/integration records, sandbox instructions and
line/StopConfig RE notes. No native runtime implementation changed.

## Not ported

**Linux native stop cargo capture remains unported.** Native line edits
still omit `cfg=` and can lose cargo filters during replay. The incoming
Windows decoder cannot be copied: Linux uses two 40-byte bit vectors with
64-bit words at Stop `+0x50/+0x78`, maxLoad at `+0xa0`, and stride `0xb8`.
Static ELF inspection confirms the backing layout but did not settle the
full Lua property conversion/write-back contract. A fresh live attempt
failed before game startup, preventing the required differential probe and
capture-lifetime validation. No guessed native reader was introduced.

See [RE evidence and next probe](../re/linux/DEV_F9D34252.md). Missing proof:
Linux property encoding/order/empty behavior, real userdata write-back, and
live capture lifetime. Other inherited gaps remain outside this commit.

## Live testing

Backed up native actor `share/tpf2mp/` and `game/mods/mp_lockstep_1/` using
`cp -a` into `.before-port-f9d34252` siblings. Installed rebuilt soldier
boot, bridge, menu, pluginhost and slice libraries plus merged Lua.
Ran `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
It exited 1 immediately: `bwrap: setting up uid map: Permission denied`.

No game started, no renderer/GPU was selected, no menu/save/filter behavior
was observed, and no gdb or desktop input was possible. Restored both
payload directories in a finally block and verified all file content hashes.
No game remains running; no save was changed. Steam, installed mod and
user game/save directories were untouched. Launcher output, installation
hashes, restoration result, actor logs/data and disassembly are copied to
this job's `meta/live/`. Historical actor logs are not candidate-run evidence.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier SDK, 70/70 CTests,
  glibc <=2.31 checks. `.git/port-native-build.log`.
- `lua5.2 tools/linux/test_slice_line_wire.lua "$PWD"`: PASS; numeric cargo
  flags, create/update, codec, replay, snapshots, equality, legacy stops,
  waypoints and native rename/color origin replay. `.git/port-line-wire.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned cumulative merge and 32 exact HUD glyphs. `.git/port-lua.log`.
  Lua manifest: `8dba1685def2dc2231c1d6d06fc620b0e2e9eea6f155178a8e4e051e28d070d1`.
- Shell syntax, CRLF-aware staged whitespace, unchanged upstream Windows
  file, empty unmerged index and retained MERGE_HEAD checks pass.

Final status: PARTIAL. No commit, merge abort or publication.
