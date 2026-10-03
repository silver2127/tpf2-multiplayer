# Native two-version sidecar release: dev 5b817efb

This change modifies mod-owned bookkeeping only. `Served::Grid::finished` is
not an engine struct field. The existing Linux mutex protects range/finished
state; LoadHook/BeginIfPending resets it. AlignmentUpdate passes true after a
skip and false after computation. PassDone keeps a loaded file only for a
skipped large pass with another unfinished tracked grid. Later large passes
on a finished grid run; small passes leave the file alone. This matches the
Windows changes without importing MSVC layouts or PE addresses.

The previous [ABI investigation](DEV_2B4FD093.md) still applies. Fresh verification
of the actual lab ELF passes its GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` and all 39 sites in
`bigmap/linux/sites.json`. Existing runtime identity/byte checks are unchanged.
Fresh objdump confirms publication at 0xcf58ad (`f3 41 0f 11 44 24 18`),
0xcf58b4 (`f3 41 0f 11 4c 24 1c`), and 0xcf58bb
(`41 83 44 24 20 01`): scaled float min/max at record+0x18/+0x1c and
32-bit version increment at +0x20. Linux's unsigned wraparound is preserved.
At 0x173e168, `49 8b 7e 08` gets the terrain from self+8 into SysV RDI;
0x173e16c passes the publication argument in RSI before calling 0xcf56d0.
No new patch bytes, hooks, register contracts or offsets are needed.

The regression fixture keeps the disabled-hook forwarding test and checks two
independent grids. The second grid receives just one tile before the first
pass, then its remaining tiles after that pass. Both publish matching min/max
and increment their own versions once. Repeating completion changes nothing;
a repeated large pass runs and releases the file even with a pending grid.
A new load resets finished state. This proves bookkeeping against fake terrain,
not engine ownership, scheduling or lifetime.

The candidate soldier libraries and merged Lua were installed into cp -a
backups of both native actor payloads; sidecars and autoload were enabled in
the actor. The prescribed lab run exited 1 immediately with
`bwrap: setting up uid map: Permission denied`. No game existed for gdb;
no title menu, Vulkan device, save/load, two passes or timing was observed.
Both payloads were restored and their file hashes and symlink targets matched.
No saves changed; Steam/user installs were untouched and no game remains.

Evidence: this job's `meta/live/verify-game.txt`, `publication.asm`,
`alignment.asm`, `launch.txt`, `launch-result.txt`, `restoration.json`, and
copied actor logs/data. The copied actor logs/data are historical and must not
be read as observations of this candidate. Existing live cache ownership,
pager synchronization and saved-world lifetime questions remain open;
`terrain_sidecar=0` remains the default. The Windows 5.8-second measurement
was not reproduced on Linux.
