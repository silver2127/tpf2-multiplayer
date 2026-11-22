# Windows dev 71549cff integration — 0.7.1.2 sidecar default

## Merged

Two incoming commits: `1328bd7b04fc4abbb501569f4b0ecfa73a58742c` (merge
of earlier Linux integrations through 10c327a7) and
`71549cff0bfe90656cfa4acbfae3754ed0690030` (native sidecars on by default).
Linux parent: `197e563335ee3076e01e54e445c3ebc1c32bda43`.
The first commit's tree equals the Linux parent's tree. The effective delta
is seven files changing the native default, its regression tests and release
notes. No conflicts; merge left staged and uncommitted. Release stays 0.7.1.2.
Windows runtime code, shared Lua and lobby code are unchanged.

## Ported (what and how)

Retained upstream's native `cfgBool(...,"terrain_sidecar",1)` fallback and
shipped `terrain_sidecar=1`. Explicit 0 disables capture, restore and sidecar
alignment bypass. Existing build-ID, byte checks and partial-hook forwarding
remain intact. Retained upstream's regression asserting all three hooks and
the alignment redirect with the key absent, and no sidecar hooks/redirect
with explicit 0. Existing mismatch and hook-failure tests remain in place.

Corrected README's stale default-off claims and marked the historical default
in Big Maps PORT.md. Advanced README/INSTALL integration links, packaged
integration record, BUILDINFO provenance and Lua verifier reference to this
commit; the Lua tree is identical to the prior baseline.

Reviewed the existing [sidecar ABI evidence](../re/linux/DEV_2B4FD093.md)
and [lookup evidence](../re/linux/DEV_5FD49A24.md). Fresh read-only verification
of the lab ELF passes its GNU build-id and all 39 Big Maps sites. This change
introduces no address, instruction pattern, field offset, calling convention
or ownership mechanism, so no new disassembly or live ABI derivation is needed.
Default enablement follows the incoming release policy; it is not new evidence
of the previously unresolved runtime contracts.

## Not ported

None from these two commits. Inherited material-index acceleration/probing,
company/UI and cross-platform validation gaps remain outside this delta.
Sidecar live cache ownership, save-world identity, worker/pager synchronization
and load-completion validation remain outstanding as documented in the earlier
RE record. They are not resolved by enabling the existing implementation.

## Live testing

No game, Proton peer, debugger or desktop-input sequence was run. This is a
configuration-default change to the existing native implementation, with no
new engine contract to derive. No lab payload or save was modified; no backup
or restoration was needed. Steam and installed user files were untouched.
No live capture/restore, rendering device, performance or gameplay result is
claimed. Earlier failed lab launches are historical evidence only.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 78/78 CTests
  in 62.91 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS against 71549cff; 31 exact
  Lua files, zero exceptions, 32 HUD glyphs and two toolbar textures.
  Manifest SHA-256: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id
  `3a0e156390b0e6f1e372051c24802c8493ae454a` and all 39 sites.
- `bash -n tools/linux/build_release.sh` and Git whitespace checks: PASS.
- Windows/MSVC compilation, release packaging and live gameplay not run.

Logs: `.git/port-71549cff-build.log`, `.git/port-71549cff-lua.log`,
`.git/port-71549cff-elf.log`, and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
