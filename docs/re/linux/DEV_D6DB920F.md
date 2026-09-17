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
