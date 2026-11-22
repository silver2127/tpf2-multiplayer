# Windows dev 262353d7 integration — housekeeping and correctness fixes

## Merged

Windows target: `262353d7be82de14622ae6bccdbe1a2e921517ca`.
Linux parent: `f30ac8d0087bd4a12284282a9b373c62bb763bdf`.
This batch incorporates merge commits `9b1f23b` (PR #17, housekeeping) and
`262353d` (PR #16, fixes), including their topic commits. Later pending Windows
commits are outside this integration. Release remains **0.7.1.3**.

Resolved README using upstream's Linux-notes list while preserving the native
sidecar default-on history and outstanding live-validation caveat. Resolved
`tools/test_host_missing_mods.py` using upstream's platform-native `SAVE`
constant, retaining Linux's portable path for the separate long-list case.
Merge remains staged and uncommitted.

## Ported (what and how)

- Shared Lua is byte-for-byte upstream: strip history tags before greedy
  command parameters, forget sold vehicle IDs in all registries, compare stop
  signatures with waypoint suffixes, retain alternative terminals and avoid
  replaying an originator's stop replacement, and resolve planned rail
  crossings against both street and rail graphs. Retain the `groundAt` export
  correction, scoped vehicle-config helper, and dead-code removal.
- Shared Python preserves renamed players' letters and stream identities,
  supports spaces and UTF-8 boundaries in TCP link names, rejects excessive
  fragment counts, contains malformed host messages, ignores stale nonzero
  save acknowledgements, retransfers a save under a fallback epoch, cleans
  broken mod archives, bounds replay-window shifts and fixes bulk accounting.
  Retain sealed-peer address proof, master-list privacy and input/rate guards,
  platform data-directory lookup, TLS context, log redaction, and flag/help fixes.
- Native bridge `SetTailPath` now clears `tailFromZero` when the instance's
  capture path changes. The supplied Linux epoch regression checks a reset
  followed by a letter/path change, preventing old-session commands being resent.
- Consolidated Linux train-order and movement kill-switch readers in
  `native/linux/src/slice/flags_linux.h`, matching upstream's reader cleanup.
  Preserve prefix matching and root-file precedence, including a root file
  without the requested key. Added filesystem regressions for those cases.
- Windows terrain byte-count multiplication was hardened upstream. Linux's
  `Cells` already rejects more than `MaxBytes * 8` cells (536,870,912) before
  `ValidTerrain` multiplies by eight, so it cannot overflow uint64. Added
  encode/decode rejection cases for enormous positive dimensions with tiny
  payloads; existing valid terrain/wire round trips remain covered.
- Windows menu array growth, truncated escape parsing, and clipboard conversion
  fixes are retained. Linux already uses `std::string` for model/request codes,
  an end-bounded `JsonReader` (`Hex4` checks four bytes), and SDL-owned clipboard
  text copied to a string then freed. Added public-list regressions for each
  truncated escape and 159/255/256-byte codes (255 accepted, 256 excluded).
- Windows `FD_SETSIZE=128` stays under `_WIN32`. Linux's shared Steam transport
  already rejects descriptors outside the native `fd_set` capacity in
  `BindLoopback`, and selects with the highest descriptor plus one. Existing
  native UDP/Steam API fixture tests cover both transport modes.
- Retain shared inline ordering helpers, Linux missing-header and GCC/glibc
  compatibility fixes, Big Maps test assertion retention and FILE deleter,
  shell quoting fixes, Python/test cleanup, Lua lint and doc-link fixes.
  Added one doc-checker exception for a removed test named in a historical
  Linux integration record, consistent with upstream's historical-path exceptions.
  Removed unused Windows logger has no Linux counterpart to remove.
- Updated the exact Lua baseline, packaged integration record, BUILDINFO
  provenance and README/INSTALL pointers.

Reviewed the installation layout and prior integration records plus
`docs/re/linux/TRAIN_ORDER.md`, `SLICE_TERRAIN_ASSETS.md` and `MENU_GAME.md`.
No changed hook site, ELF address, byte pattern, structure offset, calling
convention or game-owned lifetime is introduced. The fixes operate on mod-owned
files, strings, wire data and existing shared logic; no new RE site is needed.
Existing runtime byte/build-id gates remain intact.

## Not ported

None from this batch. Windows-specific buffer/conversion/socket fixes have
safe Linux counterparts as detailed above; MSVC and Win32 paths remain intact.
Existing limitations are unchanged: native Sandbox town-tool capture,
minimap/cargo filtering, material-index acceleration/probing, and outstanding
live terrain ownership/load-completion validation. This integration does not
claim new cross-platform gameplay or performance parity.

## Live testing

No game, Proton peer, debugger or desktop input was run. No actor payload,
userdata or save was changed, so backup/restoration was unnecessary. Steam and
user installations were untouched. Network regressions use local sockets and
fixture peers; native tests use synthetic engine memory and fake Steam APIs.
No on-screen result, GPU observation or live performance result is claimed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 79/79 CTests
  (63.10 seconds), glibc <=2.31 checks passed. Includes bridge epoch, terrain
  codec, lobby parsing, train/movement ordering, both Steam transport fixtures
  and Big Maps tests. Existing logarchive/codec and vendored stb warnings remain;
  no warning-free build claim.
- `python3 tools/linux/verify_lua_release.py`: PASS; 31 exact upstream Lua files,
  no exceptions, 32 HUD glyphs and two toolbar textures. Lua manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`.
- 20 shared Python/Lua regression scripts passed: host malformed messages,
  broken mod zips, network guards, sealed peer lock, rename letters, save fack
  ordering, stop signatures, dual-link names, missing host mods, sync operation,
  sync runtime, crossing replay, history retention, vehicle name/reuse,
  vehicle reversal, depot buying, live catch-up gaps, relay mod download,
  rendezvous and relay selftest.
- Launcher release (39), native watchdog (3), movement probe (8), and people
  probe (8) offline tests: PASS. Publication APIs were mocked; nothing published.
- Lua lint: 32/32 files parsed. Documentation checker: 377 documents,
  zero dead file links, anchors or missing prose paths (56 informational orphans).
  Shell syntax checks passed for the changed shell scripts and release builder.
- `add_handle_test.py`: initially unavailable due to missing `pefile`; after
  installing it locally, SKIP because its hardcoded Windows game path is absent.
  No Windows binary test or MSVC build result is claimed.
- Dependencies (`lupa`, `pystun3`, `zstandard`, `cryptography`, `luaparser`,
  `pefile`) were installed only in `.git/port-venv`, with cache disabled and
  temporary files inside `.git/test-tmp`. No system packages installed.

Logs: `.git/port-262353d-{build,lua,python,tooling,doclinks,add-handle,deps}.log`;
CTest detail: `native/linux/out-soldier/Testing/Temporary/LastTest.log`.
The Python batch log preserves the initial Windows-only dependency failure;
`add-handle.log` records the final skip. Unmerged index and CRLF-aware staged
whitespace checks are clean; target MERGE_HEAD is retained.
