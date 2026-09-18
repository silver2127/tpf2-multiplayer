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

-- The lobby that runs a world operation refreshes tpf2_sync_available.txt (wall=)
-- every second or two while it lives. Read every 15 ticks.
function CM.syncLobbyAlive()
	if CM.syncLobbyAt and (CM.ticks or 0) - CM.syncLobbyAt < 15 then return CM.syncLobbyAliveCached end
	CM.syncLobbyAt = CM.ticks or 0
	local a = CM.syncRead("tpf2_sync_available.txt")
	local wall = a and tonumber(a.wall)
	CM.syncLobbyAliveCached = wall ~= nil and math.abs(os.time() - wall) <= (K.SOLO_RELEASE_SECONDS or 15)
	return CM.syncLobbyAliveCached
end

-- ALONE, for a hold: the lobby that drives the operation is gone, or its
-- roster is KNOWN to be one player. Never "no peer heard": right after the
-- load every member is deaf for a while (fresh Lua state, peers still loading),
-- and the first frozen join with three players abandoned its hold on every
-- machine 15 s after the load for exactly that reason (2026-09-17 21:51):
-- each unpaused itself, the pause fence could not drain, no fingerprint was
-- taken and the round timed out in "checking".
function CM.syncAlone()
	local roster = tonumber(CM.rosterPlayers)
	if roster ~= nil and roster <= 1 then return true end
	return not CM.syncLobbyAlive()
end

function CM.autoSyncPump(now)
	local incoming = CM.syncRead("tpf2_sync_lua.txt")
	if incoming and incoming.pid == K.PROCESS_ID and incoming.operation and incoming.epoch
		and #incoming.operation == 32 and not incoming.operation:find("[^0-9a-f]")
		and #incoming.epoch == 32 and not incoming.epoch:find("[^0-9a-f]")
		and tonumber(incoming.revision) and tonumber(incoming.revision) >= 1
		and ({holding=true, waiting=true, saving=true, transferring=true, loading=true,
			checking=true, releasing=true, complete=true, error=true, aborted=true})[incoming.phase]
		and incoming.operation ~= CM.autoAbandoned then     -- an operation abandoned alone (below) stays abandoned
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
			if speed ~= 0 and speed ~= 1 and speed ~= 2 and speed ~= 3 and speed ~= 4 then return true end
			CM.autoReleased = state.epoch
			CM.recoveryReleasePacing(speed)
			CM.lgHolding, CM.resyncHold = false, false
			CM.baseSpeed = speed
			CM.setSpeed(speed, "all players verified the new world")
		end
		return false
	end
	-- Missing/partial control or an error NEVER releases an existing hold --
	-- while there is a session to protect. ALONE (2026-09-17, user: "with only
	-- 1 person in game I cannot increase game speed"): the lobby gone, or its
	-- roster known to be one, for K.SOLO_RELEASE_TICKS means nobody will ever
	-- advance this operation; the hold would pin the speed at 0 for the rest of
	-- the game. Abandon it and give the lever back. (CM.syncAlone: not by
	-- unheard peers -- see there.)
	if CM.syncAlone() then
		CM.autoAloneSince = CM.autoAloneSince or (CM.ticks or 0)
		if (CM.ticks or 0) - CM.autoAloneSince >= (K.SOLO_RELEASE_TICKS or 75) then
			local speed = tonumber(CM.autoResumeSpeed) or 0
			if speed < 1 or speed > 4 then speed = 1 end
			log(string.format("RESYNC: %s left in phase %s with nobody else in the game -- abandoned, speed back to %d",
				tostring(state.operation), tostring(state.phase), speed))
			CM.autoAbandoned = state.operation
			CM.autoSync, CM.autoFingerprint, CM.autoAloneSince = nil, nil, nil
			CM.recoveryReleasePacing(speed)
			CM.lgHolding, CM.resyncHold = false, false
			CM.baseSpeed = speed
			CM.setSpeed(speed, "the other players are gone; the world operation is abandoned")
			return false
		end
	else
		CM.autoAloneSince = nil
	end
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

-- Publish fresh GUI observations for the native panel. This is only a notice:
-- displaying a desync never starts a recovery or acquires an input hold.
function CM.resyncGuiTick(kv)
	if not validDash(kv) then return end
	if not K.PROCESS_ID then pcall(CM.detectInstance) end
	if not K.PROCESS_ID then return end
	local count = math.max(0, math.floor(tonumber(kv.desyncs) or 0))
	local f = io.open(K.BASE .. "tpf2_sync_notice.txt", "wb")
	if f then
		f:write("pid=" .. K.PROCESS_ID .. "\nworld=" .. kv.resynctoken ..
			"\nwall=" .. os.time() .. "\ndesyncs=" .. count ..
			"\nheld=" .. (kv.resync == "1" and "1" or "0") .. "\n")
		f:close()
	end
end
end
