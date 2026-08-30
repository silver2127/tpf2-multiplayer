# Networked multi-company MP: "Multiplayer Companies" × our networking

A far cleaner road to real multiplayer than shared-company lockstep. Marries the
**company model** from Swiss's "Multiplayer Companies" mod (workshop 3710243057 —
the same item hosting our DLLs) with **our netpunch/lobby transport + command
replication**. Each player, on their own machine, runs their own company in the
same map, simultaneously.

## What the mod already proves (companies.lua, 3207 lines)

The engine natively supports multiple companies — this is NOT faked:
- `game.interface.addPlayer()` creates a new player entity at runtime; each has a
  real `balance`. The mod runs 3 companies as 1 human + 2 AI players (`MAX_COMPANIES=6`).
- `api.engine.getComponent(eid, PLAYER_OWNED).player` reads an entity's owner;
  `game.interface.setPlayer(eid, pid)` reassigns it.
- `api.cmd.make.bookJournalEntry(pid, entry, vec)` books money to a SPECIFIC
  player (the mod seeds inactive companies' AI players this way — i.e. it already
  moves money to a non-active player, in shipped code).
- `game.interface.setBulldozeable(eid, false)` gives engine-level "Owned by
  another company" protection.
- GUI→engine command channel: `game.interface.sendScriptEvent("__cm__", name,
  param)` → the game script's `handleEvent(src,id,name,param)` applies it
  authoritatively (switchCompany / tagConstructions / tagEdges / lease / ...).
- Ownership attribution: new player-built entities are owned by the active human;
  the script polls, tags them with the active company, and reassigns via setPlayer.

Its ONLY stated limitation: *"No actual multiplayer networking — local hot-seat
only."* That is exactly our built layer.

## The key simplification for networked play

Hotseat has ONE active (human) company and swaps it by keyboard (`swapActiveCompany`
— heavy: iterates all owned entities). **Networked does NOT need swapping:** on
each machine the LOCAL player's company is permanently the human/active one;
every REMOTE player's company is a fixed AI player entity locally. Mirrored per
machine — company A is "human" on A's PC, an "AI player" on B's PC. We never call
the expensive swap.

## Apply model (the crux — how a remote command lands under the right company)

Engine build/buy commands execute as the ACTIVE (human) player. So to apply an
incoming command from remote company C on the local machine (whose active company
is L ≠ C), reuse the mod's proven **build-as-active-then-reassign** pattern:
1. Apply the replicated command → the new entity is created owned by the local
   human (L). (Our replication already locates new entities by position — see the
   key registries / position matching in lockstep.lua.)
2. `setPlayer(newEntity, aiPlayerIds[C])` + tag `ownedX[eid]=C` +
   `setBulldozeable(false)` — hand it to C's player. (Exactly companies.lua's
   tag/reassign flow, just triggered by a network command, not local play.)
3. Move the cost: `bookJournalEntry(humanL, +cost)` refund L, `bookJournalEntry(
   aiC, -cost)` charge C. (bookJournalEntry-to-a-specific-pid is proven.)

For the local player's OWN commands: they build as human (their company), stay on
human, and our capture ships them to peers where step 1–3 run.

## Architecture

- **Lobby (ours)** assigns each connected player a company id 1..N (N≤6). Each
  machine: `humanPlayerId = getPlayer()` = its own company; `aiPlayerIds[remote]
  = addPlayer()` for every other player.
- **Capture (ours)** tags each locally-issued command with the local company id
  and ships it over the lobby connection (existing transport).
- **Apply (ours + theirs)** runs the 3-step apply above, reusing companies.lua's
  ownership/wallet helpers.
- **Two game scripts coexist**: `lockstep.lua` (capture/replay + bridge file-IPC)
  and `companies.lua` (company model/UI). Integration options: (a) drive
  companies.lua via `sendScriptEvent("__cm__", ...)` events for attribution
  (loose coupling, reuses its handleEvent), or (b) fold the company model into our
  own mod for one coherent script we control (preferred long-term). Start with (a)
  to prototype.

## Why this beats shared-company lockstep

Separate companies = **separate state**. A divergence in company A's exact vehicle
positions does not corrupt company B, and cross-company interference is
engine-blocked. So the perfect-determinism requirement — the root of the
demolish/world-hash/save-transfer/load-RE complexity — **largely disappears**. We
only need each company's ACTIONS to appear on the other screens, attributed
correctly. The map itself is static terrain (could even be seeded identically to
avoid the 180 MB save transfer entirely).

## Spike status

Core mechanisms — ALREADY PROVEN by the shipped mod (all in companies.lua):
- add companies at runtime ✅ (`addPlayer`)
- money to a non-active company ✅ (`bookJournalEntry(pid, …)`, used to seed AI players)
- reassign ownership to any company ✅ (`setPlayer`)
- read ownership ✅ (`PLAYER_OWNED.player`), engine-level protection ✅ (`setBulldozeable`)
- GUI→engine command channel ✅ (`sendScriptEvent`/`handleEvent`)

Networked-specific validations that still need a LIVE game:
- R1 apply-then-reassign for each command family (build, buyVehicle, line ops):
  confirm the new entity is locatable and reassigns cleanly. Low risk (mod does
  the vehicle/line/construction reassign already).
- R2 cost transfer: get each command's cost (or measure the human's balance delta
  right after apply) to move it to the right company. Medium.
- R3 command families that DON'T create a new entity (upgrades, sell, line edits)
  — attribute/authorize by existing ownership instead. Medium.
- R4 script coexistence + event ordering between lockstep.lua and companies.lua.
  Medium.

## Next steps

1. Prototype company assignment from the lobby (company id per player) + on each
   machine create AI players for the others (addPlayer) at session start.
2. Route ONE command family end-to-end (station build is the richest): capture →
   ship → apply → reassign to sender's company → cost transfer. Validate R1/R2
   live with two instances.
3. Then generalize to vehicles/lines; drop the shared-lockstep determinism
   machinery that this model no longer needs.
