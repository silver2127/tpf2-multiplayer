# Extended New Game ratios, Steam build 35924

Implemented and built September 13, 2026. Native tests and emulation of the
original dropdown loop pass; interactive generation at the added ratios has
not yet been tested. A running process needs the new DLL on its next launch.

`max_ratio=20` enables 1:1 through 1:20, including the added 1:6 through 1:20.
The supported range is 5..20; 5 retains the stock five-row dropdown. Added
ratios are selectable with experimental map sizes either enabled or disabled.
GOG is excluded until its corresponding instructions are verified.

The existing size rows and explicit `size<S>_format<F>` cells still work.
Format index is denominator minus one: format5 is 1:6, format9 is 1:10,
format19 is 1:20. The claim capacity is now 19 sizes by 20 formats. Explicit
cells retain their existing priority and may intentionally specify a shape
different from the ratio label.

Unspecified shapes use approximately the square preset's area: short side is
`side / sqrt(ratio)` rounded to even tiles; long side is short side times the
ratio. Both axes remain even. The shape shrinks at the configured edge cap,
octree extent, or signed 32-bit heightmap element limit. The octree startup
extent check includes all offered ratios, including stock rows and square
claims with no added size rows. Very small custom edge caps also reduce the
maximum offered ratio to what a two-tile-wide strip can fit.

For a 256-tile square preset (65.536 km), under depth 13 and a 2,048-tile cap:

- 1:5: 114 x 570 tiles, 29.184 x 145.920 km.
- 1:10: 80 x 800 tiles, 20.480 x 204.800 km.
- 1:20: 58 x 1,160 tiles, 14.848 x 296.960 km.

Area varies slightly because dimensions are even tile counts. The maximum
allowed edge does not increase; higher ratios spend more of the same area
along the longer axis. These changes do not alter terrain-cache spacing or
compression, save compression, placement attempts, or octree depth.

## Engine changes

- `662930` constructs the map-format combo. At `662ac9`, its original loop
  compares the index against 5. The verified immediate becomes max_ratio.
- At `662a25`, indices above 2 normally consult the experimental-map flag.
  Raising that verified threshold to max_ratio-1 enables every offered ratio.
- `880940` formats only five stock cases and asserts otherwise. Its full
  entry is redirected to an ABI-compatible MSVC small-string formatter;
  every label fits inline without cross-allocator ownership. Its early short
  branches are not copied into a non-relocating trampoline.
- The existing `674aa0` size detour handles extended indices itself. Stock
  rows query their original square through the original function, preserving
  experimental-size index translation, then derive the desired ratio. Explicit
  world-dimension overrides keep the original function's override behavior.
- Formatter and sizing support exist before the combo loop is extended. All
  three patch sites are preflighted; failed writes attempt to restore earlier
  writes. No new menu rows are enabled without the size detour.

`tools/test_map_ratios.py` checks 1,260 exact-ratio bounded shapes, malformed
indices, all 7 stock sizes at all 20 formats, override forwarding, string ABI,
build/install failure guards and original patch bytes. Unicorn executes the
game's actual combo loop with the new immediate operands and confirms 20
correctly labelled, enabled entries and its unchanged default selection.
