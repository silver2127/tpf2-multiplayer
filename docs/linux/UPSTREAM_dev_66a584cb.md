# Upstream dev 66a584cb integration: equal Steam send-rate clamps

Windows target: `66a584cb4a43eb8d7619fd55224ae229fdd04cba` (0.6.1.27).
Linux merge parent: `499710486a090a7dd7176f5ec893b598c35834c9`.
The merge is resolved and staged, not committed.

## Merged

- 66a584cb: both Steam send-rate clamps are 16 MiB/s, with local/remote
  connection-quality diagnostics, networking documentation and release metadata.
- Shared lobby version is 0.6.1.27. Lua and glyph contents are unchanged.

## Ported

Resolved the whole-file tunnel conflict by retaining the existing portable
implementation and applying the upstream minimum-rate change and explanation.
Linux and Windows now set IDs 10 and 11 to 16777216 bytes/s before opening a
Messages session; the 8 MiB buffer settings remain intact. Linux retains its
dlsym resolution, socket handling, callbacks and thread shutdown. The merged
shared Messages adapter logs both quality fields through the vendored ABI.

Updated the Linux release record packaging, README, installation reference and
Lua verifier baseline. See [API evidence](../re/linux/DEV_66A584CB.md) and the
[previous integration](UPSTREAM_dev_1eb30002.md).

## Not ported

None from this commit. Inherited gameplay limitations remain: canonical ordering
is default-off and matching versions alone do not establish simulation parity.
See [the earlier baseline](UPSTREAM_dev_1eb30002.md) for those limitations.
The configured rate is not measured throughput or adaptive bandwidth estimation.

## Live testing

Backed up the native actor's `share/tpf2mp` and `game/mods/mp_lockstep_1` with
`cp -a` to `.before-port`, installed this job's soldier libraries and merged Lua,
and invoked `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
The launch exited 1 with `bwrap: setting up uid map: Permission denied` before
game execution. No title menu, GPU selection, Steam session, throughput,
gdb probe or XTEST interaction was observed. Both actor directories were restored;
no game was left running. Steam and user installations were untouched.
This job's `meta/live/launch.txt` and `restoration.txt` record the attempt.

## Tests

- `tools/linux/build_native.sh`: 57/57 soldier CTests pass; glibc baseline passes.
- Native tunnel fixtures now assert both 16 MiB/s clamps in Legacy and Messages
  modes, preserving checks for the two 8 MiB buffers and valid configuration IDs.
- Adapter fixture uses distinct local/remote quality values and checks their
  formatted log output, detecting omission or swapped fields.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one previously
  pinned native merge) and 32 glyphs pass against 0.6.1.27.
  Manifest SHA-256: `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6afcea04141816c2f1c`.
- `tools/test_steam_tunnel.py`: passes using a clone-local venv with pystun3
  and zstandard, after the system Python initially lacked `stun`.
  Its fake Steam peers verify local delivery, not Internet throughput.
- Release script syntax, conflict-marker and CRLF-aware whitespace checks pass.
