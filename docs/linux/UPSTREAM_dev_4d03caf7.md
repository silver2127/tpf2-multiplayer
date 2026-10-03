# Windows dev 4d03caf7 integration: Sandbox research

## Merged

Target `4d03caf7b22ca75411944c24ae68f9e2d366268d`, Linux parent
`5487ec42552ada4760ab290af8bb2c44a1e43b1b`. One incoming commit:
`re: Sandbox mode -- its tools, the TownInfo type, and town creation measured deterministic`.
No conflicts. Merge staged and uncommitted; version remains 0.7.0.7.

Retain `docs/re/SANDBOX.md` and its `docs/re/README.md` index entry exactly
as upstream. They describe Sandbox tools, the script-facing TownInfo fields,
three same-state town-creation measurements, cargo needs chosen by the tool,
and questions still open before implementing command capture.

## Ported (what and how)

Shared research documentation needs no native implementation. This commit
changes no code, hook, patch bytes, calling convention, struct layout or
lifetime contract. No new static or live reverse engineering is required.
The addresses in the incoming document are Windows RVAs, under the conventions
in `docs/re/README.md`; none are adopted as Linux addresses.

The same-state measurements are upstream Windows evidence, not local Linux
observations or proof of cross-platform determinism. The recommendation to
capture the complete TownInfo is future implementation guidance, not a new
replication feature. Existing Linux evidence retains its separate conventions
in `docs/re/linux/SLICE_CORE.md`.

Add this integration record and a README link. Keep the existing `52a1630a`
Lua verifier baseline and release provenance: the incoming commit changes no
Lua or assets, and its mod tree matches the current tree. Windows paths remain
intact. No new tests are needed for documentation-only changes.

## Not ported

None from this commit. Sandbox town replication and the open payload/name
questions are upstream research gaps, not an incoming implementation left
unported. Existing Linux feature and gameplay-validation limits remain.

## Live testing

No game launched, debugger attached, desktop input sent or lab payload
installed. No local town creation, rendering or cross-platform result is
claimed. Steam, actors, installed mods and saves were untouched; no backup
or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 73/73
  CTests (52.91 seconds) and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions and 32 exact HUD glyph textures. Manifest SHA-256:
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- Incoming research documents byte-identical to the target: PASS.
- Upstream mod tree unchanged from the verifier baseline: PASS.
- Working/staged whitespace checks, empty unmerged index and retained
  MERGE_HEAD: PASS.

Logs: `.git/port-4d03caf7-build.log` and `.git/port-4d03caf7-lua.log`.
