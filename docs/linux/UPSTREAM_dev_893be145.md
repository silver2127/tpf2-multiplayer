# Windows dev 893be145 integration: dedicated Workshop registry

## Merged

Windows target: `893be1450f25dc9019c1749b3632c6891350f346`.
Linux parent: `70e72f38304f8c594d82766bfd94eb3f1843cb0a`.
Three incoming commits, no conflicts; merge staged and left uncommitted.
Release remains **0.7.1**.

- `bc69efe`: stable-release Discord workflow and shared Python webhook tool.
- `894ad68`: upstream merge of Linux integrations through `d3f199f9`.
  Its diff against its Linux second parent contains only the two webhook files;
  the native changes are already present in this branch.
- `893be145`: dedicated lobby startup publishes an unscoped Workshop registry,
  preserving valid existing rows and managed downloads, plus its regression test.

## Ported (what and how)

Retained all four incoming files unchanged. The shared Python lobby runs on
both platforms. Native `lobby_linux.cpp` sets `request.dedicated = true` in its
dedicated controller and emits `--dedicated` in `Start()`. Thus `cmd_host()`
reaches the new `dedicated=True` registry publication without native changes.
Ordinary hosts and joiners retain the empty initial scope.

`modshare.write_registry()` uses Linux's resolved data directory, preserves the
receipt token, retains existing rows only while their folder contains `mod.lua`,
and adds numeric directories under `data/workshop/`. Unscoped publication now
keeps those folders available before a dedicated save's mod list arrives.
The existing native `workshop_register_linux.cpp` reads the same
`mods_registry.txt` token/tab-separated path format. Its build/byte checks and
catalogue hook are unchanged. No address, pattern, ABI, struct offset or
lifetime contract changes; no new static or live RE is needed.

The webhook workflow already runs on Ubuntu and uses Python's standard library.
It was tested offline; no release was published or webhook sent.
Advanced the exact Lua verifier baseline and package BUILDINFO provenance,
packaged this record, and updated README/INSTALL links. No Lua or native runtime
code changed. The incoming dedicated-registry test provides regression coverage
for both startup modes on Linux; no duplicate native test was added.

## Not ported

None introduced by these commits. Previously documented native Sandbox town-tool,
minimap, cargo-filter and other gaps and live-validation limits remain unchanged.
This integration does not establish cross-platform gameplay parity.

## Live testing

No game launched, debugger attached or desktop input sent. This change is to
shared registry publication and CI, with no new engine contract to probe.
The dedicated startup behavior was exercised with real temporary files in the
clone, not in a running game. No loaded-world acceptance or rendering result
is claimed. Lab actors, saves, Steam and the installed mod were untouched;
no backup/restoration was necessary and no game process was started.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 75/75 CTests and
  glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact upstream Lua
  files, no exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `4f1370ac2781c929d3526bdef7b39d17ddf945af5f25160a8707c7e7528f6e3a`.
- `tools/registry_dedicated_test.py`: PASS using the existing netpunch-build
  Python environment. Valid managed and previously registered external folders
  retained; missing folder removed; token preserved; ordinary host scope empty.
- `netpunch/modshare.py --selftest`: PASS in the same Python environment.
- Offline webhook assertions and CLI `--dry-run`: PASS for stable/draft/
  prerelease/tag filtering, release notes, mention suppression and embed limits.
- `bash -n tools/linux/build_release.sh`: PASS.
- Incoming files byte-identical to upstream; unresolved index empty; staged
  whitespace check: PASS.

Logs: clone-local `.git/port-893be145/{build,lua,registry,modshare,webhook}.log`.
System Python lacked `stun` and `pip`; the pre-existing
`~/.cache/tpf2mp/netpunch-build/venv/bin/python` supplied lobby dependencies.
No dependency installation or external environment modification was needed.
