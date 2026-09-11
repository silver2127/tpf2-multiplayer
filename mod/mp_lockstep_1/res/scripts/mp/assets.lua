-- mp/assets.lua -- the asset brush: trees, rocks and bushes (ASSETCAP -> ASSETS)
--
-- Added 2026-09-11. Loaded from the game script as
--     require("mp.assets")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- Why it matters: an asset-brush stroke that exists on one game only changes
-- where that game's towns grow, and the session desyncs minutes later on town
-- buildings and roads (2026-09-11: 104 trees on A only).
--
-- A stroke commits a proposal of ASSET GROUPS: the groups it touched are
-- removed (by id) and re-added with the models they keep, plus new groups. Lua
-- cannot build those records, so the slice does it natively, like terrain.
--
--   originator  the slice CANCELS the stroke and writes
--               ASSETCAP <bytes> <base64 stroke> <removals> <id,id,...|->
--               behind ARMED 1. CM.assetCapture turns the removed ids into
--               positions while those groups still stand, and schedules ASSETS
--               for every instance, this one included.
--   every game  CM.execAssets at the stamp finds its own group for each removal,
--               writes asset_inject_<me>.txt ("rm <ids>" + the stroke) and sends
--               an EMPTY script buildProposal; the slice fills it with the groups
--               and removals (InjectAssetsFromFile) and deletes the file.
--
-- A group is named by its engine position and model count. That is all Lua can
-- read of one (game.interface.getEntity: position, count and a model -> count
-- map), and on synced worlds both are identical everywhere.
return function(CM, K, log)
local function b64Len(n) return 4 * math.ceil(n / 3) end

function CM.assetGroupSig(id)
	local sig
	pcall(function()
		if not (id and api.engine.entityExists(id)) then return end
		local e = game.interface.getEntity(id)
		local p = e and e.position
		if e and e.type == "ASSET_GROUP" and p and e.count then
			sig = string.format("%.3f,%.3f,%.3f,%d", p[1], p[2], p[3], e.count)
		end
	end)
	return sig
end

function CM.assetGroupFind(sig)
	local x, y, z, n = tostring(sig):match("^([%-%d%.]+),([%-%d%.]+),([%-%d%.]+),(%d+)$")
	x, y, z, n = tonumber(x), tonumber(y), tonumber(z), tonumber(n)
	if not (x and y and z and n) then return nil end
	local hit
	pcall(function()
		local list = game.interface.getEntities({ pos = { x, y }, radius = 4 }, { type = "ASSET_GROUP", includeData = false }) or {}
		local ids = {}
		for _, id in pairs(list) do ids[#ids + 1] = id end
		table.sort(ids)
		for _, id in ipairs(ids) do
			local e = game.interface.getEntity(id)
			local p = e and e.position
			if p and e.count == n and math.abs(p[1] - x) < 0.01 and math.abs(p[2] - y) < 0.01 and math.abs(p[3] - z) < 0.01 then
				hit = id
				break
			end
		end
	end)
	return hit
end

function CM.assetCapture(w)
	local n, d, nrm, idtxt = tonumber(w[2]), w[3], tonumber(w[4]), w[5]
	if not (n and n > 0 and type(d) == "string" and nrm and idtxt) or #d ~= b64Len(n) or d:find("[^%w%+/=]") then
		log(string.format("ASSETCAP: damaged line (%s bytes, %s B of text) -- the stroke is NOT replicated",
			tostring(w[2]), tostring(type(d) == "string" and #d or nil)))
		return
	end
	local armed = (CM.lastArmed or 0) == 1
	local sigs, lost = {}, 0
	if idtxt ~= "-" then
		for tok in idtxt:gmatch("[^,]+") do
			local sig = CM.assetGroupSig(tonumber(tok))
			if sig then sigs[#sigs + 1] = sig else lost = lost + 1 end
		end
	end
	if lost > 0 then
		-- A removed group nobody can name: replaying the rest would leave it standing
		-- next to its own re-added models. Drop the stroke instead.
		log(string.format("ASSETCAP: %d of %d removed group(s) no longer stand here -- the stroke is NOT replicated%s",
			lost, nrm, armed and " (it was cancelled here too: paint it again)" or " (it ran on this game only)"))
		return
	end
	local args = { d = d, n = n }
	if #sigs > 0 then args.rm = table.concat(sigs, ";") end
	if not armed then args.skipOrigin = 1 end
	CM.scheduleLocal("ASSETS", args)
	log(string.format("ASSETCAP: %d B stroke, %d group(s) replaced -- %s", n, #sigs,
		armed and "cancelled here, every instance applies it at the stamp" or "applied here natively, peers apply it at the stamp"))
end

function CM.execAssets(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local seq, origin = tostring(c.seq), tostring(c.origin)
	local d, n = c.d, tonumber(c.n)
	if type(d) ~= "string" or not n or #d ~= b64Len(n) then
		log(string.format("EXEC ASSETS seq=%s from %s: payload damaged -- NOT applied, the assets here now differ", seq, origin))
		return
	end
	local ids, missing = {}, 0
	for sig in tostring(c.rm or ""):gmatch("[^;]+") do
		local id = CM.assetGroupFind(sig)
		if id then ids[#ids + 1] = tostring(id) else missing = missing + 1 end
	end
	if missing > 0 then
		log(string.format("EXEC ASSETS seq=%s from %s: %d group(s) the stroke replaces are not here -- applied without removing them (this game already differed)",
			seq, origin, missing))
	end
	local path = K.BASE .. "asset_inject_" .. K.INSTANCE .. ".txt"
	local f = io.open(path, "wb")
	if not f then
		log(string.format("EXEC ASSETS seq=%s from %s: cannot write %s -- NOT applied, the assets here now differ", seq, origin, path))
		return
	end
	f:write("rm " .. (#ids > 0 and table.concat(ids, ",") or "-") .. "\n" .. d)
	f:close()
	local ok, err = pcall(function()
		local ctx = api.type.Context:new()
		ctx.checkTerrainAlignment = false
		ctx.cleanupStreetGraph = false
		ctx.gatherBuildings = false
		ctx.gatherFields = false
		ctx.player = api.engine.util.getPlayer()
		api.cmd.sendCommand(api.cmd.make.buildProposal(api.type.SimpleProposal.new(), ctx, false), function(_, success)
			log(string.format("EXEC ASSETS seq=%s from %s: carrier %s", seq, origin,
				success and "applied" or "REJECTED -- the assets here now differ"))
			-- originator only: the slice holds the brush (its wait flag) from the
			-- cancelled stroke until the replay has applied; this empty proposal is
			-- the marker it releases on (see terrain.lua)
			if origin == K.INSTANCE then
				pcall(function()
					api.cmd.sendCommand(api.cmd.make.buildProposal(api.type.SimpleProposal.new(), ctx, false), function() end)
				end)
			end
		end)
	end)
	log(string.format("EXEC ASSETS seq=%s from %s: %d B stroke, %d removal(s) matched, carrier sent=%s%s",
		seq, origin, n, #ids, tostring(ok), ok and "" or (" err=" .. tostring(err))))
end
end
