# Upstream dev b4465474 integration

Windows target: `b44654745a2e804841af483211399243608ee93d`.
Linux parent: `937d647692ec596edb3d25d9aa0f7bf047480446`, following
[0115785c](UPSTREAM_dev_0115785c.md). One Windows commit, no conflicts.
Merge remains staged and uncommitted.

## Merged

The shared `native/src/family_canon.h` is byte-exact upstream. It sorts the
region beginning at the first displaced node, finds the affected prefix
boundary, merges and copies only the affected tail, and updates only index
positions pointing into that tail. All occupied index slots are still counted
and range-checked; key-to-node equality is checked only for tail positions.
Duplicate keys are rejected before writes. Unchanged prefix key-to-node
equality is no longer validated, matching upstream's optimization.

## Ported

Linux already includes this header directly in
`native/linux/src/family_canon_linux.inl`; its existing guarded family walk
therefore receives the optimization without a duplicate implementation.
No new executable site, address, ABI, struct offset, engine call or lifetime
contract is introduced. Existing writable-range checks and fail-closed hook
installation are retained. The shared Windows algorithm is unchanged.

Regression tests cover all five component widths, a two-node tail, a low key
merging into the ascending prefix, signed keys, sparse extraction and whole
region sorting, and scratch reuse. They check complete component payloads,
moved counts, unchanged prefix contents/index slots and control bytes, index
consistency, repeat-sort idempotence, duplicate refusal across the prefix
boundary and invalid positions/tail keys without writes.

README/install links, Lua verification baseline and release provenance now
name this commit; release packaging includes this integration record.

## Not ported

None in this commit. The inherited default-off canonical ordering and missing
loaded-world lifetime/concurrency/performance evidence remain exactly as
documented in [the preceding integration](UPSTREAM_dev_0115785c.md) and
[its RE record](../re/linux/DEV_0115785C.md). This shared optimization does not
enable those hooks or establish Windows/native gameplay parity.

## Live testing

No game launched, gdb attached, actor files installed or saves modified.
This change is shared in-memory sorting with no new platform contract;
offline algorithm and native shim tests exercise it. No live performance or
gameplay result is claimed. Lab copies and Steam were left untouched.

## Tests

- `tools/linux/build_native.sh`: soldier build, **58/58 CTests**
  (including shared family sorting and native order-canon fixtures) and glibc
  <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files with one pinned
  Linux origin-replay integration and 32 exact HUD glyphs passed. Manifest:
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.
- `python3 tools/linux/verify_order_canon_elf.py <lab ELF>`: build-id, all
  seven existing site guards, instruction boundaries, no interior branches,
  family getters, NodeList<1..5> RTTI/vtables and Step iteration call passed.
- Build/test logs: `.git/port-b446547-build.log`,
  `.git/port-b446547-lua.log`, `.git/port-b446547-elf.log`.
- Release-script shell syntax, whitespace checks, upstream header equality and
  empty unmerged index passed.
