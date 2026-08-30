-- M3 determinism probe: periodic checksum of observable sim state.
-- Two instances load the SAME save, receive NO player input, and run side by
-- side; if their hash sequences ever differ, the simulation is not
-- deterministic and lockstep multiplayer is off the table (REPORT.md §6-7).
--
-- Every access is pcall-guarded: a field that doesn't exist is simply absent
-- from BOTH runs' hashes, which keeps them comparable.
--
-- Hashing note (this matters): the game runs Lua 5.2, where every number is a
-- double with a 53-bit mantissa. The original version multiplied a ~2^53
-- accumulator by a ~2^40 prime, so every product was rounded to a multiple of
-- 2^40 and only ~13 bits survived -- and the freshly added byte was annihilated
-- by the following multiply. That collides constantly and would have reported
-- MATCHING hashes for genuinely different states, i.e. a false pass on the one
-- experiment the whole architecture depends on. Below: two Lehmer lanes whose
-- products stay under 2^53, so the arithmetic is exact.

local SAMPLE_EVERY_DAYS = 1

-- Fixed speed for both instances. NOT the fastest setting: the point is to
-- advance the clock, and a higher speed only widens any per-frame scheduling
-- differences between two processes sharing one machine, which is the very
-- thing under test.
local M3_SPEED = 1

local lastSample = -1
local sampleIndex = 0
local explored = false
local speedSet = false

-- Lehmer / MINSTD lanes. max product = (M-1)*a:
--   lane 1: 2147483646 * 48271 ~= 1.04e14  < 2^53 (9.01e15)  -> exact
--   lane 2: 2147483628 * 40692 ~= 8.74e13  < 2^53            -> exact
local M1, A1 = 2147483647, 48271
local M2, A2 = 2147483629, 40692

local function hashStr(s)
	local h1, h2 = 2166136261 % M1, 2166136261 % M2
	for i = 1, #s do
		local b = s:byte(i)
		h1 = (h1 * A1 + b) % M1
		h2 = (h2 * A2 + b) % M2
	end
	return string.format("%010d-%010d", h1, h2)
end

local function num(x)
	if type(x) == "number" then return string.format("%.17g", x) end
	return tostring(x)
end

-- in-game days elapsed. getGameTime().time is game-seconds and the conversion
-- depends on the calendar speed, so derive it rather than assuming 1s == 1 day
-- (that assumption is only true at the default millisPerDay).
local function gameDays()
	local t, mpd
	pcall(function() t = game.interface.getGameTime().time end)
	pcall(function() mpd = game.interface.getMillisPerDay() end)
	if not t or not mpd or mpd == 0 then return nil end
	return t * (1000.0 / mpd)
end

local function appendEntityFields(parts, id, fields)
	local ok, e = pcall(function() return game.interface.getEntity(id) end)
	if not ok or not e then return end
	for _, f in ipairs(fields) do
		local v = e[f]
		if type(v) == "number" then
			parts[#parts + 1] = f .. "=" .. num(v)
		elseif type(v) == "table" and #v > 0 and type(v[1]) == "number" then
			for i = 1, #v do parts[#parts + 1] = f .. i .. "=" .. num(v[i]) end
		end
	end
end

-- Entity ids must be visited in a fixed order. getVehicles() etc. may hand
-- back engine iteration order, which is exactly the kind of thing that can
-- differ between runs without the underlying state differing -- that would
-- look like a desync when it is not one.
local function sortedCopy(t)
	local out = {}
	for _, v in pairs(t or {}) do out[#out + 1] = v end
	table.sort(out)
	return out
end

local function snapshot()
	local parts = {}

	-- vehicles: count + per-vehicle position
	--
	-- NOT api.engine.system.transportVehicleSystem.getVehicles(). That system
	-- exposes no functions at all, so the call throws, the pcall swallows it,
	-- and the list comes back EMPTY -- which is not a visible failure, it is a
	-- silently narrower experiment. This probe ran 42 samples reporting nv=0 on
	-- a save full of vehicles and declared the two instances identical: the
	-- hash covered line counters and towns but not a single moving thing, which
	-- is precisely the subsystem most likely to diverge. mpbridge.lua learned
	-- this the same way (see pollVehicles) and getEntities is the working call.
	local vehs = {}
	local vok, verr = pcall(function()
		local t = game.interface.getEntities(
			{ radius = 999999 }, { type = "VEHICLE", includeData = false }) or {}
		local out = {}
		for _, vid in pairs(t) do out[#out + 1] = vid end
		return out
	end)
	if vok and type(verr) == "table" then vehs = verr end
	vehs = sortedCopy(vehs)
	-- Say so loudly rather than hashing an empty set. A blind probe and a
	-- matching probe look identical from the outside.
	if #vehs == 0 then
		print("M3 WARNING: nv=0 -- vehicle query returned nothing. The hash is "
			.. "blind to vehicles; a match proves much less than it appears to.")
	end
	parts[#parts + 1] = "nv=" .. #vehs
	for _, vid in ipairs(vehs) do
		parts[#parts + 1] = "v" .. vid
		appendEntityFields(parts, vid, { "position" })
	end

	-- lines: transport counters (integers, exact)
	local lines = {}
	pcall(function()
		lines = api.engine.system.lineSystem.getLines() or {}
	end)
	lines = sortedCopy(lines)
	parts[#parts + 1] = "nl=" .. #lines
	for _, lid in ipairs(lines) do
		local ok, le = pcall(function() return game.interface.getEntity(lid) end)
		if ok and le and le.itemsTransported then
			local keys = {}
			for ct, _ in pairs(le.itemsTransported) do
				if type(ct) == "string" and ct:sub(1, 1) ~= "_" then keys[#keys + 1] = ct end
			end
			table.sort(keys)
			for _, ct in ipairs(keys) do
				local count = le.itemsTransported[ct]
				if type(count) == "number" then
					parts[#parts + 1] = "l" .. lid .. "." .. ct .. "=" .. count
				end
			end
		end
	end

	-- player balance + journal (financial events are an exact, evolving signal)
	pcall(function()
		local pid = api.engine.util.getPlayer()
		local e = game.interface.getEntity(pid)
		if e and e.balance then parts[#parts + 1] = "bal=" .. num(e.balance) end
	end)
	pcall(function()
		local j = game.interface.getPlayerJournal(0, 1000000, false)
		if j then
			local n, sum = 0, 0
			for k, v in pairs(j) do
				if type(v) == "number" and k ~= "_sum" then n = n + 1; sum = sum + v end
			end
			parts[#parts + 1] = "jcount=" .. n
			parts[#parts + 1] = "jsum=" .. num(sum)
			if j._sum then parts[#parts + 1] = "j_sum=" .. num(j._sum) end
		end
	end)

	-- towns: population -- the README already reports town roads diverging, so
	-- this is the field most likely to break first
	pcall(function()
		local towns = sortedCopy(api.engine.system.townSystem.getTowns() or {})
		parts[#parts + 1] = "nt=" .. #towns
		for _, tid in ipairs(towns) do
			parts[#parts + 1] = "t" .. tid
			appendEntityFields(parts, tid, { "population", "initialPopulation" })
		end
	end)

	return table.concat(parts, "|")
end

-- one-shot structure dump so we can see which fields actually exist and widen
-- the probe later
local function exploreOnce()
	if explored then return end
	explored = true
	local function dumpTable(name, t, depth)
		if type(t) ~= "table" or depth > 1 then return end
		for k, v in pairs(t) do
			local tv = type(v)
			if tv == "number" or tv == "string" or tv == "boolean" then
				print("M3X " .. name .. "." .. tostring(k) .. " = " .. tostring(v))
			elseif tv == "table" and depth < 1 then
				print("M3X " .. name .. "." .. tostring(k) .. " = <table>")
			end
		end
	end
	pcall(function()
		local vs = api.engine.system.transportVehicleSystem.getVehicles() or {}
		print("M3X vehicles=" .. #vs)
		if vs[1] then dumpTable("veh0", game.interface.getEntity(vs[1]), 0) end
	end)
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines() or {}
		print("M3X lines=" .. #ls)
		if ls[1] then dumpTable("line0", game.interface.getEntity(ls[1]), 0) end
	end)
	pcall(function()
		local pid = api.engine.util.getPlayer()
		dumpTable("player", game.interface.getEntity(pid), 0)
	end)
	pcall(function()
		for k, _ in pairs(api.engine.system) do print("M3X system: " .. tostring(k)) end
	end)
end

function data()
	return {
		init = function()
			print("M3 probe live; sampling every " .. SAMPLE_EVERY_DAYS .. " in-game day(s)")
		end,

		update = function()
			-- Unpause, from inside the script, on BOTH instances.
			--
			-- A loaded save starts at speed 0, and the probe samples per in-game
			-- DAY, so paused means zero samples and an experiment that looks
			-- like it ran and produced nothing. Doing it here rather than with
			-- synthetic keystrokes keeps the two instances symmetric: they issue
			-- the identical command at the identical tick of their own run,
			-- instead of whenever a GUI click happened to land.
			if not speedSet then
				local ok, s = pcall(game.interface.getGameSpeed)
				if ok and s == 0 then
					pcall(function()
						api.cmd.sendCommand(api.cmd.make.setGameSpeed(M3_SPEED))
					end)
					print("M3 unpausing: speed 0 -> " .. M3_SPEED)
				elseif ok and s ~= nil then
					speedSet = true
					print("M3 running at speed " .. tostring(s))
				end
			end

			local d = gameDays()
			if not d then return end
			local bucket = math.floor(d / SAMPLE_EVERY_DAYS)
			if bucket == lastSample then return end
			lastSample = bucket
			sampleIndex = sampleIndex + 1
			local snap = snapshot()
			-- i= is the ordinal so two runs can be lined up even if one is
			-- started a moment later; day= is the in-game clock
			print(string.format("M3 i=%d day=%d hash=%s len=%d",
				sampleIndex, bucket, hashStr(snap), #snap))
			if bucket % 10 == 0 then
				print("M3DETAIL i=" .. sampleIndex .. " day=" .. bucket .. " " .. snap)
			end
			if sampleIndex >= 1 then exploreOnce() end
		end,

		save = function()
			return { lastSample = lastSample, sampleIndex = sampleIndex }
		end,

		load = function(s)
			if s then
				lastSample = s.lastSample or -1
				sampleIndex = s.sampleIndex or 0
			end
		end,
	}
end
