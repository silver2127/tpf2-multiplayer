# Native dedicated loading follow-up, September 22, 2026

This follows the native parity merge (PR #5). Terrain kernel work lives in
the Big Maps `linux-load-perf` branch / PR #6 and is installed as an optional
native plugin in performance-only mode.

## Startup grace

`dedicated_load_delay_sec` now controls the minimum time at the title menu
before automatic dedicated loading. Accepted values are 8 through 120 seconds;
invalid values retain 45. The default remains 45 for untested installations.
The initial host delay (8 seconds), lobby readiness, game-loading and native
I/O checks, and retry delay are unchanged. This setting does not affect an
in-session join/resync load.

The VPS operator can set `LOAD_DELAY_SEC=15` in `server.env` and run
`tpf2server configure`; its generated native flags preserve that setting.
The private native VPS successfully loaded with 15 seconds in repeated runs.
With the optional Big Maps kernels, launch-to-world times were 90.11 and
92.12 seconds. The prior min/max + copy run with 45 seconds took 114.09 seconds.
These are separate whole startup runs, not an isolated kernel speedup proof.

## Graphics experiment

The server already suppresses rendering submissions and Vulkan query waits.
Its settings disable terrain tessellation, SSAO and shadows and use the lowest
geometry/texture quality. The remaining renderer initialization cannot be
assumed safe to omit: render-data vectors still have CPU consumers.

The existing experimental `dedicated_nowsi=1` path was tested privately with
the same fast terrain kernels and 15-second grace. It loaded in 94.10 seconds,
versus 92.12 with normal presentation. It substantially reduced reported
process I/O writes, but did not improve load time in this comparison. Keep it
off for the production load-time configuration.

## Validation and limits

- The native Soldier build passed all 52 existing CTest suites after the delay
  implementation changed.
- The dedicated settings test also checks 15-second parsing, the 45-second
  default and rejection of out-of-range/malformed delay values.
- Steam remains offline; tests use a separate display, ports and userdata.
- Desktop-client joining requires a later check when the test machine returns.
- Persistent terrain calculation caching and its exact-output tests are
  documented in Big Maps `docs/linux/LOAD_PERFORMANCE.md`; it is optional and
  must be judged by whole-load timings rather than cache hit counts.

## In-session comparison

A held/paused private world saved successfully in 4.271 seconds. Loading that
same 117,305,444-byte file in one process took 69.484 seconds with the new
terrain kernels, 73.711 with stock refinement/alignment restored, then 72.371
with the kernels restored again. Min/max and row-copy stayed enabled. All
loads reported `world_ready` and accepted subsequent hold/pause requests.
The observed reload gain is small; the shorter startup grace does not shorten
this path. Disk caching remained slower even after revision (118.19 seconds
for a warm whole startup), so it is disabled in the VPS configuration.

## Production

Deployment saved the current live world as `mp_o_6ab2acae` before stopping
the idle server. Backups are under
`/opt/tpf2mp-linux-parity-20260921/production-before-perf-1790094515`.
The running server uses `LOAD_DELAY_SEC=15`, the tested Big Maps native
performance-only configuration, no disk terrain cache, and normal presentation.
Steam remains offline. The original source-based lobby launcher is preserved.

The live restart took 193.50 seconds, with slower rendering initialization
than the warmed private test environment. The native controller reports
`has_world=1`, `busy=0`; lobby TCP/UDP 29472, bridge 7771 and relay 7773 listen.
No end-to-end desktop join was claimed while the test machine is offline.

Deployed menu SHA-256:
`d839cf0e66e9ce539cfab8a3f78f9fb75e7be1237b142e2939384018b672d0db`.
