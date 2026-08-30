# M6 — title-screen menu recon

*2026-08-06.* Goal: add a **Multiplayer** entry to the title screen, opening a
page where the peer IP/port can be set before a game is loaded.

## Why this cannot be done in Lua

Game-script mods (`res/config/game_script/*.lua`) are only loaded once a save is
running. The title screen is built entirely in C++ by `UI::CMenuUI`. The Lua
that *does* run at menu time is limited to style sheets
(`res/config/style_sheet/main-menu.lua`), which can restyle existing widgets but
cannot add one.

Note the naming trap: `#mainMenuTopBar` / `MainMenuBottomBar` in
`style_sheet/game-menu.lua` are the **in-game HUD** bars, not the title screen.
The title screen is `MenuUI MainMenu` in `style_sheet/main-menu.lua`.

## Located symbols

Found with `tools/find_menu_ui.py` (same technique as M1: locate the ASCII
profiling string, find rip-relative references, resolve the enclosing function
via `.pdata`). Full output in `tools/menu_ui_map.txt`.

| VA | What | Evidence |
|---|---|---|
| `0x140667bc0` | **`UI::CMenuUI::CreatePageMain`** — builds the title-screen button list. 9855 bytes, 1 caller. | references `Free Game`, `Campaign`, `Load Game`, `Map Editor`, `Settings`, `Mod Browser`, `Credits`, `Exit`, `MainMenu`, `main-menu`, `Button` |
| `0x140663370` | `UI::CMenuUI::CreatePage` — page dispatcher. 2892 bytes, 20 callers. | references the `CreatePage(enum Page)` profiling string and `MenuUI.cpp` |
| `0x14221c930` | called after **8/8** menu labels, 175 bytes. Probably string construction / localisation. | uniform across every entry |
| `0x1407c5d30` | called after **7/8** menu labels, 363 bytes. **Button factory candidate.** | every entry except `Credits`, which is styled differently (`credits-button`) |

The per-entry pattern in `CreatePageMain` is consistent:

```
lea  rX, ["Free Game"]        ; label
call 0x14221c930              ; make string  (8/8)
call 0x1407c5d30              ; create + attach button  (7/8)
call qword ptr [rip + ...]    ; per-button click callback (indirect, unique)
```

## Proposed approach

Hook `CreatePageMain`, let the original run, then call `0x1407c5d30` once more
with our own label to append a Multiplayer entry — reusing the game's own
factory rather than constructing UI objects from scratch. RTTI is present
(`UI::Button`, `UI::CBoxLayout`, `UI::CComponent`, `UI::TextView`, `UI::Window`
in `recon/rtti_classes.txt`), so vtables are locatable if more direct
construction turns out to be necessary.

## Decoded call sequence

Correction to an earlier note: the third call after each label is *not* a click
handler, it is the `std::string` overflow path (`_Xlength_error`). Handlers are
attached much later, from saved pointers.

Each of the 8 entries is built identically (`Free Game` shown, at `0x140667cfd`):

```asm
; two empty std::strings prepared as args 2 and 3
call 0x140083270                   ; string assign("", 0)   x2

lea  rdx, ["Free Game"]
lea  rcx, [rbp+0x2f0]              ; out std::string
call 0x14221c930                   ; -> rax = out   (localise / make text)

lea  r8,  [rbp+0x370]              ; arg3 tooltip   (empty here)
lea  rdx, [rbp+0x350]              ; arg2 id/style  (empty here)
mov  rcx, rax                      ; arg1 label
call 0x1407c5d30                   ; -> rax = UI::Button*
mov  [rsp+0x68], rax               ; stashed for later
```

`0x14221c930(std::string* out, const char* literal) -> out` — text/localisation.

`0x1407c5d30(std::string* text, std::string* id, std::string* tooltip) -> Button*`
internally: builds the string `"Button"` as a type name, calls the generic
component factory `0x142324db0(out_sharedptr, text, "Button", -1)`, steals the
raw pointer out of the returned `shared_ptr`, applies `id` through
`vcall [vtbl+0xD8]`, and applies `tooltip` via `0x14227a1e0` only when
non-empty. **It does not attach the button to a layout and does not set a
handler.**

Handlers are attached later from the stashed pointers, e.g. for `Free Game` at
`0x140668918`:

```asm
lea  rdx, [rbp-0x38]
lea  rcx, [rsp+0x70]
call 0x14063e6e0                   ; -> rax = callback object
mov  r8,  rax                      ; arg3 callback
lea  rdx, [rsp+0x48]               ; arg2
mov  rcx, [rsp+0x68]               ; arg1 = the button
call 0x1422518f0                   ; ATTACH HANDLER
lea  rcx, [rsp+0x48]
call 0x142357910                   ; release temp
```

The callback is an object with a vtable — `lea rax, [0x1430229c0]; mov [rbp+0xa0], rax`
stores its vptr. Synthesising one from the DLL means allocating a struct with a
vtable of our own function pointers; the slot layout (invoke / destroy / clone)
is the main thing still unknown.

Layout assembly happens around `0x140668dd9`–`0x140668eae`, which reads the same
stashed pointers.

## Early injection — solved

`alut.dll` is a **static import** of `TransportFever2.exe` (confirmed from the
import table), so the loader maps it before the exe's entry point — well before
the menu is built. It has only 20 exports, all named, and it lives in the game
directory rather than `system32`.

`bridge/src/proxy_alut.cpp` forwards all 20 exports to `alut_real.dll` and, from
a worker thread (never inside the loader lock), loads `tpf2_bridge_mp.dll` from
the workshop `out` dir. Install/revert with `tools/install_proxy.ps1`.

Because the *same* proxy now loads into both games, identity can no longer come
from which DLL was injected where. The bridge gained `instance=auto`: it probes
the host port, and whoever finds it free becomes `a`. Verified with two
concurrent processes — `7771 free -> instance a`, `7771 taken -> instance b`.

That test also exposed a real bug: both bridges opened `tpf2_bridge.log` with
`fopen("ab")`, which on Windows is seek-then-write rather than an atomic append,
so one process's flush silently erased the other's lines. Now opened with
`FILE_APPEND_DATA`.

**Constraint this introduces:** auto-identity and the shared identity file
assume the two instances do not share one data directory. That holds for the
Sandboxie setup (B's writes land in the overlay) but would break if both
instances ran unsandboxed.

## Page enum — full map

`CreatePage(CMenuUI* this /*rcx*/, Page page /*edx*/)` at `0x140663370`
dispatches through a 17-entry jump table at `0x140663e78` (entries are 4-byte
offsets from the image base; `cmp ebx,0x10 / ja default`).

| Page | Builder | What it is |
|---|---|---|
| 0 | `0x1406614e0` | menu background |
| 1 | — | small case |
| **2** | `0x140667bc0` | **main menu** (Free Game / Campaign / Load Game / …) |
| **3, 4, 10** | `0x14066c2b0` | **new-game setup** — start year, difficulty, climate, map size |
| 5 | `0x1406641f0` | reset-to-default |
| 6 | `0x140664860` | campaign select |
| 7 | `0x14066a240` | mission start |
| **8, 11, 12** | `0x140667430` | **load page** — "Load the map", "Start the game" |
| 9 | — | small case |
| 13 | `0x140670e60` | settings |
| 14 | `0x14066ba60` | mod browser |
| 15 | — | small case |
| 16 | `0x140666440` | mission title |

Values sharing a case (3/4/10, 8/11/12) are variants the builder distinguishes
internally from the page value it still receives.

## Menu design

Requested: a **Multiplayer** entry offering *Join game*, *Host new game*,
*Host saved game*.

The important consequence of the table above is that **none of these needs a new
page**. Hosting is just the game's existing flows with a "we are hosting" flag
set, so map selection and save browsing come for free:

| Entry | Action |
|---|---|
| Host new game | set mode=host, `CreatePage(3)` — stock new-game setup |
| Host saved game | set mode=host, `CreatePage(8)` — stock save browser |
| Join game | set mode=join, pull host's save (see save transfer), `CreatePage(8)` so the player picks the downloaded `mp_host_*` world |

The Multiplayer submenu does not need a new enum value either: the handler sets
a mode flag inside our DLL and re-enters `CreatePage(2)`; the `CreatePageMain`
hook sees the flag and emits the three multiplayer entries plus *Back* instead
of the stock list. One hook, one flag, no new builder.

Connection config (peer IP/port) belongs on the Join path and must reach the
bridge before `Net_Init` — which the alut proxy now makes possible, since the
bridge starts inside the same process that owns the menu.

## Click callback — solved

RTTI on the callback vtables gives it away immediately:

```
.?AV?$_Func_impl_no_alloc@V<lambda_c8f160d0f30ef159e2c0ff1654ff6d5a>@@X$$V@std@@
```

It is not a bespoke delegate type — it is a plain **`std::function<void()>`**,
specifically MSVC's `std::_Func_impl_no_alloc`. The 16-byte object seen being
built inline in `CreatePage` (`{vptr, captured CMenuUI*}`) is just a lambda with
one pointer capture stored inline.

Vtable at `0x1430225d0`, confirmed by disassembling every slot:

| Slot | Address | Behaviour |
|---|---|---|
| 0 `_Copy(dest)` | `0x14067b1c0` | `[rdx]=vtable; [rdx+8]=[rcx+8]; return rdx` |
| 1 `_Move(dest)` | `0x14067b1c0` | same address — ICF-folded with `_Copy` |
| 2 `_Do_call()` | `0x14067e880` | loads `[rcx+8]` as `this` and invokes; takes no arguments |
| 3 `_Target_type()` | `0x140680ed0` | `lea rax,[type_info]; ret` |
| 4 `_Delete_this(bool)` | `0x1400b8790` | `if (dealloc) operator delete(this, 0x10)` |

Object layout, straight out of slot 0 and slot 4:

```cpp
struct FuncBase { const FuncVtbl* vptr; void* capture; };   // exactly 16 bytes
struct FuncVtbl {
    FuncBase*   (__fastcall* Copy)(const FuncBase*, void* dest);
    FuncBase*   (__fastcall* Move)(FuncBase*, void* dest);
    void        (__fastcall* DoCall)(FuncBase*);            // our handler
    const void* (__fastcall* TargetType)(const FuncBase*);
    void        (__fastcall* DeleteThis)(FuncBase*, bool dealloc);
};
```

We author a static vtable in the DLL, put a menu-action id in `capture`, and let
`DoCall` dispatch. Slots 3 and 4 are generic and can point straight at the
game's own implementations; slots 0 and 1 must be ours, because the game's
versions hard-code *its* vtable address into the destination.

`_Delete_this` frees with size `0x10` through the game's allocator, so any copy
the game takes must have been allocated by the game — our `Copy` should follow
the same contract as the original (write into the caller-provided `dest`) and
never hand back storage we own.

## Saving on the host — no Lua route

"Host new game" hands off to the stock new-game flow, so at that moment the
joiner has nothing to pull: the world does not exist until generation finishes.
The obvious fix — have the host auto-save right after generating — has no Lua
path: `game.interface` exposes no save function (checked the API dump).

Options, cheapest first:

1. **Host saves manually** before anyone joins. Zero work, slightly awkward UX.
2. Rely on the periodic autosave (`autosave_<name>_<date>_N.sav` files exist),
   so a joiner is at worst a few minutes stale.
3. Find the save entry point in the binary and trigger it from the bridge — the
   proper fix, and `Serializer.cpp` is already named in REPORT.md as the lead.

For a first working version, (1) with clear wording on the Join screen. "Host
saved game" does not have this problem at all, since the world already exists.

## Still open

1. **`0x1422518f0` signature** — arg2 is built at `[rsp+0x48]` and released
   right after by `0x142357910`; needs identifying before a handler can be
   attached.
2. **Layout attach** — `0x140668dd9`–`0x140668eae` needs decoding so the new
   button actually appears rather than leaking.
3. **Page rebuilds** — the menu is recreated on resolution and mod changes (see
   the `Mods changed, recreating data...` string), so the hook must re-add the
   button each time and the callback must outlive every rebuild. A static
   vtable + static callback objects in the DLL handle the lifetime half.
4. **Peer IP entry** on the Join path — text input is more UI surface than a
   button; falling back to the cfg file first is reasonable.

## Status

Recon essentially complete: button factory, page navigation, callback ABI and
save transfer are all decoded or built. Early injection is built and verified
but **not installed** (`tools/install_proxy.ps1` needs running). No UI hooked
yet — the next concrete step is `0x1422518f0` plus the layout attach.
