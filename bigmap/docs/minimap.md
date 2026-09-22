# In-game minimap (`minimap=1`)

Built September 16, 2026. Tested offline (`tools/test_minimap.py`); **not yet run
in game**. Reverse engineering: `minimap-native-preview.md`.

## Why a new one

The Workshop "Minimap" mod (3256290611) cannot be patched into working on big
maps. Four things in it are architectural:

- It finds the map edge with a linear scan that stops at 50,000 m. A 320-tile map
  is +-40,960 m and a 512-tile map +-65,536 m.
- It samples heights every 10 m from Lua: about 43 million calls at 256x256 tiles.
- It paints terrain as one UI component per cell.
- It caps roads and rails at about 5,000 lines.

## Design

| Part | Where | What it does |
|---|---|---|
| Game script | `mod/minimap/bigmap_minimap.lua`, embedded in the DLL by `tools/embed_lua.ps1`, written to `res/config/game_script/bigmap_minimap.lua` at start | Toolbar button, window, town and industry markers, camera circle, click-to-move, layer toggles, Refresh. GUI only: no `update`, `save` or `load` |
| Request | Lua: `imageView:setImage("bigmap:minimap?x0=&y0=&x1=&y1=&w=&h=&relief=&g=")` | Lua probes the map extent (binary search on `isValidCoordinate`) and owns the world-to-picture mapping, so the image, the markers and the clicks share one definition |
| Detour | `ImageView::SetImage(string)` `0x22b4610` | A 14-byte prefix check for every other image. On a token: the placeholder `ui/icons/main-menu/map_town.tga` is written into the string's own buffer (or an empty path if it does not fit), the original consumes it, then the texture is replaced |
| Terrain | `*(CGameUI+0x450)` -> `vtbl[1]` -> `+0x20` = `CTerrain`; `CGameUI` captured by a 25-byte thunk on `CreateConstructionMenu` `0x5a2900` | The thunk stores `rcx` and jumps on without touching the stack, so the function's unknown arity does not matter |
| Heights | `CTerrain::BaseGetVertices` `0x33d130` (65x65 uint16 per tile) | Not paged by the terrain pager. Empty cells are skipped before the call, which would assert |
| Colour | reimplementation of `CreateTextureData` `0x63b9a0` | Default climate ramp, water depth ramp, hillshade (`relief`), 24-direction ambient term. Shaded on up to 8 threads |
| Upload | `ImageView::SetImage(raw)` `0x22b4350`, RGBA8 | The path the engine's own map preview uses; synchronous |

Twelve sites are byte-verified before anything is hooked, including the getters
that pin every `CTerrain` offset the renderer reads. Anything that fails leaves
the game untouched and deletes the script, so no button appears without working
hooks. The script is only ever deleted if its first line is the plugin's marker.

## Offline tests (`tools/test_minimap.py`)

- The token parser, including 16 rejected forms.
- Every rendered height against an independent model of the engine layout:
  origin, packed tile argument, row = y, south in row 0, a missing tile and
  pixels outside the map. Every colour is checked to within 1 across water,
  land, outside and the multithreaded path.
- The accessor chain, including the vtable range check.
- The detour: non-tokens pass through untouched. Tokens keep their buffer and
  capacity, and the uploaded bytes equal a direct render.
- The thunk: executed with four register and two stack arguments.
- The installer against the Steam executable, plus refusal paths.
- Script sync, including never touching a foreign file.
- The Lua script under a mocked game API, plus the multiplayer repo's luacheck.

## First in-game session (September 16): what it showed

On a 192x192-tile save at a 175% UI scale (3038x1918 screen) the terrain
rendered correctly and fast (`1024x1024 px ... in 40 ms`), but the window showed
only the lower half of an oversized picture, the markers crowded its top-left
57%, and the camera circle sat off its true position. Measured from the
screenshot, the cause was two unit systems:

- `setMinimumSize` takes **UI units**, multiplied by the UI scale on screen: a
  900-unit picture was 1,576 px wide.
- `AbsoluteLayout` rect positions, `getContentRect()` and `getMousePos()` are
  **screen pixels**: markers placed at 0..900 spread over 0..900 px. The stock
  `selectortooltip.lua` mixes these three the same way.
- `AbsoluteLayout` appears to read `rect.y` as the item's **vertical centre**. A
  picture at `y = 0` showed exactly its lower half (786 of 1,576 px). The
  Workshop minimap placed full-height views at `y = height/2`. INFERRED from that
  evidence, not from code.

The script now measures the UI scale from the map container's content rect,
places everything in screen pixels, and checks the picture's measured position
once per session. If the `rect.y` rule is the other way, it rebuilds with the
other rule. Clicks use `getMousePos()` minus the picture's rect. The toolbar
icon is `ui/button/medium/terrain@2x.tga` (from `res/textures/ui/ui.zip`), not
the Workshop mod's `map@2x.tga`. The log line
`layout: UI scale S (guessed G), AbsoluteLayout y is the ...` records what was
found.

## Roads, tracks, stations, companies and industry types (September 16)

All of these are drawn into the picture natively, so a network of any size has
no line cap and no widget per edge.

- **Gathering (Lua).** When a map build's layout is confirmed, the script reads
  every `BASE_EDGE` and then every `STATION` on a 6 ms per-frame budget, showing
  "Loading network N%". For each edge it records the class (track, or a street's
  `categories[1]`: urban, country or highway; one-way types are told apart by
  name), both node positions and both Hermite tangents. For each station it
  records the type (rail, road, water or air from the construction file name),
  centre, direction and half length. Roadside stops are edge objects, placed via
  `getEdgeObject2EdgeMap`. Owners come from `PLAYER_OWNED`.
- **Transport.** The network travels once as text after the token's header:
  `P index r g b`, `E class colour x0 y0 tx0 ty0 x1 y1 tx1 ty1`, and
  `S class colour x y ux uy half`. The plugin caches it under the header's `net`
  id. `layers` (1 roads, 2 tracks, 4 stations) and `hide` (a bit mask of owner
  palette indices) re-render from the cache with a header-only request. A request
  whose id does not match the cache draws no network, so another world's network
  never shows.
- **Drawing (native).** Anti-aliased strokes along each edge's cubic Hermite
  curve, with the engine's own tangents. Draw order is roads (urban 70% blend,
  country, highway), then tracks, then stations as outlined bars.
- **Companies.** In the multiplayer mod's companies mode, owners map to company
  ids through the lobby's `mp_company_cfg.txt`. This machine's player is its
  company; the other engine players, in ascending id order, follow the roster
  order they were created in. Players that own nothing still count, so the
  mapping does not shift. Colours are the lobby chip colours (the
  `CM.cmCompanyColor` algorithm, copied). Without a roster, two or more owners
  still get distinct colours (this player first). A lone player's network keeps
  the classic track colour and shows no company legend.
- **Industry types.** Each industry construction type is listed with the icon of
  the cargo it produces and a count, and can be toggled. Markers use the same
  icon. The cargo is resolved per type before any marker is placed, in this
  order: what any industry of the type has produced
  (`game.interface.getEntity(simBuilding).itemsProduced`); the first
  `output = { CARGO = n }` table in the type's own construction file, read from
  each mod root on `package.path` (`<mod>/res/scripts/?.lua`); a built-in table of
  the 16 stock industries (their files are zipped in `construction.zip`, so they
  cannot be read); what one has taken in; else `map_industry@2x.tga`.
- **Legend.** Show toggles for towns, industries, roads, tracks, stations and the
  camera; one row per company, with its colour and a toggle; one row per
  industry type.

A real bug found offline on the way: Lua 5.2's `%d` goes through a 32-bit C
`long` on Windows and refused the network id. Ids and masks are now formatted
with `%.0f`.

## To confirm in game

The offline mock cannot settle these. Each is logged or visible:

1. **The script loads.** The stdout line `[bigmap minimap] installed` should
   appear and a map button should show on the main toolbar. This proves
   `res/config/game_script` files load without a mod entry.
2. **The picture renders.** Look for `minimap: rendered WxH px ... in N ms` in
   `tpf2mp_host.log`. The log line prints the tile rectangle and water level.
3. **Orientation.** North should be up and the picture should not be mirrored.
   Check a coast or a lake against the camera. Row 0 is assumed to be the south
   edge (`ImageView::DoRender` `0x22b3860`).
4. **Marker alignment.** Town icons should sit on the towns in the picture.
   `AbsoluteLayout` rectangles are assumed to be top-left and in UI pixels. The
   old Workshop mod placed its line view at `y = height/2`, which hints that
   `LineRenderView` may use another origin; if the camera circle is offset
   vertically, that is the cause.
5. **Click frame.** The first three map clicks log `event pos` and `screen`. If
   `event pos` is not relative to the picture, clicks will move the camera to
   the wrong place.
6. **Camera API.** `setCameraData` should move the camera. If it does not, the
   script falls back to `game.gui.setCamera` and logs when both fail.
7. **Cost.** Record the render time on a 256x256 and a 512x512 map.

8. **Network gathering time** on a large map: the `network N: ... in S s` line.
   Lua reads about five components per edge. If this is too slow on a 512x512
   network, the next step is a native ECS reader.
9. **Company mapping** in a companies-mode session: every company's tracks
   should show in its lobby chip colour, and its legend row should say the
   right company number.
10. **Cargo icons:** each industry type should show its cargo. First session
    (September 16): some markers were blank. Only industries that had produced
    something reported a cargo, markers placed before their type found one
    kept the fallback, and that fallback (`map_industry.tga`, likewise
    `map_town.tga`) is not a file; `ui.zip` ships only the `@2x` names. Now fixed
    as described above. The stdout line `industry icons: N from production,
    N from mod files, N from the stock list, N from inputs, N generic (files)`
    names any type still generic.
11. **Roadside stops** should appear as small bars on their streets.

Known limits in this version: temperate terrain colours on every climate;
industry markers capped at 4,000; a network gathered by Lua (see point 8).
