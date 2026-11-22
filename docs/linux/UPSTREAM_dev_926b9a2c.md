# Windows dev 926b9a2c integration

## Merged

Target `926b9a2ce941bc3f94665aba635e6a103fe6196b`, one commit, onto Linux
parent `cd3ceb2fd26a3bc28f91513cd7ed5eb13f8e08e7`. No conflicts. The merge is
staged and deliberately uncommitted. Windows assembly, hooks, shared deque
helper and upstream standalone test are retained exactly. Release remains
**0.7.1.1**; this commit does not bump the version. All peers must have these
ordering changes; matching release numbers alone cannot establish that.

## Ported (what and how)

Retained upstream's native `vehstop` and `unload` hooks, sorting respectively
the vehicles at a line stop and each cargo unload deque by entity ID before
consumption. Verified the Linux ELF, SysV register contracts, GNU container
layout, guard bytes and instruction boundaries; see
[RE evidence](../re/linux/DEV_926B9A2C.md). Existing identity/byte guards,
installation rollback and `TPF2MP_ORDER_CANON=0` remain in effect.

Updated the ELF verifier to cover all nine sites and the new system vtables,
lookup call and deque stride. Registered the upstream deque regression in
native CTest and extended shim coverage with a real libstdc++ deque.
Advanced Lua/package provenance and packaged this integration record.
Shared Lua is unchanged and exact to upstream.

## Not ported

None of this commit's code changes are omitted. Live container lifetime,
boarding/unloading behavior and cross-platform live-join validation remain
unverified after the launch failure below. Existing unrelated port gaps
remain unchanged.

## Live testing

Installed candidate soldier libraries and merged Lua into backed-up native
lab payloads, set autoload=1, and ran
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
Exit 1: `bwrap: setting up uid map: Permission denied`, before game startup.
No title menu, Vulkan device, gdb probe or gameplay was observed. Both
payloads were restored with matching file/symlink manifests. No save changed,
no game remains, and Steam/user installations were untouched. Evidence is
in this job's `meta/live/`; copied actor logs/data predate this run.

## Tests

- `tools/linux/build_native.sh`: soldier build, 78/78 CTests and glibc
  symbol baseline <=2.31 passed. Final log: `.git/port-926b9a2c-build-final.log`.
- `python3 tools/linux/verify_lua_release.py`: 31 exact Lua files, zero
  exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 tools/linux/verify_order_canon_elf.py GAME_ELF`: build-id, nine
  guards, instruction boundaries/no interior branches, vtables, lookup,
  deque stride and complete family-getter inventory passed.
- `bash -n tools/linux/build_release.sh` and whitespace checks passed.
  Windows/MSVC compilation and live multiplayer were not run.
