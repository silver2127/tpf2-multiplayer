# Windows dev 65302e5d integration

## Merged

One Windows commit, `65302e5d0eed83865488766003fb97e6e9768eea`, onto Linux
parent `8ff7f42` (port/dev). Resolved conflicts in native Big Maps initialization,
sidecar implementation and sidecar tests. Merge remains staged and uncommitted;
release remains **0.7.1.1**. Windows sources, tests, config and profiler remain
byte-identical to the incoming commit.

## Ported (what and how)

Native `terrain_sidecar_decode_at_pass=1` defers decoding from AddTile to the
existing alignment pass, with up to 16 workers including the caller. Pending
terrain identities retain the file until the second version has passed even
when it has not yet allocated a range slot. Logs report pass decode duration.
Preserved native hook-publication gating, unsigned version updates, lookup
accounting, load generations, all-tile skip checks and stock fallbacks.

The verified native alignment wrapper is installed for sidecars even when
batching is disabled; no Windows batching prerequisite is needed. Sidecars
remain experimental and **off by default**. No new ELF site or ABI was needed.
See [static evidence and implementation details](../re/linux/DEV_65302E5D.md).
Updated integration links, package provenance and the Lua verification target.

## Not ported

None of this commit's runtime changes. The dbghelp/PE stack walker remains a
Windows developer diagnostic, like the existing Windows profiler; native
stack inspection uses gdb's ELF unwinder. No native sampling-profiler feature
is claimed. Earlier unrelated native omissions and sidecar live-proof gaps
remain unchanged; see the linked RE record.

## Live testing

Installed candidate soldier libraries, Big Maps plugin/config and Lua into
backed-up native actor copies. Enabled sidecars/deferred decode and autoload,
disabled density rewriting. The prescribed launcher exited 1 immediately:
`bwrap: setting up uid map: Permission denied`. No title menu, Vulkan device,
loaded save, decoding timing, gdb result or cross-platform observation was
obtained. No Proton or desktop input was used. Both payload directories were
restored and `diff -qr` returned 0; no save changed and no game remains running.
Steam and the user's game/mod were untouched. Logs are in the job's `meta/live/`.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, **78/78 CTests**,
  62.80 seconds; glibc <=2.31 checks passed.
- Expanded native sidecar fixture: 6- and 72-tile deferred loads, two terrain
  versions, zero AddTile lookup/decode work, retention before the second has a
  range slot, exact heights/min/max/version updates, repeat loads at reused
  addresses, small-pass refusal, no second skip, and missing-vector fallback.
  72 tiles exercises the parallel path. Existing eager and stream tests pass.
- Native initialization fixture: deferred config default/override without
  batching; existing byte-mismatch and partial-hook failure tests still pass.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures; manifest
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME_ELF`: PASS, build-id and
  all 39 existing guarded sites. Fresh AddTile/publication disassembly saved.
- Windows/MSVC compilation and loaded-game performance were not tested.

Build/Lua/ELF logs are retained in `.git/port-65302e5-*.log` and `meta/live/`.
