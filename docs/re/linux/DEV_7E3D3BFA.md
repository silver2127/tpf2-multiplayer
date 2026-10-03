# Material-index caching and dither stepping investigation

Windows target: `7e3d3bfac53308c611948118b7bfa4673983e648`.
Linux Steam build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` (readelf checked this run).

## Incoming change

`bigmap/src/material_index.h` retains the Windows nine-argument detour at
RVA `0x315f20` and its existing byte guard. It caches up to 256 height-map
pointers and material IDs, falling back to stock for larger counts, hoists
row arithmetic and overlay presence, and advances the modulo-63 column.
The Windows dither RVA `0x2f87d20` is not a Linux address.
`tools/re/profile_load.py --map` resolves plugin samples through MSVC linker
maps; the sampler uses Windows thread contexts and Toolhelp module enumeration.
This remains Windows diagnostic tooling, not a native ELF symbol resolver.
Both incoming files are retained unchanged.

## Fresh static investigation

Read the previous material investigation in DEV_8C3C02A5 and the Windows
world-entry-performance/material-grid-lifetime records. Searched the supplied
Linux funcsig.csv for MaterialIndexManager. The UpdateBoxAsync ThreadPool
LoopImpl source signature anchors `0xcc5cd0`; objdump on the actual lab ELF
shows a direct call at `0xcc5d22` to `0xcc41f0`. That worker contains the
matching pixel-selection loop, rather than calling an equivalent standalone
Windows nine-argument function.

Evidence from `objdump -d -Mintel`:

| Site | Actual instructions / inference |
|---|---|
| `0xcc41f0` | `55 48 89 e5 41 57 41 56`; prologue then saves RDI in R14 at `0xcc41f8`, ESI in R12D at `0xcc4204`, EDX in EBX at `0xcc4208`. SysV closure/range worker entry, not the Windows signature. |
| `0xcc4241` | calls operator new with `edi=0x1000`; the containing worker owns temporary storage and does more than pixel selection. |
| `0xcc4758` | advances batch by 8; `0xcc48b6` subtracts 9 for overlapping layer bounds. |
| `0xcc4827` | `80 38 e9`: unresolved output sentinel. |
| `0xcc486f..0xcc4890` | signed modulo-63 arithmetic using `0x82082083`, followed by shift/subtract. |
| `0xcc4893` | `48 8d 05 66 e5 30 03`: dither base `0x3fd2e00`; `0xcc48a4` loads its indexed float. |
| `0xcc48aa`, `0xcc48c5` | count from current R14+0x28 and stride from R14+0x24; current R14 is not assumed to remain the entry closure. |
| `0xcc4930` | layer entries step backwards by 0x18 bytes. |
| `0xcc493d` | `49 8b 17 48 8b 12`: the two dependent height-map loads through R15 and then RDX. |
| `0xcc4943..0xcc497e` | scalar sub/mul/add interpolation and threshold comparison. |
| `0xcc4986` | reads DWORD at layer+0xc, then writes its low byte to output. |
| `0xcc4a45` | fallback ID read from current R14+0x18. |

These are disassembly observations, not an approved hook contract. A replacement
must establish the closure-to-layer-owner mapping, every live register/stack
value at the inlined loop boundary, output/base/overlay aliasing, pointer
stability through each job, and geometry behavior (including negative inputs).
The broader worker also allocates temporary buffers and looks up textures;
replacing its entry with the Windows function would be incorrect. No new
address, offset or patch bytes were added to native runtime code.

Reproduce with the supplied ELF and:

```
objdump -d -Mintel --start-address=0xcc5cd0 --stop-address=0xcc64d0 GAME
objdump -d -Mintel --start-address=0xcc41f0 --stop-address=0xcc4c40 GAME
```

## Live attempt and decision

Installed this run's soldier libraries, Big Maps plugin and merged Lua into
backed-up native lab actor payloads. Requested `autoload=1`, with lab
`newgame_density=0` to avoid resource rewriting. Used only the prescribed
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
It returned exit 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game process, menu, Vulkan device, save load or gdb attachment was reached.
Consequently no buffer ownership, register or lifetime observation was possible.

Keep native stock material selection and mark this optimization unported.
Resume with a working lab: break at the worker and inner loop, identify the
closure and per-job buffers, derive a complete replacement contract and compare
output with stock across layer counts, overlays, batches and dither wraparound.
No native timing or Windows benchmark reproduction is claimed.

Both payloads were restored from `.before-port-7e3d3bfa` backups; `diff -qr`
returned 0 for each. No save changed, desktop input was not used, no game
remains running, and Steam was not managed. The job's `meta/live/` contains
`material-worker.asm`, `material-candidate.asm`, `launch.txt`, `restoration.txt`,
and actor logs/data snapshots (which include historical files).
