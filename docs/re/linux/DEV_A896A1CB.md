# Native plugin short hooks: dev a896a1cb

The incoming commit changes the Linux plugin API's minimum stolen length from
14 to 5. Windows still requires 14. It introduces no game address, ABI or field
offset: the Linux writer already implements short hooks, but the host refused
them before reaching it. The sidecar's existing build-ID and byte checks remain
unchanged. Earlier DEV_2B4FD093's claim that the host supported its 13-byte hook
was incorrect until this fix; its fixture used a fake host callback.

Fresh objdump of the actual lab build-35924 ELF confirms AddTile at `0xcf71d0`:

```
cf71d0  f3 0f 1e fa           endbr64
cf71d4  55                    push rbp
cf71d5  48 89 e5              mov rbp,rsp
cf71d8  41 57                 push r15
cf71da  49 89 ff              mov r15,rdi
cf71dd  48 8d 3d 83 e4 22 03  lea rdi,[rip+0x322e483]
```

The first 13 bytes are position independent and end on a boundary. Stealing
14 would include part of the following RIP-relative LEA. The existing SysV
terrain argument RDI is preserved in R15; no calling convention changes.
See [the original terrain derivation](DEV_2B4FD093.md) for function anchors,
remaining arguments and unresolved terrain ownership contracts.

`hook_posix.cpp` copies the stolen bytes to an RX trampoline and appends an
absolute jump back. For 5..13 it allocates near the target, puts an absolute
jump to the detour at trampoline+128, checks the signed rel32 range and writes
an E9 jump to that stub. The original pointer is published before patching.
Runtime instruction-boundary and executable/readable-range checks in the host
remain intact, including its pre-existing decoder-unknown fallback to the
plugin's contract. This change does not make that fallback a relocation engine.

The new `plugin_host_hooks` CTest calls the real host API table with the real
writer on synthetic RX functions. Every length 5..32 executes its detour and
original trampoline, resumes a RIP-relative read at the original location and
returns the expected value. Invalid lengths, null arguments, non-executable
memory and known cuts inside the actual AddTile prologue are rejected. The
fixture suppresses only the host constructor, avoiding plugin discovery and
writes to user runtime data. Existing low-level hook tests remain intact.

Fresh build-ID and all 39 Big Maps site checks pass. Job evidence is under
`meta/live/addtile.asm` and `verify-game.txt`. Candidate libraries and Lua were
installed in backed-up lab actor directories. The prescribed native launch
exited 1 with `bwrap: setting up uid map: Permission denied`, before a game
process existed. No gdb probe, Vulkan device, menu or terrain load was observed.
Both directories were restored and their hashes/modes/symlinks checked; see
`restoration.json`. Archived actor logs/data can include historical runs and
are not evidence of candidate execution. No new live terrain claim is made.
