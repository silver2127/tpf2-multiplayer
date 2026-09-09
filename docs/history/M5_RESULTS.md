# M5 — Bidirectional replication loop: PROVEN (live, on-screen)

## The loop (all legs verified on the map)

1. User builds a depot in-game.
2. Lua bridge mod (capture-by-diff via `getEntities` CONSTRUCTION) detects it
   with fully-resolved `fileName` + world `transf` (16 floats, per-index).
3. Capture file → bridge DLL tails it → our reliable-UDP stack → fake remote.
4. Fake remote parses, offsets x by +50, sends back (same protocol).
5. Bridge DLL writes peer events file → Lua replays via
   `game.interface.buildConstruction` → **twin depot appears on the map.**

Verified RX/TX in `remote_rx.log`; replay confirmation in game stdout.

## Final architecture (much simpler than the hook-based design)

- **Lua does ALL engine interaction** (supported API only):
  capture-by-diff + `buildConstruction` replay.
- **DLL is a dumb pipe**: file tail ↔ reliable UDP ↔ file write.
  No hooks required for the core loop (M2 hook work remains available for
  action types the diff can't see, and for tick alignment later).
- **Transport**: `src/net.cpp` — seq/ack/ackbits, session id handshake,
  resend, in-order delivery. Zero dependencies.

## Lessons recorded

- `applyProposal`-level capture obsoleted by capture-by-diff: no preview
  noise, fully-resolved metadata, supported API.
- The proposal object at commit time contains processed model data, NOT
  fileName/transf — scanning it was a dead end (2KB dump: "RootNode" blocks).
- `SimpleProposal.constructionsToAdd` → make.buildProposal throws on content
  validation in every configuration tried (10+ harness iterations);
  `game.interface.buildConstruction(fileName, params, transf16)` is the
  working replay API (returns new entity id).
- Bridge must truncate its events file per session (stale events replayed).
- One bridge per process! Dev iterations left two stacked → duplicate events
  (visible as echo-twin collisions "internal error").

## Remaining for real 2-player (M6)

1. Two machines (single-instance lock blocks two TpF2 on one PC).
2. More event types: demolish (diff detects disappearance → match by
   fileName+position), vehicle buy/sell (diff vehicles), line create/edit
   (diff lines). Same capture-by-diff pattern.
3. Identity/ownership: which player owns what (the hotseat mod's company
   model can ride on top).
4. Determinism validation (M3) for sim coherence beyond constructions.
5. Dedup/session polish: single bridge instance, reconnect handling.
