-- mp/stats.lua -- the Multiplayer window's stats section, in words
--
-- Added 2026-09-11. Loaded from the game script as
--     require("mp.stats")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- GUI STATE ONLY. The stats table used to be twelve raw counters per player
-- (game time, peer time, skew, apply lag, commands applied, ...) and a verdict
-- like "DESYNC e+z vs a". This turns the same dash-file values into a status
-- line -- do the worlds match, and if not WHAT differs, since when, and what to
-- do -- and one row per player. The raw table stays behind a "numbers" toggle.
return function(CM, K, log)
local NL = string.char(10)

-- Shared by hosts and joiners; page the roster so large lobbies fit on screen.
function CM.lobbyPanelPage(body, requested)
	local lines = {}
	for line in tostring(body or ""):gmatch("([^\n]*)\n") do lines[#lines + 1] = line end
	if #lines < 3 then return "", 1, 1 end
	local count = #lines - 3
	local pages = math.max(1, math.ceil(count / 8))
	local page = math.max(1, math.min(pages, tonumber(requested) or 1))
	local out = { lines[2], lines[1] .. " - " .. lines[3], string.format("%d player(s) - page %d/%d", count, page, pages) }
	for i = 4 + (page - 1) * 8, math.min(#lines, 3 + page * 8) do out[#out + 1] = lines[i] end
	return table.concat(out, NL), page, pages
end

-- the hash lanes (hash.lua worldHash detail) in words
local LANES = {
	e = "roads and tracks", z = "road and track heights", c = "player buildings and stations",
	v = "the number of vehicles", p = "vehicle positions", t = "town buildings",
	m = "money", l = "loans", n = "people counts",
}
local ORDER = { "e", "z", "c", "v", "p", "t", "m", "l", "n" }
-- lanes that usually differ BECAUSE something else did: listed after the rest
local SIDE = { m = true, l = true, n = true }

-- A dash verdict in words. The script state writes "SYNC", "-" (no comparison
-- yet), "OFF" (no hash: the map is larger than vanilla allows), "DESYNC e+z vs a"
-- (the hash lanes that differ), "DESYNC town +5 vs a" (town buildings, ours minus
-- theirs) or "DESYNC vpos 12m vs b" (vehicle drift), and a peer's own column just
-- "DESYNC". Returns state ("sync", "desync", "off" or "checking"), the other
-- player's letter (or nil) and what differs (or nil).
function CM.verdictWords(v)
	v = tostring(v or "-")
	if v == "SYNC" then return "sync" end
	if v == "OFF" then return "off" end
	if v:sub(1, 6) ~= "DESYNC" then return "checking" end
	local d, who = v:match("^DESYNC town ([%+%-]%d+) vs (%a+)")
	if d then
		d = tonumber(d)
		return "desync", who, string.format("town buildings (%d %s here)", math.abs(d), d > 0 and "more" or "fewer")
	end
	local m, whoV = v:match("^DESYNC vpos (%d+)m vs (%a+)")
	if m then return "desync", whoV, "vehicle positions (up to " .. m .. " m apart)" end
	local lanes, whoL = v:match("^DESYNC ([%a%+%?]+) vs (%a+)")
	if not lanes then return "desync", nil, nil end
	local set = {}
	for l in lanes:gmatch("[^+]+") do set[l] = true end
	local main, side = {}, {}
	for _, k in ipairs(ORDER) do
		if set[k] then
			if SIDE[k] then side[#side + 1] = LANES[k] else main[#main + 1] = LANES[k] end
		end
	end
	if #main == 0 and #side == 0 then return "desync", whoL, nil end
	local what = table.concat(#main > 0 and main or side, ", ")
	if #main > 0 and #side > 0 then what = what .. " (also " .. table.concat(side, ", ") .. ")" end
	return "desync", whoL, what
end

-- A peer's skew as the dash writes it: OUR clock minus theirs, in game time.
-- Within 1.5 is the heartbeat's own rounding (t= is a whole unit).
function CM.clockWords(sk)
	sk = tonumber(sk)
	if not sk then return "-" end
	if math.abs(sk) < 1.5 then return "in step" end
	return string.format("%.0f %s", math.abs(sk), sk > 0 and "behind you" or "ahead of you")
end

-- Our own clock: the pacing line ("0.95x e=-2.40", e in sim steps from the
-- leader). The leader IS the clock, so on the host's own screen it is in step.
function CM.paceWords(pace, isLeader)
	if isLeader then return "in step" end
	local mult, e = tostring(pace or ""):match("^([%d%.]+)x e=([%+%-][%d%.]+)")
	e = tonumber(e)
	if mult and e then
		if e < -0.3 then return string.format("catching up (%.1f steps behind)", -e) end
		if e > 0.3 then return string.format("easing off (%.1f steps ahead)", e) end
	end
	return "in step"
end

-- The host's row on a joiner's screen: how far the host is from YOU, in sim
-- steps, from our pacing error (e < 0: we are behind, so the host is ahead).
-- The heartbeat skew only has whole game-time units, so it read "in step" for
-- almost any gap; it stays the fallback while pacing has no reading yet.
function CM.hostDiffWords(pace, skew)
	local e = tonumber(tostring(pace or ""):match("e=([%+%-][%d%.]+)"))
	if not e then return CM.clockWords(skew) end
	if math.abs(e) <= 0.3 then return "in step" end
	return string.format("%.1f steps %s", math.abs(e), e < 0 and "ahead of you" or "behind you")
end

-- the session's leader letter from the bridge ctl (a relay lobby names it)
function CM.guiLeader()
	local f = io.open((K.BASE or "") .. "tpf2_bridge_ctl.txt", "r")
	if not f then return "a" end
	local body = f:read("*a") or ""
	f:close()
	return body:match("leader=(%a+)") or "a"
end

-- The notes cell of our own row: only what is worth knowing right now.
function CM.ownNotes(kv)
	local notes = {}
	local sp = tonumber(kv.speed)
	if kv.paused == "yes" or sp == 0 then notes[#notes + 1] = "paused"
	elseif sp then notes[#notes + 1] = string.format("speed %gx", sp) end
	local q = tonumber(kv.queued) or 0
	if q > 0 then notes[#notes + 1] = q .. " command(s) waiting" end
	local late = tonumber(kv.late) or 0
	if late > 0 then notes[#notes + 1] = late .. " command(s) arrived late" end
	local rec = tonumber(tostring(kv.nack or ""):match("recovered=(%d+)")) or 0
	if rec > 0 then notes[#notes + 1] = rec .. " lost command(s) recovered" end
	local worst = 0
	for mx in tostring(kv.vdrift or ""):gmatch("/([%d%.]+)m") do
		local x = tonumber(mx) or 0
		if x > worst then worst = x end
	end
	if worst >= 5 then notes[#notes + 1] = string.format("vehicles up to %.0f m apart", worst) end
	return table.concat(notes, ", ")
end

-- The status line. kv: our dash values; npeers: players heard right now.
-- CM.guiFirstDesync remembers the first desync this GUI state saw.
function CM.statusWords(kv, npeers)
	if kv.resync == "1" then
		return "Resync: " .. tostring(kv.resyncstatus or "waiting")
			.. ". Saving, transfer, reload and comparison run automatically. Closing the window does not resume play."
	end
	local state, who, what = CM.verdictWords(kv.verdict)
	local desyncs = tonumber(kv.desyncs) or 0
	local t = tonumber(kv.t)
	if state == "desync" and not CM.guiFirstDesync then
		CM.guiFirstDesync = { t = t, clock = os.date("%H:%M"), who = who, what = what }
	end
	local fd = CM.guiFirstDesync
	if npeers == 0 then
		return "No other player heard right now -- nothing to compare."
	end
	if state == "off" then
		return "Desync check OFF: this map is larger than vanilla allows, and hashing a world this size freezes the game for seconds at a time."
	end
	if state == "desync" or desyncs > 0 then
		local lines = {}
		if state == "desync" then
			lines[#lines + 1] = "DESYNC" .. (who and (" with " .. string.upper(who)) or "") .. ": the worlds no longer match."
			lines[#lines + 1] = "Different now: " .. (what or "something the check cannot name (see the game log)") .. "."
		else
			lines[#lines + 1] = "DESYNC earlier: the last check matched, but the worlds may still differ."
		end
		if fd then
			lines[#lines + 1] = string.format("First noticed at game time %s (%s)%s.", tostring(fd.t or "?"), fd.clock,
				fd.what and (": " .. fd.what) or "")
		end
		lines[#lines + 1] = "Use Resync now in the Multiplayer Resync panel to restore the host world."
		return table.concat(lines, NL)
	end
	if state == "sync" then
		return "IN SYNC: the worlds match" .. (t and string.format(" (checked at game time %d)", t) or "") .. "."
	end
	return "Checking: the first comparison runs about a minute into the game."
end

-- Fill the section. cells[letter] = { name, sync, clock, notes } TextViews.
function CM.statsInWords(status, cells, own, present, fresh, peerInfo)
	local kv = fresh[own] or {}
	local leader = CM.guiLeader()
	local npeers = 0
	for _ in pairs(peerInfo) do npeers = npeers + 1 end
	status:setText(CM.statusWords(kv, npeers))
	local ownState = CM.verdictWords(kv.verdict)
	local desyncs = tonumber(kv.desyncs) or 0
	for _, letter in ipairs(present) do
		local c = cells[letter]
		if c then
			local name = string.upper(letter)
			if letter == own then name = name .. " (you)" end
			if letter == leader then name = name .. " host" end
			c.name:setText(name .. "   ")
			if letter == own then
				local s = "checking"
				if ownState == "desync" then s = "differ"
				elseif ownState == "sync" then s = desyncs > 0 and "matched last check" or "match"
				elseif ownState == "off" then s = "not checked" end
				c.sync:setText(s .. "   ")
				c.clock:setText(CM.paceWords(kv.pace, letter == leader) .. "   ")
				c.notes:setText(CM.ownNotes(kv))
			else
				local info = peerInfo[letter]
				local s = "not heard"
				if info then
					local st = CM.verdictWords(info.verdict)
					s = (st == "sync" and "match") or (st == "desync" and "differ")
						or (ownState == "off" and "not checked") or "checking"
				end
				c.sync:setText(s .. "   ")
				local clock = "-"
				if info then
					clock = (letter == leader) and CM.hostDiffWords(kv.pace, info.skew) or CM.clockWords(info.skew)
				end
				c.clock:setText(clock .. "   ")
				c.notes:setText("")
			end
		end
	end
end
end
