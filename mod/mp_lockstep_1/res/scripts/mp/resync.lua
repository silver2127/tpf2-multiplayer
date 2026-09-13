-- One-click recovery. The lobby owns the barrier and native save/load; this
-- module holds all Lua producers and fingerprints the freshly loaded world.
return function(CM, K, log)
local guiBoot = os.time()
CM.resyncToken = tostring(os.time()) .. tostring({}):gsub("[^%w]", "")
local function fresh(wall)
	local age = wall and os.time() - wall
	return age and age >= 0 and age <= 5
end
function CM.syncRead(name)
	local f = io.open(K.BASE .. name, "rb")
	if not f then return nil end
	local text = f:read(16384) or ""; f:close()
	if text:sub(-1) ~= "\n" then return nil end
	local kv = {}
	for key, value in text:gmatch("([%w_]+)=([^\r\n]*)[\r\n]") do kv[key] = value end
	return kv
end

function CM.syncRequest(kind, operation)
	if not K.PROCESS_ID then pcall(CM.detectInstance) end
	local available = CM.syncRead("tpf2_sync_available.txt")
	if not available or available.protocol ~= "4" or available.pid ~= K.PROCESS_ID or not tonumber(available.wall)
		or math.abs(os.time()-tonumber(available.wall)) >= 5 then return false end
	CM.syncRequestNumber = (CM.syncRequestNumber or 0) + 1
	local f = io.open(K.BASE .. "tpf2_sync_request.txt", "wb")
	if not f then return false end
	local ok = f:write("pid=" .. available.pid .. "\ncmd=" .. kind .. "\nid=" ..
		CM.resyncToken .. tostring(CM.syncRequestNumber) .. (operation and ("\noperation=" .. operation) or "") .. "\n")
	local closed = f:close()
	return ok and closed
end

function CM.recoveryGuiHeld()
	if not K.PROCESS_ID then pcall(CM.detectInstance) end
	local state = CM.syncRead("tpf2_sync_lua.txt")
	if state and state.pid == K.PROCESS_ID and state.operation and state.epoch
		and #state.operation == 32 and not state.operation:find("[^0-9a-f]")
		and #state.epoch == 32 and not state.epoch:find("[^0-9a-f]")
		and tonumber(state.revision) and tonumber(state.revision) > (CM.recoveryGuiRevision or 0)
		and ({holding=true, waiting=true, saving=true, transferring=true, loading=true,
			checking=true, releasing=true, complete=true, error=true, aborted=true})[state.phase] then
		CM.recoveryGuiRevision = tonumber(state.revision)
		CM.recoveryGuiLatest = state
		CM.recoveryGuiHold = state.phase ~= "complete"
	end
	return CM.recoveryGuiHold
end

function CM.autoSyncPump(now)
	local incoming = CM.syncRead("tpf2_sync_lua.txt")
	if incoming and incoming.pid == K.PROCESS_ID and incoming.operation and incoming.epoch
		and #incoming.operation == 32 and not incoming.operation:find("[^0-9a-f]")
		and #incoming.epoch == 32 and not incoming.epoch:find("[^0-9a-f]")
		and tonumber(incoming.revision) and tonumber(incoming.revision) >= 1
		and ({holding=true, waiting=true, saving=true, transferring=true, loading=true,
			checking=true, releasing=true, complete=true, error=true, aborted=true})[incoming.phase] then
		local old = CM.autoSync
		if (old or incoming.phase ~= "complete") and (not old or tonumber(incoming.revision) > tonumber(old.revision)) then
			CM.autoSync = incoming
			if not old or old.operation ~= incoming.operation then
				local speed; pcall(function() speed = game.interface.getGameSpeed() end)
				CM.autoResumeSpeed = speed or CM.baseSpeed or 0
				CM.setSpeed(0, "automatic world operation")
			end
			CM.autoFingerprint = nil
		end
	end
	local state = CM.autoSync
	if not state then return false end
	if state.phase == "complete" then
		if not CM.autoReleased or CM.autoReleased ~= state.epoch then
			local speed = tonumber(state.resume_speed)
			if speed ~= 0 and speed ~= 1 and speed ~= 2 and speed ~= 4 then return true end
			CM.autoReleased = state.epoch
			CM.recoveryReleasePacing(speed)
			CM.lgHolding, CM.resyncHold = false, false
			CM.baseSpeed = speed
			CM.setSpeed(speed, "all players verified the new world")
		end
		return false
	end
	-- Missing/partial control or an error NEVER releases an existing hold.
	CM.resyncHold = true
	local speed; pcall(function() speed = game.interface.getGameSpeed() end)
	if state.phase == "checking" and speed == 0 and not CM.autoFingerprint then
		local ok, hash, detail = pcall(CM.recoveryWorldHash, now)
		if ok and hash and detail and not detail:find("invalid", 1, true) then
			-- Deliberately bypass normal stamp deduplication and the running-only
			-- hash cadence: this is a fresh comparison of the paused loaded world.
			CM.autoFingerprint = hash .. ":" .. detail
		end
	end
	local f = io.open(K.BASE .. "tpf2_sync_lua_ack.txt", "wb")
	if f then
		f:write("pid=" .. tostring(K.PROCESS_ID) .. "\noperation=" .. state.operation ..
			"\nepoch=" .. state.epoch .. "\nrevision=" .. state.revision .. "\nphase=" .. state.phase ..
			"\nheld=1\npaused=" .. (speed == 0 and "1" or "0") ..
			"\nspeed=" .. tostring(CM.autoResumeSpeed or 0) ..
			"\nworld=" .. CM.resyncToken ..
			"\nfingerprint=" .. (CM.autoFingerprint or "") .. "\n")
		f:close()
	end
	if K.INSTANCE then
		local text = "boot=" .. tostring(CM.bootWall or 0) .. "\nwall=" .. os.time()
			.. "\nresynctoken=" .. CM.resyncToken .. "\nresync=1\nresyncstatus=" .. state.phase
			.. "\nverdict=RESYNC - " .. state.phase .. "\npaused=" .. (speed == 0 and "yes" or "no") .. "\n"
		for _, prefix in ipairs({"lockstep_dash_", "lockstep_status_"}) do
			local out = io.open(K.BASE .. prefix .. K.INSTANCE .. ".txt", "wb")
			if out then out:write(text); out:close() end
		end
	end
	return true
end

local function validDash(kv)
	return type(kv) == "table" and type(kv.resynctoken) == "string"
		and kv.resynctoken:match("^%w+$") and #kv.resynctoken <= 128
		and tonumber(kv.boot) and tonumber(kv.boot) >= guiBoot - 60 and fresh(tonumber(kv.wall))
end

-- ---- the Resync section of the Multiplayer window (GUI Lua state) ----
-- One section inside the existing window instead of a separate Lua popup
-- (2026-09-14). It appears when a desync is counted or a recovery is active
-- and follows the phase written by the lobby into tpf2_sync_lua.txt (the same
-- control file the engine state reads, so it survives the world reload). Its
-- only action is Resync now: while the world is held, the native input gate
-- swallows every click on game widgets (native_io.cpp), so Retry and Cancel
-- live in the native overlay's panel, which reads the mouse through its own
-- low-level hook (user test 2026-09-14: the Lua buttons could not be pressed).
local PHASE_TEXT = {
	holding = "Pausing both games", waiting = "Desync detected. Both games are paused.",
	saving = "Saving the host world", transferring = "Transferring the save", loading = "Loading the save",
	checking = "Checking that both worlds match", releasing = "Checking that both worlds match",
	complete = "All players are in sync.", aborted = "Resync cancelled. Both games remain paused.",
}
local FAILED_TEXT = {
	holding = "Could not pause both games", saving = "Could not save the host world",
	transferring = "Save transfer failed", loading = "Could not load the save",
	checking = "World comparison failed", releasing = "World comparison failed",
}
local NL = string.char(10)

local function sectionText(kv, state)
	if kv.resync ~= "1" then
		return "The game worlds are out of sync." .. NL
			.. "Resync now pauses both games, saves the host world, transfers the save and reloads it for both players." .. NL
			.. "Play resumes automatically once both worlds match. The host world is used; client-only changes will be lost."
	end
	local phase = state and state.phase or kv.resyncstatus or "holding"
	local line = PHASE_TEXT[phase] or ("Resync: " .. tostring(phase))
	if phase == "error" then line = FAILED_TEXT[state and state.step or ""] or "Resync stopped" end
	local detail = state and state.detail or ""
	return "Resync: " .. line .. (detail ~= "" and (NL .. detail) or "") .. NL
		.. "The host world is used. Client-only changes will be lost." .. NL
		.. "Retry and Cancel are in the Multiplayer Resync panel; this window cannot be clicked while the game is held."
end

local function button(label, fn)
	local b = api.gui.comp.Button.new(api.gui.comp.TextView.new(label), true)
	b:onClick(fn)
	return b
end

local function act()
	local kv = CM.resyncGuiDash
	if not validDash(kv) or kv.resync == "1" then return end
	if not CM.syncRequest("sync_request") then
		if CM.resyncText then
			CM.resyncText:setText("Automatic resync is unavailable. Both players must use the same version in a player-hosted lobby.")
		end
		return
	end
	CM.resyncRequested = { token = kv.resynctoken, at = os.time() }
	if CM.resyncText then CM.resyncText:setText("Resync requested. Waiting for confirmation; do not build anything.") end
	CM.resyncSetButtons(false)
end

-- nil hides the button, false shows it disabled, true shows it enabled
function CM.resyncSetButtons(request)
	pcall(function()
		CM.resyncButton:setVisible(request ~= nil, false); CM.resyncButton:setEnabled(request == true)
	end)
end

-- Builds the section; the dashboard adds the returned component to its layout.
function CM.resyncSection()
	local box = api.gui.layout.BoxLayout.new("VERTICAL")
	CM.resyncText = api.gui.comp.TextView.new("")
	box:addItem(CM.resyncText)
	CM.resyncButton = button("  Resync now  ", act)
	box:addItem(CM.resyncButton)
	CM.resyncBox = api.gui.comp.Component.new("mpResync")
	CM.resyncBox:setLayout(box)
	CM.resyncBox:setVisible(false, false)
	CM.resyncActive = false
	return CM.resyncBox
end

-- Called twice a second with the own dash file. Returns true while the section
-- is showing, so the dashboard shows the window even when Ctrl+Shift+D hid it.
function CM.resyncGuiTick(kv)
	CM.resyncGuiDash = kv
	pcall(CM.recoveryGuiHeld)
	local state = CM.recoveryGuiLatest
	if not validDash(kv) then
		if CM.resyncBox and CM.resyncActive then
			CM.resyncText:setText("Waiting for current game status. Do not build anything.")
			CM.resyncSetButtons(false)
		end
		return CM.resyncActive == true
	end
	if CM.resyncGuiToken ~= kv.resynctoken then
		CM.resyncGuiToken, CM.resyncRequested = kv.resynctoken, nil
	end
	local held = kv.resync == "1"
	local active = held or (tonumber(kv.desyncs) or 0) > 0
	local req = CM.resyncRequested
	if req and (held or req.token ~= kv.resynctoken or os.time() - req.at > 5) then
		CM.resyncRequested, req = nil, nil
	end
	if CM.resyncBox then
		if active then
			CM.resyncText:setText(sectionText(kv, held and state or nil))
			if held then CM.resyncSetButtons(nil) else CM.resyncSetButtons(req == nil) end
		end
		if CM.resyncActive ~= active then pcall(function() CM.resyncBox:setVisible(active, false) end) end
	end
	CM.resyncActive = active
	return active
end
end
