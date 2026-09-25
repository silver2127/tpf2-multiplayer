# Windows dev d8a3ce57 integration (release 0.7.0.3)

Windows target `d8a3ce570e429396ca45e32a753ab32a5e2bd88d` onto native parent
`9cd3666` (Port Windows dev e43d01d to Linux). One first-parent commit:

- `d8a3ce5` Release 0.7.0.3: desync, bulldoze freeze and stop fixes.

The merge is retained, staged and uncommitted; git reported no conflicts and
none had to be resolved by hand. **Version moves 0.7.0.2 -> 0.7.0.3.** This is
the release commit for the work the previous integrations ported, not new code.

## Merged

The commit touches four shared files and no code:

- `docs/releases/0.7.0.3.md` and `installer/RELEASE-0.7.0.3.md` (identical
  release notes, new files).
- `installer/VERSION`: `0.7.0.2` -> `0.7.0.3`.
- `netpunch/lobby.py`: `LOBBY_VERSION` `0.7.0.2` -> `0.7.0.3`.

Nothing in `native/`, `native/linux/`, `mod/` or `bigmap/` changes.
`git diff e43d01dd d8a3ce57 -- mod/` is empty, so the bundled Lua and the HUD
glyphs are byte-identical to the previous target. No Windows hook, MSVC
construct, `TransportFever2.exe` address, byte pattern, struct offset or
calling convention is added or moved, so there was no Linux site to find and
nothing was guessed.

## Ported (what and how)

The Linux side of a release commit is the version stamp and the release payload.

**The version reaches the native release automatically.**
`tools/linux/build_release.sh:53` reads `installer/VERSION` when `--version` is
not given, and uses it for the staged `VERSION` file, the `BUILDINFO` header and
the `tpf2mp-linux-<version>` folder and tarball name, which
`tools/linux/self_extract.sh` then wraps as the `.run` installer the notes point
Linux users at. No native source carries a version literal: `grep -rn '0\.7\.0'
native/linux/src native/linux/include native/src` is empty. So the bump needed
no code change, only the three places that name the target explicitly.

**The lobby handshake needs nothing.** `LOBBY_VERSION` is the joined-side gate
(`netpunch/lobby.py:3298` `version_rejection`), and both peers read the same
shared `lobby.py`; the native build does not re-implement it. Every test that
touches it refers to `lobby.LOBBY_VERSION` symbolically
(`tools/version_gate_test.py`, `tools/lobby_mode_test.py`,
`tools/test_lobby_limits.py`) -- no `0.7.0.2` literal survives anywhere in
`netpunch/` or `tools/`, so nothing had to be re-pinned. `version_gate_test.py`
passes on this tree, including its matched-version and mismatch cases.

**Release references advanced to the new target.**

- `tools/linux/verify_lua_release.py`: `REFERENCE` `e43d01dd...` ->
  `d8a3ce570e429396ca45e32a753ab32a5e2bd88d`, and its three labels now read
  "Windows release 0.7.0.3" / "Windows 0.7.0.3". The pinned `inject.lua`
  SHA-256 exception (`df0cf0bb...`, the native cancelled VNAME/VCOLOR origin
  replay) is unchanged and still matches. PASS: 29 Lua files, 32 HUD glyphs.
  Lua manifest SHA-256
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`,
  unchanged from the `e43d01dd` target as expected.
- `tools/linux/build_release.sh:262`: the `BUILDINFO` provenance line becomes
  `Lua: Windows 0.7.0.3 d8a3ce570e429396ca45e32a753ab32a5e2bd88d (pinned Linux
  origin replay)`. It had been left at `0.7.0.2 bd69b864` by the two ports
  since.
- `tools/linux/build_release.sh:244-246`: the staged integration records now
  include `UPSTREAM_dev_0610033.md`, `UPSTREAM_dev_e43d01dd.md` and this file.
  The first two had not been added when those ports landed, so a 0.7.0.3
  tarball would otherwise have shipped without the records for the two fixes
  the release is named after.

**Every Linux claim the notes make was rechecked against the real ELF**
(`~/.local/share/tpf2mp-lab/native/game/TransportFever2`, build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, Steam build 35924, read only). A
release must not assert something the port does not do:

| Release-note claim (Linux) | Check | Result |
| --- | --- | --- |
| "sorted only 1 of the 28 entity lists ... now sorts all 28" | `verify_order_canon_elf.py` | PASS: "family getters (28 node-list + 35 no-list, the complete vtable inventory)", `NodeList<1..5>` RTTI/vtables, Step iteration call, all seven byte guards |
| "draws the same random numbers as Windows at 15 more places" | `verify_random_parity_elf.py` | PASS: boundaries and no live direct branches into replaced interiors at `0x16ed220`, `0x16ed5b0`, `0x154a480`, `0x182a6f0`, `0x2ecc8b0`; AirConnectParts old interior branch unreachable after rewrite |
| "uses Windows' float math functions in the simulation" | `verify_libm_parity_elf.py` | PASS: GOT `0x5a472b8 acosf`, `0x5a472b0 atan2f`, three guarded street double-atan2 sites (`0x1578b3b`, `0x1579031`, `0x157a4ac`), double `atan2` PLT and eager binding |
| "`TPF2MP_TOWN_TRACE=1` on Linux" | `verify_town_trace_elf.py` | PASS: `kGuard` 70 bytes at `0x17478d8`, `kTownContextBytes` 339 bytes at `0x1746790`, 2504-byte MT, seven SysV arguments, Develop target |
| "Octree depths 12 and 13 work on the native Linux build" | Big Maps `verify_game.py`, `test_octree_depth_elf.py` | PASS: build-id and all 29 Linux patch sites; 1620 depth-13 and 1462 depth-12 original-code nodes, stock overflow reproduced, depth-10/11 IDs identical to stock |
| "Terrain compression is on by default on native Linux" | `bigmap/linux/tpf2_bigmap.cfg` (the file `build_release.sh:178` stages) | `terrain_cache_compress=1` |
| "reading `/proc/self/maps`" no longer costs the sim thread | `native/linux/src/family_canon_linux.inl` | `kFamilyMapQuery` (`_IOWR('f', 17, ...)`, i.e. `PROCMAP_QUERY`, spelled locally for the soldier SDK) is probed once per thread and used per range; the buffered `/proc/self/maps` snapshot is only the pre-6.11 fallback (dev `7cacbaaf`) |
| bulldoze burst / stop replay (the two fixes the release is named after) | `tools/edge_demolition_test.py`, `tools/stop_replay_known_test.py` | PASS on Linux, see Tests |

The remaining ELF verifiers were run for regression and also pass:
`verify_capture_elf.py` (149 probes), `verify_dev_2b466e3_elf.py`,
`verify_dev_38432b5f_elf.py`, `verify_dev_d3135a59_elf.py`,
`verify_movement_elf.py`, `verify_preview_elf.py`, `verify_speedhook_elf.py`,
`verify_train_order_elf.py`.

## Not ported

Nothing from `d8a3ce57`. It has no platform-specific part.

Earlier, unrelated port limitations are unchanged and this release does not
close them -- notably the placement-distance saturation and attempt budget from
`12407af` (see [DEV_BD69B864](../re/linux/DEV_BD69B864.md)), and the terrain
pager thrash the notes themselves list under "Known limitations".

### Pre-existing defect found while checking the release, deliberately not changed

`tools/linux/verify_company_tint_elf.py` fails on this tree, and on the
pre-merge parent, and on `origin/linux-native`. Its section 6 tripwire
("the source still says what this file verifies") asserts

    'SliceEcsIsCompany(c.rsi, entity)' in native/linux/src/slice/slice_lines.cpp

That gate was added by `7f55411` and is gone from the tree as of `efa583e`
(2026-09-21). The `kName` factory case at
`native/linux/src/slice/slice_lines.cpp:516-523` now cancels and ships
`VNAME <entity> <name> replayOrigin=1` for any `entity >= 0`, with no Player
component check; `SliceEcsIsCompany` survives only in
`native/linux/tests/company_tint_test.cpp:323-325`. So the tripwire fired
correctly on a real source change: the company-rename gate was dropped, and
the ELF half of that verifier has not run since.

It is left alone on purpose. Re-pointing the assertion at the current code
would silently endorse the gate removal, and whether a non-company rename may
now ship as `VNAME` is a question about live behaviour that this host cannot
answer (see Live testing). It is not part of the harness verification
(`build_native.sh`, `verify_lua_release.py`) and it did not regress here. It
wants an owner decision, not a quiet edit on a release commit.

## Live testing

No game was run. The lab cannot start on this host, for the same two
independent reasons the `0610033` and `e43d01dd` records give, neither of which
is mine to change. Both were rechecked, not assumed:

1. **Steam is not running.** The machine had been up 22 minutes with no Steam
   process (`pgrep -a -x steam` empty). `~/snap/steam/common/.steam/steam.pid`
   still holds pid 6573 from 2026-09-17 and `ps -p 6573` is empty. Transport
   Fever 2 needs the Steam client and starting Steam is out of remit.
2. **AppArmor blocks the Steam runtime's container helper.**
   `tools/sandbox/tpf2mp-lab check --root ~/.local/share/tpf2mp-lab` and
   `run native` both fail at the outer layer with
   `bwrap: setting up uid map: Permission denied`.
   `kernel.apparmor_restrict_unprivileged_userns = 1`, and the kernel audit
   taken during the attempt shows the transition and the refusal:

       apparmor="AUDIT" operation="userns_create" info="Userns create -
         transitioning profile" profile="unconfined" comm="srt-bwrap"
         target="unprivileged_userns" execpath=".../srt-bwrap"
       apparmor="DENIED" operation="capable" profile="unprivileged_userns"
         comm="srt-bwrap" capability=8 capname="setpcap"
       apparmor="DENIED" operation="open" info="Failed name lookup -
         disconnected path" profile="unprivileged_userns"
         name="proc/19389/uid_map" comm="srt-bwrap"

No game PID, GPU, title menu, loaded world, gdb hit, XTEST sequence or visual
result was reached, and none is claimed. Because nothing could be launched, the
native actor was **not** modified: no library, mod or save under
`~/.local/share/tpf2mp-lab/native` was written, so there was nothing to back up
or restore. Steam, the user's saves, the user's installed mod
(`~/snap/steam/common/.local/share/tpf2mp`) and Steam's own game directory were
not touched, and no game remains running. Transcript and audit lines:
job `meta/live/lab-attempt.log`.

Note on the actor directory: `~/.local/share/tpf2mp-lab/native/` holds
`share/tpf2mp.before-port-*` and `game/mods/mp_lockstep_1.before-port-*`
directories from **earlier jobs** (`011578`, `0610033`, `2c05099a`, `7cacbaaf`,
`bd69b864`, `12407af`, ...). None is named for this target and none was created
or removed by this run; they are outside my remit to clean up. The live
`share/tpf2mp/` and `game/mods/mp_lockstep_1/` are the actor's own, unmodified.

Consequently the release's own cross-platform claims -- town growth in step on
a native Linux server, depth-13 rendering and load/save, the bulldoze freeze
being gone in play -- rest on the upstream Windows measurements quoted in the
notes and on the static and offline evidence above, not on a local run.

## Tests

- `tools/linux/build_native.sh`: soldier build, **67/67 CTests passed**,
  glibc baseline pass.
- `python3 tools/linux/verify_lua_release.py` against the new `d8a3ce57`
  reference: **PASS**, 29 Lua files (one pinned native exception) and 32 HUD
  glyph textures exact. Manifest SHA-256 `d23c25e3...`.
- `tools/stop_replay_known_test.py`: PASS -- "replayed adds and removals are
  known to the catch-up scan; native objects still ship; failed replays
  register nothing".
- `tools/edge_demolition_test.py`: PASS -- "... one node index per bulldoze
  burst (rebuilt after a removal and on a new tick), cell index == full scan".
- `tools/version_gate_test.py`: 2 tests OK -- the direct coverage of the
  `LOBBY_VERSION` bump (matched version accepted, every other value rejected).
- `tools/lobby_mode_test.py`: ALL PASS. `tools/test_tcp_connectivity.py`:
  9 OK, including real loopback IPv4/IPv6 transfer. `tools/test_steam_tcp.py`:
  PASS (real loopback TCP plus simulated Steam fallback; no real Steam peer or
  router was contacted). `tools/test_transfer_status.py`: 3 OK.
- **12 of the 13** `tools/linux/verify_*_elf.py` checks pass against the
  unmodified game ELF, as tabulated above. The thirteenth,
  `verify_company_tint_elf.py`, fails identically on the pre-merge parent and on
  `origin/linux-native`; see "Not ported". Full transcript:
  job `meta/live/offline-elf-verify.log` (14 rc=0, 1 rc=1).
- Big Maps `verify_game.py`: build-id and 29 sites PASS.
  `test_octree_depth_elf.py`: original-code emulation PASS.
- `tools/test_lobby_limits.py`: 31 of 32 pass;
  `test_registry_has_no_count_cap` fails with `1 != 301`. Pre-existing and
  unrelated to the version bump: the identical failure was reproduced on a
  clean `git archive` of the pre-merge parent `9cd3666`.
- Python dependencies (`lupa`, `pystun3`, `unicorn`) were installed in the
  clone-local `.git/port-venv`; nothing was installed system-wide.

These are off-game results. They are not cross-platform determinism evidence.
