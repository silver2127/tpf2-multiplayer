-- mp/desyncreport.lua -- the desync popup: send this game's logs to the developers?
--
-- Added 2026-09-11. Loaded from the game script as
--     require("mp.desyncreport")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
--
-- GUI STATE ONLY. guiUpdate reads this game's dash file (written by the script
-- state) and calls CM.desyncReportTick with it. The first desync of a loaded
-- game then follows the player's choice, kept per computer in
-- <data dir>\tpf2mp_prefs.txt as desync_logs=ask|always|never:
--   ask     a window offers Always send / Only this once / Never
--   always  the logs go without asking (one chat line says so)
--   never   nothing, and no window ever again
-- "/desynclogs always|ask|never" in the in-game chat changes it; plain
-- "/desynclogs" says what it is set to.
--
-- Sending is the lobby's job (netpunch/desynclogs.py): this appends
-- {"cmd":"upload_logs",...} to lobby_in.jsonl, and the lobby gathers, scrubs,
-- zips and uploads the logs, then reports back as a chat line.
return function(CM, K, log)
local PREF_KEY = "desync_logs"
local NL = string.char(10)

local function prefsPath() return (K.BASE or "") .. "tpf2mp_prefs.txt" end

function CM.prefRead(key)
	local f = io.open(prefsPath(), "r")
	if not f then return nil end
	local v
	for line in f:lines() do
		local k, x = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
		if k == key then v = x end
	end
	f:close()
	return v
end

function CM.prefWrite(key, value)
	local lines, found = {}, false
	local f = io.open(prefsPath(), "r")
	if f then
		for line in f:lines() do
			if line:match("^%s*([%w_]+)%s*=") == key then
				if not found then lines[#lines + 1] = key .. "=" .. value; found = true end
			elseif line ~= "" then
				lines[#lines + 1] = line
			end
		end
		f:close()
	end
	if not found then lines[#lines + 1] = key .. "=" .. value end
	local w = io.open(prefsPath(), "w")
	if not w then return false end
	w:write(table.concat(lines, NL) .. NL)
	w:close()
	return true
end

function CM.desyncLogsPref()
	local v = CM.prefRead(PREF_KEY)
	if v == "always" or v == "never" then return v end
	return "ask"
end

local function jsonStr(s)
	local body = tostring(s):gsub('[%c"\\]', function(c)
		if c == '"' then return '\\"' elseif c == "\\" then return "\\\\" end
		return string.format("\\u%04x", c:byte())
	end)
	return '"' .. body .. '"'
end

-- fields: { {key, value}, ... } in order; numbers are written as integers
function CM.lobbyCommand(fields)
	local d = CM.netDir and CM.netDir()
	if not d then return false end
	local parts = {}
	for _, kv in ipairs(fields) do
		local v = kv[2]
		parts[#parts + 1] = jsonStr(kv[1]) .. ":" .. (type(v) == "number" and string.format("%d", math.floor(v)) or jsonStr(v))
	end
	local f = io.open(d .. "/lobby_in.jsonl", "a")
	if not f then return false end
	f:write("{" .. table.concat(parts, ",") .. "}" .. NL)
	f:close()
	return true
end

-- a chat line only this player sees (the lobby echoes it as MULTIPLAYER)
function CM.desyncNote(text)
	return CM.lobbyCommand({ { "cmd", "note" }, { "text", text } })
end

function CM.desyncSendLogs(info)
	local ok = CM.lobbyCommand({ { "cmd", "upload_logs" }, { "instance", K.INSTANCE or "?" },
		{ "reason", info.why or "?" }, { "t", info.t or 0 }, { "desyncs", info.n or 0 } })
	print("[ls-gui] desync logs: " .. (ok and "asked the lobby to send them" or "no lobby is running -- not sent"))
	return ok
end

function CM.desyncPopup(info)
	local ok, err = pcall(function()
		local win
		local function close()
			pcall(function() win:setVisible(false, false) end)
			CM.desyncWin = nil
		end
		local function button(label, fn)
			local b = api.gui.comp.Button.new(api.gui.comp.TextView.new(label), true)
			b:onClick(function()
				local okC, errC = pcall(fn)
				if not okC then print("[ls-gui] desync popup: " .. tostring(errC)) end
				close()
			end)
			return b
		end
		local box = api.gui.layout.BoxLayout.new("VERTICAL")
		box:addItem(api.gui.comp.TextView.new(table.concat({
			"The players' games no longer match (" .. tostring(info.why) .. ").",
			"",
			"Send this game's logs to the Transport Fever 2 Multiplayer developers so they can find the cause?",
			"They are this session's game and mod logs. Windows user names and IP addresses are removed first;",
			"player names and chat can remain.",
			"",
			"Change this later by typing /desynclogs always, ask or never in the chat.",
		}, NL)))
		local row = api.gui.layout.BoxLayout.new("HORIZONTAL")
		row:addItem(button("  Always send  ", function()
			CM.prefWrite(PREF_KEY, "always")
			CM.desyncSendLogs(info)
		end))
		row:addItem(button("  Only this once  ", function() CM.desyncSendLogs(info) end))
		row:addItem(button("  Never  ", function()
			CM.prefWrite(PREF_KEY, "never")
			CM.desyncNote("You won't be asked about desync logs again. Type /desynclogs ask in the chat to undo that.")
		end))
		local rowC = api.gui.comp.Component.new("mpDesyncButtons")
		rowC:setLayout(row)
		box:addItem(rowC)
		local body = api.gui.comp.Component.new("mpDesyncPopup")
		body:setLayout(box)
		win = api.gui.comp.Window.new("Desync detected", body)
		pcall(function() win:setPosition(460, 260) end)
		CM.desyncWin = win
	end)
	if not ok then print("[ls-gui] desync popup could not be shown: " .. tostring(err)) end
end

-- the GUI state's first look at this game: a dash file older than that belongs
-- to an earlier game and says nothing about this one
CM.desyncGuiBoot = CM.desyncGuiBoot or os.time()

function CM.desyncReportTick(kv)
	if CM.desyncHandled or type(kv) ~= "table" then return end
	local n, boot = tonumber(kv.desyncs), tonumber(kv.boot)
	if not n or n <= 0 then return end
	if not boot or boot < CM.desyncGuiBoot - 60 then return end
	CM.desyncHandled = true
	local info = { why = kv.desyncwhy or kv.verdict or "?", t = tonumber(kv.desynct) or tonumber(kv.t) or 0, n = n }
	local pref = CM.desyncLogsPref()
	print(string.format("[ls-gui] desync detected (%s at t=%d), desync logs set to '%s'", tostring(info.why), info.t, pref))
	if pref == "never" then return end
	if pref == "always" then
		if CM.desyncSendLogs(info) then
			CM.desyncNote("Desync detected: sending this game's logs to the developers (type /desynclogs ask to be asked instead).")
		end
		return
	end
	CM.desyncPopup(info)
end

-- "/desynclogs [always|ask|never]" typed in the chat; true when it was one
function CM.desyncLogsCommand(text)
	local word, arg = tostring(text):match("^%s*(/%S+)%s*(.-)%s*$")
	if not word or word:lower() ~= "/desynclogs" then return false end
	arg = arg:lower()
	local says = { always = "sent automatically after a desync", never = "never sent, and you won't be asked",
		ask = "you'll be asked after a desync" }
	if says[arg] then
		CM.prefWrite(PREF_KEY, arg)
		CM.desyncNote("Desync logs: " .. says[arg] .. ".")
	else
		local cur = CM.desyncLogsPref()
		CM.desyncNote("Desync logs are set to " .. cur .. " (" .. says[cur] .. "). Type /desynclogs always, ask or never.")
	end
	return true
end
end
