# Upstream dev 6e1e4ec7 integration

Windows target: `6e1e4ec72743a889b1010013a292a99c78cc8590`.
Linux parent: `6874722efec15386eb64fcc7ecc363b2300615e9`, following
[6e5eec30](UPSTREAM_dev_6e5eec30.md). One Windows commit, no conflicts.
Merge staged and uncommitted.

## Merged

Windows native input remains blocked during QueuedSave/Saving even with
`input_hold=0`, mitigating the upstream Big Maps camera/sidecar pager deadlock.
Incoming `native/src/native_io.cpp` and `.h` are retained unchanged.

## Ported

Linux SDL suppression now also checks the native save state during held rounds.
The state query uses a nonblocking mutex acquisition and conservatively blocks
that event on contention, matching Windows. Outside saving, the existing
`input_hold` policy applies. No executable patch or ABI mapping changes.
See [implementation and test evidence](../re/linux/DEV_6E1E4EC7.md).

Release documentation and Lua verification provenance reference this target;
the pinned Linux origin-replay Lua integration remains.

## Not ported

None from this commit. Earlier integration limitations remain, including
canonical simulation ordering default-off pending live validation. This
increment does not establish Windows/native gameplay parity or fix Big Maps'
underlying pager deadlock.

## Live testing

No game launch, gdb attachment or XTEST sequence was performed. No lab actor
files were modified, so no backups or restoration were needed. The changed
policy uses existing native state and the SDL filter and requires no new
engine contract. Camera/sidecar behavior was not observed live.

## Tests

- `tools/linux/build_native.sh`: soldier build, 57/57 native CTests and glibc <=2.31
  checks pass.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned Linux
  integration) and 32 exact HUD glyphs; manifest SHA-256
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6afcea04141816c2f1c`.
- Expanded `native_io` and `panel_title` coverage: all save states, contention,
  completion/failure cleanup and SDL filtering independent of `input_hold`.
- Release shell syntax, whitespace, Windows-source preservation and unmerged
  index checks. Build log: `.git/port-build.log`.
