# Faster save compression (Steam 35924)

`save_fast=1` changes the save compressor from zstd level 3 to level 1 and
increases its input stream buffer from 128 bytes to 64 KiB. It applies to
autosaves and manual saves. The serializer, uncompressed save bytes, error
handling, stream finalization, filenames and rotation logic are unchanged.
Files can be larger. Set `save_fast=0` and restart to restore stock behavior;
previously written saves do not require the option to remain enabled.

The patch is limited to Serializer.cpp's PushCompressor, RVA `2e8620`, whose
only recorded direct caller is save stream setup `2e7dd0`:

- `2e8641`: replace `mov eax,[rip+...]` (reads compression level 3 at
  `392767c`) with `mov eax,1; nop`. The shared level constant is untouched.
- `2e87ad`: change the compressor stream-buffer constructor's `r8d` argument
  from 128 to 65536. Constructor `2db930` allocates its buffer from this
  explicit size and sets buffering flags dynamically. The compressor's own
  separate 4 KiB output buffer remains unchanged.

Both complete six-byte instruction sequences are verified before any write.
If a write fails, earlier writes are rolled back and the failure is logged.
Each optimization is independently valid if a rollback itself fails. GOG and
disabled configurations do not patch. No game save is rewritten in place.

## Baseline and benchmark

The running world's game log reported autosaves of 20,879.5 and 21,723 ms,
with about 1.43 GB output, before this change.

A full autosave was decompressed read-only and recompressed into temporary
test files at levels 3 and 1, with 64 KiB input chunks. The test timed compressor
calls separately from reading/decompression, hashing and file writes. Both
outputs were decompressed again and checked for identical byte count and
SHA256; temporary output files were removed after verification.

- Uncompressed payload: 2,562,832,617 bytes.
- Level 3: 18.811 seconds compression; 1,426,467,668 compressed bytes.
- Level 1: 6.750 seconds compression; 1,558,070,429 compressed bytes.
- Approximately 2.79x faster compression and 9.23% larger output in this test.
- Both payload hashes:
  `eeac3e73f29bd96e9bc1055449221f8f929b503051de28a19cd9a86ca558f320`.

The benchmark uses Python zstandard 0.25.0, not the game's embedded compressor.
It does not measure the game's serializer, the buffer-change speedup, disk
latency, or the total autosave pause.

September 13 runtime validation: with `save_fast=1`, the current 114x570-tile
world saved in 14,416 ms under terrain compression v1 and its output loaded
successfully under v2. After two connected rail builds, v2 saved in 13,686.2 ms
(1,550,643,905 bytes). These are manual saves through the same serializer;
there is no controlled same-world stock comparison yet. The test output is
preserved separately as `MemoryV2-RailTest-20260913`, and the pre-test world as
`MemoryBaseline-20260913` in the Steam local save directory.
The v2 test output also reloaded successfully in-game with both rails intact.

`python tools/test_save_fast.py` verifies the original bytes, disabled/build
guards, preflight failure handling and rollback. Unicorn executes the original
and replacement instruction blocks, checking the compression level and input
buffer size delivered by them. Terrain-cache regression tests also passed.

The active DLL/config were backed up before deployment. Check the host log
after restart for `fast saves: zstd level 1, 64 KiB input buffer`, then compare
the next `Saving...:` timing in stdout and load a newly written save.
