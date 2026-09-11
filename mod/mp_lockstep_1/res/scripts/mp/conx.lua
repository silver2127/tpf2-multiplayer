-- mp/conx.lua -- constructions: native replay (CONP / CONX), CONFAIL, LOAN
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.conx")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- Bulldoze one entity of a list gathered up front, only if it still exists.
-- Bulldozing a town building also removes its own asset groups (and can take a
-- neighbour with it), and game.interface.bulldoze on an entity that is already
-- gone is a NATIVE crash pcall cannot catch: 2026-09-11 the survivor-diff removed
-- town CONSTRUCTION 260032, then bulldozed its ASSET_GROUP 264852 from the same
-- list -- the engine printed "entity 264852" and the game died.
function CM.bulldozeAlive(id)
	if type(id) ~= "number" or id < 0 then return false end
	local alive = false
	pcall(function() alive = api.engine.entityExists(id) end)
	if not alive then return false end
	return (pcall(game.interface.bulldoze, id))
end
-- ---------- constructions: NATIVE replay (CONP / CONX) ----------
--
-- api.cmd.make.buildProposal DOES accept a script-built ConstructionEntity --
-- measured 2026-08-28 (E2c: ok=true, entity created) -- as long as params
-- carry a seed. The 'factory rejects script constructions' finding of
-- 2026-08-17 was a stripped seed, nothing more. So the peer now issues the
-- SAME proposal the originator's UI issued: the construction plus, for CONX,
-- the captured street vectors verbatim (nodes, edges, removed edges). The
-- engine then does its own snapping/integration exactly as it did on the
-- originator, which is what buildConstruction (template at raw coordinates,
-- no integration) could never reproduce. Only positive ids need mapping:
-- the removed street edge is found under the split position, and its two
-- endpoints stand in for the originator's endpoint ids.
-- forward: execConX retries itself once after clearing a colliding town building.
-- The retry must NOT run in the same tick as the clearing bulldozes: those are
-- async commands, so an immediate retry validates against a world where the
-- buildings STILL EXIST (measured: 'cleared 3 -- retrying' then an instant
-- second Collision, while the later diagnostic scan found the footprint empty).
-- ONE construction replay in flight at a time. Placements are asynchronous: a
-- batch dropped in the same tick has every proposal validated against the world
-- as it was at submit time, so two depots splitting the same road (or adjacent
-- pieces of it) fight -- the second removes an edge the first has already
-- replaced and the engine rejects it with critical=true and NO message.
-- Measured 2026-08-30: five depots all stamped at=6.8, numbers 1, 2 and 5 built,
-- 3 and 4 did not. Queueing costs a few tenths of a game unit and the peer's
-- world ends up identical either way.
CM.conxBusy, CM.conxBusyAt = false, nil
-- Construction replays run STRICTLY IN ORDER, oldest first. They used to share the
-- retry table, which the pump drained back-to-front, so a batch queued as 12,13,14,15
-- replayed 15,14,13,12 and every retry landed after later depots had already built --
-- each one then validated against a world its predecessor had not shaped yet, and the
-- engine rejected it with critical=true and no message ("builds land out of order",
-- 2026-08-30). Entries are { c = command, notBefore = game time }; a retry goes to the
-- FRONT so nothing newer overtakes it.
CM.conxQueue = {}
-- Reproduce the engine's auto-name for the CANCEL flow. A native placement is
-- named "<nearest town> <Type> depot", duplicates "<name> #N" (first no suffix,
-- second "#2"; verified from old logs: "Alsdorf Road depot", "...#2".."#10").
-- A script buildProposal does NOT auto-name, so with the native build cancelled
-- we build the string ourselves. Deterministic: nearest town, existing names and
-- build order are identical on the synced world in lockstep step order, so every
-- instance computes the same name -- and the name is a detail lane, never hashed,
-- so even a mismatch could only be cosmetic, never a desync.
function CM.depotName(x, y, file)
	local typ = tostring(file or ""):match("([^/]+)%.con$") or "construction"
	typ = typ:gsub("_era_.*$", ""):gsub("_", " "):gsub("^%l", string.upper)   -- "Road depot"
	local town
	pcall(function()
		local best, bestD
		for _, tid in pairs(game.interface.getEntities({ pos = { x, y }, radius = 4000 },
				{ type = "TOWN", includeData = false }) or {}) do
			local e = game.interface.getEntity(tid)
			local p = e and e.position
			local px = p and (p[1] or p.x)
			local py = p and (p[2] or p.y)
			if px and py then
				local d = (px - x) ^ 2 + (py - y) ^ 2
				if not bestD or d < bestD then bestD = d; best = tid end
			end
		end
		if best then town = game.interface.getName(best) end
	end)
	local base = (town and town ~= "") and (town .. " " .. typ) or typ
	-- lowest free "#N": slot 1 = base (no suffix), slot k>=2 = "base #k"
	local taken, pat = {}, "^" .. base:gsub("(%W)", "%%%1") .. " #(%d+)$"
	pcall(function()
		for _, cid in pairs(game.interface.getEntities({ pos = { x, y }, radius = 3000 },
				{ type = "CONSTRUCTION", includeData = false }) or {}) do
			local nm
			pcall(function() nm = game.interface.getName(cid) end)
			if nm == base then taken[1] = true
			elseif nm then local k = tostring(nm):match(pat); if k then taken[tonumber(k)] = true end end
		end
	end)
	if not taken[1] then return base end
	local k = 2
	while taken[k] do k = k + 1 end
	return base .. " #" .. k
end

-- (forward declaration of execConX moved into CM)
CM.execConX = function(c)
	local nowB = CM.gameTime() or 0
	if CM.conxBusy and CM.conxBusyAt and (nowB - CM.conxBusyAt) > 3.0 then
		log(string.format("CONX: previous replay never reported back (%.1f units) -- releasing the queue", nowB - CM.conxBusyAt))
		CM.conxBusy = false
	end
	if CM.conxBusy then
		CM.conxQueue[#CM.conxQueue + 1] = { c = c, notBefore = nowB }
		log(string.format("%s seq=%s: another construction replay is in flight -- queued (%d waiting)",
			tostring(c.op), tostring(c.seq), #CM.conxQueue))
		return
	end
	CM.conxBusy, CM.conxBusyAt = true, nowB
	local ok, err = pcall(function()
		local t = {}
		for tok in tostring(c.t or ""):gmatch("[^,]+") do t[#t + 1] = tonumber(tok) end
		if #t ~= 16 then log("CONX: bad transf, " .. #t .. " numbers"); return end
		local params = CM.deserParams(c.params) or {}
		local key = CM.conKey(t[13], t[14])
		-- CANCELLED PLACEMENT (c.cancelled=1): the native build
		-- never happened, so the originator builds the scripted proposal at the
		-- stamp exactly like a peer -- the strict bulldoze+rebuild below has
		-- nothing to replace and is skipped, and with no native split to reuse
		-- it splits the road itself (rm=1, add=2) like the peers do. That is the
		-- whole fix: all three instances reshape the road and clear the buildings
		-- on the SAME sim-step, so no vehicle is offset (the host-only drift,
		-- 2026-09-08). Self-correcting: if a native copy IS standing here (the
		-- cancel was reported but the build ran), take today's strict path
		-- instead of building a second depot.
		if c.origin == K.INSTANCE and tonumber(c.cancelled or 0) == 1 then
			local rec0 = CM.consByKey[key]
			if rec0 and rec0.id and api.engine.entityExists(rec0.id) then
				log(string.format("CONX seq=%s: cancelled placement but native construction %d stands at %s -- falling back to strict replace",
					tostring(c.seq), rec0.id, key))
				c.cancelled = 0
			else
				log(string.format("CONX seq=%s: cancelled placement -- originator builds the scripted proposal at the stamp like a peer", tostring(c.seq)))
			end
		end
		-- STRICT delete-and-replay on the ORIGINATOR. First
		-- pass: bulldoze our native copy and re-queue the build so the scripted
		-- proposal validates against the post-bulldoze world (bulldoze is async;
		-- an immediate rebuild would collide). Second pass (strictPhase set):
		-- fall through and build exactly like a peer.
		if c.origin == K.INSTANCE and c.strictPhase ~= "rebuilt" and tonumber(c.cancelled or 0) ~= 1 and CM.livePeers() == 0 then
			-- ALONE: the slice builds natively when no session is live (nothing to
			-- replay, nothing to cancel) and strict must follow the same rule.
			-- Bulldozing and rebuilding a station with nobody to match asserted
			-- the engine and crashed the game (live 2026-09-09, modular_station).
			log(string.format("CONX STRICT seq=%s: no live peer -- keeping the native build, nothing to match", tostring(c.seq)))
			CM.conxBusy = false
			return
		end
		if c.origin == K.INSTANCE and c.strictPhase ~= "rebuilt" and tonumber(c.cancelled or 0) ~= 1 then
			-- NEVER strict-replay a construction that REMOVES road edges (a depot
			-- splits the road it sits on; its payload carries removals). Bulldozing
			-- the native copy heals that split, so the shipped removal no longer
			-- resolves, the rebuild goes out with rm=0 and the engine refuses it
			-- ("Construction not possible", critical) -- and the CONFAIL rollback
			-- then DELETES the player's depot. Measured 2026-09-02: station = 0
			-- removals, rebuilds fine; road depot = 1 removal, rebuild fails and the
			-- depot is lost. Those keep today's proven behaviour (originator keeps
			-- its native copy); only removal-free constructions take the strict path.
			-- ROAD-EDGE-REMOVING CONSTRUCTIONS (a depot splits the road it sits on)
			-- used to be EXEMPT from strict: bulldozing the native copy leaves the
			-- split node and its two stubs behind, the rebuild then fought them,
			-- the engine refused it and the CONFAIL rollback deleted the player's
			-- depot. Keeping the native copy was the safe choice -- but it made
			-- the originator the ONE instance not running the replay path, and
			-- that is the depot-triggered vehicle drift: the native build and the
			-- peers' replay demolish the buildings under the depot by different
			-- mechanisms at different sim-times, the people count diverges at the
			-- very next stamp (872 vs 871, measured), and the bus that drives past
			-- inherits it as different boarding times. Both replaying peers stayed
			-- at 0.00 m from each other; only the native host drifted.
			--
			-- So the originator now takes the same path as the peers. The split is
			-- HEALED DETERMINISTICALLY right after the bulldoze (below), so by the
			-- time the rebuild runs the road is one merged edge again and the
			-- shipped removal position-resolves against it exactly as it does on a
			-- peer. And a refused self-rebuild no longer CONFAILs (see execConX's
			-- fallback): it keeps whatever stands rather than deleting the depot.
			local nRmShipped = 0
			for _ in tostring(c.srm or ""):gmatch("[^;]+") do nRmShipped = nRmShipped + 1 end
			c.strictHealsSplit = (nRmShipped > 0)
			local rec = CM.consByKey[key]
			if not (rec and rec.id and api.engine.entityExists(rec.id)) then
				log(string.format("CONX STRICT seq=%s: no native construction at %s to replace -- keeping native, no strict this time", tostring(c.seq), key))
				CM.conxBusy = false
				return
			end
			-- Balance right AFTER the native build and BEFORE the bulldoze: this
			-- is exactly what the peers hold (they paid for the build once). After
			-- the bulldoze+rebuild we restore the host to this value, so the
			-- native charge is the only construction cost that stands -- matching
			-- the peers. Local only (bookJournalEntry is not shipped).
			pcall(function() local e = game.interface.getEntity(api.engine.util.getPlayer()); if e then c.strictBalPre = tonumber(e.balance) end end)
			local bal0 = tostring(c.strictBalPre or "-")
			CM.expectedDemolish[key] = true     -- our own bulldoze: do not ship a DEMOLISH
			CM.expectedCons[key] = true         -- our rebuild will re-appear: not a new build
			local townBefore = CM.townCountNear(t[13], t[14], 200)
			local bok = pcall(game.interface.bulldoze, rec.id)
			CM.consByKey[key] = nil
			log(string.format("CONX STRICT seq=%s: town buildings within 200 m: %d before bulldoze, %d after",
				tostring(c.seq), townBefore, CM.townCountNear(t[13], t[14], 200)))
			-- Heal the split(s) our native build made, NOW, instead of leaving it
			-- to the sweep. The split node is the added node that lies on a
			-- removed edge's segment (same test shipConxPair uses to arm
			-- watchSplit). healNodeAt merges the two stubs back into one edge, so
			-- the rebuild's shipped removal finds a through-road under the split.
			-- NO HEAL. Merging the two native stubs back into one road demolished
			-- the buildings lining it -- the native depot build RESHAPED the road
			-- (it leaves an apron), so there is no clean original to restore and a
			-- road build always clears what its edge overlaps (four attempts:
			-- ignoreErrors, exact tangents, gatherBuildings all failed, 2026-09-08).
			-- Instead the removal loop below SNAPS the scripted split onto the
			-- existing native split node: the road is left exactly as the native
			-- build made it (which is also the peers' final topology -- they split
			-- there too), no road is rebuilt, and only the shared survivor-diff
			-- demolishes, so A converges to the peer building set.
			c.strictPhase = "rebuilt"
			CM.conxBusy = false
			CM.conxQueue[#CM.conxQueue + 1] = { c = c, notBefore = (CM.gameTime() or 0) + 0.6 }
			log(string.format("CONX STRICT seq=%s: bulldozed native construction %d (ok=%s, bal=%s) -- replaying scripted at +0.6", tostring(c.seq), rec.id, tostring(bok), bal0))
			return
		end
		if c.origin == K.INSTANCE and c.strictPhase == "rebuilt" and c.strictBalMid == nil then
			-- balance right after our bulldoze and before the rebuild charge
			pcall(function() local e = game.interface.getEntity(api.engine.util.getPlayer()); if e then c.strictBalMid = tonumber(e.balance) end end)
		end
		local isTrack = (tonumber(c.etype) or 0) == 1
		local stype = tonumber(c.stype) or 16

		local sp = api.type.SimpleProposal.new()
		local ce = api.type.SimpleProposal.ConstructionEntity.new()
		ce.fileName = tostring(c.file)
		ce.params = params
		ce.transf = api.type.Mat4f.new(
			api.type.Vec4f.new(t[1], t[2], t[3], t[4]),
			api.type.Vec4f.new(t[5], t[6], t[7], t[8]),
			api.type.Vec4f.new(t[9], t[10], t[11], t[12]),
			api.type.Vec4f.new(t[13], t[14], t[15], t[16]))
		ce.playerEntity = api.engine.util.getPlayer()
		-- The name goes IN the proposal. Measured (probe P9, 2026-08-28): with
		-- ce.name set, the apply gives the construction AND its child entities
		-- (VEHICLE_DEPOT / stations) their NAME and PLAYER_OWNED -- exactly a
		-- UI-built depot. Without it the child has neither, and the GUI select
		-- handler dereferences the missing NAME (client crash on click). An
		-- earlier run blamed the name for 'Construction not possible'; the real
		-- cause was the halves' street type.
		-- CANCEL flow: the engine did not auto-name (script build), so reproduce
		-- "<town> Road depot [#N]" ourselves (CM.depotName, deterministic on the
		-- synced world). Non-cancel paths keep the shipped/native name.
		if tonumber(c.cancelled or 0) == 1 then
			local dn
			pcall(function() dn = CM.depotName(t[13], t[14], c.file) end)
			ce.name = (dn and dn ~= "") and dn or CM.unescName(c.name)
		else
			ce.name = CM.unescName(c.name)
		end
		sp.constructionsToAdd[1] = ce

		-- street payload (absent for a free-standing CONP)
		local nodes, adds, rms, spos = {}, {}, {}, {}
		local rawPos = {}   -- every shipped street-node position: spans the footprint
		for tok in tostring(c.snodes or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 4 then
				nodes[f[1]] = { f[2], f[3], f[4] }
				rawPos[#rawPos + 1] = { f[2], f[3] }
			end
		end
		for tok in tostring(c.sedges or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			-- 8 fields (legacy) or 10 (+ bridge/tunnel type, typeIndex)
			if #f == 8 or #f == 10 then adds[#adds + 1] = f end
		end
		for tok in tostring(c.srm or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 8 then rms[#rms + 1] = f end
		end
		for tok in tostring(c.spos or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 4 then spos[f[1]] = { f[2], f[3], f[4] } end
		end

		local idmap, nRm = {}, 0
		local splitEdgeOf = {}    -- split node id -> the peer edge it splits
		-- Kept empty: snapping a split onto an existing node was tried and reverted
		-- (see the STUB NUDGE note below). The lookups downstream are no-ops now and
		-- are left in place only so the intent -- 'a split node may alias an existing
		-- one' -- stays visible if the idea is ever revisited with a way to keep the
		-- originator's topology.
		local snapNode = {}
		for _, r in ipairs(rms) do
			-- the split node: the new node with added edges to BOTH endpoints
			local X
			for id, _ in pairs(nodes) do
				local hitA, hitB = false, false
				for _, e in ipairs(adds) do
					if (e[1] == id and e[2] == r[1]) or (e[2] == id and e[1] == r[1]) then hitA = true end
					if (e[1] == id and e[2] == r[2]) or (e[2] == id and e[1] == r[2]) then hitB = true end
				end
				if hitA and hitB then X = id; break end
			end
			local p = X and nodes[X]
			-- ORIGINATOR strict: the road is ALREADY split natively at this exact
			-- point. Snap the scripted split node onto the existing native node so
			-- downstream reuses it (nodes[X] dropped), ships NO road removal, and
			-- attaches the depot connectors to it -- the road is never touched.
			if p and c.strictHealsSplit and c.origin == K.INSTANCE then
				local nid = CM.findNodeNear(isTrack, p[1], p[2], 1.5)
				if nid then
					snapNode[X] = nid
					log(string.format("CONX STRICT seq=%s: reusing native split node %d at (%.1f,%.1f) -- no heal, no road rebuild",
						tostring(c.seq), nid, p[1], p[2]))
				end
			end
			local eid
			if p and not snapNode[X] then pcall(function() eid = CM.findEdgeContaining(isTrack, p[1], p[2]) end) end
			if eid then
				local comp, a, b, ta, tb = CM.edgeGeomT(eid)
				if comp then
					-- orient: the removed edge's start tangent points from r[1]
					-- toward r[2]; our edge runs a -> b.
					local dot = (b[1] - a[1]) * r[3] + (b[2] - a[2]) * r[4]
					if dot >= 0 then idmap[r[1]], idmap[r[2]] = comp.node0, comp.node1
					else idmap[r[1]], idmap[r[2]] = comp.node1, comp.node0 end
					-- STUB GUARD. The originator split ITS road; this peer's road is
					-- nodded differently (town growth drifts the two worlds apart), so
					-- the same world position can land a metre or two from an existing
					-- node here. Splitting there leaves a 1-4 m stub and the engine
					-- rejects the whole proposal with critical=true and NO message --
					-- every failed depot measured on 2026-08-30 was exactly this
					-- (stubs of 1.2, 1.7, 1.9, 2.6, 3.1, 3.9 m). Snap to that node
					-- instead: no split, no halves, no removal, and the depot's own
					-- connector is re-pointed at it below -- the shape the UI itself
					-- produces when you place a depot next to a junction.
					-- Same rule, same reason as execPolyline's XING_END_SNAP.
					-- STUB NUDGE. The originator split ITS road; this peer's road can be
					-- nodded differently (town roads drift between the two worlds), so the
					-- same world position can land a metre or two from an existing node
					-- here and the split would leave a stub the engine refuses -- rejecting
					-- the whole proposal with critical=true and NO message.
					--
					-- SNAPPING to that node was tried first and is WRONG (2026-08-30): it
					-- skips a split the originator made, so the peer ends up with fewer
					-- road nodes, the worlds drift further, and the NEXT depot lands even
					-- closer to a node -- failures multiplied instead of stopping. Keep the
					-- topology identical (same nodes, same edges, same removal) and move
					-- the split point along the edge until both halves clear MIN_STUB.
					-- A couple of metres of driveway is invisible; a missing node is not.
					-- 12 m, not 6: two failures measured 2026-08-30 had their split clamped
					-- to EXACTLY 6.0 m from a node and the engine still answered
					-- 'Construction not possible' (harvested by the strict probe below),
					-- so the engine's own minimum is above that. If the peer's edge is too
					-- short to host a 12 m stub at both ends there is no valid split at all
					-- -- fall back to snapping the connector onto the nearer node, which
					-- costs one road node of topology but leaves a depot that is connected
					-- rather than absent.
					-- EXPERIMENT 2026-08-30: nudging is OFF (0 = keep the originator's exact
					-- split point). Every threshold tried so far -- 6 m, then 12 m -- still
					-- produced 'Construction not possible', and the last failure was a split
					-- the originator put 11.1 m from a node that the 12 m rule moved by
					-- 0.9 m: the rule was creating the mismatch it was meant to avoid (it
					-- also shifts the node's height off the depot's platform level). Replay
					-- the position as sent and measure the true failure rate before adding
					-- any correction back.
					local MIN_STUB = 0.0
					local function at(u) return CM.hermitePos(a, ta, b, tb, u) end
					local total = 0
					do
						local prev = at(0)
						for k = 1, 40 do
							local q = at(k / 40)
							total = total + math.sqrt((q[1] - prev[1]) ^ 2 + (q[2] - prev[2]) ^ 2)
							prev = q
						end
					end
					if total <= 2 * MIN_STUB + 1 then
						local dA0 = math.sqrt((p[1] - a[1]) ^ 2 + (p[2] - a[2]) ^ 2)
						local dB0 = math.sqrt((p[1] - b[1]) ^ 2 + (p[2] - b[2]) ^ 2)
						snapNode[X] = (dA0 < dB0) and comp.node0 or comp.node1
						log(string.format("CONX: peer edge %d is only %.0f m -- no room for a %.0f m stub either side, snapping the connector to node %d",
							eid, total, MIN_STUB, snapNode[X]))
					end
					if not snapNode[X] and total > 2 * MIN_STUB + 1 then
						local uLo, uHi = MIN_STUB / total, 1 - MIN_STUB / total
						local bestU, bestD
						for k = 0, 200 do
							local u = k / 200
							local q = at(u)
							local d = (q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2
							if not bestD or d < bestD then bestD, bestU = d, u end
						end
						local uu = math.max(uLo, math.min(uHi, bestU or 0.5))
						-- Cap how far the split may move. The depot's driveway still starts
						-- at the mouth the originator chose, so moving the road end of it
						-- stretches and skews that driveway: nudges of 0.9-3 m replayed
						-- fine, 11 m ones were refused ('Construction not possible', merge
						-- pairing distance d=11.17 m, 2026-08-30). Beyond the cap there is
						-- no faithful placement on this peer -- attempt the original split
						-- and let it fail loudly rather than build something distorted.
						local MAX_NUDGE = 3.0
						do
							local q = at(uu)
							if math.sqrt((q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2) > MAX_NUDGE then
								log(string.format("CONX: peer edge %d would need a %.1f m nudge (cap %.0f m) -- this peer's road is nodded differently, replaying the split as sent",
									eid, math.sqrt((q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2), MAX_NUDGE))
								uu = bestU or uu
							end
						end
						if math.abs(uu - (bestU or uu)) > 0.0001 then
							local q = at(uu)
							log(string.format("CONX: split point was %.1f m from an end of peer edge %d -- nudged %.1f m along it to keep both halves >= %.0f m",
								math.min(math.sqrt((p[1]-a[1])^2 + (p[2]-a[2])^2), math.sqrt((p[1]-b[1])^2 + (p[2]-b[2])^2)),
								eid, math.sqrt((q[1]-p[1])^2 + (q[2]-p[2])^2), MIN_STUB))
							p[1], p[2], p[3] = q[1], q[2], q[3]
							nodes[X] = p
						end
					end
					if not snapNode[X] then
						nRm = nRm + 1
						sp.streetProposal.edgesToRemove[nRm] = eid
						splitEdgeOf[X] = eid
						-- Watch the node this cut creates. Nothing owns a replayed split,
						-- so if the construction it was cut for never lands (or is later
						-- removed) the node stays behind for good -- see healNodeAt.
						CM.watchSplit(p[1], p[2])
					end
				end
			elseif not snapNode[X] then
				log(string.format("CONX: removed edge %d->%d: no peer edge under its split -- removal skipped", r[1], r[2]))
			end
		end
		for id, p in pairs(spos) do
			if not idmap[id] then
				local n = CM.findNodeNear(isTrack, p[1], p[2], 1.5)
				if n then idmap[id] = n end
			end
		end
		-- A shipped edge that ends on an EXISTING node the originator has and we
		-- do not (the topologies already differ -- a healed split, a town road
		-- that grew differently) cannot be built here. Building it anyway put a
		-- nil endpoint into the proposal and killed both peers on an engine
		-- assert (2026-09-02, depot attached to a node the peers had merged
		-- away). A missing depot is a visible c-lane divergence; a dead game is not.
		for id, p in pairs(spos) do
			if not idmap[id] then
				log(string.format("CONX seq=%s: existing node at %.1f,%.1f is not on this instance -- placement SKIPPED (DIVERGENCE, topology differs)",
					tostring(c.seq), p[1], p[2]))
				CM.conxBusy = false
				return
			end
		end

		-- Drop OUR copy of the template's own connector, because the template
		-- rebuilds it on the peer and shipping both duplicates it ('Collision').
		--
		-- That connector is a DEPOT-shaped payload: a couple of brand-new nodes
		-- and ONE both-new edge alongside a split of an existing road. A rail
		-- STATION is the opposite shape -- its whole platform network is
		-- both-new (25 nodes / 24 edges, no removals) and the template does NOT
		-- rebuild it, so dropping both-new edges there deleted every track and
		-- the station replayed bare (nodes=0 edges=0) and collided. So only
		-- apply the filter when the payload actually looks like a connector:
		-- there is a split to attach to (rms) and just a handful of edges.
		-- (This drops a rail station's ENTIRE platform network too -- measured:
		-- every station replays nodes=0 edges=0 -- and that is CORRECT: those
		-- tracks are template content the .con rebuilds on the peer, same as
		-- the depot's apron. Open-ground stations succeed exactly this way.)
		local used = {}
		local skippedApron = 0
		-- A snapped split node (see the STUB GUARD above) behaves like an EXISTING
		-- node from here on: the depot's own connector keeps it as its far end (that
		-- is the weld shape the merge adopts), while the two halves of the split that
		-- never happened are dropped along with their removal.
		local function isNew(id) return id < 0 and not snapNode[id] end
		local droppedHalves = 0
		for i = #adds, 1, -1 do
			local e = adds[i]
			if snapNode[e[1]] and e[2] >= 0 or snapNode[e[2]] and e[1] >= 0 then
				table.remove(adds, i); droppedHalves = droppedHalves + 1
			elseif isNew(e[1]) and isNew(e[2]) then
				table.remove(adds, i); skippedApron = skippedApron + 1
			else
				used[e[1]] = true; used[e[2]] = true
			end
		end
		if droppedHalves > 0 then
			log(string.format("CONX: dropped %d half/halves of a split that snapped to an existing node", droppedHalves))
		end
		for x, _ in pairs(snapNode) do nodes[x] = nil end
		for id, _ in pairs(nodes) do if not used[id] then nodes[id] = nil end end
		if skippedApron > 0 then
			log(string.format("CONX: %d apron edge(s) left to the template; shipping split node + halves", skippedApron))
		end
		local ni, minId = 0, 0
		for id, p in pairs(nodes) do
			ni = ni + 1
			local n = api.type.NodeAndEntity.new()
			n.entity = id
			n.comp.position = api.type.Vec3f.new(p[1], p[2], p[3])
			sp.streetProposal.nodesToAdd[ni] = n
			if id < minId then minId = id end
		end
		-- Placeholder ids share ONE namespace with the template's own, and the
		-- engine allocates the template's BELOW the lowest id already in use.
		-- The UI numbers -1..-5; a -100001 base pushed the template's nodes to
		-- -100004/-100005 and the apply asserted on
		-- 'it != result.result.boundingVolumes.end()' (create_proposal_data.cpp
		-- :897). Continue the UI's numbering: segments right after the nodes.
		local ei, dropped = 0, 0
		for _, e in ipairs(adds) do
			local n0 = snapNode[e[1]] or ((e[1] < 0) and e[1] or idmap[e[1]])
			local n1 = snapNode[e[2]] or ((e[2] < 0) and e[2] or idmap[e[2]])
			if n0 and n1 and n0 ~= n1 then
				ei = ei + 1
				local s = api.type.SegmentAndEntity.new()
				s.entity = minId - ei
				s.comp.node0 = n0
				s.comp.node1 = n1
				s.comp.tangent0 = api.type.Vec3f.new(e[3], e[4], e[5])
				s.comp.tangent1 = api.type.Vec3f.new(e[6], e[7], e[8])
				s.comp.type = 0
				s.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
				if e[9] == 1 or e[9] == 2 then   -- bridge / tunnel connector, from the capture's tail
					s.comp.type = e[9]
					s.comp.typeIndex = e[10] or -1
				end
				s.type = isTrack and 1 or 0
				if isTrack then
					s.trackEdge = api.type.BaseEdgeTrack.new()
					s.trackEdge.trackType = tonumber(c.ttype) or 1
					s.trackEdge.catenary = (tonumber(c.cat) or 0) == 1
					s.streetEdge = api.type.BaseEdgeStreet.new()
					s.streetEdge.streetType = 16
				else
					s.streetEdge = api.type.BaseEdgeStreet.new()
					s.streetEdge.streetType = stype
					s.streetEdge.hasBus = false
					s.streetEdge.tramTrackType = 0
				end
				-- A half of a split keeps the SPLIT edge's type, not the
				-- construction's: the UI's halves carry the town road's type
				-- 16 while the apron is 29.
				local se = splitEdgeOf[e[1]] or splitEdgeOf[e[2]]
				if se then
					CM.copyEdgeProps(s, se, isTrack, stype)
					-- ...and its TANGENTS come from THIS peer's copy of the split edge,
					-- never from the originator's. The shipped tangents describe the
					-- originator's edge, and the two worlds' town roads drift apart
					-- (different node spacing). Measured 2026-08-30: A split a 55 m
					-- edge, the peer's matching edge was 28 m, so the second half was
					-- 7 m long carrying 12 m tangents -- a Hermite that doubles back on
					-- itself. The engine rejected the whole proposal with critical=true
					-- and an EMPTY message list, so a depot placed near another one
					-- silently never appeared on the peer. Re-deriving from the local
					-- curve is identical when the worlds agree, and right when they do not.
					pcall(function()
						local xid = splitEdgeOf[e[1]] and e[1] or e[2]
						local xp = nodes[xid]
						local comp, aP, bP, taP, tbP = CM.edgeGeomT(se)
						if not (comp and xp) then return end
						local bestU, bestD
						for k = 0, 200 do
							local u = k / 200
							local q = CM.hermitePos(aP, taP, bP, tbP, u)
							local d = (q[1] - xp[1]) ^ 2 + (q[2] - xp[2]) ^ 2
							if not bestD or d < bestD then bestD, bestU = d, u end
						end
						if not bestU or bestU <= 0.001 or bestU >= 0.999 then return end
						-- Parameter of each endpoint along the peer's curve. The
						-- difference carries the sign, so a half stored end-to-start
						-- gets correctly negated tangents.
						local function uOf(nid)
							if nid == xid then return bestU end
							if nid == comp.node0 then return 0 end
							if nid == comp.node1 then return 1 end
							return nil
						end
						local u0, u1 = uOf(n0), uOf(n1)
						if not (u0 and u1) then return end
						local sc = u1 - u0
						if math.abs(sc) < 0.001 then return end
						local t0 = CM.hermiteTangent(aP, taP, bP, tbP, u0)
						local t1 = CM.hermiteTangent(aP, taP, bP, tbP, u1)
						s.comp.tangent0 = api.type.Vec3f.new(t0[1] * sc, t0[2] * sc, t0[3] * sc)
						s.comp.tangent1 = api.type.Vec3f.new(t1[1] * sc, t1[2] * sc, t1[3] * sc)
					end)
				end
				sp.streetProposal.edgesToAdd[ei] = s
			else
				dropped = dropped + 1
				log(string.format("CONX: edge %d->%d dropped (%s)", e[1], e[2],
					(n0 and n1) and "both ends resolve to the same node" or "an endpoint is unmapped"))
			end
		end

		CM.expectedCons[key] = true
		if c.company and CM.cmMode == "companies" then CM.cmExpectedCompany[key] = tonumber(c.company); CM.cmEnsure(); CM.cmExpectedBal0[key] = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
			CM.cmLog(string.format("CM: CONX bal0 snapshot key=%s mePid=%s bal0=%s", key, tostring(CM.cmCompanyPid[CM.cmMyCompany]), tostring(CM.cmExpectedBal0[key]))) end
		-- Context: nil, like the one native placement that has APPLIED (probe
		-- E2c). buildContext() forces checkTerrainAlignment=false; the UI's
		-- context carries it true, and its whole layout differs from ours.
		-- Retry after an obstacle clear passes ignoreErrors=true: the
		-- originator's UI already validated this exact placement, and what can
		-- remain (vegetation) is not enumerable to bulldoze first.
		-- ignoreErrors=true on the FIRST attempt too (2026-08-29). The peer used to
		-- pre-clear a guessed 60x30 m pad and over-demolished (B lost 20 more town
		-- buildings than A: 101 vs 121 within 200 m). The native tool demolishes
		-- exactly what its footprint collides with; ignoreErrors lets the engine do
		-- the same here, and the real-BOUNDING_VOLUME sweep after success catches
		-- anything modular layouts leave under the floor.
		-- The engine's octree pre-check (street_builder_util.cpp CheckGraph ->
		-- FUN_1421e2330, see docs/re/PROPOSALS.md) queries a
		-- +-0.01 m box around EVERY added node and fails SILENTLY -- no message, just
		-- critical=true -- when something is already there. Log how close each added
		-- node sits to an existing one so a rejection can be attributed instead of
		-- guessed at. 1 cm is the engine's own tolerance; anything under a metre is
		-- worth seeing.
		pcall(function()
			for i = 1, ni do
				local nn = sp.streetProposal.nodesToAdd[i]
				local px, py = nn.comp.position.x, nn.comp.position.y
				local near = CM.findNodeNear(isTrack, px, py, 1.0)
				if near then
					local nc = api.engine.getComponent(near, api.type.ComponentType.BASE_NODE)
					local d = nc and math.sqrt((nc.position.x - px) ^ 2 + (nc.position.y - py) ^ 2) or -1
					log(string.format("%s seq=%s: added node %s at (%.2f,%.2f) is %.3f m from EXISTING node %d%s",
						tostring(c.op), tostring(c.seq), tostring(nn.entity), px, py, d, near,
						(d >= 0 and d < 0.01) and "  <-- inside the engine's 0.01 m octree box, this alone refuses the build" or ""))
				end
			end
		end)
		local ctx = CM.conxContext()
		-- CANCEL FLOW: let the ENGINE demolish the footprint. There is no native
		-- build here, so gatherBuildings=false (conxContext's default, correct for
		-- the old survivor-diff flow) means NOBODY clears the overlapping town
		-- buildings and the depot builds straight through them (2026-09-08). The
		-- engine computes the exact demolish set at apply time from a collider/
		-- bounding-volume overlap (construction_builder_util::CreateProposalData,
		-- drains into ProposalData+0x1e0), gated by Context.gatherBuildings -- NOT
		-- present in the make-time proposal, so it cannot be shipped; it is
		-- re-derived. With gatherBuildings=true every instance runs that same
		-- collision on the same synced world and removes the IDENTICAL set:
		-- deterministic, and exactly what a native placement clears. (The old
		-- A-vs-B over-clear was a desync only because A built native and B swept;
		-- here all instances take this one path.)
		if ctx and tonumber(c.cancelled or 0) == 1 then
			pcall(function() ctx.gatherBuildings = true end)
		end
		-- ORIGINATOR STRICT REBUILD MUST NOT DEMOLISH. Its survivor list was
		-- gathered right after the NATIVE build and already SHIPPED. If this
		-- rebuild runs with ignoreErrors=true the engine does its own collision
		-- demolish -- a DIFFERENT set from the native build's -- and the
		-- originator ends up missing buildings that are on the list it sent:
		-- the peers keep them, the originator loses them, construction-lane
		-- DESYNC (measured: peer c kept four buildings at (1759,12)..(1805,23)
		-- that were in A's own shipped survivors, 2026-09-08). The native build
		-- already cleared what the depot collides with, so the rebuild has no
		-- legitimate reason to remove anything: ignoreErrors=false. Peers keep
		-- true -- their survivor-diff is what clears the footprint for them.
		local selfRebuild = (c.origin == K.INSTANCE and c.strictPhase == "rebuilt")
		local okMake, cmd = pcall(function() return api.cmd.make.buildProposal(sp, ctx, not selfRebuild) end)
		if (not okMake or not cmd) and ctx then
			log(string.format("CONX seq=%s: make.buildProposal refused with the UI context (%s) -- retrying with nil", tostring(c.seq), tostring(cmd)))
			okMake, cmd = pcall(function() return api.cmd.make.buildProposal(sp, nil, not selfRebuild) end)
		elseif ctx then
			log(string.format("CONX seq=%s: built with the UI context (terrain align + graph cleanup, charged)", tostring(c.seq)))
		end
		if not okMake or not cmd then
			-- Fallback: the old template path. Loses street integration but
			-- still puts the building down; logged loudly so it is visible.
			log(string.format("CONX: make.buildProposal refused (%s) -- falling back to buildConstruction",
				tostring(cmd)))
			params.seed = nil
			local built, newId = pcall(game.interface.buildConstruction, tostring(c.file), params, t)
			if built and newId then
				pcall(function() game.interface.setPlayer(newId, api.engine.util.getPlayer()) end)
			else
				CM.expectedCons[key] = nil
				if c.origin == K.INSTANCE then
					-- Our OWN strict self-rebuild failed. A CONFAIL here would ask us to
					-- delete the depot we just bulldozed for the rebuild -- deleting the
					-- player's work. Log loudly and stop; the peers built theirs.
					log(string.format("CONX STRICT seq=%s: self-rebuild refused and fallback failed -- NOT rolling back (worlds may differ)", tostring(c.seq)))
				else
					-- Even the template fallback could not place it: this peer will never
					-- have the building, so the originator must not keep its copy.
					CM.scheduleLocal("CONFAIL", { target = tostring(c.origin), x = t[13], y = t[14],
					                           file = tostring(c.file), failedSeq = tostring(c.seq) })
					log(string.format("CONX seq=%s: fallback failed too -- asked %s to roll its copy back",
						tostring(c.seq), tostring(c.origin)))
				end
			end
			log(string.format("EXEC %s seq=%s origin=%s at=%s file=%s ok=%s id=%s (fallback)",
				tostring(c.op), tostring(c.seq), tostring(c.origin), tostring(c.at),
				tostring(c.file), tostring(built and newId ~= nil), tostring(newId)))
			CM.conxBusy = false
			return
		end
		local seq, origin, at, file, op = c.seq, c.origin, c.at, c.file, c.op
		local strictBalPre = c.strictBalPre     -- nil unless this is our strict self-rebuild
		local strictBal0   = CM.conBal0[key]    -- balance before the native build
		local strictBalMid = c.strictBalMid     -- balance after our bulldoze
		-- COOP money reconciliation (see the post-sweep block below): on a
		-- PEER only, snapshot the wallet before the replay so we can match the
		-- originator's true cost once the build + obstacle-clear have settled.
		local reconBefore = nil
		if CM.cmMode == "coop" and origin ~= K.INSTANCE and c.cost then
			pcall(function() reconBefore = CM.cmBalance(api.engine.util.getPlayer()) end)
		end
		api.cmd.sendCommand(cmd, function(res, success)
			-- The engine has answered: the next queued construction may go.
			CM.conxBusy = false
			local ent = "?"
			pcall(function() ent = tostring(res.resultEntities[1]) end)
			log(string.format("EXEC %s seq=%s origin=%s at=%s file=%s nodes=%d edges=%d rm=%d dropped=%d success=%s ent=%s",
				tostring(op), tostring(seq), tostring(origin), tostring(at), tostring(file),
				ni, ei, nRm, dropped, tostring(success), ent))
			-- STRICT money reconciliation: the host paid for the native build,
			-- refunded the bulldoze and paid again for this rebuild; the peers
			-- paid once. Credit the net back so the host balance returns to the
			-- pre-bulldoze value (= the peers' balance). Balance-only journal
			-- (type 6), local, never shipped. A small window of unrelated income
			-- between the two reads rides along -- refined later if it matters.
			-- Refund EXACTLY what the native copy wasted: (balance before the
			-- native build) - (balance after the bulldoze) = native cost minus
			-- whatever the bulldoze gave back. The host is then left having paid
			-- only the SCRIPTED cost, the same as the peers. Crediting back to the
			-- pre-bulldoze balance was wrong: it refunded the scripted cost too,
			-- leaving the host richer by (scripted - native) -- measured +16054.
			if success and strictBalPre then
				pcall(function()
					if not (strictBal0 and strictBalMid) then
						log(string.format("CONX STRICT seq=%s: cannot reconcile money (bal0=%s mid=%s) -- host keeps a divergence",
							tostring(seq), tostring(strictBal0), tostring(strictBalMid)))
						return
					end
					local credit = strictBal0 - strictBalMid
					local cost = nil
					pcall(function() cost = tonumber(res.resultProposalData.costs) end)
					if credit ~= 0 then
						CM.cmBookJournal(api.engine.util.getPlayer(), credit, K.JOURNAL_TRANSFER)
					end
					local e = game.interface.getEntity(api.engine.util.getPlayer())
					log(string.format("CONX STRICT seq=%s: money reconciled %+d (native waste); bal0=%s mid=%s scriptedCost=%s now=%s",
						tostring(seq), credit, tostring(strictBal0), tostring(strictBalMid),
						tostring(cost), tostring(e and e.balance)))
				end)
			end
			if success then
				-- (companies-mode reassign happens in pollNewConstructions, which every
				-- replay path reaches -- NOT here, where the buildConstruction fallback
				-- path never arrives.)
				-- After a RETRIED (ignoreErrors) build: the pre-clear pads are an
				-- approximation -- a MODULAR station's module layout can put
				-- floor outside them (user report). The built entity's
				-- BOUNDING_VOLUME (component 56, probe P33) is the EXACT
				-- footprint for any layout: sweep town constructions and asset
				-- groups still under it.
				-- Sweep after EVERY successful build, not only a retried one (2026-08-29):
				-- with ignoreErrors on the first attempt the engine left 6 town
				-- buildings standing INSIDE the station's bounding box on B (A had
				-- demolished them), and the retry-gated sweep never ran.
				-- The BOUNDING_VOLUME sweep is DISABLED (2026-08-29): a modular station's
				-- bbox spans its whole module grid (~170x120 m), far beyond the built
				-- footprint. Sweeping it removed 13 town buildings on B that A's engine
				-- collision-demolish had KEPT (all 13 sat inside the bbox). The engine's
				-- own ignoreErrors demolish is the correct set, identical on A and B.
				-- TRACK-CORRIDOR CLEAR (2026-08-29). ignoreErrors on a raw proposal does
				-- NOT demolish colliding buildings the way the native tool does -- it
				-- just suppresses the error and builds THROUGH them: after an
				-- ignoreErrors build B had 7 town buildings standing on live platform
				-- track (each within 12 m of a station track node) that A's native
				-- placement had removed; the stations were otherwise identical (14
				-- modules, 28 track nodes). The whole-bbox sweep over-cleared (13
				-- buildings A kept, 30-40 m from any track). So: bulldoze exactly the
				-- town constructions/assets within CORRIDOR m of a track EDGE of the
				-- built station -- the platforms' true footprint.
				pcall(function()
					local bid = res.resultEntities[1]
					local co = api.engine.getComponent(bid, api.type.ComponentType.CONSTRUCTION)
					local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
					local bb = bv and bv.bbox
					if not (co and bb) then return end
					local CORRIDOR = 10
					-- the station's own track edges: track nodes inside its bbox
					local segs = {}
					local tmap = api.engine.system.streetSystem.getNode2TrackEdgeMap()
					local seen = {}
					for nid, edges in pairs(tmap) do
						local q = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE).position
						if q.x >= bb.min.x - 5 and q.x <= bb.max.x + 5 and q.y >= bb.min.y - 5 and q.y <= bb.max.y + 5 then
							for _, e in pairs(edges) do
								if not seen[e] then
									seen[e] = true
									local c = api.engine.getComponent(e, api.type.ComponentType.BASE_EDGE)
									local a = api.engine.getComponent(c.node0, api.type.ComponentType.BASE_NODE).position
									local b2 = api.engine.getComponent(c.node1, api.type.ComponentType.BASE_NODE).position
									segs[#segs + 1] = { a.x, a.y, b2.x, b2.y }
								end
							end
						end
					end
					local function distToSegs(px, py)
						local best = 1e9
						for _, sg in ipairs(segs) do
							local ax, ay, bx, by = sg[1], sg[2], sg[3], sg[4]
							local dx, dy = bx - ax, by - ay
							local L = dx * dx + dy * dy
							local t = L > 0 and math.max(0, math.min(1, ((px - ax) * dx + (py - ay) * dy) / L)) or 0
							local d = math.sqrt((px - (ax + t * dx)) ^ 2 + (py - (ay + t * dy)) ^ 2)
							if d < best then best = d end
						end
						return best
					end
					local cx, cy = (bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2
					local r = math.sqrt((bb.max.x - bb.min.x) ^ 2 + (bb.max.y - bb.min.y) ^ 2) / 2 + 10
					local cleared = 0
					-- EXACT path: the originator shipped the town buildings its world still
					-- has. Anything inside this station's bbox that is NOT on that list is a
					-- building the originator lost to the placement -> remove it here too.
					if c.survivors then
						local survPts = {}
						for sx, sy in tostring(c.survivors):gmatch("([%-%d%.]+):([%-%d%.]+)") do survPts[#survPts + 1] = { tonumber(sx), tonumber(sy) } end
						local function isSurvivor(px, py)
							for _, sp in ipairs(survPts) do if (px - sp[1]) ^ 2 + (py - sp[2]) ^ 2 <= 9 then return true end end
							return false
						end
							-- diff region: the WHOLE gather disk, no bbox term. The bbox gate was
							-- the wrong knob once the diff became survivor-keyed: a depot's native
							-- placement demolished two buildings 45-58 m from its centre (outside
							-- bbox+10) and the peer kept them (2026-08-29). Since the peer only
							-- removes what the originator's snapshot LACKS, the region is bounded by
							-- the snapshot's coverage, not by the footprint. Anchor on the built
							-- construction's own position (what gatherSurvivors used on the
							-- originator), 190 m inside the 200 m gather so nothing unlisted is judged.
							local gx, gy = cx, cy
							pcall(function()
								local bc = api.engine.getComponent(bid, api.type.ComponentType.CONSTRUCTION)
								if bc and bc.transf then gx, gy = bc.transf[13], bc.transf[14] end
							end)
							local DIFF_R = 190
							local inb = function(px, py)
								return (px - gx) ^ 2 + (py - gy) ^ 2 <= DIFF_R * DIFF_R
							end
							-- Sanity cap: a placement clears a handful of buildings. A diff wanting
							-- far more means the snapshot is stale/foreign -> log and refuse rather
							-- than level a town.
							local MAX_DIFF_REMOVALS = 40
							local victims = {}
							local kept = 0
							for _, id in pairs(game.interface.getEntities({ pos = { gx, gy }, radius = DIFF_R }, { type = "CONSTRUCTION", includeData = false }) or {}) do
								if id ~= bid then
									local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
									local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
									if cco and po == nil and cco.transf and inb(cco.transf[13], cco.transf[14]) then
										if isSurvivor(cco.transf[13], cco.transf[14]) then kept = kept + 1
										else victims[#victims + 1] = { id, cco.transf[13], cco.transf[14], "CONSTRUCTION" } end
									end
								end
							end
							for _, id in pairs(game.interface.getEntities({ pos = { gx, gy }, radius = DIFF_R }, { type = "ASSET_GROUP", includeData = false }) or {}) do
								local okE, e = pcall(game.interface.getEntity, id)
								local px = okE and e and e.position and (e.position[1] or e.position.x)
								local py = okE and e and e.position and (e.position[2] or e.position.y)
								if px and py and inb(px, py) and not isSurvivor(px, py) then
									victims[#victims + 1] = { id, px, py, "ASSET_GROUP" }
								end
							end
							if #victims > MAX_DIFF_REMOVALS then
								CM.cmLog(string.format("STN: %s seq=%s survivor-diff wants %d removals (> %d) -> REFUSED, snapshot looks stale", tostring(op), tostring(seq), #victims, MAX_DIFF_REMOVALS))
							else
								for _, v in ipairs(victims) do
									if CM.bulldozeAlive(v[1]) then cleared = cleared + 1
										CM.cmLog(string.format("STN: survivor-diff bulldozed town %s %d at (%.1f,%.1f)", v[4], v[1], v[2], v[3])) end
								end
							end
							CM.cmLog(string.format("STN: %s seq=%s survivor-diff: %d survivor(s) shipped, %d kept in %d m disk, %d removed", tostring(op), tostring(seq), #survPts, kept, DIFF_R, cleared))
							return
					end
					CM.cmLog(string.format("STN: %s seq=%s NO survivors shipped -> falling back to the %d m track corridor", tostring(op), tostring(seq), CORRIDOR))
					for _, id in pairs(game.interface.getEntities({ pos = { cx, cy }, radius = r }, { type = "CONSTRUCTION", includeData = false }) or {}) do
						if id ~= bid then
							local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
							local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
							if cco and po == nil and cco.transf and distToSegs(cco.transf[13], cco.transf[14]) <= CORRIDOR then
								if CM.bulldozeAlive(id) then cleared = cleared + 1
									CM.cmLog(string.format("STN: corridor-clear town CONSTRUCTION %d at (%.1f,%.1f)", id, cco.transf[13], cco.transf[14])) end
							end
						end
					end
					for _, id in pairs(game.interface.getEntities({ pos = { cx, cy }, radius = r }, { type = "ASSET_GROUP", includeData = false }) or {}) do
						local okE, e = pcall(game.interface.getEntity, id)
						local px = okE and e and e.position and (e.position[1] or e.position.x)
						local py = okE and e and e.position and (e.position[2] or e.position.y)
						if px and py and distToSegs(px, py) <= CORRIDOR then
							if CM.bulldozeAlive(id) then cleared = cleared + 1 end
						end
					end
					CM.cmLog(string.format("STN: %s seq=%s track-corridor clear: %d track segment(s), %d cleared within %d m", tostring(op), tostring(seq), #segs, cleared, CORRIDOR))
				end)
				-- COOP: match the originator's true cost now the sweep has settled.
				if reconBefore and c.cost then
					pcall(function()
						local after = CM.cmBalance(api.engine.util.getPlayer())
						if after then
							local delta = reconBefore - tonumber(c.cost) - after
							-- DETECT a real demolish asymmetry with the cost delta (3000 clears the
							-- benign ~1200 probe noise; a building demolish is ~200k). BOOK an
							-- absolute snap to the originator's post-build balance when we have it,
							-- so the coop wallets end EXACTLY equal rather than equal-in-spend from
							-- two differently-timed baselines (which left a ~3201 residual).
							if math.abs(delta) > 3000 then
								local book = delta
								if c.bal then book = tonumber(c.bal) - after end
								CM.cmBookJournal(api.engine.util.getPlayer(), book, K.JOURNAL_TRANSFER)
								log(string.format("CONX COOP seq=%s: money reconciled %+d (costDelta=%+d) to originator bal=%s cost=%s (before=%s after=%s)",
									tostring(seq), book, delta, tostring(c.bal), tostring(c.cost), tostring(reconBefore), tostring(after)))
							end
						end
					end)
				end
				if false then
					pcall(function()
						local bid = res.resultEntities[1]
						local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
						local bb = bv and bv.bbox
						if not bb then return end
						local bx0, by0 = bb.min.x - 3, bb.min.y - 3
						local bx1, by1 = bb.max.x + 3, bb.max.y + 3
						local scx, scy = (bx0 + bx1) / 2, (by0 + by1) / 2
						local scr = math.sqrt((bx1 - bx0) ^ 2 + (by1 - by0) ^ 2) / 2 + 5
						local swept = 0
						local near = game.interface.getEntities({ pos = { scx, scy }, radius = scr },
							{ type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							if id ~= bid then
								local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
								local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
								if cco and po == nil and cco.transf
									and cco.transf[13] >= bx0 and cco.transf[13] <= bx1
									and cco.transf[14] >= by0 and cco.transf[14] <= by1 then
									if CM.bulldozeAlive(id) then swept = swept + 1 end
								end
							end
						end
						local assets = game.interface.getEntities({ pos = { scx, scy }, radius = scr },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _, id in pairs(assets) do
							local okE, e = pcall(game.interface.getEntity, id)
							local px = okE and e and e.position and (e.position[1] or e.position.x)
							local py = okE and e and e.position and (e.position[2] or e.position.y)
							if px and py and px >= bx0 and px <= bx1 and py >= by0 and py <= by1 then
								if CM.bulldozeAlive(id) then swept = swept + 1 end
							end
						end
						if swept > 0 then
							log(string.format("%s seq=%s: post-build sweep removed %d leftover(s) under the real bounding box",
								tostring(op), tostring(seq), swept))
							CM.cmLog(string.format("STN: %s seq=%s post-build sweep removed %d leftover(s)", tostring(op), tostring(seq), swept))
						end
					end)
				end
				-- STN trace: how many town buildings remain near the station after the
				-- engine's own collision-demolish (ignoreErrors) -- compare A vs B.
				pcall(function()
					local bid = res.resultEntities[1]
					local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
					local bb = bv and bv.bbox
					if bb then
						local cx, cy = (bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2
						local left = 0
						local near = game.interface.getEntities({ pos = { cx, cy }, radius = 200 }, { type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							if api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED) == nil then left = left + 1 end
						end
						CM.cmLog(string.format("STN: %s seq=%s built (retried=%s); town constructions within 200 m of the station now: %d",
							tostring(op), tostring(seq), tostring(c.retried), left))
					end
				end)
				-- verification only: the proposal's name should have given the
				-- child its NAME and PLAYER_OWNED (probe P9)
				pcall(function()
					local eid = res.resultEntities[1]
					local co = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION)
					local nd = (co and co.depots) and #co.depots or 0
					local ns = (co and co.stations) and #co.stations or 0
					local child = (nd > 0 and co.depots[1]) or (ns > 0 and co.stations[1]) or nil
					local cn = child and api.engine.getComponent(child, api.type.ComponentType.NAME)
					local cp = child and api.engine.getComponent(child, api.type.ComponentType.PLAYER_OWNED)
					log(string.format("%s seq=%s: entity %s child %s childNAME=%s childOWNED=%s",
						tostring(op), tostring(seq), tostring(eid), tostring(child),
						tostring(cn ~= nil), tostring(cp ~= nil)))
				end)
				-- a reused id is already "known" and the poll would skip it for good
				pcall(function()
					local eid = res.resultEntities[1]
					if eid and CM.forgetKnownCon(eid) then
						log(string.format("%s seq=%s: entity %s reuses a known id -- the next poll adopts it",
							tostring(op), tostring(seq), tostring(eid)))
					end
				end)
			end
			if not success then
				CM.expectedCons[key] = nil
				-- Dump what we actually submitted. A depot replay that fails with
				-- critical=true and an EMPTY message list (2026-08-30) leaves no
				-- other evidence, and every theory about WHY has to be checked
				-- against the proposal rather than guessed from the outcome.
				pcall(function()
					log(string.format("%s seq=%s PROPOSAL: %d node(s) %d edge(s) %d removal(s), dropped=%d",
						tostring(op), tostring(seq), ni, ei, nRm, dropped))
					for i = 1, ni do
						local nn = sp.streetProposal.nodesToAdd[i]
						log(string.format("  node[%d] id=%s pos=(%.2f,%.2f,%.2f)", i, tostring(nn.entity),
							nn.comp.position.x, nn.comp.position.y, nn.comp.position.z))
					end
					for i = 1, ei do
						local se2 = sp.streetProposal.edgesToAdd[i]
						log(string.format("  edge[%d] id=%s %s->%s type=%s streetType=%s", i, tostring(se2.entity),
							tostring(se2.comp.node0), tostring(se2.comp.node1), tostring(se2.type),
							tostring(se2.streetEdge and se2.streetEdge.streetType)))
					end
					for i = 1, nRm do
						local reid = sp.streetProposal.edgesToRemove[i]
						local rc = reid and api.engine.getComponent(reid, api.type.ComponentType.BASE_EDGE)
						local p0 = rc and api.engine.getComponent(rc.node0, api.type.ComponentType.BASE_NODE)
						local p1 = rc and api.engine.getComponent(rc.node1, api.type.ComponentType.BASE_NODE)
						-- Is this edge FROZEN into a construction (its apron, or a piece a
						-- previous depot's weld adopted)? A plain street proposal cannot
						-- remove a construction-owned edge, and the engine rejects it with
						-- critical=true and NO message -- the exact shape seen when two
						-- depots are placed close together (2026-08-30).
						local owner, ownerFile = nil, nil
						pcall(function()
							local ax = p0 and p0.position.x or (rc and 0) or 0
							local ay = p0 and p0.position.y or 0
							for _, cid in pairs(game.interface.getEntities({ pos = { ax, ay }, radius = 80 },
									{ type = "CONSTRUCTION", includeData = false }) or {}) do
								local cc = api.engine.getComponent(cid, api.type.ComponentType.CONSTRUCTION)
								if cc and cc.frozenEdges then
									for _, fe in pairs(cc.frozenEdges) do
										if fe == reid then owner = cid; ownerFile = tostring(cc.fileName) end
									end
								end
							end
						end)
						if owner then
							log(string.format("  remove[%d] edge=%s is FROZEN into construction %s (%s) -- a street proposal cannot remove it",
								i, tostring(reid), tostring(owner), tostring(ownerFile)))
						end
						log(string.format("  remove[%d] edge=%s n0=%s%s n1=%s%s", i, tostring(reid),
							tostring(rc and rc.node0),
							p0 and string.format("(%.1f,%.1f)", p0.position.x, p0.position.y) or "",
							tostring(rc and rc.node1),
							p1 and string.format("(%.1f,%.1f)", p1.position.x, p1.position.y) or ""))
					end
				end)
				-- LAST RESORT + BISECTION. When even the retry failed, rebuild the SAME
				-- construction with NO street payload at all. Two things come out of it:
				-- the player gets their depot (unconnected, but present, instead of the
				-- building silently missing on this peer), and the result localises the
				-- fault -- if the construction alone builds, the rejection is in the
				-- street vectors we ship, not in the construction or its template.
				-- (docs/re/PROPOSALS.md narrows it to "something we add
				-- already exists and we did not remove it"; this says which half.)
				if c.retried and not c.bare then
					pcall(function()
						local sp2 = api.type.SimpleProposal.new()
						local ce2 = api.type.SimpleProposal.ConstructionEntity.new()
						ce2.fileName = tostring(c.file)
						ce2.params = params
						ce2.transf = api.type.Mat4f.new(
							api.type.Vec4f.new(t[1], t[2], t[3], t[4]),
							api.type.Vec4f.new(t[5], t[6], t[7], t[8]),
							api.type.Vec4f.new(t[9], t[10], t[11], t[12]),
							api.type.Vec4f.new(t[13], t[14], t[15], t[16]))
						ce2.playerEntity = api.engine.util.getPlayer()
						ce2.name = CM.unescName(c.name)
						sp2.constructionsToAdd[1] = ce2
						local cmd2 = api.cmd.make.buildProposal(sp2, nil, true)
						if not cmd2 then return end
						CM.expectedCons[key] = true
						api.cmd.sendCommand(cmd2, function(res3, ok3)
							local e3 = "?"
							pcall(function() e3 = tostring(res3.resultEntities[1]) end)
							log(string.format("%s seq=%s BARE probe (construction only, no street payload): success=%s ent=%s -- %s",
								tostring(op), tostring(seq), tostring(ok3), e3,
								ok3 and "the STREET payload is what the engine refuses" or "the CONSTRUCTION itself is refused here"))
							if not ok3 then
								CM.expectedCons[key] = nil
								if origin == K.INSTANCE then
									-- our own strict self-rebuild: never roll back the player's depot
									log(string.format("%s STRICT seq=%s: self-rebuild refused (bare too) -- NOT rolling back", tostring(op), tostring(seq)))
								else
									-- Nothing of this placement exists here. Tell the originator to
									-- undo its own copy so the worlds stay identical; it is the only
									-- side that can, and leaving it standing diverges us forever.
									CM.scheduleLocal("CONFAIL", { target = tostring(origin), x = t[13], y = t[14],
									                           file = tostring(c.file), failedSeq = tostring(seq) })
									log(string.format("%s seq=%s: asked %s to roll its copy back",
										tostring(op), tostring(seq), tostring(origin)))
								end
							else
								-- The bare construction stands but WITHOUT its road connection,
								-- while the originator has the connector edges: still a
								-- divergence, just a visible one. Roll both sides back.
								pcall(function() game.interface.bulldoze(tonumber(e3)) end)
								CM.expectedCons[key] = nil
								if origin ~= K.INSTANCE then
									CM.scheduleLocal("CONFAIL", { target = tostring(origin), x = t[13], y = t[14],
									                           file = tostring(c.file), failedSeq = tostring(seq) })
								else
									log(string.format("%s STRICT seq=%s: unconnected self-rebuild removed -- NOT rolling back the original", tostring(op), tostring(seq)))
								end
								log(string.format("%s seq=%s: bare copy removed again and %s asked to roll back -- an unconnected depot on one side only is still a divergence",
									tostring(op), tostring(seq), tostring(origin)))
							end
						end)
					end)
				end
				log(string.format("%s seq=%s DBG: proposal dump done -- entering failure diagnostics", tostring(op), tostring(seq)))
				-- WHY did it fail? The first attempt runs with ignoreErrors=TRUE, and in
				-- that mode the engine returns critical=true with an EMPTY message list,
				-- which tells us nothing. Re-submit the identical proposal with
				-- ignoreErrors=FALSE purely to harvest the message -- stricter than the
				-- attempt that already failed, so it cannot build anything by accident.
				pcall(function()
					log(string.format("%s seq=%s DBG: STRICT probe: about to make.buildProposal(sp, nil, false) -- RE-SUBMITS THE SAME sp (factory hook + MergeTemplateStreet run a 2nd time)", tostring(op), tostring(seq)))
					local strict = api.cmd.make.buildProposal(sp, nil, false)
					log(string.format("%s seq=%s DBG: STRICT probe: make returned %s", tostring(op), tostring(seq), tostring(strict ~= nil)))
					if not strict then return end
					log(string.format("%s seq=%s DBG: STRICT probe: about to sendCommand", tostring(op), tostring(seq)))
					api.cmd.sendCommand(strict, function(res2, ok2)
						local msgs = ""
						pcall(function()
							local es2 = res2.resultProposalData and res2.resultProposalData.errorState
							if es2 then
								for i = 1, #es2.messages do msgs = msgs .. " '" .. tostring(es2.messages[i]) .. "'" end
								for i = 1, #(es2.warnings or {}) do msgs = msgs .. " warn:'" .. tostring(es2.warnings[i]) .. "'" end
								msgs = msgs .. " critical=" .. tostring(es2.critical)
							end
						end)
						log(string.format("%s seq=%s STRICT probe: success=%s%s", tostring(op), tostring(seq), tostring(ok2), msgs))
					end)
				end)
				log(string.format("%s seq=%s DBG: STRICT probe: sendCommand issued (callback pending)", tostring(op), tostring(seq)))
				local collided = false
				pcall(function()
					log(string.format("%s seq=%s DBG: reading res.resultProposalData.errorState of the REFUSED result", tostring(op), tostring(seq)))
					local es = res.resultProposalData and res.resultProposalData.errorState
					log(string.format("%s seq=%s DBG: errorState read: %s", tostring(op), tostring(seq), tostring(es ~= nil)))
					if es then
						local msgs = ""
						pcall(function()
							for i = 1, #es.messages do
								local m = tostring(es.messages[i])
								msgs = msgs .. " '" .. m .. "'"
								if m:find("Collision", 1, true) then collided = true end
							end
						end)
						log(string.format("%s FAIL detail: critical=%s%s", tostring(op), tostring(es.critical), msgs))
					end
				end)
				log(string.format("%s seq=%s DBG: FAIL detail block done", tostring(op), tostring(seq)))
				-- The originator's game AUTO-DEMOLISHED the town buildings under
				-- the footprint (they are in the UI proposal's toRemove, which the
				-- replay cannot carry). Do the same here: bulldoze the UNOWNED
				-- (town) constructions overlapping the footprint, then retry once.
				-- Only unowned ones, and only on a Collision, so this can never
				-- eat a player's building.
				-- Retry on ANY failure, not just a "Collision" message. The first
				-- attempt already runs with ignoreErrors=true, so a failure here is
				-- a hard reject -- and a road depot dropped where the peer still had
				-- two ASSET_GROUPs under the footprint failed with critical=true and
				-- an EMPTY message list (2026-08-30), so the Collision-only gate did
				-- nothing at all and the depot never appeared on the peer. The clear
				-- itself is unchanged and stays conservative: unowned, non-survivor,
				-- inside the station-local footprint box, once.
				if not c.retried then
					if not collided then
						log(string.format("%s seq=%s: failure carried no 'Collision' message -- clearing the footprint anyway",
							tostring(op), tostring(seq)))
					end
					-- Clear by FOOTPRINT, not a fixed disc: a modular station
					-- extends far beyond 40 m of its centre, and with many
					-- buildings under it the outer ones survived the old clear,
					-- so the ignoreErrors retry built ON TOP of them (user
					-- report 2026-08-28). The shipped platform-track nodes span
					-- the whole footprint; use their bounding box, padded 25 m.
					-- STATION-LOCAL frame, not an axis-aligned box: the head
					-- building extends ~50 m past the last track node ALONG the
					-- station, while sideways 25 m already over-reaches. Measured
					-- (P32): survivors sat 17-27 m beyond the 25 m pad at the
					-- south end; an untouched town building sat 43 m to the west.
					local ux, uy = t[1], t[2]      -- local X axis (across)
					local vx, vy = t[5], t[6]      -- local Y axis
					local ul = math.sqrt(ux * ux + uy * uy); if ul > 0 then ux, uy = ux / ul, uy / ul end
					local vl = math.sqrt(vx * vx + vy * vy); if vl > 0 then vx, vy = vx / vl, vy / vl end
					local cx0, cy0 = t[13], t[14]
					local maxU, maxV = 10, 10
					for _, pp in ipairs(rawPos) do
						local dx, dy = pp[1] - cx0, pp[2] - cy0
						local pu = math.abs(dx * ux + dy * uy)
						local pv = math.abs(dx * vx + dy * vy)
						if pu > maxU then maxU = pu end
						if pv > maxV then maxV = pv end
					end
					local PAD_ALONG, PAD_ACROSS = 60, 30
					local limU = maxU + ((maxU >= maxV) and PAD_ALONG or PAD_ACROSS)
					local limV = maxV + ((maxV > maxU) and PAD_ALONG or PAD_ACROSS)
					local qx, qy = cx0, cy0
					local qr = math.sqrt(limU * limU + limV * limV) + 5
					-- Survivors the originator's game left standing: NEVER clear these.
					-- Matched by position (ids differ per peer), 3 m tolerance.
					local survPts = {}
					if c.survivors then
						for sx, sy in tostring(c.survivors):gmatch("([%-%d%.]+):([%-%d%.]+)") do
							survPts[#survPts + 1] = { tonumber(sx), tonumber(sy) }
						end
					end
					local function isSurvivor(px, py)
						for _, sp in ipairs(survPts) do
							local dx, dy = px - sp[1], py - sp[2]
							if dx * dx + dy * dy <= 9 then return true end
						end
						return false
					end
					local function inBox(px, py)
						if not px or not py then return false end
						if isSurvivor(px, py) then return false end
						local dx, dy = px - qx, py - qy
						return math.abs(dx * ux + dy * uy) <= limU
						   and math.abs(dx * vx + dy * vy) <= limV
					end
					log(string.format("%s seq=%s DBG: clear-footprint start", tostring(op), tostring(seq)))
					local cleared = 0
					pcall(function()
						-- Town CONSTRUCTIONS (buildings) inside the footprint box.
						local near = game.interface.getEntities({ pos = { qx, qy }, radius = qr },
							{ type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
							local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
							if cco and po == nil and cco.transf and inBox(cco.transf[13], cco.transf[14]) then
								log(string.format("%s seq=%s DBG: bulldozing town CONSTRUCTION %d at (%.1f,%.1f)", tostring(op), tostring(seq), id, cco.transf[13], cco.transf[14]))
								if CM.bulldozeAlive(id) then cleared = cleared + 1
									log(string.format("%s seq=%s DBG: bulldozed %d ok", tostring(op), tostring(seq), id))
									CM.cmLog(string.format("STN: pre-clear bulldozed town CONSTRUCTION %d at (%.1f,%.1f)", id, cco.transf[13], cco.transf[14])) end
							end
						end
						-- ASSET_GROUPs (trees, rocks, props): also cleared by the
						-- game on placement; not constructions, not edges.
						local assets = game.interface.getEntities({ pos = { qx, qy }, radius = qr },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _, id in pairs(assets) do
							local okE, e = pcall(game.interface.getEntity, id)
							local px = okE and e and e.position and (e.position[1] or e.position.x)
							local py = okE and e and e.position and (e.position[2] or e.position.y)
							if inBox(px, py) then
								if CM.bulldozeAlive(id) then cleared = cleared + 1 end
							end
						end
					end)
					-- Retry once EVEN IF nothing was cleared. Builds and bulldozes are
					-- async: two depots placed in the same tick split the same road, and
					-- the second proposal is validated against a world where the first
					-- split has not landed yet -- it removes an edge that is already
					-- gone and fails with critical=true and no message (measured
					-- 2026-08-30: seq=3 and seq=4 both stamped t=41, the first built,
					-- the second did not). Re-running rebuilds the proposal from the
					-- CURRENT world, so the retry resolves the edge that actually
					-- exists by then.
					if cleared > 0 then
						log(string.format("%s seq=%s: cleared %d town obstacle(s) under the footprint -- retry in 1.5",
							tostring(op), tostring(seq), cleared))
						CM.cmLog(string.format("STN: %s seq=%s pre-clear: %d obstacle(s) bulldozed, %d survivor(s) protected (box %.0fx%.0f m)",
							tostring(op), tostring(seq), cleared, #survPts, 2 * limU, 2 * limV))
					else
						log(string.format("%s seq=%s: nothing to clear -- retrying anyway in case the world was mid-change",
							tostring(op), tostring(seq)))
					end
					local again = {}
					for k, v in pairs(c) do again[k] = v end
					again.retried = 1
					log(string.format("%s seq=%s DBG: retry queued (+1.5) -- a fresh make.buildProposal from the CURRENT world follows", tostring(op), tostring(seq)))
					table.insert(CM.conxQueue, 1, { c = again, notBefore = (CM.gameTime() or 0) + 1.5 })
				end
				-- Diagnostic: what player/town constructions sit near the footprint?
				-- A station placed over town buildings auto-demolishes them on the
				-- originator; if the replay collides, these are the obstacles.
				pcall(function()
					local near = game.interface.getEntities({ pos = { t[13], t[14] }, radius = 40 },
						{ type = "CONSTRUCTION", includeData = false }) or {}
					local n = 0
					for _, id in pairs(near) do
						local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
						if cco and cco.transf then
							n = n + 1
							if n <= 8 then
								local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
								log(string.format("%s obstacle: %s @%.1f,%.1f owned=%s", tostring(op),
									tostring(cco.fileName), cco.transf[13], cco.transf[14], tostring(po ~= nil)))
							end
						end
					end
					local na = 0
					pcall(function()
						local ag = game.interface.getEntities({ pos = { t[13], t[14] }, radius = 40 },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _ in pairs(ag) do na = na + 1 end
					end)
					log(string.format("%s FAIL: %d construction(s), %d asset group(s) within 40m of the footprint",
						tostring(op), n, na))
				end)
			end
		end)
	end)
	-- Any early return inside the body (bad transf, no command built) leaves the
	-- gate held; the watchdog above would clear it after 3 units, but releasing
	-- it here keeps the queue moving. A live sendCommand has already cleared it.
	if not ok then log("execConX error: " .. tostring(err)); CM.conxBusy = false end
end

-- The peer could not build a construction this instance placed: undo it here so
-- the two worlds stay identical. Sent by the peer as CONFAIL with target = the
-- ORIGINATING instance's letter; only that instance acts on it. Without this a
-- refused replay left the building standing on one side forever -- a permanent,
-- silent divergence that every later command near it inherited.
function CM.execConFail(c)
	if tostring(c.target) ~= K.INSTANCE then return end
	local ok, err = pcall(function()
		local key = CM.conKey(c.x, c.y)
		-- EXACT match only. The first version took the nearest player construction
		-- within 5 m, and with depots placed a few metres apart it rolled back a
		-- NEIGHBOUR that the peer had built fine -- so the peer kept that depot and
		-- its road split while this side lost both (edge counts 1116 vs 1114 with
		-- construction counts equal, 2026-08-30). The originator placed this exact
		-- construction from this exact transform, so its own copy is at the position
		-- the peer echoed back, to the centimetre. Prefer our own registry entry;
		-- otherwise accept only a same-file construction within 1 m. No candidate
		-- means it is already gone -- never guess at a neighbour.
		local best, bestD
		local rec = CM.consByKey[key]
		local recAlive = false
		if rec and rec.id then pcall(function() recAlive = api.engine.entityExists(rec.id) end) end
		if recAlive then
			best, bestD = rec.id, 0
		else
			for _, id in pairs(game.interface.getEntities({ pos = { c.x, c.y }, radius = 5 },
					{ type = "CONSTRUCTION", includeData = false }) or {}) do
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
				if co and po and co.transf and (not c.file or tostring(co.fileName) == tostring(c.file)) then
					local dx, dy = co.transf[13] - c.x, co.transf[14] - c.y
					local d = dx * dx + dy * dy
					if d < 1.0 and (not bestD or d < bestD) then best, bestD = id, d end
				end
			end
		end
		if not best then
			log(string.format("EXEC CONFAIL seq=%s: no %s of ours within 1 m of %.1f,%.1f -- already gone, nothing rolled back",
				tostring(c.seq), tostring(c.file), c.x, c.y))
			return
		end
		-- Our own removal detector must not echo this back to the peer as a
		-- player demolish: mark the spot first, exactly as execDemolish does.
		CM.expectedDemolish[key] = true
		CM.consByKey[key] = nil
		if CM.cmMode == "companies" then
			local hasCon = false
			pcall(function() hasCon = api.engine.getComponent(best, api.type.ComponentType.CONSTRUCTION) ~= nil end)
			if hasCon then pcall(game.interface.setBulldozeable, best, true) end
		end
		local done = pcall(game.interface.bulldoze, best)
		log(string.format("EXEC CONFAIL seq=%s: %s rolled back locally (entity %d at %.1f,%.1f) -- the peer refused it",
			tostring(c.seq), tostring(c.file), best, c.x, c.y))
		if not done then log(string.format("EXEC CONFAIL seq=%s: bulldoze REFUSED -- worlds now differ", tostring(c.seq))) end
	end)
	if not ok then log("execConFail error: " .. tostring(err)) end
end

-- LOAN v=<absolute loan of the origin company>. Loans are money and money is
-- world state, but there is no slice hook for the finances window: the loan is
-- POLLED instead (CM.pollLoan, every 15 ticks) and a change ships the new
-- absolute value. Each peer books the difference to whichever player stands for
-- that company here -- the origin's AI player in companies mode, the shared
-- player in co-op -- as a loan-category journal entry, which the engine treats
-- as taking/repaying a loan (measured: loan and balance both move). Absolute,
-- not delta, so a lost or doubled command converges instead of compounding. The
-- originator's own copy finds delta 0 and does nothing.
function CM.execLoan(c)
	local v = tonumber(c.v)
	if not v then log("LOAN: no value"); return end
	-- The ORIGINATOR already moved its own loan -- the player did it in the
	-- finances window; this command exists to tell the OTHER instances. Applying
	-- it here too was meant to be a no-op via delta 0, but the loan keeps moving
	-- between the poll that ships it and the replay a stamp later, so the delta
	-- was not 0 and we booked it: 20,000,000 -> 18,500,000 on the originator, a
	-- change the player never made. That fed the poll, which shipped again, and
	-- the loan walked up to the 30,000,000 cap while the peers sat at 11,000,000
	-- and went into the red (2026-09-03). Skip, exactly as every other channel
	-- skips its originator.
	if c.origin == K.INSTANCE then
		log(string.format("LOAN seq=%s: originator already set it locally (%s), skipping",
			tostring(c.seq), tostring(v)))
		CM.lastLoan = v
		return
	end
	CM.cmEnsure()
	local me = api.engine.util.getPlayer()
	local pid = me
	if CM.cmMode == "companies" then pid = CM.cmCompanyPid[tonumber(c.company)] end
	if not pid then log("LOAN: no player for company " .. tostring(c.company)); return end
	local cur = nil
	pcall(function() cur = tonumber(game.interface.getEntity(pid).loan) end)
	if not cur then log("LOAN: cannot read loan of pid " .. tostring(pid)); return end
	local delta = math.floor(v - cur + 0.5)
	if pid == me then
		-- Our own loan is about to change under us; the poll must not ship that
		-- back as if the player did it, or the two instances echo each other.
		-- A fixed two-poll countdown was not enough: cmBookJournal CHUNKS at
		-- 10,000,000 and these loans run to 30,000,000, so the move lands as
		-- several async journal entries spread over many ticks. The poll then
		-- caught a HALF-APPLIED loan, shipped it as a player action, the peer
		-- applied that, and the two ping-ponged -- gaps swinging by millions
		-- until the host pinned at the loan cap (30,000,000 vs 21,000,000
		-- measured 2026-09-03). Wait for the value to actually arrive instead
		-- of guessing how long it takes.
		CM.lastLoan = v
		CM.loanExpect = v
		CM.loanExpectSince = CM.ticks
	end
	if delta == 0 then
		log(string.format("EXEC LOAN seq=%s origin=%s co%s: already %d", tostring(c.seq), tostring(c.origin), tostring(c.company), v))
		return
	end
	local ok, err = CM.cmBookJournal(pid, delta, K.JOURNAL_LOAN)
	local after = nil
	pcall(function() after = tonumber(game.interface.getEntity(pid).loan) end)
	log(string.format("EXEC LOAN seq=%s origin=%s co%s pid=%s: %d -> %d (delta %+d) ok=%s%s now=%s",
		tostring(c.seq), tostring(c.origin), tostring(c.company), tostring(pid), cur, v, delta,
		tostring(ok), err and (" " .. tostring(err)) or "", tostring(after)))
end
end
