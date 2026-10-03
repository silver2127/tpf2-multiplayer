# Windows dev ba1fa26e integration: require the loaded bridge

## Merged

Windows target: `ba1fa26e5f5537f0ac266b7bc06f94c952216418`.
Linux parent: `e3af2152e6c34066e9879f3daac1476e0c478dad`.
One incoming Windows commit, no conflicts. Preserve upstream's Windows
module check, amber panel warning and offline regression tests unchanged.
Retain the existing Linux-branch company-config changes in Windows menu_hook.cpp.
Merge staged, not committed; release remains 0.7.0.5.

## Ported (what and how)

Native `lobby::BridgeProblem` inspects glibc's loaded-object list with
`dl_iterate_phdr`, requiring the exact basename `tpf2_bridge_mp.so`.
This includes `RTLD_LOCAL` libraries and does not load or initialize one.
`boot.cpp` loads the bridge before the menu with `RTLD_NOW | RTLD_LOCAL`;
its handles remain held. No game addresses, patches, offsets, calling
conventions or object lifetimes change. Existing engine evidence in
[MENU_GAME](../re/linux/MENU_GAME.md) remains applicable; this change needs
no ELF disassembly or game ABI probe.

Host and Join setup actions, including Host Session in an existing world,
have no hit target without the bridge and show an amber explanation.
The central `Start` refuses before validating input, replacing a model or
queuing a launch, covering direct, clipboard/public-browser and dedicated
requests. The existing cancellation-readiness check also requires the bridge,
so worker launch and save sharing recheck it. Existing slice readiness checks
remain active after bridge presence succeeds.

Decision: use one accurate Linux “not loaded” message for both missing-file
and failed-load cases. The loader already distinguishes them and records the
attempted path and `dlopen` error in `data/tpf2_proxy.log`; the panel directs
the player there and recommends reinstalling and restarting. Log the refusal
once per process. Do not infer library health from an old instance file or
file existence. As on Windows, module presence does not prove networking
health or successful bridge initialization.

Tests load an actual constructor-free fixture library under the expected
basename, using `RTLD_LOCAL`; there is no production bypass flag. The panel's
isolated test stubs the lobby API. Advance Lua-verifier/package provenance,
package this record, and update README, INSTALL and menu documentation.

## Not ported

None for this commit. Existing unrelated Linux limitations remain unchanged.

## Live testing

No game launched, gdb attached, desktop input sent or lab payload installed.
No actor, save, Steam directory or installed user mod was changed; restoration
was unnecessary. No in-game visual, rendering-device or multiplayer gameplay
result is claimed. The native dynamic-loader regression runs off-game and
proves loaded-object detection including local visibility and unload, not
successful initialization of the real bridge in Transport Fever 2.

## Tests

- `tools/linux/build_native.sh`: soldier build, 68/68 CTests and glibc <=2.31
  compatibility checks passed, including the final updated panel tests.
- `lobby_ready`: present-but-unloaded library refuses host/join and dedicated
  requests with the bridge reason and leaves model generation, active state
  and queue unchanged; `RTLD_LOCAL` load passes presence, unload fails it,
  reload passes. Existing slice readiness and lobby tests still pass.
- `panel_title`: no Host/Join targets without the bridge on title and in-world
  setup; direct host/join attempts preserve setup and expose the reason;
  Join returns after bridge availability. Existing layouts and input tests pass.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files, one pinned native
  origin-replay exception, 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Release script shell syntax, whitespace with CRLF awareness, upstream Windows
  panel/test equality and unchanged pre-existing company-config differences,
  empty unmerged index and retained MERGE_HEAD checked.
  No Windows executable, Proton test or release package was run.

Logs in this clone: `.git/port-ba1fa26-build.log`,
`.git/port-ba1fa26-build-final.log`, `.git/port-ba1fa26-lua.log`.
