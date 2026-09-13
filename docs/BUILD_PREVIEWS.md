# Shared build previews

Other players can see an unconfirmed road or railway route. The optional native
plugin reconstructs a separate 3D BuilderRenderer from the received geometry,
including elevation and street/track options. Without a working native renderer,
the receiver uses a thin ground marking in the builder's cursor colour.
The preview follows edits and remains visible while the builder waits to confirm.
Confirming, cancelling or switching construction tools removes it.

This covers the road and track builders. Buildings, stations, terrain tools and
upgrades are not covered. Bridge and tunnel resource names are carried by the
3D protocol; the ground fallback only shows their horizontal route. Both players
need the current preview Lua module. Cost labels and confirmation controls remain
local. The receiver evaluates a temporary, disconnected route against its world;
junction details can differ from the builder's own preview. The sender's error
status now controls the native blue/red tint when available. Legacy or unreadable
status keeps the receiver's evaluation. The native plugin is experimental and
supports game build 35924. Validation and remaining coverage are listed below.

## Implementation

`mp/previews.lua` observes `builder.proposalCreate` in GUI Lua and copies the
proposal's node positions and Hermite tangents. Existing node references are
resolved locally with guarded entity calls. Companion replacement segments are
excluded. Proposal userdata and entity IDs never travel across the network.

Complete, timestamped files connect each game's GUI and engine Lua states,
following the existing cursor implementation. `LSPREVIEW` carries only an origin,
generation, sequence and bounded geometry (or `off`). It bypasses the command
queue and does not alter proposals, simulation stamps, money or saved state.
The fallback draws separate `mppreview_<origin>_<segment>` zones.

The 3D body is `3|<road/rail XY curves>|<detail rows>`. Each semicolon-separated
detail row contains `z0,z1,tz0,tz1,terrain,file,bus,tram,catenary,structure`.
Resource file names use canonical percent encoding and are resolved on the
receiver. Resource indices and world entity IDs are never transmitted. Ground
segments use `-` for structure; bridge/tunnel types use their resource file name.
With known sender status the body is `4|<invalid>|<XY curves>|<detail rows>`, where
`invalid` is exactly `0` or `1`. It comes from the same proposalCreate event's
`data.errorState.messages` (nonempty means red); warnings alone do not set it.
Status-only changes also invalidate the receiver's cached preview. Version 3
remains readable with unknown status. Both peers need the updated Lua module.

Updates are limited to five per second with a one-second keepalive. Preview
bodies are limited to 4096 bytes and 24 segments. If only the 3D extension exceeds
the byte limit, the whole route falls back to XY; otherwise oversized proposals
are omitted entirely. Receivers retain at most 16 origins. Old or malformed packets are
ignored, and missing GUI, engine or peer heartbeats expire after four seconds.

On game build 35924, pressing the builder's cancel button emits no builder
event. The observer checks visible cancel/cost/error controls in the renderer's
small action layer to detect this case. The traversal is bounded and does not
replace any native handlers. A failing UI query also expires the preview.

## Native renderer

Build with `native\build.bat previews [suffix]`. This optional target is separate
from `all`, the installer and shipping deployment. For a developer test with the
games closed, the unsuffixed DLL belongs in `<game>/plugins/tpf2_previews.dll`;
the existing plugin host is required. Check for overriding plugin copies in the
data directory and Sandboxie overlay. Normal player updates use the launcher.

The GUI creates a SimpleProposal with isolated negative temporary IDs and calls
`api.cmd.make.buildProposal` only for conversion. It never submits the resulting
command. A complete request file with scene session and nonce lets the plugin
observe this conversion on the GUI thread. A matching native acknowledgement
is required before removing the XY fallback. Text reading normalizes Windows
CRLF. Session changes and nonce checks reject stale acknowledgements.

The plugin evaluates independent ProposalData and uploads buffers to one native
BuilderRenderer per origin, registered on the main scene. It retains the native
renderer factory, including a proper clone of its std::function. It never calls
SendCommand or applyProposal. Unchanged previews only refresh their heartbeat;
render-pass wrappers hide stale geometry after four seconds even if GUI polling
stops. Cancellation clears buffers; scene destruction removes and destroys all
renderers and the retained factory before the original destructor runs.
Native requests `drawok` / `drawbad` select the sender's tint before EndHeightMod
bakes the terrain overlay, then restore it after the full buffer upload for rendering.
Only that peer's cosmetic renderer is targeted. AddHeightMod can write the error
flag directly, so the EndHeightMod hook restores sender status before the terrain
overlay is generated. The cosmetic renderer's four error/normal RGBA pairs also
use the sender's selected palette, with the original palette restored for unknown
status. This covers per-segment colour selection.
Legacy `draw` retains local evaluation. ProposalData, local builders and actual
build validation are unchanged. The full setter's bytes are verified at startup.

Terrain height previews share one native UI buffer. Clearing a local builder
previously erased remote embankments while leaving their roads visible. The
plugin now composes active remote height buffers and the local builder's buffers
after changes, cancellation and local resets. Local previews are uploaded last
so they retain priority at overlaps. Borrowed local buffers are forgotten before
their renderer clears or dies. This only changes the temporary terrain display;
the simulation terrain is still changed by the normal confirmed build command.

All hook prologues and the supported game build are checked before installation.
Measured RVAs for build 35924:

| Entry | RVA |
|---|---|
| Renderer factory | `859240` |
| Main scene Add / RemoveRenderable | `6d32e0` / `6d9290` |
| Scene destructor body | `6d22d0` |
| scripting::Convert | `20e72f0` |
| CreateProposalData / destructor | `a072b0` / `3e5030` |
| AddToRenderer / Clear | `48d8e0` / `817f70` |
| EndHeightMod / renderer destructor body | `8191d0` / `814b20` |
| Upload / reset UI terrain heights | `34cd90` / `34e5a0` |
| Error-colour setter | `81df00` |

Native safety checks bound geometry and reject non-temporary IDs, removals,
edge objects and invalid vectors before evaluating a received proposal.

## Preview processing cost

Snapshot polling and changed-preview delivery retain their 0.2-second interval.
An unchanged GUI/engine snapshot is written once per second as a heartbeat;
failed opens, writes or closes are retried on the next poll. The GUI encodes its
copied proposal once until a new event replaces it. Receivers reuse validated
payloads, but still check file completeness, freshness and packet ordering on
every poll/receive. Expiry and cancellation discard GUI decode caches. Native
scene identity is read once per GUI poll needing native work; ACK checks and
expired-renderer recovery remain active. Resource indices are reused only within
one proposal conversion, never across scene or resource reloads.

`python tools/preview_perf_test.py [--baseline <saved-previews.lua>]` exercises
four GUI/engine contexts with mocked files and native ACKs. In its 10-second
stationary-preview workload (40 polls), snapshot writes fell from 160 to 40,
encode calls from 80 to 0, and decode calls from 220 to 0 after warmup. Both
versions sent 20 packets and performed 10 native keepalive conversions. All 20
moving-preview updates still reached the native stub in their respective poll.
A 24-segment route using one street resource reduced resource lookups from 24
to 1; two simultaneous origins reduced native ready-file reads from 4 to 1 in
one GUI poll. These are deterministic operation counts, not measured game FPS
or native proposal/terrain timings. The native renderer is unchanged from 0.1.5.

The same harness checks failed publication/retry, colour-only changes,
cancellation, native session restart, rejected keepalive, stale GUI/engine/peer
data, malformed/duplicate packets, partial snapshots and identity changes.
In-game verification of this optimization is still pending.

## Checks

Run `python tools/preview_test.py` with `lupa.lua52`, then
`python tools/luacheck.py`. The preview harness runs the real module in four
separate GUI/engine contexts and exercises geometry, network dispatch, ordering,
keepalives, cancellation and stale or partial snapshots without command submission.
The 84 checks also cover sender status transport and changes, 3D payload validation, resource lookup with different
indices on each client, missing resources, temporary entities and native ACKs.
Native rendering itself requires the live game check below.

`tools/preview_native_test.cpp` includes the real plugin implementation and tests
Windows LF/CRLF file handling, partial/oversized requests, scene identity and
ACK/expiry behaviour with a stub converter. Its header gives the MSVC build
command. Run it without `NDEBUG`; it does not attach to a game or install hooks.
It also checks shared terrain composition, local priority, local resets, remote
cancellation and expiry with multiple renderer buffers.

For a live check, load the same save through a two-player lobby and wait for
SYNC. Draw a road and then a railway without confirming; inspect the other
world, hold the preview, cancel and switch tools. Check both directions and
verify that the existing build confirmation still works and returns to SYNC.

Earlier versions were tested locally on build 35924 with two Steam/Sandboxie
instances: road client to host, railway host to client, ground fallback and native
rendering, simultaneous local/remote previews, holding while paused, changed
rail slope, cancellation, tool switching and road confirmation. Logs showed
matching post-build SYNC hashes, zero desyncs and an empty command queue.
Terrain checks covered opening and switching local tools while a remote raised
road remained visible, lowering it, cancelling one of two previews, and
expiry/recovery through the sender's menu. Normal game exit logged renderer
disposal. These checks preceded the Lua performance optimization in this PR.

A user screenshot confirmed the current native 0.1.5 road/bridge colour case,
including the ground approach. Invalid/red previews and rail colour transitions
are covered by automated tests but still need in-game verification. Modded
resources, more than two players, and comprehensive bridge/tunnel cases have
not had separate live coverage. No game FPS improvement is claimed.

Height buffers cover rectangular areas. A stationary local proposal can mask a
nearby newly confirmed terrain change until that proposal changes or is
cancelled; local preview priority does not merge competing earthworks into one
joint proposal. Standalone terraforming tools are outside preview capture.
