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
CM.sentRing = {}       -- our seq -> encoded LSCMD line, kept until every live peer has it (CM.sentPrune)
CM.sentLo = 1          -- the lowest of our seqs still in sentRing
CM.rx = {}             -- origin letter -> receive-side tracking
CM.resendAt = {}       -- our seq -> tick we last rebroadcast it
CM.nackSent = 0
CM.nackAnswered = 0
CM.recovered = 0

function CM.recordSent(seq, line)
	CM.sentRing[seq] = line
end

-- RETAINED BY ACKNOWLEDGEMENT, NOT BY COUNT (2026-09-16). Our own commands used
-- to be kept 256 deep (K.CMD_RING): a NACK for anything older found nothing,
-- and a burst of 300 commands (a big batch buy, a long road) could have its
-- first ones asked for after they were gone. Now every heartbeat carries
-- ak=<origin>:<n>,... -- for each origin this instance hears, the seq through
-- which it holds EVERY command (its contiguous high-water; a gap it gave up
-- NACKing counts as passed). A line of ours is dropped only once every live
-- peer has reported past it. With no live peer, or a live peer that has not
-- reported about us yet, nothing is dropped. What this no longer holds, the
-- leader's history (CM.hist, below) still does: onNack falls back to it.
function CM.rxAckOf(r)
	local a = r.ack or r.firstSeq or 0
	local top = math.max(r.maxSeq or 0, r.advMax or 0)
	while a < top and (r.seen[a + 1] or (r.nackN[a + 1] or 0) >= K.NACK_MAX) do a = a + 1 end
	r.ack = a
	return a
end
-- " ak=b:120,c:77" for our heartbeat, "" before anything was heard
function CM.ackReport()
	local parts = {}
	for o, r in pairs(CM.rx) do
		if o ~= K.INSTANCE then parts[#parts + 1] = o .. ":" .. CM.rxAckOf(r) end
	end
	if #parts == 0 then return "" end
	table.sort(parts)
	return " ak=" .. table.concat(parts, ",")
end
function CM.sentPrune()
	local floor
	for _, pr in pairs(CM.peers or {}) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			if pr.ackMine == nil then return end   -- a live peer that has said nothing about us: keep everything
			if not floor or pr.ackMine < floor then floor = pr.ackMine end
		end
	end
	if not floor then return end                 -- nobody live: nobody to have acknowledged anything
	local n = 0
	while CM.sentLo <= floor and CM.sentLo <= (CM.seqNo or 0) do
		if CM.sentRing[CM.sentLo] then CM.sentRing[CM.sentLo] = nil; n = n + 1 end
		CM.sentLo = CM.sentLo + 1
	end
	if n > 0 then CM.sentPruned = (CM.sentPruned or 0) + n end
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
		if r.stamp then r.stamp[g] = nil end
		if r.holdDone then r.holdDone[g] = nil end
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
-- COMMAND HISTORY (2026-09-09): every command that crossed this instance --
-- ours and everyone else's -- with its stamp. The Factorio shape of hot join:
-- a newcomer loads a save taken at step S and asks for everything stamped
-- after S (LSNEED); the host answers from this history. Lines are re-sent with
-- hist=1 hfor=<letter>, so nobody else pays attention.
--
-- RETAINED BY NEED, NOT BY COUNT (2026-09-16). It used to be a ring of 4,096
-- lines: a joiner whose save predated the oldest retained command got a hole
-- -- a desync at the moment it joined -- and nothing said so. What a joiner
-- can ask for is bounded by the SAVE it loads: every command stamped at or
-- before the save's step is inside the save, so only later ones are ever
-- needed. Every save handed to a joiner is at least as new as the one before
-- (a hot join takes a fresh autosave; a relay hands out its newest upload), so
-- once a joiner has loaded a save taken at S (its LSNEED ... save=1 says so,
-- and every instance hears it: the leader role can move), nothing at or
-- before S can be asked for again -- by anyone who is IN. Someone still
-- loading may hold an older save, so the prune waits until the lobby roster
-- (players= in the bridge ctl) is fully heard and nobody is catching up. Until
-- a floor is known, everything is kept and its size logged every 4,096 lines
-- (~200 B a command: an hour of busy play is about a megabyte).
CM.hist = {}          -- { at=, line=, o=, seq= } in arrival order
CM.histIdx = {}       -- "origin:seq" -> entry
CM.histBytes = 0
CM.histFloor = nil    -- the stamp of the newest save a joiner loaded
CM.histPrunedTo = nil -- the highest stamp ever pruned: a request below it cannot be served in full
function CM.histWhyKept()
	local why = CM.histHold()
	return why and (" -- kept in full: " .. why) or string.format(" -- prunable at or before %.1f", CM.histFloor or 0)
end
function CM.histPush(line, at)
	local o = line:match("origin=(%a+)")
	local seq = tonumber(line:match("seq=(%d+)"))
	local k = o and seq and (o .. ":" .. seq) or nil
	if k and CM.histIdx[k] then return end   -- a resend of a line we hold
	local e = { at = at or 0, line = line, o = o, seq = seq }
	local h = CM.hist
	h[#h + 1] = e
	if k then CM.histIdx[k] = e end
	CM.histBytes = CM.histBytes + #line
	if #h % 4096 == 0 then
		log(string.format("HIST: %d command(s) retained (%d KB)%s", #h, math.floor(CM.histBytes / 1024), CM.histWhyKept()))
	end
end
function CM.histFind(o, seq)
	local e = CM.histIdx[tostring(o) .. ":" .. tostring(seq)]
	return e and e.line or nil
end
-- LSNEED ... save=1 from L: L loaded a save taken at S. Heard by everyone.
function CM.histFloorNote(S, L)
	if CM.histFloor and S <= CM.histFloor then return end
	CM.histFloor = S
	log(string.format("HIST: %s loaded a save taken at %.1f -- every later save holds what was stamped at or before it; prunable once everyone is in", L, S))
end
-- Why the history cannot be pruned right now (nil = it can)
function CM.histHold()
	if not CM.histFloor then return "no joiner has loaded a save yet" end
	local roster = tonumber(CM.rosterPlayers)
	if not roster then return "the lobby roster size is unknown" end
	local live = CM.livePeers and CM.livePeers() or 0
	if live < roster - 1 then
		return string.format("%d of %d other roster member(s) not heard (still loading?)", roster - 1 - live, roster - 1)
	end
	for o, pr in pairs(CM.peers or {}) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS and pr.cu then return o .. " is catching up" end
	end
	if CM.histSend then return "a history feed is in flight" end
	if CM.catchingUp2 or (CM.lgFetch and CM.lgFetch ~= "done") then return "we are catching up ourselves" end
	return nil
end
function CM.histPrune()
	if CM.speedRequest then pcall(CM.speedRequest) end   -- players= and leader=, re-read every ~2 s inside
	local F = CM.histFloor
	if not F then return end
	if CM.histPrunedFor == F and (CM.ticks - (CM.histPrunedAt or 0)) < 512 then return end
	if CM.histHold() then return end
	local keep, dropped, bytes = {}, 0, 0
	for _, e in ipairs(CM.hist) do
		if e.at > F then
			keep[#keep + 1] = e; bytes = bytes + #e.line
		else
			dropped = dropped + 1
			if e.o and e.seq then CM.histIdx[e.o .. ":" .. e.seq] = nil end
		end
	end
	CM.histPrunedFor, CM.histPrunedAt = F, CM.ticks
	if dropped == 0 then return end
	CM.hist, CM.histBytes = keep, bytes
	if not CM.histPrunedTo or F > CM.histPrunedTo then CM.histPrunedTo = F end
	log(string.format("HIST: pruned %d command(s) stamped at or before %.1f (everyone is in and holds a save at least that new) -- %d retained (%d KB)",
		dropped, F, #keep, math.floor(bytes / 1024)))
end
-- LSNEED t=S o=L: gather what L is missing, tell it the per-origin ranges
-- (LSHIST), then feed the lines K.HIST_PER_TICK per tick (histPump) and close
-- with LSHISTEND. Only the host serves: it hears everything. A request for
-- history that was pruned is refused LOUDLY: what is left is sent, and the end
-- marker carries hole=<stamp> so the requester knows its world is forked.
K.HIST_PER_TICK = 40
function CM.histServe(S, L)
	if not CM.isLeader() then return end
	local lines, per = {}, {}
	for _, e in ipairs(CM.hist) do
		if e.at > S then
			lines[#lines + 1] = e.line
			local o, seq = e.o, e.seq
			if o and seq then
				local r = per[o] or { lo = seq, hi = seq }
				if seq < r.lo then r.lo = seq end
				if seq > r.hi then r.hi = seq end
				per[o] = r
			end
		end
	end
	local hole = nil
	if CM.histPrunedTo and S < CM.histPrunedTo then
		hole = CM.histPrunedTo
		log(string.format("!! HIST: %s needs every command after %.1f but everything at or before %.1f was pruned (the roster was complete and a joiner had loaded a save taken at %.1f) -- its history has a HOLE; only a resync repairs that",
			L, S, hole, CM.histFloor or hole))
	end
	for o, r in pairs(per) do
		CM.broadcast(string.format("LSHIST for=%s o=%s from=%d to=%d", L, o, r.lo, r.hi))
	end
	CM.histSend = { fr = L, lines = lines, i = 1, hole = hole }
	log(string.format("HIST: %s needs everything after %.1f -- %d command(s) from %d origin(s) queued", L, S, #lines, (function() local n = 0; for _ in pairs(per) do n = n + 1 end; return n end)()))
	if #lines == 0 then CM.broadcast(CM.histEndLine(CM.histSend)); CM.histSend = nil end
end
function CM.histEndLine(hs)
	return string.format("LSHISTEND for=%s n=%d%s", hs.fr, #hs.lines, hs.hole and string.format(" hole=%.4f", hs.hole) or "")
end
function CM.histPump()
	local hs = CM.histSend
	if not hs then return end
	local n = 0
	while hs.i <= #hs.lines and n < K.HIST_PER_TICK do
		CM.broadcast(hs.lines[hs.i] .. string.format(" hist=1 hfor=%s", hs.fr))
		hs.i = hs.i + 1; n = n + 1
	end
	if hs.i > #hs.lines then
		CM.broadcast(CM.histEndLine(hs))
		log(string.format("HIST: %d command(s) sent to %s", #hs.lines, hs.fr))
		CM.histSend = nil
	end
end

function CM.onNack(o, seq)
	if o ~= K.INSTANCE then
		-- the host also answers for OTHER origins from its history: a
		-- newcomer's gaps can be older than the originator's own ring
		if not CM.isLeader() then return end
		local line = CM.histFind(o, seq)
		if not line then return end
		local key = o .. ":" .. seq
		if CM.resendAt[key] and CM.ticks - CM.resendAt[key] < K.RESEND_MIN_GAP then return end
		CM.resendAt[key] = CM.ticks
		CM.broadcast(line)
		log(string.format("RESEND %s seq=%d from history (answering a NACK for %s)", o, seq, o))
		return
	end
	local line = CM.sentRing[seq] or CM.histFind(K.INSTANCE, seq)
	if not line then
		log(string.format("!! NACK for our seq=%d but it is no longer kept: every live peer had acknowledged past it (our ring starts at %d) and the history below %s was pruned",
			seq, CM.sentLo, CM.histPrunedTo and string.format("%.1f", CM.histPrunedTo) or "nothing"))
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
	if CM.resyncHold then return end
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
	local fastT = CM.fastestPeerClock()
	if fastT then
		lead = fastT - now
		if lead < 0 then lead = 0 end
		-- Capping this at the barrier's threshold (5 units then) was wrong: a live
		-- session was seen 9.2 units apart, and a command stamped 5.6 out then
		-- still landed in the peer's past and was applied out of step. Cap high
		-- enough to cover the gaps seen in practice; the delay is felt by the
		-- player, so it is not unbounded either.
		if lead > CM.MAX_LEAD then lead = CM.MAX_LEAD end
	end
	-- the measured delay (CM.execDelayTick), or K.EXEC_DELAY when pinned or not yet measured
	local base = CM.execDelayCur or K.EXEC_DELAY
	local delay = base + lead
	if lead > 0 then
		log(string.format("stamp: peer is %.2f ahead -- scheduling %.2f out instead of %.2f",
			lead, delay, base))
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
	CM.histPush(wire, at)
	if CM.dropNextCmd then
		-- DROPNEXT test hook (inject.lua): kept for resend, announced below, not sent
		CM.dropNextCmd = nil
		log(string.format("DROPNEXT: %s seq=%d NOT sent -- peers should hold for it and NACK", op, CM.seqNo))
	else
		CM.broadcast(wire)
	end
	-- announce it separately too (a small line, lost independently of the command):
	-- a peer that misses the LSCMD learns it exists and its stamp, and holds for it
	CM.broadcast(string.format("LSHI o=%s s=%d at=%.4f", K.INSTANCE, CM.seqNo, at))
	CM.lastSchedAt = at
	log(string.format("SCHED %s seq=%d at=%.4f (now=%.4f)", op, CM.seqNo, at, now))
end

-- Compare our hash against the peer's for one stamp, whichever arrived last.
-- Declared ABOVE onLine because it is called from there: a local declared later
-- resolves to a nil global at the call site, which is how an entire sweep in
-- mpbridge silently aborted for hours (see the lastReplayTick note there).
-- Every desync is counted here, so the first one of a game can be reported
-- (the dash file carries it to the GUI state's popup, desyncreport.lua).
function CM.noteDesync(why, stamp)
	CM.desyncs = CM.desyncs + 1
	if not CM.firstDesync then CM.firstDesync = { why = tostring(why), t = stamp } end
end

CM.comparedAt = {}
-- Which lanes differ between two detail strings, logged one per lane. Used for
-- EVERY mismatch, not only the third: the first two after a hot join were
-- blind, and they are exactly the ones that show where a join goes wrong
-- (2026-09-15: a divergence at the first stamp after a join stayed invisible
-- until it had compounded through town growth into a declared desync).
-- Returns the verdict lanes (t is counted separately, see the town streak).
local function logLaneDiff(dm, dt)
	local diffLanes = {}
	for comp in dm:gmatch("[^,]+") do
		local name = comp:match("^(%a+)")
		local other = dt:match("(" .. name .. "[^,]*)")
		if other and other ~= comp then
			if name ~= "t" then diffLanes[#diffLanes + 1] = name end
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
	return diffLanes
end

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
								CM.dashVerdict = string.format("DESYNC town %+d vs %s", d, origin)
								CM.noteDesync(CM.dashVerdict, stamp)
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
			local dm = CM.myDetails[stamp]
			if dm and dt then log("   mine " .. dm); log("   peer " .. dt); logLaneDiff(dm, dt) end
			return
		end
		CM.noteDesync("DESYNC vs " .. tostring(origin), stamp)
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
			local diffLanes = logLaneDiff(dm, dt)
			CM.dashVerdict = "DESYNC " .. (#diffLanes > 0 and table.concat(diffLanes, "+") or "?") .. " vs " .. tostring(origin)
			if CM.firstDesync and CM.firstDesync.t == stamp then CM.firstDesync.why = CM.dashVerdict end
		end
	end
end

-- Our own sim rate in game units per wall second, sampled from update().
function CM.sampleSimRate()
	local now = CM.gameTime and CM.gameTime()
	if not now then return end
	local clk = os.clock()
	if not CM.rateClk then CM.rateClk, CM.rateT = clk, now; return end
	local dc = clk - CM.rateClk
	if dc >= 1.0 then
		local r = (now - CM.rateT) / dc
		if r >= 0 and r < 60 then CM.simRate = CM.simRate and (CM.simRate * 0.5 + r * 0.5) or r end
		CM.rateClk, CM.rateT = clk, now
	end
end

-- The furthest-ahead peer clock as it most likely stands NOW: its last sim
-- step (0.2 resolution, not the floored t=) moved forward by the wall time
-- since that heartbeat arrived, at our own sim rate.
function CM.projectedPeerMax()
	local best
	local rate = CM.simRate or 0
	local clk = os.clock()
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS and (pr.step or pr.time) then
			local pt = pr.step and (pr.step * K.SIM_STEP) or pr.time
			if pr.clk and rate > 0 then
				local age = clk - pr.clk
				if age > 0 and age < 5 then pt = pt + age * rate end
			end
			if not best or pt > best then best = pt end
		end
	end
	return best
end

-- The clock a command's stamp has to clear (CM.scheduleLocal): the fastest fresh
-- peer's, PROJECTED, not its last heartbeat (2026-09-10). peerBounds hands back
-- t=, a whole unit rounded DOWN, as it was when the heartbeat left. At speed 2 a
-- joiner read the leader as 0.2 ahead while it was 1.4 ahead, stamped 0.8 out,
-- and the leader applied four of its commands 1-3 steps late: two vehicles bought
-- and put on a line at different sim steps, a vehicle drift desync for everyone.
-- The peer's sim STEP moved forward by the wall time since it arrived, at our own
-- measured sim rate, plus a step of margin, covers that. The barrier still reads
-- peerBounds unchanged. nil while no peer is fresh. How far behind this clock we
-- are is also what turns the player's actions off (inject.lua CM.actionsBlockTick).
-- Not defined above scheduleLocal: tools/bridge_companion_test.py cuts the wire
-- codec out of this file as the text between encodeCmd and scheduleLocal.
function CM.fastestPeerClock()
	local _, fastT = CM.peerBounds()
	local projT = CM.projectedPeerMax and CM.projectedPeerMax() or nil
	if projT and (not fastT or projT + K.SIM_STEP > fastT) then fastT = projT + K.SIM_STEP end
	return fastT
end

-- ---------- measured round trips, the command delay, the gap hold (2026-09-11) ----------
--
-- ROUND TRIP. Every heartbeat carries ms= (our os.clock in ms) and e=, the last
-- ms= heard from each peer with how long we held it before this send. A peer's
-- echo of OUR ms therefore times the whole path both ways -- file relay, lobby,
-- network, the tick that reads it -- minus the time it sat on the other side.
-- Smoothed like TCP's RTO (RFC 6298): srtt and rttvar per peer.
--
-- FREEZES ARE NOT LATENCY (2026-09-15). An autosave freezes every game for about
-- a second, and the round trips timed across it read 640-730 +-250-320 ms against a
-- steady 392 +-3: one of them lifted the delay from 1.2 to 2.4 or 2.8 units, which
-- then took minutes to step back down (players' logs, 2026-09-13: 8 of the 12
-- biggest raises came straight after "Saving..."). A freeze on either side shows
-- here as a silence from that peer -- ours stops us reading its heartbeats, its own
-- stops them coming. A silence longer than K.RTT_FREEZE_MIN_SEC and than
-- K.RTT_FREEZE_MULT times its usual heartbeat spacing marks when it ended
-- (pr.freezeEnd, CM.heartbeatGapNote), and a round trip whose ping went out before
-- that waited it out and is not a sample. A real change of latency makes no
-- silence and still moves the delay at once.
K.RTT_FREEZE_MIN_SEC = 0.3
K.RTT_FREEZE_MULT = 2.5
K.DELAY_DOWN_MARGIN = 0.1   -- game units the lower step must clear the requirement by (CM.execDelayTick)

-- Every LSTICK from o, BEFORE its echo is read (onLine).
function CM.heartbeatGapNote(o, pr, clk)
	local prev = pr.hbClk
	pr.hbClk = clk
	if not prev then return end
	local gap = clk - prev
	if gap < 0.02 then return end   -- read in the same tick as the heartbeat before it
	if not pr.hbGap then pr.hbGap = math.min(gap, 0.5); return end
	if gap > math.max(K.RTT_FREEZE_MIN_SEC, K.RTT_FREEZE_MULT * pr.hbGap) then
		pr.freezeEnd = clk
		if gap >= 1 then
			log(string.format("RTT: %s was silent for %.1f s -- round trips that waited it out are not latency samples", tostring(o), gap))
		end
	else
		pr.hbGap = pr.hbGap * 0.9 + gap * 0.1
	end
end

function CM.rttNote(o, sentMs, heldMs)
	local r = os.clock() * 1000 - sentMs - heldMs
	if r < 0 or r > 10000 then return end
	local pr = CM.peerFor(o)
	-- the ping went out before this peer's last silence ended: it waited that out (above)
	if pr.freezeEnd and sentMs / 1000 <= pr.freezeEnd then
		pr.rttSkipped = (pr.rttSkipped or 0) + 1
		return
	end
	if not pr.srtt then
		pr.srtt, pr.rttvar = r, r / 2
	else
		pr.rttvar = 0.75 * pr.rttvar + 0.25 * math.abs(pr.srtt - r)
		pr.srtt = 0.875 * pr.srtt + 0.125 * r
	end
	pr.rttN = (pr.rttN or 0) + 1
end

-- " e=b:123456:40,c:..." for our heartbeat: each fresh peer's last ms= and how long ago it arrived
function CM.heartbeatEcho()
	local parts, clk = {}, os.clock()
	for o, pr in pairs(CM.peers) do
		if pr.ms and pr.msClk and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			parts[#parts + 1] = string.format("%s:%d:%d", o, pr.ms, math.floor((clk - pr.msClk) * 1000 + 0.5))
		end
	end
	table.sort(parts)
	return #parts > 0 and (" e=" .. table.concat(parts, ",")) or ""
end

local function snapStep(u) return math.floor(u / K.SIM_STEP + 0.5) * K.SIM_STEP end

-- The delay a command we stamp now needs, in game units, before the peers'
-- clocks reach it: the worst peer's one-way latency (half its round trip plus
-- K.DELAY_DEV_MULT deviations plus K.DELAY_SLACK_MS) at the rate our sim runs. The stamp
-- still adds the fastest peer's lead on top (scheduleLocal). Once per tick.
function CM.execDelayTick()
	local clk = os.clock()
	if CM.tickClk then
		local dt = clk - CM.tickClk
		if dt > 0 and dt < 2 then CM.tickSec = CM.tickSec and (CM.tickSec * 0.9 + dt * 0.1) or dt end
	end
	CM.tickClk = clk
	if not CM.execDelayAuto then CM.execDelayCur = K.EXEC_DELAY; return end
	-- a held or paused game measures a sim rate near 0: never let that shrink the delay
	local rate = math.max(CM.simRate or 0, (CM.effSpeed or 1) * 0.9, 0.9)
	local worstMs, who
	for o, pr in pairs(CM.peers) do
		if pr.srtt and (pr.rttN or 0) >= K.RTT_MIN_SAMPLES and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			local oneway = pr.srtt / 2 + K.DELAY_DEV_MULT * (pr.rttvar or 0) + K.DELAY_SLACK_MS
			if not worstMs or oneway > worstMs then worstMs, who = oneway, o end
		end
	end
	local want, raw = K.EXEC_DELAY, nil
	if worstMs then
		raw = worstMs / 1000 * rate
		want = math.ceil(raw / K.SIM_STEP - 1e-6) * K.SIM_STEP
		if want < K.EXEC_DELAY_MIN then want = K.EXEC_DELAY_MIN end
		if want > K.EXEC_DELAY_MAX then want = K.EXEC_DELAY_MAX end
	end
	want = snapStep(want)
	local cur = snapStep(CM.execDelayCur or K.EXEC_DELAY)
	local why = worstMs and string.format("worst peer %s: round trip %d+-%d ms, one way %d ms at %.2f units/s",
		tostring(who), math.floor(CM.peers[who].srtt + 0.5), math.floor((CM.peers[who].rttvar or 0) + 0.5), math.floor(worstMs + 0.5), rate) or "nothing measured yet"
	if want > cur + 1e-6 then
		log(string.format("EXEC_DELAY auto: %.1f -> %.1f (%s)", cur, want, why))
		cur, CM.execDelayLowSince = want, nil
	-- DOWN ONLY WITH A MARGIN (2026-09-15): the step below must clear the requirement by
	-- K.DELAY_DOWN_MARGIN. A requirement sitting on a step boundary -- 249 ms one way is
	-- 0.99 units at 3.99 u/s and 1.02 at 4.10 -- otherwise stepped down and straight
	-- back up for a whole session (1.2 -> 1.0 -> 1.2, players' logs, 2026-09-13).
	elseif want < cur - 1e-6 and (not raw or raw <= cur - K.SIM_STEP - K.DELAY_DOWN_MARGIN) then
		CM.execDelayLowSince = CM.execDelayLowSince or CM.ticks
		if CM.ticks - CM.execDelayLowSince >= K.DELAY_DOWN_TICKS then
			local nxt = snapStep(math.max(want, cur - K.SIM_STEP))
			log(string.format("EXEC_DELAY auto: %.1f -> %.1f (%s)", cur, nxt, why))
			cur, CM.execDelayLowSince = nxt, CM.ticks
		end
	else
		CM.execDelayLowSince = nil
	end
	CM.execDelayCur = cur
end

-- the stamp an origin announced for one of its commands (LSHI, or ha= for hi=)
function CM.rxStampNote(o, seq, at)
	local r = CM.rx[o]
	if not r or not seq or not at then return end
	r.stamp = r.stamp or {}
	r.stamp[seq] = at
end

-- The most urgent command we know exists and do not have, if we must stop for it
-- now: {o, seq, at} (at nil when its stamp is unknown), or nil. A command whose
-- stamp is already behind us cannot be helped by stopping (it applies late); one
-- that has used up its NACKs, or that a hold already gave up on, never holds.
function CM.gapHoldNeed(now)
	local rate = math.max(CM.simRate or 0, (CM.effSpeed or 1) * 0.9, 0.9)
	local engage = K.GAP_HOLD_ENGAGE_TICKS * (CM.tickSec or 0.19) * rate + K.SIM_STEP
	local nowStep = CM.stepOf(now)
	local best
	for o, r in pairs(CM.rx) do
		if o ~= K.INSTANCE then
			local top = math.max(r.maxSeq or 0, r.advMax or 0)
			local topAt = r.stamp and r.stamp[top]
			for g = (r.firstSeq or 0) + 1, top do
				local pastGrace = CM.ticks - (r.missSince[g] or CM.ticks) >= K.GAP_HOLD_GRACE_TICKS
				if not pastGrace and not r.seen[g] then
					-- NO GRACE WHEN IT IS DUE NOW. The grace lets a reordered packet land
					-- without a stop, but the sim keeps stepping meanwhile. Relay session
					-- 2026-09-11: b's 53 and 55 arrived together on the tick a applied 53;
					-- 54 (same stamp as 53) was missing, the hold engaged a tick later at
					-- step 9281, and 54 applied one step late -- the calendar ran a step
					-- longer on a, and the worlds split. The nearest command we HOLD above
					-- the gap bounds it: due this step or the next, stop now.
					for s = g + 1, top do
						local sa = r.seen[s] and r.stamp and r.stamp[s]
						if sa then
							if CM.stepOf(sa) <= nowStep + 1 then pastGrace = true end
							break
						end
					end
				end
				if not r.seen[g] and (r.nackN[g] or 0) < K.NACK_MAX and not (r.holdDone and r.holdDone[g])
				   and pastGrace then
					local at = r.stamp and r.stamp[g]
					if at then
						if CM.stepOf(at) >= nowStep and at - now <= engage then
							if not best or not best.at or at < best.at then best = { o = o, seq = g, at = at } end
						end
					elseif not (topAt and CM.stepOf(topAt) < nowStep) then
						-- stamp unknown (the command and its LSHI both lost): stop now,
						-- unless even the newest command it precedes is already due
						if not best then best = { o = o, seq = g } end
					end
				end
			end
		end
	end
	return best
end

-- Once per tick from the pacing controller: true while this game must hold.
function CM.gapHoldTick(now)
	local need = CM.gapHoldNeed(now)
	if not need then
		if CM.gapHold then
			log(string.format("HOLD: released after %d tick(s) -- %s seq=%d arrived", CM.ticks - CM.gapHold.since,
				tostring(CM.gapHold.o), CM.gapHold.seq or -1))
			CM.gapHold = nil
		end
		return false
	end
	if not CM.gapHold then
		CM.gapHold = { since = CM.ticks }
		CM.gapHolds = (CM.gapHolds or 0) + 1
		log(string.format("HOLD: %s's command seq=%d%s has not arrived -- holding this game until it does (at most %d ticks)",
			need.o, need.seq, need.at and string.format(" (stamp %.1f, now %.1f)", need.at, now) or " (stamp unknown)", K.GAP_HOLD_MAX_TICKS))
	end
	CM.gapHold.o, CM.gapHold.seq, CM.gapHold.at = need.o, need.seq, need.at
	if CM.ticks - CM.gapHold.since > K.GAP_HOLD_MAX_TICKS then
		-- give up on everything holding us now; those commands apply late if they arrive
		local n = 0
		while need and n < 256 do
			local r = CM.rx[need.o]
			r.holdDone = r.holdDone or {}
			r.holdDone[need.seq] = true
			n = n + 1
			need = CM.gapHoldNeed(now)
		end
		log(string.format("HOLD: gave up after %d ticks on %d command(s) -- running on; they apply late if they arrive", CM.ticks - CM.gapHold.since, n))
		CM.gapHold = nil
		return false
	end
	return true
end

local function onLine(line)
	local op = line:match("^(%u+)")
	if op == "LSRESYNC" then
		if CM.resyncReceive then CM.resyncReceive(line) end
		return
	end
	if CM.resyncHold and op ~= "LSTICK" then return end
	if op == "LSTICK" then
		local t = tonumber(line:match("t=([%d%.%-]+)"))
		if t then
			local o = line:match(" o=(%a+)") or "?"
			local pr = CM.peerFor(o)
			if CM.resyncHeartbeat then CM.resyncHeartbeat(pr, line) end
			local clk = os.clock()
			-- a silence before this heartbeat voids the round trips that waited it out (CM.rttNote)
			CM.heartbeatGapNote(o, pr, clk)
			pr.time = t; pr.at = CM.ticks; pr.clk = clk   -- wall clock at arrival: stamping projects the peer forward from here
			-- LSTICK has always carried the SIM STEP as well, and nothing read
			-- it. t= is math.floor(now), so it is quantised to a whole unit --
			-- a controller cannot hold a lead tighter than its own measurement
			-- error. The step is K.SIM_STEP (0.2) resolution, 5x finer.
			--
			-- Kept in a SEPARATE field on purpose. Rewriting pr.time would move
			-- what peerBounds returns, and that value is what command stamping
			-- was tuned against -- it would silently stamp every command further
			-- out.
			local st = tonumber(line:match(" s=(%-?%d+)"))
			if st then pr.step = st end
			pr.cu = (line:find(" cu=1", 1, true) ~= nil)   -- catching up: not a pacing reference
			CM.peerSeen = true
			local hi = tonumber(line:match(" hi=(%d+)"))
			if hi then pcall(CM.rxAdvertise, o, hi) end
			local ha = tonumber(line:match(" ha=([%-%d%.]+)"))
			if hi and ha then pcall(CM.rxStampNote, o, hi, ha) end
			-- what it holds of OURS, contiguously: our sent ring keeps a line until every
			-- live peer reports past it (CM.sentPrune)
			local ak = line:match(" ak=(%S+)")
			if ak then
				for ao, an in ak:gmatch("(%a+):(%d+)") do
					if ao == K.INSTANCE then pr.ackMine = tonumber(an) end
				end
			end
			-- round trips: remember the peer's clock for our echo, and time our own echoed back
			local ms = tonumber(line:match(" ms=(%d+)"))
			if ms then pr.ms, pr.msClk = ms, os.clock() end
			-- its world hash's cost: the leader sets the hash cadence by the slowest (hash.lua)
			pr.hashMs = tonumber(line:match(" hc=(%d+)"))
			local e = line:match(" e=(%S+)")
			if e then
				for eo, sms, held in e:gmatch("(%a+):(%d+):(%d+)") do
					if eo == K.INSTANCE then pcall(CM.rttNote, o, tonumber(sms), tonumber(held)) end
				end
			end
		end
	elseif op == "LSHI" then
		-- a command an origin just issued: its seq and stamp, sent beside the LSCMD,
		-- so a lost command is known -- and when it is due -- before its stamp passes
		local o = line:match(" o=(%a+)")
		local seq = tonumber(line:match(" s=(%d+)"))
		local at = tonumber(line:match(" at=([%-%d%.]+)"))
		if o and seq and o ~= K.INSTANCE then
			pcall(CM.rxAdvertise, o, seq)
			if at then pcall(CM.rxStampNote, o, seq, at) end
		end
	elseif op == "LSCUR" then
		-- another player's cursor (cursors.lua): cosmetic, straight to the GUI's file
		pcall(CM.cursorRecv, line)
	elseif op == "LSPREVIEW" then
		pcall(CM.previewRecv, line)
	elseif op == "LSEFF" then
		-- SPEED V2: the host broadcasts the session's effective speed; joiners
		-- apply it. Not while the load gate holds (only the local lever releases
		-- us). vt= is the votes it is the mean of, for the Multiplayer window.
		CM.voteCounted = line:match(" vt=(%S+)") or ""
		if not CM.lgHolding then
			local v = tonumber(line:match(" v=([%d%.]+)"))
			if v then
				CM.effSpeed = v
				-- The host unpaused the session: a ceiling of 0 of our own is lifted
				-- (host-authoritative unpause, see CM.hostUnpause). A real pause here
				-- is re-learned from the next persistent 0 the detector sees.
				if v > 0 and CM.myCeiling == 0 then
					CM.myCeiling = v
					log(string.format("SPEED2: host unpaused the session at %g -- our ceiling of 0 lifted", v))
				end
				-- Applied by CM.paceV2 on the next tick, which turns a session pause
				-- into a sync point (run to the leader's
				-- clock, then stop) instead of freezing everyone where they are.
			end
		end
	elseif op == "LSNACK" then
		local o = line:match(" o=(%a+)")
		local seq = tonumber(line:match(" seq=(%d+)"))
		if o and seq then pcall(CM.onNack, o, seq) end
	elseif op == "LSNEED" then
		local S = tonumber(line:match(" t=([%d%.]+)"))
		local L = line:match(" o=(%a+)")
		if S and L and L ~= K.INSTANCE then
			-- save=1: L loaded a save taken at S (the load gate's request). A plain
			-- catch-up request names a live clock, not a save, and moves no floor.
			if line:find(" save=1", 1, true) then CM.histFloorNote(S, L) end
			pcall(CM.histServe, S, L)
		end
	elseif op == "LSHIST" then
		local fr = line:match(" for=(%a)")
		if fr == K.INSTANCE then
			CM.histProgressAt = CM.ticks
			local o = line:match(" o=(%a+)")
			local lo = tonumber(line:match(" from=(%d+)"))
			local hi = tonumber(line:match(" to=(%d+)"))
			if o and lo and hi and o ~= K.INSTANCE then
				-- track this origin from the first history seq: gaps in the
				-- burst are NACKed and the host answers them from its ring
				CM.rx[o] = { seen = {}, maxSeq = lo - 1, firstSeq = lo - 1, advMax = lo - 1, missSince = {}, nackAt = {}, nackN = {} }
				log(string.format("HIST: expecting %s seq %d..%d", o, lo, hi))
			end
		end
	elseif op == "LSHISTEND" then
		if line:match(" for=(%a)") == K.INSTANCE then
			CM.histProgressAt = CM.ticks
			CM.histEndSeen = true
			local hole = tonumber(line:match(" hole=([%d%.]+)"))
			if hole then
				CM.histHole = hole
				log(string.format("!! HIST: the host had pruned its history at or below %.1f -- commands stamped between our save and that never reach this game. THIS GAME IS FORKED from here; a resync is the only repair", hole))
			end
			log(string.format("HIST: end of history (%s lines announced, %d received)", tostring(line:match(" n=(%d+)")), CM.histGot or 0))
		end
	elseif op == "LSCMD" then
		local c = decodeCmd(line)
		if c and c.hist then
			if c.hfor ~= K.INSTANCE then c = nil   -- someone else's catch-up
			else CM.histProgressAt = CM.ticks; CM.histGot = (CM.histGot or 0) + 1 end
		elseif c and c.origin ~= K.INSTANCE then
			CM.histPush(line, c.at)
		end
		if c then
			-- track the origin's sequence for gap detection + resend
			if c.origin and c.seq then pcall(CM.rxNote, c.origin, c.seq) end
			-- and its stamp: a gap below a command we hold is urgent when that command is
			-- due now (CM.gapHoldNeed)
			if c.origin and c.seq and c.at and c.origin ~= K.INSTANCE then pcall(CM.rxStampNote, c.origin, c.seq, c.at) end
			-- a resent command may arrive after its stamp; it still executes
			-- (LATE) so the entity exists and the world converges
			if not c.hist and c.origin ~= K.INSTANCE and CM.rx[c.origin] and CM.rx[c.origin].nackN and CM.rx[c.origin].nackN[c.seq] then
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
				-- desync rather than a late delivery. It is the exact failure the stamp's
				-- K.EXEC_DELAY and peer-lead margin exist to prevent, so say so loudly
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
				-- spare = game time left before the stamp when it arrived here: the
				-- measured margin K.EXEC_DELAY buys (0.4 since 2026-09-11)
				log(string.format("RECV %s seq=%d at=%d from %s spare=%s", tostring(c.op), c.seq, c.at, c.origin,
					now and string.format("%.2f", c.at - now) or "?"))
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
			local o = line:match(" o=(%a+)") or "?"
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
	if not CM.resyncHold then CM.histPump() end
	if CM.ticks % 32 == 0 then pcall(CM.sentPrune) end
	if CM.ticks % 64 == 0 then pcall(CM.histPrune) end
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
