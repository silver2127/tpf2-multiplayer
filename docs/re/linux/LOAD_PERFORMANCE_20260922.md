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
