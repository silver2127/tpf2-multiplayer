# Upstream dev 0620342e integration: adaptive Steam rate and Cross-play

Windows target: `0620342ee15cd41a6634a09384a4ef844d8ab09d` (0.6.1.28).
Linux merge parent: `55b522a34745721de70be145f0e824ce43009ba2`.
One Windows commit; the merge is resolved and staged, not committed.

## Merged

Adaptive Messages rate controller and adapter sampling, Windows redesigned
menu Cross-play controls, networking notes, release metadata and lobby version.
Shared Lua and HUD glyph contents are unchanged.

## Ported

Resolved the whole-file steam_tunnel.cpp conflict by retaining Linux sockets,
dlsym, callback ABI and thread shutdown, then applying the upstream controller
include, early platform-specific Legacy-file check, initial clamps and periodic
rate update. Messages starts at 1 MiB/s; Legacy retains 16 MiB/s.
The shared controller samples worst outgoing-peer remote quality, halves on
loss, grows after three good backlogged samples, and remembers a lower ceiling.
Setter ordering and rollback attempts are shared with Windows.

The native redesigned menu already exposed Cross-play. Aligned Create Game's
descriptive label and spacing, and made the host-lobby checkbox visible without
the extra hostSteam gate, matching upstream. Action 51 retains the existing
host request and invitation-code event handling.

Updated README, installation, release packaging/provenance and Lua verifier.
See [native API evidence](../re/linux/DEV_0620342E.md).

## Not ported

None from this commit. Inherited limitations, including default-off canonical
simulation ordering, remain as documented in the
[previous integration](UPSTREAM_dev_66a584cb.md). Matching versions do not prove
Windows/native simulation parity.

## Live testing

Backed up the native actor's share/tpf2mp and game/mods/mp_lockstep_1 using
cp -a to .before-port, installed this job's five soldier core libraries and
merged Lua, and invoked the lab's native run with a 180-second timeout.
It exited 1 immediately: "bwrap: setting up uid map: Permission denied".
The game did not execute: no title menu, GPU selection, Steam rate changes,
transfer throughput, gdb probe or XTEST interaction was observed.
Both actor directories were restored and no game remained running.
Steam and the user's installation/saves were untouched.

Evidence is in this job's meta/live/: launch.txt, restoration.txt,
elf-notes.txt, steam-setconfig.txt, actor-logs/ and actor-data/.
Copied actor logs/data can predate this failed launch and are not evidence
that this build ran. Real-peer Steam performance and visual rendering remain
unverified; this commit introduces no new game hook/lifetime contract.

## Tests

- tools/linux/build_native.sh: soldier build, 57/57 CTests and glibc baseline pass.
- Tunnel fixtures check 1 MiB/s Messages versus 16 MiB/s Legacy initial clamps.
- Shared adapter tests cover backoff, growth, learned ceiling, unknown/idle
  guards, multiple peers, five-second timing and failed-setter rollback.
  Added native status-to-controller sampling and growth setter-order assertions.
- Native panel tests check Cross-play hit targets in Create Game and host lobby,
  including a host without hostSteam, and absence for a joining player.
  Existing lobby tests verify invitation-code/Cross-play event updates.
- python3 tools/linux/verify_lua_release.py: 29 Lua files (one pinned native
  integration) and 32 glyphs pass against 0.6.1.28.
  Manifest: b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c.
- Release script syntax, CRLF-aware whitespace and empty unmerged index checks.
