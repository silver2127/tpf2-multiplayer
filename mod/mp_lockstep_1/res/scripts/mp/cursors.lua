-- mp/cursors.lua -- where the other players' mouse cursors are: coloured ground circles
--
-- Added 2026-09-10. Loaded from the game script as
--     require("mp.cursors")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- COSMETIC ONLY. Nothing here enters the command queue or touches the world, so a
-- cursor that arrives late, or never, cannot fork the session.
--
--   GUI state     CM.cursorGuiTick: the ground point under our mouse
--                 (game.gui.getTerrainPos) goes to tpf2mp_cursor_<me>.txt; every
--                 other player's cursor, read from tpf2mp_cursors_in_<me>.txt, is
--                 drawn as a zone circle in that player's colour
--                 (game.interface.setZone, the call the campaign draws areas with).
--   script state  CM.cursorTick: our file goes out as LSCUR, on a move of
--                 K.CURSOR_MOVE_M or every K.CURSOR_KEEPALIVE_S while still; every
--                 peer's last LSCUR (CM.cursorRecv, called by net.lua) is written
--                 to tpf2mp_cursors_in_<me>.txt with its colour.
-- The two Lua states share nothing but files, which is why a cursor passes through
-- both of them.
return function(CM, K, log)
K.CURSOR_MOVE_M = 2          -- metres the cursor must move before it is sent again
K.CURSOR_KEEPALIVE_S = 3     -- a cursor that stays still is re-sent this often
K.CURSOR_STALE_S = 8         -- a peer's cursor not heard for this long is taken down
K.CURSOR_SEGMENTS = 20       -- points per circle

function CM.cursorFile(me) return K.BASE .. "tpf2mp_cursor_" .. tostring(me) .. ".txt" end
function CM.cursorsInFile(me) return K.BASE .. "tpf2mp_cursors_in_" .. tostring(me) .. ".txt" end

-- A cursor's colour: its company's chip colour when the companies are separate
-- (the lobby roster, the vehicles and the Multiplayer window share that palette),
-- otherwise one colour per player from the same palette by letter: a red, b blue,
-- c green, d yellow, ...
function CM.cursorColor(o)
	local cid = (CM.cmMode == "companies") and CM.cmOriginCompany and CM.cmOriginCompany[o]
	if cid then return CM.cmCompanyColor(cid) end
	local s, idx = tostring(o), 0
	for i = 1, #s do idx = idx * 26 + (s:byte(i) - 96) end
	return CM.cmCompanyColor(math.max(1, idx))
end

-- ---------- script state ----------

-- net.lua hands every LSCUR line here. One entry per origin; the newest wins.
function CM.cursorRecv(line)
	local o = line:match(" o=(%a+)")
	if not o or o == K.INSTANCE then return end
	CM.curWire = CM.curWire or {}
	if line:find(" off%s*$") then
		CM.curWire[o] = { off = true, wall = os.time() }
	else
		local x = tonumber(line:match(" x=(%-?%d+)"))
		local y = tonumber(line:match(" y=(%-?%d+)"))
		if not x or not y then return end
		CM.curWire[o] = { x = x, y = y, z = tonumber(line:match(" z=(%-?%d+)")) or 0, wall = os.time() }
	end
	CM.curDirty = true
end

-- Every tick from the update loop; does its work on even ticks.
function CM.cursorTick()
	if not K.INSTANCE or not K.BASE or (CM.ticks or 0) % 2 ~= 0 then return end
	local wall = os.time()
	-- our cursor, as the GUI state last wrote it
	local f = io.open(CM.cursorFile(K.INSTANCE), "r")
	local line = f and f:read("*l")
	if f then f:close() end
	if line then
		local last = CM.curSent
		local x, y, z = line:match("^(%-?%d+) (%-?%d+) (%-?%d+)")
		x, y, z = tonumber(x), tonumber(y), tonumber(z)
		if x and y then
			local moved = (not last) or last.off or ((x - last.x) ^ 2 + (y - last.y) ^ 2 >= K.CURSOR_MOVE_M ^ 2)
			if moved or wall - last.wall >= K.CURSOR_KEEPALIVE_S then
				CM.broadcast(string.format("LSCUR o=%s x=%d y=%d z=%d", K.INSTANCE, x, y, z or 0))
				CM.curSent = { x = x, y = y, wall = wall }
			end
		elseif line:find("^off") and last and not last.off then
			CM.broadcast(string.format("LSCUR o=%s off", K.INSTANCE))
			CM.curSent = { off = true, wall = wall }
		end
	end
	-- every peer's cursor, for the GUI state to draw
	CM.curWrittenAt = CM.curWrittenAt or 0
	if CM.curDirty or (CM.curWire and next(CM.curWire) ~= nil and wall - CM.curWrittenAt >= 2) then
		CM.curDirty = false
		CM.curWrittenAt = wall
		local out = { "v1 " .. wall }
		for o, c in pairs(CM.curWire or {}) do
			if not c.off and wall - c.wall <= K.CURSOR_STALE_S then
				local r, g, b = CM.cursorColor(o)
				out[#out + 1] = string.format("%s %d %d %d %d %.3f %.3f %.3f", o, c.x, c.y, c.z, c.wall, r, g, b)
			elseif wall - c.wall > 4 * K.CURSOR_STALE_S then
				CM.curWire[o] = nil
			end
		end
		out[#out + 1] = "end"
		local w = io.open(CM.cursorsInFile(K.INSTANCE), "w")
		if w then w:write(table.concat(out, "\n") .. "\n"); w:close() end
	end
end

-- ---------- GUI state ----------

-- {x, y, z} or {x=, y=, z=} -> x, y, z; nil for anything else.
local function posOf(p)
	if p == nil then return nil end
	local x, y, z
	pcall(function() x, y, z = tonumber(p[1]), tonumber(p[2]), tonumber(p[3]) end)
	if not x then pcall(function() x, y, z = tonumber(p.x), tonumber(p.y), tonumber(p.z) end) end
	if not x or not y or x ~= x or y ~= y then return nil end
	return x, y, z or 0
end

-- The ground point under the mouse, or nil (over a window, off the map).
-- game.gui.getTerrainPos is in the game binary, but the base game never calls it
-- and it is not documented, so the call shape is found at run time and logged:
-- no argument first; the mouse position as the argument if that raises, or if it
-- has returned nothing for a while. A shape that raises is not called again.
local function terrainPos()
	local g = game and game.gui
	if not g or CM.curShape == "none" then return nil end
	if CM.curShape ~= "mouse" then
		local ok, p = pcall(function() return g.getTerrainPos() end)
		if not ok then
			print("[ls-gui] cursors: game.gui.getTerrainPos() raised: " .. tostring(p) .. " -- trying the mouse position as its argument")
			CM.curShape = "mouse"
			return nil
		end
		local x, y, z = posOf(p)
		if x then
			if CM.curShape == nil then
				CM.curShape = "noarg"
				print(string.format("[ls-gui] cursors: game.gui.getTerrainPos() works: %.1f %.1f %.1f", x, y, z))
			end
			return x, y, z
		end
		if CM.curShape == nil then
			CM.curEmpty = (CM.curEmpty or 0) + 1
			if CM.curEmpty % 300 == 0 and not CM.curMouseBad then
				local ok2, p2 = pcall(function() return g.getTerrainPos(g.getMousePos()) end)
				if not ok2 then
					CM.curMouseBad = true
					print("[ls-gui] cursors: getTerrainPos() has returned no position yet (" .. type(p) .. "), and getTerrainPos(mouse) raised: " .. tostring(p2))
				elseif posOf(p2) then
					CM.curShape = "mouse"
					print("[ls-gui] cursors: getTerrainPos() returns nothing, getTerrainPos(mouse) works -- using that")
					return posOf(p2)
				end
			end
		end
		return nil
	end
	local ok, p = pcall(function() return g.getTerrainPos(g.getMousePos()) end)
	if not ok then
		print("[ls-gui] cursors: game.gui.getTerrainPos(mouse) raised: " .. tostring(p) .. " -- cursor sharing is off")
		CM.curShape = "none"
		return nil
	end
	local x, y, z = posOf(p)
	if x and not CM.curMouseOk then
		CM.curMouseOk = true
		print(string.format("[ls-gui] cursors: game.gui.getTerrainPos(mouse) works: %.1f %.1f %.1f", x, y, z))
	end
	return x, y, z
end

-- Circle radius in metres: about 2% of the camera distance, so a circle reads the
-- same size on screen at any zoom. Quantised, so a steady camera redraws nothing.
local function cursorRadius()
	local d
	pcall(function()
		local cam = game.gui.getCamera()
		d = tonumber(cam[3])
		if not d then d = tonumber(cam.distance) end
	end)
	if not CM.curCamLogged then
		CM.curCamLogged = true
		print("[ls-gui] cursors: camera distance " .. tostring(d))
	end
	if not d or d <= 0 then return 12 end
	local r = d * 0.02
	if r < 4 then r = 4 elseif r > 200 then r = 200 end
	if r < 20 then return math.floor(r + 0.5) end
	return 5 * math.floor(r / 5 + 0.5)
end

-- GUI update, several times a second.
function CM.cursorGuiTick()
	if not K.INSTANCE then pcall(CM.detectInstance) end
	local me = K.INSTANCE
	if not me or not K.BASE then return end
	-- ours, for the script state to send
	local x, y, z = terrainPos()
	local line = x and string.format("%d %d %d", math.floor(x + 0.5), math.floor(y + 0.5), math.floor(z + 0.5)) or "off"
	if line ~= CM.curOut then
		local f = io.open(CM.cursorFile(me), "w")
		if f then f:write(line .. "\n"); f:close(); CM.curOut = line end
	end
	-- theirs, as the script state last wrote them; a file caught mid-write (no
	-- closing "end") keeps the last complete set
	local f = io.open(CM.cursorsInFile(me), "r")
	if f then
		local body = f:read("*a") or ""
		f:close()
		if body:find("\nend") then
			local set = {}
			for o, cx, cy, _, w, r, g, b in body:gmatch("(%a+) (%-?%d+) (%-?%d+) (%-?%d+) (%d+) ([%d%.]+) ([%d%.]+) ([%d%.]+)") do
				set[o] = { x = tonumber(cx), y = tonumber(cy), wall = tonumber(w), r = tonumber(r), g = tonumber(g), b = tonumber(b) }
			end
			CM.curPeers = set
		end
	end
	local gi = game and game.interface
	if not gi then return end
	local now = os.time()
	local radius = cursorRadius()
	CM.curDrawn = CM.curDrawn or {}
	for o, c in pairs(CM.curPeers or {}) do
		if o ~= me and c.wall and now - c.wall <= K.CURSOR_STALE_S then
			local sig = string.format("%d %d %d %.2f %.2f %.2f", c.x, c.y, radius, c.r, c.g, c.b)
			if CM.curDrawn[o] ~= sig then
				local poly = {}
				for i = 1, K.CURSOR_SEGMENTS do
					local a = (i - 1) * 2 * math.pi / K.CURSOR_SEGMENTS
					poly[i] = { c.x + radius * math.cos(a), c.y + radius * math.sin(a) }
				end
				local ok, err = pcall(function()
					gi.setZone("mpcursor_" .. o, { polygon = poly, draw = true, drawColor = { c.r, c.g, c.b, 0.8 } })
				end)
				if ok then
					CM.curDrawn[o] = sig
				elseif not CM.curZoneErr then
					CM.curZoneErr = true
					print("[ls-gui] cursors: game.interface.setZone raised: " .. tostring(err))
				end
			end
		end
	end
	for o in pairs(CM.curDrawn) do
		local c = CM.curPeers and CM.curPeers[o]
		if not c or o == me or not c.wall or now - c.wall > K.CURSOR_STALE_S then
			pcall(function() gi.setZone("mpcursor_" .. o, nil) end)
			CM.curDrawn[o] = nil
		end
	end
end
end
