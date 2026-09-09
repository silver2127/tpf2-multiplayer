-- mp/net.lua -- command reliability (NACK + resend), encode/decode, scheduleLocal, onLine, pollEvents
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.net")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- command reliability (NACK + resend) ----------
CM.sentRing = {}       -- our seq -> encoded LSCMD line
CM.rx = {}             -- origin letter -> receive-side tracking
CM.resendAt = {}       -- our seq -> tick we last rebroadcast it
CM.nackSent = 0
CM.nackAnswered = 0
CM.recovered = 0

function CM.recordSent(seq, line)
	CM.sentRing[seq] = line
	local drop = seq - K.CMD_RING
	if drop > 0 and CM.sentRing[drop] then CM.sentRing[drop] = nil end
end

-- an LSCMD arrived from origin o with sequence number seq
function CM.rxNote(o, seq)
	if o == K.INSTANCE then return end
	local r = CM.rx[o]
	if not r then
		-- firstSeq is the earliest seq we ever hear from o: commands issued
		-- before we joined (or before this baseline) are not ours to demand.
		r = { seen = {}, maxSeq = seq, firstSeq = seq, advMax = seq, missSince = {}, nackAt = {}, nackN = {} }
		CM.rx[o] = r
	end
	r.seen[seq] = true
	r.missSince[seq] = nil; r.nackAt[seq] = nil; r.nackN[seq] = nil
	if seq > r.maxSeq then
		-- any interior seq not yet seen becomes a candidate gap, timed from now
		for g = r.maxSeq + 1, seq - 1 do
			if not r.seen[g] and not r.missSince[g] then r.missSince[g] = CM.ticks end
		end
		r.maxSeq = seq
	end
	if seq > (r.advMax or 0) then r.advMax = seq end
end

-- The origin advertises its highest command seq in every heartbeat. Without
-- this, the NACK scanner only ever fills gaps BELOW the highest seq it has
-- SEEN, so a command dropped at the END of a burst (the relay's host->joiner
-- hop is plain UDP, no resend) leaves maxSeq short and is never asked for --
-- one instance silently missed the last VBUY of a batch and ran a vehicle
-- short forever (2026-09-02). Treat the advertised high-water like received
-- seqs for gap purposes: mark every unseen seq up to it as a candidate gap.
function CM.rxAdvertise(o, hi)
	if o == K.INSTANCE or not hi then return end
	local r = CM.rx[o]
	if not r then
		-- first contact: commands issued before we could hear them are not ours
		-- to demand (we start from a transferred save), so set the baseline and
		-- backfill nothing -- exactly as rxNote does on first contact.
		CM.rx[o] = { seen = {}, maxSeq = hi, firstSeq = hi, advMax = hi, missSince = {}, nackAt = {}, nackN = {} }
		return
	end
	local top = math.max(r.maxSeq, r.advMax or r.maxSeq)
	for g = top + 1, hi do
		if not r.seen[g] and not r.missSince[g] then r.missSince[g] = CM.ticks end
	end
	if hi > (r.advMax or 0) then r.advMax = hi end
end

-- periodic: NACK gaps that have persisted past the reorder grace
-- Move firstSeq up past a contiguous run we already hold, and forget its
-- bookkeeping. Without this the scan below walked an origin's entire history
-- every 10 ticks -- cost growing with session length -- and `seen` never
-- stopped growing. A genuine gap stops the advance, so nothing still owed is
-- skipped; a seq that has exhausted its retries is stepped over too, because
-- re-asking cannot help it.
function CM.rxAdvance(r)
	if not r then return end
	local g = r.firstSeq + 1
	while r.seen[g] or (r.nackN and (r.nackN[g] or 0) >= K.NACK_MAX) do
		r.seen[g] = nil
		if r.missSince then r.missSince[g] = nil end
		if r.nackAt then r.nackAt[g] = nil end
		if r.nackN then r.nackN[g] = nil end
		r.firstSeq = g
		g = g + 1
	end
end

-- How many commands do we KNOW a peer has issued that we have not received?
--
-- This is the completeness question the whole design rests on. Every instance
-- advertises its highest command seq in its heartbeat, so a hole below that
-- high-water is a command that exists and is owed to us. Returns the number of
-- such holes still worth waiting for, the age in ticks of the oldest, and which
-- origin/seq it is (for the log). A seq that has exhausted NACK_MAX is NOT
-- counted: re-asking cannot help it, and waiting on it would freeze the game
-- forever.
function CM.rxGaps()
	local n, oldest, who, seq = 0, 0, nil, nil
	for o, r in pairs(CM.rx) do
		if o ~= K.INSTANCE then
			local top = math.max(r.maxSeq or 0, r.advMax or 0)
			for g = (r.firstSeq or 0) + 1, top do
				if not r.seen[g] and (r.nackN[g] or 0) < K.NACK_MAX then
					n = n + 1
					local age = CM.ticks - (r.missSince[g] or CM.ticks)
					if age >= oldest then oldest, who, seq = age, o, g end
				end
			end
		end
	end
	return n, oldest, who, seq
end

function CM.nackScan()
	local sent = 0
	for o, r in pairs(CM.rx) do
		if o ~= K.INSTANCE then
			CM.rxAdvance(r)
			-- up to the advertised high-water (inclusive): a dropped TAIL command
			-- sits at advMax, above maxSeq, and must be reachable here
			for g = r.firstSeq + 1, math.max(r.maxSeq, r.advMax or 0) do
				if sent >= K.NACK_PER_SCAN then break end
				if not r.seen[g] and r.missSince[g] then
					local last = r.nackAt[g] or (r.missSince[g] - K.NACK_EVERY)
					local due = (CM.ticks - r.missSince[g] >= K.NACK_GRACE) and (CM.ticks - last >= K.NACK_EVERY)
					if due and (r.nackN[g] or 0) < K.NACK_MAX then
						CM.broadcast(string.format("LSNACK o=%s seq=%d by=%s", o, g, K.INSTANCE))
						r.nackAt[g] = CM.ticks
						r.nackN[g] = (r.nackN[g] or 0) + 1
						CM.nackSent = CM.nackSent + 1
						sent = sent + 1
						if (r.nackN[g] or 0) == 1 then
							log(string.format("NACK %s seq=%d (missing, gap below %d)", o, g, r.maxSeq))
						elseif (r.nackN[g] or 0) >= K.NACK_MAX then
							log(string.format("NACK %s seq=%d GAVE UP after %d tries -- desync will stand until resync", o, g, K.NACK_MAX))
						end
					end
				end
			end
		end
	end
end

-- someone asked us (or another origin) to resend a command
function CM.onNack(o, seq)
	if o ~= K.INSTANCE then return end          -- only the origin answers
	local line = CM.sentRing[seq]
	if not line then
		log(string.format("NACK for our seq=%d but it is no longer in the ring (>%d old)", seq, K.CMD_RING))
		return
	end
	if CM.resendAt[seq] and CM.ticks - CM.resendAt[seq] < K.RESEND_MIN_GAP then return end
	CM.resendAt[seq] = CM.ticks
	CM.nackAnswered = CM.nackAnswered + 1
	CM.broadcast(line)
	log(string.format("RESEND seq=%d (answering a NACK)", seq))
end

-- Field order on the wire is FIXED (sorted), not pairs() order.
--
-- pairs() iteration order is not guaranteed and can differ between the two
-- processes even for an identical table. That would not break parsing, but it
-- makes identical commands serialise to different text, which defeats logging,
-- diffing and any future checksum over the command stream. Sort and the wire
-- form is canonical.
--
-- `params` is emitted LAST and unquoted: it is a serialised Lua table that can
-- contain spaces and '=', so it must be the greedy tail of the line. Every
-- other field is a bare token.
local function encodeCmd(c)
	local keys = {}
	for k, v in pairs(c) do
		if k ~= "op" and k ~= "at" and k ~= "origin" and k ~= "seq" and k ~= "params" then
			keys[#keys + 1] = k
		end
	end
	table.sort(keys)
	local parts = { string.format("LSCMD op=%s at=%.4f origin=%s seq=%d",
		c.op, c.at, c.origin, c.seq) }
	for _, k in ipairs(keys) do
		local v = c[k]
		if type(v) == "number" then
			parts[#parts + 1] = string.format("%s=%.4f", k, v)
		else
			parts[#parts + 1] = k .. "=" .. tostring(v)
		end
	end
	if c.params then parts[#parts + 1] = "params=" .. c.params end
	return table.concat(parts, " ")
end

local function decodeCmd(line)
	local c = {
		op     = line:match("op=(%u+)"),
		at     = tonumber(line:match("at=([%-%d%.]+)")),
		origin = line:match("origin=(%a+)"),
		seq    = tonumber(line:match("seq=(%d+)")),
	}
	if not (c.op and c.at and c.origin and c.seq) then return nil end
	-- params is the greedy tail; strip it before scanning bare key=value tokens
	local head = line
	local p = line:match("params=(.+)$")
	if p then c.params = p; head = line:gsub("%s*params=.*$", "") end
	for k, v in head:gmatch("(%w+)=([^%s]+)") do
		if c[k] == nil then
			-- A name is text even when it looks like a number: a line called
			-- "007" arrived as 7, "1e3" as 1000 (review, 2026-08-31).
			local n = (k ~= "name") and tonumber(v) or nil
			c[k] = (n ~= nil) and n or v
		end
	end
	return c
end

function CM.scheduleLocal(op, args)
	local now = CM.gameTime()
	if not now then return end
	CM.seqNo = CM.seqNo + 1
	-- No math.floor. Flooring `now` before adding the delay discarded up to a
	-- whole game-time unit -- about 1.1s of wall clock, more than the entire
	-- latency budget -- and made the actual delay vary between K.EXEC_DELAY-1 and
	-- K.EXEC_DELAY. Stamps are carried in the command, so they never needed to be
	-- integers to agree.
	-- Round to the wire precision at CREATION: encodeCmd ships at as %.4f, so
	-- without this the originator holds a full-precision stamp and the peer a
	-- rounded one -- two commands within ~1e-4 could sort differently per peer.
	-- The stamp has to be in the K.PEER's future, not just ours. K.EXEC_DELAY alone
	-- assumes the two clocks are together; when the peer is running ahead by more
	-- than the delay, our command arrives already due and it executes at once
	-- while we still wait -- the two sims then apply the same command at
	-- different game times. That is what "builds sometimes land out of order"
	-- was. Measured on a live session: skew sat at +2 to +4 units against an
	-- K.EXEC_DELAY of 0.6, so EVERY command from the trailing side landed in the
	-- leader's past. Pay the peer's lead plus a margin when there is one; when
	-- the clocks are together this is exactly K.EXEC_DELAY again.
	local lead = 0
	local _, fastT = CM.peerBounds()
	if fastT then
		lead = fastT - now
		if lead < 0 then lead = 0 end
		-- Capping this at K.BARRIER_AHEAD was wrong. The barrier is a backstop that
		-- acts only once a peer is 5 units ahead, and it takes time to bite -- a
		-- live session was seen 9.2 units apart. A command stamped 5.6 out then
		-- still lands in the peer's past and is applied out of step. Cap high
		-- enough to cover any gap the barrier tolerates in practice; the delay is
		-- felt by the player, so it is not unbounded either.
		if lead > CM.MAX_LEAD then lead = CM.MAX_LEAD end
	end
	local delay = K.EXEC_DELAY + lead
	if lead > 0 then
		log(string.format("stamp: peer is %.2f ahead -- scheduling %.2f out instead of %.2f",
			lead, delay, K.EXEC_DELAY))
	end
	-- snap to the NEXT step boundary: a stamp between two steps names no
	-- simulation state, and the lead term (an integer peer time) took stamps
	-- off the grid before this
	local rawAt = now + delay + (tonumber(args and args.delay or 0) or 0)
	local at = tonumber(string.format("%.4f", math.ceil(rawAt / K.SIM_STEP - 1e-6) * K.SIM_STEP))
	local c = { op = op, at = at, origin = K.INSTANCE, seq = CM.seqNo }
	for k, v in pairs(args) do c[k] = v end
	-- companies mode: stamp the originating company so the peer can attribute the
	-- resulting entity. No-op in coop => the wire form is unchanged there.
	CM.cmEnsure()
	if CM.cmMode == "companies" and CM.cmMyCompany then c.company = CM.cmMyCompany end
	CM.queue[#CM.queue + 1] = c
	-- The originator does NOT execute now. It queues for the same stamp as
	-- everyone else -- that is the whole point. Applying locally and shipping a
	-- copy is what the old state-diff design did, and it is why the two worlds
	-- were never actually in step.
	local wire = encodeCmd(c)
	CM.recordSent(CM.seqNo, wire)
	CM.broadcast(wire)
	log(string.format("SCHED %s seq=%d at=%.4f (now=%.4f)", op, CM.seqNo, at, now))
end

-- Compare our hash against the peer's for one stamp, whichever arrived last.
-- Declared ABOVE onLine because it is called from there: a local declared later
-- resolves to a nil global at the call site, which is how an entire sweep in
-- mpbridge silently aborted for hours (see the lastReplayTick note there).
CM.comparedAt = {}
-- One peer's hash for one stamp against ours. compareAt (below) runs this for
-- every peer that has reported the stamp, once each.
function CM.compareOne(stamp, origin, theirs, dt)
	local mine = CM.myHashes[stamp]
	if not mine or not theirs then return end
	local pr = CM.peerFor(origin)
	-- MONEY / LOAN ride in the DETAIL, not the verdict: balances can diverge with
	-- no geometry difference at all (a stop that cost the originator its native
	-- price but a peer only its cheaper edge-rebuild, a delivery timed slightly
	-- differently). The verdict then says SYNC while the wallets drift apart
	-- silently. Compare them here on EVERY stamp and log only when the GAP
	-- CHANGES, so each event that widens or closes the split is timestamped --
	-- which is what tells a stop-cost asymmetry from a vehicle-income one.
	do
		local dm = CM.myDetails[stamp]
		if dm and dt then
			CM.moneyGap = CM.moneyGap or {}
			for lane, tag in pairs({ m = "MONEY", l = "LOAN", n = "PEOPLE", t = "TOWN" }) do
				local a = dm:match(lane .. ":(%-?%d+)")
				local b = dt:match(lane .. ":(%-?%d+)")
				if a and b and a ~= "-" and b ~= "-" then
					local d = (tonumber(a) or 0) - (tonumber(b) or 0)
					local gkey = lane .. origin
					if CM.moneyGap[gkey] ~= d then
						local was = CM.moneyGap[gkey]
						CM.moneyGap[gkey] = d
						if d ~= 0 or (was ~= nil and was ~= 0) then
							log(string.format("$$ %s t=%d vs %s: %s vs %s (gap %+d, was %s)",
								tag, stamp, origin, a, b, d, was ~= nil and tostring(was) or "0"))
						end
					end
					-- TOWN BUILDINGS AS A DESYNC. The verdict hashes only PLAYER
					-- constructions; town buildings are counted (t:) but never hashed, so
					-- a placement that clears different buildings on different instances
					-- was invisible to it (A 58 vs peers 60, verdict SYNC, 2026-09-08).
					-- The count is deterministic across honest instances -- B and C have
					-- matched each other on every run today -- but a building placed
					-- exactly on a stamp boundary can differ by one for a single sample,
					-- so it counts only once the gap has PERSISTED for two compared stamps.
					if lane == "t" then
						CM.townGapStreak = CM.townGapStreak or {}
						if d ~= 0 then
							CM.townGapStreak[origin] = (CM.townGapStreak[origin] or 0) + 1
							if CM.townGapStreak[origin] == 2 then
								CM.desyncs = CM.desyncs + 1
								CM.dashVerdict = string.format("DESYNC town %+d vs %s", d, origin)
								log(string.format("!! DESYNC (town buildings) t=%d vs %s: %s vs %s (gap %+d, persisted) -- total %d",
									stamp, origin, a, b, d, CM.desyncs))
							end
						else
							CM.townGapStreak[origin] = 0
						end
					end
				end
			end
		end
	end
	if mine == theirs then
		pr.streak = 0
		pr.verdict = "SYNC"
		log(string.format("SYNC t=%d hash=%s (%s)", stamp, mine, origin))
		if not (CM.comparedAt[stamp] and CM.comparedAt[stamp].bad) then CM.dashVerdict = "SYNC" end
	else
		-- A 1-2 stamp mismatch right after a build is expected: commands
		-- execute up to ~2 units apart under real relay latency and additions
		-- self-correct. Only a mismatch that PERSISTS is a divergence.
		pr.streak = pr.streak + 1
		if pr.streak < 3 then
			log(string.format("~~ LAG t=%d vs %s (mismatch %d/3, waiting for convergence)",
				stamp, origin, pr.streak))
			return
		end
		CM.desyncs = CM.desyncs + 1
		pr.verdict = "DESYNC"
		CM.comparedAt[stamp].bad = true
		log(string.format("!! DESYNC t=%d mine=%s peer %s=%s (total %d)",
			stamp, mine, origin, theirs, CM.desyncs))
		-- WHICH component diverged. A single opaque number proves the worlds
		-- differ but says nothing about where, and the two candidate causes need
		-- opposite responses: a real divergence in the simulation is a bug in
		-- replication, whereas a difference confined to entity IDs means the
		-- worlds agree and the DETECTOR is over-sensitive. Reporting per-component
		-- counts and hashes separates them on sight.
		local dm = CM.myDetails[stamp]
		if dm and dt then
			log("   mine " .. dm)
			log("   peer " .. dt)
			local diffLanes = {}
			for comp in dm:gmatch("[^,]+") do
				local name = comp:match("^(%a+)")
				local other = dt:match("(" .. name .. "[^,]*)")
				if other and other ~= comp and name ~= "t" then diffLanes[#diffLanes + 1] = name end
			end
			CM.dashVerdict = "DESYNC " .. (#diffLanes > 0 and table.concat(diffLanes, "+") or "?") .. " vs " .. tostring(origin)
			for comp in dm:gmatch("[^,]+") do
				local name = comp:match("^(%a+)")
				local other = dt:match("(" .. name .. "[^,]*)")
				if other and other ~= comp then
					if name == "p" then
						-- vehicles: only a difference if both looked at the same sim time
						local tm, tp = comp:match("@([%-%d%.]+):"), other:match("@([%-%d%.]+):")
						if tm and tp and tm ~= tp then
							log(string.format("   -> p sampled at different sim times (%s vs %s) -- not comparable", tm, tp))
						else
							log(string.format("   -> p DIFFERS at sim time %s: %s vs %s", tostring(tm), comp, other))
						end
					else
						log(string.format("   -> %s DIFFERS: %s vs %s", name, comp, other))
					end
				end
			end
		end
	end
end

local function onLine(line)
	local op = line:match("^(%u+)")
	if op == "LSTICK" then
		local t = tonumber(line:match("t=([%d%.%-]+)"))
		if t then
			local o = line:match(" o=(%a)") or "?"
			local pr = CM.peerFor(o)
			pr.time = t; pr.at = CM.ticks
			-- LSTICK has always carried the SIM STEP as well, and nothing read
			-- it. t= is math.floor(now), so it is quantised to a whole unit --
			-- a controller cannot hold a lead tighter than its own measurement
			-- error. The step is K.SIM_STEP (0.2) resolution, 5x finer.
			--
			-- Kept in a SEPARATE field on purpose. Rewriting pr.time would move
			-- what peerBounds returns, and that value is what K.BARRIER_AHEAD
			-- and command stamping were tuned against -- it would silently
			-- relax the barrier by ~0.5 and stamp every command further out.
			local st = tonumber(line:match(" s=(%-?%d+)"))
			if st then pr.step = st end
			local ce = tonumber(line:match(" ceil=(%d+)"))
			if ce then pr.ceil = ce end
			CM.peerSeen = true
			local hi = tonumber(line:match(" hi=(%d+)"))
			if hi then pcall(CM.rxAdvertise, o, hi) end
		end
	elseif op == "LSEFF" then
		-- SPEED V2: the host broadcasts the session's effective speed; joiners
		-- apply it. Not while the load gate holds (only the local lever releases
		-- us, same rule as LSSPEED).
		if CM.cfgFlag("speed_v2", false) and not CM.lgHolding then
			local v = tonumber(line:match("v=(%d+)"))
			if v then
				CM.effSpeed = v; CM.baseSpeed = v
				-- The host unpaused the session: a ceiling of 0 of our own is lifted
				-- (host-authoritative unpause, see CM.hostUnpause). A real pause here
				-- is re-learned from the next persistent 0 the detector sees.
				if v > 0 and CM.myCeiling == 0 then
					CM.myCeiling = v
					log(string.format("SPEED2: host unpaused the session at %d -- our ceiling of 0 lifted", v))
				end
				local s0; pcall(function() s0 = game.interface.getGameSpeed() end)
				-- Not while the hard barrier holds us: we are AHEAD, and running now
				-- would only widen it. The release returns to baseSpeed (= v).
				if s0 ~= v and not CM.paused then CM.setSpeed(v, "host effective speed") end
			end
		end
	elseif op == "LSSPEED" then
		-- The other player moved the speed lever: follow. Speed is local pacing,
		-- not simulated state, so it is applied on arrival, not at a stamp.
		--
		-- NOT while the load gate holds. With three players this was the whole
		-- failure: the host's gate held at 0 (and, sent raw, that 0 was shared
		-- as if the player had chosen it), a joiner that saw everyone in hit its
		-- tick-100 "speed 0 -> 1" unpause, THAT was shared back, the held host's
		-- speed became 1, and its gate read a non-zero speed after its own 0 as
		-- the player pressing play -- releasing while the third player was still
		-- loading. Two players never showed it because the joiner is counted in
		-- before its tick 100. While we hold, only our own lever moves us.
		local v = tonumber(line:match("v=(%d+)"))
		if v and CM.lgHolding then
			if not CM.lgIgnoredSpeed then
				CM.lgIgnoredSpeed = true
				log(string.format("LOADGATE: ignoring peer speed %d while holding (only this player's lever releases us)", v))
			end
		elseif v then
			local s0
			pcall(function() s0 = game.interface.getGameSpeed() end)
			if s0 ~= v then
				CM.baseSpeed = v
				CM.catchingUp = false
				CM.lastSeenSpeed = v      -- so shareSpeed does not echo it back
				CM.setSpeed(v, "the other player set it")
			end
		end
	elseif op == "LSNACK" then
		local o = line:match(" o=(%a)")
		local seq = tonumber(line:match(" seq=(%d+)"))
		if o and seq then pcall(CM.onNack, o, seq) end
	elseif op == "LSCMD" then
		local c = decodeCmd(line)
		if c then
			-- track the origin's sequence for gap detection + resend
			if c.origin and c.seq then pcall(CM.rxNote, c.origin, c.seq) end
			-- a resent command may arrive after its stamp; it still executes
			-- (LATE) so the entity exists and the world converges
			if c.origin ~= K.INSTANCE and CM.rx[c.origin] and CM.rx[c.origin].nackN and CM.rx[c.origin].nackN[c.seq] then
				CM.recovered = CM.recovered + 1
				log(string.format("RECOVERED %s seq=%d from %s (a NACK was answered)", tostring(c.op), c.seq, c.origin))
			end
			-- our own command coming back off the wire; already queued
			if c.origin ~= K.INSTANCE then
				-- companies mode: the lobby's assignment wins over the sender's stamp
				if CM.cmOriginCompany == nil then CM.cmReadConfig() end
				local lc = CM.cmMode == "companies" and CM.cmOriginCompany and CM.cmOriginCompany[c.origin]
				if lc then
					if c.company and tonumber(c.company) ~= lc then
						CM.cmLog(string.format("CM: origin %s claimed company %s but the lobby assigned %d -- overriding", tostring(c.origin), tostring(c.company), lc))
					end
					c.company = lc
				end
				CM.queue[#CM.queue + 1] = c
				-- A command whose stamp has already passed here will execute at a
				-- DIFFERENT sim time than it did on the originator, which is a
				-- desync rather than a late delivery. It is the exact failure
				-- K.EXEC_DELAY > K.BARRIER_AHEAD exists to prevent, so say so loudly
				-- if it ever happens instead of letting it look like a mystery
				-- hash mismatch later.
				local now = CM.gameTime()
				if now and c.at < math.floor(now) then
					-- A command is meant to be applied at a GAME TIME both sides
					-- agree on. This one's moment has already passed here, so it
					-- will be applied on arrival instead: the build still appears
					-- on both machines -- which is why a session with bad skew
					-- looks like it is working -- but the two sims performed it at
					-- different points in their own histories. Everything that
					-- depends on when it happened (what a town had grown to, where
					-- a vehicle was) can differ from here on.
					CM.lateCount = CM.lateCount + 1
					log(string.format("!! LATE %s seq=%d at=%d but now=%d (%d so far) " ..
						"-- applied out of step; the worlds agree on the build, not on when",
						tostring(c.op), c.seq, c.at, math.floor(now), CM.lateCount))
				end
				log(string.format("RECV %s seq=%d at=%d from %s", tostring(c.op), c.seq, c.at, c.origin))
			end
		else
			log("undecodable command: " .. line:sub(1, 80))
		end
	elseif op == "LSVPOS" then
		pcall(CM.vposRecv, line)
	elseif op == "LSHASH" then
		local t = tonumber(line:match("t=(%-?%d+)"))
		local h = line:match("h=(%S+)")
		if t and h then
			local o = line:match(" o=(%a)") or "?"
			local pr = CM.peerFor(o)
			pr.hashes[t] = h
			pr.details[t] = line:match("d=(%S+)")
			-- Compare HERE as well as when we compute our own.
			--
			-- Doing it only at compute time silently made the detector
			-- one-directional: whichever instance runs slightly ahead always
			-- computes its hash for a stamp BEFORE the peer's arrives, finds
			-- nothing to compare, and never revisits the stamp. Measured after
			-- the first passing run -- A=0 SYNC, B=1 -- so "0 desyncs" was
			-- mostly "0 comparisons". Checking on arrival too makes it
			-- order-independent.
			CM.compareAt(t)
		end
	end
end

function CM.pollEvents()
	if not K.EVENTS_FILE then return end
	local data, newOff = CM.readFrom(K.EVENTS_FILE, CM.eventsOffset)
	CM.eventsOffset = newOff
	if not data then return end
	for line in data:gmatch("[^\r\n]+") do
		local ok, err = pcall(onLine, line)
		if not ok then log("parse error: " .. tostring(err)) end
	end
end

-- Test injection. Real UI capture needs the native hook that can cancel a local
-- command before the engine applies it; until that exists, commands enter here.
end
