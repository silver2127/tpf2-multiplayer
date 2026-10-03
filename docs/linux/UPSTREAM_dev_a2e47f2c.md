# Windows dev a2e47f2c integration

## Merged

One Windows commit, `a2e47f2c14373a6eee3283a58c160f9ffd0e79ec`, onto Linux
parent `72fd257b55f2542aff7ba107f6983fd9f9af614b` on `port/dev`:
`re tools: stack walking and follow mode in profile_load, ETW stack/wait readers for trace_load captures`.
No conflicts. All three incoming Python files remain byte-identical to upstream.
The merge remains staged and uncommitted; release remains **0.7.1.1**.

## Ported (what and how)

Retain the Windows profiler's new follow mode and both Windows ETW readers.
Following earlier integrations (65302e5d and 5fd49a24), these remain developer
diagnostics for Windows captures. There is no native runtime counterpart to
change and no hook, address, byte pattern, layout or ABI to translate.
The standard-library wait reader also runs on Linux for offline Windows XML.
Native stack inspection continues through gdb's ELF unwinder; this does not
provide equivalent sampling statistics or automatic thread following.

Document the platform/input boundaries in
[the tooling scope record](../re/linux/DEV_A2E47F2C.md), link this integration
from README and INSTALL, and include the integration record in Linux release
packaging. Keep the 65302e5d Lua verifier and BUILDINFO baseline: the incoming
commit's mod tree is identical. No runtime settings or installation changes.

## Not ported

None requiring a native runtime port in this commit. The Win32 sampler and
ETW-specific diagnostics remain Windows tools by design, consistent with the
existing port; no native sampler or Linux scheduler/waker analyzer is claimed.
Earlier unrelated native omissions and live-validation gaps remain unchanged.
This scope decision is based on the full code diff and its platform inputs,
not a failed attempt to derive a native hook. No static/live RE was needed.

## Live testing

No game launch, gdb attachment, desktop input or actor installation was needed
or performed. No gameplay, Vulkan, load-time, real ETW capture or cross-platform
result is claimed. Lab actors, saves, Steam and the user's installed mod were
untouched; no backup or restoration was necessary, and no game was started.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, **78/78 CTests**
  in 62.76 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero pinned exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- All three incoming Python scripts: AST parse and upstream byte comparison
  passed. The incoming mod tree equals the retained Lua verification baseline.
- Synthetic eight-event ETW XML smoke test of `trace_waits.py` on Linux:
  tid 101 on CPU 1.0 s, off CPU 0.5 s, ready delay 0.25 s, UserRequest wait,
  waker tid 202; all assertions passed. This is fixture evidence only.
- `bash -n tools/linux/build_release.sh`, working/staged whitespace checks,
  empty unmerged index and retained MERGE_HEAD: PASS.
- No native implementation changed or existing native profiling test exists;
  no permanent regression test added for documentation/package-list edits.
  Win32 profiler execution, real ETW stack decoding, Windows compilation and
  release packaging were not run.

Logs: `.git/port-a2e47f2-{build,lua,tools}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in the job clone.
