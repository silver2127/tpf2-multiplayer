# Upstream dev 61578d27 integration: company-wash paint targets

Windows target: `61578d278c2e975bfbed7227cf09a7d272228884`.
Linux merge parent: `8a90063fb0c026ba3913f0d3c1948ca6b59889f7`, following
[8e31f1e0](UPSTREAM_dev_8e31f1e0.md). One Windows commit, no conflicts.
The merge remains staged and uncommitted. Status: partial native integration
because the native tint prerequisites remain unavailable.

## Merged

The shared `res/config/style_sheet/mp_lockstep.lua` is byte-exact upstream.
For each company 1..200 it adds ancestor-class/descendant-name selectors for
StationItem::StationIcon and VehicleDepotItem::Icon, with opaque company
backgroundColor1 and 80%-brightness hover colours. Window!mpWinCoN's
Window::Title-bar receives backgroundColor with alpha 0.85. Existing root
wash and dashboard swatch rules remain intact. No native Windows or lobby
code changed in this commit.

## Ported

The platform-independent stylesheet is included unchanged in the Linux mod.
The Linux Lua verifier and release BUILDINFO now target this commit, the
release packages this record, and README/install/resume coverage is current.
All 28 Lua files match upstream, with no pinned exceptions. Manifest SHA-256:
`5a0459b347090e938e7a9b1221ea757d2eb5945da2e3427f801f1a0f9ca313a0`.
No new game hook, patch, ABI adaptation or offset is needed by this diff.
The existing exact-file regression check was updated to cover the new sheet.

## Not ported

Visible native HUD station/depot and entity-window tinting still lacks the
inherited root-class tagging implementation. The shared selectors cannot
supply that prerequisite. Static follow-up found and disassembled the style
helper already used by the native menu, correcting an overly broad missing-
evidence statement in earlier records. Native class append/layout evidence
exists; safe HUD/window entity-to-owner flow, non-asserting StationGroup
traversal, hook state/unwind contracts and world-change lifetime remain
unproven. See [the fresh evidence and decision](../re/linux/DEV_61578D27.md)
and its linked earlier investigations. No speculative hook was enabled.
This commit introduces no additional platform-specific omission. Previous
tint diagnostics, ownership, recovery and Workshop gaps remain unchanged.

## Tests

- `tools/linux/build_native.sh`: soldier build, 45/45 CTests and glibc <=2.31
  checks passed. Log: `.git/port-61578d2-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact files; manifest above.
- `python3 tools/palette_sync_test.py`: all five 20-colour palettes and
  generated-colour boundary match.
- `python3 tools/linux/verify_dev_d3135a59_elf.py <native-game>`: build-id,
  ten guard spans, five branches, instruction boundaries/no interior targets,
  PlayerOwned RTTI and ViewCreator vtable passed.
- `bash -n tools/linux/build_release.sh`, staged whitespace checks, empty
  unmerged index and upstream stylesheet equality passed.

No live rendering or cross-platform gameplay test is claimed. No game/Steam
launch, installation, publication, commit or merge abort occurred.
