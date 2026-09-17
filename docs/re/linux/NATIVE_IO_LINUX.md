# The native controller on Linux: what a live session settled, what it did not

The Windows `native_io.cpp` / `native_control.cpp` pair is what lets a host run
an automatic resync: pause and drain, save, share, load, hold input, and report
progress through `tpf2_native_request.txt` / `tpf2_native_event.txt`. Linux has
no counterpart, so a Linux host cannot run a resync, frozen joins are inactive,
and a Linux joiner advertises recovery 0.

This note records what the 2026-09-17 lab session settled, so a later run does
not repeat it. It does **not** describe shipped code: nothing of the controller
is implemented yet.

## Already in the Linux port (not newly found, but easy to miss)

`native/linux/src/menu_game_linux.cpp` is further along than the backlog
entries assume. It already has, byte-verified against build 35924:

| Windows `NativeIo` piece | Linux equivalent, already present |
| --- | --- |
| `UI::CMenuUI::StartSavegame` | `RVA_START_SAVEGAME = 0x113f450`, called from `LoadBody` with a real `LoadGameParams` (`0x1162170`) and `SavegameInfo` (`0xc7e520`), inside the game's own C++ runtime so a throw is caught |
| the load queued from our side | `MenuGame_RequestAutoload` + `AutoloadTick`, driven from `CMenuUI`'s per-frame update slot (`0x1140a90`, vtable `0x5a16f98`) |
| `ObserveStart` | `MenuGame_ObserveLoads` -- accepted vanilla UI loads, on the UI thread |
| `ObserveMenu` | `MenuGame_ObserveMenu`, from the verified `CreatePage` hook |
| a save on demand | `MenuGame_ForceAutosave` -- the game's **own** autosave, forced from `CGameUI`'s update (`0x100fb20`), not a named save |
| load progress | `MenuGame_LoadPercent`, from the verified `ProgressMonitor` |

## Settled in the running game, 2026-09-17

**`HasWorld()` is a single read.** With "Stations" loaded, the `.bss` slot the
port already calls `RVA_G_GAMEUI` held a live object whose vtable was exactly
the `CGameUI` vtable the port had identified statically:

```
g_gameUI (base+0x5a4fb38) -> 0x592c3b33f8a0
CGameUI vtable 0x592c358d4468 == base+0x5a11468
```

So `HasWorld()` is `*(void**)(base+0x5a4fb38) != nullptr` with a vtable check,
and the world's identity for "the world changed under a queued operation" is
that same pointer. No hook is needed for it.

**The drained test already exists.** Windows' `PauseAndDrain` decides
quiescence with `queue = [[ui+0x448]+0x160]`, `impl = [queue]`,
`impl[0]==impl[8]`. The Linux equivalent is already PROVEN in
[SLICE_CORE.md](SLICE_CORE.md): the `CommandList`'s `m_data` is `*self`,
`m_data+0x00` is the `std::vector<Command>`, `m_data+0x18` the generation byte
and `m_data+0x20` the lazily created signal; `CGame+0x158` is the CommandList
and `CGame+0x160` the game states. "Drained" is that vector's begin == end.

**Input suppression has a native path.** `SetActionsHeld` is a window-procedure
filter on Windows. On Linux the panel already installs an SDL event filter
("[panel] SDL event filter installed", `panel_linux.cpp`), which is the same
choke point: the game reads its input through SDL.

## What is still missing, and why the controller was not written

`PauseAndDrain` and `Save` both have to **construct a Command and enqueue it
with a completion callback**, which nothing in the Linux port does today -- the
slice only observes, cancels and blocks commands, it never creates one. That
needs, in Linux terms:

* the pause command's factory (Windows `0x9de9e0`); the Linux `SetGameSpeed`
  factory `0x15eb600` is hooked by the slice and is the obvious candidate, but
  its result's ownership was not traced;
* `CommandList::Add`'s ABI is documented (SLICE_CORE.md 2.4) -- but the
  completion has to be a **libstdc++ `std::function`**, an Itanium-ABI callable
  with a manager/invoker pair whose storage the engine moves from and later
  destroys, not MSVC's 64-byte `SmallFunction` with its six-entry table. The
  layout is known (`menu_linux.cpp`'s `GFunc`); what was not established is
  which side frees the functor after `Add` moves it, and what the engine does
  with it when the world is destroyed mid-command;
* the named save needs the Linux counterparts of Windows' `0x5647b0`
  (metadata), `0xbf9b40` (the thumbnail), `0x2e4e40` (context) and `0x9de0e0`
  (the save command), plus the two world fields Windows writes before and after
  (`world+0xb48`, `world+0x648`);
* the world constructor/destructor hooks that turn a started load into
  `world_ready` and a lost world into a failed operation.

None of these were guessed. `MenuGame_ForceAutosave` plus `MenuGame_NewestSave`
can stand in for a *named* save only if the lobby is willing to take "the
newest autosave" instead of a name, which is a protocol question for the shared
side, not a Linux one.

## Lab note

The whole session ran with software Vulkan (Mesa lavapipe): part-way through,
this host's NVIDIA modeset device stopped accepting new clients and every
launch blocked forever in `nvkms_open_common` opening `/dev/nvidia-modeset`.
See [DEV_D6DB920F.md](DEV_D6DB920F.md) for that and for the AppArmor
user-namespace workaround the lab needed.
