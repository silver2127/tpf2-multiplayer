# Windows dev 0047c19f integration: stop cargo filters (partial)

## Merged

Windows target: `0047c19fdb4050dacb5e54d4108f99af920838ce`.
Linux parent: `7501831cc764716596fceaee3411800f976534e1`.
One incoming commit. Resolved both conflict hunks in `mp/inject.lua` by
inserting cargo capture before waypoint capture, retaining the native
rename/color `replayOrigin=1` handling and existing line endings.
All changes are staged; the merge is deliberately uncommitted.
Windows `native/src/slice/{lines,capture}.inl` and shared `mp/lines.lua`
remain byte-identical to the incoming commit.

## Ported (what and how)

Shared Lua carries the `cfg=` capture tail into the stop's `^load|unload|maxLoad`
suffix before its waypoints. Snapshots retain that suffix, signature equality
compares it, and line replay passes it to stopConfig. Native receivers can
parse these new wire records; unfiltered legacy records remain readable.
These are code/fixture results, not confirmed in-game cargo behavior.

Extended the existing `test_slice_line_wire.lua` regression with real
capture, network codec, line builder and snapshot functions under a mock API.
It covers both create and update, nonempty and empty filters, waypoint
coexistence, malformed capture rows, exact filter equality, tolerant positions,
legacy stops, and retention of native rename/color origin replay.

Advanced the Lua verifier and package BUILDINFO to the incoming commit,
pinning the cumulative native inject.lua exception. Package this integration
record. Release/handshake version remains 0.7.0.5; that version alone does not
identify the new cargo wire support or establish cross-platform parity.
Reviewed README, Linux installation/integration records, sandbox instructions,
and native line/vehicle RE documentation.

## Not ported

**Native capture of stop cargo filters remains unported.** Linux's
`native/linux/src/slice/slice_lines.cpp` still emits no `cfg=`. A line edit
captured there can therefore still lose its filters during replay, including
filters previously received from another peer. This is not a complete fix
for native Linux gameplay.

Static RE confirmed the Linux Stop stride 0xb8, two 40-byte packed bit
containers at +0x50/+0x78, and maxLoad at +0xa0. Lua load/unload are properties
with integer-vector setters, not ordinary integer vectors in native memory.
Tracing component/property registration and the copy/destructor paths did
not establish their full conversion or property write-back semantics.
The live attempt failed before game startup, so those contracts could not
be probed. No guessed reader or offsets were added, and the native build
and existing hook byte checks are preserved.

See [RE evidence and next probe](../re/linux/DEV_0047C19F.md). Missing proof:
load/unload integer encoding, ordering/empty behavior, capture lifetime, and
real userdata replay/read-back. The upstream Windows decoder itself labels
its new offsets inferred; its claims were not independently live-validated.
Other inherited port gaps remain outside this commit's scope.

## Live testing

Backed up native actor `share/tpf2mp/` and `game/mods/mp_lockstep_1/` with
`cp -a` to `.before-port-0047c19f` siblings (existing backups left intact).
Installed the five freshly built soldier libraries and the merged Lua mod.
Ran `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
It immediately exited 1: `bwrap: setting up uid map: Permission denied`.

No game started; no title menu, renderer/GPU, loaded save, filter edit,
gdb attachment, desktop input or Proton peer was observed. Both payload
directories were restored from their backups in a finally block. No lab
game remains running; no save was loaded or changed. Steam and user-installed
mod/game directories were untouched. Copied launcher output, actor logs/data
and static transcripts to this job's `meta/live/`. Historical actor files
are not evidence of this candidate running.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier build, 70/70 CTests,
  glibc <=2.31 checks. Log: `.git/port-native-build.log`.
- `lua5.2 tools/linux/test_slice_line_wire.lua "$PWD"`: PASS, including the
  added cargo and native-origin cases. Log: `.git/port-line-wire.log`.
  The existing optional CTest registration uses this same file; soldier has
  no Lua interpreter, so the Lua regression was run explicitly on the host.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned cumulative merge and 32 exact HUD glyphs. Manifest SHA-256:
  `ddb4f3c606cf1df6b7602853a0cc6e6a5855010b4447beed64672274ec6860d8`.
  Log: `.git/port-lua.log`.
- Shell syntax, CRLF-aware staged whitespace (`core.whitespace=cr-at-eol`),
  incoming-file equality, empty unmerged
  index and retained MERGE_HEAD checks pass.

No commit, merge abort, publication or push. Final job status: PARTIAL.
