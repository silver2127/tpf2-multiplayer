# Backlog revisit, 2026-09-17: the HUD company tint and the company rename

Not a new Windows integration. Windows `dev` is merged up to
`5172156d78`, there is no merge in progress, and no Windows file changed.
This records what the earlier, static-only runs had listed under "Not ported"
and what a session with the game actually running settled.

The lab was used for the first time in this series: the native actor ran, saves
were loaded, breakpoints were taken in the running engine, and the result was
looked at on screen. Everything below that says "live" was observed, not
inferred.

## Merged

Nothing. `git status` was clean at the start and no Windows commit is involved.

## Ported

### 1. The company wash on the HUD station/depot icons

Closes the native half of Windows `4e0ec0f` / `db8a477` / `daca5b9` /
`c4c1e86` / `093dfff` / `1fef2d5` for Linux -- the items whose records are
[d6db920f](UPSTREAM_dev_d6db920f.md), [50d7588b](UPSTREAM_dev_50d7588b.md),
[66c870cf](UPSTREAM_dev_66c870cf.md), [5fb7aea2](UPSTREAM_dev_5fb7aea2.md),
[61578d27](UPSTREAM_dev_61578d27.md), [db8a4776](UPSTREAM_dev_db8a4776.md),
[8e31f1e0](UPSTREAM_dev_8e31f1e0.md), [a658fc11](UPSTREAM_dev_a658fc11.md),
[59bb258a](UPSTREAM_dev_59bb258a.md) and [23419163](UPSTREAM_dev_23419163.md).

New: `native/linux/src/slice/ecs_linux.{h,cpp}` (the engine's component walk,
without the asserting accessor), `company_tint_linux.{h,cpp}`,
`ecs_checks_linux.h`, `company_tint_checks_linux.h`.

The Linux mechanism is not a translation of the Windows one:

* The class goes on the icon element through the game's own
  `addStyleClass 0x30550d0`, by redirecting the eleven `StationItem` and five
  depot carrier-class calls -- the same element the game puts `train`/`road`
  on, so the mod's existing `StationItem::StationIcon!mpWinCoN` rule matches.
* The entity comes from the component lookup each path already performs:
  `0x109a8f0` at `0x10902e4` inside the `StationItem` constructor (which covers
  the DoStep build *and* the 1000 ms cargo rebuild, because both run that
  constructor) and `0x9e5590` at `0x1095525` for the depot item DoStep inlines.
  Windows needed a content-builder entry hook, a constructor prologue hook and
  a post-attach re-add; none of those are needed here.
* The owner is read with a non-asserting scan of the entity's own component
  record. On Linux the station-group entity carries `PlayerOwned` **directly**,
  which is the fact the static runs could not establish; the Windows
  group -> stations[0] walk is kept only as a fallback.

Kill switch `stationicon=0`, and `tintclass=mpCo` as on Windows. A refusal is
not folded into multiplayer readiness: the tint is cosmetic and must not take
startup with it.

Evidence, including the gdb transcripts: [DEV_D6DB920F.md](../re/linux/DEV_D6DB920F.md).

### 2. The company rename through the game's company window

Closes the Linux half of Windows `2a87bb4` ([b5dade06](UPSTREAM_dev_b5dade06.md)).
`slice_lines.cpp` no longer refuses every `SetName`: a rename whose entity
carries a `Player` component is the game's company window, and the merged
shared Lua turns that `VNAME` into `CMNAME`, which renames the company on every
peer *including* the originator. So a cancelled click is not lost, and the
origin-replay adapter the old refusal was waiting for is not needed. Vehicle
and line renames, and every `SetColor`, keep the old refusal.

The name travels percent-encoded, with `native/src/slice_hook.cpp`'s encoder
reproduced byte for byte. Evidence: [DEV_B5DADE06_LIVE.md](../re/linux/DEV_B5DADE06_LIVE.md).

## Not ported

* **The entity-window wash and the station-label tint** (the rest of Windows
  `windowcolor` / `61578d27`). The Linux bind site that all entity-view
  creators funnel through was not located, and -- decisively -- an entity
  window cannot be opened in this lab: it needs a mouse click, and this desktop
  has no input automation. Nothing would have been observable. The icon tint's
  machinery (`ecs_linux`, the class helper) is there for it when a click is.
* **The native controller (`NativeIo`), the automatic resync and the automatic
  in-world client load.** Not implemented. The session did settle three of its
  open contracts live and found that the Linux port already has more of it than
  the backlog assumed; it also found the one piece that genuinely blocks it:
  nothing in the Linux port constructs and enqueues a Command, and the
  completion has to be a libstdc++ `std::function` whose post-`Add` ownership
  was not established. Written up in
  [NATIVE_IO_LINUX.md](../re/linux/NATIVE_IO_LINUX.md).
* **Managed Workshop registration** (`90bbedc`) and the **generalized
  modular-station endpoint weld** (`station_weld.h`). Not attempted this run;
  the earlier records stand. Both need a live case the lab cannot produce
  without clicking (a Workshop subscription, a modular-station proposal).

## Live testing

See the report in the job's `meta/` directory for the full transcript list. In
short: the lab's native actor was run repeatedly, "Ordering desync Sep15" (as
"Stations") was loaded through the mod's own autoload, the icon path was
broken on in gdb, and the result was photographed. The same depot icon rendered
`(0,130,200)`, `(60,180,75)` and `(230,25,75)` as the company was set to 2, 3
and 1 -- the mod stylesheet's palette entries exactly.

Two host problems had to be worked around, neither of them in the port: this
machine's AppArmor forbids the lab's nested user namespace, and its NVIDIA
modeset device stopped accepting new clients part-way through the session.
Both are described in [DEV_D6DB920F.md](../re/linux/DEV_D6DB920F.md).

## Tests

- `tools/linux/build_native.sh`: soldier build, **46/46** CTests, glibc <= 2.31
  import check passed. New test `company_tint` (the engine walk against a
  synthetic world: the slot scan, stride 4 and 24 with the pool's own bounds,
  the flat and the paged half, direct and station-group owners, a town refused,
  the class string). `slice_commands` extended with the company-rename branch:
  a plain entity still blocked, a `Player` entity shipped as
  `VNAME 7 Coal%2050%25%3D%C3%A9`, an empty name refused.
- `python3 tools/linux/verify_company_tint_elf.py <TransportFever2>`: new.
  Build-id, 13 complete anchor spans, the 11+5 `addStyleClass` calls and the
  two context calls resolved by disassembly, no branch inside either icon
  function landing inside a redirected five-byte call, and the
  Player/PlayerOwned/StationGroup RTTI names behind the typeinfo pointers.
- `python3 tools/linux/verify_lua_release.py`: unchanged Lua, passed.
