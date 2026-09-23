# Upstream dev 1d0ca473 integration

Windows target: `1d0ca47338ff40f797a09bfcf793fd5dd08ba317`.
Baseline: native Linux after [b5dade06](UPSTREAM_dev_b5dade06.md).
The one-commit merge is staged and uncommitted, with no conflicts.

## Merged

- `1d0ca47`: recognize the CreateLine replay's Add by the script call site and
  claiming thread when sendCommand has rebuilt the factory command, allowing
  the line editor's held completion callback to follow the replay.

Windows `native/src/slice_hook.cpp` is byte-identical to the target. All 28 Lua
files also exactly match the target. The verifier and release BUILDINFO now
identify this revision; the unchanged Lua manifest SHA-256 is
`f5651d1a4ac8f245c6d81bcbf8100e9ef6f4f9f7d3c9ba8d7d4681836199d0db`.

## Ported

The existing native implementation already provides the requested behavior:
thread-local callback reservation, both Linux script Add call sites, rebuilt
command payload validation and the additional Linux direct-apply script sink.
Rechecked all three paths against the actual ELF and retained the existing
SysV/libstdc++ code and fail-closed hook checks. No new address or patch is
needed. Added explicit regression tests for copied commands at both Add sites,
wrong caller/thread rejection and one-time callback handoff. See
[RE evidence](../re/linux/DEV_1D0CA473.md).

## Not ported

None from this Windows commit. Earlier company rename/ownership, automatic
recovery/in-world client load and Workshop gaps remain as recorded in the
previous integration; this update does not resolve or expand them. Overall
native coverage remains partial. No live gameplay validation is claimed.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, **44/44 CTests**
  (including expanded `slice_commands`) and glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: 28 exact files; manifest above.
- Read-only ELF: build-id plus four script replay instruction spans passed;
  objdump independently confirms SysV setup, both Add targets and direct apply.
- Windows source equality, shell syntax, merge-state and whitespace checks.

No game or Steam launch, commit, merge abort, publication, system-wide install
or real-game installation was performed. Build log: `.git/port-build.log`.
