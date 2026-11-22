# Profiling tool scope: dev a2e47f2c

Reviewed the complete diff of `a2e47f2c14373a6eee3283a58c160f9ffd0e79ec`
against Linux parent `72fd257b55f2542aff7ba107f6983fd9f9af614b`.
Only three files under `tools/re/` change. There is no game patch, injected
runtime change, shared Lua change, new address or engine ABI contract.

## Windows inputs and Linux applicability

- `profile_load.py`: the actual delta adds `--follow`. Sampling a selected
  PE function enrolls its thread ID for subsequent StackWalk64 calls; followed
  chains retain five caller frames. Stack walking, plugin MSVC maps, thread
  reporting and cores-busy reporting already existed in the parent. Importing
  this script initializes `kernel32` and `dbghelp` through `ctypes.WinDLL`.
  Win32 CONTEXT offsets and `.pdata` RVAs are not Linux register/layout data.
- `trace_stacks.py`: reads tracerpt XML, decodes ETW StackWalk payloads and
  identifies the Windows game/modules from image rundown. Names PE functions
  through `profile_load.pdata` and the Windows `EXE_PATH`. That import also
  makes the reader Windows-dependent; it is not a native ELF trace reader.
  Its introductory example lists `--per-second`, but the parser has no such
  option: per-second output is unconditional. Preserve upstream unchanged.
- `trace_waits.py`: standard-library Python processing Windows CSwitch and
  ReadyThread XML, with Windows wait-reason names. It can run offline on Linux
  against Windows XML, but does not capture or decode Linux scheduler events.
  The upstream light capture has no CSwitch data for this analysis; use an
  appropriate Windows capture. Synthetic input is not real game evidence.

Following [the preceding profiler decision](DEV_65302E5D.md), preserve these
Windows developer diagnostics intact. Native stack investigation continues to
use gdb's ELF unwinder and verified Linux sites. A gdb backtrace is a stopped
snapshot, not equivalent CPU/wait statistics or automatic follow sampling.
No native sampling, scheduler capture or native waker analysis is added or
claimed. The native runtime has no consumer or counterpart of these scripts.

No address translation, disassembly or live ABI probe is needed for this
integration: none of the changed tools patches the game or establishes a
new native contract. Existing build-35924 evidence and byte guards remain
unchanged. No game was launched or actor modified for this tooling-only merge.

Validation: all three scripts parse and match upstream byte for byte. The
wait reader ran on a synthetic eight-event trace on Linux: tid 101 ran 1.0 s,
was off CPU 0.5 s including 0.25 s ready, and was woken by tid 202 with
UserRequest as its wait reason. Win32 sampling and real ETW stack decoding
were not executed. Full build results are in the
[integration record](../../linux/UPSTREAM_dev_a2e47f2c.md).
