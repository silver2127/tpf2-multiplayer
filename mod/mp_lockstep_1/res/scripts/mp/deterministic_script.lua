-- Scoped compatibility for simulation callbacks. No persistent global patches:
-- networking, GUI callbacks and other mods retain their original clocks/RNG.
local M = {}
local unpack = table.unpack or unpack
local floor = math.floor
local KEY = "__tpf2mp_deterministic_v1"
local MOD = 2147483647
local EPOCH = 946684800 -- 2000-01-01 UTC; independent of the host's clock.
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
	local function invoke(fn, ...)
		local t = simTime()
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
		return unpack(result, 2, result.n)
	end
	for _, name in ipairs({"init", "handleEvent"}) do
		local fn = script[name]
		if fn then out[name] = function(...) return invoke(fn, ...) end end
	end
	if script.update then
		out.update = function(...)
			local now = simTime()
			-- Pause/render frequency must not cause extra RNG draws or mutations.
			if lastUpdate == now then return end
			local result = pack(invoke(script.update, ...))
			lastUpdate = now
			return unpack(result, 1, result.n)
		end
	end
	out.save = function(...)
		local payload = script.save and invoke(script.save, ...) or nil
		assert(payload == nil or type(payload) == "table", "deterministic script requires table save state")
		local saved = payload and copy(payload) or {}
		assert(saved[KEY] == nil, "deterministic save metadata collision")
		saved[KEY] = {version = 1, id = id, seed = seed, lastUpdate = lastUpdate, empty = payload == nil}
		return saved
	end
	out.load = function(saved, ...)
		seed, lastUpdate = hash(id), nil
		local payload = saved
		local meta = type(saved) == "table" and saved[KEY]
		if meta then
			assert(meta.version == 1 and meta.id == id and integer(meta.seed) and meta.seed > 0 and meta.seed < MOD,
				"invalid deterministic script save state")
			seed, lastUpdate = meta.seed, meta.lastUpdate
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
