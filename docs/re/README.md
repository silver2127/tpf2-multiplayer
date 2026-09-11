# Engine reference

What the hooks rest on. Everything here was established on the Steam build **35924** of
`TransportFever2.exe` (PE `TimeDateStamp` `0x675abcc6`, linked 2024-12-12; `SizeOfImage`
`0x046ce000`). The game has not been updated since, which is what makes hardcoded addresses
workable. The slice DLL and the plugin host compare the PE header and stay inert on any other
build; most other hooks compare their prologue bytes before patching (the menu's `CreatePage` hook
does not).

| file | covers |
|---|---|
| [COMMANDS.md](COMMANDS.md) | the command pipeline, `CommandList::Add`, how to cancel a command safely, UI tools and their call sites, every `make_cmd` factory with its arguments |
| [PROPOSALS.md](PROPOSALS.md) | the proposal structure (nodes, segments, construction entities, parameters), templates, refusal rules, level crossings, stops and signals, the terraform and paint grids |
| [GAME_LOOP_AND_UI.md](GAME_LOOP_AND_UI.md) | threads, sim pacing and game speed, forcing a save, loading a save, the title menu |
| [SCRIPT_API.md](SCRIPT_API.md) | how the game-script Lua API actually behaves: crashes `pcall` cannot catch, vehicles, lines, constructions, GUI vs engine state |

## Conventions

Addresses are **RVAs** relative to the image base `0x140000000`. Ghidra shows virtual addresses:
`FUN_1409dc750` is RVA `0x9dc750`.

| label | meaning |
|---|---|
| CONFIRMED | decompiled or named by the binary's own `__FUNCSIG__` strings, and consistent with live behaviour |
| DECOMPILED | read from a decompile with a recovered signature; not checked against live bytes |
| MEASURED | observed in the running game: a log line, a memory dump, a differential capture |
| sweep EXACT | a ground-truth sweep put known values in and the field held the swept value in every sample |
| LIKELY / INFERRED | placed by source file, address order or elimination only |

## Why the binary is workable

No packer or anti-tamper, intact RTTI, and MSVC's assert macros embed `__FUNCSIG__` (the full
demangled signature) and `__FILE__` (the source path) of every function that asserts. Extracting
those strings names roughly 22,500 functions and attributes about 6,500 to some 720 source files;
because the linker keeps a translation unit contiguous, unnamed functions can still be placed in
the right file. RTTI vftables give a third, independent axis (e.g. vftable slot 5 of every
`UI::IAction` is `Step`).

## Rules learned the hard way

1. **One sample that matches a prediction is not evidence.** Identify a field with a differential
   (captures that differ in exactly one property) or a ground-truth sweep. A rotation matrix, a track
   type and a tram flag were all "found" in single samples and were noise.
2. **Offsets from a decompile without a recovered signature are not evidence.** The decompiler's
   `param_N` are guesses. Apply a prototype first (`ApplySigDecompile.java`) or check live bytes.
3. **Truncate output, never detection.** Size limits on what counts as a plausible vector hid the
   answer four times.
4. **Runtime counts say which code ran; a decompile says what it is.** A caller of `applyProposal`
   that scored "UI only" by frequency was a per-tick simulation system.
5. **A float near 1.0 read from pointer-dense memory is the low half of a pointer.**
6. **In a hook, never cancel on a failed decode**, and identify the command to cancel by its
   pointer, not by its caller.
7. **Read the official API reference before disassembling anything that has a Lua binding**
   ([SCRIPT_API.md](SCRIPT_API.md)).

## Ground-truth sweeps

Driving known values through a factory is faster and more reliable than inferring from player
actions: `api.cmd.make.*` builds the Command and fires the slice's factory hook without anything
being sent to the engine, so a sweep changes nothing, needs no restart, and takes seconds. Give each
sample a sentinel value so nothing depends on ordering, and accept an offset only when it held the
swept value in every sample.

The sweep mode that did this (`groundtruth=1`, the mod's `gt.lua` and `tools/gt_correlate.ps1`) was
removed with the other diagnostic switches; it is in git history, for example
`git show v0.4.11:tools/gt_correlate.ps1`.

`dumpprop=1` dumps whole proposals from the factory hook, UI and script side alike;
`tools/re/dumpprop_diff.py` and `dumpprop_vecs.py` diff and decode them.

## Static tooling

### Ghidra (`tools/ghidra`)

The runners default to Ghidra 12.1.2 in `C:\tools\ghidra_12.1.2_PUBLIC`, an Eclipse Adoptium JDK
21, the project `C:\tools\ghidra_proj` (name `TpF2`) and output in `C:\tools\ghidra_out`; override
the parameters for another machine. The project is locked to one process: never run two scripts at
once.

| runner | does |
|---|---|
| `analyze.ps1` | imports a copy of the exe and runs full auto-analysis (hours; 48 GB heap) |
| `extract.ps1 [-Targets <file>] [-SkipClassMap]` | `ExportClassMap` then `DecompileTargets` against the analysed project (`-noanalysis`) |
| `run.ps1 <Script.java> [args]` | any script from `scripts/`, headless, `-noanalysis` |

| script | arguments | output |
|---|---|---|
| `DumpStringXrefs` | `<outdir> [minlen=4]` | `strings.csv`: every string with the functions that reference it |
| `DumpCallEdges` | `[outdir]` | `call_edges.csv`: caller, callee, call site |
| `DumpVtables` | `<outdir> <class substring>...` | `vtable_dump.csv` |
| `ExportClassMap` | `[outdir]` | `classes.csv`, `class_methods.csv`, `vftables.csv` |
| `ExportNamedFunctions` | `[outdir]` | `named_functions.csv` |
| `DecompileTargets` | `<targets> [outdir] [timeout=180]` | `<label>.c` and `<label>.fields.txt` (offsets used off each pointer) |
| `ApplySigDecompile` | `<outdir> <rva> <label> <retBytes> [timeout] [nParams]` | applies a struct-return prototype, then decompiles (`.sig.c`) |
| `CallGraphUp` | `<outdir> <rva> <depth> <marker>...` | breadth-first callers looking for marker symbols |
| `FindCallSites` | `<outdir> <rva> [count]` | call sites with a "return value used" guess |
| `FindTypeUsage` | `<outdir> <type substring>...` | `type_usage.csv` |
| `DumpPtrTable` | `<outdir> <rva> <count> [label]` | a function-pointer table, following thunks |
| `PrologueBoundaries` | `<outfile> <rva>...` | instruction boundaries of the first 48 bytes (hook steal sizes) |
| `ResolveLambdaVtables` | `<outdir> <hash>...` | decompiles each vtable slot of a lambda's `std::function` impl |

`tools/ghidra/targets/*.txt` are the per-investigation target lists for `DecompileTargets`
(`<rva-or-va> <label>`, `#` comments); every address in them is build 35924.

### Python (`tools/re`)

Needs `numpy`, `pefile` and `capstone` for the disassembly helpers. The corpus scripts read the
CSVs the Ghidra scripts write.

| script | does |
|---|---|
| `funcsig.py [outdir]` | `strings.csv` → `funcsig.csv`: the `__FUNCSIG__` symbol table |
| `func2src.py [outdir]` | `strings.csv` → `func2src.csv`, `src2func.csv` |
| `src_ranges.py [outdir] [filter]` | source file → address range |
| `whois.py <rva>...` | signature, source file and vftable slot of an address |
| `xq.py callers\|callees\|src\|file\|str\|fnstr ...` | queries over the corpora |
| `cmdmap.py` | regenerates the per-action `CommandList::Add` / factory table |
| `extract.py` | strings, RTTI class names, assert strings and source paths straight from the exe (stdlib only) |
| `find_hooks.py`, `walk_up.py`, `find_menu_ui.py`, `find_sym.py`, `tpfdis.py` | capstone-based xref and disassembly helpers (`find_menu_ui.py` regenerates `menu_ui_map.txt`) |
| `profile_load.py` | a sampling profiler for the running game (suspends threads to read RIP) |
| `trace_load.ps1`, `trace_summary.py` | Windows Performance Recorder traces of a load, and their summary (elevated) |
| `dumpprop_diff.py`, `dumpprop_vecs.py` | diff and decode `dumpprop=1` proposal dumps from `tpf2_slice.log` |

The exe path in these scripts is the default Steam location; edit it for another library.
