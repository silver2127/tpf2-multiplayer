# Native investigation for Windows dev b5dade06

Target: Steam Linux build 35924. Static `readelf -n` rechecked build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` in
`~/.local/share/tpf2mp-lab/native/game/TransportFever2`. No game was run.

## Lobby controls

The Windows changes use Win32 mouse events, GDI controls, process arguments
and JSON roster state; they add no executable address or game struct access.
Linux equivalents use the existing SDL event filter, panel layer, worker queue
and lobby model. SDL_BUTTON_RIGHT is translated to the previous-company rule;
a captured right release is consumed even outside the panel or after closing.
Only chips act on right presses. Mode requests use the existing generation-bound
queue, require a ready live host/relay leader, and are confirmed by roster state.
The launch preference is separate from the authoritative current lobby mode.
No new ELF patches or ABI assumptions were introduced.

## Company-window rename: investigated, not enabled

2a87bb4 relies on pre-existing Windows SetName capture. Linux's
`slice_lines.cpp` explicitly refuses both SetName and SetColor; its central
player-action barrier cancels unsupported clicks. The new shared inject branch
can turn VNAME for the local player into CMNAME, but receives no such Linux record.
Ordinary VNAME still sets skipOrigin=1 in the current Lua, so simply shipping
cancelled names would leave the originating entity unchanged.

Revisited [SLICE_LINES.md](SLICE_LINES.md), its named assert anchors, factory
arguments, two UI callers, and Lua maker exclusion. Actual disassembly checked
with `objdump -d -Mintel --start-address=... --stop-address=...`:

| Site | Verified bytes / behavior |
| --- | --- |
| SetName 0x15ee6d0 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55`: existing guarded 14-byte prologue |
| 0x15ee6e8 | `48 89 b5 10 f2 ff ff`: saves rsi (engine) |
| 0x15ee6ef | `48 89 8d 18 f2 ff ff`: saves rcx (libstdc++ string reference) |
| 0x15ee705 | `83 fa ff`: validates edx (entity) against -1 |
| 0x15ee71c | `49 89 ff`: saves rdi (hidden Command return storage) |
| 0x15ee72e | `48 8b 59 08`: reads string length at +8, not MSVC layout |
| 0x14287da | `e8 f1 5e 1c 00`: generic UI SetName call; return 0x14287df |
| 0x1428801 | `e8 3a 20 1b 00`: CommandList::Add, rdx=r13=the factory return slot |
| 0x1426830 | `f3 0f 1e fa 48 8b 07 48 8b 00 c6 00 00 c3`: UI completion dereferences captured state twice and clears a byte, independent of success |

The factory ABI is known (SysV rdi=result, rsi=engine, edx=entity,
rcx=string). Knowing it is insufficient to enable the feature safely. The
attempt traced the existing generic UI caller and its callback but did not
establish company-window-specific ownership/lifetime of that captured byte,
or a cancellation/completion adapter that releases it while correctly replaying
the originating rename. A company-only capture also needs a verified way to
distinguish this player entity before shipping, without enabling the other
skipOrigin name paths. Those are the remaining evidence and implementation
gaps. Existing byte guards and unsupported-action cancellation remain intact;
no guessed field or address was added. Company defaults, registry names,
CMNAME protocol and the new dashboard are shared and merged.
