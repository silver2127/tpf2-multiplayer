# Windows dev a896a1cb integration — native plugin short hooks

## Merged

Windows target: `a896a1cbca4e7884a475de71a8986d3ce508ba78`.
Linux parent: `41204f4795870d302524ce5e0d264ee64e5e5e0d`.
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release remains 0.7.1.2; shared Lua/lobby and Windows runtime are unchanged.

## Ported (what and how)

Retained upstream's Linux host fix: `installHook` accepts 5..32 stolen bytes.
Lengths 5..13 reach the existing nearby trampoline/jump-stub writer, removing
the API rejection of the terrain sidecar's 13-byte AddTile hook. The shared
header documents the Linux exception while preserving the Windows >=14 rule.
Existing byte/build checks, instruction-boundary checks and hook-failure
forwarding remain unchanged. No address, struct offset or ABI was invented.

Added a host API regression using the actual writer, covering execution and
original calls for every length 5..32, a RIP-relative continuation, invalid
lengths/nulls/non-executable memory and cuts inside the verified AddTile
prologue. A test-only constructor guard prevents plugin discovery or runtime
data writes; production constructor behavior is unchanged. Existing writer
and terrain tests remain in place.

Updated README/INSTALL/plugin-host guidance, packaged integration record,
BUILDINFO and Lua verification baseline. Fresh ELF disassembly confirms the
13-byte boundary; the fourteenth byte begins a RIP-relative LEA. See
[RE evidence](../re/linux/DEV_A896A1CB.md), which corrects the earlier record's
claim that the host already supported this short hook.

## Not ported

None from this commit. Inherited native feature gaps and terrain ownership,
worker/pager synchronization, save-world identity and load-completion live
validation remain outstanding. Upstream's successful dedicated-server report
was not reproduced locally and is not new local proof of those contracts.

## Live testing

Backed up both native actor payload directories with `cp -a` to unique
`.before-port-a896a1cb` siblings. Installed candidate soldier libraries,
plugins/config and merged Lua, then ran:

```
tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab
```

It exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game process existed for gdb; no title menu, renderer device, loaded save,
hook installation or terrain behavior was observed. No Proton peer or XTEST
input was used. Steam and user installation were untouched. Both payloads
were restored; file hashes, modes and symlink targets match their backups.
Saves were untouched and no lab game remains running.

Job `meta/live/` contains the launch transcript/result, ELF disassembly/site
checks, archived actor logs/data and restoration results. Archived actor
logs/data include historical content, not candidate execution evidence.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK; 79/79 CTests
  in 62.63 seconds, including new `plugin_host_hooks`; glibc <=2.31 checks pass.
- `python3 tools/linux/verify_lua_release.py`: PASS against a896a1cb;
  31 exact Lua files, zero exceptions, 32 HUD glyphs, two toolbar textures.
  Manifest SHA-256: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, GNU build-id
  `3a0e156390b0e6f1e372051c24802c8493ae454a` and all 39 sites.
- `bash -n tools/linux/build_release.sh`, Git whitespace and merge-state
  checks pass. Windows/MSVC compilation and release packaging were not run.

Logs: `.git/port-a896a1cb-{build,lua,elf}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No commit, merge abort, push or publication was made.
