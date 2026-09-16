# Hosting crash in the Windows 0.4.22 lobby under Proton

## Reproduction

Proton 11.0 launches the Windows game and multiplayer DLLs successfully. Joining a
Windows host transfers the shared save and runs the game; seven common state and
geometry hash checkpoints through simulation time 144 matched, including after
remote build proposals. This is one short test, not extended determinism coverage.

Hosting instead emitted one `observing NAT` status and exited with Windows status
`0xc0000005`. The menu retained the waiting message. An isolated invocation of the
official `netpunch.exe host` reproduced the crash without the game, public listing,
or rendezvous. Joining skips the UPnP discovery that imports the failing dependency.

## Cause

The official executable's SHA-256 is
`54bed86eaa76a54c70560f380716fef9bf5bded6ac549df4643853e6a7e6882c`.
Its embedded `miniupnpc-5520bde33208435242b8509993047f10.dll` contains 2,179
non-padding base relocation entries but only 2,115 unique targets: 64 DIR64 entries
appear twice. The other embedded PE binaries do not have duplicate targets.

The duplicate records include all four absolute pointers in the TLS directory.
In the captured failure:

| Value | Address |
| --- | --- |
| Preferred DLL base | `0x6ad80000` |
| Actual DLL base | `0x7abf0000` |
| Relocation delta | `0x0fe70000` |
| Original TLS index pointer | `0x6ad9c04c` |
| Correct pointer after one relocation | `0x7ac0c04c` |
| Pointer after duplicate relocation | `0x8aa7c04c` |

The crash is a write to `0x8aa7c04c`, exactly the duplicate-adjusted pointer.
The installed Proton's `ntdll.dll` symbols and disassembly identify the instruction
as `fixup_imports+0x18b`, writing the TLS module index through `AddressOfIndex`.
The fault occurs while loading the dependency, before router discovery executes.
This is malformed relocation metadata exposed when the DLL is relocated; it does
not imply that Wine should ignore ordinary relocation records.

## Repair scope

Preserve each first DIR64 relocation and convert only its repeated records to
ABSOLUTE padding. Keep section layout, executable code, exports, and the first
relocation at every address unchanged. Recalculate the PE checksum and repackage
only that dependency inside the frozen lobby. All other archive member contents
must remain byte-identical. Both the released executable and dependency are
unsigned, so no signature can be preserved or should be implied.

This repair does not change the game-hook DLLs, the 24 shared Lua files, the lobby
protocol, or native Linux artifacts. The released Windows hooks' existing strict
replay coverage limits remain separate from this hosting crash.

## Repair validation

The repaired lobby completed a real isolated hosting test under Proton 11.0. It
produced the lobby code and initial roster after 9.8 seconds, including Wine startup.
STUN returned both answers in 147 ms. The test used a separate UDP port, no public
listing, and disabled rendezvous, then sent the normal quit command. The process
exited successfully with no remaining probe process.

Repaired executable SHA-256:
`da4fb91961f1aed3c7e7543a837220902a7af11a0ea59412dd7b77bfd001111f`.
The repaired lobby was installed with the pinned setup variant and all 33
installed payload files verified, including the unchanged Lua. The game was
relaunched successfully to the main menu. The user's next Host Game attempt
will confirm the menu flow; the isolated host test above has already passed.
