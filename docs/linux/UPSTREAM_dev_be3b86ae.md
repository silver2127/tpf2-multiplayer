# Upstream dev be3b86ae integration: company loading gate

Windows target: `be3b86ae11ec67f7e337e11093875c83ee3b04a0`.
Linux merge parent: `edce5311d2fe57d84b47372b5306cfdd3af05a7f`, following
[61578d27](UPSTREAM_dev_61578d27.md). One Windows commit, no conflicts.
The merge remains staged and uncommitted. This commit is fully ported;
existing native coverage gaps and runtime validation limits still apply.

## Merged

Shared companies.lua reads mp_loading.txt and names other loading players.
The inject branch refuses CMNEW, CMSWITCH and CMDEL before scheduling while
any other player is loading; CMPW remains available. Dashboard buttons and
status show the waiting note. Windows native code and the upstream Lua test
remain intact. All 28 Lua files exactly match the target, without exceptions.
Manifest SHA-256:
`ae8920e2e1994a3540913e9a2ad3b22b1f7b04752bbcd33b6e383efea9199a8d`.

## Ported

Linux WritePlayerNames now exports mp_loading.txt from the same locked roster
snapshot as mp_players.txt, using stable origin letters and nonempty stages.
Every roster refresh replaces the file atomically, including empty contents
when players finish or depart. File-write errors are logged. The shared Lua
reader ignores the local player's entry and enforces the gate before shipping
commands. No new game hook, patch site, ABI or offset is involved; source
mapping and decisions are in [DEV_BE3B86AE.md](../re/linux/DEV_BE3B86AE.md).

The Lua verifier, release BUILDINFO, packaged integration records and Linux
coverage documentation now identify this target.

## Not ported

None from this commit. Earlier native tinting, ownership, recovery/in-world
reload, company rename capture and Workshop limitations remain documented
in previous integrations. This change does not claim to close them.

## Tests

- `tools/linux/build_native.sh`: soldier build, 45/45 CTests and glibc <=2.31
  checks passed. Expanded lobby_ready tests cover roster-to-file stages,
  relay identities, completion, departure and missing stages.
- `python3 tools/linux/verify_lua_release.py`: all 28 Lua files exact; manifest above.
- `.git/test-venv/bin/python tools/company_loading_gate_test.py`: all checks
  passed using Lupa Lua 5.2 (reader, refusal, password exception and resumption).
- `bash -n tools/linux/build_release.sh` and whitespace checks passed.

Logs are in `.git/port-be3b86ae-*.log`; the Python environment and package
cache are inside this clone. No game or Steam was launched, and no live
cross-platform test is claimed. No installation, publication, commit or
merge abort occurred.
