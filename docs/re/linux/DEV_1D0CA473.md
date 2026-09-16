# dev 1d0ca473: line-editor callback replay

Rechecked read-only against Steam Linux build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. No game was run.

## Mapping and decision

Windows `1d0ca47338ff40f797a09bfcf793fd5dd08ba317` adds the
`sendCommand` Add return address `0x1126f1a` and claiming-thread match because
Lua rebuilds the factory's Command before Add. The Linux implementation already
handles this in `slice_lines.cpp`: `ClaimCreate` reserves a held callback in
thread-local `t_carrier`; `ReplayAdd` recognizes the two script Add return
addresses, and `TakeCarrier` checks the reservation, normalized CreateLine
payload, tag 3, session, instance and five-second claim lifetime. No factory
pointer match is used. Preserve this implementation, including the direct sink,
rather than copying the Windows address, thread-id storage or function layout.

The shared anchors and full registration chain are in
[SLICE_CORE.md section 5.2](SLICE_CORE.md#52-script-issued-commands--c-script-3-proven-c-script-4-unproven):
`SetupCommandInterface` at `0x19638f0`, its `sendCommand` registration at
`0x1963a2d`, sol2 wrapper `0x197cf00`, and body `0x197a6f0`.
Fresh objdump inspection confirms the Command move at `0x197adab` and sink
invocation at `0x197add9` (`ff 50 18`). SysV sink arguments are `rdi` = functor,
`rsi` = Command, `rdx` = callback. The sink's next move constructs another
Command; using the factory's output address would be incorrect here too.

## Rechecked instructions and bytes

Both queued sinks copy the callback with manager operation 2, move the Command
to `[rbp-0xa0]`, and call `CommandList::Add` at `0x15da840`.
Immediately before Add, `rdi` = connection result slot, `rsi` = CommandList,
`rdx` = rebuilt Command, `rcx` = callback, `r8` = progress function.

| Site | Bytes | Meaning |
|---|---|---|
| `0xa2f5a5` | `e8 f6 98 ba 00` | move Command through `0x15d8ea0` |
| `0xa2f5aa` | `48 89 d9 4c 89 e2 4c 89 f6 4c 8d 85 10 ff ff ff 4c 89 ff` | Add argument setup |
| `0xa2f5bd` | `e8 7e b2 ba 00` | Add; return **`0xa2f5c2`** |
| `0x112258c` | `e8 0f 69 4b 00` | move Command through `0x15d8ea0` |
| `0x1122591` | `48 89 d9 4c 89 e2 4c 89 f6 4c 8d 85 10 ff ff ff 4c 89 ff` | Add argument setup |
| `0x11225a4` | `e8 97 82 4b 00` | Add; return **`0x11225a9`** |
| `0xa2d650` | `f3 0f 1e fa 55 48 89 e5 41 55 49 89 fd 41 54 49 89 d4` | direct sink entry; existing 18-byte check, 15 stolen bytes |
| `0xa2d698` | `e8 d3 57 bb 00` | direct apply through `0x15e2e70`, without Add |
| `0xa2d6ab` | `41 ff 54 24 18` | invoke completion through callback +0x18 |

The two complete move/setup/Add spans, direct entry and direct apply/completion
span were compared byte-for-byte to executable PT_LOAD segments; all four
passed (`.git/port-line-elf.log`). Addresses above came from disassembly, not
Windows offsets. libstdc++ callbacks remain 32 bytes: 16-byte storage, manager
at +0x10, invoker at +0x18. The queued Add takes ownership; the direct sink
invokes without moving, and the existing wrapper releases the held callback.

No new patch sites are introduced. Existing build-id/read-backend gates,
Add/factory prologue checks and direct-sink byte/boundary checks remain intact;
failed checks prevent the affected hooks/cancellation from activating. The two
return addresses are caller classifiers, not sites patched by this change.

## Regression coverage and limits

`test_slice_commands.cpp` now tests both queued sinks with distinct maker and
replay Command objects and a copied payload. An unrelated UI caller and a second
thread cannot take the claim. The correct caller on the claiming thread takes
it once, preserves the Lua callback object and releases the held record; a
second Add cannot repeat the handoff. Existing tests cover direct completion,
mismatched payloads, stale/reused/malformed claims, callback ownership and expiry.
The Add harness models engine ownership transfer; it does not prove live line
selection or which Lua state runs a particular session. Those require gameplay,
which this task explicitly forbids starting.
