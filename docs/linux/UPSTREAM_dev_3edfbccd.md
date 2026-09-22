# Upstream dev 3edfbccd integration: train reservation order

Windows commit: `3edfbccd52c0498124a55db57c0714796f3600a1`.
Linux merge parent: `19bc87f`, following the dev `55e97a48` integration.
This batch contains only the requested one commit; the remaining 38 pending
commits are outside its scope. Merge remains staged and uncommitted.
`installer/VERSION` is unchanged. Nothing is installed or published.

## Merged

The Windows build, hook, MASM relay, shared ordering header, Lua train-name
hash lane and diagnostics, and three upstream tests are retained. There were
no merge conflicts. Both platform paths remain available.

This incoming commit branches from 0.5.6 (`e4702669`), not `55e97a48`.
The cumulative Lua verifier therefore checks 25 files exactly against
`55e97a48`, net.lua exactly against `3edfbccd`, and pins the two reviewed
combined files: the pre-existing dashboard-tabs lockstep.lua and hash.lua
with both the older vehicle-key diagnostics and new train-name lane.
[The hash.lua difference from incoming](UPSTREAM_dev_3edfbccd_hash.patch)
records the retained earlier work. The existing tabs patch remains applicable.
The 28-file Lua manifest SHA-256 is
`8c4bc9c89fa4f58070ae2ad2f69c4bb8edb9a469cec92c17ba05c22793a7b8ed`.
Release BUILDINFO and packaged integration records describe that provenance.

## Ported

Native Linux replaces the verified Update2 shuffle CALL at `0x175849d` with
a SysV relay, passing the engine's array, self and world to the shared
name/id rank + seeded jitter algorithm. Linux-specific ECS and libstdc++
layout readers supply names safely; the raw GameTime seed uses Windows's
uint32 normalization even when its sign bit is set. Memory is copied before
sorting, and refusal invokes the unchanged original Linux shuffle.

The hook is installed before gameplay, including solo mode, as on Windows.
All sites are byte-checked behind the existing GNU build-id gate. Failure
keeps multiplayer readiness false. `trainorder=0` in the root/data
`tpf2_menu_flags.txt` disables the patch explicitly, with a log message.
Step sampling, name counts, id hashes, timing and periodic counters are
available in the slice log. Detailed addresses, bytes and ABI evidence:
[TRAIN_ORDER.md](../re/linux/TRAIN_ORDER.md).

## Not ported

None from this commit. Existing recovery, ownership, Workshop and other
native Linux gaps remain as recorded in earlier integrations. The upstream
name/id tie-break assumptions are retained. No live cross-platform gameplay,
performance or starvation-freedom claim is made by the offline tests.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 40 CTests and glibc <=2.31
  compatibility checks. Includes the upstream train ordering suite and the
  Linux layout/guard/relay/fallback fixture.
- `python3 tools/linux/verify_lua_release.py`: pinned cumulative 28-file manifest.
- `python3 tools/linux/verify_train_order_elf.py GAME`: actual supplied ELF,
  GNU build-id, 16 runtime signatures, CALL targets/boundaries and Name RTTI.
- Lua 5.2 using a clone-local Lupa environment: `train_name_lane_test.py`,
  `vpos_key_test.py` and `dash_tabs_test.py` all pass.
- Release shell syntax and staged whitespace check (upstream CRLF respected).

Steam and Transport Fever 2 were never started. The Windows-only PE byte test
was not run: Linux sites were verified against the supplied ELF instead.
