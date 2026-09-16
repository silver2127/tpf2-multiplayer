# Linux vehicle, line and clock command capture

`native/linux/src/slice/slice_vehicles.cpp`, `slice_lines.cpp` and `slice_time.cpp`
implement the 14 factories mapped for Steam Linux build 35924, with the
release-0.4.22 protocol and strict line-creation changes. They use the
shared slice registration, guarded readers, inject writer and cancellation
services. All addresses and container offsets come from
`docs/re/linux/SLICE_VEHICLES.md`, `SLICE_LINES.md` and `SLICE_TIME.md`.

## Implemented behavior

- Vehicles: buy, sell, replacement, send to depot and reverse; Linux vehicle
  config/part layouts; load configs, colors, vehicle groups and libstdc++ packed
  automatic-load flags converted to the Lua reader's signed 32-bit words.
- Buys: the factory captures the config before the game moves it. A matched Add
  checks the command tag, unapplied result and buy callback identity. The complete
  `VBUY` and optional `VBUYLINE` are written together in one append, followed by
  cancellation. A failed write leaves no replay record and the strict Add barrier blocks the click. The unapplied
  buy callback is not fired: its proven result-minus-one path changes no state.
- Lines: assignment, strict creation, decoded updates and deletion.
  An empty stop list is a valid update. An unreadable or
  out-of-policy line is refused; no read-back event is emitted in multiplayer.
  The alternative-terminal callback is recognized and required because it
  balances a counter; other cancelled line callbacks remain silent.
- Strict creation: `LCREATEX` captures the complete name/color/line before the
  factory moves them, and holds the UI's callback until the local replay. A
  fresh single-integer `lclaimSeq` from unchanged Windows Lua selects the oldest
  matching held callback once. Claims already present at capture, duplicate
  claims, malformed claims and claims older than five seconds are refused.
  The script factory and eventual command must both match the held wire content.
  The two Add-backed script sinks use the shared callback override, while the
  simulation script's immediate sink has its own verified 15-byte hook. Both
  paths deliver the real applied result to the original UI callback. The capture
  record and claim file contain exactly the existing Windows fields.
- Clock: the exact speed-button, pause-toggle, date-picker and calendar-slider
  callers are intercepted, with the documented numeric limits and zero calendar
  speed preserved. Internal and script callers pass through.
- Clock counters: cancelled button/date commands never run their completion
  callback inside Add. A guarded `DoStep` hook removes the ensuing increment
  before the next step, with button-pointer identity checks against reused Clock
  storage. Automatic performance clamps are cancelled without a `SPEEDBTN` and
  their counter is released after one second to avoid repeating every frame.

Supported player commands request cancellation in multiplayer. `VCOLOR` and
`VNAME` currently remain blocked: unchanged Windows 0.4.22 Lua sets `skipOrigin=1`
for both regardless of `ARMED`, so emitting these captures would apply only on
peers. A native origin-replay adapter is still required. Solo calls are not
captured. There is no multiplayer switch to run these commands locally. Capture
failure is handled by the shared strict Add barrier, with callback policies
still respected.

The clock wrapper has no C++ exception cleanup. Its thread-local clamp scope is
valid only while its exact frame and caller remain on the guarded frame-pointer
chain. This permits game exceptions to cross the wrapper without involving the
library's static exception runtime, and invalidates stale scope state on the next
check. Keep `-fno-omit-frame-pointer` on the slice target.

Dependent features check `SliceHookInstalled`, whose atomic state is published
only after the auxiliary patch succeeds. A retained trampoline from a partial or
failed patch does not enable clock cancellation or strict line creation.

The writers that need Add's callback (buy, strict creation, alternative-terminal
update, and time) buffer at the factory and use the core's `prepareCancel` commit
callback after it validates the command and callback. An unwritten record is
blocked before native application in multiplayer. Callback metadata and counter
reservations are checked before writing; partial writes retain the cancellation
because Lua might already have parsed the prefix. Clock records have no `ARMED`
line, as required by their existing Lua consumers.

## Validation and boundaries

Run `tools/linux/test_slice_commands.sh`. The harness uses real libstdc++
containers and guarded reads, exercises malformed pointers and I/O failure, checks
wire records and cancellation policies, and verifies clock identity, delayed
clamps, nested scopes and exception recovery. A separate shared library throws
and catches through `DoStep` using the dynamic C++ runtime while the harness links
its own hidden static runtime, matching the game's runtime boundary. Core Add and
file services are modeled; this does not replace live detour or multiplayer tests.

These paths have off-game coverage; a title-screen startup check does not
validate live multiplayer command replay. The existing 0.4.22 format and limits
remain intentional:

- The proven engine missing-resource replacement path remains native. It is
  engine repair work with different billing, rather than a captured player click.
- The existing wire format omits vehicle-part reversal, logos, purchase and
  maintenance fields, line waypoints, stop cargo configs and vehicle-info fields.
  This port preserves the current Windows/Lua protocol rather than inventing
  fields peers do not read.
- The documented decode limits still apply: at most 64 vehicle parts/stops,
  256 config entries/automatic-load bits, eight alternative terminals, 255 name
  bytes, and nonnegative finite line waiting times no greater than 36,000.
- Counter-dependent clock capture is refused if the DoStep hook was not
  installed, a callback cannot be recognized, guarded counter writes are refused,
  or the bounded compensation table is full. A write failure after reserving a
  counter still schedules its compensation, because the blocked Add is followed
  by the UI's counter increment. Pause-toggle and calendar-slider
  capture do not need that counter hook.
- Strict line creation needs the verified immediate-sink hook. Unknown callbacks,
  unreadable payloads and a full eight-entry stash refuse the capture; no native
  create or read-back fallback is requested. Unclaimed callbacks expire after one minute; a
  callback whose replay throws across the native hook remains retained, because
  destroying it while the game's ownership is uncertain would be unsafe.

## Supplemental strict-create verification

The original Linux map predates release-0.4.22. The additional patch/layout facts
were checked against the same build-id binary on 2026-09-14:

- `0xa2d650` is the direct sink already traced in `SLICE_CORE.md` C-SCRIPT-3.
  It takes `_Any_data*`, `Command*`, `std::function*` in rdi/rsi/rdx, moves the
  command at `0xa2d67d`, applies it at `0xa2d698`, invokes the callback at
  `0xa2d6ab` and destroys the command at `0xa2d6b3`.
- Its first 18 bytes are
  `f3 0f 1e fa 55 48 89 e5 41 55 49 89 fd 41 54 49 89 d4`.
  The first 15 bytes end on `push r12`; none use RIP-relative addressing.
  Scanning rel32 calls/jumps/conditionals in both executable ELF segments and
  nearby rel8 branches found no raw branch pattern targeting the stolen interior.
- CreateLine's packaged data has Line at +0, name at +0x30, color at +0x50,
  player at +0x5c and result at +0x60. Stores at `0x15eff73`, `0x15eff80`,
  `0x15eff8c` and `0x15eff98` populate the last four fields before packaging.
  The recognized UI managers/invokers were already established by the line map:
  `0x10d44b0`/`0x10d6c30` and `0x10d8db0`/`0x10e1a60`.
