-- Scoped compatibility for simulation callbacks. No persistent global patches:
-- networking, GUI callbacks and other mods retain their original clocks/RNG.
local M = {}
-- The game's init.lua replaces table.unpack with a one-argument wrapper.
-- Passing (results, 2, n) to it returns pcall's success flag as the payload.
-- Explicit bounds also preserve nil and trailing nil callback return values.
local function unpackRange(values, first, last)
	if first > last then return end
	return values[first], unpackRange(values, first + 1, last)
end
local floor = math.floor
local KEY = "__tpf2mp_deterministic_v1"
local MOD = 2147483647
local EPOCH = 946684800 -- 2000-01-01 UTC; independent of the host's clock.
-- DETERMINISTIC UPDATE GRID. A wrapped script's update must run at the SAME
-- simulation times on every instance, not on whatever frames each machine
-- happens to render. Natural Town Growth gates on "10 virtual seconds since
-- the last update" and then SNAPS its deadline to the tick it happened to
-- fire on, so a game catching up at 4x (sim time arriving in ~0.8 jumps rather
-- than 0.2) crossed that gate at 10.4 where a 1x game crossed at 10.0, and the
-- two towns grew on permanently different schedules: town lane -5, +2, +5 with
-- every other hash lane matching (2026-09-15). Frame rate alone can do it too.
-- So update runs once per whole virtual second, in order, each with the clock
-- pinned to that second: at any sim time T every instance has run exactly the
-- ticks <= T, which is what makes comparing a hash at a stamp meaningful.
local UPDATE_GRID = 1
-- A bound so a state from far in the past cannot spin. Both sides compute the
-- same jump from the same saved tick and the same sim clock, so it stays equal.
local MAX_CATCHUP_TICKS = 600
local function pack(...) return {n = select("#", ...), ...} end
local function copy(t)
	local out = {}
	for k, v in pairs(t) do out[k] = v end
	return out
end
local function hash(s)
	local n = 1
	for i = 1, #s do n = (n * 131 + s:byte(i)) % (MOD - 1) end
	return n + 1
end
local function integer(n)
	return type(n) == "number" and n == n and math.abs(n) < 2^53 and n == floor(n)
end
-- Gregorian calendar -> UTC seconds, with month/day overflow normalization.
local function utc(t)
	assert(type(t) == "table", "os.time expects a calendar table")
	local y, m, d = t.year, t.month, t.day
	assert(integer(y) and integer(m) and integer(d), "invalid calendar date")
	y, m = y + floor((m - 1) / 12), (m - 1) % 12 + 1
	local h, minute, second = t.hour or 12, t.min or 0, t.sec or 0
	assert(integer(h) and integer(minute) and integer(second), "invalid calendar time")
	y = y - (m <= 2 and 1 or 0)
	local era = floor(y / 400)
	local yo = y - era * 400
	local doy = floor((153 * (m + (m > 2 and -3 or 9)) + 2) / 5) + d - 1
	local days = era * 146097 + yo * 365 + floor(yo / 4) - floor(yo / 100) + doy - 719468
	return days * 86400 + h * 3600 + minute * 60 + second
end

function M.wrap(script, id, options)
	options = options or {}
	assert(type(script) == "table", "game script must return a table")
	local out = copy(script)
	local seed, lastUpdate = hash(id), nil
	local rawDate = os.date
	local collections = options.naturalTownGrowth and require("natural_town_growth/collections") or nil
	local function simTime()
		local t = game.interface.getGameTime().time
		assert(type(t) == "number" and t == t and t >= 0 and t < 2^52,
			"deterministic script requires the simulation clock")
		return floor(t * 5 + 0.5) / 5
	end
	-- The grid tick the update sequence resumes AFTER. Taken at load/init, which
	-- run at the same simulation time on every instance -- never at the first
	-- rendered frame, whose timing differs per machine and per catch-up speed.
	-- It is the tick BEFORE the current one, so the tick a load lands on still runs
	-- once, on every instance alike.
	local function anchorTick() return floor(simTime() / UPDATE_GRID) * UPDATE_GRID - UPDATE_GRID end
	local function random(...)
		local args = pack(...)
		assert(args.n <= 2, "wrong number of arguments to random")
		local low, high
		if args.n > 0 then
			low, high = args.n == 1 and 1 or args[1], args.n == 1 and args[1] or args[2]
			assert(integer(low) and integer(high) and low <= high and high - low < MOD - 1,
				"unsupported random interval")
		end
		-- Park-Miller: all products are exact with Lua's double representation.
		local function nextInt()
			seed = (seed * 48271) % MOD
			return seed - 1
		end
		local r = nextInt()
		if args.n == 0 then return r / (MOD - 1) end
		local width = high - low + 1
		local limit = (MOD - 1) - (MOD - 1) % width
		while r >= limit do r = nextInt() end
		return low + r % width
	end
	local function randomseed(n)
		assert(integer(n), "randomseed requires an integer")
		seed = hash(id .. ":" .. string.format("%.0f", n))
	end
	local function invokeAt(t, fn, ...)
		local oldRandom, oldSeed = math.random, math.randomseed
		local oldTime, oldDate, oldClock = os.time, os.date, os.clock
		local oldTowns = game.interface.getTowns
		local oldKeys = collections and collections.keys
		math.random, math.randomseed = random, randomseed
		os.time = function(calendar) return calendar and utc(calendar) or floor(EPOCH + t) end
		os.clock = function() return t end
		os.date = function(format, timestamp)
			format = format or "%c"
			if format:sub(1, 1) ~= "!" then format = "!" .. format end
			return rawDate(format, timestamp or floor(EPOCH + t))
		end
		if options.naturalTownGrowth then
			collections.keys = function(t)
				local keys = oldKeys(t)
				table.sort(keys)
				return keys
			end
			game.interface.getTowns = function(...)
				local ids = copy(oldTowns(...))
				table.sort(ids)
				return ids
			end
		end
		local result = pack(pcall(fn, ...))
		math.random, math.randomseed = oldRandom, oldSeed
		os.time, os.date, os.clock = oldTime, oldDate, oldClock
		game.interface.getTowns = oldTowns
		if collections then collections.keys = oldKeys end
		if not result[1] then error(result[2], 0) end
		return unpackRange(result, 2, result.n)
	end
	local function invoke(fn, ...) return invokeAt(simTime(), fn, ...) end
	for _, name in ipairs({"init", "handleEvent"}) do
		local fn = script[name]
		if fn then out[name] = function(...) return invoke(fn, ...) end end
	end
	-- A new game never calls load, so init anchors the grid instead.
	if script.init then
		local wrapped = out.init
		out.init = function(...)
			local r = pack(wrapped(...))
			lastUpdate = anchorTick()
			return unpackRange(r, 1, r.n)
		end
	end
	if script.update then
		out.update = function(...)
			local tick = floor(simTime() / UPDATE_GRID) * UPDATE_GRID
			-- load/init normally anchor the grid. This fallback covers a script wrapped
			-- and driven directly (no load, no init): run the current tick rather than
			-- silently nothing, which would leave such a script never updating at all.
			if lastUpdate == nil then lastUpdate = tick - UPDATE_GRID end
			if tick <= lastUpdate then return end   -- pause/render frequency draws nothing
			if tick - lastUpdate > MAX_CATCHUP_TICKS * UPDATE_GRID then
				lastUpdate = tick - MAX_CATCHUP_TICKS * UPDATE_GRID
			end
			local result
			local at = lastUpdate + UPDATE_GRID
			while at <= tick do
				result = pack(invokeAt(at, script.update, ...))
				lastUpdate = at
				at = at + UPDATE_GRID
			end
			if result then return unpackRange(result, 1, result.n) end
		end
	end
	-- THE ENGINE CALLS load EVERY FRAME (2026-09-16). save/load are not only the
	-- world's save and load: the engine syncs script state between its Lua
	-- states through them, so during play load arrives once per frame with the
	-- state the previous save returned (1,360 calls in one session on the host,
	-- 499 on the joiner, interleaved with the sim's own clock samples). This
	-- wrapper re-applied every one of them: the RNG seed and the update-grid
	-- anchor were rewritten from the saved values and the script's own load
	-- put its state table back to the saved copy, once per frame. Harmless as
	-- long as the echo carries exactly what the last save returned (it does),
	-- but a needless per-frame rewrite of live state, and a trap the moment
	-- save and load stop being back to back. (The town-lane desyncs this was
	-- first suspected of were the running autosave: the joiner loaded a world
	-- the host had already left -- see the hot-join held save.)
	--
	-- A load is applied only when its metadata is not what this wrapper handed
	-- out in a save since the last real load: a world load brings a different
	-- seed/anchor pair (or none), an echo brings ours back. The multiplayer
	-- script's own load handlers work the same way (take a value only when we
	-- have none yet).
	local echoed = nil   -- {seed, lastUpdate} of the last save we returned
	out.save = function(...)
		local payload = script.save and invoke(script.save, ...) or nil
		assert(payload == nil or type(payload) == "table", "deterministic script requires table save state")
		local saved = payload and copy(payload) or {}
		assert(saved[KEY] == nil, "deterministic save metadata collision")
		saved[KEY] = {version = 1, id = id, seed = seed, lastUpdate = lastUpdate, empty = payload == nil}
		echoed = {seed = seed, lastUpdate = lastUpdate}
		return saved
	end
	local echoes = 0
	out.load = function(saved, ...)
		local meta = type(saved) == "table" and saved[KEY]
		if meta and echoed and meta.seed == echoed.seed and meta.lastUpdate == echoed.lastUpdate then
			echoes = echoes + 1
			if echoes == 1 or echoes % 1000 == 0 then
				print("[mpdet] " .. id .. ": load is the engine echoing our own state (x" .. echoes .. ") -- ignored")
			end
			return
		end
		echoed, echoes = nil, 0
		seed, lastUpdate = hash(id), anchorTick()
		local payload = saved
		if meta then
			assert(meta.version == 1 and meta.id == id and integer(meta.seed) and meta.seed > 0 and meta.seed < MOD,
				"invalid deterministic script save state")
			seed, lastUpdate = meta.seed, meta.lastUpdate or anchorTick()
			payload = copy(saved)
			payload[KEY] = nil
			if meta.empty then payload = nil end
		elseif options.naturalTownGrowth and type(saved) == "table" then
			-- Old state contains wall epochs. Preserve capacity values/factors;
			-- restart cooldowns together instead of mixing incompatible clocks.
			payload = copy(saved)
			local epoch = floor(EPOCH + simTime())
			payload.lastUpdatedAtEpoch = epoch - 10
			payload.capacitiesByTownId = {}
			for town, capacities in pairs(saved.capacitiesByTownId or {}) do
				local migrated = copy(capacities)
				for _, kind in ipairs({"residential", "commercial", "industrial"}) do
					if capacities[kind] then
						migrated[kind] = copy(capacities[kind])
						migrated[kind].setAtEpoch = epoch
					end
				end
				payload.capacitiesByTownId[town] = migrated
			end
		end
		print("[mpdet] " .. id .. " v1 loaded: " .. (meta and "restored RNG/clock state" or "initialized simulation clock"))
		if script.load then return invoke(script.load, payload, ...) end
	end
	return out
end
return M
