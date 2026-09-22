# dev 66c870cf: HUD glyph overlay

This commit changes only shared Lua styling and TGA textures. No Windows
hook, address, struct layout or calling convention changes. Linux packaging
copies the whole mod resource tree, so both image resolutions are included.
The upstream stylesheet now selects carrier-specific backgroundImage2 masks
and applies company backgroundColor2 on the icon element. Layer 1 remains
under the game's control. All shared files are preserved byte for byte.

## Inherited native prerequisite: static follow-up

Reviewed [DEV_50D7588B.md](DEV_50D7588B.md),
[DEV_61578D27.md](DEV_61578D27.md) and the native source. Searching native/linux
for TintClassPrefix, mpWinCo and StationIconTint found no company-class path.
Read the supplied Linux ELF without executing it; readelf confirms build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. Re-disassembled
0x10905b5..0x10905e3, confirming the previously identified carrier switch and
style call: 0x10905d9 (`4c 89 ee`) passes r13 in SysV rsi;
0x10905dc (`4c 89 e7`) passes r12 in rdi; 0x10905df
(`e8 ec 4a fc 01`) calls 0x30550d0. The earlier full-function analysis
identifies the class temporary and icon respectively, with native libstdc++
string semantics. Captures: `.git/port-66c870cf-elf.log` and
`.git/port-66c870cf-station.asm`. These are evidence only, not patch sites
introduced by this integration.

The shared image layer does not establish the missing entity/owner-to-widget
mapping, non-asserting StationGroup traversal, stale-class cleanup or safe
relay lifetime across world changes. Those remain the blockers to visible
native company tinting. No speculative hook is added. No new platform-specific
port is needed for this diff, and no live selector/rendering result is claimed.
