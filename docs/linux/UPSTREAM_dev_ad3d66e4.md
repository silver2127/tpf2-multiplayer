# Upstream dev ad3d66e4 integration: canonical ordering defaults on

Windows target: `ad3d66e4f94821f785c8ad5debe86db03dcbda97`.
Linux parent: `535b551a5b066f4d5c4b9be4a6c7a6f250ad673d`.
One Windows commit. Merge staged and uncommitted. Result: **DONE** for this
commit; inherited live-validation gaps remain as described below.

## Merged

Resolved `docs/re/HOTJOIN_ORDER.md` by retaining the upstream Linux-default
section and the native implementation and retained-world-join notes. Updated
the obsolete default-off description of the native family canonicalizer.
Windows code, shared Lua, lobby code and release version are unchanged.

## Ported (what and how)

Both `order_canon_linux.cpp`'s install gate and
`person_map_order_linux.cpp`'s cached startup predicate now enable canonical
ordering for an unset variable or any value other than exactly `0`.
`TPF2MP_ORDER_CANON=0` disables it with status `off (TPF2MP_ORDER_CANON=0)`.
The person-map walk still requires the canonical module's active flag, so it
cannot enable its ascending walk when hook installation fails. Build-id,
byte guards, family-layout checks and transactional rollback are unchanged.
README, installation instructions and the public header document the default
and matching peer settings; release packaging includes this record.

No new address, offset, byte pattern or calling convention was needed.
Re-ran `verify_order_canon_elf.py` against the actual lab build-35924 ELF
(build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`): all seven guards,
instruction boundaries, absence of interior branch targets, family getters,
NodeList RTTI/vtables and the Step iteration call passed. Existing SysV/layout
evidence remains in [DEV_0115785C](../re/linux/DEV_0115785C.md) and
[DEV_B7760259](../re/linux/DEV_B7760259.md).

Decision: apply the owner's explicit default-on requirement despite the
inherited live-validation limits. Historical integration/RE records describe
their original results and are not rewritten. Native retained-world joins
remain separately opt-in; this commit requests changing the sorts' default.

## Not ported (and why)

None from this commit. Loaded-game lifetime/synchronization, performance and
cross-platform retained-host parity remain unverified, as earlier RE records
explain. Default-on is an implemented policy change, not evidence that those
inherited validation gaps have passed.

## Live testing

Copied the new soldier libraries, Big Maps plugin and merged Lua to the native
lab actor after `cp -a` backups with suffix `.before-port-ad3d66e4` (preserving
older backups). Ran `tools/sandbox/tpf2mp-lab run native --root
~/.local/share/tpf2mp-lab` with `TPF2MP_ORDER_CANON` unset and a 180-second limit.
The launcher exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game process, title menu, Vulkan device, loaded world, gdb probe or gameplay
was observed. No XTEST input or Steam operation was performed. Restored both
actor directories and checked `diff -qr`: both exit 0. No save was changed and
no lab game remains running. The job's `meta/live/` includes the launch log,
restoration log and archived actor logs/data; older actor logs are not evidence
of a successful launch in this job.

## Tests

- `tools/linux/build_native.sh`: soldier build, **62/62 CTests passed** and
  glibc compatibility checks passed.
- Updated `order_canon` fixture checks installation for unset, empty, `1`, `0`,
  `00` and `false`; exact `0` leaves bytes unchanged and reports the kill switch.
- Updated `person_map_order` tests use fresh child processes for the cached
  environment: the same six values, ascending walks for enabled cases, and
  refusal while the canonical module is inactive. Existing real-shim ABI,
  layout, lifetime fixture and rollback suites still pass.
- `python3 tools/linux/verify_lua_release.py`: **29 Lua files**, one pinned
  Linux integration, and **32 HUD glyph textures** passed; manifest SHA-256
  `fb3b58aee11225608e15d1f7d151e4562a5ac03c531f289e3eaa290dbbae7755`.
- ELF verifier passed as detailed above. Release script syntax and staged
  whitespace checks passed; no unmerged index entries remain.

Build, Lua and ELF logs are retained under `.git/port-ad3d66e4-*.log` and copied
to the job's `meta/live/` alongside the lab test script.
