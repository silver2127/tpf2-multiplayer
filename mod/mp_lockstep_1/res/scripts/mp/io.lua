-- mp/io.lua -- runtime files: append/read, instance detection, wire broadcast
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.io")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- io ----------
-- The game's Lua has no os.remove. Its os table is cut down, and the unguarded
-- call in CM.syncTick raised "attempt to call field 'remove' (a nil value)" on a
-- player's host the moment a hot-join save was shared (0.4.11, build 35924,
-- 2026-09-10). Every pcall'd os.remove had been failing silently: stale dash and
-- status files from days before, and a fractional speed that was never cleared.
-- A file this mod wants gone is EMPTIED instead, and every reader treats an empty
-- file as absent: the bridge's tpf2_speed.txt ("absent, empty or 0 = off"), the
-- sync marker, the dash and status files. A missing file is not created.
function CM.clearFile(path)
	local f = io.open(path, "r")
	if not f then return end
	f:close()
	if os and os.remove then
		local ok, removed = pcall(os.remove, path)
		if ok and removed then return end
	end
	local w = io.open(path, "w")
	if w then w:close() end
end

local function appendLine(path, line)
	local f = io.open(path, "a")
	if not f then return false end
	f:write(line .. "\n")
	f:close()
	return true
end

function CM.readFrom(path, offset)
	local f = io.open(path, "r")
	if not f then return nil, offset end
	local size = f:seek("end")
	if offset < 0 or offset > size then f:seek("set", size); f:close(); return nil, size end
	if offset == size then f:close(); return nil, offset end
	f:seek("set", offset)
	local data = f:read("*a") or ""
	f:close()
	-- Never consume a partial line. The hook writes a ROADC/ROADE line as many
	-- separate fprintfs on a shared handle; polling mid-write used to swallow
	-- the fragment (the length guard rejected it) and the command was silently
	-- LOST. Trim to the last newline and re-read the remainder next poll.
	local tail = data:match("[^\n]*$")
	if #tail > 0 then
		if #tail == #data then return nil, offset end
		data = data:sub(1, #data - #tail)
	end
	return data, offset + #data
end

function CM.detectInstance()
	local f = io.open(K.IDENTITY_FILE, "r")
	if not f then return false end
	local s = f:read("*l")
	f:close()
	if not s or #s == 0 then return false end
	local inst = s:gsub("%s", "")
	if inst == K.INSTANCE then return true end
	K.INSTANCE = inst
	K.PEER = (inst == "a") and "b" or "a"
	K.CAPTURE_FILE = K.BASE .. "tpf2_capture_" .. K.INSTANCE .. ".txt"
	K.EVENTS_FILE  = K.BASE .. "tpf2_events_" .. K.INSTANCE .. ".txt"
	K.INJECT_FILE  = K.BASE .. "lockstep_inject_" .. K.INSTANCE .. ".txt"
	-- events: -1 means "seek to end", which is right -- peer traffic from before
	-- we loaded is stale and replaying it would apply commands whose stamps have
	-- long passed.
	CM.eventsOffset = -1
	-- inject: prime to the file's CURRENT size instead. -1 here loses the first
	-- command every time: while the file does not exist the offset stays -1, and
	-- the poll that finally opens it seeks straight to the end -- past the line
	-- it was supposed to read. Same shape as the bug mpbridge records for its
	-- events file, where the joiner primed to end-of-file and reported
	-- consumed=0 while holding every line.
	CM.injectOffset = 0
	local f2 = io.open(K.INJECT_FILE, "r")
	if f2 then CM.injectOffset = f2:seek("end"); f2:close() end
	-- A status file for the letter we are NOT is last session's, and it looks
	-- alive: "desyncs=832" from a previous run was read as this session's count
	-- (2026-08-31, after the two instances swapped letters on restart). Remove it
	-- so only one status file exists and it is always the live one.
	for letter in ("abcdefgh"):gmatch(".") do
		if letter ~= inst then
			pcall(CM.clearFile, K.BASE .. "lockstep_status_" .. letter .. ".txt")
			pcall(CM.clearFile, K.BASE .. "lockstep_dash_" .. letter .. ".txt")
		end
	end
	log("identity " .. K.INSTANCE .. " (peer " .. K.PEER .. ")")
	-- WALL CLOCK AT SCRIPT START. The game times a few of its own phases
	-- (ModelRep, shader reload) and those add up to a couple of seconds, which
	-- is nowhere near how long a 600 MB save actually takes to come up -- most
	-- of the load is untimed and therefore invisible. This is the only anchor
	-- the game script can give: subtract the PROCESS start time from it and the
	-- difference is the whole load, timed phases and untimed alike.
	--   powershell: Get-Process TransportFever2 | Select Id,StartTime
	pcall(function()
		log(string.format("BOOT: game script live at %s (subtract the process start time for the true load duration)",
			os.date("%H:%M:%S")))
	end)
	if not CM.baseLogged then
		-- once, and on disk: stdout is buffered until exit, cmLog is not
		CM.baseLogged = true
		local out = CM.cmLog or log
		out("  base " .. K.BASE .. "  [" .. tostring(CM.baseSource) .. "]")
	end
	log("  send -> " .. K.CAPTURE_FILE)
	log("  recv <- " .. K.EVENTS_FILE)
	log("  inject <- " .. K.INJECT_FILE)
	return true
end

-- ---------- wire ----------
function CM.broadcast(line)
	if K.CAPTURE_FILE then appendLine(K.CAPTURE_FILE, line) end
end

function CM.stepOf(t) return math.floor((t or 0) / K.SIM_STEP + 0.5) end
end
