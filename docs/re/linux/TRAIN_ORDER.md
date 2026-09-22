# Train reservation order: Linux build 35924

Integration: Windows `3edfbccd52c0498124a55db57c0714796f3600a1`.
ELF GNU build-id: `3a0e156390b0e6f1e372051c24802c8493ae454a`.
The supplied `~/.local/share/tpf2mp-lab/native/game/TransportFever2` was read,
never executed. All addresses below are PIE-relative virtual addresses.

## Identification and evidence

`~/tpf2-re/linux/funcsig.csv` identifies `0x1758160`, length 7234, as
`virtual void ecs::TrainMoveSystem::Update2(ecs::Engine*, int, float) const`,
anchored by the TrainMoveSystem.cpp assertion/source strings. Its worker lambda
is `0x1755380`; the ThreadPool loop is `0x1757330`. The serial reservation
loop follows the index shuffle, before the parallel movement work. This matches
Windows Update2 `0xabdc20` structurally, without borrowing its addresses.

Commands used to derive the map:

```sh
rg 'TrainMoveSystem|component::Name' ~/tpf2-re/linux/funcsig.csv
readelf -n ~/.local/share/tpf2mp-lab/native/game/TransportFever2
objdump -d -Mintel --start-address=0x1758160 --stop-address=0x17586d0 GAME
objdump -d -Mintel --start-address=0xc0cf10 --stop-address=0xc0cf30 GAME
objdump -d -Mintel --start-address=0x9e3d50 --stop-address=0x9e3dd5 GAME
objdump -d -Mintel --start-address=0x9e5590 --stop-address=0x9e57e0 GAME
objdump -d -Mintel --start-address=0x14910f0 --stop-address=0x14913f0 GAME
objdump -d -Mintel --start-address=0x12f1ac0 --stop-address=0x12f1e7b GAME
objdump -d -Mintel --start-address=0x175a510 --stop-address=0x175a5c5 GAME
```

Replace GAME with that ELF path. `0x14910f0` is a Name-component access
identified by its CompAccess assertion; `0x12f1ac0` has the Name CompVec
downcast assertion and serializes both flat and paged Name storage.

## Hook and SysV ABI

| Site | Measured behavior |
| --- | --- |
| `1758173` | `49 89 fe`: self (`rdi`) saved in `r14` |
| `1758188` | `48 89 b5 68 fe ff ff`: world (`rsi`) saved at `[rbp-0x198]` |
| `1758423` | `89 04 86`: fill int32 index array with iota |
| `175842f` | `49 8b 46 48 48 8b 78 18 e8 d4 4a 4b ff`: self+48 -> +18 -> GameTime accessor `c0cf10` |
| `175843c` | signed extend accessor int into `rcx`, seed reduction follows |
| `175843f..1758466` | array begin in `rbx`/`rdi`/`r15`; end in `r13`/`rsi` |
| `175848f..1758496` | normalized RNG state on stack; its address in `rdx` |
| **`175849d`** | **`e8 6e 20 00 00`: call shuffle `175a510`** |
| **`17584a2`** | **`49 39 dd 0f 84 24 03 00 00`: resume with begin/end comparison** |
| `175869d..17586c4` | self+8 -> record base; index*12; entity id +0, Train slot +4, MovePath slot +8 |

Only the shuffle CALL's displacement changes. It goes through the existing
near-allocation helper to `SliceTrainOrderRelay`. The relay passes additional
SysV arguments `rcx=r14` (self), `r8=[rbp-0x198]` (world), tail-jumps to the
C++ function and retains the engine return address. Arguments 1–3 stay
`rdi=begin`, `rsi=end`, `rdx=RNG*`. No stack adjustment, stolen prologue or
shuffle-body jump is needed. The relay has DWARF CFI.

The engine resumes with its callee-saved `rbx`, `rbp`, `r12..r15` intact; the
reservation loop uses the stack-saved floating values. XMM registers are
caller-saved in SysV and no Windows XMM nonvolatile assumption is used.
The helper at `175a510` is independently confirmed as an int32 shuffle: begin
and end difference divided by four, random draw at `175a5a4`, swaps at
`175a5a9..175a5b5`. Its untouched RNG argument and array are passed to the
original function on refusal. Foreign calls sit outside our allocation catch.

`c0cf10` has bytes
`f3 0f 1e fa 55 48 8d 77 10 48 8b 7f 08 48 89 e5 e8 0b 43 00 00 5d 8b 40 30 c3`:
it gets the component through `c11230`, returning the int at component+30.
The Linux wrapper reads that raw value again through the same accessor, casts
to uint32, and uses the shared Windows seed fixup. Using GCC's already
sign-extended and reduced 64-bit seed would diverge for high-bit int values.

## Name lookup and memory layout

| Item | Evidence |
| --- | --- |
| Name RTTI `5a02600` | +8 points to `3f82170`, string `N3ecs9component4NameE`; loaded pointer and text checked |
| registry world+48 | `149113b..1491153`: RTTI pointer address passed in `rsi`, registry in `rdi`, call `9e4ab0` |
| non-asserting type find `9e3d50` | called by `9e4ab0`; hashes RTTI name and returns map node or null; `9e4adb..9e4ae1` subtracts one from node+10 |
| pools world+80 | `1491162..1491176`: type*8 -> pool; flat data header at pool+b8 |
| entity slots world+98 | `9e55bd..9e55df`: id*24 -> vector, 8-byte `{type,slot}` pairs |
| slot result | `9e5730..9e573b`: return matching pair+4; missing-slot path formats assertion, so never called |
| flat Name | `14912d8..14912e1`: slot*32 + data at pool+b8 |
| paged Name | `149118b..14911b5`: slot>=40000000; page=(slot-40000000)/32, table at pool+d0, 16-byte page entry, 32-byte Name |
| Name string | `12f1cc0`, `12f1ced`: length +8, pointer +0; increment 32; paged serializer repeats at `12f1d50..12f1da9` |

The implementation calls the non-asserting type find once per step, without
caching a world's type index. It scans entity slots with guarded reads and
uses the port's existing libstdc++ string validator for both SSO and heap
strings. Missing/unreadable names become empty names, matching Windows.
All names, keys, records and indices are copied to thread-local private
buffers before sorting. An invalid count, records/count mismatch, non-iota
array, failed record read or allocation failure leaves the engine array
untouched and invokes its original shuffle. Only a successfully arranged
permutation is copied into the engine-owned writable index allocation.

## Guards and tests

`native/linux/src/slice/train_order_checks.h` contains the exact bytes and
lengths of all 16 runtime checks, including the full `175842f..17584ab` seed,
argument setup, call and resume sequence. Installation also checks the loaded
RTTI text and resolves the original CALL target. The existing slice build-id,
read-backend, static-check and `TPF2MP_NO_PATCHES` gates precede installation.
A failed installation keeps multiplayer readiness false. The explicit
`trainorder=0` switch leaves the engine shuffle installed, checking the root
flags file before the data flags file, as on Windows.

`tools/linux/verify_train_order_elf.py GAME` checks these bytes from ELF load
segments, build-id, RTTI text, seed/shuffle/type CALL targets and the complete
CALL boundary. It decodes Update2 and rejects branches into that CALL. No
other engine instructions are removed or skipped.

The shared upstream ordering test runs in CTest. The Linux test adds guarded
reads of flat/paged names, short/long strings, missing/invalid slots,
registration-order independence, high-bit seeds and failure paths. A
synthetic mmap code image checks the real installer, mutated-byte rejection,
near relay and SysV frame arguments, raw seed handling and original-shuffle
fallback. It executes only fixture code, never the game ELF.

Diagnostics preserve seed, count, named count, reorder flag, id hash,
duplicate warning, deterministic seed sampling, first >1 ms warning, and
periodic step/reorder/refusal/last-seed/count/max-time counters.
No live game validation was performed. Name/id ties and recycled-id rank
assumptions remain upstream limitations; bounded jitter is not a proof of
starvation freedom for every possible train pair.
