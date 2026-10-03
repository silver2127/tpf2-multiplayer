# Windows dev 2f65bae3 integration: packed terrain edits

## Merged

Windows target: `2f65bae35c75b3cc9eef90068e98d65f7cca009e`.
Linux parent: `532fcad526a54ab05f63298bb081252f9942a582`.
One Windows commit, no conflicts. The merge remains staged and uncommitted.
Upstream `terrain_assets.inl`, `tplz.h`, `test_tplz.cpp` and `test_tplz.bat`
are retained byte for byte. Release version remains 0.7.0.5.

## Ported (what and how)

Native terrain capture now passes the canonical Windows-layout TPTG v1 blob
through the exact shared `TplzPack` before base64 encoding. TERRAINCAP reports
the wire length; the native log reports raw, wire and base64 sizes. A frame
that does not shrink (or cannot allocate its packing buffer) stays version 1.
The underlying Linux grid reader and mask conversion remain unchanged.

Native replay accepts version 1 and version 2. Version 2 is bounded to 64 MiB,
decompressed and checksum-checked before the existing v1 grid validation and
allocation into the game's carrier. The v1 header minimum is applied after
unpacking, so short compressed frames work. Nested frames, invalid grid
lengths, truncation, trailing bytes, oversized lengths and checksum failures
are rejected before carrier installation. Temporary codec allocations use
RAII; game-owned allocations still use the established native allocator.

The codec is wired only into capture/replay, keeping validation-only uses of
`EncodeTerrain` uncompressed. TPAS assets are unchanged. All session members
need this build: the unchanged release handshake cannot detect an older
terrain reader. README and INSTALL state that requirement. The Lua verifier
and release BUILDINFO now name this target, and packages include this record.

No hook, byte pattern, engine offset, ABI, lifetime or patch site changed.
See [terrain evidence and scope](../re/linux/SLICE_TERRAIN_ASSETS.md#dev-2f65bae3-wire-compression).
The change needs no additional engine reverse engineering or live ABI probe.

## Not ported

None from this commit. Previous native feature gaps and live gameplay
validation limits remain unchanged.

## Live testing

No game or Proton peer was launched, no gdb/XTEST input was used, and no
live terrain stroke or multiplayer latency result is claimed. Steam, the
user's installation, lab actor contents and saves were untouched; no backup
or restore was needed. No live logs were generated.

The existing offline ELF fixture was run against the lab's build-35924
binary. It maps ELF segments privately, checks the existing constructor hash
and runtime byte gates, calls the isolated constructor and resolves the
allocator in the test process. This is not a running-game observation.
The fixture exercised the real native capture/replay handlers: a corrupt
packed file was consumed without modifying the empty carrier, a valid packed
file installed readable terrain, and capture emitted a version-2 TERRAINCAP
with the correct wire length. The canonical version-1 path is also covered.

## Tests

- `tools/linux/build_native.sh`: pinned soldier SDK build, **69/69 CTests**,
  and glibc <=2.31 checks pass. Log: `.git/port-2f65bae-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  integration), 32 HUD textures pass; manifest SHA-256
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
  Log: `.git/port-2f65bae-lua.log`.
- `native/linux/out-soldier/test_slice_construction_terrain
  ~/.local/share/tpf2mp-lab/native/game/TransportFever2`: pass, including native
  handler coverage above. Log: `.git/port-2f65bae-terrain-elf.log`.
- `test_tplz` (now a native CTest) and a separate GCC AddressSanitizer build
  with leak detection: pass. Synthetic upstream paint fixture: 402,125 bytes
  to 14,142 compressed bytes (plus 24 bytes for the frame). Terraform fixture:
  244,800 to 165,095 bytes. Of 20,000 damaged frames, 19,982 were rejected and
  18 decoded to the exact original. This is an offline compression result,
  not a live performance measurement. Logs: `.git/port-2f65bae-tplz.log` and
  `.git/port-2f65bae-tplz-asan.log`.
- Native wire tests cover both directions through the unchanged Windows
  codec, base64, old v1 frames, short v2 frames, checksum corruption, every
  truncated prefix of the packed fixture, zero/oversized raw lengths,
  trailing data, nested frames and invalid decoded grids.
- Release builder shell syntax, upstream file equality and merge-state checks
  pass. Staged whitespace checks pass with `cr-at-eol`; the default check
  flags upstream CRLF lines in `terrain_assets.inl`, preserved deliberately.
  No commit, merge abort, push or publication.
