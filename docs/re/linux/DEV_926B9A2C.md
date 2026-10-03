# dev 926b9a2c: vehicles at a stop and unload queues

Incoming Windows commit also supplies the Linux implementation. Retained it
without changing its hook contracts after independently checking the lab ELF,
GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a` (Steam 35924).
The behavioral investigation is [TERMINAL_WAIT_ORDER.md](../TERMINAL_WAIT_ORDER.md).
This record distinguishes local static verification from unobserved gameplay.

## Static evidence and ABI

The exported function inventory gives terminal Update at `0x16f6090`, 12884
bytes, and vehicle Update at `0x16fd820`, 4572 bytes. The Itanium vtables
`0x59c0c40` and `0x59c0cb0` have those Update functions in slot 11 and
EntityAdded `0x16f5bb0` / `0x16fd260` in slot 4. All four entries were read
from the actual ELF, and are now checked by `verify_order_canon_elf.py`.

At `0x16f67fc`, RDI receives the TransportVehicleSystem from the terminal
system's +0x20 field; RSI receives the line entity pointer and EDX the stop
index. The call at `0x16f680d` (`e8 ce b0 07 00`) targets `0x17718e0`.
That getter hashes the two keys and probes phmap storage at system+0x1d8,
+0x1e0 and +0x1f0. Its assert references `parallel_hashmap/phmap.h` at
`0x3e8e0f0` and the mask assertion at `0x3e8f578`. It returns a vector
address in RAX (or the static empty vector at `0x5a51d50` on a miss).
RAX is not overwritten before the hook. The consumer at `0x16f6fe2` loads
the saved pointer, then its end at +8 and begin at +0.

The `vehstop` hook at `0x16f6823` steals exactly seven bytes:
`48 89 85 e0 f1 ff ff`, `mov [rbp-0xe20],rax`. The helper sorts the int32
IDs in this vector in place, and the trampoline replays the store. Its full
16-byte guard is `48 89 85 e0 f1 ff ff 89 da 4c 89 f7 e8 5c ed 2e`.

At `0x16fde3f..0x16fde4a`, cargo index times five, shifted left four and
added to the vector begin, selects an 80-byte deque object in R15. Its
start/finish cursors are compared at +0x10/+0x30 before the unload path.
At `0x16fe0ae` the hook steals eight bytes: `49 8b 47 30 49 8b 5f 10`,
loading finish.cur into RAX and start.cur into RBX. The full guard is
`49 8b 47 30 49 8b 5f 10 4d 8b 67 20 4d 8b 7f 28`. Subsequent loads take
start.last and start.node from +0x20/+0x28. The consumer reads an int32 at
RBX, advances four bytes, and switches blocks through the node map with a
0x200-byte block end (`0x16fe13f..0x16fe146`): 128 entity IDs per block.
The shared GNU deque helper sorts values without changing the header.

Neither stolen span contains RIP-relative instructions; full-function
Capstone disassembly confirms instruction boundaries and no branch into
either span's interior. The jump at `0x16fe1df` targets the unload hook's
first instruction. The existing SysV assembly dispatcher preserves GP/XMM,
flags, MXCSR and stack alignment; fixtures execute all nine actual shims.
Runtime installation checks build-id and every site's 16 bytes before
patching, and rolls back on installation failure. The existing
`TPF2MP_ORDER_CANON=0` switch still disables the whole ordering module.

## Tests and live limitation

Added the upstream synthetic MSVC/GNU deque test to native CTest. Extended
the Linux shim test with an actual libstdc++ deque: 700 inserts, 117 front
removals, sorting the remaining 583 IDs over multiple blocks, a second
already-sorted pass, then complete consumption and an empty pass. This
checks the SDK layout independently of the synthetic headers. It does not
prove game-side container lifetime or cross-platform synchronization.

Installed the candidate soldier libraries and merged Lua in the native lab
actor after `cp -a` backups suffixed `.before-port-926b9a2c`, enabled autoload,
and ran the prescribed lab command. It exited 1 immediately with
`bwrap: setting up uid map: Permission denied`. No game process existed
for gdb, so no live register/container, menu, Vulkan, boarding, unloading,
or retained-host/loaded-joiner observation is claimed. No desktop input or
Proton run was attempted. Both payloads were restored and SHA-256/symlink
manifests match; no saves changed and no game was left running.

Evidence in this job's `meta/live/`: `terminal.asm`, `vehicle.asm`,
`getter.asm`, `verify-order.txt`, `launch.txt`, `launch-result.txt`,
`restoration.json`. Copied `actor-logs/` and `actor-data/` are historical,
not evidence that this candidate ran. Live ordering and lifetime validation
remain outstanding; no code from this commit is omitted.
