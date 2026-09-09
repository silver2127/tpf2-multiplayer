-- mp/pacing.lua -- barrier, catch-up pacing, speed sharing, load gate
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.pacing")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- barrier ----------
--
-- DEADLOCK. The barrier pauses whoever is more than K.BARRIER_AHEAD in front. With
-- heartbeats every 20 ticks, each side's view of the peer was ~3.7s stale --
-- about 3.3 game units -- while the threshold was 0.4. Both instances read a
-- stale peer time, both concluded they were ahead, and both paused. With both
-- paused neither clock advances, so the gap never closes and the release
-- condition can never fire: the game froze on 22 August and stayed there.
--
-- Three independent defences, because a barrier that can freeze BOTH games is a
-- worse failure than the desync it exists to prevent:
--   1. heartbeat far more often than the threshold (see K.HEARTBEAT_EVERY)
--   2. never hold on a STALE peer time -- a silent peer is not a slow peer
--   3. a watchdog that force-releases, so any residual deadlock self-heals
K.MAX_PAUSE_TICKS  = 60     -- ~11s held = something is wrong, let it run
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

-- ---------- catch-up pacing ----------
--
-- The barrier is a wall: it does nothing until one side is K.BARRIER_AHEAD (5
-- units) in front, then pauses it dead. Between those extremes the two clocks
-- are free to drift, and they do -- whichever instance renders faster pulls
-- ahead, and a live session sat at +2 to +4 units for its whole length. Skew
-- that size is not cosmetic: it is larger than K.EXEC_DELAY, so commands from the
-- trailing side arrive in the leader's past (see scheduleLocal).
--
-- So pace continuously instead of only at the wall: whoever is BEHIND runs its
-- own clock faster until it has caught up. Speed is local pacing, not simulated
-- state -- the same lever the barrier already pulls when it pauses a peer -- and
-- it is the SAFE direction to be wrong in: if both sides wrongly believe they
-- are behind (stale heartbeats), both speed up and nothing deadlocks, which is
-- not true of both wrongly pausing.
--
-- The player's own speed choice is preserved: whatever speed they are running at
-- when the clocks agree is the speed restored after a catch-up.
CM.CATCHUP_BEHIND = 0.8    -- units behind before we run faster
CM.CATCHUP_DONE   = 0.2    -- units behind at which we hand the speed back
-- The engine's speeds are 0, 1, 2, 4 -- it reported 4 live (2026-08-31), so
-- the ladder is not consecutive. Catching up means ONE notch up from whatever
-- the player picked (1 -> 2, 2 -> 4), and there is no room at all at 4.
CM.SPEED_UP = { [1] = 2, [2] = 4, [3] = 4 }
CM.MAX_SPEED      = 4
-- THE DOWN LADDER. The pacer used to have only an UP actuator: it could push a
-- TRAILING instance one notch faster, and when it was already at the top it
-- logged "no notch left, waiting for the barrier" and did nothing. So the only
-- brake in the whole system was the barrier's setSpeed(0), which is why a
-- faster instance sawtoothed between speed 4 and a dead stop:
--
--   BARRIER hold: 5.20 ahead -> PACE: speed -> 0
--   PACE: speed -> 4 (peer caught up) -> BARRIER release: 1.80 ahead   (repeat)
--
-- This is a missing actuator, not a gain that needs tuning.
--
-- IT STOPS AT 1, NEVER 0. That is the single most important property here.
-- CM.pace is reached ONLY from applyBarrier, and applyBarrier RETURNS EARLY
-- when no peer is fresh -- before the pacer call. So a pacer that could command
-- 0 would stop the game with paused=false, where K.MAX_PAUSE_TICKS cannot see
-- it and nothing would ever re-command a speed: a permanent freeze one dropped
-- peer away. Only the barrier may command 0, because only the barrier sets
-- paused and is therefore covered by the watchdog.
CM.SPEED_DOWN = { [4] = 2, [2] = 1 }   -- deliberately no [1]: 1 is the floor
-- Throttle well before the barrier would fire (K.BARRIER_AHEAD = 5.0), and let
-- go at a clearly lower lead so the two thresholds cannot chatter against each
-- other. Both are measured against the SLOWEST peer, because that is the
-- quantity the barrier actually trips on -- the pacer's old "behind" reading
-- chases the FASTEST peer, which is the wrong end for braking.
-- ONE controller, ONE signal, and a deadband wider than one correction.
--
-- The first attempt used two: this brake on the lead over the SLOWEST peer, and
-- the pre-existing catch-up on the lag behind the FASTEST peer. With three
-- instances an instance is routinely BOTH -- ahead of the slowest and behind
-- the fastest -- so the two fought each other every dwell:
--
--   PACE: speed -> 2 (2.60 ahead of the slowest peer)
--   PACE: speed -> 4 (1.00 behind the peer)
--
-- Both now read the lead over the SLOWEST peer, which is the quantity
-- K.BARRIER_AHEAD fires on: positive means slow down, negative means we are the
-- laggard and may speed up. Everyone converges on the slowest member, which is
-- the only pace the whole session can actually sustain.
--
-- THE DEADBAND MUST EXCEED ONE CORRECTION'S EFFECT, or the loop cannot settle
-- however the thresholds are placed. A notch is worth ~1.8 units/s of relative
-- drift, so over a dwell of D ticks (~5.4 Hz) one correction moves the lead by
-- ~1.8 * D / 5.4 units. At the old PACE_COOLDOWN of 16 ticks that is ~5.3
-- units -- larger than any sane band, which is why every adjustment overshot
-- and the observed cycle was a clean 0.80 <-> 2.60 limit cycle. At 8 ticks it
-- is ~2.7, comfortably inside the 4.5-unit band below.
-- Ticks are ~5.4 Hz. At a dwell of 5 (~0.93 s) one notch moves the lead ~1.7
-- units, so a 2.0-unit band still comfortably exceeds one correction and the
-- loop settles -- while capping the steady-state skew near 1.2 instead of 3.
-- Tightening further is measurement-bound, not gain-bound: the peer clock now
-- reads to 0.2 units, so a band under ~1 would chatter on quantisation alone.
CM.PACE_DWELL     = 5      -- ticks between ordinary (non-micropause) speed nudges
-- MICROPAUSE PACING. Every instance runs at the player's chosen speed. An
-- instance that is behind or level simply STAYS at that speed and catches
-- up -- it is never throttled (the old notch-down is what made a client that
-- fell 1.2 units behind drop to speed 1 and never recover). Only an instance
-- that has pulled more than PACE_MICRO_AHEAD ahead of the slowest peer
-- micropauses: brief speed-0 pulses, each hard-capped at PACE_MICRO_MAX ticks
-- so a pulse can never wedge the game, repeated until the lead is shaved back
-- under PACE_MICRO_DONE. The hard barrier (K.BARRIER_AHEAD) sits above the
-- micropause band as a rare backstop.
CM.PACE_MICRO_AHEAD = 5.0   -- only slow down once this far ahead of the slowest
CM.PACE_MICRO_DONE  = 2.0   -- stop pulsing once the lead is back under this
CM.PACE_MICRO_MAX   = 3     -- max ticks per pause pulse (self-limiting)
CM.THROTTLE_AHEAD = 1.2    -- above this we are too far ahead: one notch down
CM.PACE_BEHIND    = 0.8    -- below -this we are the laggard: one notch up
CM.catchingUp  = false
CM.baseSpeed   = nil       -- the player's speed, sampled while in step
CM.pacedTopWarned = false  -- log the "no notch left" case once, not per tick

-- WHO OWNS THE SPEED CONTROL. The pacer and the player share one lever, and
-- without an owner they fight over it: the pacer sets 2 to catch up, the player
-- presses 1 because their game is running away, the pacer sets 2 again on the
-- next tick, and the game stutters between them (reported from a live session).
--
-- Two rules settle it. First, a speed we did not set is the PLAYER's, and the
-- player wins: the pacer adopts it as the new normal and stops chasing. Second,
-- a change of ours starts a cooldown, so the controller can never flap faster
-- than a person can react to what it did.
CM.PACE_COOLDOWN = 16         -- ticks between changes we make: a tick is ~0.19 s, so ~3 s

-- SPEED V2 (cfg speed_v2, default off): host-authoritative effective speed.
-- effective = min(each player's manual ceiling, a sustainable cap). Every
-- instance runs the SAME effective speed, so no instance out-runs another and
-- the lead never accumulates -- which is what the v1 micropause/notch pacer
-- fought with bang-bang corrections, producing the 4<->2 limit-cycle flicker
-- (measured 2026-09-03). The player's speed button sets THIS instance's ceiling
-- (min across players wins -- anyone can slow the shared clock; a pause is a
-- ceiling of 0). The sustainable cap drops a notch when the fastest-slowest
-- lead grows for a sustained window (a machine cannot keep up -> slow everyone)
-- and climbs back toward the ceiling only after prolonged stability -- the
-- hysteresis makes speed changes rare and deliberate, never a per-tick loop.
CM.SPD2_LEAD_DOWN  = 3.0   -- lead (units) above this, sustained -> cap down a notch
CM.SPD2_LEAD_UP    = 0.6   -- lead below this, long-sustained -> cap up a notch
CM.SPD2_DOWN_TICKS = 8     -- ~1.5 s of high lead before stepping down
CM.SPD2_UP_TICKS   = 40    -- ~7.5 s of low lead before stepping up (asymmetric)
-- LEADER MICROPAUSE under v2 (2026-09-08). One nominal speed for everyone does
-- not give one THROUGHPUT: the rendered host instance falls behind the sandboxed
-- ones, and a lead of 2.6-3.2 units sat there all session -- v2 only steps the
-- shared cap down above SPD2_LEAD_DOWN, and a lower cap does not touch the
-- differential anyway. That lead is paid twice: every command is stamped
-- EXEC_DELAY + lead out ("assigning a stop takes several tries" = 4 s of
-- nothing happening), and the leader's heartbeat under-reports it, so commands
-- land in its past (LATE applies, and the 5.6 m vehicle offset after a set-line:
-- the leader started the vehicle 2-3 sim steps late). So whoever is more than
-- SPD2_MICRO_AHEAD ahead of the slowest peer (step-precise reading) pulses speed
-- 0, hard-capped at PACE_MICRO_MAX ticks, until back under SPD2_MICRO_DONE.
-- Heartbeats every 2 ticks keep the reading within ~0.35 units of the truth.
CM.SPD2_MICRO_AHEAD = 1.0
CM.SPD2_MICRO_DONE  = 0.4
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
-- start and this does it again at script load, so a leftover fraction from
-- the last session cannot slow this one from its first frame.
pcall(function() os.remove(K.BASE .. "tpf2_speed.txt") end)
CM.ditherCur = ""
function CM.setDither(v)
	local want = (v and v ~= math.floor(v)) and string.format("%.4f", v) or ""
	if CM.ditherCur == want then return end
	CM.ditherCur = want
	pcall(function()
		local p = K.BASE .. "tpf2_speed.txt"
		if want == "" then os.remove(p) else local f = io.open(p, "w"); if f then f:write(want, "\n"); f:close() end end
	end)
end

function CM.setSpeed(v, why)
	v = tonumber(v) or 0
	CM.setDither(v)
	v = CM.leverOf(v)
	-- TWO SLOTS, not one. A single remembered value is enough only while at most
	-- one of our commands is in flight; the moment corrections come faster than
	-- the engine applies them, the engine reports the OLDER one, shareSpeed
	-- fails to recognise it as ours, concludes the PLAYER moved the lever, and
	-- broadcasts our own throttle to every peer as their choice.
	CM.prevSetSpeed = CM.lastSetSpeed
	CM.lastSetSpeed = v
	CM.paceSetTick = CM.ticks
	CM.paceApplied = false
	-- The pacing dwell, NOT PACE_COOLDOWN. This is checked before the pacer's
	-- own dwell, so leaving it at the 16-tick cooldown made that dwell inert:
	-- corrections stayed ~3 s apart and each one moved the lead ~5 units, which
	-- is larger than any workable deadband and is why adjustments overshot.
	CM.paceQuietUntil = CM.ticks + (CM.PACE_DWELL or CM.PACE_COOLDOWN)
	pcall(function() api.cmd.sendCommand(api.cmd.make.setGameSpeed(v)) end)
	log(string.format("PACE: speed -> %s (%s)", tostring(v), why))
end

-- ONE SPEED FOR THE SESSION. Each instance used to run at whatever its player
-- chose, and the barrier and pacer then fought to keep two clocks together that
-- were being driven apart on purpose: one side at 4 racing ahead, the other at 1
-- being paused and released in turn. A speed change the PLAYER makes is now
-- shared, and the other side adopts it -- so both clocks run at the same rate
-- and the pacer is left with only the small drift it was built for.
--
-- Only the player's changes travel. Ours (a catch-up notch, a barrier hold, a
-- release, or a speed we adopted from the peer) are recognised because setSpeed
-- recorded them, and are never re-broadcast -- that is what stops the two
-- instances echoing one change back and forth forever.
function CM.shareSpeed()
	local s
	if not pcall(function() s = game.interface.getGameSpeed() end) or s == nil then return end
	local prev = CM.lastSeenSpeed
	CM.lastSeenSpeed = s
	if prev == nil or s == prev then return end
	if CM.lastSetSpeed ~= nil and s == CM.lastSetSpeed then return end   -- ours, not the player's
	-- ...and the one before it, while our newest has not been observed yet.
	if not CM.paceApplied and CM.prevSetSpeed ~= nil and s == CM.prevSetSpeed then return end
	CM.baseSpeed = s
	CM.catchingUp = false
	CM.broadcast(string.format("LSSPEED v=%d o=%s", s, K.INSTANCE))
	log(string.format("SPEED: player set %d -- shared with the peer", s))
end

-- `ahead` is the lead over the FASTEST peer (the catch-up signal, unchanged).
-- `lead` is the lead over the SLOWEST peer -- the quantity K.BARRIER_AHEAD
-- fires on, and therefore the only correct signal for braking.
function CM.pace(ahead, lead)
	-- The load gate owns the speed until every player is in. Without this the
	-- pacer would see one peer, floor itself at 1 and fight the gate's 0.
	if CM.lgHolding then return end
	if CM.paused then return end                       -- the barrier owns the speed
	local behind = -ahead
	-- A gap this size is not pacing: it is two instances on different saves, or
	-- one still loading. Seen live at 55156 units. Speeding up cannot fix that
	-- and pretending otherwise just runs somebody's game at double speed.
	if behind > 60 or behind < -60 then return end
	local s
	if not pcall(function() s = game.interface.getGameSpeed() end) or s == nil then return end
	-- A speed of 0 is the PLAYER pausing -- unless it is OUR micropause pulse,
	-- which must fall through to its own end-check below or it never ends.
	if s == 0 and not CM.microPausing then           -- player paused on purpose
		-- If we were mid-catch-up, the game will come back at OUR 2, not the
		-- player's speed; remember to hand it back the moment it does.
		if CM.catchingUp then CM.restoreAfterPause = CM.baseSpeed or 1 end
		CM.catchingUp = false
		CM.lastSetSpeed = nil
		return
	end
	if CM.restoreAfterPause then
		local back = CM.restoreAfterPause
		CM.restoreAfterPause = nil
		CM.setSpeed(back, "player's speed restored after their pause")
		return
	end
	-- setGameSpeed is a COMMAND: the engine applies it a tick or more after we
	-- send it. On the tick in between, the speed still reads the old value --
	-- which the check below would take for the player moving the lever, adopt
	-- as their choice, and cancel the very catch-up we just started. So a
	-- mismatch only counts as the player's once our own change has been seen
	-- to land, or after a grace period in case it never does (review,
	-- 2026-08-31).
	if CM.lastSetSpeed and s == CM.lastSetSpeed then CM.paceApplied = true end
	local settled = CM.paceApplied or (CM.ticks > (CM.paceSetTick or 0) + 8)   -- ~1.5 s
	-- The player moved the lever: that is now the speed they want. Adopt it,
	-- stop any catch-up in progress, and do not argue.
	if CM.lastSetSpeed and settled and s ~= CM.lastSetSpeed then
		if CM.catchingUp then
			log(string.format("PACE: player set speed %s while catching up -- theirs wins", tostring(s)))
		end
		CM.baseSpeed = s
		CM.catchingUp = false
		CM.paceMovedAt = nil      -- their lever outranks our pacing; react at once
		CM.lastSetSpeed = nil
		CM.paceQuietUntil = CM.ticks + CM.PACE_COOLDOWN
		return
	end
	-- (The old "caught up -> snap back to the player's speed" branch lived here.
	-- It fired on `behind`, the FASTEST-peer signal, and jumped straight to the
	-- ceiling -- re-accelerating us into the next throttle and completing the
	-- limit cycle. The controller below holds instead, in both directions, off
	-- a single signal.)
	if lead == nil then return end
	local ceiling = CM.baseSpeed or CM.MAX_SPEED
	if ceiling < 1 then ceiling = 1 end

	-- MICROPAUSE duty-cycle. Checked EVERY tick (exempt from the dwell) so a
	-- pulse ends on time; the pulse is hard-capped so it cannot wedge.
	if CM.microPausing then
		if lead <= CM.PACE_MICRO_DONE or (CM.ticks - (CM.microPausedAt or CM.ticks)) >= CM.PACE_MICRO_MAX then
			CM.microPausing = false
			CM.setSpeed(ceiling, string.format("micropause done (%.2f ahead)", lead))
		end
		return
	end
	if lead > CM.PACE_MICRO_AHEAD then
		-- too far ahead: pulse-pause to let the slowest peer close the gap
		CM.microPausing = true
		CM.microPausedAt = CM.ticks
		CM.setSpeed(0, string.format("micropause: %.2f ahead of the slowest peer", lead))
		return
	end

	-- ordinary nudges honour the dwell so we do not spam setGameSpeed
	if CM.paceQuietUntil and CM.ticks < CM.paceQuietUntil then return end

	-- BEHIND OR LEVEL: run at the player's chosen speed so we catch up. A
	-- lagging client is NEVER throttled -- that was the bug (a client that fell
	-- behind got dropped a notch and never came back). If we are below the
	-- target, climb straight to it.
	if s ~= ceiling then
		CM.setSpeed(ceiling, string.format("resume to ceiling %d (%.2f ahead of slowest)", ceiling, lead))
	elseif not CM.lastSetSpeed then
		CM.baseSpeed = s   -- at the ceiling and it was not one we set: adopt as the player's
	end
end

-- Undo a hold that WE placed. If the speed is no longer the 0 we set, the
-- player has taken the lever back (they paused, or unpaused us) -- leave it
-- alone rather than yanking the game back to speed under their hands.
function CM.releaseSpeed(why)
	local s0
	pcall(function() s0 = game.interface.getGameSpeed() end)
	if s0 ~= nil and s0 ~= 0 then
		log("BARRIER release: the player already changed the speed (" .. why .. ") -- theirs kept")
		CM.lastSetSpeed = nil
		return
	end
	CM.setSpeed(CM.baseSpeed or 1, why)
end

-- HOST-AUTHORITATIVE UNPAUSE (2026-09-08). The host's play-click beats every
-- peer's ceiling of 0: their 0s are forgotten here (a peer re-advertises its
-- real ceiling on its next heartbeat, and the LSEFF handler lifts a 0 of its
-- own), and LSEFF carries the new speed at once. Before this the host could
-- not unpause the session at all while any peer's ceiling read 0 -- and with
-- the host behind and the peers barrier-held on it, nobody could.
function CM.hostUnpause(s)
	local cleared = 0
	for _, pr in pairs(CM.peers) do
		if pr.ceil == 0 then pr.ceil = nil; cleared = cleared + 1 end
	end
	local v = math.min(s, CM.susCap or s)
	if v < 1 then v = 1 end
	CM.effSpeed = v; CM.baseSpeed = v
	CM.broadcast(string.format("LSEFF v=%d", v))
	log(string.format("SPEED2: host unpaused the session at %d (%d peer ceiling(s) of 0 overridden)", v, cleared))
end

-- SPEED V2 controller -- MANUAL model (2026-09-09). The players own the lever:
--   * every instance reads its own speed button as its CEILING (0..4) and
--     ships it on the heartbeat; the host takes the MINIMUM over everyone and
--     broadcasts it as the session speed (LSEFF). Anyone can slow or pause
--     the whole session; the host's play-click overrides a stuck 0.
--   * a PAUSE IS A SYNC POINT: when the session speed is 0 the leader stops
--     at once and everyone behind keeps running until they reach the
--     leader's clock, then stops there. So "pause to let people catch up"
--     does exactly that, and an unpause resumes everyone in step.
--   * nothing else moves the lever. The automatic corrections that were here
--     (leader micropause pulses, the hysteretic sustainable cap) throttled
--     and stuttered the session in ways players felt but could not see;
--     they are kept behind cfg speed_auto=1, default OFF.
-- What remains automatic is the correctness floor only: the hard barrier
-- (K.BARRIER_AHEAD) and the load gate. Commands are stamped past the fastest
-- clock (net.lua), so a gap costs the slow player latency, never a fork.
-- "/speed 2.5" typed in the lobby chat: the panel (menu DLL) writes speed=2.5
-- into tpf2_bridge_ctl.txt; the HOST reads it here as the session speed
-- request. Read every ~2 s, not per tick. "/speed off" (or 0, or 1..4 as a
-- whole number) clears it and the lowest lever rules again.
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
		local pid = body:match("pid=([^\r\n]+)")
		if pid and pid ~= CM.pidOvText then
			CM.pidOvText = pid
			CM.pidOv = CM.pidOv or {}
			for k, v in pid:gmatch("(%a+)[=:]([%d%.]+)") do
				if tonumber(v) then CM.pidOv[k] = tonumber(v) end
			end
			log("PID: gains from the tuner -> " .. pid)
		end
	end)
	if req and (req <= 0 or req >= 64) then req = nil end
	if req ~= CM.spdReq then
		log(string.format("SPEED2: session speed request -> %s", req and string.format("%.2f", req) or "none (lowest lever)"))
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
	if K.INSTANCE ~= "a" then return end
	-- NO PAUSE (2026-09-09, the Factorio shape): the save is taken while the
	-- session runs; the newcomer loads it at its step S, asks for every
	-- command stamped after S (LSNEED -> the history ring) and runs through
	-- them at catch-up speed until it reaches the live clock.
	CM.syncState = "saving"
	CM.syncSince = CM.ticks
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
			local name = f:read("*l") or "?"; f:close()
			os.remove(K.BASE .. "tpf2_sync_sent.txt")
			CM.syncEnd(string.format("save shared (%s) -- the newcomer loads it and catches up on its own", name))
		end
	end
	local _ = n + same
end

-- CATCH-UP (the newcomer's side, but any instance that finds itself far
-- behind). Behind the fastest non-catching-up peer by more than
-- K.CATCHUP_MIN units: hold at 0, ask the host for the command history
-- after our clock (LSNEED), wait for LSHISTEND with no gaps left (or a
-- timeout), then run at catchup_speed (cfg, default 8) until within half a
-- unit of the session, at which point ordinary pacing takes over. cu=1 on
-- our heartbeat keeps the others from pacing against us meanwhile. Returns
-- the speed to impose while active, nil otherwise.
K.CATCHUP_MIN = 8          -- below this the PID closes the gap gradually; catch-up (hold + 4x) is for hot joins and stalls
K.FRAC_PACE_TICKS = 8      -- ~1.5 s between pacing decisions (heartbeats are 2 ticks apart)

-- The controller. Returns target speed (or nil when level) and the error.
function CM.pidPace(now, eff)
	-- GENTLER (2026-09-09, second live pass: "too harsh"): half the gains, a
	-- wider dead band, a narrower clamp, and a SLEW LIMIT -- the target may
	-- move at most pid_slew per decision, so a 2-unit gap is closed by a
	-- 1.1x that creeps in over a few seconds, not a 1.3x that arrives at once.
	-- "/pid kp=0.06 ..." from the in-game tuner overrides the cfg (CM.pidOv,
	-- filled by speedRequest from the ctl file's pid= line)
	local ov = CM.pidOv or {}
	local kp   = ov.kp   or CM.cfgNum("pid_kp",   0.05)
	local ki   = ov.ki   or CM.cfgNum("pid_ki",   0.015)
	local kd   = ov.kd   or CM.cfgNum("pid_kd",   0.02)
	local dead = ov.dead or CM.cfgNum("pid_dead", 0.30)
	local lo   = ov.min  or CM.cfgNum("pid_min",  0.70)
	local hi   = ov.max  or CM.cfgNum("pid_max",  1.20)
	local slew = ov.slew or CM.cfgNum("pid_slew", 0.05)
	CM.pidGains = string.format("kp:%g,ki:%g,kd:%g,dead:%g,min:%g,max:%g,slew:%g", kp, ki, kd, dead, lo, hi, slew)
	-- THE HOST IS THE CLOCK (2026-09-09): it runs the session speed untouched
	-- and only its lever changes it (a host that sees a peer lagging can slow
	-- everyone down by choice). Every joiner's reference is the host's precise
	-- clock; nobody steers the host.
	if K.INSTANCE == "a" then CM.pidHold, CM.pidErr, CM.pidI = nil, nil, 0; return nil end
	local host = CM.peers["a"]
	local ref
	if host and host.at and (CM.ticks - host.at) <= K.PEER_STALE_TICKS then
		ref = host.step and (host.step * K.SIM_STEP) or host.time
	end
	local dtTicks = CM.ticks - (CM.pidAt or CM.ticks)
	CM.pidAt = CM.ticks
	if not ref or math.abs(ref - now) >= 60 then CM.pidHold, CM.pidErr, CM.pidI = nil, nil, 0; return nil end
	local n = 1
	local e = now - ref                           -- + = we are ahead of the host
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
	local target = math.floor(eff * m / 0.05 + 0.5) * 0.05
	if target > (CM.MAX_SPEED or 4) then target = CM.MAX_SPEED or 4 end
	if target < 0.5 then target = 0.5 end
	if math.abs(m - 1) < 0.025 then target = eff end   -- level enough: exactly the session speed
	local prev = CM.pidHold or eff
	local maxStep = slew * eff
	if target > prev + maxStep then target = prev + maxStep elseif target < prev - maxStep then target = prev - maxStep end
	target = math.floor(target / 0.05 + 0.5) * 0.05
	if target ~= CM.pidHold then
		log(string.format("PID: e=%+.2f vs host P=%+.3f I=%+.3f D=%+.3f -> %.2fx of %g", e, kp * eD, ki * CM.pidI, kd * d, target, eff))
	end
	CM.pidHold, CM.pidErr = target, e
	CM.paceInfo = string.format("%.2fx e=%+.2f", target, e)
	-- the last 60 decisions, for the in-game graph
	CM.pidHist = CM.pidHist or {}
	CM.pidHist[#CM.pidHist + 1] = { e = e, m = target / eff }
	while #CM.pidHist > 60 do table.remove(CM.pidHist, 1) end
	return target, e
end
K.CATCHUP_SPEED_MAX = 4    -- the sim cannot keep up above the game's own 4 on real hardware; 8 felt SLOWER
function CM.catchUpTick(now, s)
	if not CM.cfgFlag("hot_join", true) then return nil end
	local fastP = CM.peerFastPrecise()
	if not fastP then return nil end
	local behind = fastP - now
	if not CM.catchingUp2 then
		if behind > K.CATCHUP_MIN then
			CM.catchingUp2 = true
			CM.cuPhase = "fetch"
			CM.cuSince = CM.ticks
			CM.histEndSeen = false
			CM.broadcast(string.format("LSNEED t=%.4f o=%s", now, K.INSTANCE))
			log(string.format("CATCHUP: %.1f unit(s) behind the session -- holding, asked the host for the command history after %.1f", behind, now))
			return 0
		end
		return nil
	end
	if CM.cuPhase == "fetch" then
		local gaps = CM.rxGaps()
		if CM.histEndSeen and gaps == 0 then
			CM.cuPhase = "run"
			log(string.format("CATCHUP: history complete -- running at %gx to close %.1f unit(s)", math.min(K.CATCHUP_SPEED_MAX, CM.cfgNum("catchup_speed", 4)), behind))
		elseif CM.ticks - CM.cuSince > 160 then
			CM.cuPhase = "run"
			log(string.format("CATCHUP: no complete history after ~30 s (end=%s, gaps=%d) -- running anyway", tostring(CM.histEndSeen), gaps))
		else
			return 0
		end
	end
	if behind < 0.5 then
		CM.catchingUp2 = false; CM.cuPhase = nil
		log("CATCHUP: caught up with the session -- ordinary pacing from here")
		return nil
	end
	return math.max(1, math.min(K.CATCHUP_SPEED_MAX, CM.cfgNum("catchup_speed", 4)))
end

function CM.paceV2(now, lead)
	if CM.lgHolding then return end
	local MAXS = CM.MAX_SPEED or 4
	if CM.myCeiling == nil then CM.myCeiling = MAXS end
	if CM.susCap == nil then CM.susCap = MAXS end
	local s
	if not pcall(function() s = game.interface.getGameSpeed() end) or s == nil then return end
	-- Is s a speed WE imposed, or one the player just clicked? (two-slot, as v1)
	if CM.lastSetSpeed and s == CM.lastSetSpeed then CM.paceApplied = true end
	local settled = CM.paceApplied or (CM.ticks > (CM.paceSetTick or 0) + 8)
	local ours = (CM.lastSetSpeed and s == CM.lastSetSpeed)
		or (not CM.paceApplied and CM.prevSetSpeed and s == CM.prevSetSpeed)
	-- PLAYER CEILING. Read even while the hard barrier holds us (2026-09-08):
	-- the `if CM.paused then return end` used to sit above this, so a held
	-- instance's play-click was invisible -- which closed the deadlock (b's
	-- ceiling stuck at 0 -> host effective 0 -> host behind -> peers held on
	-- it -> b's detector never ran). The barrier's own 0 is `ours` and skipped.
	local prevS = CM.spd2LastS
	CM.spd2LastS = s
	if settled and not ours and s ~= CM.myCeiling then
		if s == 0 then
			CM.spd2ZeroSince = CM.spd2ZeroSince or CM.ticks
			if CM.ticks - CM.spd2ZeroSince >= CM.SPD2_PAUSE_TICKS then
				CM.myCeiling = 0                   -- the player paused everyone
				log(string.format("SPEED2: player ceiling -> 0 (paused %d ticks)", CM.ticks - CM.spd2ZeroSince))
			end
		else
			CM.spd2ZeroSince = nil
			CM.myCeiling = s                       -- the player set their ceiling
			log(string.format("SPEED2: player ceiling -> %d", s))
		end
	elseif s ~= 0 then
		CM.spd2ZeroSince = nil
		-- A game that is SIMULATING is not paused by its player, whatever the
		-- two-slot test says: the return from a blip to the effective speed is
		-- indistinguishable from our own set, and left the ceiling at 0 forever.
		if CM.myCeiling == 0 and settled then
			CM.myCeiling = s
			log(string.format("SPEED2: running at %d -- the 0 ceiling was a blip, cleared", s))
		end
	end
	-- The host's player pressed play (a hand-set non-zero speed after a 0, or
	-- while the session's effective speed is 0): that unpauses the SESSION.
	if K.INSTANCE == "a" and settled and not ours and s > 0 and (prevS == 0 or CM.effSpeed == 0) then
		CM.hostUnpause(s)
	end
	-- A pause the player just made is being DEBOUNCED (SPD2_PAUSE_TICKS) before
	-- it becomes a ceiling of 0. Pushing the session speed back onto the game
	-- during that window undoes the player's pause before it can register --
	-- nobody could pause at all. Leave the lever alone until the detector
	-- has decided (a blip clears itself when the game runs again).
	if s == 0 and CM.spd2ZeroSince and CM.myCeiling ~= 0 then return end
	if CM.paused then return end                 -- the hard barrier owns the speed transiently
	-- a peer 60+ units away is in a different game (loading, or a leftover
	-- heartbeat from the last session): never pace against it.
	if lead > 60 then return end
	local auto = CM.cfgFlag("speed_auto", false)
	if auto then
		local slowP = CM.peerSlowPrecise()
		local myLead = slowP and (now - slowP) or 0
		if CM.microPausing then
			if myLead <= CM.SPD2_MICRO_DONE or (CM.ticks - (CM.microPausedAt or CM.ticks)) >= CM.PACE_MICRO_MAX then
				CM.microPausing = false
				CM.setSpeed(CM.effSpeed or CM.baseSpeed or 1, string.format("micropause done (%.2f ahead)", myLead))
			end
			return
		end
		if myLead > CM.SPD2_MICRO_AHEAD and myLead < 60 and s ~= 0 and CM.myCeiling ~= 0 then
			CM.microPausing = true
			CM.microPausedAt = CM.ticks
			CM.setSpeed(0, string.format("micropause: %.2f ahead of the slowest peer", myLead))
			return
		end
	end
	if K.INSTANCE == "a" then
		-- min ceiling across fresh instances (self + peers)
		local minCeil = CM.myCeiling
		for _, pr in pairs(CM.peers) do
			if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS and pr.ceil and pr.ceil < minCeil then
				minCeil = pr.ceil
			end
		end
		local eff = minCeil
		if auto then
			-- sustainable cap, hysteretic on the fastest-slowest lead
			lead = lead or 0
			if lead > CM.SPD2_LEAD_DOWN then
				CM.spd2HiSince = CM.spd2HiSince or CM.ticks
				CM.spd2LoSince = nil
				if CM.ticks - CM.spd2HiSince >= CM.SPD2_DOWN_TICKS then
					CM.susCap = math.max(1, (CM.effSpeed or minCeil) - 1)
					CM.spd2HiSince = CM.ticks
					log(string.format("SPEED2: lead %.1f sustained -- cap down to %d", lead, CM.susCap))
				end
			elseif lead < CM.SPD2_LEAD_UP then
				CM.spd2LoSince = CM.spd2LoSince or CM.ticks
				CM.spd2HiSince = nil
				if (CM.effSpeed or 0) < minCeil and CM.ticks - CM.spd2LoSince >= CM.SPD2_UP_TICKS then
					CM.susCap = math.min(MAXS, (CM.effSpeed or 1) + 1)
					CM.spd2LoSince = CM.ticks
					log(string.format("SPEED2: in step -- cap up to %d", CM.susCap))
				end
			else
				CM.spd2HiSince = nil; CM.spd2LoSince = nil
			end
			if CM.susCap < 1 then CM.susCap = 1 end
			eff = math.min(minCeil, CM.susCap)
		end
		if eff < 0 then eff = 0 end
		local req = CM.speedRequest()
		local why = "lowest lever"
		if req and eff > 0 then eff = req; why = "/speed request" end
		CM.syncTick(now, s)
		local changed = (eff ~= CM.effSpeed)
		CM.effSpeed = eff
		if not changed and (CM.ticks % 25) == 0 then CM.broadcast(string.format("LSEFF v=%g", eff)) end   -- a newcomer needs it at load
		if changed then
			CM.broadcast(string.format("LSEFF v=%g", eff))
			log(string.format("SPEED2: session speed -> %g (%s%s)", eff, why, auto and ", auto cap " .. tostring(CM.susCap) or ""))
		end
	end
	-- APPLY (every instance). Joiners learn CM.effSpeed from LSEFF (net.lua),
	-- the host computed it above.
	local cu = CM.catchUpTick(now, s)
	if cu ~= nil then
		if settled and s ~= CM.leverOf(cu) then CM.setSpeed(cu, cu == 0 and "catch-up: holding for the history" or string.format("catch-up at %gx", cu)) end
		return
	end
	local eff = CM.effSpeed
	if eff == nil then return end
	if eff > 0 then CM.runSpeed = eff end
	CM.baseSpeed = eff                          -- a barrier release then returns to eff
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
	-- FRACTIONAL PACING (2026-09-09, cfg speed_frac_pace, default on). Whoever
	-- is ahead of the slowest fresh clock runs proportionally SLOWER -- a smooth
	-- fraction through the dither (see CM.setSpeed), never a pause pulse -- until
	-- the gap closes: 1 unit ahead = 3/4 speed, 2 ahead = half, floor at 0.4x.
	-- The slowest instance is the reference and runs the session speed; it is
	-- never asked to go faster than that, since it cannot. Quantised to 0.05 so
	-- the dither file is rewritten on real changes only. Replaces the pulses
	-- players felt as stutter: the leader eases off and the tail catches up.
	-- PID PACING (2026-09-09, cfg speed_frac_pace, default on). The host runs
	-- the session speed as set; every JOINER drives its clock to the host's by
	-- scaling the session speed with the dither: ahead -> eases off, behind
	-- with headroom -> speeds up (to pid_max x, lever 4 at most). A joiner
	-- that cannot keep up stays behind and the host's table shows it; the host
	-- decides whether to slow the session. One decision per
	-- K.FRAC_PACE_TICKS from heartbeats two ticks apart; held in between.
	-- LIVE-TUNABLE: pid_kp, pid_ki, pid_kd, pid_dead, pid_min, pid_max in
	-- tpf2_slice.cfg are re-read every ~5 s (CM.cfgFlag), and every decision
	-- that changes the target logs its terms.
	local paced = nil
	if target == eff and eff > 0 and CM.cfgFlag("speed_frac_pace", true) then
		if CM.pidHold and (CM.ticks - (CM.pidAt or 0)) < K.FRAC_PACE_TICKS then
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

function CM.applyBarrier(now)
	local slowT, fastT = CM.peerBounds()
	CM.slowT, CM.fastT = slowT, fastT
	if not CM.peerSeen then return end

	-- Watchdog first, so it runs even when the conditions below would hold.
	if CM.paused and CM.pausedSince and (CM.ticks - CM.pausedSince) > K.MAX_PAUSE_TICKS then
		CM.paused = false
		CM.pausedSince = nil
		CM.releaseSpeed("watchdog")
		log(string.format("!! BARRIER WATCHDOG: held %d ticks, forcing release " ..
			"(now=%.2f peer=%.2f). Sync is not guaranteed while this fires.",
			K.MAX_PAUSE_TICKS, now, slowT or -1))
		return
	end

	-- A peer that has not reported recently may itself be paused or gone.
	-- Holding against a stale reading is exactly how both sides deadlock.
	local stale = (slowT == nil)   -- nobody fresh: do not hold against silence
	if stale then
		if CM.paused then
			CM.paused = false
			CM.pausedSince = nil
			CM.releaseSpeed("peer time stale")
			log("BARRIER release: peer time is stale, not holding against it")
		end
		return
	end

	local ahead = now - slowT          -- the barrier holds against the SLOWEST peer
	-- Nothing the gate does with the speed is the player's choice; never share it.
	if CM.cfgFlag("speed_v2", true) then
		-- V2: host-authoritative effective speed (CM.paceV2). Lead includes self
		-- (now), so a host that is itself the leader still measures the spread.
		local hi = math.max(now, fastT or now)
		local lo = math.min(now, slowT or now)
		CM.paceV2(now, hi - lo)
	else
		if not CM.lgHolding then CM.shareSpeed() end
		-- The pacer gets the PRECISE lead over the slowest peer; the barrier above
		-- keeps the coarse peerBounds value it was tuned against.
		local slowP = CM.peerSlowPrecise()
		CM.pace(now - fastT, slowP and (now - slowP) or ahead)
	end
	-- COMPLETENESS HOLD (cfg strict_barrier, default OFF).
	--
	-- The clock barrier above only keeps the instances CLOSE in time. It
	-- does not stop us simulating past a step whose command we are still
	-- missing: recovery then applies that command late, which is a real
	-- divergence wearing the costume of a save. Real lockstep refuses to
	-- advance while anything is outstanding.
	--
	-- Safe by construction: it holds through the SAME paused/pausedSince
	-- state as the clock barrier, so the existing watchdog force-releases
	-- after K.MAX_PAUSE_TICKS and a hole that can never be filled cannot
	-- freeze the game. Pausing also HELPS, since the network thread keeps
	-- running while the sim is stopped, so the resend arrives sooner. Off
	-- by default: tightening this barrier has deadlocked both games before.
	local gapN, gapAge, gapWho, gapSeq = 0, 0, nil, nil
	if CM.cfgFlag("strict_barrier", false) then
		gapN, gapAge, gapWho, gapSeq = CM.rxGaps()
	end
	-- a brief grace so ordinary UDP reordering does not stutter the game
	local holdGap = gapN > 0 and gapAge >= K.GAP_GRACE_TICKS
	CM.dashGaps = gapN
	if (ahead > K.BARRIER_AHEAD or holdGap) and not CM.paused then
		CM.paused = true
		CM.pausedSince = CM.ticks
		if holdGap then
			CM.setSpeed(0, string.format("waiting on %d missing command(s)", gapN))
			log(string.format("BARRIER hold: %d command(s) outstanding, oldest %s:%s for %d ticks -- not simulating past them",
				gapN, tostring(gapWho), tostring(gapSeq), gapAge))
		else
			CM.setSpeed(0, string.format("barrier hold, %.2f ahead of peer", ahead))
			log(string.format("BARRIER hold: %.2f ahead of the slowest peer (%.2f)", ahead, slowT))
		end
	elseif not holdGap and ahead <= K.BARRIER_AHEAD / 2 and CM.paused then
		CM.paused = false
		CM.pausedSince = nil
		CM.catchingUp = false
		CM.releaseSpeed("peer caught up")
		log(string.format("BARRIER release: %.2f ahead", ahead))
	end
end

-- A loaded save starts at speed 0. The barrier only ever calls setGameSpeed
-- when RELEASING a hold, so with nothing to release the clock would sit frozen
-- forever, and a command stamped in the future would never come due -- an
-- experiment that looks like it ran and simply reports nothing. Both peers do
-- this identically, and speed is local pacing rather than simulated state, so
-- it cannot itself cause divergence.
-- ONE SHOT, deliberately. Nudging the speed whenever it reads 0 would override
-- a pause the player pressed on purpose, and fight them every time they stopped
-- to look at something. Firing once after load gets an unattended test moving
-- without taking the speed control away for the rest of the session. The
-- barrier is unaffected: it sets speed 0 directly and is allowed to.
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
-- lands a command in a peer's PAST, which is a desync, not a delay, unless
-- strict_barrier is on to hold the sim for it. Absent = 0.6. Read HERE, after
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
-- because applyBarrier returns immediately while `not peerSeen` -- it will not
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
-- Two modes. With expect_players=N set, it waits for exactly N-1 peers, which is
-- what a fixed rig wants. Otherwise it waits for at least one peer and then for
-- the count to stop changing (K.LOADGATE_SETTLE), which handles players
-- trickling in without needing to be told how many to expect.
function CM.loadGateReady()
	if not CM.cfgFlag("load_gate", true) then return true end
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
	--   1. expect_players in the cfg -- an explicit manual override.
	--   2. players= in tpf2_bridge_ctl.txt -- the LOBBY ROSTER, written by the
	--      menu DLL, which is the only thing that actually knows. Re-read while
	--      waiting, because a player can still be joining the lobby.
	--   3. the settle heuristic, which is a guess and can only ever be wrong in
	--      one of the two directions.
	-- The cfg is deliberately NOT required: it gets overwritten by an installer
	-- run, and a wiped cfg must not change how this behaves.
	local want = CM.cfgNum("expect_players", 0)
	if want < 2 then
		local f = io.open(K.BASE .. "tpf2_bridge_ctl.txt", "r")
		if f then
			local body = f:read("*a") or ""
			f:close()
			want = tonumber(body:match("players=(%d+)")) or 0
		end
	end
	local ready
	if want > 1 then
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
	if didInitialUnpause or CM.paused then return end
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
	-- Safe despite commanding 0, for reasons the barrier's own speed-0 hold does
	-- not get for free: ensureRunning is called UNCONDITIONALLY from the update
	-- loop (right after applyBarrier), so unlike anything living inside CM.pace
	-- it always gets a chance to release. It releases on all three of: the
	-- roster filling up, K.LOADGATE_MAX_TICKS expiring, and the player taking
	-- the lever back.
	if not CM.loadGateReady() then
		if s == 0 then
			CM.lgSawZero = true         -- our pause landed; anything else now is the player
		elseif not CM.lgHeld then
			CM.lgResumeSpeed = s        -- remember ONCE: the save's own speed
			CM.lgHeld, CM.lgHeldAt, CM.lgHolding = true, CM.ticks, true
			-- Through setSpeed, NOT a raw sendCommand: setSpeed records the value
			-- so shareSpeed recognises the 0 as OURS. The raw send made shareSpeed
			-- take the gate's pause for the player's lever and broadcast
			-- LSSPEED v=0 to every joiner -- which is how three players broke:
			-- see the LSSPEED handler.
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
