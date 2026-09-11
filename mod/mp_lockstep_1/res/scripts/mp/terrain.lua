-- mp/terrain.lua -- terraforming and terrain painting (TERRAINCAP -> TERRAIN)
--
-- Added 2026-09-11. Loaded from the game script as
--     require("mp.terrain")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- A terraform or paint stroke commits a BuildProposal whose whole edit is three
-- grids: absolute heights on 4 m cells, material on 1 m texels, and a mask. A
-- script SimpleProposal cannot express them, so the grids travel as bytes.
--
--   originator  the slice CANCELS the commit at CommandList::Add and writes
--               TERRAINCAP <bytes> <base64> behind ARMED 1 once the cancel has
--               landed; CM.terrainCapture schedules TERRAIN for everyone, this
--               instance included. Behind ARMED 0 (the edit had to run natively
--               here) it schedules with skipOrigin, the v1 fallback.
--   every peer  CM.execTerrain at the stamp writes the base64 to
--               terrain_inject_<me>.bin and sends an EMPTY script buildProposal;
--               the slice sees the empty carrier, fills its grids from the file
--               (InjectTerrainFromFile) and deletes the file.
--
-- STRICT since 2026-09-11: every instance applies the grids at the same stamp.
-- Terrain moves no vehicle, but a road built or a town grown in the window
-- between the originator's native edit and the peers' replay reads a height
-- the others do not have yet -- the seed of a later drift.
return function(CM, K, log)
-- the wire length of n bytes of base64
local function b64Len(n) return 4 * math.ceil(n / 3) end

function CM.terrainCapture(w)
	local n, d = tonumber(w[2]), w[3]
	if not (n and n > 0 and type(d) == "string") or #d ~= b64Len(n) or d:find("[^%w%+/=]") then
		log(string.format("TERRAINCAP: damaged line (%s bytes, %s B of text) -- the edit stays on this instance only",
			tostring(w[2]), tostring(d and #d)))
		return
	end
	local armed = (CM.lastArmed or 0) == 1
	local args = { d = d, n = n }
	if not armed then args.skipOrigin = 1 end
	CM.scheduleLocal("TERRAIN", args)
	log(string.format("TERRAINCAP: %d B terrain edit (%d B on the wire) -- %s", n, #d,
		armed and "cancelled here, every instance applies it at the stamp"
		      or "applied here natively, peers apply it at the stamp"))
end

function CM.execTerrain(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local seq, origin = tostring(c.seq), tostring(c.origin)
	local d, n = c.d, tonumber(c.n)
	if type(d) ~= "string" or not n or #d ~= b64Len(n) then
		log(string.format("EXEC TERRAIN seq=%s from %s: payload damaged (%s B of text for %s B) -- NOT applied, the terrain here now differs",
			seq, origin, tostring(type(d) == "string" and #d or nil), tostring(c.n)))
		return
	end
	local path = K.BASE .. "terrain_inject_" .. K.INSTANCE .. ".bin"
	local f = io.open(path, "wb")
	if not f then
		log(string.format("EXEC TERRAIN seq=%s from %s: cannot write %s -- NOT applied, the terrain here now differs", seq, origin, path))
		return
	end
	f:write(d)
	f:close()
	local ok, err = pcall(function()
		local ctx = api.type.Context:new()
		ctx.checkTerrainAlignment = false
		ctx.cleanupStreetGraph = false
		ctx.gatherBuildings = false
		ctx.gatherFields = false
		ctx.player = api.engine.util.getPlayer()
		api.cmd.sendCommand(api.cmd.make.buildProposal(api.type.SimpleProposal.new(), ctx, false), function(_, success)
			log(string.format("EXEC TERRAIN seq=%s from %s: carrier %s", seq, origin,
				success and "applied" or "REJECTED -- the terrain here now differs"))
			-- STRICT, originator only: the slice holds the terrain tool (its wait
			-- flag) from the cancelled commit until the replay has APPLIED, and
			-- this callback is the first moment that is true. A second, empty
			-- proposal is the marker it releases on: nothing to apply, no grid
			-- file left to inject, and only ever sent here.
			if origin == K.INSTANCE then
				pcall(function()
					api.cmd.sendCommand(api.cmd.make.buildProposal(api.type.SimpleProposal.new(), ctx, false), function() end)
				end)
			end
		end)
	end)
	-- the slice deletes the file as it fills the carrier; still there means it
	-- has not been used yet (or the carrier was never sent)
	local left = io.open(path, "rb")
	if left then left:close() end
	log(string.format("EXEC TERRAIN seq=%s from %s: %d B edit, carrier sent=%s%s%s", seq, origin, n, tostring(ok),
		ok and "" or (" err=" .. tostring(err)),
		left and " (the grid file is still unread after sendCommand)" or " (grids filled)"))
end
end
