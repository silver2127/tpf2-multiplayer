# dev b5dade06 revisit: the company-window rename, settled live

Supersedes the "Company-window rename: investigated, not enabled" section of
[DEV_B5DADE06.md](DEV_B5DADE06.md). The rename now ships
(`native/linux/src/slice/slice_lines.cpp`, case `kName`).

## What the static record left open

> A company-only capture also needs a verified way to distinguish this player
> entity before shipping, without enabling the other skipOrigin name paths.

and, from the integration record, the cancel/replay lifetime at
`CommandList::Add` and the originating entity.

## The factory, watched in the running game

The game was driven through the mod's own `EVAL` inject channel -- the one
`tools/sandbox/people_probe.py` uses -- which runs Lua inside the loaded world:

```
EVAL local p = api.engine.util.getPlayer()
     api.cmd.sendCommand(api.cmd.make.setName(p, "MP Rename Probe"))
```

with a breakpoint at `0x15ee705` (inside `SetName`, past the slice's 14-byte
steal, at the `cmp edx,-1` the static note had already identified):

```
SETNAME #1 rdi(result)=0x73320a7e94b0 rsi=0x733222f3c530 entity(edx)=19427
          rcx(string)=0x73320a7e94f0 name='MP Rename Probe'
   rsi as engine -> type indices {'Name': 19, 'Player': 18, 'PlayerOwned': 52}
   entity 19427 components: [(18, 0), (19, 0), (20, 0), (21, 0), (9, 1)]
   has Player=True  has PlayerOwned=False
   #1 FactoryDispatch  #2 FactoryDetour<29>   (tpf2_slice.so)
```

That settles three things at once:

1. **`rsi` really is the ecs engine.** The static note could only say "saves
   rsi (engine)" from `0x15ee6e8`. Here the same engine-relative type-index
   lookup the icon tint uses (`0x9e3d50(rsi+0x48, &type_info*)`) returns the
   *same* indices that the HUD path returned from its own engine pointer.
2. **`rdx` is the renamed entity** and **`rcx` a readable libstdc++
   `std::string`** with the new name.
3. **A company is distinguishable natively.** Entity 19427 -- the one
   `api.engine.util.getPlayer()` returned, named "ComradeSilver Transport" --
   carries a `Player` component (type 18) and no `PlayerOwned`. A vehicle, a
   line or a station never carries `Player`. That is the "verified way to
   distinguish this player entity" the static attempt was missing.

## Why shipping it is now safe

The merged shared Lua (`inject.lua`) turns a `VNAME` whose entity is a company
into `CMNAME`, and `CMNAME` renames that company on **every** peer, the
originator included. The Linux refusal existed because unchanged 0.4.22 Lua set
`skipOrigin=1` on every `VNAME`, so a cancelled click applied only on the other
peers. That reasoning still holds for vehicles and lines, which stay blocked,
but not for a company. No new origin-replay adapter is needed, so the
`CommandList::Add` lifetime question does not arise for this record: it is an
ordinary armed-and-cancelled ship, exactly like `LDELETE` and `VLINE`, on the
`Add` path that `SLICE_CORE.md` already proves.

## What is shipped

`VNAME <entity> <percent-encoded name>`, with `native/src/slice_hook.cpp`'s
encoder reproduced byte for byte (the wire splits records on whitespace, and
`%`/`=` are escaped so a name round-trips). An empty name is not shipped: the
Lua's `VNAME` parser needs a third token.

## Still not settled live

The *player click* path (`0x14287da` -> `0x1428801`) was not exercised: this
desktop has no input automation, and the probe above came through the script
caller, which the slice routes differently (`c.script`). The factory ABI is the
same for both -- it is one function with one signature -- and the ship/arm
machinery is the one already used for the other line records, but no click has
been observed end to end, and no cross-platform rename has been run.
