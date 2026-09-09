-- mp/vehicles.lua -- vehicles: cross-peer identity, names/colours, vehicle commands, buy, replace
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.vehicles")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- vehicles: cross-peer identity ----------
--
-- Entity ids differ between instances, so nothing on the wire names a vehicle
-- by id. A replicated purchase gets a shared KEY, origin:seq; each peer records
-- which LOCAL vehicle that purchase produced -- the vehicle in the target depot
-- that was not known before -- and later commands ship keys. A vehicle that was
-- in the save carries the same id on both peers (same file) and is keyed s:<id>.
local vehIdOf
CM.vehKeyOf, vehIdOf = {}, {}       -- localId -> key, key -> localId
local knownVeh = {}                    -- every vehicle id seen in a depot so far
CM.primedVeh = {}                   -- vehicles that existed at load: same ids on both peers
local pendingVehKeys = {}              -- { key=, depot=<child>, since= }
local vehPrimed = false

-- Vehicles parked in a construction's depot. NOT game.interface.getDepotVehicles:
-- that call errors for the construction AND for the VEHICLE_DEPOT child
-- (measured, probe P10), which is why every purchase key timed out. A parked
-- vehicle's getEntity().depot is the CONSTRUCTION id (mp_bridge depotPosOf),
-- so enumerate vehicles and filter.
-- A depot-parked vehicle is NOT a world entity: getEntities(type="VEHICLE")
-- never lists it (measured: 79 before and after a purchase) and
-- game.interface.getDepotVehicles errors for construction and child alike.
-- transportVehicleSystem.getVehiclesWithState(IN_DEPOT) does list them; the
-- vehicle's TRANSPORT_VEHICLE.depot names its depot -- accept the construction
-- id or any of its VEHICLE_DEPOT children, whichever the engine stores.
local function depotVehicles(constructionId)
	local ids = {}
	pcall(function()
		local accept = { [constructionId] = true }
		local co = api.engine.getComponent(constructionId, api.type.ComponentType.CONSTRUCTION)
		if co and co.depots then
			for i = 1, #co.depots do accept[co.depots[i]] = true end
		end
		local tvs = api.engine.system.transportVehicleSystem
		local parked = tvs.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
		for i = 1, #parked do
			local v = parked[i]
			local ok, tv = pcall(function() return api.engine.getComponent(v, api.type.ComponentType.TRANSPORT_VEHICLE) end)
			if ok and tv and accept[tv.depot] then ids[#ids + 1] = v end
		end
	end)
	-- NOT sorted by id. Entity ids come from each instance's own free list, so
	-- id order is not creation order and differs per instance (measured
	-- 2026-09-01: a batch of 8 trucks bound key a:5 to entity 42827 on A and
	-- key a:4 to 42827 on the peer). The engine's enumeration order is kept.
	return ids
end

local function vehPurchaseTime(vid)
	local pt = nil
	pcall(function()
		local tv = api.engine.getComponent(vid, api.type.ComponentType.TRANSPORT_VEHICLE)
		local v0 = tv and tv.transportVehicleConfig and tv.transportVehicleConfig.vehicles and tv.transportVehicleConfig.vehicles[1]
		if v0 then pt = tonumber(v0.purchaseTime) end
	end)
	return pt
end

local function registerVehKey(key, vid)
	CM.vehKeyOf[vid] = key
	vehIdOf[key] = vid
	knownVeh[vid] = true
	log(string.format("veh: %s <-> local vehicle %d purchaseTime=%s", key, vid, tostring(vehPurchaseTime(vid))))
end

-- s:<id> is only valid for a vehicle both peers loaded from the save. A new
-- vehicle whose purchase key never registered must NOT fall back to its id:
-- the same number on the peer is a different vehicle (or none -- measured:
-- the peer's sellVehicle threw on a stale id).
function CM.vehKeyFor(vid)
	if CM.vehKeyOf[vid] then return CM.vehKeyOf[vid] end
	if CM.primedVeh[vid] then return "s:" .. tostring(vid) end
	log(string.format("veh: local vehicle %s has no cross-peer key -- not shipped", tostring(vid)))
	return nil
end

local function vehIdFor(key)
	if vehIdOf[key] then return vehIdOf[key] end
	local s = tostring(key):match("^s:(%-?%d+)$")
	local id = s and tonumber(s) or nil
	if id and CM.primedVeh[id] then return id end
	return nil
end

-- Prime knownVeh from every player depot once constructions are primed.
function CM.primeVehKeys()
	-- consByKey fills at K.PRIME_PER_TICK per tick after consPrimed; priming the
	-- depot lists before that drained saw an empty table ('primed 0'), and a
	-- purchase would then have resolved to an OLD parked vehicle. Wait for the
	-- queue, and treat every save vehicle as known regardless.
	if vehPrimed or not CM.consPrimed or #CM.primeQueue > 0 then return end
	vehPrimed = true
	local n = 0
	pcall(function()
		local all = game.interface.getEntities({ radius = 999999 },
			{ type = "VEHICLE", includeData = false }) or {}
		for _, v in pairs(all) do CM.primedVeh[v] = true; knownVeh[v] = true end
	end)
	local np = 0
	for _ in pairs(CM.primedVeh) do np = np + 1 end
	log(string.format("veh: primed %d save vehicle(s) as known / s:<id>", np))
end

-- forward: CM.shipVehCap/drainVehCap (below) sell through forgetVehicle, which
-- is defined further down; without this the name resolves to a nil global and
-- a mass sell crashed the originator (2026-09-02, drainVehCap:forgetVehicle nil).
local forgetVehicle
-- (forward declaration of lineKeyFor moved into CM)
-- Vehicle-key-dependent captures (VLINE, VSELL) whose keys were not bound at
-- capture time. A batch buy binds keys asynchronously, so an assign or sell
-- issued in the same breath must WAIT for the binding rather than drop the
-- vehicle. Retried every tick until every id resolves or the window closes.
CM.pendVehCap = {}
K.VEHCAP_WAIT = 8.0        -- game units to keep retrying before giving up
function CM.deferVehCap(entry)
	-- try once now; only queue if something is unresolved
	if CM.shipVehCap(entry) then return end
	CM.pendVehCap[#CM.pendVehCap + 1] = entry
end

-- Returns true when the entry is fully shipped (or definitively empty), false
-- if it still has unresolved ids and should be retried.
function CM.shipVehCap(entry)
	if entry.kind == "VLINE" then
		local k = entry.id and CM.vehKeyFor(entry.id)
		local lk = entry.line and CM.lineKeyFor(entry.line)
		if k and lk then
			log(string.format("VLINE: %s -> line %s stop %d", k, lk, entry.stop))
			CM.scheduleLocal("VLINE", { key = k, line = lk, stop = entry.stop, armed = entry.armed or 0 })
			return true
		end
		return false
	elseif entry.kind == "VSELL" then
		local keys, unresolved = {}, false
		for _, id in ipairs(entry.ids) do
			local k = CM.vehKeyFor(id)
			if k then keys[#keys + 1] = k else unresolved = true end
		end
		-- ship only when EVERY id is resolved, so the batch stays one atomic
		-- sale on all peers; a partial ship would split the money differently
		if unresolved then return false end
		if #keys > 0 then
			local armed = tonumber(entry.armed or 0)
			log(string.format("VSELL: %s%s", table.concat(keys, ","), armed == 1 and " (strict)" or ""))
			CM.scheduleLocal("VSELL", { keys = table.concat(keys, ","), armed = armed })
			-- Forget the ids only if the sale already ran natively here. Under
			-- strict the vehicles still exist; the replay's own callback forgets
			-- them on success (execVehCmd), and forgetting now would make our
			-- own replay fail to resolve the keys it is about to sell.
			if armed ~= 1 then for _, id in ipairs(entry.ids) do forgetVehicle(id) end end
		end
		return true
	end
	return true
end

function CM.drainVehCap()
	if #CM.pendVehCap == 0 then return end
	local now = CM.gameTime() or 0
	local i = 1
	while i <= #CM.pendVehCap do
		local e = CM.pendVehCap[i]
		if CM.shipVehCap(e) then
			table.remove(CM.pendVehCap, i)
		elseif now - (e.since or now) > K.VEHCAP_WAIT then
			if e.kind == "VLINE" then
				log(string.format("VLINE: vehicle %s never keyed within %.0fs -- assignment DROPPED (DIVERGENCE)", tostring(e.id), K.VEHCAP_WAIT))
			else
				local miss = {}
				for _, id in ipairs(e.ids or {}) do if not CM.vehKeyFor(id) then miss[#miss + 1] = tostring(id) end end
				-- ship whatever DID resolve at the deadline rather than lose the whole sale
				local keys = {}
				for _, id in ipairs(e.ids or {}) do local k = CM.vehKeyFor(id); if k then keys[#keys + 1] = k end end
				local armed = tonumber(e.armed or 0)
				if #keys > 0 then
					-- Strict: the unkeyed ones were cancelled here too, so they are
					-- LOST everywhere (the player re-sells them) -- a lost action,
					-- not a divergence. Native: they were sold here only.
					log(string.format("VSELL: shipping %d of %d at the deadline; unkeyed [%s] %s", #keys, #e.ids, table.concat(miss, ","),
						armed == 1 and "LOST on every instance (re-sell them)" or "stay LOCAL (DIVERGENCE)"))
					CM.scheduleLocal("VSELL", { keys = table.concat(keys, ","), armed = armed })
					if armed ~= 1 then for _, id in ipairs(e.ids) do if CM.vehKeyFor(id) then forgetVehicle(id) end end end
				else
					log(string.format("VSELL: %d id(s), none keyed within %.0fs -- %s", #e.ids, K.VEHCAP_WAIT,
						armed == 1 and "sale LOST on every instance (re-sell them)" or "sale stays LOCAL (DIVERGENCE)"))
				end
			end
			table.remove(CM.pendVehCap, i)
		else
			i = i + 1
		end
	end
end

-- Resolve pending purchase keys: the depot's vehicle that is not yet known.
function CM.pollVehKeys()
	if #pendingVehKeys == 0 then return end
	local now = CM.gameTime()
	if not now then return end
	-- Oldest pending key first, each taking the smallest unknown id: two
	-- purchases landing in one tick keep their identities in order.
	local resolved = {}
	for i = 1, #pendingVehKeys do
		local p = pendingVehKeys[i]
		local fresh, total = {}, 0
		local ents = {}
		for idx, v in ipairs(depotVehicles(p.depot)) do
			total = total + 1
			if not knownVeh[v] then
				local pt = nil
				pcall(function()
					local tv = api.engine.getComponent(v, api.type.ComponentType.TRANSPORT_VEHICLE)
					local v0 = tv and tv.transportVehicleConfig and tv.transportVehicleConfig.vehicles and tv.transportVehicleConfig.vehicles[1]
					if v0 then pt = tonumber(v0.purchaseTime) end
				end)
				ents[#ents + 1] = { id = v, idx = idx, pt = pt or 0 }
			end
		end
		-- WHICH new vehicle is this key? The key names the N-th purchase, so it
		-- must bind to the N-th CREATED vehicle on every instance -- the same
		-- physical truck in the same depot slot. Ascending entity id was wrong
		-- (ids are recycled differently per instance). Order of preference:
		--   1. the entity the buy command itself returned (peer replay; exact),
		--   2. purchase time, then the engine's enumeration order (originator's
		--      native buys; creation order as far as the API shows it).
		if p.hint and not knownVeh[p.hint] then
			local hinted = nil
			for _, e in ipairs(ents) do if e.id == p.hint then hinted = e end end
			if hinted then ents = { hinted } end
		end
		table.sort(ents, function(a, b)
			if a.pt ~= b.pt then return a.pt < b.pt end
			return a.idx < b.idx
		end)
		for _, e in ipairs(ents) do fresh[#fresh + 1] = e.id end
		if #ents > 1 then
			local parts = {}
			for _, e in ipairs(ents) do parts[#parts + 1] = string.format("%d@%s#%d", e.id, tostring(e.pt), e.idx) end
			log(string.format("VEHORDER %s: %d candidates [id@purchaseTime#enum] %s -> %d%s", p.key, #ents,
				table.concat(parts, " "), fresh[1], p.hint and (" (hint " .. tostring(p.hint) .. ")") or ""))
		end
		if #fresh >= 1 then
			registerVehKey(p.key, fresh[1])
			-- companies mode: a remote company's purchase landed on our player;
			-- hand the vehicle over and move the cost (balance delta since apply).
			if p.company and CM.cmMode == "companies" then
				-- A vehicle ON A LINE inherits its line's owner; setPlayer on it
				-- trips a FATAL engine assert (interface.cpp:2340, seen live). The
				-- Companies mod's rule (v10 followsLine): reassign the LINE, skip the
				-- vehicle. Only a depot/unassigned vehicle needs its own setPlayer.
				local onALine = false
				pcall(function()
					local tv = api.engine.getComponent(fresh[1], api.type.ComponentType.TRANSPORT_VEHICLE)
					onALine = (tv ~= nil and tv.line ~= nil and tv.line ~= -1 and tv.line ~= 0)
				end)
				if onALine then CM.cmLog(string.format("CM: vehicle %d is on a line -> follows its line, setPlayer skipped", fresh[1]))
				else CM.cmReassignEntity(fresh[1], p.company, "vehicle") end
				local nowBal = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
				if p.bal0 and nowBal then CM.cmTransferCost(p.company, p.bal0 - nowBal, "VBUY " .. p.key) end
			end
			resolved[#resolved + 1] = i
		elseif now - p.since > 6 then
			log(string.format("veh: key %s never produced a vehicle in depot %s (%d listed) -- dropped",
				p.key, tostring(p.depot), total))
			resolved[#resolved + 1] = i
		end
	end
	for k = #resolved, 1, -1 do table.remove(pendingVehKeys, resolved[k]) end
end

-- A sold vehicle's key must not outlive it: entity ids get reused.
function forgetVehicle(vid)
	local key = CM.vehKeyOf[vid]
	if key then vehIdOf[key] = nil end
	CM.vehKeyOf[vid] = nil
end

local function expectVehicle(key, depotChild, company, hint)
	-- companies mode: remember the origin company and our balance BEFORE the
	-- purchase lands, so the bind step can hand the vehicle over and move the
	-- exact cost (balance delta) to that company.
	local bal0 = (company and CM.cmMode == "companies") and CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany]) or nil
	pendingVehKeys[#pendingVehKeys + 1] = { key = key, depot = depotChild, since = CM.gameTime() or 0, company = company, bal0 = bal0, hint = hint }
end

-- The host's own buy is shipped one tick LATE, on purpose: once the new
-- vehicle exists here, so the wire carries its real purchaseTime. The native
-- buy applies at the click's sim-step and cannot be cancelled (the depot
-- window waits for it); the peers' replay applies at the stamp, ~3 steps
-- later -- the one host-only difference left in a buy, and purchaseTime is
-- what a vehicle keeps from its creation step. B and C (both replays) stay at
-- 0.00 m with each other while A drifts after a batch buy (2026-09-02); giving
-- every instance the same purchaseTime is the cheapest test of that link.
CM.parkedBuys = {}
function CM.shipParkedBuys()
	if #CM.parkedBuys == 0 then return end
	local now = CM.gameTime() or 0
	local claimed = {}
	local i = 1
	while i <= #CM.parkedBuys do
		local pb = CM.parkedBuys[i]
		local found, pt = nil, nil
		pcall(function()
			local best
			for _, v in ipairs(depotVehicles(pb.depot)) do
				if not knownVeh[v] and not claimed[v] then
					local vpt = vehPurchaseTime(v) or 0
					if not best or vpt < best.pt then best = { id = v, pt = vpt } end
				end
			end
			if best then found, pt = best.id, best.pt end
		end)
		if found then
			claimed[found] = true
			pb.args.pt = pt or -1
			CM.scheduleLocal("VBUY", pb.args)
			-- our own new vehicle gets the same key the peer will use; the hint
			-- binds exactly this one even when several are fresh
			expectVehicle(K.INSTANCE .. ":" .. tostring(CM.seqNo), pb.depot, nil, found)
			log(string.format("VBUY: shipped once vehicle %d existed, purchaseTime=%s (%.1f s after the click)",
				found, tostring(pt), now - pb.since))
			table.remove(CM.parkedBuys, i)
		elseif now - pb.since > 1.5 then
			CM.scheduleLocal("VBUY", pb.args)
			expectVehicle(K.INSTANCE .. ":" .. tostring(CM.seqNo), pb.depot)
			log("VBUY: no new vehicle seen in the depot within 1.5 s -- shipped without purchaseTime")
			table.remove(CM.parkedBuys, i)
		else
			i = i + 1
		end
	end
end

-- ---------- vehicles: BuyVehicle replication ----------
--
-- Optimistic-local like constructions: the originator's buy proceeds natively
-- (never cancelled -- a cancelled BuyVehicle is untested territory and a wrong
-- vehicle type in a depot is an uncatchable native assert), and the peer buys
-- the SAME config into the depot found at the SAME position. Model ids travel
-- as file names, depots as positions; nothing on the wire is an entity id.
-- Vehicle identity for later commands (sell / line / send-to-depot) is a
-- separate problem, not solved here.
-- ---------- names and colours ----------
--
-- SetName and SetColor take an entity, and an entity id means nothing on the
-- other machine -- so what travels is the same key the vehicle and line channels
-- already use, or a position for a construction. kind says which registry to ask,
-- because a vehicle and a line can hold the same number.
local function targetFor(kind, key)
	if kind == "veh" then return vehIdFor(key) end
	if kind == "line" then return CM.lineIdFor(key) end
	if kind == "con" then
		local rec = CM.consByKey[key]
		if rec and rec.id then
			local alive = false
			pcall(function() alive = api.engine.entityExists(rec.id) end)
			if alive then return rec.id end
		end
		local kx, ky = tostring(key):match("^(%-?[%d%.]+)/(%-?[%d%.]+)$")
		if kx then return CM.constructionAt(tonumber(kx), tonumber(ky)) end
	end
	return nil
end

function CM.execSetName(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		local id = targetFor(tostring(c.kind or ""), tostring(c.key or ""))
		if not id then
			log(string.format("VNAME seq=%s: no local %s for key %s -- skipped",
				tostring(c.seq), tostring(c.kind), tostring(c.key)))
			return
		end
		local name = CM.unescName(tostring(c.name or ""))
		api.cmd.sendCommand(api.cmd.make.setName(id, name), function(_, okc)
			log(string.format("EXEC VNAME seq=%s %s %s -> %d name=%q success=%s",
				tostring(c.seq), tostring(c.kind), tostring(c.key), id, name, tostring(okc)))
		end)
	end)
    if not ok then log("exec VNAME error: " .. tostring(err)) end
end

function CM.execSetColor(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		local id = targetFor(tostring(c.kind or ""), tostring(c.key or ""))
		if not id then
			log(string.format("VCOLOR seq=%s: no local %s for key %s -- skipped",
				tostring(c.seq), tostring(c.kind), tostring(c.key)))
			return
		end
		local r, g, b = tonumber(c.r) or 0, tonumber(c.g) or 0, tonumber(c.b) or 0
		api.cmd.sendCommand(api.cmd.make.setColor(id, api.type.Vec3f.new(r, g, b)), function(_, okc)
			log(string.format("EXEC VCOLOR seq=%s %s %s -> %d rgb=%.2f,%.2f,%.2f success=%s",
				tostring(c.seq), tostring(c.kind), tostring(c.key), id, r, g, b, tostring(okc)))
		end)
	end)
	if not ok then log("exec VCOLOR error: " .. tostring(err)) end
end

function CM.execVehCmd(c)
	-- Strict ops replay on the originator ONLY if the slice actually cancelled
	-- the local command (armed=1). A Reverse left to run natively and then
	-- replayed is a toggle applied twice.
	if c.origin == K.INSTANCE and (not K.STRICT_OPS[c.op] or tonumber(c.armed or 1) == 0) then
		log(string.format("%s seq=%s: originator already applied locally, skipping", c.op, tostring(c.seq)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("%s seq=%s: STRICT -- originator replaying at stamp (local was cancelled)", c.op, tostring(c.seq)))
	end
	-- A VLINE whose vehicle key is not bound yet (its buy is still draining one
	-- per tick, or its buy callback has not fired) must WAIT, not skip: skipping
	-- left the vehicle unassigned on the peer while the host assigned it, and a
	-- setLine on the unresolved id crashed both peers on GetComponentDataIndex
	-- (2026-09-01). Same retry the "line not here yet" path uses.
	if c.op == "VLINE" and c.origin ~= K.INSTANCE then
		local haveAll = true
		if c.key and not vehIdFor(c.key) then haveAll = false end
		if not haveAll then
			c.tries = (c.tries or 0) + 1
			if c.tries <= 30 then
				-- advance by a FIXED number of steps from this command's own
				-- (agreed) target, never from local game-time: the retry schedule
				-- is then the same sim-steps on every instance
				c.notBeforeStep = (c.notBeforeStep or CM.stepOf(c.at)) + K.VLINE_RETRY_STEPS
				CM.retryQueue = CM.retryQueue or {}
				CM.retryQueue[#CM.retryQueue + 1] = c
				if c.tries == 1 or c.tries % 10 == 0 then
					log(string.format("VLINE seq=%s: vehicle key not bound yet -- retry %d (step %d)", tostring(c.seq), c.tries, c.notBeforeStep))
				end
				return
			end
			log(string.format("VLINE seq=%s: vehicle key never bound after %d tries -- left unassigned (DIVERGENCE)", tostring(c.seq), c.tries))
			return
		end
	end
	local ok, err = pcall(function()
		local function resolve(key)
			local id = vehIdFor(key)
			if not id then log(string.format("%s seq=%s: unknown vehicle key %s", c.op, tostring(c.seq), tostring(key))) end
			return id
		end
		local cmds = {}
		if c.op == "VSELL" then
			for key in tostring(c.keys or ""):gmatch("[^,]+") do
				local id = resolve(key)
				if id then cmds[#cmds + 1] = { api.cmd.make.sellVehicle(id), "sell " .. key, id } end
			end
		elseif c.op == "VDEPOT" then
			local id = resolve(c.key)
			if id then cmds[#cmds + 1] = { api.cmd.make.sendToDepot(id, tonumber(c.sell) == 1), "sendToDepot " .. tostring(c.key) } end
		elseif c.op == "VREV" then
			local id = resolve(c.key)
			if id then cmds[#cmds + 1] = { api.cmd.make.reverseVehicle(id), "reverse " .. tostring(c.key) } end
		elseif c.op == "VMAINT" then
			local id = resolve(c.key)
			local v = tonumber(c.v) or 1.0
			if id then cmds[#cmds + 1] = { api.cmd.make.setVehicleTargetMaintenanceState(id, v), "maint " .. tostring(c.key) .. " " .. tostring(v) } end
		elseif c.op == "VLINE" then
			local id = resolve(c.key)
			local line = CM.lineIdFor(c.line)
			-- A line the peer has not finished building yet is not a lost cause:
			-- LCREATE and its LUPDATE stops can still be in flight, or waiting on
			-- a station that has not replicated. Retry for a while instead of
			-- dropping the assignment, which leaves that vehicle unassigned on
			-- this instance for good (seen live 2026-08-31: the line's stop could
			-- not be resolved, the line was dropped, and every VLINE for it then
			-- failed).
			if id and not line then
				c.tries = (tonumber(c.tries) or 0) + 1
				if c.tries <= 20 then
					-- DETERMINISTIC retry step from the AGREED stamp, never local
					-- game-time: rewriting c.at to nowG+1 put the retry on a
					-- different sim-step on each instance, so a vehicle that needed
					-- one retry left the depot a step apart on host and peers -- the
					-- "vehicles left at different times" drift (2026-09-08). Matches
					-- the vehicle-key retry above. NOT straight back onto `queue`:
					-- the pump rebuilds that table (`queue = keep`) and would discard
					-- the append; the retry list is merged in, and the seq forgiven,
					-- at the top of the next pump.
					c.notBeforeStep = (c.notBeforeStep or CM.stepOf(c.at)) + K.VLINE_RETRY_STEPS
					CM.retryQueue = CM.retryQueue or {}
					CM.retryQueue[#CM.retryQueue + 1] = c
					if c.tries == 1 or c.tries % 10 == 0 then
						log(string.format("VLINE seq=%s: line %s not here yet -- retry %d (step %d)",
							tostring(c.seq), tostring(c.line), c.tries, c.notBeforeStep))
					end
				else
					log(string.format("VLINE seq=%s: line %s never arrived -- vehicle %s left unassigned (DIVERGENCE)",
						tostring(c.seq), tostring(c.line), tostring(c.key)))
				end
				return
			end
			if id and line then
				-- Types, because a wrong one here surfaced as an unreadable Lua
				-- error ("execVLINE error: function: 0000018D4AC0FF60") with
				-- nothing to say which argument was wrong.
				-- A stop of -1 means "engine, pick one", which is what the UI sends when a
				-- vehicle is assigned without choosing a stop -- the normal way a TRAIN is
				-- assigned. Replaying it verbatim had the engine refuse the command every
				-- time (EXEC VLINE ... success=false), and because the slice has already
				-- cancelled the player's own SetLine, the assignment was simply lost --
				-- "I can't set a line on a train" (2026-09-03). Buses were unaffected
				-- because the UI ships a real stop index for them. Clamp to the first stop:
				-- deterministic, since every instance clamps identically.
				local stopIx = tonumber(c.stop) or 0
				if stopIx < 0 then stopIx = 0 end
				local okMake, made = pcall(api.cmd.make.setLine, id, line, stopIx)
				if okMake and made then
					cmds[#cmds + 1] = { made, "setLine " .. tostring(c.key) }
				else
					log(string.format("VLINE seq=%s: setLine(%s:%s, %s:%s, %s) refused by the maker: %s",
						tostring(c.seq), type(id), tostring(id), type(line), tostring(line),
						tostring(stopIx), tostring(made)))
				end
			end
		end
		for _, pair in ipairs(cmds) do
			local what, vid = pair[2], pair[3]
			api.cmd.sendCommand(pair[1], function(res, success)
				local why = ""
				if not success then
					-- the engine's own reason, which this callback used to discard:
					-- "success=false" alone cannot tell a refused command from a lost one
					pcall(function()
						local es = res and res.resultProposalData and res.resultProposalData.errorState
						if es then
							why = " critical=" .. tostring(es.critical)
							for i = 1, #es.messages do why = why .. " '" .. tostring(es.messages[i]) .. "'" end
						end
					end)
				end
				log(string.format("EXEC %s seq=%s origin=%s at=%s %s success=%s%s",
					c.op, tostring(c.seq), tostring(c.origin), tostring(c.at), what, tostring(success), why))
				if success and c.op == "VSELL" and vid then forgetVehicle(vid) end
			end)
		end
	end)
	if not ok then log(string.format("exec%s error: %s", tostring(c.op), tostring(err))) end
end

-- The TransportVehicleConfig a command carries, rebuilt on this instance.
--
-- VBUY and VREPL ship the SAME encoding (name~loads~colour~autoloads;... plus a
-- vehicleGroups list), so they decode it with the same code: a second copy of
-- this would drift the moment one op learned about a new field, and a wrong
-- config is a wrong vehicle in a depot -- an uncatchable native assert away.
-- A global (not a `local function`): the chunk is at Lua 5.1's 200-local limit.
--
-- RAISES on a part this peer cannot build (unknown model, malformed spec). Both
-- callers run it inside their pcall, so the command is logged and skipped
-- instead of half-applied.
function buildVehConfig(c)
	local config = api.type.TransportVehicleConfig.new()
	local u = 0
	for spec in tostring(c.parts or ""):gmatch("[^;]+") do
		local name, loads, col, autos = spec:match("^([^~]*)~([^~]*)~([^~]*)~([^~]*)$")
		if not name then error("bad part spec: " .. spec) end
		local mid = tonumber(name:match("^#(%-?%d+)$") or "")
		if not mid then pcall(function() mid = api.res.modelRep.find(name) end) end
		if not mid or mid < 0 then error("model not found on this peer: " .. name) end
		local part = api.type.VehiclePart.new()
		part.modelId = mid
		local lc = part.loadConfig
		local n = 0
		for v in loads:gmatch("[^/]+") do n = n + 1; lc[n] = tonumber(v) or 0 end
		part.loadConfig = lc
		part.reversed = false     -- offset not yet decoded; TODO sweep
		local r, g, b = col:match("^([^,]+),([^,]+),([^,]+)$")
		part.color = api.type.Vec3f.new(tonumber(r) or -1, tonumber(g) or -1, tonumber(b) or -1)
		part.logo = ""
		local tvp = api.type.TransportVehiclePart.new()
		-- purchaseTime is game-time in ms (measured on a native buy: 2771200 at
		-- t=2771.2). Zero is what every replayed vehicle carried, and every
		-- replayed buy produced a non-fatal engine assert + an ~800 KB minidump
		-- (the visible stall on a buy). The stamp is identical on every peer, so
		-- it is deterministic; it is also within a second of the originator's.
		-- the originator's real purchaseTime when the wire has it (it ships the
		-- buy once its vehicle exists); the stamp only as the fallback
		local pt = tonumber(c.pt)
		if pt and pt > 0 then tvp.purchaseTime = math.floor(pt)
		else tvp.purchaseTime = math.floor((tonumber(c.at) or 0) * 1000) end
		tvp.maintenanceState = 1.0
		tvp.targetMaintenanceState = 0
		local alc = tvp.autoLoadConfig
		n = 0
		for v in autos:gmatch("[^/]+") do n = n + 1; alc[n] = tonumber(v) or 0 end
		tvp.autoLoadConfig = alc
		tvp.part = part
		u = u + 1
		config.vehicles[u] = tvp
	end
	if u == 0 then error("no parts") end
	local grp = config.vehicleGroups
	local ng = 0
	for v in tostring(c.groups or ""):gmatch("[^/]+") do ng = ng + 1; grp[ng] = tonumber(v) or 1 end
	if ng == 0 then grp[1] = u end
	config.vehicleGroups = grp
	return config, u
end

function CM.execVBuy(c)
	-- The originator replays its own purchase ONLY if the buy was actually
	-- cancelled here. Both conditions are required and neither is sufficient:
	-- K.STRICT_OPS.VBUY is read from cfg ONCE at load, so it can outlive a
	-- config change, and c.armed is per-command truth from the slice. Replaying
	-- on top of a purchase that really happened buys the vehicle TWICE and
	-- charges for both -- observed live 2026-09-03 when the slice armed a
	-- cancel whose completion callback then could not be fired.
	if c.origin == K.INSTANCE
			and (not K.STRICT_OPS.VBUY or tonumber(c.armed or 0) ~= 1) then
		log(string.format("VBUY seq=%s: originator already bought locally, skipping (strict=%s armed=%s)",
			tostring(c.seq), tostring(K.STRICT_OPS.VBUY and true or false), tostring(c.armed)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("VBUY seq=%s: STRICT -- originator replaying buy at stamp (local was cancelled)", tostring(c.seq)))
	end
	local ok, err = pcall(function()
		local x, y = tonumber(c.x), tonumber(c.y)
		if not (x and y) then log("VBUY: no depot position"); return end
		local rec = CM.consByKey[CM.conKey(x, y)]
		local depot = rec and rec.id
		local alive = false
		if depot then pcall(function() alive = api.engine.entityExists(depot) end) end
		if not alive then
			depot = nil
			pcall(function()
				local list = game.interface.getEntities({ pos = { x, y }, radius = 6 },
					{ type = "CONSTRUCTION", includeData = false }) or {}
				for _, id in pairs(list) do depot = depot or id end
			end)
		end
		if not depot then
			log(string.format("EXEC VBUY seq=%s: no depot at %.1f,%.1f -- vehicle NOT bought", tostring(c.seq), x, y))
			return
		end
		-- The depot must be the same KIND as the originator's: a train into a
		-- road depot is a native assert (measured), so refuse rather than risk.
		local want = tostring(c.file or "?")
		if want ~= "?" then
			local fn = ""
			pcall(function()
				local co = api.engine.getComponent(depot, api.type.ComponentType.CONSTRUCTION)
				fn = co and co.fileName and tostring(co.fileName) or ""
			end)
			if fn ~= want then
				log(string.format("EXEC VBUY seq=%s: depot at %.1f,%.1f is '%s', expected '%s' -- refusing",
					tostring(c.seq), x, y, fn, want))
				return
			end
		end
		-- buyVehicle wants the VEHICLE_DEPOT child, not the construction
		local target
		pcall(function()
			local co = api.engine.getComponent(depot, api.type.ComponentType.CONSTRUCTION)
			if co and co.depots and #co.depots >= 1 then target = co.depots[1] end
		end)
		if not target then
			log(string.format("EXEC VBUY seq=%s: construction %d has no depot child -- vehicle NOT bought", tostring(c.seq), depot))
			return
		end
		local config, u = buildVehConfig(c)
		local seq, origin, at = c.seq, c.origin, c.at
		-- COMPANIES MODE: buy AS the originating company (2026-09-09). The first
		-- argument of buyVehicle is the player entity; passing our own and then
		-- game.interface.setPlayer()ing the vehicle over asserts FATALLY in the
		-- engine (interface.cpp:2340 "Assertion false", 50 of 50 purchases in a
		-- friends' 4-company session, a minidump each). Bought under the right
		-- player the engine owns and charges it from birth, the reassign below
		-- sees the owner already right and skips, and the cost settle moves 0.
		local buyer = api.engine.util.getPlayer()
		if c.company and CM.cmMode == "companies" then
			CM.cmEnsure()
			local pid = CM.cmCompanyPid[tonumber(c.company)]
			if pid then buyer = pid
			else log(string.format("EXEC VBUY seq=%s: no player for company %s -- buying as ourselves", tostring(c.seq), tostring(c.company))) end
		end
		local okM, cmd = pcall(function() return api.cmd.make.buyVehicle(buyer, target, config) end)
		if not okM or not cmd then
			log(string.format("EXEC VBUY seq=%s: make.buyVehicle refused: %s", tostring(seq), tostring(cmd)))
			return
		end
		api.cmd.sendCommand(cmd, function(res, success)
			log(string.format("EXEC VBUY seq=%s origin=%s at=%s construction=%d depot=%s parts=%d success=%s",
				tostring(seq), tostring(origin), tostring(at), depot, tostring(target), u, tostring(success)))
			if success then
				-- buyVehicle is entity-returning (same shape VREPL reads): bind
				-- this key to THAT entity, not to whichever new id sorts first
				local nid = nil
				pcall(function()
					local r = res and res.resultEntity
					if type(r) == "number" then nid = r elseif r ~= nil then nid = tonumber(tostring(r)) end
				end)
				if not (nid and nid > 0) then nid = nil end
				expectVehicle(tostring(origin) .. ":" .. tostring(seq), depot, c.company and tonumber(c.company) or nil, nid)
			end
		end)
	end)
	if not ok then log("execVBuy error: " .. tostring(err)) end
end

-- ---------- vehicles: ReplaceVehicle ----------
--
-- The vehicle window's "replace" (command factory 4: r8 = the vehicle, r9 = a
-- TransportVehicleConfig with the layout VBUY already serialises) swaps a
-- vehicle's configuration in place.
--
-- OPTIMISTIC, like BuyVehicle and for the same reason: the UI waits for the
-- command's result, and cancelling a vehicle command that the UI is waiting on
-- is exactly what wedged the build tool. So the originator's replace applies
-- natively at T0, the peers apply at the stamp, and the originator skips its
-- own replay.
--
-- Identity: the vehicle travels as a cross-peer KEY (VSELL/VDEPOT/VREV all do
-- this), never an entity id. If the engine mints a NEW entity for the replaced
-- vehicle, the key must follow it or every later command for that vehicle
-- resolves to a dead id -- hence the re-registration in the callback.
--
-- A global, not a `local function`: the chunk is at Lua 5.1's 200-local limit.
function CM.execVReplace(c)
	-- Strict only when the slice actually cancelled the local replace (armed=1);
	-- a replace left to run natively and replayed on top swaps the consist twice.
	if c.origin == K.INSTANCE and (not K.STRICT_OPS.VREPL or tonumber(c.armed or 1) == 0) then
		log(string.format("VREPL seq=%s: originator already replaced locally, skipping",
			tostring(c.seq)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("VREPL seq=%s: STRICT -- originator replaying at stamp (local was cancelled)", tostring(c.seq)))
	end
	local ok, err = pcall(function()
		local key = tostring(c.veh or "")
		local veh = vehIdFor(key)
		if not veh then
			log(string.format("EXEC VREPL seq=%s: unknown vehicle key %s -- replace NOT applied",
				tostring(c.seq), key))
			return
		end
		local config, u = buildVehConfig(c)
		local seq, origin, at = c.seq, c.origin, c.at
		local okM, cmd = pcall(function() return api.cmd.make.replaceVehicle(veh, config) end)
		if not okM or not cmd then
			log(string.format("EXEC VREPL seq=%s: make.replaceVehicle refused: %s",
				tostring(seq), tostring(cmd)))
			return
		end
		-- TODO companies mode: a replace bills the executing player, so the cost
		-- has to move to c.company the way VBUY does it (balance delta captured
		-- around the apply, then CM.cmTransferCost). Not wired yet -- in coop this
		-- is a no-op, in companies mode the buyer's company is under-charged.
		api.cmd.sendCommand(cmd, function(res, success)
			-- The replace may hand back a NEW entity. Take it from the result
			-- rather than assume either way; both shapes have been seen for
			-- entity-returning commands.
			local nid
			pcall(function()
				local r = res and res.resultEntity
				if type(r) == "number" then nid = r
				elseif r ~= nil then nid = tonumber(tostring(r)) end
			end)
			log(string.format("EXEC VREPL seq=%s origin=%s at=%s %s vehicle=%s parts=%d "
				.. "result=%s success=%s",
				tostring(seq), tostring(origin), tostring(at), key, tostring(veh), u,
				tostring(nid), tostring(success)))
			if success and nid and nid > 0 and nid ~= veh then
				forgetVehicle(veh)          -- the old id is dead; ids get reused
				registerVehKey(key, nid)
				log(string.format("VREPL: key %s now names entity %d (was %d)", key, nid, veh))
			end
		end)
	end)
	if not ok then log("execVReplace error: " .. tostring(err)) end
end
end
