# Upstream dev 1eb30002 integration: TCP first and Steam Messages

Windows target: `1eb3000202a191003033eb1293fb769402ead04f` (0.6.1.26).
Linux merge parent: `5e036fa8942f9cd27d1a3b0c18bf0d695346c36f`.
The merge is resolved and staged, not committed.

## Merged

- b14f914: shared lobby tries TCP in both directions before Steam chunks,
  waits at most TCP_FIRST_WAIT, and handles early TCP failure feedback.
- 1eb30002: experimental Messages v002 default, explicit Legacy comparison,
  modern diagnostics, ABI headers, Windows regression and release metadata.

## Ported

The shared tunnel retains its Linux socket/thread and shutdown implementation.
Messages uses the game's loaded `libsteam_api.so` through dlsym and the
vendored Linux ABI definitions. The Legacy switch uses the native runtime data
path. Both peers must match. Failure to resolve Messages keeps Steam transport
off; no second Steam initialization or implicit fallback is introduced.
Callback registration follows successful socket creation; pending messages are
released at shutdown and startup mode is reset for restarts. Windows API paths
are preserved. See [native evidence](../re/linux/DEV_1EB30002.md).

Release packaging and the Lua verifier identify this target. All 29 Lua files
match Windows except the existing pinned native origin-replay change; all 32
HUD glyphs match. Lua manifest SHA-256:
`b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.

## Not ported

None from these two commits. Earlier limitations, including default-off
canonical ordering and outstanding gameplay validation, remain as documented
in [b7760259](UPSTREAM_dev_b7760259.md). This record does not establish general
Windows/native simulation parity.

## Live testing

Backed up native actor `share/tpf2mp` and `game/mods/mp_lockstep_1` as
`.before-port`, installed this job's soldier libraries and merged Lua, and ran
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
It exited 1 before game exec: `bwrap: setting up uid map: Permission denied`.
No title menu, GPU selection, Steam transport session, gdb probe, or XTEST
sequence occurred. No real-peer throughput claim is made. Both directories
were restored; no game is running. Steam and user installations were untouched.
Logs are archived under this job's `meta/live/`; inherited actor logs are
explicitly not new observations.

## Tests

- `tools/linux/build_native.sh`: 57/57 soldier CTests pass; glibc <=2.31 check passes.
- CTest now compiles the upstream adapter fixture and runs the full native
  tunnel against fake Steam exports in both Legacy and Messages modes, with
  real UDP, startup selection and unavailable-accessor restart coverage.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 glyphs pass.
- `tools/test_steam_tcp.py`: both TCP directions, zero Steam chunks when TCP
  connects, unavailable-address fallback (10.66 s), no-address fallback,
  exact file hashes all pass. These use fake Steam tunnels, not the Internet.
- `tools/test_steam_tunnel.py`, `tools/test_steam_save_transfer.py` and
  `tools/test_steam_backpressure.py`: pass; the save test transferred 48 MiB
  with identical bytes. Backpressure covers bounded queues/lost feedback.
- Release script shell syntax and port-change whitespace checks (CRLF-aware)
  pass. The full staged check flags existing trailing whitespace in the
  unmodified upstream Valve headers; those vendor bytes are preserved.

Python network tests used `.git/port-venv` with pystun3 and zstandard. Installing
all requirements failed building optional miniupnpc; these tests passed without
it. An initial new fixture compile error (protected Steam message destructor)
was fixed by using a derived fixture object before the passing final build.
Logs are in `.git/port-*.log`, also copied to `meta/live/validation/`.
