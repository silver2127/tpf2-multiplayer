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
--   GUI state     CM.cursorGuiTick, every frame: the ground point under our mouse
--                 (game.gui.getTerrainPos) goes to tpf2mp_cursor_<me>.txt; every
--                 other player's cursor, read from tpf2mp_cursors_in_<me>.txt, is
--                 drawn as a zone circle in that player's colour
--                 (game.interface.setZone, the call the campaign draws areas with).
--   script state  CM.cursorTick, every tick: our file goes out as LSCUR on a move of
--                 K.CURSOR_MOVE_M, or every K.CURSOR_KEEPALIVE_S while still; every
--                 peer's last LSCUR (CM.cursorRecv, called by net.lua) is written
--                 to tpf2mp_cursors_in_<me>.txt with its colour.
-- The two Lua states share nothing but files, which is why a cursor passes through
-- both of them.
--
-- MOTION (2026-09-10, second pass). Reports arrive at the script tick rate, about
-- five a second. Moving a circle straight to each report was jumpy, and gliding
-- quickly toward the newest one still went stop-and-go. A circle is now drawn
-- K.CURSOR_DELAY_S in the past and interpolated between the two reports around
-- that moment, the way games draw remote players: steady motion for a small,
-- fixed delay.
-- SIZE. The radius follows the camera distance continuously (1% steps), so a circle
-- keeps its size on screen through a zoom instead of stepping.
return function(CM, K, log)
K.CURSOR_MOVE_M = 0.5        -- metres the cursor must move before it is sent again
K.CURSOR_KEEPALIVE_S = 3     -- a cursor that stays still is re-sent this often
K.CURSOR_STALE_S = 8         -- a peer's cursor not heard for this long is taken down
K.CURSOR_SEGMENTS = 24       -- points per circle
K.CURSOR_DELAY_S = 0.25      -- circles are drawn this far behind the reports, interpolated between them
K.CURSOR_GAP_S = 0.5         -- a report after a pause this long starts moving 0.2 s before it, not across the pause
K.CURSOR_SNAP_M = 300        -- consecutive reports farther apart than this are a jump, not a glide
K.CURSOR_IO_FRAMES = 3       -- GUI frames between file reads and writes (20 a second at 60 fps)
K.CURSOR_SCREEN = 0.02       -- circle radius as a share of the camera distance

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
		local x = tonumber(line:match(" x=(%-?[%d%.]+)"))
		local y = tonumber(line:match(" y=(%-?[%d%.]+)"))
		if not x or not y then return end
		CM.curWire[o] = { x = x, y = y, z = tonumber(line:match(" z=(%-?[%d%.]+)")) or 0, wall = os.time() }
	end
	CM.curDirty = true
end

-- Every tick from the update loop.
function CM.cursorTick()
	if not K.INSTANCE or not K.BASE then return end
	local wall = os.time()
	-- our cursor, as the GUI state last wrote it
	local f = io.open(CM.cursorFile(K.INSTANCE), "r")
	local line = f and f:read("*l")
	if f then f:close() end
	if line then
		local last = CM.curSent
		local x, y, z = line:match("^(%-?[%d%.]+) (%-?[%d%.]+) (%-?[%d%.]+)")
		x, y, z = tonumber(x), tonumber(y), tonumber(z)
		if x and y then
			local moved = (not last) or last.off or ((x - last.x) ^ 2 + (y - last.y) ^ 2 >= K.CURSOR_MOVE_M ^ 2)
			if moved or wall - last.wall >= K.CURSOR_KEEPALIVE_S then
				CM.broadcast(string.format("LSCUR o=%s x=%.1f y=%.1f z=%.1f", K.INSTANCE, x, y, z or 0))
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
				out[#out + 1] = string.format("%s %.1f %.1f %.1f %d %.3f %.3f %.3f", o, c.x, c.y, c.z, c.wall, r, g, b)
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
-- (Measured 2026-09-10: the no-argument call works.)
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

-- Circle radius in metres: K.CURSOR_SCREEN of the camera distance, so a circle keeps
-- its size on screen at any zoom. 1% steps: a zoom redraws smoothly, a still camera
-- redraws nothing. (getCamera()[3] is the distance: 230 m logged on a default view.)
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
	local r = d * K.CURSOR_SCREEN
	if r < 0.5 then r = 0.5 elseif r > 2000 then r = 2000 end
	return math.exp(math.floor(math.log(r) / 0.01 + 0.5) * 0.01)
end

-- The files, every K.CURSOR_IO_FRAMES frames: ours out, theirs in. A file caught
-- mid-write (no closing "end") changes nothing. Each new reported spot becomes a
-- sample on that player's track, stamped with the time we saw it.
local function cursorIo(me, clk)
	local x, y, z = terrainPos()
	local line = x and string.format("%.1f %.1f %.1f", x, y, z) or "off"
	if line ~= CM.curOut then
		local f = io.open(CM.cursorFile(me), "w")
		if f then f:write(line .. "\n"); f:close(); CM.curOut = line end
	end
	local f = io.open(CM.cursorsInFile(me), "r")
	if not f then return end
	local body = f:read("*a") or ""
	f:close()
	if not body:find("\nend") then return end
	local set = {}
	local num = "(%-?[%d%.]+)"
	for o, cx, cy, _, w, r, g, b in body:gmatch("(%a+) " .. num .. " " .. num .. " " .. num .. " (%d+) ([%d%.]+) ([%d%.]+) ([%d%.]+)") do
		set[o] = { x = tonumber(cx), y = tonumber(cy), wall = tonumber(w), r = tonumber(r), g = tonumber(g), b = tonumber(b) }
	end
	CM.curTracks = CM.curTracks or {}
	for o, c in pairs(set) do
		local tr = CM.curTracks[o]
		if not tr then
			-- a new circle appears at once, where it was reported
			tr = { samples = { { t = clk - K.CURSOR_DELAY_S, x = c.x, y = c.y } } }
			CM.curTracks[o] = tr
		else
			local s = tr.samples
			local last = s[#s]
			if last.x ~= c.x or last.y ~= c.y then
				local dx, dy = c.x - last.x, c.y - last.y
				if dx * dx + dy * dy > K.CURSOR_SNAP_M * K.CURSOR_SNAP_M then
					s[#s + 1] = { t = clk, x = last.x, y = last.y }
					s[#s + 1] = { t = clk + 0.001, x = c.x, y = c.y }
				else
					if clk - last.t > K.CURSOR_GAP_S then
						s[#s + 1] = { t = clk - 0.2, x = last.x, y = last.y }
					end
					s[#s + 1] = { t = clk, x = c.x, y = c.y }
				end
			end
		end
		tr.wall, tr.r, tr.g, tr.b = c.wall, c.r, c.g, c.b
	end
	-- a player no longer listed (cursor over a window, gone quiet, left) is taken down
	for o, tr in pairs(CM.curTracks) do
		if not set[o] then tr.gone = true end
	end
end

-- Where a track stands at time rt: interpolated between the two samples around it.
local function trackAt(s, rt)
	if rt <= s[1].t then return s[1].x, s[1].y end
	local n = #s
	if rt >= s[n].t then return s[n].x, s[n].y end
	for i = 2, n do
		local b = s[i]
		if rt <= b.t then
			local a = s[i - 1]
			local u = (b.t > a.t) and (rt - a.t) / (b.t - a.t) or 1
			return a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u
		end
	end
	return s[n].x, s[n].y
end

-- GUI update, every frame.
function CM.cursorGuiTick()
	if not K.INSTANCE then pcall(CM.detectInstance) end
	local me = K.INSTANCE
	if not me or not K.BASE then return end
	-- os.clock is wall time on Windows (the CRT's clock()); without it, frames at 60 fps
	local clk
	if os and os.clock then clk = os.clock() else CM.curFakeClk = (CM.curFakeClk or 0) + 1 / 60; clk = CM.curFakeClk end
	CM.curFrame = (CM.curFrame or 0) + 1
	if CM.curFrame % K.CURSOR_IO_FRAMES == 1 then cursorIo(me, clk) end
	local gi = game and game.interface
	if not gi or not CM.curTracks then return end
	local now = os.time()
	local radius = cursorRadius()
	local rt = clk - K.CURSOR_DELAY_S
	for o, tr in pairs(CM.curTracks) do
		local s = tr.samples
		if o == me or tr.gone or not tr.wall or now - tr.wall > K.CURSOR_STALE_S then
			if tr.drawn then pcall(function() gi.setZone("mpcursor_" .. o, nil) end) end
			CM.curTracks[o] = nil
		else
			while #s > 2 and s[2].t < rt - 1.0 do table.remove(s, 1) end
			local px, py = trackAt(s, rt)
			-- centimetres: coarser, and the end of a glide would never be drawn
			local sig = string.format("%.2f %.2f %.3f %.2f %.2f %.2f", px, py, radius, tr.r, tr.g, tr.b)
			if tr.sig ~= sig then
				local poly = {}
				for i = 1, K.CURSOR_SEGMENTS do
					local a = (i - 1) * 2 * math.pi / K.CURSOR_SEGMENTS
					poly[i] = { px + radius * math.cos(a), py + radius * math.sin(a) }
				end
				local ok, err = pcall(function()
					gi.setZone("mpcursor_" .. o, { polygon = poly, draw = true, drawColor = { tr.r, tr.g, tr.b, 0.8 } })
				end)
				if ok then
					tr.sig, tr.drawn = sig, true
				elseif not CM.curZoneErr then
					CM.curZoneErr = true
					print("[ls-gui] cursors: game.interface.setZone raised: " .. tostring(err))
				end
			end
		end
	end
end
end
