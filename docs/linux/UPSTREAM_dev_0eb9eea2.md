# Windows dev 0eb9eea2 integration

## Merged

One Windows commit, `0eb9eea2b05ac132565e6a4bbc3d3a981f3a60ca`, onto Linux
parent `3575e7133f1e471e338313a7814af6d6eb85cac5` on `port/dev`:
`bigmap: commit counts as tight under 3 GiB free (was 4)`.
No conflicts. All three incoming Windows files remain byte-identical to the
target. The merge is staged and uncommitted; release remains **0.7.1.1**.

## Ported (what and how)

Retained the Windows threshold change: RAM/8 bounded to 2..3 GiB, unknown RAM
falls back to 3 GiB, and positive `commit_tight_mb` still overrides it.
Retained native MemAvailable-based paging, following the prior b6d73041
integration. Windows free commit and Linux physical RAM headroom are different
resources; the native backend has no commit-tight predicate. The native cfg
already documents that `commit_tight_mb` has no effect.
See [source review](../re/linux/DEV_0EB9EEA2.md).

Added README, INSTALL and Big Maps PORT entries and packaged this integration
record. Kept the Lua verifier/BUILDINFO baseline at 65302e5d after checking
that its mod tree equals this target. No addresses, layouts, byte guards or
ABI changes are needed. Native policy tests already cover the retained
behavior; the incoming Windows test updates are preserved. No additional
native tests are needed for documentation and the package-list change.

## Not ported

None newly required for this commit. This Windows-only threshold does not
apply to the existing native controller. Earlier native material/small-pager
and Windows commit-controller differences remain inherited limitations; no
full memory-policy parity is claimed.

## Live testing

No game, Proton peer, gdb session or desktop input was run. Native runtime
behavior and engine contracts are unchanged; validation uses existing native
fixtures and the read-only ELF verifier. No title-menu, Vulkan device,
loaded-world performance or multiplayer observation is claimed. Lab actors,
saves, Steam and the user's installed mod were untouched, so no backup or
restoration was needed. No game process was started.

## Tests

Validation logs in this clone: `.git/port-0eb9eea2-{build,lua,elf}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.

- `tools/linux/build_native.sh`: PASS with the pinned soldier SDK, 78/78
  CTests in 62.89 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- Actual lab ELF GNU build-id and all 39 guarded Big Maps sites: PASS.
- Incoming file equality, mod-tree equality to the retained baseline,
  release shell syntax, whitespace checks, empty unmerged index and retained
  target MERGE_HEAD: PASS.
- Windows/MSVC build and Windows ctypes tests, release packaging and live
  gameplay were not run.
