-- mp/sandbox.lua -- Sandbox mode's tools, replicated
--
-- Added 2026-09-27. Loaded from the game script as
--     require("mp.sandbox")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- TOWNS (docs/re/SANDBOX.md). The stock Sandbox mode mod only turns on the game's
-- sandbox button; its town tool issues CreateTowns, which used to build the town on
-- the placing player's game only -- a desync, then a resync that threw the town
-- away (a player's report, 2026-09-27). The slice cancels the tool's command and
-- ships its TownInfo list as one TOWNC per town (native/src/slice/capture.inl);
-- every game, the placer included (a STRICT op, ARMED 1), builds it here at the stamp.
-- The tool waits on its command's callback (a "building town" bar); the slice fires
-- it at the cancel, so the tool is free at once and the town follows at the stamp.
--
-- Why a stamped replay is enough, all measured through EVAL:
--   * the same createTowns at the same step gives the same town on every load:
--     the same entity id, buildings, streets and -- with the name left empty, as
--     the tool leaves it -- the same name the engine picks;
--   * the cargo needs are the TOOL's choice, carried in the command; the engine
--     does not fill empty ones, so they travel whole and nobody chooses again;
--   * a command applies while the game is paused.
--
-- Wire: TOWNC pos=<x,y> c1= c2= c3= n1= n2= n3= name=
--   pos    "%.9g,%.9g" as the slice read the floats; a string on the wire, so
--          every game sets the same float32
--   c1..3  initialLandUseCapacities (residential, commercial, industrial)
--   n1..3  landUse2CargoNeeds, comma-joined cargo type ids, "-" for none
--   name   percent-encoded, "-" when the tool left the name to the engine

return function(CM, K, log)

-- "28,26,28" -> { 28, 26, 28 }; "-" or anything malformed -> nil (malformed also
-- returns an error text, so the command is refused rather than half-applied)
local function parseIds(s)
	s = tostring(s or "-")
	if s == "-" then return {} end
	local out = {}
	for tok in s:gmatch("[^,]+") do
		local n = tonumber(tok)
		if not n or n < 0 or n ~= math.floor(n) then return nil, "cargo id " .. tok end
		out[#out + 1] = n
	end
	return out
end
CM.sandboxParseIds = parseIds

-- The TownInfo the command describes, or nil and why. Pure: the tests run it.
function CM.townInfoFields(c)
	local x, y = tostring(c.pos or ""):match("^([%-%+%d%.eE]+),([%-%+%d%.eE]+)$")
	x, y = tonumber(x), tonumber(y)
	if not x or not y then return nil, "position " .. tostring(c.pos) end
	local caps = { tonumber(c.c1), tonumber(c.c2), tonumber(c.c3) }
	for i = 1, 3 do
		if not caps[i] or caps[i] < 0 then return nil, "capacity " .. i end
	end
	local needs = {}
	for i = 1, 3 do
		local list, err = parseIds(c["n" .. i])
		if not list then return nil, err end
		needs[i] = list
	end
	local name = tostring(c.name or "-")
	name = (name == "-") and "" or CM.unescName(name)
	return { x = x, y = y, caps = caps, needs = needs, name = name }
end

function CM.execTownCreate(c)
	local f, why = CM.townInfoFields(c)
	if not f then
		log(string.format("TOWNC seq=%s origin=%s: refused -- %s", tostring(c.seq), tostring(c.origin), why))
		return
	end
	local ok, err = pcall(function()
		local ti = api.type.TownInfo.new()
		ti.name = f.name
		ti.position.x = f.x
		ti.position.y = f.y
		for i = 1, 3 do
			ti.initialLandUseCapacities[i] = f.caps[i]
			-- engine vectors take their elements one at a time (a table is refused)
			local v = ti.landUse2CargoNeeds[i]
			for k, id in ipairs(f.needs[i]) do v[k] = id end
		end
		api.cmd.sendCommand(api.cmd.make.createTowns({ ti }), function(_, success)
			log(string.format("TOWNC seq=%s origin=%s at=%s: town at %.1f,%.1f success=%s",
				tostring(c.seq), tostring(c.origin), tostring(c.at), f.x, f.y, tostring(success)))
		end)
	end)
	if not ok then
		log(string.format("TOWNC seq=%s origin=%s: createTowns failed -- %s", tostring(c.seq), tostring(c.origin), tostring(err)))
	end
end

end
