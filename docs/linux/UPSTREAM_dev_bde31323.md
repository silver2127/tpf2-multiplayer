# Windows dev bde31323 integration: companies rewrite

## Merged

Windows target: `bde31323afc6bd4f1233eda02f363c1dcd1a0376`.
Linux parent: `86fc476` (integration of `ac3b4be3`).
One incoming first-parent merge, incorporating PR #15's 13 topic commits;
`git log -p HEAD..bde31323` includes those topic commits too. No conflicts.
Merge is staged, in progress and uncommitted; no later pending commit is taken.

Retain the replicated company registry, stable player identities, join/leave
commands, command attribution at execution stamp, save migration, starting
loans, asset/wallet switches, ownership/HQ rules, deletion and takeover,
company wallet diagnostics, free colors and rebuilt COMPANIES tab. Shared Lua,
Windows code and Big Maps Lua remain the incoming implementation.
Release remains **0.7.1.1**; `RELEASE-0.7.0.8.md` is historical experimental
branch documentation and does not downgrade package or handshake versions.

## Ported (what and how)

The merge supplies native Linux pid/class/RGB parsing, exact RGB vehicle and
station-label drawing, and `me`-based foreign window tinting. Compiled and
validated these against the existing native wrappers and actual game ELF.
Legacy file formats still work. Linux's structured roster JSON parser already
provides the bounded object lookup fixed on Windows; added a real-handler
regression for empty objects followed by letters. No additional runtime
adaptation, new hook, address, byte pattern, ABI or offset is needed.
See [native evidence](../re/linux/DEV_BDE31323.md).

Extended native tint tests and palette consistency coverage. Repaired a stale
source assertion and added existing draw-site coverage to the ELF verifier.
Advanced the exact Lua baseline and package BUILDINFO provenance to this merge,
packaged this record, and updated README and Linux INSTALL.

## Not ported

None introduced by this integration. The shared Big Maps minimap color reader
is retained, but native minimap rendering remains an inherited limitation;
this merge does not supply its missing native renderer. Inherited Sandbox
town-tool capture, cargo-filter, terrain-sidecar and other documented native
limitations remain. No cross-platform gameplay parity is inferred from tests.

## Live testing

The native actor's libraries/data and Lua mod were backed up with `cp -a`
to `.before-port-bde31323` siblings (the pre-existing `.before-port` directories
were left untouched). Installed this soldier build and merged Lua, then ran:

`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`

The launch exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
The prior lab `check` failed for the same reason. No game process started,
no Vulkan device was selected, and no title menu, companies UI, live registry,
rendered color or native/Proton session was observed. No gdb or desktop input
was used. Steam, user saves and installed user mod were not changed.

Restored both actor directories in a finally block, verifying file SHA-256s
and symlink targets against their pre-test manifests. No save was loaded or
modified and no game was left running. Launch output, actor logs/data and
restoration evidence are copied to this job's `meta/live/`; historical actor
logs/data are not presented as observations of this build.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier SDK, 75/75 CTests in 59.03 s,
  including extended company tint and roster tests, glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyph textures and two toolbar textures.
  Manifest: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `company_registry_test.py`, `company_rules_test.py`, `company_gui_test.py`,
  `company_color_test.py`, `company_loading_gate_test.py`: PASS, 228 checks,
  using existing lobby venv's Lua 5.2 harness; temporary files in the clone.
- `palette_sync_test.py`: PASS, extended to all five consumers including
  native Linux, 20 matching fixed colors and overflow at 21.
- `verify_company_tint_elf.py` against the lab's actual Linux ELF: PASS,
  including the added 24 draw spans and five draw call targets.
- `bash -n tools/linux/build_release.sh`: PASS. Package itself not built.
- Merge target and empty unresolved index verified; whitespace checks use
  `core.whitespace=cr-at-eol` to respect preserved upstream CRLF files.

Build/test and static evidence logs: clone-local `.git/port-bde31323/`.
No system package, dependency or analysis tool was installed; no publication.
