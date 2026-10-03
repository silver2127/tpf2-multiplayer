# Windows 0.7 / dev 8c3c02a5 integration

Windows target: `8c3c02a5a886151c54133669dae9ef2437644d28`.
Linux merge parent: `b16178c0413ae84530cee095edf23e4623ad8ab6`.
Follows [ef275a3c](UPSTREAM_dev_ef275a3c.md). Four first-parent Windows commits;
the Big Maps merge also introduces its historical ancestry. Merge remains
staged and uncommitted. Status: **PARTIAL**.

## Merged

- `c6a17ba`: import Windows Big Maps into `bigmap/`.
- `3aaaf3d`: unified Windows build, deployment and MSI payload.
- `f2c22ca`: release/version 0.7 and release notes.
- `8c3c02a`: Windows live join defaults on; `0`, `off`, `no` disable it.

Resolve README by retaining Linux documentation and adding the Big Maps row.
Resolve HOTJOIN_ORDER by retaining native evidence and documenting both defaults.
Windows build/installer/hook paths are preserved. Shared multiplayer Lua is
unchanged relative to the previous integration, including the pinned native
origin-replay exception. Release provenance and Lua verification target 0.7.

## Ported (what and how)

Import the existing Big Maps native implementation from sibling `4769cd3`, with
its tests, Linux configuration, site manifest and historical RE evidence.
Use multiplayer's identical shared plugin ABI header directly. The unified
native CMake build now builds/tests Big Maps; release packaging ships its plugin,
Linux config and density restoration helper. Auto-install already hashes tracked
sources, so in-tree Big Maps changes participate without a sibling checkout.
The optional external-worktree override remains available, with a separate build
folder. Upgrade/uninstall restores exact density patches before removing the
helper and refuses to discard user-modified patches.

Native support includes extra map sizes/ratios, sparse density, guarded depth 11,
lossless opt-in userfaultfd terrain compression, fast saves and SSE2 terrain scans.
Do not use the Windows depth-13 config on Linux. Preserve the MSVC codec-test path
and add a portable fopen path for native tests. Only Tpf2mpPluginInit is exported.
All native libraries, including Big Maps, are checked against glibc <=2.31.

Native live join remains explicit opt-in because default-off native canonical
ordering lacks loaded-world validation. Windows keeps its new default. Policy
regressions cover both platforms, missing/empty/malformed controls, switches and
rereading. Existing sync tests cover retained rounds and fallback.

## Not ported

Windows Big Maps features beyond the imported native implementation: minimap;
depth 12/13 and placement-distance/budget changes; material-index fast path and
material/small/block paging; adaptive memory policy, COW/dedup and warmup;
alignment batching/fast path, refined terrain optimizations and save/load sidecar;
instance shrinking, generation-memory buffer reuse and world-entry timing;
travel-time configuration. Windows source and docs remain present, but these
features are not activated or advertised as native implementations.

Static searches/disassembly and the failed live attempt are documented in
[DEV_8C3C02A5](../re/linux/DEV_8C3C02A5.md), including the specific missing ABI,
ownership, lifetime and constant-use evidence. No guessed Linux patches added.
Default-on live join also remains unported pending those earlier ordering probes.
For a mixed session, turn live join off on the Windows host too; no capability
negotiation was added. Inherited parity limitations are not resolved by version 0.7.

## Live testing

The prescribed lab launcher failed before game startup with `bwrap: setting up
uid map: Permission denied`, exit 1. Newly built libraries, Big Maps and Lua were
installed only in the backed-up actor copies; both directories were restored and
compared byte-for-byte afterward. No menu, GPU, gameplay, save load, gdb register
observation, two-peer join or on-screen feature was observed. No save changed;
no game left running; Steam and the user's installation were untouched.
Evidence: job `meta/live/` (copied actor logs/data include older runs).

## Tests

- Soldier unified build: 62/62 CTests pass, including four Big Maps suites;
  glibc baseline checks pass. Initial codec fixture MSVC fopen_s failure fixed.
- Lua verifier: 29 files, one pinned origin-replay exception; 32 exact HUD glyphs.
  Manifest `fb3b58aee11225608e15d1f7d151e4562a5ac03c531f289e3eaa290dbbae7755`.
- Big Maps ELF checker: build-id and all 20 sites pass. Canonical-order ELF
  checker: seven sites plus family metadata and iteration-call contract pass.
- Shared sync operation: 31 tests; sync runtime: 17 tests; native live-join
  policy and auto-install regressions pass.
- Combined `.run` package and black-box install/upgrade/uninstall tests pass,
  including Big Maps payload and density-record restoration. The package test
  used `/bin/true` as an explicitly supplied lobby placeholder (`0.7-port-test`);
  it does not validate a frozen lobby executable or constitute a shipping release.
- Independent Big Maps builder: 4/4 tests, export and glibc checks pass after
  correcting its read-only mount layout and avoiding duplicate version scripts.
- Shell syntax and empty unmerged index pass. Staged whitespace checks pass with
  core.whitespace=cr-at-eol; plain checks flag preserved upstream CRLF. Logs retained
  under `.git/port-*.log` and copied to job `meta/live/`.
