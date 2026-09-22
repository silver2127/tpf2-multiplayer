# dev a658fc11: company palette

The palette change itself introduces no game addresses, ABI changes or new
hooks. Linux lobby `panel_linux.cpp::CoColor` owns RGB byte values and uses the
same arithmetic as Windows `menu_hook.cpp::coColor`; it now uses all 20 incoming
triples and starts the golden-angle fallback at company 21. Shared Lua supplies
vehicle paint and stylesheet colours unchanged from upstream.

## Existing native tint gap: static recheck

Read the supplied native Steam build 35924 ELF, confirming GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` with `readelf -n`.
Following the ItemCreator and ViewCreator candidates anchored in
[DEV_D3135A59.md](DEV_D3135A59.md) and [DEV_59BB258A.md](DEV_59BB258A.md),
used `objdump -d -Mintel` from the function starts `0x1383570` and
`0x1385ec0`, and inspected the window-binding helper at `0x1444ee0`.
Addresses below are analysis only, never installed patches.

- Label helper: `0x1383594: 49 89 d5` saves SysV rdx in r13;
  `0x13836bf: 66 41 0f 6f 65 00` reads an aligned 16-byte colour from it.
  The append path advances rsi by 32 at `0x1383688` (`48 83 c6 20`).
  This confirms a colour input but does not prove station owner lookup,
  world lifetime or all live state at the proposed caller insertion point.
- Vehicle candidate: `0x1385ec6: 41 89 d7` saves edx as a flag; its append
  advances rsi by 16 at `0x138610f` (`48 83 c6 10`). The nested call
  `0x138612e: e8 ad 6b ff ff` targets `0x137cce0`, with rdi from the saved
  pointer, esi from r15d and rdx from r14. It still does not establish a
  compatible colour-pointer draw path or texture/material contract.
- Window candidate: `0x1444f70: 4c 89 e6` passes a temporary libstdc++ string
  in rsi; `0x1444f73: 4c 89 ef` restores the widget argument in rdi;
  `0x1444f85: e8 86 4f ce 01` calls `0x3129f10`. String cleanup follows.
  As previously traced, this binds an entity ID; no evidence establishes
  it as the style-class setter needed for the window wash. That setter's
  ABI/ownership and safe company lookup remain missing.

The new RGB constants cannot supply these missing render contracts. Linux
has no `IconCompanyColor` equivalent to update. Icon/label/window tinting
therefore stays unavailable, and this integration is marked partial for
those consumers. Existing byte-verified visibility and permission patches
are unchanged. No guessed offsets, calls or colour-buffer substitutions were
introduced. Captures are in `.git/port-a658fc11-{label,vehicle,window}.asm`;
this record preserves the evidence independently. The game was never run.
