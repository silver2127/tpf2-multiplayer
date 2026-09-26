# Native map preview (`UI::MapPreviewComp`) as an in-game minimap (Steam 35924)

Read-only reverse engineering, September 16, 2026. The game was not launched,
attached to or patched, and nothing under the Steam directory was modified. Executable
SHA256 `782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`. All
addresses are RVAs relative to `0x140000000`.

Labels, as in `runtime-memory-audit.md`:

- **MEASURED**: observed in a running game. **No in-game measurements were taken for
  this document.** A few claims come from shipped data files (style sheets, climate
  scripts). Those are marked **FILE**.
- **DERIVED**: read from decompiled or disassembled code. Correct if the reading is
  correct, but not observed at run time.
- **GUESS**: an inference that the code does not pin down, such as a name, a meaning
  or a performance figure.

## Answers in brief

1. `UI::MapPreviewComp` (vtable `3019868`, ctor `63a190`, 0x498 bytes) is a
   `ContentView`. It turns a `Map` struct (the map generator's output) into an RGBA
   `ImageView`, adds town and industry marker `ImageView`s on top, and shows a
   "{length} x {width} km" text. Whatever `std::function` generator it is given fills
   the `Map` on a worker thread. The component has no `TerrainToolkit` or generator
   state of its own. [DERIVED]
2. `CreateTextureData` (`63b9a0`) produces the image. It is point-sampled from the
   `Map`'s full-resolution float heightmap and is at most about 1024 px on its long
   side. Land uses a climate height ramp, a hillshade and a tree tint. Water uses a
   depth ramp. The image is uploaded as a fresh raw RGBA8 texture through
   `ImageView::SetImage(int,int,int,const vector<uint8>&,bool)` (`22b4350`) and
   `tex_util::LoadRaw` (`25a0f40`). Neither `CreateTextureAsync` nor `ReloadTexture` is
   on this path. A refresh builds a new `ImageView` and a new texture. [DERIVED]
3. Markers are child `ImageView` components in a `CFloatingLayout` over the image, at
   normalized positions with y flipped. They are not drawn into the texture. [DERIVED]
4. **Partly.** It already runs in game: all six non-menu users sit in
   `UI::CGameUI::CreateConstructionMenu` (`5a2900`), the in-game map-editor terrain
   tools. There it is fed from the live world by `map_util::MakeMap` (`360ce0`). As
   used there it does not scale. `MakeMap` materialises the whole 4 m heightmap as
   floats: 1.0 GiB at 256² tiles, 4.0 GiB at 512², and a 32-bit overflow above 724²
   tiles. The component keeps that `Map` alive and rebuilds its whole UI subtree on
   every refresh. The reusable pieces are lower down: `ImageView::SetImage` (raw),
   `IRenderContext::UpdateTexture`-like slot `0x218`, and `CTerrain::BaseGetVertices`
   (`33d130`). [DERIVED]
5. Lua can reach none of this. `MapPreviewComp`, `TextureTriangleBuffer`,
   `EmissionLayerComp` and `DebugTextureView` have no sol usertype. The Lua
   `ImageView` usertype has `new(string)` and `setImage(string, bool)` only, and both
   resolve a **file path**. No Lua entry point takes pixels or a `TextureId`. [DERIVED]
6. The cheapest native height reader is `CTerrain::BaseGetVertices(CVec2i tile)`
   (`33d130`, the ECS `TerrainTileHeightmap`, 65x65 uint16). Heights in metres are
   `v * CTerrain+0x34 + CTerrain+0x3c`. The water level is `CTerrain+0x40`. The tile
   rectangle is at `*(CTerrain+0x18)`. [DERIVED]

## 1. `UI::MapPreviewComp`

### Identity

| Item | Value | Evidence | Class |
|---|---|---|---|
| RTTI / vtable | `.?AVMapPreviewComp@UI@@`, vtable `3019868`, 40 slots | `vftables.csv`, `vtable_dump.csv` | DERIVED |
| Base class | `UI::ContentView`: vtable identical to `388b050` except slot 0 (dtor `63b780`) and slot 33 (`63bdb0`, per-frame update) | `vtable_dump.csv` | DERIVED |
| Source file | `src\Game\UI\Components\MapPreviewComp.cpp`; asserts in `63ca90` and `63b9a0` | strings `3019b10` | DERIVED |
| Object size | 0x498 (`operator new(0x498)` at `66d90f` before the ctor call at `66d941`, and in `5b71b0` and `52f1b0`) | disassembly | DERIVED |
| Constructor | `63a190(this, const terrain::ClimateRep& climates, CVec2i size, bool flag)` | decompile; the size is packed in `r8` (`66d933`) and passed to `22810d0`, which converts it to floats and calls component vslot `0xc0` (min size). `flag` goes to `22819c0` on the text box | DERIVED (meaning of `flag`: GUESS, text visibility) |
| Destructor | `63b780`: destroys the std::function at +0x458, then the impl (`63b610`) | decompile | DERIVED |

### Field layout (the parts that matter)

`MapPreviewComp` (0x498):

| Offset | Field | Evidence |
|---|---|---|
| +0x018 | core pointer (base `CComponent`). `*(core+0x60)` is the `FileSystem` used by `Update` | `63bdb0` → `5f0ef0` |
| +0x440 | `ContentView*`, the centred progress/error slot. It is also item 2 of the floating layout | ctor, `63c6a0` |
| +0x448 | `Impl*` (0xa0 bytes, below) | ctor |
| +0x450 | `const terrain::ClimateRep*` | ctor, `63bdb0` |
| +0x458 | `std::function<UI::CComponent*(const Map&)>`, the marker-overlay factory; impl pointer at +0x490 | dtor, `66d96e`, `7c5100` |

`Impl` (0xa0):

| Offset | Field | Evidence |
|---|---|---|
| +0x00 | `std::string`, the progress text | `63c4f0` |
| +0x20 | `std::function<void(const std::atomic<bool>&, UpdateProgress&, UpdatePreview&, Map*)>`, the generator (impl at +0x58) | `63c4f0`, `63a010` |
| +0x60 / +0x68 | `std::shared_ptr<Map>` (a new 0x168-byte `Map` for each generation, `63b830`) | ctor, `63b830`, `63b610` |
| +0x70 | async task (0x40 bytes, ctor `31c6d0`). Byte +0 is the cancel flag, byte +2 means "result pending", +8 is the progress, and an `exception_ptr` sits at +0x30 | `63bdb0`, `31cb20`, `63c4f0` |
| +0x78 | `TextView*` ("main-menu" style) | ctor |
| +0x80 | `CFloatingLayout*`. Items 0 and 1 are `ContentView`s at (-2,-2); item 0 holds the image. Item 2 is `this+0x440` at (0.5, 0.5) | ctor, `63ca90`, `63c6a0` |
| +0x88 | `std::vector<uint8>`, the RGBA pixel buffer. **It is kept after upload** | `63bdb0` passes `impl+0x88` to `Update` |

### Who creates it and how it is fed

| Creator | Call site | Feeds it with | Class |
|---|---|---|---|
| `UI::CMenuUI::CreatePageNewGame` (`66c2b0`) | `66d941`: `MapPreviewComp(*(CMenuUI+0x590), {512,512}, true)`, then the marker factory `7c5100(false,false)` is moved into +0x458 | `65b620` ("Generating map...") calls `63c4f0` with the New Game generator lambda (`644b30` → `63f200`, `do_call` `67d8c0` → `6568b0` → `35f480`, which reads `TerrainGeneratorDesc` and reports `UpdateProgress`) | DERIVED (roles of the individual combo boxes: GUESS) |
| `UI::ExportMapComp` (`52f1b0`) | `52fb3b`, size `DAT_144133390` = (512,512) in `.data` | `5315d0` ("Generating preview..."): `MakeMap(out, ?, ?, terrain, engine, heightmap=1, towns=0, industries=0, trees=0)` at `53163f`, then a generator that copies the ready `Map` (`52e9c0`) | DERIVED |
| `GenerateHeightmapComp` (`5b71b0`) | `5b7334` | lambda `5ba740` ("Generating map...") | DERIVED |
| `GenerateIndustriesComp` (`5bef50`) | `5bf15c` | lambda `5c11c0` ("Generating industries...") | DERIVED |
| `GenerateStreetsComp` (`5c4580`) | `5c4ce7` | `5c76e0` ("Generating streets...") | DERIVED |
| `GenerateTownsComp` (`5cbc80`) | `5cbe9a` | `5ce260` ("Generating towns...") | DERIVED |
| `ImportHeightmapComp` (`5e79c0`) | `5e7fed` | `5e95f0` ("Reading heightmap...") | DERIVED |

The six map-editor components are constructed only by
`UI::CGameUI::CreateConstructionMenu` (`5a2900`, called from `CGameUI::CreateUI`
`569f00` at `56e8ca`). They are gated on CGameUI bytes `+0x50d`/`+0x50e`/`+0x50f`;
`ExportMapComp` needs `+0x50f`. [DERIVED; the flags' meaning (map-editor capabilities)
is a GUESS.] The in-game style sheet also styles the component:
`construction-menu.lua` has `!construct-editor-menu MapPreviewComp` (350x350) and
`!construct-menu MapPreviewComp` (600x600) [FILE].

**Public entry point.** `63c4f0(this, const std::function<generator>&, const
std::string& text)` (name GUESS: `StartGeneration`). It stores the generator and the
text. If no task is running, `63c6a0` resets the `Map` (`63b830`), places a progress
bar in layout item 2 and starts the task. The task binds the generator to the `Map*`
(`63a010`). If a task is already running, it sets the cancel flag and the restart
byte. [DERIVED]

**Per-frame slot 33 (`63bdb0`).** It returns at once when `impl+0x70` is null. When
the task has finished and nothing is pending, it calls `Update` (`63ca90`) on the UI
thread and frees the task. [DERIVED]

### `Map` (0x168; ctor `35bcb0`, filled by `MakeMap` `360ce0`)

| Offset | Field | Evidence | Class |
|---|---|---|---|
| +0x00 / +0x04 | int tiles X / Y | `63ca90`, `360ce0` (from `CTerrain` `33d540`) | DERIVED |
| +0x08 | int `levels` (base; samples per tile = `1<<levels`) | `33d270` → `CTerrain+0x28` | DERIVED |
| +0x0c | `CVec3f resolution`: x and y are metres per sample, z is the height scale | `33d280` → `CTerrain+0x2c..+0x37` | DERIVED |
| +0x18 | float height offset | `33d560` → `CTerrain+0x3c` | DERIVED |
| +0x1c | float water level (passed in `xmm3` to `CreateTextureData`, `63d483`) | `33d7f0` → `CTerrain+0x40` | DERIVED |
| +0x20 | `std::vector<float> heightmap`, `((X<<levels)+1) * ((Y<<levels)+1)` metres, row = y | `360ce0`, `35d3e0` | DERIVED |
| +0x90 | `std::vector<MapTree>` (16 B: int id, CVec3f pos) | `63ca90` tree loop | DERIVED |
| +0x118 | towns, 0x80 B each: +0x00 `CVec2f pos`, +0x08 name, +0x78 int existing-entity or -1 | `7c4560`, `35cf20` | DERIVED |
| +0x130 | industries, 0x38 B each: +0x00 `CVec2f pos`, +0x10 name, +0x30 int existing or -1 | `7c42b0` | DERIVED |
| +0x148 | `std::string` climate id | `63ca90` (`ResTypeRep<ClimateDesc>` lookup) | DERIVED |

The two 0x40-byte objects at +0x38 and +0xa8 and the vectors at +0x78, +0xe8 and
+0x100 were not identified (GUESS: assets and generator-internal lists).

## 2. The texture

### Size [DERIVED, `63ca90`]

```
hmW = (X << levels) + 1                 hmH = (Y << levels) + 1
pw  = 1 << ceil(log2(X << levels))      ph  = 1 << ceil(log2(Y << levels))
fact = max(pw,ph) <= 1024 ? 1 : floor(max(pw,ph) / 1024 + 0.5)   // assert texSizeFact > 0
texW = pw / fact                        texH = ph / fact
UV max = (hmW / (texW*fact), hmH / (texH*fact))   -> ImageView::SetCustomTextureCoordinates (22b4300)
```

`fact` is also the **sampling stride**: texel (x,y) reads heightmap sample
(`fact*x`, `fact*y`) with no filtering.

| Map (tiles) | levels | heightmap samples | texture | stride | metres per texel |
|---|---|---:|---|---:|---:|
| 114x228 | 6 | 7,297 x 14,593 | 512 x 1024 | 16 | 64 |
| 256x256 | 6 | 16,385² | 1024 x 1024 | 16 | 64 |
| 512x512 | 6 | 32,769² | 1024 x 1024 | 32 | 128 |

(DERIVED, assuming levels = 6 and resolution 4 m, as in `runtime-memory-audit.md`.)

### `CreateTextureData` (`63b9a0`)

Full signature (string `30199b0`):
`CreateTextureData(int hmW, int hmH, const CVec3f& res, float waterLevel, const
vector<float>& heights, int texW, int texH, int fact, const
terrain::PreviewColoring& pc, const std::function<CVec3f(float h,int x,int y)>&
colorFn, vector<uint8>& out, int nChannels)`, with the assert `nChannels == 4`.

The row loop is `ThreadPool::LoopImpl` (`639420`), one row per item, with chunks of
`texH/64` rows. The body is `63aa90`. The UI thread blocks until the loop finishes.
[DERIVED]

Before the call, `Update` zeroes `out` and adds 2 to byte R of the texel under every
`MapTree` (`63ca90`). Per texel with `h = heights[hmW*fact*y + fact*x]` [DERIVED]:

- **Water (`h < waterLevel`):**
  `t = clamp((h - wl) / (-100 - wl), 0, 1)`,
  `rgb = lerp(pc.waterColor0, pc.waterColor1, t)`. No shading.
- **Land:**
  - Normal from central differences on immediate neighbours (±1 sample, not ±`fact`):
    `n = normalize(-(h[x+1]-h[x-1])*(0.5/res.x)*2, -(h[y+1]-h[y-1])*(0.5/res.y)*2, 1)`.
  - Sun term `s = max(0, n·(-1,1,1)/√3)`.
  - Ambient term `a = (3*n.z + Σ max(0, n·d_i)) / (3 + Σ d_i.z)` over 24 hemisphere
    directions (3 rings at `θ=(r+0.5)/3·π/2`, 8 azimuths).
  - Base colour `c = colorFn(h, x, y)`.
  - Tree tint from the pre-accumulated byte `k`: `t = max(0,k-0.75)*0.75/(k+1)`.
  - `rgb = lerp(c, pc.treeColor, t) * (a*pc.ambientColor + s*pc.sunColor) * 0.5`.
- Channels: R, G, B = `floor(v*255+0.5)`, A = 255. Format RGBA8 (4 channels).

### `terrain::PreviewColoring` (climate `mapColoring`) [DERIVED from reader `36f090`, default ctor `36d810`]

| Offset | Lua key | Default |
|---|---|---|
| +0x00..+0x38 | `std::variant<LevelTex, vector<LevelColor>>`, index byte at +0x38: `texture = {levels, fileName}` (0) or `levels = {{color, height}}` (1) | index 1: (93,112,66)@0 m, (64,78,50)@100 m, (242,235,220)@550 m |
| +0x40 | `waterColor0` | (100,135,158) |
| +0x4c | `waterColor1` | (60,80,91) |
| +0x58 | `ambientColor` | (204,230,255) |
| +0x64 | `sunColor` | (255,255,204) |
| +0x70 | `treeColor` | (24,36,16) |

- The `levels` colour function (`63e1d0`) binary-searches the height and interpolates
  linearly, clamping at both ends [DERIVED].
- The `texture` variant loads `res/textures/<fileName>` (must be `.tga`) and
  `res/textures/terrain/noise.tga`, and samples through `63a800` [DERIVED, sampler not
  decompiled].
- Shipped climates [FILE]: `temperate.clima.lua` sets only `ambientColor`/`sunColor`
  and so gets the default ramp. `dry.clima.lua` and `tropical.clima.lua` define
  `levels`, and `tropical` also defines water colours.

### Upload, and updating after creation [DERIVED]

1. `Update` allocates an `ImageView` (0x488) and calls the raw-pixel ctor
   `22b2f90(this, 4, texW, texH, const vector<uint8>&, CVec2i frames=(-1,-1), int
   numFrames=-1)` (`63d84a`).
2. The ctor calls `ImageView::SetImage(int nChannels, int w, int h, const
   vector<uint8>&, bool validate)` (`22b4350`, assert `size == 4 || size == 3`):
   - It releases any previous texture (`22b4240`).
   - It builds a `TextureCreateInfo` (`22b4160`, `25a0c60`).
   - It calls `tex_util::LoadRaw(IRenderContext&, TextureFormat (RGB_8=2, RGBA_8=3),
     w, h, const uint8*, const TextureCreateInfo&)` (`25a0f40`). That is
     `IRenderContext` vslot `0x1f8` = `CreateRawTexture(const void*, TextureFormat,
     CVec2i, const TextureCreateInfo&)`: GL `257c180`, Vulkan `25d29c0`. Vulkan
     `vulkan_util::LoadRawTexture` (`25f3f00`) runs synchronously, and only the data
     pointer (`vector.begin`) is read.
   - It stores a 0x14-byte `Texture {TextureId id; int 0; int w; int h; int
     channels}` at `ImageView+0x440`.
   - The render context comes from `engine::GetGlobalGLContext()` (`daf90`, global
     `0x4331bb8`), field `+0x38`.
3. `SetCustomTextureCoordinates` (`22b4300`), the style name `map-preview-image`,
   and `ContentView::SetContent` (`2286020`) into layout item 0.
4. **After creation:**
   - `MapPreviewComp` never updates the texture in place. Each generation creates a
     new `ImageView`.
   - Calling `ImageView::SetImage` again destroys and recreates the texture
     (`22b4240` → `25a1700` → vslot `0x200`, destroy a `vector<TextureId>`).
   - The render context does have an in-place sub-rectangle update: vslot **`0x218`**.
     GL `25840d0` does `glPixelStorei(UNPACK_ALIGNMENT,1)`, `glBindTexture`, then
     `glTexSubImage2D(GL_TEXTURE_2D, 0, off.x, off.y, size.x, size.y, <stored format>,
     <stored type>, data)`. Vulkan `25dd130` does a staging upload through `25f6440`
     (`CmdBegin`, `CmdLayoutTransition`, `CmdImageCopy`).
   - The engine uses it in game for `engine::ColorMap::UpdateSubData` (`d8760`),
     reached from `UI::EmissionLayerComp::UpdateTexture` (`52e050`). Call shape:
     `rc->vtbl[0x218](rc, TextureId, CVec2i offset (r8), CVec2i size (r9), const
     void* data ([rsp+0x20]))`. The pixel layout must match the texture's creation
     format.
   - `OpenGLRenderContext::ReloadTexture` (vslot `0x208`, GL `2583870`) takes a
     `unique_ptr<gli::texture>` and checks `textureData.reloadFn`. It serves
     file-backed and streaming textures and is not needed here (GUESS).
     `CreateTextureAsync` is not on this path.

## 3. Markers [DERIVED]

- The `std::function<CComponent*(const Map&)>` at +0x458 is
  `_Func_impl<lambda_ea347651...>` (factory `7c5100(bool existingTowns, bool
  existingIndustries)`, `do_call` `7c5170` → `7c4bb0`).
- `7c4bb0` builds one `CFloatingLayout` (`22ae540`). It adds
  `IndustryMarkers(map+0x130)` (`7c42b0`) and `TownMarkers(map+0x118)` (`7c4560`), each
  its own `CFloatingLayout`.
- Each marker is `new ImageView("ui/icons/main-menu/map_town.tga" |
  "map_industry.tga")` (path ctor `22b30e0`):
  - style name `MapPreviewComp::TownMarker` / `::IndustryMarker`;
  - tooltip from the name (component vslot `0xd8`);
  - style class `existing` when the flag is set and the entity id is ≥ 0;
  - `CFloatingLayout::AddItem(marker, u, v)` (`22ae690`).
- Position mapping, lambda `do_call` `5c9a90` (COMDAT-shared):

```
origin = (-(X/2)*(1<<L)*res.x, -(Y/2)*(1<<L)*res.y)      // integer X/2
inv    = (1/((X*(1<<L)+1)*res.x), 1/((Y*(1<<L)+1)*res.y))
u = clamp((pos.x - origin.x) * inv.x, 0, 1)
v = clamp((-pos.y - origin.y) * inv.y, 0, 1)             // y flipped: v=0 is north
```

- `Update` places the marker layout over the image through `ImageView` vslot `0xb0`
  (a child layout).
- `ImageView::DoRender` (`22b3860`) pairs texcoord min with the rectangle's
  bottom-left, so texture row 0 (minimum world y) is drawn at the bottom. That is
  consistent with the flipped v.
- Marker colours [FILE `main-menu.lua`]: town (255,255,255,200), industry
  (220,160,0,200), and a grey or brown variant for `!existing`.

## 4. Gameplay feasibility

### What works in game already [DERIVED]

- `MapPreviewComp`, raw-texture `ImageView`s and marker components are created on the
  UI thread during a game session, by the map-editor tools listed in section 1.
- The live world reaches it through `map_util::MakeMap` (`360ce0`):
  `MakeMap(Map* out, ?, ?, const CTerrain* terrain, const ecs::Engine* engine, bool
  heightmap, bool towns, bool industries, bool trees)`.
- The editor gets `terrain` and `engine` from `UI::TerrainPtr` / `UI::EnginePtr`
  (`8bb820` / `8b9e60`). Both call `8bb7f0`: `acc = *ptr; state =
  acc->vtbl[1](acc)`, then `CTerrain* = *(state+0x20)` and `ecs::Engine* =
  *(state+0x28)`.
- In `CGameUI` the accessor is `*(CGameUI+0x450)` (`5a2900` passes `param_1[0x8a]`).
  The accessor class was not identified.
- The in-game `ClimateRep` is `*(*(*(CGameUI+0x448)+0x150)+0xd8)` (`5a2900` → `5b71b0`
  argument).

### Why it does not scale as used

| Cost | 114x228 | 256² | 512² | Evidence | Class |
|---|---:|---:|---:|---|---|
| `MakeMap` float heightmap `(64X+1)(64Y+1)*4` | 406 MiB | 1.00 GiB | 4.00 GiB | `360ce0`, `35d3e0` | DERIVED |
| ...kept by the component (`shared_ptr<Map>` at impl+0x60) until the next generation or destruction | same | same | same | `63b830`, `63b610` | DERIVED |
| RGBA pixel buffer kept at impl+0x88 | 2 MB | 4 MB | 4 MB | `63bdb0`, `63ca90` | DERIVED |
| Heightmap pass: 16- or 32-sample stride, one full-map pass per refresh | | | | `63aa90` | DERIVED |
| Per refresh: a new `ImageView` + texture + every marker `ImageView` | | | | `63ca90` | DERIVED |

Further points [DERIVED unless marked]:

- **The sample count is 32-bit.** `MakeMap` computes it with `imul edx, eax; movsxd`
  (`360ec6`), so any map with `(64X+1)(64Y+1) > 2^31-1` (square maps above 724² tiles)
  gets a wrapped, undersized vector. The fill lambda also indexes with int arithmetic.
  GUESS: heap corruption.
- **Side finding.** `industry_util::CreateAndAddIndustry` (`92c1d0`) calls `MakeMap`
  with `heightmap=1, towns=1, industries=1` at `92c295` (bytes at `92c26d..92c281`).
  If that path runs when an industry spawns during play (GUESS; its caller `152e90` is
  a `GameSim.cpp` "Connect industry" lambda), every industry creation on a 512² map
  allocates a transient 4.0 GiB float heightmap. The spawn-decision lambda `152ab0`
  asks for industries only. **Verify before trusting.** It matters for bigmap
  regardless of the minimap.
- It has no camera rectangle, no roads, rails or water meshes, and no click handling.
  The layout is the New Game widget: text, a progress bar during generation, and a
  "Map generation failed" error slot (`63c0b0`).

### Hard dependencies

| Dependency | New-Game-only? | Notes |
|---|---|---|
| `terrain::ClimateRep&` | No: `CMenuUI+0x590` in the menu, `GameRes+0xd8` in game | only for `PreviewColoring` |
| `Map` producer (`std::function`) | No | any function that fills `Map*`. `TerrainToolkit` / `ScriptGenerator` live only in the New Game lambda (`67d8c0` → `6568b0` → `35f480`), not in the component |
| `FileSystem` (`core+0x60`) | No | only for the `texture` colouring variant |
| Style names `map-preview-image`, marker styles | FILE: marker styles live in `main-menu.lua`; `construction-menu.lua` styles the component | GUESS: whether `main-menu.lua` rules apply in game |
| Global `ThreadPool` (`23827c0`) | No | row loop and async task |

### Reusable lower-level pieces

| Piece | RVA | Use |
|---|---|---|
| `ImageView` raw ctor | `22b2f90` | native-owned image component |
| `ImageView::SetImage(raw)` | `22b4350` | (re)create the texture from RGBA bytes on an existing `ImageView` |
| `ImageView::SetCustomTextureCoordinates` | `22b4300` | crop padding |
| `ImageView::SetImage(string)` | `22b4610` | the only method Lua reaches; detour target |
| `Texture` struct at `ImageView+0x440` | | `TextureId` at +0 |
| `GetGlobalGLContext` global | `0x4331bb8` (+0x38 = `IRenderContext*`) | render context |
| `IRenderContext` vslot `0x218` | GL `25840d0`, VK `25dd130` | in-place sub-rectangle upload |
| `CreateTextureData` | `63b9a0` | reference algorithm. Callable in principle, but it needs game-ABI `std::function` and `vector` objects and a pre-sized buffer. Reimplementing it (section 2) is simpler |
| `map_util::MakeMap` with `heightmap=false` | `360ce0` | towns and industries as `Map` records |
| `UI::DebugTextureView` | ctor `22a88e0` (0x458), `TextureId` at +0x440 | draws any `TextureId`, but through a debug-only draw `249a9c0` (single caller) with scale, flip and threshold fields. GUESS: may not show RGBA faithfully. `ImageView` uses the common quad `249b030` (12 callers) |

### Minimal native call sequences

**A. Raw texture behind an `ImageView`** (recommended core) [DERIVED call shapes]

Run on the UI thread, inside a call where the `ImageView` is known to be alive:

```
// create or replace
struct { uint8_t* b, *e, *c; } px = { buf, buf + W*H*4, buf + W*H*4 };   // only b is read
ImageView_SetImage_raw(iv /*rcx*/, 4, W, H, &px, true);                   // 22b4350
// in-place region update later
TextureId id = (*(Texture**)(iv + 0x440))->id;
IRenderContext* rc = *(IRenderContext**)(*(void**)(base + 0x4331bb8) + 0x38);
rc->vtbl[0x218/8](rc, id, CVec2i{x,y} /*r8*/, CVec2i{w,h} /*r9*/, rgba /*[rsp+20]*/);
```

**B. The component itself with a reduced `Map`** [DERIVED; std::function ABI GUESS]

```
p = operator_new(0x498);                                       // 2bf3a80
MapPreviewComp_ctor(p, climateRep, CVec2i{512,512}, true);     // 63a190
move marker std::function from 7c5100(tmp,false,false) into p+0x458
Map m: X,Y; levels=2; resolution=(64,64,zScale); offset; waterLevel;
       heightmap (4X+1)(4Y+1) floats sampled natively (16 MB at 512²);
       towns/industries from MakeMap(&tmp, ?, ?, terrain, engine, 0, 1, 1, 0) on the UI thread
StartGeneration(p, std::function{ copy m into Map* }, "…")     // 63c4f0
insert p into a layout (no Lua route); slot 33 does the rest
```

Keep `resolution.xy * (1<<levels)` = 256 m so the origin and marker formulas still
hold. The colouring adapts to the coarser spacing through `0.5/res`.

## 5. Lua exposure [DERIVED from sol metatable RTTI strings]

**Sol-bound UI classes** (`usertype_metatable@V<Class>@UI@@`):
- Components and layouts: `AbsoluteLayout`, `AbstractSlider`, `Button`,
  `CAbstractLayout`, `CBoxLayout`, `CComponent`, `CFloatingLayout`, `Chart`,
  `CheckBox`, `ColorChooser`, `ColorChooserButton`, `ColorPicker`, `ComboBox`,
  `ContentView`, `CProgressBar`, `CRendererComponent`, `CTextInputField`,
  `DoubleSpinBox`, `ILayout`, `ILayoutItem`, `ImageView`, `LayoutBase`,
  `LineRenderView`, `List`, `ScrollArea`, `Slider`, `Slider2`, `SpinBox`,
  `StationGroupDisplayComp`, `Table`, `TabWidget`, `TextView`, `ToggleButton`,
  `ToggleButtonGroup`, `Window`.
- Non-components: `CCore`, `CGameUI`, `CameraController`, `ViewManager`.
- Registration of `ImageView` is in `RegisterUsertypesGui1` (`18fb6e0`).

**Not bound:** `MapPreviewComp`, `TextureTriangleBuffer`, `EmissionLayerComp`,
`DebugTextureView`. No metatable string exists for any of them.

- `TextureTriangleBuffer` (`868680` `SetTexture(TextureId)`) is not a UI component. It
  owns GPU vertex and index buffers (`867270`), and its users are
  `brush_util::Brush::UpdateBrush` (`485550`) and
  `track_render_util::AddOneWayTextureTriangles`. GUESS: world-space overlays.

**Lua `ImageView` usertype** (`usertype_metatable<ImageView, …>` at `42cb270`):
- `"new"`: factory `18db550` → path ctor `22b30e0(this, path, 0, 0, -1, -1, -1)`.
- `"setImage"`: lambda `1863370` → `22b4610(this, std::string path, bool)`. That
  resolves `tex_load::ResolveTex` (`259d4c0`, empty string becomes
  `ui/missing_image.tga`) → `22b4700` → `TextureManager::AddRef(TextureDesc)`
  (`2490820`).
- Two read-only properties with 7- and 9-character names that were not resolved.
- Base classes `CComponent`, `ILayoutItem`.

The legacy `game.gui` table (`113f530`) exposes `imageView_create` (`1139200` →
`22b30e0`) and `imageView_setImage` (`112a770` → `22b4610`), which are also path-only.
**No Lua path accepts raw pixels or a `TextureId`.** The only Lua-reachable code that
touches a texture is the path string passed to `22b4610`.

`LineRenderView::DoRender` (`61a9f0`) rebuilds a two-vertex-per-line array every frame
from the line vector at +0x458. No cap was found in it or in the start of the UI line
draw `24995d0`; where the reported ~5,000-line limit comes from was not established.

## 6. Live world data for the native side

### `CTerrain` fields [DERIVED: getters `33d270`, `33d280`, `33d330`, `33d340`, `33d540`, `33d560`, `33d770`, `33d790`, `33d7f0`]

| Offset | Meaning |
|---|---|
| +0x08 | `ecs::Engine*` (used by `BaseGetVertices`) |
| +0x18 | tile grid header `{int x0, y0, W, H; cells* at +0x10}`, cells 0x28 bytes, tile entity id at cell+0 (-1 = none) |
| +0x28 | int base levels (6 → 65x65 per tile) |
| +0x2c..+0x37 | `CVec3f` base resolution (x, y metres per base sample; z height scale) |
| +0x38 | int high levels (8 → 257x257 cache) |
| +0x3c | float height offset |
| +0x40 | float **water level** (also `ITiledTerrain` slot 10, `156a30` / `34e1d0`) |
| +0x50 | component type index of `TerrainTileHeightmap` |

- Map extent: `W*256 m x H*256 m`, tile rectangle `[x0, x0+W) x [y0, y0+H)`.
- World origin as the preview uses it: `(-(W/2)*256, -(H/2)*256)` [DERIVED from
  `63ca90`/`7c4bb0`; GUESS for odd W or H, verify against a known position].

### Height readers

| Reader | What it returns | Paging interaction | Class |
|---|---|---|---|
| **`CTerrain::BaseGetVertices(CVec2i tile)`** `33d130` | `const vector<uint16>&`, 65x65 base heightmap from the ECS `TerrainTileHeightmap`. Lock-free component lookup (`engine+0x88[cti]`, dense +0x68 or paged +0x80). Asserts if the cell id is -1 | not paged by the plugin (the pager covers the 257² cache only, `terrain_pager.h`) | DERIVED |
| metres | `v * *(float*)(CTerrain+0x34) + *(float*)(CTerrain+0x3c)` (exactly what `MakeMap` does, `35d3e0`) | | DERIVED |
| `sub_terrain_util::GetHeightmap(int srcLevels, const vector<uint16>&, int dstLevels, x0, y0, x1, y1, uint16* out, stride, outX, outY)` `3ac330` | resampling to a coarser level (`3acf50`) or the same level (`30a540`) | pure CPU | DERIVED |
| 257² cache via `33d580` cell +0x08 | 1 m heights | faults the bigmap pager | DERIVED |

Water in the editor can also be `ecs::component::WaterMesh` entities
(`EngineTerrain::GetWaterMeshes` `2f7ed0`). The preview ignores them and uses the
global level only [DERIVED]. Lakes placed at other levels would need those meshes
(GUESS).

**Cost estimate** (GUESS, not measured):
- A 1024² minimap of a 512² map needs 2x2 samples per tile: 262,144
  `BaseGetVertices` calls and about 1 M sample reads, roughly 20-80 ms
  single-threaded, dominated by cache misses on 8.4 KB tile vectors.
- 2048² (4x4 per tile) is about 4 M reads.
- Only terraformed tiles need re-sampling later. Tile publication `33cd10` (already
  patched by `terrain_minmax.h`) is the natural dirty signal.

**Which `CTerrain`.** The two game states each hold a `CTerrain` (`33d240` copies the
grid, `runtime-memory-audit.md`). The UI reads through the accessor chain in section 4
(`*(CGameUI+0x450)` → vslot 1 → +0x20). Capturing a raw `CTerrain*` once risks reading
the version the simulation is writing (GUESS). Re-derive it per use.

**Getting `CGameUI`.**
- `5741d0` (`CGameUI` per-frame update) is already detoured by `tpf2_menu.dll`
  (`tpf2-multiplayer/native/src/menu_hook.cpp`, 21-byte steal). A second
  `verifyBytes`-guarded hook there would fail when both are loaded.
- Candidate instead: `CreateConstructionMenu` `5a2900`, which runs once per world with
  `this` = `CGameUI`. Its prologue `40 55 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 c0
  fa ff ff` is 20 bytes with no RIP-relative operand. [DERIVED]

Towns, industries, the camera and the street/track network: use Lua (`api.engine`).
`MakeMap(towns/industries only)` is a native alternative but is not cheaper for a few
hundred entities (GUESS).

## 7. Ranked implementation approaches (512² tiles and beyond)

### 1. Native RGBA texture behind a Lua-owned `ImageView` (magic `setImage` path). **Recommended.**

**Native side:**
- Detour `ImageView::SetImage(string)` at `22b4610`. Steal 20 bytes
  `40 53 56 57 48 81 ec 90 00 00 00 48 c7 44 24 20 fe ff ff ff`; the next
  instruction at `22b4624` is RIP-relative. It has 49 calling functions (70 call
  sites), so the non-magic path must be one length check plus a `memcmp`.
- On a magic string, e.g. `bigmap:minimap` (15 chars or fewer, so it stays in the SSO
  buffer):
  1. Overwrite the string with a real small UI texture path using the game's
     `basic_string::assign` (`83270`), and call the original. That keeps by-value
     string ownership stock.
  2. Call `ImageView::SetImage(raw)` `22b4350` with the plugin's RGBA buffer.
- On `bigmap:minimap:update`, push dirty rectangles through `IRenderContext` vslot
  `0x218` using `*(ImageView+0x440)->id`.
- Heights come from `CTerrain::BaseGetVertices` `33d130` through the accessor chain.
  Colouring reimplements section 2: height ramp from the climate, water depth ramp,
  hillshade.
- Roads and rails can be rasterized into the same texture (no line cap) once their
  data reaches native code (see risks).

**Lua side:**
- Window, `api.gui.comp.ImageView.new("<any real icon>")`, then
  `setImage("bigmap:minimap", false)` on open and a timed `bigmap:minimap:update`.
- Town and industry markers and the camera rectangle as child components in a
  `FloatingLayout` or `AbsoluteLayout`.
- Click → world position → camera move.
- Refresh cadence is Lua's choice. Every native texture write happens synchronously
  inside that Lua call, on a live `ImageView`.

**Risk:** medium-low. Raw-texture `ImageView`s created on the UI thread have in-game
precedent (the map editor).

**What could break:**
- A game update moves `22b4610`, `22b4350`, `ImageView+0x440`, vslot `0x218` or
  `0x4331bb8`. `verifyBytes` catches code, not field offsets.
- Vulkan in-place updates may differ from GL (unmeasured); the fallback is a full
  `SetImage` at about 4-16 MB per refresh.
- The texture dies with the `ImageView`, so native code must never cache the
  `TextureId`.
- A wrong `CTerrain` (sim-side) read.
- No native road/rail reader exists yet: needs ECS `BaseEdge`/`BaseNode` layouts or a
  Lua→file channel.

### 2. The same texture in a native-owned UI

- `ImageView` raw ctor `22b2f90` plus native window and layout insertion into
  `CGameUI`, with native markers and input.
- Lua does nothing.
- **Risk:** medium-high. The window, layout, input and lifetime APIs were not
  researched.
- **What could break:** UI tree ownership (double free when the game tears the UI
  down), style sheets, input focus.

### 3. `MapPreviewComp` with a reduced `Map`

Sequence 4.B:
- `levels=2`, resolution 64 m, 16 MB of floats at 512².
- `MakeMap(towns, industries)` only, run on the UI thread before `StartGeneration`
  `63c4f0`.
- The engine's own colouring and markers are reused. Hosting is still native, because
  Lua cannot reference the component.

**Risk:** medium.

**What could break:**
- A game-ABI `std::function` must be built in the plugin (layout stable since
  VS2015 — GUESS).
- Every refresh rebuilds the `ImageView`, texture and markers and flashes a progress
  bar.
- The `Map` plus a 4 MB pixel copy stay resident.
- At `levels=2` the hillshade neighbour step (one sample, 64 m) equals the sampling
  stride, so shading is not aliased the way it is at `levels=6` (4 m neighbours
  under a 128 m stride). There is still no camera overlay, roads or in-place updates.
- Marker styles may not apply in game (FILE vs GUESS).

### 4. File-backed texture for a plain Lua `ImageView`

- Native writes a TGA into a mod `res/textures` folder; Lua calls `setImage(path)`.
- **Risk:** medium.
- **What could break:**
  - `TextureManager::AddRef` caches by `TextureDesc` (`2490820`), so a refresh needs a
    new file name each time, and old textures accumulate until released.
  - `ReloadTexture` needs a `reloadFn` (GL `2583870`) and has no Lua route.
  - Disk I/O of 4 MB or more per refresh; stale images.

### 5. `MapPreviewComp` fed like the map editor (`MakeMap` with `heightmap=true`). **Rejected.**

4.0 GiB of floats at 512², kept by the component; a 32-bit sample count above 724²
tiles; full UI rebuild per refresh [DERIVED].

### 6. A second top-down `CRendererComponent`. **Rejected (GUESS).**

It renders the 3D scene again, with far-plane and LOD limits over 131 km and a large
frame cost. It is not researched here.

## Open questions and how to check them read-only

1. **Vulkan in-place update.** Does vslot `0x218` on an `ImageView`-owned raw texture
   display correctly? It needs an in-game test build; code reading cannot settle it.
2. **The industry-spawn `MakeMap` (`92c1d0`).** Log calls and `(X,Y)` on a 256² save.
   A working-set spike of about 1 GiB per industry creation would confirm it.
3. **World origin for odd tile counts.** Compare a known town position from
   `api.engine` with the formula in section 3.
4. **The accessor class behind `*(CGameUI+0x450)`.** Confirm that its vslot 1 returns
   the UI-side (read) game state.

## Reproduction

Decompiles are in the session scratchpad `mm\b1`..`mm\b6`, each with its `tN.txt`
target list. They were generated with `tpf2-multiplayer/tools/ghidra/run.ps1
DecompileTargets.java <targets> <outdir> 240 -ProjDir <private clone of
C:\tools\ghidra_proj>`. Vtable slots, prologues and float constants were read from
the executable with `pefile` + `capstone` (read-only). String and call-edge lookups
used `C:\tools\ghidra_out\{strings,funcsig,func2src,call_edges,vftables,vtable_dump}.csv`.
