# dev 568e7ea7: generator timestamps and shared save compression

Target Linux Steam 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`.

## Generator routing

Rechecked the actual ELF with `objdump` and the 30-site Big Maps verifier.
The existing `fopen@plt` slot at `0x6dd160` remains:

```
ff 25 ca a5 36 05 68 94 01 00 00 e9 a0 e6 ff ff
```

The RIP-relative jump names `fopen@GLIBC_2.2.5` at GOT `0x5a47730`.
The existing full-slot byte guard and build-id gate remain unchanged.
SysV arguments are filename in RDI, mode in RSI, returned FILE* in RAX.
[The original routing analysis](DEV_6584FD03.md) records the Lua loader,
filename wrapper, reader and fclose ownership. No new game address, object
layout, calling convention or lifetime assumption is introduced.

Windows 568e7ea copies creation/write times onto a named patched file and
excludes attribute-only CreateFileW opens. Native routing is already limited
to read-only `fopen` modes; stat/open metadata queries are not redirected.
Its per-open anonymous tmpfile nevertheless had a fresh mtime, disagreeing
with the source pathname. Capture `fstat(fileno(source))` before reading,
then `futimens(fileno(copy))` after flushing all patched bytes and before
returning the rewound FILE*. Both calls use libc's compiled struct stat and
timespec definitions, not guessed ELF offsets. Access and modification times
retain nanosecond precision; POSIX has no settable creation time and ctime is
metadata-change time. Failure to read or set timestamps closes/discards the
copy and falls back to the original. Source content/mtime are not written.

The native fixture sets an old fractional timestamp, invokes the emitted
machine-code route and a second direct open, and compares descriptor mtime
against pathname mtime. It also checks the captured atime, patched content,
unchanged source content, independent stream lifetimes and update-mode bypass.
This proves real libc stream behavior, not the game's preview refresh policy.

## Save compression

`d1ef108` moves the Linux Streams implementation into `native/src/save_zstd.h`.
A comparison with the Linux parent shows only comments and removal of the
Linux installer declarations; executable stream logic is unchanged. Linux
keeps its dlopen'd threaded libzstd >=1.4 and atomic takeover gate after all
six redirects. Windows alone builds the vendored zstd and adds its startup
self-test; native API probing and stream/partial-install tests remain intact.

Re-ran `verify_save_steam_elf.py`: all five whole-function guards and seven
call targets (including the unchanged Steam poll hook) match the actual ELF.
The six save sites and SysV derivation are in
[DEV_F0212C87.md](DEV_F0212C87.md). Independent disassembly again shows deflate
loading context from +0 into RDI, input from +0x10 into RDX and output from
+0x18 into RSI before `0x3563379 -> 0x3d8d940`. No Windows RVA is reused.
Host libzstd 1.5.7 round-tripped 25,165,824 bytes into 9,894,603 bytes with
four workers through the shared header; this is not a game-save measurement.

## Live attempt

Installed the soldier candidate and merged Lua into backed-up native actor
copies and invoked only `tools/sandbox/tpf2mp-lab run native --root
~/.local/share/tpf2mp-lab`. It exited 1 before game execution with
`bwrap: setting up uid map: Permission denied`. There was no game process
for gdb, no Vulkan selection, and no preview/save/load observation. No desktop
input or workaround outside the lab was attempted. Both directories were
restored and recursively compared with `.before-port-568e7ea7` backups.
Job `meta/live/` contains disassembly, ELF checks, launch output and restoration
proof; copied actor logs/data predate this failed game launch and are not new
live evidence. Preview stability and game save/load remain unobserved locally.
