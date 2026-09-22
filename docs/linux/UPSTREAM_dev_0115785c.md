# Upstream dev 0115785c integration

Windows target: `0115785c2e92997455562e9335c1230a9b461e86`.
Linux parent: `19ce7d037c8fdcda79e0d2928871976f445aad75`, following
[5c6c084b](UPSTREAM_dev_5c6c084b.md). Two Windows commits, no conflicts.
Merge remains staged and uncommitted. Status: **PARTIAL**, pending loaded-world
validation and default activation of canonical ordering.

## Merged

- `ce563b8`: match TCP link hellos using assigned then requested joiner names;
  disambiguate by address; open/map a Steam joiner's TCP listener and offer WAN
  first; record dial-failure reasons; remove its UPnP mapping at shutdown.
- `0115785`: canonicalize every ECS NodeList and its entity-to-position index
  before each simulation iteration, using shared `family_canon.h`.

Windows hooks, assembly and shared canonicalizer are preserved.

## Ported

Native `order_canon_linux.cpp` adds a seventh guarded site inside
Engine::Update (`0x32515c5`, RDI engine, XMM0 dt). The native libstdc++ family
walk recognizes NodeList<1..5>, validates memory ranges and indices, and uses
the same shared algorithm. It retains the existing opt-in setting
`TPF2MP_ORDER_CANON=1`. [RE evidence](../re/linux/DEV_0115785C.md) records the
actual ELF bytes, offsets, ABI, fixture coverage and missing live proof.

The Python changes merge directly. The new renamed-joiner test exposed a
Linux-only obstacle: `strace` showed every bound dial failing EADDRINUSE
because its listener already held that port. Linux now sets SO_REUSEPORT on
both dual dial/listener sockets and the bulk listener which receives host
links. Windows retains SO_REUSEADDR alone. After the fix, the renamed joiner
receives 300/300 frames with simulated 30% UDP loss; the link-on/off comparison
receives 100%/70%. Real router mapping and external Steam networking were not
tested; the new UPnP regression uses an injected mapper.

README/install links, Lua baseline and package provenance now name this commit;
release packaging includes this record alongside prior integrations.

## Not ported

**Default-on, live-proven per-iteration canonical ordering.** The guarded native
implementation and static contracts are present, but the lab could not reach
a world. Standard launch fails uid-map setup; isolated fallback exits 53 at
Steam initialization. No loaded-game gdb observation could establish engine
lifetimes, all actual families, or absence of concurrent position consumers.
Canonical ordering remains default-off, including the previously unvalidated
hooks. Matching versions still do not establish Windows/native simulation parity.
The detailed RE record lists the missing probes. No executable address or
lifetime is claimed to have been verified live.

## Live testing

Installed the soldier-built libraries and merged Lua only into backed-up lab
copies, including a final-build attempt with TPF2MP_ORDER_CANON=1. Requested
RADV and 1280x720; no GPU selection, menu, save load, simulation or gdb probe
was reached. Both standard and fallback attempts failed as described above.
The final game attempted its automatic Steam bootstrap inside the lab, which
failed read-only logging and missing libGL; its test-created shells/dialog were
cleaned up. No Steam client was manually started, stopped or restarted.
All library/mod/userdata backups were restored and hash-checked; no test game
remains running. Evidence and cleanup details: job `meta/live/`.

## Tests

- `tools/linux/build_native.sh`: soldier build, **58/58 CTests**, glibc <=2.31
  checks passed. Includes the seven-shim fixture and shared family algorithm.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files, one pinned Linux
  origin-replay integration, 32 exact HUD glyphs; manifest SHA-256
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.
- `python3 tools/linux/verify_order_canon_elf.py <lab ELF>`: all seven sites,
  both getters, five RTTI/vtables and iteration call passed.
- `.git/port-venv/bin/python tools/test_dual_link_names.py`: passed after the
  Linux bind fix; the initial failure and syscall trace are preserved.
- `.git/port-venv/bin/python netpunch/lobby.py --selftest-dual`: passed.
- `.git/port-venv/bin/python netpunch/bulk_tcp.py`: 64 MiB transfer identical,
  wrong-token refusal passed.

Logs are in `.git/port-*.log` and copied into the job's `meta/live/`.
Python test dependencies were installed only into `.git/port-venv`.
