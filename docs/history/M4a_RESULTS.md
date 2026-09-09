# M4a — replication capture + transport: COMPLETE (live-tested)

## What works (all proven in one live session)

1. **Capture**: hooks on `0xa16d00` (transform snapshot, rcx = 4x4 float
   matrix, per preview frame), `0xa18ca0` (params snapshot, rdx = params
   block, per frame), `0x9e76e0` (applyProposal commit → emit event).
   NOTE: first build used WRONG targets (0xa152e0/0xa182a0) due to a probe8
   id-mapping slip (id=4 → 0xa16d00, id=8 → 0xa18ca0). Corrected; verified.
2. **Transport**: `src/net.cpp` — our own reliable-ordered UDP (seq/ack/
   ackbits, 250ms resend, in-order delivery, 20ms net thread). Survived a
   peer restart: queued event delivered on resend. Zero dependencies.
3. **End-to-end**: depot built in-game → event in peer console:
   identity rotation matrix, position (0, 25, 0) [decode refinement TBD —
   likely relative transform, not world coords], params containing the
   type ids 26/31 matching probe8 recon.

## Protocol lesson (fix in M4b)

No session handshake: when the game restarted, its seq restarted at 0 and
the peer dropped packets as "duplicates". M4b needs a session id /
handshake so receivers reset state per sender session.

## Files

- `src/net.{h,cpp}` — reliable UDP, shared between DLL and peer test
- `src/capture.{h,cpp}` — snapshot + commit hooks (blob/relay, M2 infra)
- `src/dllmain.cpp` — init, config (`tpf2_mp.cfg`), port auto-fallback +10
- `src/hook.{h,cpp}`, `src/applyrelay.asm` — M2 detour machinery
- `peer_test/main.cpp` — standalone console peer (loopback test)
- `build.bat` (DLL+peer), `relink_b/c/d.bat` (relink while old DLL locked
  in a running game — link to a NEW filename each dev iteration)

## Next: M4b (remote re-application)

- Session handshake (above).
- Refine position decode (f[13]=25.0 — map the transform layout vs world
  coordinates; probably anchor-relative).
- Remote side: receive event → call make_proposal(params, transform) →
  applyProposal at aligned tick. Calling convention of make_proposal chain
  needs one debugger session (or careful reading of the M1 disasm).
- Then the deferred M3 determinism test before real 2-player.
