# Windows dev 7cacbaaf integration

Merged `7cacbaaf398b49a5a3bd8778d14eca6ea9449497` into `port/dev`,
based on `linux-native`; merge remains staged and uncommitted. Version stays 0.7.

## Merged

Retained the upstream dedicated-server measurements, `Readable` cost contract,
VPS sysctl configuration and installer changes. Resolved `tools/server/README.md`
by retaining both native and Proton runbooks and adding the kernel-limits section.
The setup script was syntax checked, **not executed**; no host sysctls changed.
Windows code paths and shared Lua remain unchanged from the merge.

## Ported

The family walk now uses Linux `PROCMAP_QUERY` when available (Linux 6.11+).
It asks the kernel for the VMA covering the address, requires read/write
permissions as appropriate, and checks every VMA crossed by the range. It
neither enumerates the process's unrelated mappings nor caches positive results.
The descriptor is thread local, close-on-exec and closed at thread exit; normal
queries use stack storage and no userspace mutex or allocation.

On older kernels or when ioctl access is denied at initialization, retain the
fresh per-iteration snapshot for compatibility. Its parser now uses buffered
`read` and a hexadecimal prefix scanner instead of `fgets`/`sscanf`; lookups
start with binary search. This fallback still enumerates mappings and may allocate:
it does **not** provide the modern-kernel performance guarantee. Keeping it
preserves the existing permission checks and ordering behavior on older systems.

Endpoint probes would miss unreadable holes and cannot prove writability;
`msync` also cannot establish read/write permissions. A refresh-only-on-miss
cache would accept stale permissions after `mprotect` or `munmap`. Those upstream
suggestions were therefore not used. Existing slice probes already avoid maps
parsing and are unchanged. No game address, layout or calling convention changed.
See [implementation evidence](../re/linux/DEV_7CACBAAF.md).

## Not ported

None of this commit's changes are omitted. Camera placement and terrain-pager
ideas in the upstream profiling report are suggestions, not new Windows
implementations. Existing unrelated port limitations remain in their records.
Older-kernel performance and loaded-game performance remain unmeasured.

## Live testing

Installed the newly built libraries and merged Lua only in the native lab actor,
after backing up its library, mod and userdata trees with the suffix
`.before-port-7cacbaaf` (pre-existing backups were preserved).
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`
exited 1: `bwrap: setting up uid map: Permission denied`. The game executable
was not reached; no title menu, GPU selection, loaded world, gdb or gameplay
result is claimed. No game or Steam process appeared in the final process scan.
Steam was not started, stopped or changed. All three trees were restored and
SHA-256/symlink inventories matched. Evidence is in this job's `meta/live/`;
copied actor logs also contain earlier runs and are not new test observations.

## Tests

- `tools/linux/build_native.sh`: final soldier build, 63/63 CTests and glibc
  baseline check passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures passed.
- `verify_order_canon_elf.py` against the actual lab ELF: build-id, seven patch
  sites, instruction boundaries, getters and NodeList RTTI passed.
- Extended `order_canon`: both real ioctl and forced snapshot fallback checked
  across read-only/PROT_NONE middle pages, an unmapped hole, remapping, exact
  boundaries, zero length and overflow; existing family/shim tests passed.
- Synthetic 50,000-page alternating-permission VMAs: 5,415.95 us per buffered
  snapshot+range (20 iterations), 0.102 us per ioctl refresh+range (10,000).
  Host kernel 7.0.0-31-generic. This is a local microbenchmark, not a simulation
  speedup or reproduction of upstream's 49,000-tile server profile.
- `sh -n tools/server/setup_vps.sh` and staged whitespace check with
  `core.whitespace=cr-at-eol`: passed (upstream Windows source retains CRLF).
