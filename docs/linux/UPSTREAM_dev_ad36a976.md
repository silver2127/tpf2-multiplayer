# Windows dev ad36a976 integration: cargo replay request protocol

## Merged

Windows target: `ad36a9769fc4059c58ee7554e5852497bff99df1`.
Linux parent: `f4fd600b69d306d7464813a8baa0a21b5205fb21`.
One incoming commit, no conflicts. All three incoming files remain identical
to the Windows target. Merge stays staged and uncommitted; release 0.7.0.5.

## Ported (what and how)

Retained shared Lua's removal of lineApplyCargo and its replacement with
lockstep_lcargo_<instance>.txt requests immediately around updateLine in
lineApplyNow and scheduled LUPDATE replay. Requests name the line, sequence,
stop index and load/unload/maxLoad lists. Windows reads these in its factory
hook and populates empty containers through the game's allocator. Both
Windows source changes are preserved unchanged.

Updated the existing Lua line-wire regression to test sparse stop indices,
multiple records, waypoint suffix exclusion, sequence progression, empty
filters, failed opens, clearing, and request presence during the factory
and absence before send on both replay paths. It retains numeric wire and
snapshot coverage at lengths 2, 30, 33, 65 and 1024. It explicitly verifies
that Lua-built filters are empty, without simulating a successful native
writer. This supersedes the prior copy-on-read mock's unsupported setter
assumption; passing Lua fixtures does not prove engine writes.

Advanced Lua verification and package BUILDINFO to this target, retained the
pinned Linux inject.lua origin-replay exception, and packaged this record.

## Not ported

**Native cargo replay writer**: no Linux consumer of lockstep_lcargo exists.
Linux replayed stops therefore still lack the requested filters. The previous
4ccdde5d integration's claim of a replay fix was based on a mock and must not
be treated as working native gameplay. Native cargo capture was already
unported and remains so.

Performed fresh ELF disassembly of stop copy/destruction, StopConfig binding,
the float-vector copy candidate and UpdateLine. Linux uses 40-byte bool
containers with 64-bit words at +0x50/+0x78 and floats at +0xa0, with stop
stride 0xb8. Windows offsets/allocator calls cannot be copied.
The live attempt failed before process startup, leaving container ownership,
empty-state and successful replay/cleanup unverified. No new unsafe writer
or guessed patch was installed. Full evidence and remaining probes:
[DEV_AD36A976.md](../re/linux/DEV_AD36A976.md).

## Live testing

Backed up native actor library/data and mod directories using cp -a;
installed the five newly built libraries and merged Lua. The prescribed lab
launcher, time-boxed to 180 seconds, exited 1 immediately with
`bwrap: setting up uid map: Permission denied`. No game, gdb attachment,
GPU selection, menu, loaded save or cargo result was observed. No desktop
input or Proton peer was run. Steam and installed user payloads were untouched.

Both actor directories were restored and their content manifests match.
No save changed and no game remains running. Job `meta/live/` contains raw
static evidence, attempt.log, installed.json, restoration.json and actor
logs/data snapshots (historical, not new-run evidence).

## Tests

- `tools/linux/build_native.sh`: PASS, soldier SDK, 70/70 CTests and
  glibc <=2.31 checks; `.git/port-ad36a976-build.log`.
- `lua5.2 tools/linux/test_slice_line_wire.lua "$PWD"`: PASS, including
  both request lifetimes; `.git/port-ad36a976-line-wire.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with
  one pinned native exception, 32 exact HUD textures;
  `.git/port-ad36a976-lua.log`. Manifest SHA-256:
  `395f7dc46343862d04ee8b0b33151b4a1799c445c16ef5c5081f7eebd76faca1`.

Final status: PARTIAL. Build and shared integration pass; native cargo replay
remains unavailable after static investigation and a blocked live attempt.
No commit, merge abort or publication.
