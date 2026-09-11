-- mp/pacing.lua -- session speed, PID pacing, catch-up, load gate
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.pacing")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- load gate ----------
--
-- LOAD GATE. Ticks are ~5.4 Hz (300 ticks measured over 56 s), so these are
-- ~11 s and ~2.8 min.
-- How soon the gate may act. Long enough for game.interface to answer, short
-- enough that an instance is not simulating alone while the others load.
K.LOADGATE_MIN_TICKS = 5
K.LOADGATE_SETTLE    = 60    -- ticks with no NEW peer before the roster counts as complete
K.LOADGATE_MAX_TICKS = 900   -- absolute cap: a session must never hang forever
-- The manual override ("press play to start anyway") is honoured only after
-- this many ticks of holding (~60 s). A friends' 4-player night (2026-09-09)
-- started with the host pressing play at "2 of 3 in": the third player was
-- still loading, missed a station placed at t=7, and every hash from t=16 on
-- disagreed -- 618 desync reports, stops on roads the host did not have,
-- vehicles 100 m apart. Nothing replays history to a player who arrives after
-- a command's stamp, so an early start is a guaranteed fork. Before the window
-- a play press is put back to 0 and the log says how long until it counts.
K.LOADGATE_FORCE_TICKS = 320

-- ---------- session speed ----------
--
-- The engine's speeds are 0, 1, 2, 4 -- it reported 4 live (2026-08-31), so
-- the ladder is not consecutive. MAX_SPEED is the top of the lever.
CM.MAX_SPEED      = 4

-- A ceiling of 0 ("pause everyone") needs the 0 to PERSIST this long. An
-- autosave, the menu or a focus loss reads speed 0 for a tick or two; taken
-- at face value that made a 0 ceiling nothing could clear (b at t=261,
-- 2026-09-08: the host could not unpause the session from then on).
CM.SPD2_PAUSE_TICKS = 8

-- FRACTIONAL SPEEDS (2026-09-09). The engine's speed is a whole number of sim
-- iterations per frame; the bridge DLL's speedhook dithers that count per
-- frame to whatever number stands in DATADIR\tpf2_speed.txt (2.5 -> 2,3,2,3),
-- and the engine's own pause still wins. So a session speed of 2.5 is: lever
-- = round(2.5) so the UI shows something sane and the sim is not paused, plus
-- the file. A whole number clears the file and is the plain lever. The lever
-- is what getGameSpeed() reads back, so the "ours vs the player's" test keeps
-- comparing whole numbers.
-- A FRACTION NEVER MOVES THE LEVER (2026-09-09): the dither alone decides the
-- step count, so while the session speed is 2 and the PID asks for 1.7 or
-- 2.3 the lever stays at 2. Rounding the fraction to the nearest lever made
-- it flip between 1 and 2 as the PID crossed 1.5, and every flip plays the
-- game's speed-button click. The lever moves only when the session speed
-- itself changes (or for a pause / a whole-number target).
function CM.leverOf(v)
	v = tonumber(v) or 0
	if v == math.floor(v) then return v end
	local base = CM.effSpeed
	if base and base > 0 then
		if base ~= math.floor(base) then base = math.floor(base + 0.5) end
		return math.max(1, math.min(CM.MAX_SPEED or 4, base))
	end
	return math.max(1, math.min(CM.MAX_SPEED or 4, math.floor(v + 0.5)))
end
-- No dither target survives a load: the bridge deletes the file at game
-- start and this empties it again at script load (CM.clearFile: the game has no
-- os.remove), so a leftover fraction from
-- the last session cannot slow this one from its first frame.
pcall(CM.clearFile, K.BASE .. "tpf2_speed.txt")
CM.ditherCur = ""
K.DITHER_REASSERT_TICKS = 25   -- ~5 s at speed 1: the file is written again even when the target is unchanged
function CM.setDither(v)
	local want = (v and v ~= math.floor(v)) and string.format("%.4f", v) or ""
	-- REWRITTEN every K.DITHER_REASSERT_TICKS even when unchanged (2026-09-10). With
	-- the cache alone, one lost write -- and, before CM.clearFile, every clear --
	-- left the bridge applying a stale fraction for the rest of the session:
	-- joiners "caught up at 4x" at about 1 unit/s under a leftover 0.95.
	if CM.ditherCur == want and (CM.ticks or 0) - (CM.ditherAt or -1e9) < K.DITHER_REASSERT_TICKS then return end
	CM.ditherCur = want
	CM.ditherAt = CM.ticks or 0
	pcall(function()
		local p = K.BASE .. "tpf2_speed.txt"
		if want == "" then CM.clearFile(p) else local f = io.open(p, "w"); if f then f:write(want, "\n"); f:close() end end
	end)
end

function CM.setSpeed(v, why)
	v = tonumber(v) or 0
	CM.setDither(v)
	v = CM.leverOf(v)
	-- TWO SLOTS, not one. A single remembered value is enough only while at most
	-- one of our commands is in flight; the moment corrections come faster than
	-- the engine applies them, the engine reports the OLDER one, CM.paceV2
	-- fails to recognise it as ours, takes it for the PLAYER moving the lever,
	-- and ships our own change to every peer as their ceiling.
	CM.prevSetSpeed = CM.lastSetSpeed
	CM.lastSetSpeed = v
	CM.paceSetTick = CM.ticks
	CM.paceApplied = false
	pcall(function() api.cmd.sendCommand(api.cmd.make.setGameSpeed(v)) end)
	log(string.format("PACE: speed -> %s (%s)", tostring(v), why))
end

-- HOST UNPAUSE: the host's play press resumes the session at once, LSEFF
-- carrying the new speed without waiting for the next controller pass.
function CM.hostUnpause(s)
	local v = math.min(s, CM.MAX_SPEED or 4)
	if v < 1 then v = 1 end
	CM.effSpeed = v
	CM.broadcast(string.format("LSEFF v=%d", v))
	log(string.format("SPEED2: host unpaused the session at %d", v))
end

-- A speed button clicked on THIS game: SPEEDBTN <v> from the slice, which
-- cancelled the click, so no lever moved. The leader's click becomes the
-- session speed; a follower's is ignored, its lever following the session.
-- While the load gate holds, a play press is the player's override
-- (CM.ensureRunning reads CM.lgPress).
function CM.speedButton(v)
	v = tonumber(v)
	if not v then return end
	v = math.max(0, math.min(CM.MAX_SPEED or 4, math.floor(v)))
	if CM.lgHolding then
		CM.lgPress = v
		return
	end
	if not CM.peerSeen then
		-- nobody to pace with (the slice saw a session a moment ago): apply it here
		CM.setSpeed(v, "speed button, nobody else in the session")
		return
	end
	if not CM.isLeader() then
		log(string.format("SPEED2: speed button %d ignored -- the host's speed buttons set the session speed", v))
		return
	end
	local wasPaused = (CM.effSpeed == 0)
	CM.myCeiling = v
	CM.spd2ZeroSince = nil
	CM.btnAt, CM.ceilByButton = CM.ticks, true
	log(string.format("SPEED2: host speed button -> %d", v))
	if v > 0 and wasPaused then CM.hostUnpause(v) end
end

-- THE EDITOR'S CALENDAR (2026-09-11). The date picker and the date speed slider
-- only ever changed the clicking player's game. While a session is live the
-- slice cancels them and writes SETDATE <julian day> / CALSPEED <ms per day>;
-- inject.lua schedules that like any command, and every instance applies it
-- here at the stamp, so the calendar moves on the same sim step everywhere.
-- SetDate's value is boost::gregorian's day number, the Julian Day Number.
function CM.julianToYmd(jdn)
	local a = jdn + 32044
	local b = math.floor((4 * a + 3) / 146097)
	local c = a - math.floor(146097 * b / 4)
	local d = math.floor((4 * c + 3) / 1461)
	local e = c - math.floor(1461 * d / 4)
	local m = math.floor((5 * e + 2) / 153)
	local day = e - math.floor((153 * m + 2) / 5) + 1
	local month = m + 3 - 12 * math.floor(m / 10)
	local year = 100 * b + d - 4800 + math.floor(m / 10)
	return year, month, day
end

function CM.execCalendar(c)
	local t0
	pcall(function() t0 = game.interface.getGameTime().time end)
	if c.op == "CALSPEED" then
		local ms = tonumber(c.ms)
		-- 0 is the slider's stopped calendar (a multiplier of 0), a real setting
		if not ms or ms < 0 then
			log(string.format("EXEC CALSPEED seq=%s: bad value %s -- not applied", tostring(c.seq), tostring(c.ms)))
			return
		end
		local before, after
		pcall(function() before = game.interface.getMillisPerDay() end)
		local ok, err = pcall(function() game.interface.setMillisPerDay(ms) end)
		pcall(function() after = game.interface.getMillisPerDay() end)
		log(string.format("EXEC CALSPEED seq=%s origin=%s ms/day %s -> %s (asked %d) success=%s%s",
			tostring(c.seq), tostring(c.origin), tostring(before), tostring(after), ms, tostring(ok),
			ok and "" or (" err=" .. tostring(err))))
		return
	end
	local jdn = tonumber(c.jdn)
	if not jdn then
		log(string.format("EXEC SETDATE seq=%s: bad value %s -- not applied", tostring(c.seq), tostring(c.jdn)))
		return
	end
	local y, m, d = CM.julianToYmd(jdn)
	-- setDate takes the date in the order getDateFromNowPlusOffsetDays hands it
	-- back (the old mp_bridge fed one into the other); the year is the entry
	-- above 31, which tells day-first from year-first
	local cur
	pcall(function() cur = game.interface.getDateFromNowPlusOffsetDays(0) end)
	local args
	if type(cur) == "table" and tonumber(cur[1]) and tonumber(cur[3]) then
		if tonumber(cur[1]) > 31 then args = { y, m, d } elseif tonumber(cur[3]) > 31 then args = { d, m, y } end
	end
	if not args then
		local shape = {}
		pcall(function() for k, v in pairs(cur or {}) do shape[#shape + 1] = tostring(k) .. "=" .. tostring(v) end end)
		log(string.format("EXEC SETDATE seq=%s: cannot read today's date shape (%s: %s) -- %04d-%02d-%02d NOT applied",
			tostring(c.seq), type(cur), table.concat(shape, ","), y, m, d))
		return
	end
	local ok, err = pcall(function() game.interface.setDate((table.unpack or unpack)(args)) end)
	local t1
	pcall(function() t1 = game.interface.getGameTime().time end)
	log(string.format("EXEC SETDATE seq=%s origin=%s -> %04d-%02d-%02d (jdn %d) success=%s%s | game time %s -> %s",
		tostring(c.seq), tostring(c.origin), y, m, d, jdn, tostring(ok), ok and "" or (" err=" .. tostring(err)),
		tostring(t0), tostring(t1)))
end

-- SPEED V2 controller -- THE HOST'S SPEED BUTTONS (2026-09-10):
--   * the session speed is what the leader's player picks with the game's own
--     speed buttons. While a session is live the slice DLL cancels a click on
--     the clock's controls (the speed buttons and the pause toggle) and writes
--     SPEEDBTN <v> to the inject file, so a click moves no lever by itself:
--     CM.speedButton makes the leader's click the session speed, which LSEFF
--     carries to everyone, and ignores a follower's. A lever change the slice
--     did not cancel (a game without hooks, the menu setting the speed as it
--     switches to the game) still reaches the leader's detector in CM.paceV2.
--   * a PAUSE IS A SYNC POINT: when the session speed is 0 the leader stops
--     at once and everyone behind keeps running until they reach the
--     leader's clock, then stops there. So "pause to let people catch up"
--     does exactly that, and an unpause resumes everyone in step.
--   * nothing else moves the lever. The automatic corrections that were here
--     (leader micropause pulses, the hysteretic sustainable cap) throttled
--     and stuttered the session in ways players felt but could not see, and
--     are gone.
-- What remains automatic: the load gate, the catch-up of a game far behind,
-- and each joiner's PID trim toward the host's clock. Commands are stamped
-- past the fastest clock (net.lua), so a gap costs the slow player latency,
-- never a fork.
-- "/speed 2.5" typed in the lobby chat: the panel (menu DLL) writes speed=2.5
-- into tpf2_bridge_ctl.txt; the HOST reads it here as the session speed
-- request. Read every ~2 s, not per tick. "/speed off" (or 0) clears it. The
-- newer of the two wins: a speed button pressed after a /speed request
-- overrides it until the request next changes.
function CM.speedRequest()
	if CM.spdReqAt and CM.ticks - CM.spdReqAt < 10 then return CM.spdReq end
	CM.spdReqAt = CM.ticks
	local req, syncN, players
	pcall(function()
		local f = io.open(K.BASE .. "tpf2_bridge_ctl.txt", "r")
		if not f then return end
		local body = f:read("*a") or ""
		f:close()
		req = tonumber(body:match("speed=([%d%.]+)"))
		syncN = tonumber(body:match("sync=(%d+)")) or 0
		players = tonumber(body:match("players=(%d+)"))
		local xf = body:match("xfer=([^\r\n]*)")
		CM.xferInfo = (xf and xf ~= "" and xf ~= "-") and xf or nil
		local xf = body:match("xfer=([^\r\n]*)")
		CM.xferInfo = (xf and xf ~= "" and xf ~= "-") and xf or nil
		local ld = body:match("leader=(%a+)")
		if ld and ld ~= CM.leader then
			CM.leader = ld
			log("leader is now " .. ld .. (CM.isLeader() and " (that is us)" or ""))
		end
	end)
	if req and (req <= 0 or req >= 64) then req = nil end
	if req ~= CM.spdReq then
		CM.spdReqChangedAt = CM.ticks
		log(string.format("SPEED2: session speed request -> %s", req and string.format("%.2f", req) or "none (the host's speed buttons)"))
	end
	CM.spdReq = req
	if players then CM.rosterPlayers = players end
	if syncN ~= nil and syncN ~= (CM.syncSeen or 0) then
		CM.syncSeen = syncN
		if syncN > 0 then CM.syncBegin() else CM.syncEnd("cancelled (/sync off)") end
	end
	return req
end

-- HOT JOIN = a SYNC POINT (2026-09-09). A player arriving mid-session needs
-- the world at a known step and the only carrier is a save. The host runs it:
--   pausing : session speed 0 (a pause is a sync point: everyone runs to the
--             leader's clock and stops there); wait until we are at 0, our
--             queue is empty and every fresh peer reports our step
--   saving  : ask the menu DLL for a save (tpf2_sync_save.txt; it forces the
--             game's own autosave and then shares the file with every joiner
--             the way START GAME does, writing tpf2_sync_sent.txt)
--   waiting : hold at 0 until the roster's players are all in AND every fresh
--             peer sits at our step -- the newcomer loaded the save at exactly
--             this step -- then release the levers. "/sync off" abandons it.
-- Joiners already playing ignore the start; their lobbies latch 'started'.
-- The hash lane right after the resume is the proof that a loaded save equals
-- the memory it was taken from; a DESYNC there means it does not.
function CM.syncBegin()
	if not CM.isLeader() then return end
	-- NO PAUSE (2026-09-09, the Factorio shape): the save is taken while the
	-- session runs; the newcomer loads it at its step S, asks for every
	-- command stamped after S (LSNEED -> the history ring) and runs through
	-- them at catch-up speed until it reaches the live clock.
	CM.syncState = "saving"
	CM.syncSince = CM.ticks
	-- a marker left by an earlier sync (emptied, never deleted: see CM.clearFile)
	-- must not end this one before its save is even taken
	pcall(CM.clearFile, K.BASE .. "tpf2_sync_sent.txt")
	pcall(function()
		local f = io.open(K.BASE .. "tpf2_sync_save.txt", "w")
		if f then f:write(string.format("step=%d\n", CM.stepOf(CM.gameTime() or 0))); f:close() end
	end)
	log("SYNC: requested -- asking for a save (the session keeps running; the newcomer catches up)")
end
function CM.syncEnd(why)
	if not CM.syncState then return end
	log(string.format("SYNC: %s (was %s)", tostring(why), tostring(CM.syncState)))
	CM.syncState = nil
end
function CM.syncPeersAtStep(now)
	local myStep = CM.stepOf(now)
	local n, same = 0, 0
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			n = n + 1
			if pr.step and pr.step == myStep then same = same + 1 end
		end
	end
	return n, same
end
function CM.syncTick(now, s)
	local st = CM.syncState
	if not st then return end
	if CM.ticks - (CM.syncSince or CM.ticks) > 900 * 4 then CM.syncEnd("timed out after ~11 min"); return end
	local n, same = CM.syncPeersAtStep(now)
	if st == "saving" then
		local f = io.open(K.BASE .. "tpf2_sync_sent.txt", "r")
		if f then
			local name = f:read("*l"); f:close()
			-- EMPTIED, not deleted, and an empty marker is no marker: the game's
			-- Lua has no os.remove, and calling it here crashed a host the moment
			-- a hot-join save was shared (0.4.11, 2026-09-10).
			if name and name ~= "" then
				pcall(CM.clearFile, K.BASE .. "tpf2_sync_sent.txt")
				CM.syncEnd(string.format("save shared (%s) -- the newcomer loads it and catches up on its own", name))
			end
		end
	end
	local _ = n + same
end

-- CATCH-UP (the newcomer's side, but any instance that finds itself far
-- behind). Behind the LEADER's clock (never on the leader) by more than
-- K.CATCHUP_MIN units: hold at 0, ask the host for the command history
-- after our clock (LSNEED), wait for LSHISTEND with no gaps left (or a
-- timeout), then run at K.CATCHUP_SPEED_MAX (or the session speed, if higher)
-- until half a second of closing speed short of the leader, where ordinary
-- pacing takes over. cu=1 on
-- our heartbeat keeps the others from pacing against us meanwhile. Returns
-- the speed to impose while active, nil otherwise.
K.CATCHUP_MIN = 8          -- below this the PID closes the gap gradually; catch-up (hold + 4x) is for hot joins and stalls
K.PACE_OTHER_GAME = 1800   -- units: a leader clock this far off is another game, not one to pace against
K.FRAC_PACE_TICKS = 8      -- ~1.5 s between pacing decisions (heartbeats are 2 ticks apart)

-- The controller. Returns target speed (or nil when level) and the error.
function CM.pidPace(now, eff)
	-- GENTLER (2026-09-09, second live pass: "too harsh"): half the gains, a
	-- wider dead band, a narrower clamp, and a SLEW LIMIT -- the target may
	-- move at most `slew` per decision, so a 2-unit gap is closed by a
	-- 1.1x that creeps in over a few seconds, not a 1.3x that arrives at once.
	local kp   = 0.05
	local ki   = 0.015
	local kd   = 0.02
	local dead = 0.30    -- dead band, in game units
	local lo   = 0.70    -- clamp on the multiplier of the session speed
	local hi   = 1.20
	local slew = 0.05
	-- THE HOST IS THE CLOCK (2026-09-09): it runs the session speed untouched
	-- and only its lever changes it (a host that sees a peer lagging can slow
	-- everyone down by choice). Every joiner's reference is the host's precise
	-- clock; nobody steers the host.
	if CM.isLeader() then CM.pidHold, CM.pidErr, CM.pidI, CM.pidFar = nil, nil, 0, nil; return nil end
	local ref = CM.leaderPrecise()
	local dtTicks = CM.ticks - (CM.pidAt or CM.ticks)
	CM.pidAt = CM.ticks
	-- No 60-unit cut-off (2026-09-10): a joiner 60 ahead of the leader was
	-- left at full session speed and got 233 ahead. Only a clock THIS far
	-- away is another game (a leftover heartbeat); far BEHIND is catch-up's.
	if not ref or math.abs(ref - now) >= K.PACE_OTHER_GAME then CM.pidHold, CM.pidErr, CM.pidI, CM.pidFar = nil, nil, 0, nil; return nil end
	local e = now - ref                           -- + = we are ahead of the host
	-- FAR AHEAD of the leader (2026-09-10). The PID below is tuned for drift:
	-- 0.70x at the least, moving 0.05 per decision, so a joiner that overshot
	-- closed the gap at a crawl and pulled away again whenever the session
	-- slowed. More than `far` units ahead it runs at `farMin` of the
	-- session speed straight away -- a decimal speed (the dither reaches a
	-- quarter of the lever), never a pause -- rising in proportion as the gap
	-- closes until it meets `lo`, where the PID takes over from its floor.
	local far = 3.0
	local farMin = 0.25
	local farM = 1 - (1 - farMin) * e / far
	if e > far or (CM.pidFar and farM < lo) then
		if farM < farMin then farM = farMin end
		local target = math.max(0.25, math.floor(eff * farM / 0.05 + 0.5) * 0.05)
		if not CM.pidFar or target ~= CM.pidHold then
			log(string.format("PID: %.2f ahead of the leader -> %.2fx of %g until it reaches us", e, target, eff))
		end
		CM.pidFar = true
		CM.pidI, CM.pidLastE = 0, e
		CM.pidHold, CM.pidErr, CM.pidEff = target, e, eff
		CM.paceInfo = string.format("%.2fx e=%+.2f far ahead", target, e)
		return target, e
	end
	if CM.pidFar then
		CM.pidFar = nil
		log(string.format("PID: %.2f ahead of the leader -- fine pacing again", e))
	end
	local dt = math.max(1, dtTicks) / 5.4         -- seconds between decisions
	local eD = (math.abs(e) < dead) and 0 or e
	CM.pidI = (CM.pidI or 0) + eD * dt
	if ki > 0 then                                -- anti-windup: the I term alone stays inside the output range
		local cap = math.max(hi - 1, 1 - lo) / ki
		if CM.pidI > cap then CM.pidI = cap elseif CM.pidI < -cap then CM.pidI = -cap end
	end
	local d = (e - (CM.pidLastE or e)) / dt
	CM.pidLastE = e
	local u = kp * eD + ki * CM.pidI + kd * d
	local m = 1 - u
	if m < lo then m = lo elseif m > hi then m = hi end
	if math.abs(m - 1) < 0.025 then m = 1 end     -- level enough: exactly the session speed
	-- SLEW ON THE MULTIPLIER, not the speed (2026-09-10). The hold was an
	-- absolute speed, so when the session dropped from 4x to 1x a joiner held
	-- at 2.8x crept down from 2.75x of a 1x session and ran away ("PID ...
	-- -> 2.75x of 1", live).
	local prevM = (CM.pidHold and CM.pidEff and CM.pidEff > 0) and (CM.pidHold / CM.pidEff) or 1
	if m > prevM + slew then m = prevM + slew elseif m < prevM - slew then m = prevM - slew end
	local target = math.floor(eff * m / 0.05 + 0.5) * 0.05
	if target > (CM.MAX_SPEED or 4) then target = CM.MAX_SPEED or 4 end
	if target < 0.25 then target = 0.25 end       -- the dither's floor: a quarter of the lever
	if math.abs(m - 1) < 1e-9 then target = eff end
	if target ~= CM.pidHold then
		log(string.format("PID: e=%+.2f vs host P=%+.3f I=%+.3f D=%+.3f -> %.2fx of %g", e, kp * eD, ki * CM.pidI, kd * d, target, eff))
	end
	CM.pidHold, CM.pidErr, CM.pidEff = target, e, eff
	CM.paceInfo = string.format("%.2fx e=%+.2f", target, e)
	return target, e
end
K.CATCHUP_SPEED_MAX = 4    -- catch-up runs at this (or the session speed, if higher): the sim cannot keep up above the game's own 4 on real hardware; 8 felt SLOWER
K.CATCHUP_LOOKAHEAD = 0.5  -- s of closing speed: catch-up hands over this early (anti-overshoot)
function CM.catchUpTick(now, s)
	-- THE LEADER NEVER CATCHES UP (2026-09-10). It is the session clock; a
	-- joiner ahead of it is the one that slows down (CM.pidPace). Live, joiners
	-- that overshot sat 8 units ahead, the leader called that "behind the
	-- session", held itself at 0 waiting ~30 s for a command history only it
	-- could send (twice) and put the 0 back over its own player's play clicks.
	if CM.isLeader() then
		if CM.catchingUp2 then
			CM.catchingUp2 = false; CM.cuPhase = nil
			log("CATCHUP: we are the leader -- the session clock does not catch up")
		end
		return nil
	end
	-- behind the LEADER; the fastest other peer only while the leader is silent
	local ref = CM.leaderPrecise() or CM.peerFastPrecise()
	if not ref then
		if CM.catchingUp2 then
			CM.catchingUp2 = false; CM.cuPhase = nil
			log("CATCHUP: no leader or peer heard -- nothing to catch up with, stopped")
		end
		return nil
	end
	local behind = ref - now
	if not CM.catchingUp2 then
		if behind > K.CATCHUP_MIN then
			CM.catchingUp2 = true
			CM.cuPhase = "fetch"
			CM.cuSince = CM.ticks
			CM.histEndSeen = false
			CM.broadcast(string.format("LSNEED t=%.4f o=%s", now, K.INSTANCE))
			log(string.format("CATCHUP: %.1f unit(s) behind the leader -- holding, asked the host for the command history after %.1f", behind, now))
			return 0
		end
		return nil
	end
	if CM.cuPhase == "fetch" then
		local gaps = CM.rxGaps()
		if CM.histEndSeen and gaps == 0 then
			CM.cuPhase = "run"
			log(string.format("CATCHUP: history complete -- running at %gx to close %.1f unit(s)", K.CATCHUP_SPEED_MAX, behind))
		elseif CM.ticks - CM.cuSince > 160 then
			CM.cuPhase = "run"
			log(string.format("CATCHUP: no complete history after ~30 s (end=%s, gaps=%d) -- running anyway", tostring(CM.histEndSeen), gaps))
		else
			return 0
		end
	end
	local eff = CM.effSpeed or 1
	local speed = K.CATCHUP_SPEED_MAX
	if eff > speed then speed = eff end            -- never catch up slower than the session runs
	-- Hand over BEFORE the gap closes. The leader's clock we read is a
	-- heartbeat old and our speed change lands a frame or two late, so
	-- running on to 0.5 behind carried a 4x runner past the leader. The
	-- margin is half a second of the closing speed; the PID does the rest.
	local margin = math.max(0.5, (speed - eff) * K.CATCHUP_LOOKAHEAD)
	if behind < margin then
		CM.catchingUp2 = false; CM.cuPhase = nil
		CM.pidHold, CM.pidI, CM.pidLastE, CM.pidFar = nil, 0, nil, nil
		log(string.format("CATCHUP: %.1f unit(s) behind the leader -- ordinary pacing from here", behind))
		return nil
	end
	return speed
end

function CM.paceV2(now)
	if CM.lgHolding then return end
	local MAXS = CM.MAX_SPEED or 4
	if CM.myCeiling == nil then CM.myCeiling = MAXS end
	local s
	if not pcall(function() s = game.interface.getGameSpeed() end) or s == nil then return end
	-- Is s a speed WE imposed, or one the player just clicked? (two-slot, see CM.setSpeed)
	if CM.lastSetSpeed and s == CM.lastSetSpeed then CM.paceApplied = true end
	local settled = CM.paceApplied or (CM.ticks > (CM.paceSetTick or 0) + 8)
	local ours = (CM.lastSetSpeed and s == CM.lastSetSpeed)
		or (not CM.paceApplied and CM.prevSetSpeed and s == CM.prevSetSpeed)
	-- A LEVER MOVE THE SLICE DID NOT CANCEL (a game without the slice's hooks,
	-- or the menu setting the speed as it switches to the game), read on the
	-- leader only: a follower's lever follows the session speed. Read before
	-- anything below can return (2026-09-08): a hold check used to sit above
	-- this and hid the host's play-click. A 0 we set ourselves is `ours`.
	local prevS = CM.spd2LastS
	CM.spd2LastS = s
	if CM.isLeader() then
		-- A lever that has not moved is not the player's choice while the ceiling
		-- came from a speed button: the button's speed reaches the lever only when
		-- pacing applies it. A host whose lever pacing had never set read its old
		-- speed back here and undid every click (2026-09-10).
		local unmovedAfterButton = CM.ceilByButton and s == prevS
		if settled and not ours and s ~= CM.myCeiling and not unmovedAfterButton then
			if s == 0 then
				CM.spd2ZeroSince = CM.spd2ZeroSince or CM.ticks
				if CM.ticks - CM.spd2ZeroSince >= CM.SPD2_PAUSE_TICKS then
					CM.myCeiling = 0                   -- the player paused everyone
					CM.btnAt, CM.ceilByButton = CM.ticks, false
					log(string.format("SPEED2: player ceiling -> 0 (paused %d ticks)", CM.ticks - CM.spd2ZeroSince))
				end
			else
				CM.spd2ZeroSince = nil
				CM.myCeiling = s                       -- the player set the speed
				CM.btnAt, CM.ceilByButton = CM.ticks, false
				log(string.format("SPEED2: player ceiling -> %d", s))
			end
		elseif s ~= 0 then
			CM.spd2ZeroSince = nil
			-- A game that is SIMULATING is not paused by its player, whatever the
			-- two-slot test says: the return from a blip to the effective speed is
			-- indistinguishable from our own set, and left the ceiling at 0 forever.
			-- Not a 0 from a speed button: that pause is explicit, and the lever still
			-- reads the old speed until pacing applies it.
			if CM.myCeiling == 0 and settled and not CM.ceilByButton then
				CM.myCeiling = s
				log(string.format("SPEED2: running at %d -- the 0 ceiling was a blip, cleared", s))
			end
		end
		-- The host's player pressed play (a hand-set non-zero speed after a 0, or
		-- while the session's effective speed is 0): that unpauses the SESSION.
		if settled and not ours and not unmovedAfterButton and s > 0 and (prevS == 0 or CM.effSpeed == 0) then
			CM.hostUnpause(s)
		end
	end
	-- A pause the player just made is being DEBOUNCED (SPD2_PAUSE_TICKS) before
	-- it becomes a ceiling of 0. Pushing the session speed back onto the game
	-- during that window undoes the player's pause before it can register --
	-- nobody could pause at all. Leave the lever alone until the detector
	-- has decided (a blip clears itself when the game runs again).
	if s == 0 and CM.spd2ZeroSince and CM.myCeiling ~= 0 then return end
	-- No spread cut-off here (2026-09-10): one used to switch ALL pacing off on
	-- every instance for a 60-unit spread, the leader's session speed and the
	-- pacing that brings a runaway joiner back included -- one got 233 units
	-- ahead. Joiners pace against the leader's clock alone (CM.pidPace,
	-- CM.catchUpTick).
	if CM.isLeader() then
		-- the host's speed buttons, or a /speed request newer than the last press
		local eff = CM.myCeiling
		if eff < 0 then eff = 0 end
		local req = CM.speedRequest()
		local why = "host's speed buttons"
		CM.spdReqInForce = req and eff > 0 and (CM.spdReqChangedAt or 0) >= (CM.btnAt or -1) or false
		if CM.spdReqInForce then eff = req; why = "/speed request" end
		CM.syncTick(now, s)
		local changed = (eff ~= CM.effSpeed)
		CM.effSpeed = eff
		if not changed and (CM.ticks % 25) == 0 then CM.broadcast(string.format("LSEFF v=%g", eff)) end   -- a newcomer needs it at load
		if changed then
			CM.broadcast(string.format("LSEFF v=%g", eff))
			log(string.format("SPEED2: session speed -> %g (%s)", eff, why))
		end
	end
	-- APPLY (every instance). Joiners learn CM.effSpeed from LSEFF (net.lua),
	-- the host computed it above.
	local cu = CM.catchUpTick(now, s)
	if cu ~= nil then
		-- same lever as the catch-up speed: still clear a PID fraction left in the dither (2.8 under lever 4 is not 4x)
		if settled and s == CM.leverOf(cu) then CM.setDither(cu) end
		if settled and s ~= CM.leverOf(cu) then CM.setSpeed(cu, cu == 0 and "catch-up: holding for the history" or string.format("catch-up at %gx", cu)) end
		return
	end
	local eff = CM.effSpeed
	if eff == nil then return end
	if eff > 0 then CM.runSpeed = eff end
	local target = eff
	if eff == 0 then
		-- PAUSE IS A SYNC POINT: run to the leader's clock, then stop there.
		local fastP = CM.peerFastPrecise()
		local hi = fastP and math.max(fastP, now) or now
		if hi - now > K.SIM_STEP * 1.5 then
			target = CM.runSpeed or 1
			if not CM.syncingTo then
				log(string.format("SPEED2: session paused -- running %.1f unit(s) to the leader's clock before stopping", hi - now))
			end
			CM.syncingTo = hi
		elseif CM.syncingTo then
			log("SPEED2: reached the pause point -- stopped in step with the leader")
			CM.syncingTo = nil
		end
	else
		CM.syncingTo = nil
	end
	-- PID PACING (2026-09-09). The host runs
	-- the session speed as set; every JOINER drives its clock to the host's by
	-- scaling the session speed with the dither: ahead -> eases off, behind
	-- with headroom -> speeds up (to 1.2x, lever 4 at most). A joiner
	-- that cannot keep up stays behind and the host's table shows it; the host
	-- decides whether to slow the session. One decision per
	-- K.FRAC_PACE_TICKS from heartbeats two ticks apart; held in between.
	-- Every decision that changes the target logs its terms (CM.pidPace).
	local paced = nil
	if target == eff and eff > 0 then
		if CM.pidHold and CM.pidEff == eff and (CM.ticks - (CM.pidAt or 0)) < K.FRAC_PACE_TICKS then
			target = CM.pidHold; paced = CM.pidErr
		else
			local t2, e = CM.pidPace(now, eff)
			if t2 then target = t2; paced = e end
		end
	end
	if settled and s == CM.leverOf(target) then CM.setDither(target) end   -- same lever, new fraction
	if not paced and CM.paceInfo then CM.paceInfo = nil end
	if s ~= CM.leverOf(target) and settled then
		if paced then
			CM.setSpeed(target, string.format("PID %.2fx (e=%+.2f)", target, paced))
		elseif target == eff then
			CM.setSpeed(target, string.format("session speed %g", eff))
		else
			CM.setSpeed(target, string.format("catching up to the pause point (%.1f behind)", (CM.syncingTo or now) - now))
		end
	end
end

-- Once per tick: the slowest peer's clock for the status and dash files
-- (CM.slowT), then the session speed controller.
function CM.paceTick(now)
	local slowT = CM.peerBounds()
	CM.slowT = slowT
	if not CM.peerSeen then return end
	-- A peer that has not reported recently may itself be paused or gone:
	-- with nobody fresh there is nothing to pace against.
	-- THE LEADER runs the controller while it hears ANY fresh peer (2026-09-10).
	-- peerBounds leaves out peers that are catching up (cu=1), so while every
	-- joiner was catching up the leader skipped its own controller: its
	-- player's speed click did not become the session speed, LSEFF was not
	-- re-sent, and a pause it made was undone by the sync-point run once a
	-- joiner arrived (tools/pacing_sim.py: click_during_catchup,
	-- pause_during_catchup).
	if slowT == nil and not (CM.isLeader() and CM.livePeers() > 0) then return end
	CM.paceV2(now)
end

-- A loaded save starts at speed 0. Left alone, the clock could sit frozen
-- forever, and a command stamped in the future would never come due -- an
-- experiment that looks like it ran and simply reports nothing. Both peers do
-- this identically, and speed is local pacing rather than simulated state, so
-- it cannot itself cause divergence.
-- ONE SHOT, deliberately. Nudging the speed whenever it reads 0 would override
-- a pause the player pressed on purpose, and fight them every time they stopped
-- to look at something. Firing once after load gets an unattended test moving
-- without taking the speed control away for the rest of the session.
local didInitialUnpause = false
-- Numeric cfg value (CM.cfgFlag only answers yes/no). Shares its cache, so
-- calling this first also populates it.
function CM.cfgNum(key, default)
	CM.cfgFlag(key, false)
	local v = CM.cfgCache and CM.cfgCache[key]
	return tonumber(v) or default
end

-- exec_delay (tpf2_slice.cfg): how far ahead every command is stamped, in game
-- units, snapped UP to the 0.2 sim-step grid -- so this is the felt latency of
-- every strict action. 0.6 (three steps, ~0.66 s at speed 1) is the shipped
-- default and carries internet margin; on one machine or a LAN 0.4 is safe
-- (every apply of 2026-09-08 measured late=0 at 0.6). Below that a jitter spike
-- lands a command in a peer's PAST, which is a desync, not a delay. Absent,
-- unparsable or outside 0.2..5 = 0.6. Read HERE, after
-- cfgNum exists: reading it earlier in the file crashed the script at load
-- ("attempt to call field 'cfgNum'", 2026-09-08).
do
	local d = CM.cfgNum("exec_delay", K.EXEC_DELAY)
	if d and d >= 0.2 and d <= 5 then K.EXEC_DELAY = d end
	log(string.format("EXEC_DELAY = %.1f game unit(s) = %d sim step(s)", K.EXEC_DELAY,
		math.ceil(K.EXEC_DELAY / K.SIM_STEP - 1e-6)))
end

-- LOAD GATE: is everybody in?
--
-- Without this the first instance to finish loading starts simulating alone,
-- because paceTick returns immediately while `not peerSeen` -- it will not
-- hold against silence, which is correct once a session is running but wrong
-- before one has started. Anything the player then does is captured, found to
-- have no peer, and DROPPED by CM.soloDrop, while the native action still
-- happens here. A vehicle bought in that window exists on this instance and on
-- no other, so the setLine that follows has nothing to bind to on the peers --
-- which is exactly the "bought while the others were still loading" breakage.
--
-- The gate WITHHOLDS the initial unpause rather than commanding speed 0. The
-- game already loads paused, so this adds no new way to stop the simulation and
-- needs no watchdog of its own: the player pressing play is a first-class
-- override, handled by the s ~= 0 branch in ensureRunning below.
--
-- With the lobby roster's player count (players= in tpf2_bridge_ctl.txt) it
-- waits for that many; without one it waits for at least one peer and then
-- for the count to stop changing (K.LOADGATE_SETTLE), which handles players
-- trickling in without needing to be told how many to expect.
function CM.loadGateReady()
	-- THE LEADER NEVER WAITS. It is the session clock: whoever loads later is
	-- a hot joiner and catches up to it (LSNEED + the history ring). Holding
	-- the leader for a roster member whose game is still downloading the save
	-- froze the leader for minutes (live 2026-09-09, twice).
	if CM.isLeader() then
		if not CM.lgAnnounced then CM.lgAnnounced = true; log("LOADGATE: we are the leader -- the session clock waits for nobody") end
		return true
	end
	local n = 0
	for _ in pairs(CM.peers) do n = n + 1 end
	if n ~= CM.lgCount then
		CM.lgCount = n
		CM.lgChangedAt = CM.ticks
	end
	if CM.ticks > K.LOADGATE_MAX_TICKS then
		if not CM.lgAnnounced then
			CM.lgAnnounced = true
			log(string.format("LOADGATE: giving up after %d ticks with %d peer(s) -- starting anyway. "
				.. "Anything done before the others arrive will NOT reach them.",
				K.LOADGATE_MAX_TICKS, n))
		end
		return true
	end
	-- How many instances to expect. In order of authority:
	--   1. players= in tpf2_bridge_ctl.txt -- the LOBBY ROSTER, written by the
	--      menu DLL, which is the only thing that actually knows.
	--   2. the settle heuristic, which is a guess and can only ever be wrong in
	--      one of the two directions.
	-- Decided ONCE, at the first look: the roster as it was when WE loaded is
	-- who loaded with us. A player who joins later is a hot joiner -- it
	-- catches up to the running world -- and must never freeze anyone; and a
	-- leader who loaded alone (a relay's auto-resume) has nobody to wait for.
	-- Re-reading players= every tick did both (live 2026-09-09: the leader sat
	-- at speed 0 for minutes while a joiner's save was still in transit).
	if CM.lgWant == nil then
		local want = 0
		local f = io.open(K.BASE .. "tpf2_bridge_ctl.txt", "r")
		if f then
			local body = f:read("*a") or ""
			f:close()
			want = tonumber(body:match("players=(%d+)")) or 0
		end
		CM.lgWant = want
		if want == 1 then log("LOADGATE: loaded alone (roster of one) -- nothing to wait for") end
	end
	local want = CM.lgWant
	local ready
	local ld = CM.peers[CM.leader or "a"]
	local leaderIn = ld and ld.at and (CM.ticks - ld.at) <= K.PEER_STALE_TICKS
	if leaderIn or want == 1 then
		ready = true
	elseif want > 1 then
		ready = (n >= want - 1)
	else
		ready = (n >= 1 and (CM.ticks - (CM.lgChangedAt or CM.ticks)) >= K.LOADGATE_SETTLE)
	end
	if not ready then
		-- roughly every two seconds, so the player can see WHY it is paused
		if (CM.ticks % 12) == 0 then
			log(string.format("LOADGATE: holding at the loaded save -- %d peer(s) in%s. Press play to start anyway.",
				n, (want > 1) and string.format(", waiting for %d", want - 1)
				   or " (roster size unknown -- holding until it settles)"))
		end
		return false
	end
	if not CM.lgAnnounced then
		CM.lgAnnounced = true
		log(string.format("LOADGATE: %d peer(s) in -- releasing", n))
	end
	return true
end

function CM.ensureRunning()
	if didInitialUnpause then return end
	-- The 100-tick "let the world finish loading" grace used to sit HERE, ahead
	-- of everything. That is ~18 s during which this instance simulates at the
	-- save's own speed with no gate at all -- and 18 s is longer than the gap
	-- between two players loading, so by the first time the gate looked, the
	-- others were already in and it released without ever holding
	-- ("LOADGATE: 2 peer(s) in -- releasing", straight after load). The grace
	-- now applies only to the ordinary unpause path, below.
	if CM.ticks < K.LOADGATE_MIN_TICKS then return end
	local s
	local ok = pcall(function() s = game.interface.getGameSpeed() end)
	if not ok or s == nil then return end

	-- LOAD GATE. It has to ACTIVELY pause, not merely withhold an unpause: a
	-- save restores its OWN speed when it loads, so the game is normally
	-- already running by the time we get here. The first version only held when
	-- it found speed 0 and so never fired at all -- the log said "already
	-- running at speed 2" and the instance started simulating alone.
	--
	-- Safe despite commanding 0: ensureRunning is called UNCONDITIONALLY from
	-- the update loop (right after paceTick), so it always gets a chance to
	-- release. It releases on all three of: the roster filling up,
	-- K.LOADGATE_MAX_TICKS expiring, and the player taking the lever back.
	if not CM.loadGateReady() then
		-- A play press the slice cancelled (SPEEDBTN, CM.speedButton): no lever
		-- moved, so the lever tests below cannot see it.
		local press = CM.lgPress
		CM.lgPress = nil
		if press and press > 0 and CM.lgHeld then
			if (CM.ticks - (CM.lgHeldAt or 0)) < K.LOADGATE_FORCE_TICKS then
				log(string.format("LOADGATE: play pressed with players still loading -- held. Starting now would fork the session; the override unlocks in %d s",
					math.floor((K.LOADGATE_FORCE_TICKS - (CM.ticks - (CM.lgHeldAt or 0))) * 0.19)))
			else
				didInitialUnpause = true
				CM.lgHolding = false
				CM.setSpeed(press, "load gate: started manually")
				log(string.format("LOADGATE: game started manually at speed %d -- releasing. Anything done before the others arrive will NOT reach them.", press))
				return
			end
		end
		if s == 0 then
			CM.lgSawZero = true         -- our pause landed; anything else now is the player
		elseif not CM.lgHeld then
			CM.lgResumeSpeed = s        -- remember ONCE: the save's own speed
			CM.lgHeld, CM.lgHeldAt, CM.lgHolding = true, CM.ticks, true
			-- Through setSpeed, NOT a raw sendCommand: setSpeed records the value
			-- so the speed controller recognises the 0 as OURS. A raw send once had
			-- the gate's pause taken for the player's lever and shared with every
			-- joiner, which is how a three-player start broke.
			CM.setSpeed(0, "load gate: holding until the other players are in")
			log(string.format("LOADGATE: pausing (was speed %d) until the other players are in", s))
		elseif CM.lgSawZero and (CM.ticks - (CM.lgHeldAt or 0)) < K.LOADGATE_FORCE_TICKS then
			-- The player pressed play while someone is still loading. Too early
			-- to honour (see K.LOADGATE_FORCE_TICKS): put it back and say why.
			CM.lgSawZero = false
			CM.setSpeed(0, "load gate: still waiting for players")
			log(string.format("LOADGATE: play pressed with players still loading -- held. Starting now would fork the session; the override unlocks in %d s",
				math.floor((K.LOADGATE_FORCE_TICKS - (CM.ticks - (CM.lgHeldAt or 0))) * 0.19)))
		elseif CM.lgSawZero then
			-- We held it at 0, saw that take effect, and it is running again:
			-- the player pressed play after the window. Their lever wins.
			didInitialUnpause = true
			CM.lgHolding = false
			log(string.format("LOADGATE: game started manually at speed %d -- releasing. Anything done before the others arrive will NOT reach them.", s))
		elseif (CM.ticks - (CM.lgHeldAt or 0)) > 12 then
			-- Never saw it reach 0, so the command was lost rather than
			-- overridden. Re-send rather than mistaking this for the player.
			CM.lgHeldAt = CM.ticks
			CM.setSpeed(0, "load gate: pause did not take, re-sending")
			log("LOADGATE: pause did not take -- re-sending")
		end
		return
	end

	-- Everybody is in. If WE paused, give the speed back at once -- the world
	-- has plainly finished loading by now, and making a held game sit out the
	-- rest of the 100-tick grace would be a second, pointless freeze.
	if CM.lgHeld then
		didInitialUnpause = true
		CM.lgHolding = false
		local want = CM.lgResumeSpeed or 1
		if want == 0 then want = 1 end
		CM.setSpeed(want, "load gate: everyone is in")
		log(string.format("LOADGATE: releasing -- speed %d restored", want))
		return
	end
	if CM.ticks < 100 then return end            -- let the world finish loading
	didInitialUnpause = true
	CM.lgHolding = false
	if s == 0 then
		pcall(function() api.cmd.sendCommand(api.cmd.make.setGameSpeed(1)) end)
		log("initial unpause (speed 0 -> 1); speed is yours from here")
	else
		log("already running at speed " .. tostring(s))
	end
end
end
