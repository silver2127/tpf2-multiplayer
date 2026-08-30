# Title-screen menu UI -- toward an in-menu Multiplayer button

Image base 0x140000000; addresses are RVAs. Provenance: strings.csv xrefs +
call_edges.csv (M1 static) unless marked live.

## Established (2026-08-29)

| RVA | identity | evidence |
|---|---|---|
| 663370 | `UI::CMenuUI::CreatePage(enum UI::CMenuUI::Page)` | its own assert string; also refs "Mods changed" |
| 667bc0 | main-page builder (called by CreatePage) | refs "Free Game", "Map Editor", "credits-button", "MainMenu", "Load Game", "Campaign", "continue"; sole caller = 663370 |
| 7c5d30 | menu-button construction helper (hypothesis) | called 9x from 667bc0 (~= button count), 27 sites binary-wide, no strings of its own (label must be an argument); appears in the "Button"-string referencer set |
| 569f00 | `UI::CGameUI::CreateUI` -- the IN-GAME ui builder | own assert; 118 strings incl. finances/stats menus AND mainMenuLeft/RightLayout etc. (in-game escape menu / top bar) |

## Key structural fact

CreatePage(663370) has MANY callers (65a0f0, 663110, 6725c0, 675060, 677d00,
67c8c0, 67c9f0, 67cb70, 67cb90, 67cce0, ...): the menu page is REBUILT on every
navigation. So the hook shape is: intercept CreatePage, run the original, and
when the built page is the MAIN page, append a "Multiplayer" button with the
same helper the builder uses. No live-widget-tree surgery needed.

## Open questions (round-2 decompile: tools/ghidra_targets_menu2.txt)

1. 7c5d30's exact signature: label (localized how?), icon, CALLBACK form
   (std::function layout, capture), parent layout argument.
2. The Page enum value for the main page, and how CreatePage dispatches to
   667bc0.
3. Where the button gets ADDED to the layout (addChild-equivalent) -- can we
   call it again after the original returns?
4. v1 callback plan: our DLL spawns tools/mp_launch.ps1 (CreateProcess) --
   the full in-menu save picker can come later.

## Round 2 -- CONFIRMED (decompiled)

- `CreatePage` = `void FUN_140663370(longlong this, int page)`. It `switch`es
  on the page enum; **`case 2` calls the main-page builder `FUN_140667bc0`** with
  4 args (this + 3 layout/render contexts set up before the switch). So the hook:
  detour 663370, call original, and when `page == 2` append our button.
- Main-page builder `667bc0` builds each entry as:
    `ctx = FUN_14221c930(&tmpstr, "<ActionKey>")`   -- action-keyed group
    `btn = FUN_1407c5d30(ctx, &iconA, &iconB)`       -- Button widget factory
    ... `FUN_14227a1e0(btn, &"name")` sets the widget NAME
    ... `FUN_14227f880(btn, 4)` finalizes / adds to layout
  Observed action keys, in order: "Free Game" (New Game), "Campaign",
  "Load Game", "Map Editor", plus username/credits/edition buttons.
- `7c5d30` = Button factory `(styleCtx, labelText, iconText) -> Button*`:
  builds a "Button" widget (083270 makes the std::string "Button"), sets text
  via vtable slot +0xd8, optional icon via 227a1e0. Takes NO callback -- the
  click is routed by the ACTION KEY, not a std::function.

## The click routing (the important part)

Buttons are bound by an action-key STRING, not an inline callback. A dispatcher
maps "Load Game"/"Campaign"/... to a handler that calls CreatePage with the
target page. CreatePage's caller cluster (677d00, 67c8c0, 67c9f0, 67cb70,
67cb90, 67cce0) are the per-action handlers. => Our button needs (a) an action
key and (b) that key routed to our code -- NOT a C++ std::function. Round 3
(tools/ghidra_targets_menu3.txt) decompiles 0x221c930 (the action factory) and a
handler to nail the binding form.

## Build plan (once round 3 lands)

DLL detours CreatePage(663370); on page==2, after the original returns, it
either (A) replicates the 21c930/7c5d30/227f880 recipe to add a "Multiplayer"
button, then hooks the action dispatch to catch our key and CreateProcess
`tools/mp_launch.ps1`; or (B) if 21c930 binds a callback pointer we can supply,
pass our own thunk directly. Fallback if the ABI proves too fragile: a
DLL-drawn overlay button (no menu-layout surgery) triggering the same launch.

> RVA NOTE (2026-08-29): the 0x22xxxxx helpers were truncated to 0x2xxxx in
> the first target files -- Ghidra then decompiled unrelated functions. Correct
> RVAs: action factory 0x221c930, click binder 0x227ca40, setName 0x227a1e0,
> finalize 0x227f880. Round 3b uses these.

## Round 3b -- COMPLETE PICTURE (decompiled, correct RVAs)

- `FUN_14221c930(&outCtx, "Free Game")` = builds the style/localization CONTEXT
  for a named menu action (calls 221d4a0 with &DAT_144466b80, a resource root).
  Returns the ctx that 7c5d30 uses. NOT a callback.
- `FUN_14227ca40(widget, &layout, weight, flag)` = adds the widget to a WEIGHTED
  layout list (weights 100/1000 seen; seeds an MT19937 for an element id). NOT a
  callback.
- `FUN_14227a1e0(widget, &nameStr)` = sets the widget's NAME/action string.

**Conclusion: no per-button click callback exists.** Each button is identified by
its action-key string (its name, set via 227a1e0) and the CENTRAL menu input
dispatcher routes a click on that name to the matching handler. This is the last
unmapped layer (the input/event loop that string-matches the clicked widget).

## Recommendation (build decision)

Two viable ends, both fed by the now-complete chain:

* **Native in-menu button (high fidelity, high risk):** detour CreatePage(663370)
  on page==2; after the original runs, replicate 221c930 -> 7c5d30 -> 227a1e0
  (name = "mp.multiplayer") -> 227ca40 to add the button, then hook the central
  click dispatcher to catch that name and CreateProcess mp_launch.ps1. Cost:
  ABI-exact std::string/SSO construction from injected code, plus finding+hooking
  the dispatcher -- every wrong offset crashes the menu. Multi-session build.

* **Overlay / companion (works today, low risk):** tools/mp_menu.ps1 is a
  companion "main menu" (save picker -> host session -> live status). A DLL-drawn
  overlay button in the menu is a smaller middle option. Neither touches the
  game's widget tree, so neither can crash the menu.

RE STATUS: the menu button CONSTRUCTION path is fully reverse-engineered and
documented. The remaining native-button work is the central click dispatcher +
ABI-exact injection -- a build task, not a discovery task.

## BUILD PROGRESS -- native menu button (2026-08-29)

Delivery mechanism and widget construction are PROVEN and working every launch:

1. **Startup injection** -- the installed proxy `alut.dll` (proxy_alut.cpp, a
   static import) now also `LoadLibraryW`s `tpf2_menu.dll`, so the hook is in
   place BEFORE the title menu's first build. Log: `[proxy] menu load OK`.
2. **Detour** -- `menu_hook.cpp` detours CreatePage(0x663370), steal 20 (through
   the rsp-relative `lea`, no RIP-relative). Fires `page=2` on the first main
   page, game unaffected. build_menu.bat -> tpf2_menu.dll.
3. **std::string ABI** -- built a game std::string via `83270(dest, cstr, len)`:
   `str probe: size=14 cap=15 data='mp.multiplayer'` (textbook SSO).
4. **Native Button widget** -- `221c930(&ctx, "mp.multiplayer")` then
   `7c5d30(ctx, &iconA, &iconB)` returned a real heap Button with a valid
   in-image vtable (0x7ff72e083600). No crash. So injected code CAN drive the
   game's UI factories.

Iteration loop (fast, no manual inject): close game -> rebuild tpf2_menu.dll ->
copy to workshop out dir -> launch (proxy auto-loads) -> read tpf2_menu.log.

### The remaining step, and why it is the hard one

Insertion is `FUN_1422518f0(button, &container, &binding)` where
`container = param_4` of the main-page builder 667bc0 (so hook 667bc0 to get it).
The `binding` (667bc0 local_4d8) is an INTRICATE multi-field stack structure:
  local_420 = param_1;                          // the menu ctx
  4c0f40(&local_3e8, param_3);                   // temp from param_3
  local_4d8 = &local_3e8; ... local_4b0=&local_420; local_498=&local_408; ...
  PTR_LAB_143022900[ 64aea0(param_2+0x1e8) ](&local_4d8, param_2+0x1e8);  // CLICK handler
  63e6e0(&local_4d8, &local_420);                // finalize (writes +0x38)
  2518f0(button, &param_4, &local_4d8);          // ADD
  357910(&param_4);                              // cleanup
The click handler IS this binding (installed via the PTR-table dispatch). So
insertion and click are the SAME fragile reconstruction of a stack-based binding
whose exact field layout must be byte-correct or the UI thread crashes. This is
the genuinely uncertain part; steps 1-4 were not.

### Realistic paths from here

- **A. Grind the binding:** hook 667bc0, reconstruct local_4d8's stack layout
  field-by-field, calling the game's own 4c0f40/63e6e0/2518f0. Several
  crash-and-adjust iterations (SEH-guarded, test instance). Highest fidelity.
- **B. Vtable click override:** insert with a minimal/cloned binding just to
  RENDER, then override the button's vtable click slot to point at our thunk ->
  CreateProcess(mp_launch). Sidesteps the game's dispatch; needs the click slot.
- **C. Ship the companion window (tools/mp_menu.ps1) for real function NOW,**
  keep the native button as the mapped-but-unfinished stretch.

## Insertion map -- CORRECTED RVAs + full binding layout (2026-08-29)

RVA-truncation struck twice more; corrected. Real functions:
- add-to-page:  FUN_1422518f0  (RVA 0x22518f0)  -- NOT 0x2518f0 (a vector move)
- attach:       FUN_14040b4c0  (RVA 0x40b4c0)   2518f0 -> 40b4c0(button+0x450, container, handler)
- cleanup:      FUN_142357910  (RVA 0x2357910)
- seed:         FUN_1404c0f40  (RVA 0x4c0f40)
- finalize:     FUN_14063e6e0  (RVA 0x63e6e0)   writes the handler to binding+0x38
- action ctx:   FUN_14221c930  (0x221c930)  } proven working in the widget probe
- button:       FUN_1407c5d30  (0x7c5d30)   }

Hook point for insertion: the main-page builder FUN_140667bc0 (0x667bc0),
prologue `48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 ...` -> STEAL 14
(mov rax,rsp + 7 pushes; rax-relative lea follows, position-independent).
rcx/rdx/r8/r9 = param_1..param_4; param_4 = the page container, param_1 = menu
ctx, param_3 = seed source.

Binding cluster in 667bc0 (base = &local_4d8; suffixes are -rbp offsets, so
fields are 8 apart), from lines 552-575:
  local_420 = param_1
  4c0f40(&local_3e8, param_3)
  +0x00 &local_3e8   +0x08 &local_430   +0x10 &local_408   +0x18 (0)
  +0x20 param_4(container)   +0x28 &local_420   +0x38 <- 63e6e0 writes handler
  +0x40 &local_408   +0x48 &local_420
  [skip line 568 PTR_LAB_143022900[64aea0(param_2+0x1e8)](&binding,param_2+0x1e8)
   -- that installs the real page-transition click; omit for a render-only test]
  63e6e0(&binding, &{param_1})
  2518f0(button, &{param_4}, &binding)
  357910(&{param_4})

STILL NEEDED before a clean attempt: sizeof(local_3e8) that 4c0f40 fills, and
local_430's role -- without them the cross-pointers are guesses. This is the
crash-grind boundary: everything ABOVE (build the button) is proven; the binding
reconstruction is the real remaining reverse-engineering + iteration.

For the CLICK, once inserted: prefer overriding the button's vtable click slot
to a thunk (CreateProcess mp_launch.ps1) over reproducing the game's dispatch.

## ARCHITECTURAL WALL -- the main menu is DATA-MODEL DRIVEN (2026-08-29)

Decompiling the real insertion chain to the bottom revealed WHY a hand-built
widget cannot simply be inserted:

- `4c0f40` allocates a `_Ref_count_obj<ModRep_const_>` (0x178 B) -- the binding
  embeds a **shared_ptr<ModRep>**. Menu entries are backed by ModRep DATA
  objects, not free widgets.
- `40b4c0` (what `2518f0` calls) is a **boost::signals2::signal<void()>**
  connection: `button+0x450` is the click SIGNAL; 40b4c0 connects a slot (the
  binding's boost::function at +0x38). It does the CLICK wiring + data binding.
- `2518f0` does NOT visually parent the button to the page. There is no simple
  "addChildWidget(container, button)" here -- the page's entries are generated
  FROM the ModRep list, and 7c5d30's ctx (from 221c930) is a resource/data
  context, not a layout parent.

Conclusion: injecting a standalone Button into the live tree is fighting the
architecture. The button object builds fine (proven), but it has no seat in a
data-driven list, so it will not render or lay out. A truly native entry would
mean adding to the menu's ModRep data model -- a separate, larger RE effort of
its own.

### Realistic native-in-menu paths, revised

1. **Overlay button** (recommended, achievable): our menu DLL already runs at
   the menu with the CreatePage(2) trigger. Draw a clickable button over the
   game window (Win32 layered child window or GDI/DirectX overlay), positioned
   in the menu, that CreateProcess's mp_launch.ps1. Visible in-menu, no
   widget-tree surgery, no data-model fight. NOT crash-prone.
2. **ModRep data-model entry** (deep): reverse the menu's entry list + how
   CreatePage(2) enumerates ModReps, and add our own entry. Highest fidelity,
   large uncertain effort, entangled with the mod/save data model.

What is BANKED regardless: startup injection, the CreatePage(2) trigger, game
std::string + widget construction from injected code -- all proven and reusable.

## Menu button text (deferred 2026-08-29): swapchain usage flag

TRANSFER_DST=1 -> GDI bitmap -> vkCmdCopyImage per frame. =0 -> font-atlas pipeline.
