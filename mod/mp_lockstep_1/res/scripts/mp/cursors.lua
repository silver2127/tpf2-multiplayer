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
--                 (game.gui.getTerrainPos) is kept as timed samples in
--                 tpf2mp_cursor_<me>.txt; every other player's samples, read from
--                 tpf2mp_cursors_in_<me>.txt, are played back as a zone circle in that
--                 player's colour (game.interface.setZone, the call the campaign
--                 draws areas with).
--   script state  CM.cursorTick, every tick: the samples our GUI took since the last
--                 LSCUR go out in one LSCUR (a still cursor is re-sent every
--                 K.CURSOR_KEEPALIVE_S); every peer's samples (CM.cursorRecv, called
--                 by net.lua) are written to tpf2mp_cursors_in_<me>.txt with its colour
--                 and the moment each batch arrived.
-- The two Lua states share nothing but files, which is why a cursor passes through
-- both of them. They do share the process clock (os.clock), which the arrival stamps
-- rely on.
--
-- MOTION (third pass, 2026-09-10). The second pass drew a circle 0.25 s behind the
-- reports and was still jumpy: the sender's script sent only its newest spot on each
-- tick (about five a second), and the receiver stamped a report with the moment its
-- own GUI read it, which lands on the receiver's script tick. Two unrelated
-- five-a-second clocks put 0-400 ms of jitter on the report times, so the circle kept
-- running out of reports, stopping, then lurching. Now:
--   * the sender's GUI samples the cursor about 30 times a second on its own clock
--     and the script sends every sample since its last LSCUR, so the path and its
--     timing are the sender's, not the ticks';
--   * the receiver plays the samples back on the sender's clock, mapped to its own by
--     the smallest delay seen lately, and trails by the spread of delays it measures
--     (bounded, and eased so playback never changes speed abruptly);
--   * between samples the circle follows a curve through the neighbouring samples
--     (Hermite with Catmull-Rom tangents) instead of straight segments;
--   * a cursor that holds still is pinned where it stopped, so a circle holds through
--     the pause and never glides across it; a jump of K.CURSOR_SNAP_M is a jump.
-- tools/cursor_sim.py (added with this) measures the stutter offline.
-- SIZE. The radius follows the camera distance continuously (1% steps), so a circle
-- keeps its size on screen through a zoom instead of stepping.
return function(CM, K, log)
K.CURSOR_KEEPALIVE_S = 3     -- a cursor that stays still is re-sent this often
K.CURSOR_STALE_S = 8         -- a peer's cursor not heard for this long is taken down
K.CURSOR_SEGMENTS = 24       -- points per circle
K.CURSOR_IO_FRAMES = 2       -- GUI frames between samples and file reads (30 a second at 60 fps)
K.CURSOR_MOVE_M = 0.05       -- a new sample only when the cursor moved this far
K.CURSOR_KEEP = 16           -- samples kept per cursor, in each file and on each track
K.CURSOR_SEND_MAX = 12       -- samples in one LSCUR at most (the newest)
K.CURSOR_GAP_S = 0.25        -- no sample for this long = the cursor held still
K.CURSOR_SNAP_M = 300        -- samples farther apart than this are a jump, not a glide
K.CURSOR_DELAY_MIN_S = 0.08  -- playback trails the fastest delivery by at least this
K.CURSOR_DELAY_MAX_S = 0.6   -- ...and by at most this, however bad the jitter
K.CURSOR_DELAY_PAD_S = 0.04  -- on top of the measured spread: one GUI read
K.CURSOR_JITTER_S = 10       -- the delay spread is measured over this long
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

-- net.lua hands every LSCUR line here: "LSCUR o=b p=<ms>,<x>,<y>;... z=<z>" is a
-- batch of the sender's samples, <ms> on the sender's clock; "LSCUR o=b off" takes
-- the circle down. A line with x= and y= and no p= (an older build) is one sample,
-- timed by its arrival.
function CM.cursorRecv(line)
	local o = line:match(" o=(%a+)")
	if not o or o == K.INSTANCE then return end
	CM.curWire = CM.curWire or {}
	local wall = os.time()
	if line:find(" off%s*$") then
		CM.curWire[o] = { off = true, wall = wall }
		CM.curDirty = true
		return
	end
	local c = CM.curWire[o]
	if not c or c.off then c = { samples = {} }; CM.curWire[o] = c end
	local arr = math.floor(((os.clock and os.clock()) or 0) * 1000 + 0.5)
	local s = c.samples
	local lastMs = s[#s] and s[#s].ms or -1
	local p = line:match(" p=(%S+)")
	if p then
		for ms, x, y in p:gmatch("(%d+),(%-?[%d%.]+),(%-?[%d%.]+)") do
			ms = tonumber(ms)
			if ms < lastMs - 5000 then s = {}; c.samples = s; lastMs = -1 end   -- the sender's game started again
			if ms > lastMs then
				s[#s + 1] = { ms = ms, x = tonumber(x), y = tonumber(y), arr = arr }
				lastMs = ms
			end
		end
	else
		local x, y = tonumber(line:match(" x=(%-?[%d%.]+)")), tonumber(line:match(" y=(%-?[%d%.]+)"))
		if not x or not y then return end
		if arr > lastMs then s[#s + 1] = { ms = arr, x = x, y = y, arr = arr } end
	end
	while #s > K.CURSOR_KEEP do table.remove(s, 1) end
	c.z = tonumber(line:match(" z=(%-?[%d%.]+)")) or c.z or 0
	c.wall = wall
	CM.curDirty = true
end

-- Every tick from the update loop.
function CM.cursorTick()
	if not K.INSTANCE or not K.BASE then return end
	local wall = os.time()
	-- our cursor: the samples the GUI state took since the last LSCUR
	local f = io.open(CM.cursorFile(K.INSTANCE), "r")
	local body = f and f:read("*a")
	if f then f:close() end
	if body and body:find("\nend", 1, true) then
		local sent = CM.curSent
		if body:find("\noff\n", 1, true) then
			if sent and not sent.off then
				CM.broadcast(string.format("LSCUR o=%s off", K.INSTANCE))
				CM.curSent = { off = true, wall = wall, ms = sent.ms }
			end
		else
			local list = {}
			for ms, x, y, z in body:gmatch("\n(%d+) (%-?[%d%.]+) (%-?[%d%.]+) (%-?[%d%.]+)") do
				list[#list + 1] = { ms = tonumber(ms), x = x, y = y, z = z }
			end
			local newest = list[#list]
			if newest then
				local from = (sent and sent.ms) or -1
				if newest.ms < from then from = -1 end   -- the GUI's clock started again (a reload)
				local fresh = {}
				for _, sm in ipairs(list) do if sm.ms > from then fresh[#fresh + 1] = sm end end
				while #fresh > K.CURSOR_SEND_MAX do table.remove(fresh, 1) end
				if #fresh == 0 and sent and not sent.off and wall - sent.wall >= K.CURSOR_KEEPALIVE_S then
					fresh = { newest }   -- still: say so now and then, or peers take the circle down
				end
				if #fresh > 0 then
					local parts = {}
					for i, sm in ipairs(fresh) do parts[i] = sm.ms .. "," .. sm.x .. "," .. sm.y end
					CM.broadcast(string.format("LSCUR o=%s p=%s z=%s", K.INSTANCE, table.concat(parts, ";"), newest.z))
					CM.curSent = { ms = newest.ms, wall = wall }
				end
			end
		end
	end
	-- every peer's cursor, for the GUI state to play back
	CM.curWrittenAt = CM.curWrittenAt or 0
	if CM.curDirty or (CM.curWire and next(CM.curWire) ~= nil and wall - CM.curWrittenAt >= 2) then
		CM.curDirty = false
		CM.curWrittenAt = wall
		local out = { "v2 " .. wall }
		for o, c in pairs(CM.curWire or {}) do
			if not c.off and c.samples and #c.samples > 0 and wall - c.wall <= K.CURSOR_STALE_S then
				local r, g, b = CM.cursorColor(o)
				out[#out + 1] = string.format("o %s %d %.3f %.3f %.3f", o, c.wall, r, g, b)
				for _, sm in ipairs(c.samples) do
					out[#out + 1] = string.format("s %d %.2f %.2f %d", sm.ms, sm.x, sm.y, sm.arr)
				end
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

-- One sample onto a player's track: t on the sender's clock, arr when its batch
-- reached our script, both in seconds. A jump or a held-still gap gets a pinned
-- point first, so playback holds instead of gliding. The delays (arr - t, which also
-- carries the two clocks' offset) of the last K.CURSOR_JITTER_S give the offset
-- (the smallest) and how far behind it playback must trail (the spread).
local function addSample(tr, t, x, y, arr, clk)
	local s = tr.samples
	local last = s[#s]
	if last then
		local dx, dy = x - last.x, y - last.y
		if dx * dx + dy * dy > K.CURSOR_SNAP_M * K.CURSOR_SNAP_M then
			s[#s + 1] = { t = t - 0.001, x = last.x, y = last.y }
		elseif t - last.t > K.CURSOR_GAP_S and (dx ~= 0 or dy ~= 0) then
			s[#s + 1] = { t = t - 0.03, x = last.x, y = last.y }
		end
	end
	s[#s + 1] = { t = t, x = x, y = y }
	local ds = tr.ds
	ds[#ds + 1] = { at = clk, d = arr - t }
	while #ds > 1 and ds[1].at < clk - K.CURSOR_JITTER_S do table.remove(ds, 1) end
	local lo, hi = math.huge, -math.huge
	for _, e in ipairs(ds) do
		if e.d < lo then lo = e.d end
		if e.d > hi then hi = e.d end
	end
	local spread = hi - lo + K.CURSOR_DELAY_PAD_S
	if spread < K.CURSOR_DELAY_MIN_S then spread = K.CURSOR_DELAY_MIN_S
	elseif spread > K.CURSOR_DELAY_MAX_S then spread = K.CURSOR_DELAY_MAX_S end
	tr.lagWant = lo + spread
	tr.lagLo = lo
end

-- The files, every K.CURSOR_IO_FRAMES frames: a sample of ours when the cursor moved,
-- and every player's new samples in. A file caught mid-write (no closing "end")
-- changes nothing.
local function cursorIo(me, clk)
	local x, y, z = terrainPos()
	CM.curMine = CM.curMine or {}
	local q, dirty = CM.curMine, false
	if x then
		local ms = math.floor(clk * 1000 + 0.5)
		local last = q[#q]
		if CM.curMineOff or not last or (x - last.x) ^ 2 + (y - last.y) ^ 2 >= K.CURSOR_MOVE_M ^ 2 then
			if last and not CM.curMineOff and ms - last.ms > K.CURSOR_GAP_S * 1000 then
				-- it held still until now: pin the spot, so a peer holds the circle through the pause
				q[#q + 1] = { ms = ms - 30, x = last.x, y = last.y, z = last.z }
			end
			q[#q + 1] = { ms = ms, x = x, y = y, z = z }
			while #q > K.CURSOR_KEEP do table.remove(q, 1) end
			CM.curMineOff, dirty = false, true
		end
	elseif not CM.curMineOff then
		CM.curMineOff, dirty = true, true
	end
	if dirty then
		local out = { "v2" }
		if CM.curMineOff then
			out[2] = "off"
		else
			for _, sm in ipairs(q) do out[#out + 1] = string.format("%d %.2f %.2f %.1f", sm.ms, sm.x, sm.y, sm.z or 0) end
		end
		out[#out + 1] = "end"
		local f = io.open(CM.cursorFile(me), "w")
		if f then f:write(table.concat(out, "\n") .. "\n"); f:close() end
	end

	local f = io.open(CM.cursorsInFile(me), "r")
	if not f then return end
	local body = f:read("*a") or ""
	f:close()
	if not body:find("\nend", 1, true) then return end
	CM.curTracks = CM.curTracks or {}
	local seen, tr = {}, nil
	for line in body:gmatch("[^\n]+") do
		local o, w, r, g, b = line:match("^o (%a+) (%d+) ([%d%.]+) ([%d%.]+) ([%d%.]+)")
		if o then
			tr = CM.curTracks[o]
			if not tr then tr = { samples = {}, ds = {}, lastMs = -1 }; CM.curTracks[o] = tr end
			tr.wall, tr.r, tr.g, tr.b, tr.gone = tonumber(w), tonumber(r), tonumber(g), tonumber(b), nil
			seen[o] = true
		elseif tr then
			local ms, sx, sy, arr = line:match("^s (%d+) (%-?[%d%.]+) (%-?[%d%.]+) (%d+)")
			ms = tonumber(ms)
			if ms then
				if ms < tr.lastMs - 5000 then tr.samples, tr.ds, tr.lastMs, tr.lag = {}, {}, -1, nil end   -- the sender started again
				if ms > tr.lastMs then
					addSample(tr, ms / 1000, tonumber(sx), tonumber(sy), tonumber(arr) / 1000, clk)
					tr.lastMs = ms
				end
			end
		end
	end
	-- a player no longer listed (cursor over a window, gone quiet, left) is taken down
	for o, t in pairs(CM.curTracks) do
		if not seen[o] then t.gone = true end
	end
end

-- Where a track stands at time rt (sender's clock): on a curve through the samples
-- around it. The tangents come from the neighbouring samples, scaled to the span, so
-- a pinned (held) point flattens them and the circle eases out of a pause; a held
-- segment, or a neighbour across a jump, is not curved at all.
local function trackAt(s, rt)
	local n = #s
	if rt <= s[1].t then return s[1].x, s[1].y end
	if rt >= s[n].t then return s[n].x, s[n].y end
	local snap2 = K.CURSOR_SNAP_M * K.CURSOR_SNAP_M
	for i = 2, n do
		local b = s[i]
		if rt <= b.t then
			local a = s[i - 1]
			local span = b.t - a.t
			if span <= 0 or (a.x == b.x and a.y == b.y) then return b.x, b.y end
			local u = (rt - a.t) / span
			local m1x, m1y, m2x, m2y = b.x - a.x, b.y - a.y, b.x - a.x, b.y - a.y
			local p0, p3 = s[i - 2], s[i + 1]
			if p0 and b.t > p0.t and (a.x - p0.x) ^ 2 + (a.y - p0.y) ^ 2 <= snap2 then
				local k = span / (b.t - p0.t)
				m1x, m1y = (b.x - p0.x) * k, (b.y - p0.y) * k
			end
			if p3 and p3.t > a.t and (p3.x - b.x) ^ 2 + (p3.y - b.y) ^ 2 <= snap2 then
				local k = span / (p3.t - a.t)
				m2x, m2y = (p3.x - a.x) * k, (p3.y - a.y) * k
			end
			local u2 = u * u
			local u3 = u2 * u
			local h00, h10, h01, h11 = 2 * u3 - 3 * u2 + 1, u3 - 2 * u2 + u, -2 * u3 + 3 * u2, u3 - u2
			return h00 * a.x + h10 * m1x + h01 * b.x + h11 * m2x, h00 * a.y + h10 * m1y + h01 * b.y + h11 * m2y
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
	local dt = CM.curLastClk and (clk - CM.curLastClk) or 0
	if dt < 0 or dt > 1 then dt = 0 end
	CM.curLastClk = clk
	CM.curFrame = (CM.curFrame or 0) + 1
	if CM.curFrame % K.CURSOR_IO_FRAMES == 1 or K.CURSOR_IO_FRAMES <= 1 then cursorIo(me, clk) end
	local gi = game and game.interface
	if not gi or not CM.curTracks then return end
	local now = os.time()
	local radius = cursorRadius()
	for o, tr in pairs(CM.curTracks) do
		local s = tr.samples
		if o == me or tr.gone or not tr.wall or now - tr.wall > K.CURSOR_STALE_S or #s == 0 then
			if tr.drawn then pcall(function() gi.setZone("mpcursor_" .. o, nil) end) end
			CM.curTracks[o] = nil
		else
			-- how far behind the sender's clock to play: up quickly toward what the jitter asks
			-- for, so samples do not run dry; down only when clearly more than needed, and
			-- slowly, so playback speed never wobbles with every new jitter reading
			local want = tr.lagWant or 0
			if not tr.lag then
				-- a new circle starts at the worst-case delay and eases down to what the
				-- jitter needs: starting low and growing ran dry (stalls) for the first seconds
				tr.lag = math.max(want, (tr.lagLo or want) + K.CURSOR_DELAY_MAX_S)
			elseif want > tr.lag then
				local k = 3.0 * dt
				if k > 1 then k = 1 end
				tr.lag = tr.lag + (want - tr.lag) * k
			elseif want < tr.lag - 0.05 then
				local k = 0.2 * dt
				if k > 1 then k = 1 end
				tr.lag = tr.lag + (want + 0.02 - tr.lag) * k
			end
			local rt = clk - tr.lag
			while #s > 4 and s[2].t < rt - 1.0 do table.remove(s, 1) end
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
