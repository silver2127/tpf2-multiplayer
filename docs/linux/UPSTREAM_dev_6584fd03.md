# Windows dev 6584fd03 integration: plugin-owned Fantasia buffer reuse

## Merged

Target `6584fd03a529685b3edceebd8e88ed90b09e5407`, one Windows commit.
The Windows import routing, default configuration, embedding and documentation
are retained. Both modify/delete conflicts are resolved by accepting deletion
of the retired stand-in installer/test. Their Linux path selection and climate
coverage now live in the replacement test. The merge remains uncommitted.
Release version stays 0.7.0.7; the Lua release pin and package BUILDINFO now
name this target, retaining the existing native origin-replay exception.

## Ported (what and how)

Native Big Maps embeds the same Lua pass at CMake configuration and shares
the Windows text transformation in `generator_memory_text.h`. A length-indexed
anchor scan fixes short-input pointer arithmetic without changing valid output.
The Windows PE routes remain intact. Linux byte-verifies the executable's
16-byte fopen PLT entry and redirects it to a SysV detour. Read-only Fantasia
opens receive an independent anonymous temporary stream; unrelated and writable
opens go directly to libc. Missing/repeated anchors and read/allocation/write
failures fall back to the original. The source file is never edited.

The switch defaults to `generator_memory=1`, with the same >32768² m² area
threshold as Windows. No extra mod is needed. Disable the retired low-memory
stand-in if previously installed. Linux exact-case path matching follows its
filesystem conventions. The Windows persistent `%TEMP%` cache is replaced by
per-open streams so multiple callers cannot overwrite each other's copies.

[RE record](../re/linux/DEV_6584FD03.md) derives the site, ABI and fclose
ownership from the actual ELF. Its new site is in `bigmap/linux/sites.json`.
Documentation and the release package include this integration record.

## Not ported

None from this commit. Earlier native feature gaps are unchanged. Native
Fantasia selection, generated terrain and process peak-memory savings still
need live validation; symbolic pipeline equivalence is not a memory benchmark.

## Live testing

Backed up the actor's `share/tpf2mp` and `game/mods/mp_lockstep_1` to
`.before-port-6584fd0` siblings, then installed this build and the merged Lua.
Ran `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
It exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game reached startup, so no gdb attachment, Vulkan-device observation,
map generation, rendering, saves or multiplayer results are claimed.
No Steam command or desktop input was issued. No system policy was changed.
Both payload trees were restored and `diff -qr` matched their backups. No
save was loaded/changed and no game process remains.

The job's `meta/live/` contains launch, restoration, static disassembly,
verification logs and copied actor logs/data. The copied actor logs/data are
historical: this launch never ran the game.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 72/72 CTests;
  glibc <=2.31 validation passed.
- New native generator test: executes the emitted absolute jump through a
  mapped code page; verifies switch-off, byte mismatch and patch failure;
  three filenames, anchor refusal/CRLF/EOF, original-file preservation,
  independent stream lifetimes, update-mode bypass, changed-source fallback,
  and missing-file errno.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception and 32 exact HUD textures. Lua manifest SHA-256
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- `python3 bigmap/tools/linux/verify_game.py <native ELF>`: PASS, GNU build-id
  and all 30 patch sites.
- Lua 5.2 symbolic test uses the native build's shared transformation and
  installed Fantasia resources read-only: PASS. At 40 km, temperate and
  tropical reduce 325 -> 10 names; dry reduces 328 -> 10 (the lower bound).
  All three 32 km pipelines are exactly unchanged; line numbers are preserved.
- Release script syntax and merge-index/whitespace checks pass (upstream CRLF
  treated as line endings). Windows compilation was unavailable on this host.

Logs: `.git/port-6584fd0-{build-final,lua,elf,fantasia}.log`. No commit,
merge abort, publication or system-wide installation occurred.
