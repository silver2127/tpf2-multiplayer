# Linux native preview renderer: build 35924

`native/linux/src/plugin/preview_plugin_linux.cpp` ports the release-0.4.22
cosmetic preview service. It exports the existing `Tpf2mpPluginInit` ABI as
`tpf2_previews.so`. Enable it with `[previews] enabled=true` in the host's normal
configuration. The packaged default remains **off** until loaded-world rendering
and lifecycle testing is complete. Lua's existing colored-zone previews continue
when native readiness or a successful request acknowledgement is absent.

The implementation does not call `sendCommand`, `applyProposal`, a simulation
step, or any command queue. The GUI creates a command solely to convert a
`SimpleProposal`. The plugin then evaluates that converted proposal, creates
independent cosmetic `BuilderRenderer` objects, and registers their renderable
pointers with the existing renderer component.

## Evidence and current validation

The mapping was derived from the installed Linux ELF, independently of the
Windows offsets:

- Game: `TransportFever2`, Linux build 35924.
- GNU build-id: `3a0e156390b0e6f1e372051c24802c8493ae454a`.
- Local evidence: `/home/topsnek/tpf2-re/linux/{functions,funcsig,xrefs}.csv`, ELF
  RTTI/vtables, disassembly of callers/callees, allocation and sized-delete sites.
- `funcsig.csv` aggregates inlined assertions; the first signature in a function
  is not necessarily that function's signature. The own-signature references
  below resolve the conversion and evaluation functions explicitly.

Completed validation:

1. All eight hook prologues and nine called-function signatures, the complete
   error-color setter, both RTTI names, the relocated BuilderRenderer vtable,
   fourteen direct calls, and twelve layout anchors match the ELF.
2. Every stolen span ends on a complete instruction and contains no RIP-relative
   operand or branch. An independent audit also scanned for incoming rel32 and
   nearby rel8 branches into the stolen interiors and found none.
3. Off-game tests cover complete road/rail requests, construction validation,
   palette and shared-terrain behavior, session/nonce/file parsing, every signature
   refusal and partial hook-install failure, runtime thread readiness, expiry,
   unexpected renderer destruction, scene disposal and guarded invalid pointers.
4. The float frame-time regression deliberately clobbers XMM0 in clock queries
   and verifies that the native Update receives the exact noninteger dt.
5. The hidden static-runtime test loads a separate dynamic-runtime exception
   fixture. Original game-call exceptions reach that runtime's own caller/catch;
   preview evaluation and nested terrain-mesh exceptions produce an error ACK,
   release the plugin mutex and preserve the Lua fallback. ASan/UBSan also pass.
6. The actual game started under Steam Snap with the plugin enabled. All eight
   detours installed and plugin initialization returned OK; the process stayed
   alive at the title screen. This validates initialization, not world rendering.

Still requiring an in-game check: load a world, activate the street/rail tool,
observe `GUI/render thread verified` and a numeric native-ready file, then check
remote road/rail/construction geometry, valid/invalid tint, local terrain priority,
clear/expiry, world exit/reload and two-process synchronization. Rendering, mesh
appearance and GPU behavior cannot be certified by the off-game fixtures.

## Function map

All addresses below are ELF RVAs, to which the host's verified module base is
added. Calls follow x86-64 System V, including the hidden aggregate-result pointer.

| Operation | RVA | Ownership / ABI evidence |
|---|---:|---|
| `scripting::Convert(toolkit, scripting::Proposal)` | `0x2e237b0` | Own signature referenced at `0x2e25578`. Hidden output in `rdi`, toolkit in `rsi`, source proposal in `rdx`. GUI Lua maker calls at `0x1971288`, return `0x197128d`. |
| `CreateProposalData` | `0x162f050` | Own signature reference `0x163646c`. Hidden output, toolkit, cost, proposal, shapes, Context occupy `rdi,rsi,rdx,rcx,r8,r9`. The optional cost and shapes pointers are null. |
| `AddToRenderer` | `0xef03c0` | Full signature in `funcsig.csv`: toolkit, renderer, ProposalData, `CVec3f`, const entity map, three bools. Last two bools are stack arguments. |
| ProposalData destructor | `0xde3df0` | At `0xe596f3`, followed by sized delete of `0x900` bytes. |
| Context destructor | `0xdd8820` | Lua maker default construction and existing construction map; `0x68` bytes. |
| BuilderRenderer factory | `0x13fa4b0` | Takes factory in `rdi`, returns allocated renderer in `rax`. Allocates `0x1b0` at `0x13fa4f2`; calls renderer ctor at `0x13fa54c`. |
| Factory callable copy | `0x13fa5b0` | Native `std::function` manager operation 2, followed by manager/invoker copy. Manager operation 3 destroys the retained callable. |
| BuilderRenderer ctor | `0x139a1a0` | Writes audited vptr and allocates its state. |
| BuilderRenderer complete destructor | `0x13948d0` | Writes BuilderRenderer's vptr; first Itanium vtable slot. |
| BuilderRenderer deleting destructor | `0x1394d00` | Calls complete dtor at `0x1394d10`, then sized delete `0x1b0`. Takes only `this`; there is no Windows deleting-dtor flags argument. |
| BuilderRenderer Update | `0x138eba0` | Vtable slot 2. Caller loads float seconds into `xmm0` at `0x11d9608`, component into `rsi` at `0x11d960d`, calls at `0x11d9610`. Typed ABI is `void(this, component, float dt)`. |
| BuilderRenderer primary render pass | `0x1395340` | Vtable slot 3; `rsi` is the renderer component, `rdx` its render helper. Hook observes the real scene's execution thread before publishing readiness. |
| Renderer component `AddRenderable` | `0x11da630` | Named signature; pointer vector at `+0x4c0/+0x4c8/+0x4d0`. Main `CGameUI::CreateUI` call `0xffb241`, return `0xffb246`, identifies the scene. |
| Renderer component `RemoveRenderable` | `0x11da5a0` | Named signature; erases a pointer without deleting the renderer. |
| Renderer component complete dtor | `0x11d99e0` | RTTI/vtable identifies `UI::CRendererComponent`. Frees the vector storage, not the renderers it references. |
| BuilderRenderer `Clear` | `0x13981d0` | `this, models, terrain` in `rdi,esi,edx`; terrain clear branch at `0x13992f0`. |
| BuilderRenderer `EndHeightMod` | `0x1397170` | `AddToRenderer` calls BeginHeightMod `0x13915e0`, AddHeightMod `0x1394d80`, then this function at `0xef0c0f`. |
| UI terrain upload | `0xd0a070` | `EndHeightMod` calls at `0x1397e3a` with target, descriptor vector and option byte. |
| UI terrain reset | `0xd09950` | `Clear` calls at `0x1399302`, with target and option byte. |
| Renderer-wide error setter | `0x1391f00` | Entire 19-byte function verified: loads `this+0x198`, writes `state+0x1684`, returns. `AddToRenderer` calls it at `0xef075e`. |

The factory is captured only from the first StreetBuilder constructor's call
(return `0xe7d14a`). Its last accessed field is `+0xb0`, hence the retained
`0xb8`-byte footprint. The 32-byte callable at `+0x80` is cloned with the game's
copy function and destroyed through the game's manager; the plugin never
constructs or destroys game-owned callable storage with its static C++ runtime.

The scene is **CRendererComponent**, not a guessed Windows RenderScene layout.
RTTI `N2UI18CRendererComponentE` at `0x41d3cc0` leads to typeinfo `0x5a198d8`
and vtable address point `0x5a199c8`. The capture site belongs to
`UI::CGameUI::CreateUI` (`0xffae10`), whose string references include `Render
Thread`, `UI_root` and `mainView`. The runtime thread observation remains necessary
in addition to those static references.

## Renderer and proposal storage

| Object / field | Linux layout | Proof |
|---|---|---|
| BuilderRenderer object | `0x1b0` bytes | Factory allocation and deleting-dtor size agree. |
| Terrain pointer | renderer `+0x50` | EndHeightMod upload and Clear reset argument loads. |
| Terrain enable / upload option byte | renderer `+0xd0` / `+0xd4` | EndHeightMod branch and upload arguments. |
| Color palette | renderer `+0xf8`, `0x80` bytes | Ctor passes renderer `+0xd0` to config initializer `0x1390cc0`; initializer writes four normal RGBA entries at config `+0x28..+0x67` and four error entries at `+0x68..+0xa7`. |
| Private renderer state | pointer at `+0x198`, allocation `0x1918` | Ctor allocation at `0x139a31a`, state ctor call `0x139a359`, store `0x139a35e`. |
| State error flag | `+0x1684` | Complete error setter. |
| State terrain descriptor vector | `+0x1830/+0x1838/+0x1840` | EndHeightMod passes `state+0x1830` to terrain upload. The descriptors remain renderer-owned. |
| ProposalData | `0x900` bytes | Destructor followed by matching sized delete. |
| Construction-to-add vector | Proposal `+0x2a0`, stride `0x8f0` | Linux construction map; see `SLICE_CONSTRUCTION.md`. |
| Construction filename / transform | entry `+0x00` string / `+0x738` float matrix | Linux construction layout. |
| Street node/edge vectors | Proposal `+0x00/+0x18`, stride `24/120` | `SLICE_PROPOSAL.md`. |

The BuilderRenderer RTTI name `N2UI15BuilderRendererE` is at `0x426d4e0`,
its typeinfo at `0x5a20748`, and vtable address point at `0x59bba48`.
The full ten-qword image from `0x59bba38` is:

```
0, typeinfo,
13948d0, 1394d00, 138eba0, 1395340, e160b0, 1395910, 139c320, 1395c90
```

Private renderer vtables preserve offset-to-top, RTTI and **both** Itanium
destructor entries. Only Update and the five render entries at slots 2–7 are wrapped. Update
forwards float dt through a typed call so clock/mutex helpers cannot lose XMM0;
slots 3–7 take three GP-register arguments. All six caller sites ignore the
return value. The normal
game renderer's vptr and palette are never rewritten. A peer's original palette
is retained and restored before choosing the sender's normal or error half.

The Context initialization is reproduced from the Lua maker's instructions at
`0x197129a..0x1971302`, not copied from the Windows constructor:

- Zero thirteen qwords (`0x68` bytes).
- Set bytes `+1` and `+9` to 1; float `+4` to 1; float `+0x10` to .75;
  int `+0x14` to -1.
- The empty map at `+0x18` points to the Context's own single bucket at `+0x48`,
  has bucket count 1 at `+0x20` and max load factor 1 at `+0x38`.
- All remaining fields are zero. Use the game's Context destructor afterwards.

`AddToRenderer` receives a read-only empty entity map with audited libstdc++ size
`0x38`; a compile-time assertion and runtime test cover its empty-bucket layout.
No game operation inserts into this map, and its storage stays plugin-owned.

## Execution, lifetime and failure handling

All eight detours are checked before any patch. A separate success flag is set
only after every install returns success; a partially populated trampoline
pointer never activates the service. Scene/factory retention and conversion
handling require that success flag.

Capturing the scene records its GUI thread but leaves the ready file blank. A
normal BuilderRenderer primary pass must subsequently reference that same scene
on that thread before readiness is published. Mismatch disables native previews
for that scene; later requests and peer render passes are refused. The mutex is
recursive because native buffer upload re-enters the height hooks on the same
thread. Own helpers catch their static-runtime allocation exceptions.

`preview_game_guard.h` contains a private adaptation of the proven menu-game
exception guard. Game-initiated forwarding calls have no active cleanup objects
in their detour frames. Every game function initiated by the preview plugin is
called below a catch trampoline using the dynamically resolved game personality,
`__cxa_begin_catch`, `__cxa_end_catch` and `_Unwind_Resume`. Trivial argument
frames have compile-time destructor checks. Initialization is refused if these
functions resolve into the plugin itself or cannot be found. A foreign native
failure is propagated through nested height operations, prevents an `ok` ACK and
turns off native drawing for the scene. This avoids both a swallowed failed draw
and a foreign unwind through this plugin's hidden static libgcc cleanups.

Proposal validation probes memory with `process_vm_readv`, validates complete
vector triplets, bounds counts before traversing and rejects malformed pointers,
non-finite coordinates, duplicate temporary IDs, missing or equal endpoints,
zero tangents, existing-world node IDs, removed entities and edge objects. A
construction must have a bounded relative `.con` path and finite, nonsingular
transform. The initial construction path is experimental; unsupported input
receives an error ACK so the existing Lua zone remains visible.

Terrain descriptors are borrowed only while their renderer lives. Remote grids
are uploaded first, then the local tool's grids, preserving local-tool priority.
Clear and renderer destruction forget borrowed local descriptors; destruction
also invalidates a matching peer pointer. Expiration clears remote models and
recomposes shared terrain. Scene destruction removes private renderables before
deleting them with the native deleting destructor, drops borrowed terrain
pointers and destroys the retained native callable. Native ready/request/ACK
files are cleared at initialization, including the disabled-plugin path, to
prevent a previous process's readiness from being reused.

The request wire format is unchanged from release 0.4.22: `session nonce origin
mode`, followed by `\nend\n`. Modes are `clear`, `keep`, `draw`, `drawok` and
`drawbad`. Requests are consumed before evaluation; ACKs include both session and
nonce. LF and CRLF are accepted; incomplete, oversized, NUL-containing and extra-
field requests are rejected. Keepalive cannot revive expired geometry.

## Reproduce checks

```sh
python3 tools/linux/verify_preview_elf.py '/path/to/TransportFever2'
cmake --build native/linux/out --target tpf2_previews test_preview_plugin slice_test_foreign
ctest --test-dir native/linux/out -R preview_plugin --output-on-failure
```

The standalone verifier needs `pyelftools` and `capstone`, reads the unmodified
ELF and derives its signature list from the implementation. It never loads or
executes the game. The C++ test includes the implementation directly and uses
mock native operations plus `slice_test_foreign` for the foreign runtime test;
it does not install hooks into a process or require a graphics device.
