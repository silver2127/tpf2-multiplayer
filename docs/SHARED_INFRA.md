# Shared infrastructure prototype — 0.4.25

Companies keep ownership of infrastructure. Other companies are intended to run
services over the same tracks and use the same stations. There is no leasing fee
or Workshop mod dependency. This is an experimental implementation: live foreign
station selection, pathfinding, maintenance accounting and save/reload still need
verification with two instances. Offline mocks do not prove those engine behaviors.

The line editor's `AddStationInputComponentChecker::IsValidInput` (RVA 609b00)
does not contain a player-owner test in the examined decompilation. Its construction
selection helper is 21a9050. This supports trying owner-retaining sharing before
changing any UI permission checks; it does not establish that every downstream
routing or UI path permits foreign infrastructure.

## Changes

- Line ownership transfers change the line and its fleet, including parked
  vehicles. They bypass the stock `setPlayer(line, player)` behavior that also
  transfers stop stations, station constructions and track edges.
- Construction ownership transfers retain visiting vehicles' original owners.
  Company switches use the same scoped setters.
- Road and rail proposals use the originating company's engine player in their
  build context. This covers polylines whose result entity list is empty. Since
  the engine charges that player directly, the old refund/charge settlement is
  skipped for those proposals.
- Cancelled construction upgrades, construction demolition and road/track
  replacement captures refuse changes to foreign-owned infrastructure. Existing
  engine ownership restrictions remain intact. Edge demolition that already ran
  natively is still replicated; refusing its replay would create a divergence.
- Co-op uses the existing build context and stock ownership behavior.

The replacement guard also checks the other transport network because a road
proposal can refresh a rail bridge. It currently refuses replacement of a foreign
span even when the engine only intended to refresh it unchanged. Test connections
and roads beneath foreign bridges before treating this as production-ready sharing.

## Native extension

`setplayer_patch.cpp` retains the generic-entity assertion fix at RVA 11677a1.
An additional guarded 18-byte detour at 11673da runs **after** the binding has
initialized its cleanup object. The MASM relay recognizes an explicitly encoded
player argument (`0x60000000 + playerId`) and dispatches to the engine's generic
owner setter block at 11677a3. Ordinary arguments reconstruct the original
construction/line dispatch. Player IDs must fit in 28 bits.

The bridge advertises `entity_owner_v1=1` in its current identity file only after
installation succeeds. Lua will not send encoded arguments without that capability.
An identity-file flag avoids relying on separate CRT environment snapshots. The
new protocol version prevents connecting a 0.4.25 peer to older ownership behavior.

## Validation and next live test

`tools/shared_infra_test.py` checks ownership isolation, visiting depot vehicles,
permission checks through the real capture parser, capability failures, executable
instruction guards, and 14 paths through the actual assembled relay using Unicorn.
`tools/crossing_replay_test.py` additionally checks distinct per-peer company IDs in
real road proposal generation and verifies that empty result lists do not trigger
duplicate settlement. Line-create, bridge companion and version-gate checks pass.

For the live test, company A should build two stations and a connecting track.
Company B should create its own line using those stations, buy its own train and
run alongside A. Check both peers for the same logical owners, successful routes,
separate fleets and balances, and refusal of B's station upgrades/demolition.
Then test a company switch and save/reload, plus visiting aircraft in an airport
depot. Do not equate successful offline checks with these remaining live results.

The local update bundle is built separately from the running installed release;
building it does not activate it or restart either game.
