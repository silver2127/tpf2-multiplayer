-- mp/gt.lua -- ground-truth sweeps (constructions, vehicles, lines, demolish)
--
-- Split out of lockstep.lua on 2026-09-08 (the single 11k-line chunk was at
-- 182 of Lua 5.1's 200 file-scope locals). Loaded from the game script as
--     local gt = require("mp.gt")(CM, K, log)
-- It is a FACTORY, not a plain module table: every load of the game script
-- gets fresh state (package.loaded would otherwise hand the next game the
-- previous game's file-scope locals), and the code below keeps the same
-- per-load semantics it had inside lockstep.lua. Anything defined later in
-- lockstep.lua is reached through CM (CM.ser, CM.deepcopy, ...); CM is the
-- shared state table, K the constants table, log the instance-tagged logger.
-- The body stays at column 0 on purpose: tools/luacheck.py's use-before-define
-- checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- ground-truth sweeps ----------
--
-- Drive KNOWN values into the proposal factory and let the native hook record
-- where they land. This inverts the method that failed repeatedly this session:
-- instead of watching one player build and inferring a field's meaning from a
-- single sample, sweep a parameter and see which offset tracks it.
--
-- NON-DESTRUCTIVE. api.cmd.make.buildProposal IS the factory call -- it builds
-- the Command and fires the hook. Without a matching sendCommand, nothing is
-- ever applied, so a sweep of hundreds of values leaves the world untouched.
--
-- Samples are self-identifying: node 0's X carries 900000 + testId*1000 + index,
-- so every capture is labelled in-band and nothing depends on matching by order.
K.GT_BASE_X = 900000

local function gtProposal(testId, index, isTrack, streetType, trackType, catenary)
	local sp = api.type.SimpleProposal.new()
	local x = K.GT_BASE_X + testId * 1000 + index
	local n0 = api.type.NodeAndEntity.new()
	n0.entity = -900001
	n0.comp.position = api.type.Vec3f.new(x, 0, 0)
	local n1 = api.type.NodeAndEntity.new()
	n1.entity = -900002
	n1.comp.position = api.type.Vec3f.new(x + 20, 0, 0)
	sp.streetProposal.nodesToAdd[1] = n0
	sp.streetProposal.nodesToAdd[2] = n1

	local e = api.type.SegmentAndEntity.new()
	e.entity = -900003
	e.comp.node0 = -900001
	e.comp.node1 = -900002
	e.comp.tangent0 = api.type.Vec3f.new(20, 0, 0)
	e.comp.tangent1 = api.type.Vec3f.new(20, 0, 0)
	e.comp.type = 0
	e.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
	e.type = isTrack and 1 or 0
	if isTrack then
		e.trackEdge = api.type.BaseEdgeTrack.new()
		e.trackEdge.trackType = trackType
		e.trackEdge.catenary = catenary and true or false
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = 16
	else
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = streetType
		e.streetEdge.hasBus = false
		e.streetEdge.tramTrackType = 0
	end
	sp.streetProposal.edgesToAdd[1] = e

	-- Factory only. No sendCommand: the world never sees this.
	api.cmd.make.buildProposal(sp, nil, false)
end

-- ---------- ground truth: CONSTRUCTIONS ----------
--
-- STEP 1 of the skeptic's plan: does a script-built construction proposal reach
-- make_cmd::BuildProposal at all, and with which API shape? M5 recorded that
-- every constructionsToAdd configuration threw, but none of those attempts
-- survive, and the serialiser that fed them was later found to truncate module
-- tables to "?" (mpbridge.lua 109-113) -- so the negative may have been the
-- serialiser's, not the API's. Every statement is pcall'd separately so the
-- exact failing call and its message are on record, not just "it threw".
--
-- Factory only, no sendCommand: nothing touches the world, so used-ground and
-- seed hazards do not apply.
local function gtLoadStationParams()
	local f = io.open(K.BASE .. "gt_station_params.lua", "r")
	if not f then return nil, "no gt_station_params.lua" end
	local s = f:read("*a"); f:close()
	local fn, err = load("return " .. s)
	if not fn then return nil, "params parse: " .. tostring(err) end
	local ok, t = pcall(fn)
	if not ok then return nil, "params eval: " .. tostring(t) end
	t.seed = nil            -- reusing a seed drives errorState critical elsewhere
	return t
end

local function gtTransf(x, y, z)
	-- Try the typed Mat4f first (public-API shape, unverified on this build),
	-- then a plain 16-number column-major table. Report which one took.
	local m
	local ok = pcall(function()
		m = api.type.Mat4f.new(
			api.type.Vec4f.new(1, 0, 0, 0), api.type.Vec4f.new(0, 1, 0, 0),
			api.type.Vec4f.new(0, 0, 1, 0), api.type.Vec4f.new(x, y, z, 1))
	end)
	if ok and m then return m, "Mat4f.new(4xVec4f)" end
	return { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }, "table16"
end

-- One construction proposal through the factory. Returns (fired?, detail).
local function gtConFile(testId, index, params, mutate, fileName)
	local x = K.GT_BASE_X + testId * 1000 + index
	local y, z = -7890.25, 33.125          -- odd values: cannot be confused with anything else
	local steps = {}
	local function step(name, fn)
		local ok, err = pcall(fn)
		steps[#steps + 1] = name .. (ok and " ok" or (" FAIL: " .. tostring(err)))
		return ok
	end
	local sp, ce, tr, trKind
	if not step("SimpleProposal.new", function() sp = api.type.SimpleProposal.new() end) then return false, steps end
	if not step("ConstructionEntity.new", function() ce = api.type.SimpleProposal.ConstructionEntity.new() end) then
		return false, steps
	end
	step("fileName", function() ce.fileName = fileName or "station/rail/modular_station/modular_station.con" end)
	step("params", function()
		local p = params
		if mutate then p = mutate(params, index) end
		ce.params = p
	end)
	step("transf", function() tr, trKind = gtTransf(x, y, z); ce.transf = tr end)
	steps[#steps] = steps[#steps] .. " (" .. tostring(trKind) .. ")"
	step("playerEntity", function() ce.playerEntity = api.engine.util.getPlayer() end)
	step("name", function() ce.name = "gt" end)
	if not step("constructionsToAdd[1]", function() sp.constructionsToAdd[1] = ce end) then return false, steps end
	local fired = step("make.buildProposal", function() api.cmd.make.buildProposal(sp, nil, false) end)
	return fired, steps
end

local function gtCon(testId, index, params, mutate)
	return gtConFile(testId, index, params, mutate, nil)
end


local function runGroundTruthCon()
	local params, perr = gtLoadStationParams()
	if not params then log("GT con: " .. tostring(perr)); return end
	local nMods = 0
	for _ in pairs(params.modules or {}) do nMods = nMods + 1 end
	log(string.format("GT con: params loaded, %d modules, seed stripped", nMods))

	-- test 10: the GATE plus translation sweep. Four samples, only x varies.
	local fired, steps = gtCon(10, 0, params)
	for _, s in ipairs(steps) do log("GT con   " .. s) end
	if not fired then
		log("GT con: GATE FAILED -- constructionsToAdd does not reach the factory " ..
			"with this shape. See the FAIL line above for the exact statement.")
		-- Variants, for the record: is it the payload or the API?
		local withSeed = CM.deepcopy(params); withSeed.seed = 1
		local depot = { year = 1850, paramX = 0, paramY = 0 }
		local depotSeed = { year = 1850, paramX = 0, paramY = 0, seed = 1 }
		local function try(label, p, file)
			local f2, st2 = gtConFile(12, 0, p, nil, file)
			log(string.format("GT con   variant %-22s -> %s", label, f2 and "FIRED" or st2[#st2]))
		end
		try("station+seed", withSeed, "station/rail/modular_station/modular_station.con")
		try("depot minimal", depot, "depot/road_depot_era_a.con")
		try("depot+seed", depotSeed, "depot/road_depot_era_a.con")
		try("station params={}", {}, "station/rail/modular_station/modular_station.con")
		return
	end
	for i = 1, 3 do gtCon(10, i, params) end
	log("GT con: gate PASSED, translation sweep 4 samples issued")

	-- test 11: module-identity sentinel. Same station, but slot 8401000's
	-- updateScript.fileName carries a string sentinel and its variant an int
	-- one. Strings are what the hook's pointer chase finds most reliably; if
	-- "GTSENT_<i>.lua" is reachable from a3 the ModuleInfo map is too, and its
	-- layout falls out of the chase path. If it is nowhere in the tree, module
	-- params never reach this command and must travel out-of-band.
	local function mutate(p, i)
		local q = CM.deepcopy(p)
		local m = q.modules and q.modules[8401000]
		if m then
			m.variant = 100 + i
			m.updateScript = m.updateScript or {}
			m.updateScript.fileName = string.format("GTSENT_%d.lua", i)
			m.updateScript.params = { zz = 4242.5 + i }
		end
		return q
	end
	for i = 0, 3 do gtCon(11, i, params, mutate) end
	log("GT con: module-sentinel sweep 4 samples issued (world untouched)")
end

-- pcall-per-statement runner shared by the vehicle/line/demolish sweeps below,
-- mirroring gtCon: the FIRST failing API call is named in the log, not just
-- "it threw". buildAndFire(step) returns whether the factory call fired.
local function gtSweepSample(chan, test, i, buildAndFire)
	local steps = {}
	local function step(name, fn)
		local ok, err = pcall(fn)
		steps[#steps + 1] = name .. (ok and " ok" or (" FAIL: " .. tostring(err)))
		return ok
	end
	local fired = buildAndFire(step)
	if not fired then
		log(string.format("GT %s t%d.%d FAILED:", chan, test, i))
		for _, s in ipairs(steps) do log("GT " .. chan .. "   " .. s) end
	end
	return fired and true or false
end

-- ---------- ground truth: VEHICLES (tests 30..40) ----------
--
-- Factory-only: api.cmd.make.buyVehicle/sellVehicle/sendToDepot/replaceVehicle
-- with NO sendCommand -- the factory builds the Command and fires the native
-- hook, nothing is ever applied. The buy sentinel rides in depotEntity (the
-- factory only asserts != -1). The config recipe is copied verbatim from the
-- measured-working mpbridge builder (mpbridge.lua:1651-1711): read-modify-write
-- every vector property (sol2 hands them back BY VALUE; in-place writes are
-- silently lost), vehicleGroups summing to #vehicles. One field varies per
-- test; everything else stays at the known-good defaults.
local function gtVehPickModel()
	-- A REAL model id + its compartments count, from api.res.modelRep. Ids are
	-- resolved through find(fileName) so no assumption is made about getAll()'s
	-- key base. If nothing resolves, the config sweeps are SKIPPED, not guessed.
	local mid, nComp, kind
	local ok, err = pcall(function()
		local all = api.res.modelRep.getAll()
		for _, fn in pairs(all) do
			if type(fn) == "string"
				and (fn:find("vehicle/train/", 1, true) or fn:find("vehicle/road/", 1, true)) then
				local cand = api.res.modelRep.find(fn)
				if cand and cand >= 0 then
					local md = api.res.modelRep.get(cand)
					local tv = md and md.metadata and md.metadata.transportVehicle
					if tv and tv.compartments then
						local c = 0
						for _ in pairs(tv.compartments) do c = c + 1 end
						if c > 0 then mid = cand; nComp = c; kind = fn:find("vehicle/train/", 1, true) and "train" or "road"; return end
					end
				end
			end
		end
	end)
	if not ok then return nil, nil, "modelRep scan FAIL: " .. tostring(err) end
	if not mid then return nil, nil, "no train/road vehicle with compartments in modelRep" end
	return mid, nComp, nil, kind
end

-- Build a TransportVehicleConfig with nUnits units, defaults from the mpbridge
-- recipe, then apply the per-test overrides in mut. Returns nil on any failure
-- (the failing statement is already in steps via step()).
local function gtVehConfig(step, mid, nComp, nUnits, mut)
	local config
	if not step("TransportVehicleConfig.new", function()
		config = api.type.TransportVehicleConfig.new()
	end) then return nil end
	for u = 1, nUnits do
		local part, tvp
		if not step("VehiclePart.new", function() part = api.type.VehiclePart.new() end) then return nil end
		step("part.modelId", function() part.modelId = mut.modelId or mid end)
		step("part.loadConfig", function()
			local lc = part.loadConfig
			if mut.loadConfig then
				for c = 1, #mut.loadConfig do lc[c] = mut.loadConfig[c] end
			else
				for c = 1, nComp do lc[c] = 0 end
			end
			part.loadConfig = lc
		end)
		step("part.reversed", function() part.reversed = mut.reversed or false end)
		step("part.color", function()
			local cv = mut.color or { -1, -1, -1 }
			part.color = api.type.Vec3f.new(cv[1], cv[2], cv[3])
		end)
		step("part.logo", function() part.logo = mut.logo or "" end)
		if not step("TransportVehiclePart.new", function()
			tvp = api.type.TransportVehiclePart.new()
		end) then return nil end
		step("tvp.purchaseTime", function() tvp.purchaseTime = mut.purchaseTime or 0 end)
		step("tvp.maintenanceState", function() tvp.maintenanceState = mut.ms or 1.0 end)
		step("tvp.targetMaintenanceState", function() tvp.targetMaintenanceState = mut.tms or 0 end)
		step("tvp.autoLoadConfig", function()
			local alc = tvp.autoLoadConfig
			if mut.autoLoadConfig then
				for c = 1, #mut.autoLoadConfig do alc[c] = mut.autoLoadConfig[c] end
			else
				for c = 1, nComp do alc[c] = 1 end
			end
			tvp.autoLoadConfig = alc
		end)
		step("tvp.part", function() tvp.part = part end)
		step("config.vehicles[" .. u .. "]", function() config.vehicles[u] = tvp end)
	end
	step("config.vehicleGroups", function()
		local grp = config.vehicleGroups
		if mut.groups then
			for g = 1, #mut.groups do grp[g] = mut.groups[g] end
		else
			grp[1] = nUnits
		end
		config.vehicleGroups = grp
	end)
	return config
end

local function runGroundTruthVeh()
	local mid, nComp, perr = gtVehPickModel()
	if not mid then
		log("GT veh: " .. tostring(perr) .. " -- config sweeps t30-t36/t40 SKIPPED")
	else
		log(string.format("GT veh: model id %d, %d compartments", mid, nComp))
	end
	local function buySweep(test, label, nUnits, mutFor)
		if not mid then
			log(string.format("GT veh t%d (%s): skipped (no model)", test, label))
			return
		end
		local fired = 0
		for i = 0, 7 do
			if gtSweepSample("veh", test, i, function(step)
				local cfg = gtVehConfig(step, mid, nComp, nUnits, mutFor(i))
				if not cfg then return false end
				return step("make.buyVehicle", function()
					api.cmd.make.buyVehicle(api.engine.util.getPlayer(),
						K.GT_BASE_X + test * 1000 + i, cfg)
				end)
			end) then fired = fired + 1 end
		end
		log(string.format("GT veh t%d (%s): %d/8 fired", test, label, fired))
	end
	buySweep(30, "modelId", 1, function(i) return { modelId = 700000 + i } end)
	buySweep(31, "loadConfig", 1, function(i) return { loadConfig = { i, i + 1 } } end)
	buySweep(32, "purchaseTime", 1, function(i) return { purchaseTime = 123456000 + i } end)
	buySweep(33, "maint floats", 1, function(i) return { ms = i / 10, tms = 1 - i / 10 } end)
	buySweep(34, "autoLoadConfig", 1, function(i) return { autoLoadConfig = { i } } end)
	buySweep(35, "vehicleGroups", 3, function(i) return { groups = { i, 3 - i } } end)
	buySweep(36, "rev/color/logo", 1, function(i)
		return { reversed = (i % 2 == 1), color = { i, i, i }, logo = "gt" .. i }
	end)
	log("GT veh t37: skipped here on purpose -- live readback (sendCommand + real"
		.. " depot), belongs to STEP 5, not a factory-only sweep")
	-- t38 SellVehicle: sentinel is the vehicle entity (ends up INSIDE the
	-- native vector<Entity>; the hook reads *(int*)[r8]).
	local f38 = 0
	for i = 0, 7 do
		local ok, err = pcall(function()
			api.cmd.make.sellVehicle(K.GT_BASE_X + 38 * 1000 + i)
		end)
		if ok then f38 = f38 + 1 else log("GT veh t38." .. i .. " FAIL: " .. tostring(err)) end
	end
	log(string.format("GT veh t38 (sellVehicle): %d/8 fired", f38))
	-- t39 SendToDepot(sentinel, bool): r8 = entity, r9 = bool tracks i%2
	local f39 = 0
	for i = 0, 7 do
		local ok, err = pcall(function()
			api.cmd.make.sendToDepot(K.GT_BASE_X + 39 * 1000 + i, i % 2 == 1)
		end)
		if ok then f39 = f39 + 1 else log("GT veh t39." .. i .. " FAIL: " .. tostring(err)) end
	end
	log(string.format("GT veh t39 (sendToDepot): %d/8 fired", f39))
	-- t40 ReplaceVehicle(sentinel, cfg) -- only if the Lua maker exists here
	local mkRep
	pcall(function() mkRep = api.cmd.make.replaceVehicle end)
	if mkRep == nil then
		log("GT veh t40: skipped (api.cmd.make.replaceVehicle not present on this build)")
	elseif not mid then
		log("GT veh t40: skipped (no model for config)")
	else
		local f40 = 0
		for i = 0, 7 do
			if gtSweepSample("veh", 40, i, function(step)
				local cfg = gtVehConfig(step, mid, nComp, 1, {})
				if not cfg then return false end
				return step("make.replaceVehicle", function()
					mkRep(K.GT_BASE_X + 40 * 1000 + i, cfg)
				end)
			end) then f40 = f40 + 1 end
		end
		log(string.format("GT veh t40 (replaceVehicle): %d/8 fired", f40))
	end
end

-- ---------- ground truth: LINES (tests 10..18, T1..T9) ----------
--
-- Factory-only api.cmd.make.updateLine(sentinelLineEntity, line): the factory
-- asserts only lineEntity != -1, so the sentinel passes and nothing touches
-- the world. Line shape copied from the measured mptest buildLineObject.
-- NOTE (r8 plan step 0): CreateLine/UpdateLine must NEVER be cancelled live;
-- this sweep never sends, so no cancel can arise here either.
local function gtLineObject(step, nStops, wait, mutStop)
	local line
	if not step("Line.new", function() line = api.type.Line.new() end) then return nil end
	step("waitingTime", function() line.waitingTime = wait or 180 end)
	for k = 1, nStops do
		local s
		if not step("Line.Stop.new", function() s = api.type.Line.Stop.new() end) then return nil end
		step("stationGroup", function() s.stationGroup = 800000 + k end)
		step("station", function() s.station = 0 end)
		step("terminal", function() s.terminal = 0 end)
		step("loadMode", function() s.loadMode = 0 end)
		step("minWaitingTime", function() s.minWaitingTime = 0 end)
		step("maxWaitingTime", function() s.maxWaitingTime = 180 end)
		if mutStop then mutStop(step, s, k) end
		step("stops[" .. k .. "]", function() line.stops[k] = s end)
	end
	return line
end

local function runGroundTruthLine()
	local function sweep(test, label, nStopsFor, waitFor, mutFor)
		local fired = 0
		for i = 0, 7 do
			if gtSweepSample("line", test, i, function(step)
				local line = gtLineObject(step, nStopsFor(i),
					waitFor and waitFor(i) or nil, mutFor and mutFor(i) or nil)
				if not line then return false end
				return step("make.updateLine", function()
					api.cmd.make.updateLine(K.GT_BASE_X + test * 1000 + i, line)
				end)
			end) then fired = fired + 1 end
		end
		log(string.format("GT line t%d (%s): %d/8 fired", test, label, fired))
	end
	local one = function() return 1 end
	-- T1 (10): stop count = i -> stops-vector span tracks 0xa8*i
	sweep(10, "stop count", function(i) return i end)
	-- T2 (11): waitingTime = 100+i -> Line+0x18
	sweep(11, "waitingTime", one, function(i) return 100 + i end)
	-- T3 (12): stop.terminal = i  \ these two DECIDE the +0x04/+0x08 order --
	-- T4 (13): stop.station  = i  / nothing ships before they disagree/agree
	sweep(12, "terminal", one, nil, function(i)
		return function(step, s) step("mut terminal", function() s.terminal = i end) end
	end)
	sweep(13, "station", one, nil, function(i)
		return function(step, s) step("mut station", function() s.station = i end) end
	end)
	-- T5 (14): loadMode = i%3 -> +0x28
	sweep(14, "loadMode", one, nil, function(i)
		return function(step, s) step("mut loadMode", function() s.loadMode = i % 3 end) end
	end)
	-- T6 (15): minWaitingTime = i, maxWaitingTime = 100+i -> +0x2c/+0x30
	sweep(15, "min/maxWait", one, nil, function(i)
		return function(step, s)
			step("mut minWaitingTime", function() s.minWaitingTime = i end)
			step("mut maxWaitingTime", function() s.maxWaitingTime = 100 + i end)
		end
	end)
	-- T7 (16): #alternativeTerminals = i with {station=i, terminal=7-i}
	-- -> span 8*i at stop+0x10; StationTerminal.new existence is INFERRED,
	-- a FAIL line here is itself the measurement.
	sweep(16, "altTerminals", one, nil, function(i)
		return function(step, s)
			step("mut alternativeTerminals", function()
				local at = s.alternativeTerminals
				for a = 1, i do
					local t = api.type.StationTerminal.new()
					t.station = i
					t.terminal = 7 - i
					at[a] = t
				end
				s.alternativeTerminals = at
			end)
		end
	end)
	-- T8 (17): stationGroup = 800000+i -> stop+0x00
	sweep(17, "stationGroup", one, nil, function(i)
		return function(step, s) step("mut stationGroup", function() s.stationGroup = 800000 + i end) end
	end)
	-- T10 (19): stopConfig -- the per-stop cargo filter. The engine registers
	-- StopConfig with members `unload` and `maxLoad` (exe strings beside
	-- LineLoadMode), shape unknown from Lua: try array-index first, then whole
	-- assignment. maxLoad carries 700000+i (a value the correlator can track);
	-- unload is a flag, so its COUNT is what varies. The stop record's last
	-- 88 bytes (0x50..0xa7, after the waypoints vector) are where these land.
	sweep(19, "stopConfig", one, nil, function(i)
		return function(step, s)
			step("stopConfig.maxLoad", function()
				local sc = s.stopConfig
				local ok = pcall(function() sc.maxLoad[1] = 700000 + i end)
				if not ok then sc.maxLoad = { 700000 + i } end
				s.stopConfig = sc
			end)
			step("stopConfig.unload", function()
				local sc = s.stopConfig
				local ok = pcall(function() for u = 1, i + 1 do sc.unload[u] = true end end)
				if not ok then local t = {}; for u = 1, i + 1 do t[u] = true end; sc.unload = t end
				s.stopConfig = sc
			end)
		end
	end)
	-- T9 (18): createLine, name length crossing the SSO boundary (4 chars even
	-- i, 20 chars odd i -> heap), colour=(i,0,0), player sentinel in r9; plus
	-- deleteLine/setLine sentinels (args register-visible in [cap], no dump).
	local f18 = 0
	for i = 0, 7 do
		local sent = K.GT_BASE_X + 18 * 1000 + i
		if gtSweepSample("line", 18, i, function(step)
			local line = gtLineObject(step, 1)
			if not line then return false end
			local name = "gtl" .. i                              -- 4 chars: SSO
			if i % 2 == 1 then name = name .. string.rep("z", 16) end -- 20: heap
			return step("make.createLine", function()
				api.cmd.make.createLine(name, api.type.Vec3f.new(i, 0, 0), sent, line)
			end)
		end) then f18 = f18 + 1 end
		local okd, errd = pcall(function() api.cmd.make.deleteLine(sent) end)
		if not okd then log("GT line t18." .. i .. " deleteLine FAIL: " .. tostring(errd)) end
		local oks, errs = pcall(function() api.cmd.make.setLine(sent, 800000 + i, i) end)
		if not oks then log("GT line t18." .. i .. " setLine FAIL: " .. tostring(errs)) end
	end
	log(string.format("GT line t18 (createLine SSO + deleteLine/setLine): %d/8 fired", f18))
end

-- ---------- ground truth: DEMOLISH (tests 4..6) ----------
--
-- Factory-only buildProposal carrying the SAME valid 2-node/1-edge street the
-- existing street sweep uses (so scripting::Convert accepts the proposal) with
-- the sentinel in node0.position.x for in-band labelling, PLUS removal entries
-- whose entity id IS the sentinel. Predictions (r9 step 2): t4 span48==120,
-- r48+0x00==sentinel (sample 8 carries TWO edges -> 240); t5 span30==24,
-- r30+0x14==sentinel; t6 span1e0==4, r1e0[0]==sentinel or 'refused'.
local function gtDemol(test, i, mut)
	return gtSweepSample("demolish", test, i, function(step)
		local x = K.GT_BASE_X + test * 1000 + i
		local sp
		if not step("SimpleProposal.new", function() sp = api.type.SimpleProposal.new() end) then
			return false
		end
		step("nodesToAdd", function()
			local n0 = api.type.NodeAndEntity.new()
			n0.entity = -900001
			n0.comp.position = api.type.Vec3f.new(x, 0, 0)
			local n1 = api.type.NodeAndEntity.new()
			n1.entity = -900002
			n1.comp.position = api.type.Vec3f.new(x + 20, 0, 0)
			sp.streetProposal.nodesToAdd[1] = n0
			sp.streetProposal.nodesToAdd[2] = n1
		end)
		step("edgesToAdd", function()
			local e = api.type.SegmentAndEntity.new()
			e.entity = -900003
			e.comp.node0 = -900001
			e.comp.node1 = -900002
			e.comp.tangent0 = api.type.Vec3f.new(20, 0, 0)
			e.comp.tangent1 = api.type.Vec3f.new(20, 0, 0)
			e.comp.type = 0
			e.comp.typeIndex = -1   -- native road edges: typeIndex=-1 (live probe)
			e.type = 0
			e.streetEdge = api.type.BaseEdgeStreet.new()
			e.streetEdge.streetType = 16
			e.streetEdge.hasBus = false
			e.streetEdge.tramTrackType = 0
			sp.streetProposal.edgesToAdd[1] = e
		end)
		-- If the removal entry cannot be set, do NOT fire: a sample without its
		-- removal entry would show span 0 and pollute the correlation.
		if not mut(step, sp, x) then return false end
		return step("make.buildProposal", function() api.cmd.make.buildProposal(sp, nil, false) end)
	end)
end

local function runGroundTruthDemol()
	-- test 4: edgesToRemove -- 8 single-edge samples + sample 8 with two edges
	local f4 = 0
	for i = 0, 8 do
		if gtDemol(4, i, function(step, sp, x)
			return step("edgesToRemove", function()
				sp.streetProposal.edgesToRemove[1] = x
				if i == 8 then sp.streetProposal.edgesToRemove[2] = x + 100 end
			end)
		end) then f4 = f4 + 1 end
	end
	log(string.format("GT demolish t4 (edgesToRemove): %d/9 fired (sample 8 = two edges)", f4))
	-- test 5: nodesToRemove
	local f5 = 0
	for i = 0, 7 do
		if gtDemol(5, i, function(step, sp, x)
			return step("nodesToRemove", function()
				sp.streetProposal.nodesToRemove[1] = x
			end)
		end) then f5 = f5 + 1 end
	end
	log(string.format("GT demolish t5 (nodesToRemove): %d/8 fired", f5))
	-- test 6: constructionsToRemove -- may be refused like constructionsToAdd
	-- was; the refusal IS the recorded answer (fall back to the 3-sample UI
	-- differential, r9 step 2).
	local f6 = 0
	for i = 0, 7 do
		if gtDemol(6, i, function(step, sp, x)
			return step("constructionsToRemove", function()
				sp.constructionsToRemove[1] = x
			end)
		end) then f6 = f6 + 1 end
	end
	if f6 == 0 then
		log("GT demolish t6: refused -- constructionsToRemove never reached the factory")
	else
		log(string.format("GT demolish t6 (constructionsToRemove): %d/8 fired", f6))
	end
end
local function runGroundTruth(what)
	local n = 0
	local ok, err = pcall(function()
		if what == "track" then
			-- test 1: trackType 0..7 with catenary off
			for v = 0, 7 do gtProposal(1, v, true, 16, v, false); n = n + 1 end
			-- test 2: SAME trackType, catenary on -- isolates the catenary bit
			for v = 0, 7 do gtProposal(2, v, true, 16, v, true); n = n + 1 end
		elseif what == "con" then
			runGroundTruthCon(); return
		elseif what == "street" then
			-- test 3: streetType 0..39
			for v = 0, 39 do gtProposal(3, v, false, v, 1, false); n = n + 1 end
		elseif what == "vehicle" then
			runGroundTruthVeh(); return
		elseif what == "line" then
			runGroundTruthLine(); return
		elseif what == "demolish" then
			runGroundTruthDemol(); return
		else
			log("GT: unknown sweep '" .. tostring(what)
				.. "' (try track|street|con|vehicle|line|demolish)")
			return
		end
	end)
	if ok then
		log(string.format("GT: %s sweep issued %d samples (world untouched)", what, n))
	else
		log("GT error: " .. tostring(err))
	end
end

return { gtVehPickModel = gtVehPickModel, gtVehConfig = gtVehConfig, runGroundTruth = runGroundTruth }
end
