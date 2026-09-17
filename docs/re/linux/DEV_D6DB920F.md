# dev d6db920f: icon-element class application (not ported)

Read-only analysis of Steam Linux build 35924; `readelf -n` confirms build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. No game was executed.
Addresses below are ELF virtual addresses, evidence only, not installed patches.

## Windows contract

4e0ec0f captures edx=entity at the content-builder entry 0x5e45d0 and
redirects two carrier-class calls (0x5e07f9 and 0x5e2d13). The wrapper
applies the original class, then the owner's !mpWinCoN on that same icon.
It factors the non-asserting owner lookup and adds glyph counters. The shared
stylesheet uses backgroundColor1 and a darker hover variant. e69a3cf fixes
an invalid non-ASCII Python bytes literal in the Windows verification test.

## Fresh Linux trace

Searched functions/signature/xref exports, then disassembled the complete
station factory 0x1090250..0x1090b9c and HudIconManager::DoStep
0x1093b30..0x1096eed from the actual ELF. Captures are in
`.git/port-d6db920f-{station,hud}.asm`. The function/source signature anchors
identify HudIconManager.cpp; actual ELF string reads confirm StationItem at
0x3f2b74a, ::StationIcon at 0x3f2b784, VehicleDepotItem at 0x3f2b81f,
and ::Icon at 0x3f275db.

The style helper 0x30550d0 and mutable libstdc++ string contract were already
established in [DEV_61578D27.md](DEV_61578D27.md). New caller evidence:

- Station: 0x109057c constructs the ::StationIcon name. Allocation is saved
  in r12 at 0x109059b (`49 89 c4`), then constructed by 0x3053c10.
  At 0x10905d9/0x10905dc (`4c 89 ee 4c 89 e7`), rsi=r13 points to the
  temporary class string and rdi=r12 is the icon. 0x10905df
  (`e8 ec 4a fc 01`) calls 0x30550d0. This is one of ELEVEN carrier
  branches, not the single call asserted by the Windows test: others are
  0x1090829, 0x1090848, 0x1090867, 0x1090886, 0x10908a5, 0x10908c4,
  0x10908e3, 0x1090902, 0x1090921, 0x1090940. All resolve to 0x30550d0
  in the disassembly and converge at 0x10905e8. The loop increments ebx
  and compares against 11 at 0x1090616..0x109061c; ebx here is a carrier
  index, not the entity. The jump table is at 0x4132260.
- Depot construction is inlined in DoStep. 0x109559e loads ::Icon;
  0x10955c0 (`49 89 c4`) saves the icon allocation in r12 before
  construction at 0x10955c3. 0x109566c/0x109566f
  (`4c 89 f6 4c 89 e7`) pass rsi=r14 class temporary and rdi=r12 icon.
  0x1095672 (`e8 59 fa fb 01`) calls 0x30550d0 for the `air` string
  at 0x3f2b83a. Four alternate calls at 0x1096b65, 0x1096b84,
  0x1096ba3, 0x1096bc2 converge at 0x1095677. The first alternate
  uses `tram` at 0x3f2b7b7. The switch at 0x10955dd reads [rbx],
  with jump table 0x413228c; rbx here is a component pointer.

Attempted to trace context from the station factory entry: it saves rsi,
rdx, rcx, r8, r9d at rbp-0xa8, -0xb0, -0xb8, -0xc0, -0xc4,
and also consumes four stack arguments. Calls through 0x146f0a0 and
0x109a8f0 use these saved values. This does not establish a Windows-like
common content entry with a single entity argument for both paths.
Reviewed the existing non-asserting component scan and owner evidence in
[DEV_DB8A4776.md](DEV_DB8A4776.md), and searched PlayerOwned/StationGroup
RTTI references. Those references alone do not establish the live engine,
StationGroup pool stride or first-station ownership at these icon sites.

## Decision and missing evidence

The attempt locates both icon/class paths but does not establish a complete
entity-to-owner relay. Still needed: station and depot entity provenance,
UI-engine lifetime across world changes, verified non-asserting StationGroup
and PlayerOwned access with native pool layouts, and an exception-safe relay
covering all carrier branches (including indirect branch targets if stealing
instructions). A global Windows-style entity cache is not justified by this
inlined/looped Linux structure. No guessed address, offset, ABI or patch is
introduced. Native tinting and glyph counters remain unavailable; shared
selectors cannot apply themselves. Existing visibility hooks stay intact.

---

# 2026-09-17 revisit: settled in the running game

The sections above are the static-only record. This one supersedes their
"Decision and missing evidence": the open contracts were closed in the lab
game with gdb, and the tint now ships
(`native/linux/src/slice/company_tint_linux.cpp`).

## How the game was driven

There is no input automation on this desktop, so the game was steered through
the mod's own autoload. First `MenuGame_RequestAutoload` was called through
gdb; that worked twice and then crashed the game twice (crash dumps 16:26:47
and 16:35:08), because gdb runs an inferior call on whichever thread it
stopped, and a save name longer than a `std::string`'s local buffer makes that
call allocate. It was replaced by pure memory writes into the mod's own
`AL()` state and `g_alPending`
(`meta/live/request_load.py` in the job directory): no code runs while the
game is stopped, and the load then happens on the game's own menu frame.
Save: "Ordering desync Sep15", copied to the short name "Stations".

## The engine and the entity (0x1090250)

Breakpoint at the `StationItem` constructor entry, before the prologue:

```
CTOR #1 entity=28301 enginePtr=0x5730bf82ef80 ret=0x5730af4c70ba
ENGINE: EnginePtr=0x5730bf82ef80 -> engine=0x74138b5cc760
```

`rsi` is the `UI::EnginePtr` and `r9d` the entity, exactly as the prologue's
`mov [rbp-0xa8],rsi` / `mov [rbp-0xc4],r9d` suggested. The engine came from
calling the game's own `0x146f0a0(&EnginePtr)` (`0x1477120(ptr)` then
`[rax+0x28]`) on the saved slot.

## Type indices (0x9e3d50)

`0x9e3d50(engine+0x48, &type_info*)` returns a node whose `+0x10` holds
index+1. Live, against the typeinfo objects located by their RTTI name
strings:

| component | typeinfo | index |
| --- | --- | --- |
| `ecs::component::Name` | 0x5a02600 | 19 |
| `ecs::component::Player` | 0x5a025f0 | 18 |
| `ecs::component::PlayerOwned` | 0x5a01c18 | 52 |
| `ecs::component::Station` | 0x5a03488 | 53 |
| `ecs::component::StationGroup` | 0x5a01b98 | 55 |
| `ecs::component::Town` | 0x5a01b88 | 22 |

0x5a02600 is the one `train_order_linux.cpp` already uses for Name, so the
whole table is anchored on a value the port had verified before.

## The owner: no StationGroup walk needed on Linux

The icon entity's own component record (`engine+0x98`, 24 bytes per entity, a
`std::vector` of 8-byte (type, slot) pairs) read:

```
entity 28301 components: [(55, 1), (19, 1344), (52, 7), (0, 23979), (9, 305)]
```

That is StationGroup **and** PlayerOwned on the same entity -- the thing the
Windows note said was absent there (`0x472900` returns null for a group on
Windows, which is why `db8a477` added the group -> stations[0] walk). Reading
PlayerOwned slot 7:

```
  PlayerOwned slot=7
    stride 4 -> comp 0x7412f8e6ae8c  first int32 = 19427
    ALL PlayerOwned int32 entries (stride 4): [19427 x 14]
    owner candidate entity 19427: Player=True name='ComradeSilver Transport'
  StationGroup slot=1 comp=0x7412f91abdb8 vector 0x7412f91e07c0..0x7412f91e07c4 -> stations [28300]
    station 28300 PlayerOwned slot=6 value=19427
  station-group name: "Dinnington St John's Modular terminal #2"
```

So both routes agree on owner 19427, and that entity carries a `Player`
component and the company's name. The group walk is kept as a fallback only.

## The pool layout and the strides

Live, per component pool (`engine+0x80` is the pool table):

```
  Name          ti=19  count@0xb0=1979  data=0x7412f8d00a80..0x7412f8d0b700  pages=0
  PlayerOwned   ti=52  count@0xb0=14    data=0x7412f8e6ae70..0x7412f8e6aea8  pages=0
  StationGroup  ti=55  count@0xb0=2     data=0x7412f91abda0..0x7412f91abdd0  pages=0
  Player        ti=18  count@0xb0=1     data=0x7412f8d12290..0x7412f8d122b8  pages=0
```

`+0xb8`/`+0xc0` are the component vector's begin/end and `+0xd0` the page
table -- the same fields `NameComponent` already used, plus the end pointer,
which the tint uses as a bound so a stale slot cannot read past the vector.
`+0xb0` is *not* a reliable element count (for Name it moved by 22 while the
vector grew by one 32-byte element between two probe runs), so nothing derives
a stride from it.

The strides are fixed statically instead, by the engine's own PlayerOwned read
inside the function the port already patches for `showicons`
(0x138ba89..0x138bacd):

```
138baa1: call 9e5590                 ; GetComponentDataIndex(engine, &entity, type)
138baa6: mov rdx,[r14+0x80]          ; pools
138baad: movsxd rcx,r13d             ; the PlayerOwned type index, cached at [rbx+0x190]
138bab0: mov rcx,[rdx+rcx*8]         ; pool
138bab4: cmp eax,0x3fffffff ; jg     ; paged?
138babb: mov rdx,[rcx+0xb8]          ; flat data
138bac4: lea rax,[rdx+rax*4]         ; STRIDE 4
138bac8: mov eax,[rax]               ; the owner
  paged: sub eax,0x40000000 ; and eax,0x1f ; sar edx,5 ; shl rdx,4
         add rdx,[rcx+0xd0] ; mov rdx,[rdx] ; lea rax,[rdx+rax*4]   ; STRIDE 4
```

`StationGroup` is one `std::vector<ecs::Entity>`; 24 matches both the type and
the live slot-1 address (`data + 24`).

## Where the class goes

`addStyleClass` 0x30550d0 disassembled in full: `rdi` = widget, `rsi` =
`std::string*`; an empty string returns at 0x30550f7; the class list is
`[widget+0xb0, widget+0xb8)` with capacity at `+0xc0`, 0x20 per string; a
duplicate is dropped (`cmp rax,[rbx+0xb8] ; jne` at 0x305513c); otherwise the
string is **moved** into the slot (local-buffer source handled at 0x30551d0),
`+0xb8` advanced by 0x20 and the source cleared at 0x305519d. A class name
`mpWinCo1..mpWinCo200` is at most 10 characters, so the string the tint passes
is always local: nothing crosses between the game's allocator and ours.

The entity for the element being styled is taken from the component lookup each
path already performs:

* stations: `call 0x109a8f0` at **0x10902e4** inside the constructor
  (`rdi`=engine, `rsi`=&entity, `edx`=type), which covers the DoStep build and
  the cargo-state rebuild alike, because both run this constructor;
* depots: `call 0x9e5590` at **0x1095525** in `HudIconManager::DoStep`, where
  0x1095505 copies the entity to `[rbp-0xe9c]` and 0x1095516 takes its address.

Both are redirected with `Tpf2mpRedirectCall`, which refuses anything that is
not that exact call. `tools/linux/verify_company_tint_elf.py` re-checks all
eighteen sites against the ELF and proves no branch inside either icon function
lands inside a redirected five-byte call.

## Re-checking it

`tools/linux/verify_company_tint_elf.py <TransportFever2>` re-derives all of
the above from the shipped ELF: the build-id, every byte anchor as a whole
number of instructions, the 11+5 `addStyleClass` calls and the two context
calls resolved by disassembly, that no branch inside either icon function lands
inside a redirected five-byte call, and the RTTI names behind the three
typeinfo pointers.

## Installed, live

```
[stationicon] installed: 11 of 11 station and 5 of 5 depot carrier-class calls
              redirected; class prefix mpWinCo
```

## Lab note

Two host problems had to be worked around, neither in the port:

* AppArmor `apparmor_restrict_unprivileged_userns=1` lets only profiled
  `/usr/bin/bwrap` create a user namespace, and a nested one is refused
  (`bwrap//&unpriv_bwrap` has no `allow userns`), so the lab's inner
  pressure-vessel layer cannot start. The job's copy of `tpf2mp-lab` runs the
  native game directly inside the outer bwrap, which is the layer that provides
  the isolation. No host policy was changed.
* Part-way through the session the NVIDIA modeset device stopped accepting new
  clients: every launch blocked forever in `nvkms_open_common` opening
  `/dev/nvidia-modeset`, with `dmesg` repeating "GPU:0: Error while waiting for
  GPU progress" (which had started before this session). Restricting the Vulkan
  loader to Mesa's lavapipe got the game running again.
