# Windows dev c8dd5157: threaded terrain-sidecar encoding

## Merged

Windows target: `c8dd515770d41a78d2422b1477c0dc7dd1cbcf5c`.
Linux parent: `b5a86236b1ea3c1e6b4605bdec754a8a21b37bf8`.
One Windows commit, no conflicts. Merge remains staged and uncommitted.
Retained Windows thread configuration, plugin initialization, worker encoding,
ordered file output and per-tile SEH guard. Release remains **0.7.1.1**.
Windows runtime code is unchanged from the incoming commit; the shared header
has only a corrected Linux-support comment beyond upstream.

## Ported (what and how)

The upstream POSIX file/directory/rwlock implementation now has native build
and regression coverage. CMake builds the shared sidecar test with pthreads
and Release assertions. The test uses a Linux-only fopen adapter while
retaining Windows CRT calls. Corrected its synthetic allocation from 0x20 to
0x28 bytes to hold the 0x10 prefix and three vector pointers without overflow.
Expanded 3,000 records to 4,500 so the comparison truly crosses two complete
2,048-record windows and a partial third. At 2, 4, 7 and automatic threads,
all output bytes equal the single-thread output (1,500 eligible tiles).
Added thread-limit, UTF-8 filename, case-insensitive save extension,
matching-fingerprint directory lookup, capacity rejection and refingerprint
checks. Existing restore, corruption and stale-save tests also run on Linux.

Documented the native runtime boundary in README, INSTALL, Big Maps port notes
and native configuration comments. Packaged this integration record in the
release script. No unavailable native enable switch or unverified hook was
added; Lua and native runtime behavior remain unchanged.

## Not ported

Native in-game threaded terrain capture and `terrain_sidecar_threads` remain
unported because the existing native port has no sidecar capture/serve hooks.
Fresh static analysis confirmed the native grid and SaveGame candidates;
a real lab launch failed before startup. It could not prove saved-world
terrain ownership, a freeze covering worker joins, vector lifetime, safe
worker/pager reads or native capture/hash/restore lifecycle. The Linux codec
branch does not have Windows' SEH fault recovery. See
[static evidence, live attempt and missing contracts](../re/linux/DEV_C8DD5157.md).
Linux continues its existing alignment and save paths. The upstream 12.2-second
cost and any threaded improvement are not local native measurements.

## Live testing

Installed this soldier build and merged Lua into the native actor after
`cp -a` backups of both payload directories. Set `autoload=1` and used
`newgame_density=0`. Ran `tools/sandbox/tpf2mp-lab run native --root
~/.local/share/tpf2mp-lab` with a 170-second bound. It exited 1 immediately:
`bwrap: setting up uid map: Permission denied`.
No game process, menu, Vulkan device, loaded world, save, gdb probe or threaded
terrain behavior was observed. Both payloads were restored in a finally block
and their file SHA-256s and symlink targets verified. No save changed, no game
remains running, no desktop input was used and Steam was untouched.
Evidence is archived in this job's `meta/live/`; actor logs/data may predate
the attempted run and are not evidence of candidate gameplay.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, **76/76 CTests**
  (62.04 seconds), glibc <=2.31 checks. New sidecar test: 3.06 seconds.
- Host GCC build of the shared sidecar test with AddressSanitizer and
  UndefinedBehaviorSanitizer: PASS, exit 0, no sanitizer findings.
- `python3 tools/linux/verify_lua_release.py`: PASS, **31 exact Lua files**,
  zero exceptions, **32 HUD glyphs** and **2 toolbar textures**.
  Manifest SHA-256: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id and
  **30/30 guarded sites**.
- Release script shell syntax, staged whitespace (CRLF-aware), empty unmerged
  index, and retained MERGE_HEAD checked. No package archive or Windows DLL
  build was run; synthetic Linux tests do not prove cross-platform gameplay.

Final status: **PARTIAL**. No commit, abort or publication.
