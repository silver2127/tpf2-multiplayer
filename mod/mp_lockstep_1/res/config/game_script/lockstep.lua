-- MP Lockstep -- the game-script half of TpF2 Multiplayer (docs/ARCHITECTURE.md).
--
-- Replicates COMMANDS, not state. Every command carries the game time at which
-- all peers must execute it; nobody executes early, including the originator.
-- Correctness rests on the simulation being deterministic, which was measured:
-- two instances, same save, 79 vehicles, 59/59 state hashes identical across 58
-- in-game days (docs/re/GAME_LOOP_AND_UI.md).
--
-- WHY GAME TIME IS THE CLOCK
-- Wall clock is useless -- the two processes are never in step, and one may be
-- paused. A per-instance tick counter is no better: it starts at load and
-- counts frames, so the same tick number means different world states. Game
-- time is part of the simulation, both instances load it from the same save,
-- and the determinism run showed it advances identically. So "execute at game
-- time T" names the SAME sim state on every peer, which is precisely what
-- lockstep needs.
--
-- Player commands reach this script through the slice DLL's inject file
-- (captured, and cancelled natively while a session is live); what travels,
-- and how each action is replayed, is in docs/REPLICATION.md.
--
-- LAYOUT (since 2026-09-08). This file is the entry point: constants (K), the
-- shared state table (CM), the command dispatcher, the desync check and data().
-- Everything else lives in res/scripts/mp/*.lua, each a factory
--     require("mp.<name>")(CM, K, log)
-- constructed in the order the sections used to sit in this file:
--   hash       exact hashing, game time, world hash, vehicle drift metric
--   io         runtime files (append/read), instance detection, wire broadcast
--   companies  multi-company mode
--   roads      road/track replay: command order, execEdge, execPolyline
--   geom       hermite, node/edge lookup, mid-span splitting
--   cons       constructions: hybrid replication, station edits, edge demolish, capture polls
--   vehicles   vehicle identity, names/colours, vehicle commands, buy, replace
--   lines      line identity, create/update/delete
--   conx       native construction replay (CONP/CONX), CONFAIL, LOAN
--   net        command reliability (NACK+resend), encode/decode, scheduleLocal, onLine
--   stops      roadside stops and native-shape stop replay
--   inject     the inject reader (pollInject)
--   pacing     session speed, PID pacing, catch-up, load gate
-- Every symbol used across module boundaries is a field of CM (CM.ticks,
-- CM.queue, CM.execLine, ...); a file-scope local is by construction private
-- to its module. Load-time order matters only for chunk-level statements
-- (pacing's exec_delay read calls CM.cfgFlag from stops, for instance); calls
-- between modules happen at runtime, after every factory has run.

-- ---------- runtime data directory ----------
-- Every runtime file (identity, events, captures, injects, status, logs) lives
-- in ONE directory shared with the bridge and slice DLLs; native/src/datadir.h
-- is the C++ half of this contract. Two candidates:
--   1. $TPF2MP_DATADIR             (a harness pin)
--   2. $LOCALAPPDATA/tpf2mp/data/  (shipping layout: Program Files is read-only
--                                   for the game process, LOCALAPPDATA is not)
-- The FIRST candidate holding tpf2_instance.txt wins: the bridge writes that
-- file at game start, so its presence proves the DLLs settled on that dir. If
-- none has it yet, 2 is used; with no readable LOCALAPPDATA either, loading
-- stops with a readable error rather than guessing a folder.
-- The game's Lua may lack 'os' entirely, hence the pcall around every getenv.
-- No new top-level locals (the chunk sits at Lua 5.1's 200-local limit): the
-- discovery is an immediately-invoked function and its bookkeeping lives in CM
-- (CM.baseSource = which candidate won, CM.baseCandidates = every candidate
-- dir, for readers that must look where a peer may have landed).
-- Constants and the per-instance file paths live here rather than as two
-- dozen more top-level locals: Lua allows 200 per chunk, and this file has
-- already hit that ceiling once, where the mod silently fails to load.
local K = {}

local CM = {}   -- the one catch-all state table; documented at its former home below
-- Boot markers (2026-09-10): a friend's game died loading this file with
-- "Lua exception" and an EMPTY message. Each step now prints before it runs,
-- so stdout shows how far loading got, and a failure says what broke. Bytes
-- above 127 are printed as "?": a Windows path with an accented user name is
-- not valid UTF-8, and a message holding one came out as nothing at all.
print("[ls-boot] lockstep.lua: finding the data folder")
CM.bootOk, K.BASE = pcall(function()
	local function env(name)
		local ok, v = pcall(function() return os.getenv(name) end)
		if ok and type(v) == "string" and #v > 0 then return v end
		return nil
	end
	local function dir(p)
		p = p:gsub("\\", "/")
		if p:sub(-1) ~= "/" then p = p .. "/" end
		return p
	end
	local cands = {}
	local function add(source, p)
		if p then cands[#cands + 1] = { source = source, path = dir(p) } end
	end
	add("TPF2MP_DATADIR", env("TPF2MP_DATADIR"))
	local lad = env("LOCALAPPDATA")
	add("LOCALAPPDATA", lad and (lad .. "/tpf2mp/data"))
	CM.baseCandidates = {}
	for _, c in ipairs(cands) do CM.baseCandidates[#CM.baseCandidates + 1] = c.path end
	for _, c in ipairs(cands) do
		local f = io.open(c.path .. "tpf2_instance.txt", "r")
		if f then
			f:close()
			CM.baseSource = c.source .. " (identity file found)"
			return c.path
		end
	end
	-- no identity anywhere yet: the shipping folder
	local pick
	for _, c in ipairs(cands) do if c.source == "LOCALAPPDATA" then pick = c end end
	if not pick then error("LOCALAPPDATA is not readable, and TPF2MP_DATADIR (if set) holds no tpf2_instance.txt", 0) end
	CM.baseSource = pick.source .. " (no identity file yet)"
	return pick.path
end)
if not CM.bootOk then
	local msg = "Transport Fever 2 Multiplayer: finding the data folder failed: " .. (tostring(K.BASE):gsub("[\128-\255]", "?"))
	print("[ls-boot] " .. msg)
	error(msg, 0)
end
print("[ls-boot] data folder " .. (K.BASE:gsub("[\128-\255]", "?")) .. " (" .. tostring(CM.baseSource) .. ")")
K.IDENTITY_FILE = K.BASE .. "tpf2_instance.txt"

K.INSTANCE  = nil
K.PEER      = nil
-- (was: local K.CAPTURE_FILE, K.EVENTS_FILE, K.INJECT_FILE) -- fields of K now, nil until set
local guiTick = 0   -- gui-state only

-- UNITS. getGameTime().time is NOT seconds: comparing a live reading (t=55234)
-- against the M3 probe's day counter (day=27617) puts it at ~2 units per
-- in-game DAY. The first draft used 30 here thinking it meant 30 seconds; it
-- would have been 15 game days, roughly seven minutes of waiting per command,
-- with desync checks 100 days apart. Everything below is in these units.
--
-- How far ahead commands are scheduled. Must exceed worst-case delivery
-- latency, which here is a file relay measured in milliseconds -- so ~2 game
-- days is enormous margin, and still under a minute of wall clock at speed 1.
-- MEASURED: 1 game-time unit is ~1.1s of wall clock at speed 1 (300 ticks took
-- 56s and advanced 50 units), so this delay IS the felt latency of a build.
--
-- What correctness needs is K.EXEC_DELAY > the peers' ACTUAL skew, and a
-- stamp also pays the fastest peer's lead (net.lua scheduleLocal). The !! LATE
-- warning measures the real skew -- if it starts firing, actual drift exceeds
-- the margin and the delay must go up. That is a measurement, not a guess.
-- MEASURED: the game clock is FRACTIONAL, advancing in steps of exactly 0.2
-- units (~0.22s wall clock) -- so sub-second stamps are possible. A single
-- sample at load read 55234.000000 and looked integer; it was just a round
-- value from the save. Step size is what settles resolution, not one reading.
K.EXEC_DELAY = 0.4   -- two sim steps; was 0.6 until 2026-09-11 (RECV logs spare= to keep checking it)

-- The most peer lead a command's stamp will pay for: a live session was seen
-- 9.2 units apart (net.lua scheduleLocal).
CM.MAX_LEAD = 15.0

-- Heartbeats cross between instances through a FILE RELAY (B is sandboxed), so
-- they are not free. At every 2 ticks the relay fell behind and instance A was
-- reading peer times ~13 units stale while B saw A correctly -- both then paused
-- against bad data. 5 ticks is a rate the relay keeps up with.
K.HEARTBEAT_EVERY = 2     -- ticks between LSTICK broadcasts (~0.37s; was 5 -- the pacer's lead reading is only as fresh as this)

-- ~4.6s without a heartbeat = do not trust the peer's clock. Declared up here
-- because scheduleLocal consults it too, long before the pacing section.
K.PEER_STALE_TICKS = 25
K.HASH_EVERY_GAMETIME = 12 -- was 4: the hash costs ~380 ms on the sim thread (a visible freeze), so ~3x rarer (2026-09-09)
-- COST-AWARE HASH CADENCE. Measured on a 6,000-edge map: one world hash costs
-- ~400 ms, and at the base cadence that is ~10% of wall time spent inside our
-- own bookkeeping -- which is what "it feels laggy" actually was.
--
-- The interval CANNOT be tuned from each instance's own measured cost: the hash
-- stamp is floor(now / interval) * interval, so two instances with different
-- intervals produce DISJOINT stamp sets and never compare a single one. (That
-- exact failure is recorded at the checkHash call site: one SYNC verdict for a
-- whole session while a real divergence sat invisible.) So it is derived from
-- the EDGE COUNT instead, which every instance reads from the same save, and
-- bucketed coarsely so a few edges of drift cannot change the answer.
K.HASH_EDGES_PER_STEP = 2000   -- edges per extra interval step
K.HASH_EVERY_MAX_MULT = 8      -- never stretch beyond this
function CM.hashEveryFor(edges)
	local mult = math.floor((tonumber(edges) or 0) / K.HASH_EDGES_PER_STEP) + 1
	if mult > K.HASH_EVERY_MAX_MULT then mult = K.HASH_EVERY_MAX_MULT end
	return K.HASH_EVERY_GAMETIME * mult
end

-- COST-AWARE POLL CADENCE. The capture-side polls scan the whole world
-- (getEntities over every construction, every roadside stop) on a fixed tick
-- cadence, so their cost grows with the map while their frequency does not --
-- measured update() averaging 30-84 ms per tick with spikes over a second.
--
-- Unlike the hash these are LOCAL: a poll only notices what the player did HERE
-- and ships it with a stamp fixed at capture, so slowing one delays the capture
-- slightly and changes nothing about when peers apply it. Per-instance
-- adaptation is therefore safe.
CM.pollCost = {}
function CM.pollDue(name, baseEvery)
	local c = CM.pollCost[name]
	local every = baseEvery
	if c and c > 20 then
		local mult = math.floor(c / 20) + 1
		if mult > 8 then mult = 8 end
		every = baseEvery * mult
	end
	return (CM.ticks % every) == 0
end
function CM.pollTimed(name, fn)
	local t0 = os.clock()
	fn()
	local ms = (os.clock() - t0) * 1000
	-- rolling, so one slow scan does not pin the cadence open forever
	CM.pollCost[name] = ((CM.pollCost[name] or ms) * 3 + ms) / 4
end

CM.ticks        = 0
CM.eventsOffset = -1
CM.injectOffset = -1
CM.seqNo        = 0
-- EVERY PEER, keyed by its letter. The lockstep core was written for exactly
-- two players (one peer, letters a/b); this table is what makes N work. The
-- rules that used to read "the peer" now read over all of them: a command's
-- stamp clears the FASTEST, joiners pace against the leader, and a stamp is
-- SYNC only when every peer that reported agrees. A peer is "fresh" while its
-- last tick is within K.PEER_STALE_TICKS.
CM.peers = {}   -- origin -> { time=, at=, hashes={[stamp]=h}, details={[stamp]=d}, streak=n }
-- THE LEADER: the one instance that is the session clock (constant speed, the
-- LSEFF sender, the history/NACK server, the sync-save taker). It used to be
-- letter "a" by definition; a relay lobby names it in the bridge ctl
-- (leader=<letter>, the roster's host) so the role survives "a" leaving.
CM.leader = "a"
function CM.isLeader() return K.INSTANCE == (CM.leader or "a") end
-- peers heard within the stale window: 0 = we are playing alone right now
function CM.livePeers()
	local n = 0
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then n = n + 1 end
	end
	return n
end
function CM.peerFor(o)
	local pr = CM.peers[o]
	if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end
	return pr
end
-- Fastest peer by the PRECISE clock: the pause point everyone runs to. Falls
-- back to the coarse reading for a peer that has not sent a step yet (an older
-- build). A peer that is CATCHING UP (cu=1 on its heartbeat: a hot joiner
-- running through the command history at high speed) is left out: it would
-- drag everyone to its clock. It counts again the moment it drops the flag.
function CM.peerFastPrecise()
	local maxT
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS and not pr.cu then
			local t = pr.step and (pr.step * K.SIM_STEP) or pr.time
			if t and (not maxT or t > maxT) then maxT = t end
		end
	end
	return maxT
end

-- THE LEADER's precise clock (its last sim step): nil when we ARE the leader
-- or have not heard it within the stale window. Joiners pace against this and
-- nothing else (2026-09-10). The fastest peer used to be the catch-up
-- reference, so joiners chased a joiner that had overshot, and the leader --
-- with no leader of its own -- took that joiner for "the session" and held
-- itself at speed 0 to catch up with it. pr.cu is ignored: the leader never
-- catches up, and an older build's flag must not hide the session clock.
function CM.leaderPrecise()
	if CM.isLeader() then return nil end
	local pr = CM.peers[CM.leader or "a"]
	if not (pr and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS) then return nil end
	return pr.step and (pr.step * K.SIM_STEP) or pr.time
end

-- cu=1 on our heartbeat: catching up, or far behind the leader, so nobody
-- paces against our clock. Never on the leader: it IS the clock.
function CM.heartbeatCu(now)
	if CM.isLeader() then CM.farBehind = false; return false end
	local ref = CM.leaderPrecise() or CM.peerFastPrecise()
	CM.farBehind = (ref ~= nil) and (ref - now) > K.CATCHUP_MIN
	return (CM.catchingUp2 or CM.farBehind) and true or false
end

function CM.peerBounds()
	local minT, maxT
	for _, pr in pairs(CM.peers) do
		if pr.time and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS and not pr.cu then
			if not minT or pr.time < minT then minT = pr.time end
			if not maxT or pr.time > maxT then maxT = pr.time end
		end
	end
	return minT, maxT
end
-- The status line's peer clock is also the slice's evidence that somebody will
-- replay a build: SessionLive (slice_hook.cpp) cancels and replays a player's
-- action only when peer= holds a positive time, and "?" means a solo game, so the
-- engine builds natively. peerBounds leaves out peers that are catching up --
-- right for pacing, wrong here. On 2026-09-10 a joiner still flagged cu=1 after
-- its hot join made the leader's status read "peer=?" for a few seconds; a track
-- the leader drew in that window ran natively on the leader, the joiner replayed
-- it with different geometry (10 nodes apart) and the session desynced. Any fresh
-- peer counts here, catching up or not; a stale one still does not.
function CM.statusPeerT()
	if CM.slowT then return CM.slowT end
	local minT
	for _, pr in pairs(CM.peers) do
		if pr.time and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			if not minT or pr.time < minT then minT = pr.time end
		end
	end
	return minT
end
-- The line the MP panel and the slice read. A live peer shows at least 1: the
-- slice treats peer=0 as solo, and a session's first second is at t=0.
function CM.statusLine(now)
	local pt = CM.statusPeerT()
	return string.format("t=%d  peer=%s  skew=%s  desyncs=%d  late=%d  applylag=%.1f/%d of %d  queued=%d",
		math.floor(now), tostring(pt and math.max(1, math.floor(pt)) or "?"),
		pt and string.format("%+.1f", now - pt) or "?",
		CM.desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
		#CM.queue)
end
-- Letter -> 0..7, for anything that needs a per-origin namespace.
function CM.originIdx(o)
	local b = string.byte(tostring(o or "a"), 1) or 97
	return math.max(0, math.min(7, b - 97))
end
CM.peerSeen     = false
CM.lateCount    = 0   -- commands whose game-time stamp had already passed here
CM.queue        = {}         -- pending commands
local executed     = {}         -- key -> true, so a command runs at most once
local executedAge  = {}         -- insertion order, so `executed` stays bounded
local executedSeq  = 0
-- NOTHING keyed by stamp or by command may grow for the life of the session.
-- All of these were unbounded: one entry per 4-unit stamp per peer across five
-- tables, one per command in `executed`, and one per sequence number per origin
-- in the rx bookkeeping -- and CM.nackScan then walked the WHOLE sequence range
-- every 10 ticks, so the per-tick cost of the gap scan rose with everything the
-- players had ever done. The vpos lanes were already pruned to K.VPOS_KEEP; the
-- rest never got the same treatment.
K.STAMP_KEEP    = 64      -- hash/detail/compare stamps kept per side (~4 min)
K.EXECUTED_KEEP = 2048    -- applied commands remembered, for resend de-dup

-- Drop all but the newest `keep` entries of a table whose keys sort in age
-- order (a stamp, or a "<letter>:<stamp>" string).
function CM.pruneOldest(tbl, keep)
	if not tbl then return end
	local ks, n = {}, 0
	for k in pairs(tbl) do n = n + 1; ks[n] = k end
	if n <= keep then return end
	table.sort(ks)
	for i = 1, n - keep do tbl[ks[i]] = nil end
end
local lastHashAt   = nil
CM.myHashes     = {}         -- [stamp] = our own hash
CM.myDetails    = {}         -- [stamp] = our own per-component breakdown
CM.desyncs = 0

-- The in-game dashboard (guiUpdate, a separate Lua state) can only read files,
-- so notable events are harvested HERE, at the one point every message passes,
-- and written out with the status. Nothing else needs to know a dashboard exists.
CM.dashEvents = {}
CM.dashVerdict = "-"
local function dashNote(line)
	local keep = line:find("success=false", 1, true) or line:find("DESYNC", 1, true)
		or line:find("LATE", 1, true) or line:find("FAIL", 1, true) or line:find("DIVERGENCE", 1, true)
		or line:find("captured", 1, true) or line:find("EXEC ", 1, true) or line:find("PACE:", 1, true)
		or line:find("error", 1, true)
	if not keep then return end
	if line:find("SYNC t=", 1, true) and not line:find("DESYNC", 1, true) then return end
	local stamp = os.date("%H:%M:%S")
	local ev = CM.dashEvents
	ev[#ev + 1] = stamp .. "  " .. line:sub(1, 110)
	while #ev > 8 do table.remove(ev, 1) end
end
local function log(s)
	print("[ls-" .. (K.INSTANCE or "?") .. "] " .. s)
	pcall(dashNote, s)
end

-- ---------- module loading that says what broke ----------
-- Every module loads through CM.boot: a step marker first, then on a failure
-- a readable message naming the module (accented bytes shown as "?", see the
-- boot markers above). A require that hands back something other than our
-- factory is named too: another mod replacing require would look like that.
-- CM fields, not locals: this chunk sits near Lua 5.1's 200-local limit.
function CM.bootText(v)
	return (tostring(v):gsub("[\128-\255]", "?"))
end
function CM.boot(name)
	print("[ls-boot] loading " .. name)
	local ok, factory = pcall(require, name)
	if not ok then
		local msg = "Transport Fever 2 Multiplayer: require('" .. name .. "') failed: " .. CM.bootText(factory)
		print("[ls-boot] " .. msg)
		error(msg, 0)
	end
	if type(factory) ~= "function" then
		local msg = "Transport Fever 2 Multiplayer: require('" .. name .. "') returned a " .. type(factory) .. ", not the module factory (another mod may have replaced require)"
		print("[ls-boot] " .. msg)
		error(msg, 0)
	end
	local ok2, result = pcall(factory, CM, K, log)
	if not ok2 then
		local msg = "Transport Fever 2 Multiplayer: module " .. name .. " failed while loading: " .. CM.bootText(result)
		print("[ls-boot] " .. msg)
		error(msg, 0)
	end
	return result
end

-- ---------- exact hashing, game time, world hash (desync detector), vehicle drift metric ----------
-- Lives in res/scripts/mp/hash.lua (see the header there).
local hash = CM.boot("mp.hash")
local worldHash, vposPrune
CM.gameTime, worldHash, vposPrune = hash.gameTime, hash.worldHash, hash.vposPrune
-- ---------- runtime files: append/read, instance detection, wire broadcast ----------
-- Lives in res/scripts/mp/io.lua.
CM.boot("mp.io")
-- ---------- multi-company mode (opt-in; co-op is the default and is untouched) ----------
-- Lives in res/scripts/mp/companies.lua (see the header there).
CM.boot("mp.companies")

-- Forward declarations: worldHash uses these, and they are defined further
-- down. A later `local function` would create a DIFFERENT variable and this
-- reference would resolve to a nil global at call time -- the groundAt bug.
-- (forward declaration of ser, deepcopy, isPlayerConstruction moved into CM)
-- (forward declaration of scheduleLocal moved into CM)
-- JournalEntryCategory.type, MEASURED live (2026-09-01, EVAL probe: ten entries
-- of 1000*2^t booked with type t): every type adds to the balance, and type 0
-- ALSO adds to the loan. So 0 is the loan category and 6 (the constructor's
-- default) is a plain balance movement. Cost transfers used 0 until today and
-- were quietly changing both players' loans by the transferred amount.
K.JOURNAL_LOAN = 0
-- How long a replayed loan may take to land before we stop waiting for it.
-- cmBookJournal chunks at 10,000,000 and each chunk is an async command, so
-- a 30,000,000 loan is several ticks of settling.
K.LOAN_SETTLE_TICKS = 90
K.JOURNAL_TRANSFER = 6
K.STRICT_OPS = { VREV = true, VLINE = true, VSELL = true, VDEPOT = true, VREPL = true, VBUY = true, LUPDATE = true, LDELETE = true }   -- replay on the originator too, but only when ARMED=1 (the slice cancelled it)
-- CONX/CONP have no slice cancel (the construction's module params cannot be
-- read from the proposal); the originator instead deletes its native copy and
-- replays, gated by c.cancelled rather than ARMED. See execConX.
-- Command reliability. LSCMD ships as fire-and-forget UDP; a fully-dropped
-- command silently desyncs the peer that missed it (measured 2026-09-01: one
-- instance never got a VBUY and ran a truck short). Each instance keeps a ring
-- of its own recently-sent commands; a receiver that sees a gap in an origin's
-- contiguous seq numbers NACKs it and the origin rebroadcasts. Duplicates are
-- already harmless (executed[cmdKey] dedups at apply time).
-- The simulation advances in fixed steps of 0.2 game units (GameSim::Step, 5 Hz
-- at speed 1; measured 2026-08-07). A command stamp is therefore a STEP, not a
-- time: stamps are snapped to the step grid, application is decided in steps,
-- and lag is counted in whole steps. This is the unit the delivery buffer will
-- be sized in.
K.SIM_STEP = 0.2
-- Batch buys: departures must land on the SAME sim-step everywhere. A buy's
-- dependent line-assign is held until the batch's last buy plus this many
-- steps (bind latency of the async buy callback + pollVehKeys), and a VLINE
-- still waiting for its key advances in fixed steps, never by local wall/game
-- time. Both derive only from the agreed stamp, so they are identical on every
-- instance (a frame-tick count is not: ticks map to instance-specific game-time,
-- which is what drifted departures -- review, 2026-09-01).
K.BIND_GUARD_STEPS = 10      -- 2 game-units after the last buy of a batch
K.VLINE_RETRY_STEPS = 5      -- 1 game-unit per key-not-bound retry
K.LINE_MATERIALIZE_STEPS = 5 -- hold a batch's line ops/assigns this many steps after the LCREATE that makes their line (createLine binds its key async)
K.CMD_RING = 256          -- own commands kept for resend
K.NACK_GRACE = 15         -- ticks a gap must persist before NACKing (UDP reorder)
K.NACK_EVERY = 30         -- ticks between re-NACKs of the same seq
-- How close a peer's node must be to a shipped demolish endpoint to be
-- accepted as the same node, SQUARED. Nodes come from identically replayed
-- proposals so they agree to well under a centimetre; a metre is generous
-- enough to absorb float drift and still far too tight to select a
-- neighbouring junction and bulldoze the wrong road.
K.EDEMO_TOL_SQ = 1.0
K.NACK_MAX = 10           -- give up on one seq after this many NACKs
K.NACK_PER_SCAN = 12      -- cap NACKs sent per scan so a big gap does not flood
K.RESEND_MIN_GAP = 5      -- ticks: do not rebroadcast the same seq more often

-- ---------- road/track replay: command order, proposal context, execEdge, execPolyline ----------
-- Lives in res/scripts/mp/roads.lua.
CM.boot("mp.roads")
-- ---------- edge geometry: hermite, node/edge lookup, mid-span splitting (ported from mp_bridge) ----------
-- Lives in res/scripts/mp/geom.lua (see the header there).
local geom = CM.boot("mp.geom")
CM.hermitePos, CM.hermiteTangent, CM.edgeGeomT, CM.findNodeNear, CM.findEdgeContaining, CM.copyEdgeProps = geom.hermitePos, geom.hermiteTangent, geom.edgeGeomT, geom.findNodeNear, geom.findEdgeContaining, geom.copyEdgeProps

-- Diagnostic export: EVAL chunks run in the global environment and cannot
-- see this file's locals. Exposing the geometry helpers lets a probe try a
-- weld or a split on the live world without a rebuild cycle.
LS = { findNodeNear = CM.findNodeNear, findEdgeContaining = CM.findEdgeContaining,
       edgeGeomT = CM.edgeGeomT, hermitePos = CM.hermitePos, hermiteTangent = CM.hermiteTangent,
       copyEdgeProps = CM.copyEdgeProps, buildContext = CM.buildContext, groundAt = CM.groundAt }

-- Build an N-node polyline as ONE proposal.
--
-- execEdge handles the two-point case and stays untouched -- it is the path M9
-- verified, and rewriting it to be general would put that result at risk for no
-- gain. This is the captured-from-UI case: a drawn road tessellates into three
-- or more nodes, and collapsing it to first-and-last would replicate a straight
-- line where the player drew a curve. Both peers would then agree on the wrong
-- road and the hash check would PASS, which is the worst kind of failure -- a
-- green test over a visibly broken feature.
--
-- All nodes and edges go in a single proposal so the segments share nodes and
-- come out as one connected road. Emitting one command per segment would build
-- disconnected stubs, because each command mints its own placeholder nodes.
-- Replay a captured road/track as ONE proposal, resolving every endpoint by
-- POSITION.
--
-- Endpoints arrive as coordinates, never entity ids. For each one:
--   1. a node already there  -> reuse it (this is how roads connect end-on)
--   2. lands mid-span on an edge -> SPLIT that edge: remove it, re-add both
--      halves around a new node. buildProposal refuses a bare mid-span node,
--      which is exactly why junctions failed.
--   3. otherwise -> plant a new node
--
-- The split is recreated on every peer rather than shipped. A host snapping onto
-- an existing road splits nothing and captures no removal list, so there was
-- never one to send -- which is why looking for edgesToRemove in the proposal
-- found only garbage.
-- ---------- constructions: hybrid replication, station edits, edge demolish, capture polls ----------
-- Lives in res/scripts/mp/cons.lua.
CM.boot("mp.cons")
-- ---------- vehicles: cross-peer identity, names/colours, vehicle commands, buy, replace ----------
-- Lives in res/scripts/mp/vehicles.lua.
CM.boot("mp.vehicles")
-- ---------- lines: cross-peer identity, create/update/delete ----------
-- Lives in res/scripts/mp/lines.lua.
CM.boot("mp.lines")
-- ---------- constructions: native replay (CONP / CONX), CONFAIL, LOAN ----------
-- Lives in res/scripts/mp/conx.lua.
CM.boot("mp.conx")
local function execute(c)
	if c.op == "CONP" or c.op == "CONX" then CM.execConX(c)
	elseif c.op == "CONU" then CM.execConU(c)
	elseif c.op == "ROADP" then CM.execPolyline(c)
	elseif c.op == "ROAD" or c.op == "RAIL" then CM.execEdge(c)
	elseif c.op == "CON" then CM.execCon(c)
	elseif c.op == "DEMOLISH" then CM.execDemolish(c)
	elseif c.op == "EDEMO" then CM.execEdgeDemolish(c)
	elseif c.op == "CONFAIL" then CM.execConFail(c)
	elseif c.op == "VBUY" then CM.execVBuy(c)
	elseif c.op == "VREPL" then CM.execVReplace(c)
	elseif c.op == "VSELL" or c.op == "VDEPOT" or c.op == "VLINE" or c.op == "VREV" then CM.execVehCmd(c)
	elseif c.op == "STOPADD" or c.op == "STOPDEL" or c.op == "STOPREP" then CM.stopEnqueue(c)
	elseif c.op == "VNAME" then CM.execSetName(c)
	elseif c.op == "VCOLOR" then CM.execSetColor(c)
	elseif c.op == "LCREATE" or c.op == "LUPDATE" or c.op == "LDELETE" then
		-- behind any stop / construction replay still in flight: a line update
		-- that re-adds a replaced stop must find that stop already there
		if CM.conxBusy or #CM.conxQueue > 0 then
			CM.conxQueue[#CM.conxQueue + 1] = { c = c, notBefore = CM.gameTime() or 0 }
			log(string.format("%s seq=%s: a replay is in flight -- queued behind it (%d waiting)", tostring(c.op), tostring(c.seq), #CM.conxQueue))
		else
			CM.execLine(c)
		end
	elseif c.op == "LOAN" then CM.execLoan(c)
	elseif c.op == "SETDATE" or c.op == "CALSPEED" then CM.execCalendar(c)
	elseif c.op == "CMNEW" or c.op == "CMSWITCH" or c.op == "CMDEL" or c.op == "CMPW" then CM.execCompanyCmd(c)
	else log("unknown op: " .. tostring(c.op)) end
end

function CM.groundAt(x, y)
	local z = 0
	pcall(function() z = game.interface.getHeight({ x, y }) or 0 end)
	return z or 0
end

-- ---------- command reliability (NACK + resend), encode/decode, scheduleLocal, onLine, pollEvents ----------
-- Lives in res/scripts/mp/net.lua.
CM.boot("mp.net")
-- ---------- roadside stops (edge objects) and native-shape stop replay ----------
-- Lives in res/scripts/mp/stops.lua.
CM.boot("mp.stops")
-- ---------- inject reader (pollInject) ----------
-- Lives in res/scripts/mp/inject.lua.
CM.boot("mp.inject")
-- ---------- session speed, PID pacing, catch-up, load gate ----------
-- Lives in res/scripts/mp/pacing.lua.
CM.boot("mp.pacing")
-- ---------- other players' cursors as coloured ground circles (cosmetic) ----------
-- Lives in res/scripts/mp/cursors.lua.
CM.boot("mp.cursors")
-- ---------- the Multiplayer window's stats section, in words (GUI state) ----------
-- Lives in res/scripts/mp/stats.lua.
CM.boot("mp.stats")
-- ---------- desync check ----------
function CM.compareAt(stamp)
	CM.comparedAt[stamp] = CM.comparedAt[stamp] or {}
	if not CM.myHashes[stamp] then return end
	for o, pr in pairs(CM.peers) do
		local h = pr.hashes[stamp]
		if h and not CM.comparedAt[stamp][o] then
			CM.comparedAt[stamp][o] = true
			CM.compareOne(stamp, o, h, pr.details[stamp])
		end
	end
end

-- Defined here, after broadcast(): a CM function above it would bind a nil
-- global of that name (luacheck's use-before-definition class of bug).
-- called from checkHash right after the hash is broadcast
function CM.vposShip(stamp)
	local pts, st = CM.lastVposRaw or {}, CM.lastVposT or -1
	CM.vposMine[stamp] = { s = st, pts = pts }
	vposPrune(CM.vposMine, K.VPOS_KEEP)
	local m = math.max(1, math.ceil(#pts / K.VPOS_PER_PART))
	for i = 1, m do
		local seg = {}
		for j = (i - 1) * K.VPOS_PER_PART + 1, math.min(i * K.VPOS_PER_PART, #pts) do
			seg[#seg + 1] = string.format("%.1f,%.1f", pts[j][1], pts[j][2])
		end
		CM.broadcast(string.format("LSVPOS t=%d s=%.1f o=%s i=%d m=%d n=%d d=%s",
			stamp, st, K.INSTANCE, i, m, #pts, #seg > 0 and table.concat(seg, ";") or "-"))
	end
	-- a peer's parts may already be waiting
	for o in pairs(CM.vposPeer) do CM.vposCompare(stamp, o) end
end

local function checkHash(now)
	-- CM.hashEvery is set from the map size on the first hash and is the same
	-- on every instance (same save); until then the base interval applies.
	local every = CM.hashEvery or K.HASH_EVERY_GAMETIME
	local stamp = math.floor(now / every) * every
	if lastHashAt == stamp then return end
	lastHashAt = stamp
	local ph0 = os.clock()
	local h, detail = worldHash(now)
	do  -- PERF: the hash walks every vehicle, construction and edge -- the one
		-- O(world) cost the mod adds; measured, not estimated, so a big map's
		-- hitch is visible in the log as ms per stamp
		local dt = (os.clock() - ph0) * 1000
		local pf = CM.perfHash or { n = 0, sum = 0, max = 0 }
		pf.n = pf.n + 1; pf.sum = pf.sum + dt; if dt > pf.max then pf.max = dt end
		CM.perfHash = pf
	end
	CM.myHashes[stamp] = h
	CM.myDetails[stamp] = detail
	CM.pruneOldest(CM.myHashes, K.STAMP_KEEP)
	CM.pruneOldest(CM.myDetails, K.STAMP_KEEP)
	CM.pruneOldest(CM.comparedAt, K.STAMP_KEEP)
	CM.pruneOldest(CM.vposDone, K.STAMP_KEEP * 4)   -- keyed per peer per stamp
	for _, pr in pairs(CM.peers) do
		CM.pruneOldest(pr.hashes, K.STAMP_KEEP)
		CM.pruneOldest(pr.details, K.STAMP_KEEP)
	end
	CM.broadcast(string.format("LSHASH t=%d h=%s d=%s o=%s", stamp, h, detail or "-", K.INSTANCE))
	pcall(CM.vposShip, stamp)
	CM.dashLastDetail = detail
	-- verdict is set by compareAt; a fresh agreeing tick clears it there
	-- One shared comparison, used from here and from the LSHASH handler, so the
	-- check fires whichever side's hash lands second.
	CM.compareAt(stamp)
end

print("[ls-boot] all modules loaded")

function data()
	return {
		update = function()
			CM.ticks = CM.ticks + 1
			pcall(CM.sampleSimRate)
			if CM.cmVehPending or CM.cmRepairAt then pcall(CM.cmVehRecheck) end   -- companies: vehicles left to follow their lines in a switch
			if CM.ticks % 60 == 0 or not K.INSTANCE then
				if not CM.detectInstance() then return end
				-- a save's company state (load hook) is applied here, on the sim
				-- thread with the engine API up, not inside load() itself
				if CM.cmSaved and not CM.cmLive then CM.cmReadConfig() end
				-- WORLD INTEGRITY, once per load: a road edge without its
				-- TransportNetwork component is a half-built leftover of a failed
				-- proposal (the 2026-09-09 rail crossing left two); the engine
				-- asserts and dies the moment the UI touches one. Say so loudly, so
				-- a poisoned save is recognised before it spreads through the relay.
				if not CM.integrityChecked and CM.ticks >= 60 then   -- in the every-60-ticks block: "== 30" never ran
					CM.integrityChecked = true
					pcall(function()
						local bad, n = {}, 0
						local t = game.interface.getEntities({ radius = 999999 }, { type = "BASE_EDGE", includeData = false }) or {}
						for _, eid in pairs(t) do
							n = n + 1
							local ok, c = pcall(function() return api.engine.getComponent(eid, api.type.ComponentType.TRANSPORT_NETWORK) end)
							if not ok or c == nil then bad[#bad + 1] = tostring(eid) end
						end
						if #bad > 0 then
							log(string.format("!! WORLD INTEGRITY: %d of %d edges have NO TransportNetwork (%s) -- clicking near them CRASHES the game; this save is damaged, repair or roll back", #bad, n, table.concat(bad, ",")))
							pcall(dashNote, string.format("!! DAMAGED SAVE: %d broken road edge(s) %s -- clicking near them crashes; repair or roll back", #bad, table.concat(bad, ",")))
						else
							log(string.format("world integrity: %d edges, all complete", n))
						end
					end)
				end
			end
			if not K.INSTANCE then return end

			local now = CM.gameTime()
			if not now then return end
			local upd0 = os.clock()

			-- Both every tick. pollInject at every 10th tick added up to 1.9s of
			-- pure dead time before a build was even scheduled; a file stat per
			-- tick is far cheaper than that.
			CM.pollEvents()
			CM.pollInject()
			if CM.ticks % K.CON_POLL_EVERY == 0 then CM.pollNewConstructions() end
			if CM.ticks % K.CON_POLL_EVERY == 3 then CM.pollStops() end
			if CM.ticks % 15 == 7 then CM.pollLoan() end
			if CM.ticks % 10 == 5 then CM.nackScan() end
			CM.flushConPairs()
			CM.primeConstructions()
			CM.primeVehKeys()
			CM.shipParkedBuys()
			CM.pollVehKeys()
			CM.drainVehCap()
			CM.primeLineKeys()
			CM.pollLineKeys()
			if not CM.conxBusy and #CM.conxQueue > 0 then
				local nowG = CM.gameTime() or 0
				local head = CM.conxQueue[1]
				if not head.notBefore or nowG >= head.notBefore then
					table.remove(CM.conxQueue, 1)
					CM.runQueued(head.c)
				end
			end
			if CM.ticks % K.REMOVAL_POLL_EVERY == 0 then CM.pollConstructionRemovals() end
			-- Cheap: the watch list is empty unless a replay has cut a road, and
			-- each entry is looked at once, CM.SPLIT_SETTLE ticks after the cut.
			if CM.ticks % 60 == 0 and not CM.conxBusy then CM.sweepSplits() end
			if CM.ticks % K.CON_EDIT_SCAN_EVERY == 0 then CM.scanConstructionEdits() end

			if CM.ticks % K.HEARTBEAT_EVERY == 0 then
				-- far behind the session (a fresh hot joiner, load-gated or not): say so on
				-- every heartbeat, so nobody paces against a peer that must catch up
				-- (CM.heartbeatCu: measured against the LEADER, never set on the leader)
				CM.broadcast(string.format("LSTICK t=%d o=%s s=%d hi=%d%s", math.floor(now), K.INSTANCE, CM.stepOf(now), CM.seqNo,
					CM.heartbeatCu(now) and " cu=1" or ""))
			end

			CM.paceTick(now)
			CM.ensureRunning()
			pcall(CM.cursorTick)   -- other players' cursors (cursors.lua): cosmetic, never the sim

			-- Commands that asked to be tried again (a VLINE whose line has not
			-- arrived yet). They were executed once as far as the pump knows, so
			-- that mark is lifted before they go back in.
			if CM.retryQueue and #CM.retryQueue > 0 then
				for _, rc in ipairs(CM.retryQueue) do
					executed[CM.cmdKey(rc)] = nil
					CM.queue[#CM.queue + 1] = rc
				end
				CM.retryQueue = {}
			end

			-- run everything due, in the agreed order
			if #CM.queue > 0 then
				table.sort(CM.queue, CM.cmdLess)
				local keep = {}
				-- PRE-PASS: DETERMINISTIC targets for a batch. Spreading buys by a
				-- frame-tick count made departures drift: ticks map to instance-
				-- specific game-time (review, 2026-09-01). Instead every batched buy
				-- gets an explicit apply STEP derived only from its agreed stamp and
				-- its position in the agreed (at, origin, seq) order -- one step
				-- apart -- and any command stamped at or before the last such buy
				-- (the batch's line-assigns, lower in the sort) is held until that
				-- buy plus K.BIND_GUARD_STEPS. Identical on every instance, host
				-- included (its skipOrigin buys take the same targets, so its
				-- departures wait for the same step). Targets are sticky per
				-- command; a NACK resend recomputes the same value from the stamp.
				do
					local lastBuyStep = nil
					local newLineStep = {}   -- line key created THIS batch -> its LCREATE's apply step
					for _, c in ipairs(CM.queue) do
						if not executed[CM.cmdKey(c)] then
							local st = CM.stepOf(c.at)
							if c.op == "VBUY" then
								if not c.notBeforeStep then
									c.notBeforeStep = math.max(st, (lastBuyStep or (st - 1)) + 1)
								end
								if not lastBuyStep or c.notBeforeStep > lastBuyStep then lastBuyStep = c.notBeforeStep end
							else
								if lastBuyStep and not c.notBeforeStep and st <= lastBuyStep then
									c.notBeforeStep = lastBuyStep + K.BIND_GUARD_STEPS
								end
								-- A line created earlier in THIS batch is not queryable in the
								-- step it is issued: createLine materializes on a later sim step
								-- and its key binds only when the entity appears. Hold the ops and
								-- assigns that NAME that line until the create's step plus a
								-- materialize margin, so the stops land before a vehicle is
								-- assigned to it and the host (native line, already built) fires
								-- the assignment on the same step as the peers (which must build
								-- the line first). Matched only against lines born in this batch,
								-- so an assign to a pre-existing line is untouched; the retry
								-- paths are the fallback when a stall overshoots this margin.
								if c.op == "LCREATE" then
									newLineStep[tostring(c.origin) .. ":" .. tostring(c.seq)] = c.notBeforeStep or st
								else
									local dep = (c.op == "LUPDATE" or c.op == "LDELETE") and tostring(c.key)
										or (c.op == "VLINE") and tostring(c.line) or nil
									local cs = dep and newLineStep[dep]
									if cs then
										local lg = cs + K.LINE_MATERIALIZE_STEPS
										if not c.notBeforeStep or c.notBeforeStep < lg then c.notBeforeStep = lg end
									end
								end
							end
						end
					end
				end
				-- The per-tick cap below is now only WEDGE protection (never N
				-- buyVehicle in one update); ordering safety comes from the targets.
				local buysThisTick = 0
				local deferRest = false
				for _, c in ipairs(CM.queue) do
					if deferRest then
						keep[#keep + 1] = c
					elseif (c.notBeforeStep or CM.stepOf(c.at)) <= CM.stepOf(now) then
						if c.op == "VBUY" and buysThisTick >= 1 and not executed[CM.cmdKey(c)] then
							deferRest = true
							keep[#keep + 1] = c
						else
						local k = CM.cmdKey(c)
						if not executed[k] then
							executed[k] = true
							-- remember insertion order so this cannot grow for the life of
							-- the session; a resend arrives inside the NACK window, far
							-- fewer than K.EXECUTED_KEEP commands ago
							executedSeq = executedSeq + 1
							executedAge[executedSeq] = k
							if executedSeq % 256 == 0 then
								local cut = executedSeq - K.EXECUTED_KEEP
								for i = cut - 255, cut do
									local oldk = executedAge[i]
									if oldk then executed[oldk] = nil; executedAge[i] = nil end
								end
							end
							if c.op == "VBUY" then buysThisTick = buysThisTick + 1 end
							-- MEASUREMENT: how far past its stamp is a command actually
							-- issued? update() runs per frame while the clock moves in
							-- 0.2-unit sim steps, so at speed 2-3 the first frame past a
							-- stamp can be several steps late -- and differently late on
							-- each instance. Logged on every command, worst case kept in
							-- the status line (applylag=). If this is routinely > 0 the
							-- sim-step gate is justified.
							local lag = now - c.at
							local lagSteps = CM.stepOf(now) - CM.stepOf(c.at)
							if lag > (CM.applyLagMax or 0) then CM.applyLagMax = lag end
							CM.applyCount = (CM.applyCount or 0) + 1
							if lagSteps > 0 then CM.applyLate = (CM.applyLate or 0) + 1 end
							log(string.format("APPLY %s seq=%s origin=%s at=%.1f now=%.1f lag=%.1f step=%d late=%d%s",
								tostring(c.op), tostring(c.seq), tostring(c.origin), c.at, now, lag, CM.stepOf(now), lagSteps,
								c.notBeforeStep and string.format(" target=%d", c.notBeforeStep) or ""))
							execute(c)
						end
						end
					else
						keep[#keep + 1] = c
					end
				end
				CM.queue = keep
			end

			-- EVERY tick, not every 50th: checkHash itself dedupes to one hash
			-- per K.HASH_EVERY_GAMETIME stamp. Sampling on a tick modulus put each
			-- instance on its own phase of the stamp grid (A hashed t%20 in {0,8},
			-- B in {4,12}) so the stamp sets were DISJOINT: one SYNC verdict in an
			-- entire session, and a real 3-edge divergence sat invisible behind it.
			checkHash(now)
			do  -- PERF: whole per-tick script cost (file polls, queue, apply, hash check)
				local dt = (os.clock() - upd0) * 1000
				local pf = CM.perfUpd or { n = 0, sum = 0, max = 0 }
				pf.n = pf.n + 1; pf.sum = pf.sum + dt; if dt > pf.max then pf.max = dt end
				CM.perfUpd = pf
			end

			if CM.ticks % 15 == 0 then
				-- The dashboard file: one key=value per line, then the recent
				-- events. Read by guiUpdate in the GUI Lua state.
				pcall(function()
					local f = io.open(K.BASE .. "lockstep_dash_" .. K.INSTANCE .. ".txt", "w")
					if f then
						local sp = "?"
						pcall(function() sp = tostring(game.interface.getGameSpeed()) end)
						f:write(string.format("eff=%s\nspeedreq=%s\nsync=%s\npace=%s\nxfer=%s\n", CM.effSpeed and string.format("%g", CM.effSpeed) or "-",
							CM.spdReqInForce and CM.spdReq and string.format("%g", CM.spdReq) or "-", CM.syncState or "-", CM.paceInfo or "-", CM.xferInfo or "-"))
						-- companies: mine, the roster, and who plays what ("3:a,b 4:c")
						pcall(function()
							local ids, who = {}, {}
							for _, cid in ipairs(CM.cmRoster or { CM.cmMyCompany or 1 }) do
								ids[#ids + 1] = tostring(cid)
								local p = CM.cmPlayersOf(cid); if #p > 0 then who[#who + 1] = cid .. ":" .. table.concat(p, ",") end
							end
							local locked = {}
							for cid in pairs(CM.cmPw or {}) do locked[#locked + 1] = tostring(cid) end
							table.sort(locked)
							f:write(string.format("company=%s\nroster=%s\nplayed=%s\nconote=%s\ncolocked=%s\n", tostring(CM.cmMyCompany or 1), table.concat(ids, ","), table.concat(who, " "), tostring(CM.cmLastNote or ""), table.concat(locked, ",")))
						end)
						-- paused=yes: the speed lever reads 0 (a pause, the load gate, a catch-up hold)
						f:write(string.format("t=%d\npeer=%s\nskew=%s\ndesyncs=%d\nlate=%d\napplylag=%.1f\napplylate=%d\napplied=%d\nqueued=%d\npaused=%s\nspeed=%s\nverdict=%s\ndetail=%s\n",
							math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
							CM.slowT and string.format("%+.1f", now - CM.slowT) or "?",
							CM.desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
							#CM.queue, tonumber(sp) == 0 and "yes" or "no", sp, CM.dashVerdict or "-", tostring(CM.dashLastDetail or "-")))
						-- vehicle drift: worst peer's latest mean/max, plus skipped count
						local vd = "-"
						if CM.vposLast then
							local parts = {}
							for o, r in pairs(CM.vposLast) do parts[#parts + 1] = string.format("%s:%.1f/%.1fm", o, r.mean, r.max) end
							table.sort(parts)
							if #parts > 0 then vd = table.concat(parts, " ") end
						end
						f:write("vdrift=" .. vd .. "\n")
						f:write("money=" .. tostring(CM.dashMoney or "-") .. " / loan " .. tostring(CM.dashLoan or "-") .. "\n")
						-- The GUI used to decide which columns exist by which
						-- lockstep_dash_<x>.txt files it could open. That is wrong
						-- across Sandboxie: each boxed instance writes its own copy and
						-- reads through to the native dir, so the native A never saw
						-- c, and a stale file from an earlier session showed a dead b
						-- for minutes. The peer set travels HERE instead, from the
						-- LSTICK table, with a wall clock so a leftover file can be
						-- told from a live one.
						f:write("wall=" .. tostring(os.time()) .. "\n")
						f:write(string.format("nack=%d/%d recovered=%d\n", CM.nackSent or 0, CM.nackAnswered or 0, CM.recovered or 0))
						local ps = {}
						for o, pr in pairs(CM.peers) do
							if pr.time and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
								ps[#ps + 1] = string.format("%s:%d:%+.1f:%s", o, math.floor(pr.time), now - pr.time, pr.verdict or "-")
							end
						end
						table.sort(ps)
						f:write("peers=" .. (#ps > 0 and table.concat(ps, ",") or "-") .. "\n")
						for _, ev in ipairs(CM.dashEvents) do f:write("ev=" .. ev .. "\n") end
						f:close()
					end
				end)
				-- status for the in-game MP panel (guiUpdate reads it; the gui
				-- runs in a separate Lua state, so a file IS the channel --
				-- same as the whole wire)
				pcall(function()
					local f = io.open(K.BASE .. "lockstep_status_" .. K.INSTANCE .. ".txt", "w")
					if f then
						f:write(CM.statusLine(now))
						f:close()
					end
				end)
			end
			do
				local st = CM.stepOf(now)
				local d = CM.lastStepSeen and (st - CM.lastStepSeen) or 0
				CM.lastStepSeen = st
				local sp = -1
				pcall(function() sp = game.interface.getGameSpeed() or -1 end)
				CM.stepHist = CM.stepHist or {}
				local h = CM.stepHist[sp] or {}
				CM.stepHist[sp] = h
				if d > 9 then d = 9 end
				h[d] = (h[d] or 0) + 1
			end
			if CM.ticks % 300 == 0 then
				pcall(function()
					local parts = {}
					for sp, h in pairs(CM.stepHist or {}) do
						local cells, tot, skip = {}, 0, 0
						for d = 0, 9 do if h[d] then cells[#cells + 1] = d .. ":" .. h[d]; tot = tot + h[d]; if d >= 2 then skip = skip + h[d] end end end
						parts[#parts + 1] = string.format("speed %s -> steps/update {%s} skipped %.1f%%", tostring(sp), table.concat(cells, " "), tot > 0 and 100 * skip / tot or 0)
					end
					table.sort(parts)
					if #parts > 0 then log("STEPS: " .. table.concat(parts, " | ")) end
				end)
				pcall(function()
					local u, h = CM.perfUpd, CM.perfHash
					if u and u.n > 0 then
						log(string.format("PERF: update avg=%.2f ms max=%.2f ms over %d ticks | hash avg=%.1f ms max=%.1f ms over %d stamps%s",
							u.sum / u.n, u.max, u.n, h and h.n > 0 and h.sum / h.n or 0, h and h.max or 0, h and h.n or 0,
							CM.hashPartsMs and (" | last hash: " .. CM.hashPartsMs) or ""))
					end
					CM.perfUpd, CM.perfHash = nil, nil
				end)
				log(string.format("alive t=%d peer=%s queued=%d desyncs=%d",
					math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
					#CM.queue, CM.desyncs))
			end
		end,

		-- the company state rides in the save (companies.lua cmSaveState)
		save = function()
			-- called every frame in the GUI state too (engine -> GUI sync): keep it cheap, no log
			local ok, st = pcall(CM.cmSaveState)
			return { cm = ok and st or nil }
		end,
		load = function(s)
			-- also the per-frame engine -> GUI sync in the GUI state: no log here
			if type(s) == "table" and s.cm then pcall(CM.cmLoadState, s.cm) end
		end,

		-- ---------- multiplayer status panel (GUI Lua state) ----------
		guiUpdate = function()
			guiTick = guiTick + 1
			-- other players' cursors (cursors.lua): every frame, so the circles glide; ahead of
			-- the panel's own twice-a-second refresh
			if CM.cursorGuiTick then pcall(CM.cursorGuiTick) end
			if guiTick % 30 ~= 0 then return end
			pcall(function()
				-- NATIVE WIDGETS. The GUI Lua state has the game's own widget set
				-- (Window, Table, TextView, BoxLayout), so the dashboard is built
				-- from those rather than one text blob: a metrics table with a
				-- column per instance, a verdict line naming the lanes that
				-- differ, and the last few notable events harvested from the log.
				-- Everything comes from lockstep_dash_<a|b>.txt, written every
				-- 15 ticks by the game-script state.
				-- The lobby's folder, the same two candidates the menu DLL tries
				-- (resolveNetDir): %LOCALAPPDATA%\tpf2mp\netpunch, then <game>\netpunch (the CWD).
				function CM.netDir()
					if CM.netDirCached ~= nil then return CM.netDirCached or nil end
					local cands = {}
					local ok, la = pcall(os.getenv, "LOCALAPPDATA")
					if ok and la then cands[#cands + 1] = la .. "/tpf2mp/netpunch" end
					cands[#cands + 1] = "netpunch"
					for _, d in ipairs(cands) do
						local f = io.open(d .. "/lobby_out.jsonl", "r")
						if f then f:close(); CM.netDirCached = d; return d end
					end
					CM.netDirCached = false
					return nil
				end
				function CM.chatSend(text)
					local d = CM.netDir()
					if not d then return false end
					local f = io.open(d .. "/lobby_in.jsonl", "a")
					if not f then return false end
					local esc = tostring(text):gsub("\\", "\\\\"):gsub('"', '\\"')
					f:write('{"cmd":"chat","text":"' .. esc .. '"}' .. string.char(10))
					f:close()
					return true
				end
				-- Last n chat lines from lobby_out.jsonl, read incrementally from a
				-- remembered offset (the file also carries roster/transfer events
				-- and grows all session; the first read starts 16 KB from the end).
				CM.chatLines = CM.chatLines or {}
				function CM.chatTail(n)
					local d = CM.netDir()
					if not d then return CM.chatLines end
					local f = io.open(d .. "/lobby_out.jsonl", "rb")
					if not f then return CM.chatLines end
					local size = f:seek("end") or 0
					if CM.chatOff == nil or CM.chatOff > size then
						CM.chatOff = math.max(0, size - 16384)
						CM.chatLines = {}
					end
					f:seek("set", CM.chatOff)
					local chunk = f:read("*a") or ""
					f:close()
					CM.chatOff = size
					for line in chunk:gmatch("[^\n]+") do
						if line:find('"type":"chat"', 1, true) or line:find('"type": "chat"', 1, true) then
							local from = line:match('"from":%s*"([^"]*)"') or "?"
							local text = line:match('"text":%s*"(.-)",%s*"ts"') or line:match('"text":%s*"(.-)"}') or ""
							text = text:gsub('\\"', '"'):gsub("\\\\", "\\")
							-- "!..." lines are panel-to-panel notices (the host's
							-- "!hotjoin" while it saves for a newcomer), not chat:
							-- the title-menu panel shows them as its status line;
							-- in the game they were repeated on every save (2026-09-10)
							if text:sub(1, 1) == "!" then text = nil end
							if text then CM.chatLines[#CM.chatLines + 1] = from .. ": " .. text end
							while #CM.chatLines > n do table.remove(CM.chatLines, 1) end
						end
					end
					return CM.chatLines
				end
				local function readDash(inst)
					local bases = { K.BASE }
					for _, p in ipairs(CM.baseCandidates or {}) do if p ~= K.BASE then bases[#bases + 1] = p end end
					for _, base in ipairs(bases) do
						local f = io.open(base .. "lockstep_dash_" .. inst .. ".txt", "r")
						if f then
							local kv, ev = {}, {}
							for line in f:lines() do
								local k, v = line:match("^(%w+)=(.*)$")
								if k == "ev" then ev[#ev + 1] = v elseif k then kv[k] = v end
							end
							f:close()
							if next(kv) then return kv, ev end
						end
					end
					return nil
				end
				-- Which instances are in the session: ourselves, every peer our own
				-- game-script state hears LSTICKs from (the peers= line), and any
				-- other instance whose dash file on this machine is FRESH (its wall
				-- clock within 30 s of ours). Not "whatever files exist": across
				-- Sandboxie each box has its own copy of the data dir, so the native
				-- instance never sees a boxed one's file, and a stale file is a dead
				-- session (review, 2026-09-01: A showed a+b, B and C showed a+b+c).
				-- the GUI state never runs the engine-side identity detection: read
				-- the identity file here, or every instance's window thinks it is "a"
				-- (B's "new company" click went into lockstep_inject_a.txt, 2026-09-09)
				if not K.INSTANCE then pcall(CM.detectInstance) end
				local own = K.INSTANCE or "a"
				local ownKv = readDash(own)
				local ownWall = ownKv and tonumber(ownKv.wall) or nil
				local peerInfo = {}
				if ownKv and ownKv.peers and ownKv.peers ~= "-" then
					for o, pt, sk, vd in ownKv.peers:gmatch("(%a+):([%-%d]+):([%+%-%d%.]+):([^,]+)") do
						peerInfo[o] = { t = pt, skew = sk, verdict = vd }
					end
				end
				local present, fresh = {}, {}
				local letters, seenL = {}, {}
				for l in ("abcdefgh"):gmatch(".") do letters[#letters + 1] = l; seenL[l] = true end
				if not seenL[own] then letters[#letters + 1] = own; seenL[own] = true end
				for o in pairs(peerInfo) do if not seenL[o] then letters[#letters + 1] = o; seenL[o] = true end end
				for _, letter in ipairs(letters) do
					local isPresent = (letter == own) or (peerInfo[letter] ~= nil)
					local kv = (letter == own) and ownKv or readDash(letter)
					if kv then
						local w = tonumber(kv.wall)
						if letter == own or (w and ownWall and math.abs(ownWall - w) <= 30) then
							fresh[letter] = kv
							isPresent = true
						end
					end
					if isPresent then present[#present + 1] = letter end
				end
				if #present == 0 then present = { own } end
				local colsKey = table.concat(present)
				local D = CM.dash
				if D and D.win and D.colsKey ~= colsKey then
					pcall(function() D.win:setVisible(false, false) end)   -- the set of players changed: rebuild
					CM.dash = nil; D = nil
				end
				if not D or not D.win then
					D = {}
					CM.dash = D
					D.cols = present
					D.colsKey = colsKey
					D.rows = { "t", "peer", "skew", "speed", "paused", "queued", "desyncs", "late", "applylag", "applied", "vdrift", "money" }
					D.labels = { t = "game time", peer = "peer time", skew = "skew", speed = "speed", paused = "paused",
					             queued = "queued", desyncs = "desyncs", late = "late arrivals", applylag = "worst apply lag", applied = "commands applied",
					             vdrift = "vehicle drift mean/max", money = "balance / loan" }
					D.cells = {}
					D.table = api.gui.comp.Table.new(1 + #D.cols, "NONE")
					local head = { api.gui.comp.TextView.new("") }
					for _, letter in ipairs(D.cols) do head[#head + 1] = api.gui.comp.TextView.new(string.upper(letter)) end
					D.table:addRow(head)
					for _, key in ipairs(D.rows) do
						local row = { api.gui.comp.TextView.new(D.labels[key]) }
						D.cells[key] = {}
						for _, letter in ipairs(D.cols) do
							local cell = api.gui.comp.TextView.new("-")
							D.cells[key][letter] = cell
							row[#row + 1] = cell
						end
						D.table:addRow(row)
					end
					D.verdict = api.gui.comp.TextView.new("verdict: -")
					local box = api.gui.layout.BoxLayout.new("VERTICAL")
					-- Show/hide (2026-09-09): the stats table and the chat block each
					-- have a toggle; Ctrl+Shift+D still hides the whole window.
					local function toggleBtn(label, fn)
						local b = api.gui.comp.Button.new(api.gui.comp.TextView.new(label), true)
						b:onClick(fn)
						return b
					end
					CM.dashShowStats = (CM.dashShowStats == true)          -- hidden by default
					CM.dashShowChat = (CM.dashShowChat ~= false)
					CM.dashShowCompanies = (CM.dashShowCompanies == true)  -- hidden by default
					CM.dashShowSpeed = (CM.dashShowSpeed == true)           -- hidden by default
					local tog = api.gui.layout.BoxLayout.new("HORIZONTAL")
					tog:addItem(toggleBtn("  stats  ", function()
						CM.dashShowStats = not CM.dashShowStats
						pcall(function() D.statsBox:setVisible(CM.dashShowStats, false) end)
					end))
					tog:addItem(toggleBtn("  speed  ", function()
						CM.dashShowSpeed = not CM.dashShowSpeed
						pcall(function() D.speedBox:setVisible(CM.dashShowSpeed, false) end)
					end))
					tog:addItem(toggleBtn("  chat  ", function()
						CM.dashShowChat = not CM.dashShowChat
						pcall(function() D.chatBox:setVisible(CM.dashShowChat, false) end)
					end))
					tog:addItem(toggleBtn("  companies  ", function()
						CM.dashShowCompanies = not CM.dashShowCompanies
						pcall(function() D.coBox:setVisible(CM.dashShowCompanies, false) end)
					end))
					local togC = api.gui.comp.Component.new("mpToggles")
					togC:setLayout(tog)
					box:addItem(togC)
					-- ---- stats, in words (2026-09-11) ----
					-- A status line (do the worlds match; if not, what differs, since when
					-- and what to do) and one row per player (stats.lua). The raw counters
					-- that used to be the whole section stay behind "numbers".
					D.statusText = api.gui.comp.TextView.new("Checking...")
					D.ptable = api.gui.comp.Table.new(4, "NONE")
					D.ptable:addRow({ api.gui.comp.TextView.new("player   "), api.gui.comp.TextView.new("worlds   "),
						api.gui.comp.TextView.new("clock (game time)   "), api.gui.comp.TextView.new("notes") })
					D.pcells = {}
					for _, letter in ipairs(D.cols) do
						local pc = { name = api.gui.comp.TextView.new(string.upper(letter)), sync = api.gui.comp.TextView.new("-"),
							clock = api.gui.comp.TextView.new("-"), notes = api.gui.comp.TextView.new("") }
						D.pcells[letter] = pc
						D.ptable:addRow({ pc.name, pc.sync, pc.clock, pc.notes })
					end
					CM.dashShowNumbers = (CM.dashShowNumbers == true)       -- hidden by default
					local rawL = api.gui.layout.BoxLayout.new("VERTICAL")
					rawL:addItem(D.table)
					rawL:addItem(D.verdict)
					D.rawBox = api.gui.comp.Component.new("mpStatsNumbers")
					D.rawBox:setLayout(rawL)
					local statsL = api.gui.layout.BoxLayout.new("VERTICAL")
					statsL:addItem(D.statusText)
					statsL:addItem(D.ptable)
					statsL:addItem(toggleBtn("  numbers  ", function()
						CM.dashShowNumbers = not CM.dashShowNumbers
						pcall(function() D.rawBox:setVisible(CM.dashShowNumbers, false) end)
					end))
					statsL:addItem(D.rawBox)
					D.statsBox = api.gui.comp.Component.new("mpStats")
					D.statsBox:setLayout(statsL)
					box:addItem(D.statsBox)
					-- ---- session speed + lobby chat (2026-09-09) ----
					-- The lobby (netpunch) keeps running behind the game; its
					-- lobby_out.jsonl carries every chat line and lobby_in.jsonl takes
					-- commands, so the in-game chat is those two files. "/speed x" typed in
					-- the chat reaches every panel, which writes it into the bridge ctl; the
					-- host's pacer applies it and broadcasts the session speed (LSEFF). The
					-- -0.5 / +0.5 / reset / sync buttons are gone (2026-09-10), and the speed
					-- line has its own show/hide toggle next to stats, chat and companies.
					D.speedText = api.gui.comp.TextView.new("session speed: -")
					local function speedBtn(label, fn)
						local b = api.gui.comp.Button.new(api.gui.comp.TextView.new(label), true)
						b:onClick(fn)
						return b
					end
					local row = api.gui.layout.BoxLayout.new("HORIZONTAL")
					row:addItem(D.speedText)
					D.speedBox = api.gui.comp.Component.new("mpSpeedRow")
					D.speedBox:setLayout(row)
					box:addItem(D.speedBox)
					-- ---- companies (2026-09-09): switch, create, dissolve ----
					-- The GUI state cannot reach the lockstep queue, so a button
					-- appends "CMSWITCH 3" to the inject file; inject.lua schedules
					-- the command and every peer applies it on the same step.
					D.coText = api.gui.comp.TextView.new("company: -")
					D.coSel = nil
					local function coPw()
						local t = ""
						pcall(function() t = D.coPwInput and D.coPwInput:getText() or "" end)
						return (t or ""):gsub("[%c]", "")
					end
					local function coRequest(op, cid)
						local pw = coPw()
						local f = io.open(K.BASE .. "lockstep_inject_" .. (K.INSTANCE or "a") .. ".txt", "a")
						if f then f:write(op .. (cid and (" " .. cid) or "") .. (pw ~= "" and (" " .. pw) or "") .. string.char(10)); f:close() end
					end
					local function coStep(dir)
						local r = D.coRoster or {}
						if #r == 0 then return end
						local i = 1
						for k, v in ipairs(r) do if v == D.coSel then i = k end end
						i = ((i - 1 + dir) % #r) + 1
						D.coSel = r[i]
					end
					local crow = api.gui.layout.BoxLayout.new("HORIZONTAL")
					-- company colour swatches (2026-09-10): the chip colour the lobby roster shows
					-- (style classes !mpCo1..!mpCo200 in res/config/style_sheet/mp_lockstep.lua),
					-- for our company before the text and for the selected one after it
					D.coSwMine = api.gui.comp.TextView.new("  ##  ")
					D.coSwSel = api.gui.comp.TextView.new("  ##  ")
					crow:addItem(D.coSwMine)
					crow:addItem(D.coText)
					crow:addItem(D.coSwSel)
					crow:addItem(speedBtn("  <  ", function() coStep(-1) end))
					crow:addItem(speedBtn("  >  ", function() coStep(1) end))
					crow:addItem(speedBtn("  switch to it  ", function() if D.coSel then coRequest("CMSWITCH", D.coSel) end end))
					crow:addItem(speedBtn("  new company  ", function() coRequest("CMNEW") end))
					-- (CMDEL "dissolve into mine" exists in the sim but has no button: too easy to misread, 2026-09-09)
					D.coNote = api.gui.comp.TextView.new("")
					-- password: used by "new company" (locks the new one), by "switch"/"dissolve"
					-- (the attempt), and by "set password" (your own company; empty clears)
					pcall(function()
						local mk = api.gui.comp.TextInputField
						local ok1, inp = pcall(function() return mk.new() end)
						if not ok1 then inp = mk.new("") end
						D.coPwInput = inp
						pcall(function() D.coPwInput:setMinimumSize(api.gui.util.Size.new(180, 26)) end)
						pcall(function() D.coPwInput:setMaximumSize(api.gui.util.Size.new(260, 26)) end)
					end)
					local prow = api.gui.layout.BoxLayout.new("HORIZONTAL")
					prow:addItem(api.gui.comp.TextView.new("company password: "))
					if D.coPwInput then prow:addItem(D.coPwInput) end
					prow:addItem(speedBtn("  set on mine  ", function() if D.coMine then D.coHint = (coPw() ~= "" and "setting" or "clearing") .. " the password on company " .. D.coMine .. "..."; coRequest("CMPW", D.coMine) end end))
					local prowC = api.gui.comp.Component.new("mpCompanyPwRow")
					prowC:setLayout(prow)
					local crowC = api.gui.comp.Component.new("mpCompanyRow")
					crowC:setLayout(crow)
					local coL = api.gui.layout.BoxLayout.new("VERTICAL")
					coL:addItem(crowC); coL:addItem(prowC); coL:addItem(D.coNote)
					D.coBox = api.gui.comp.Component.new("mpCompanies")
					D.coBox:setLayout(coL)
					box:addItem(D.coBox)
					local chatL = api.gui.layout.BoxLayout.new("VERTICAL")
					D.chatText = api.gui.comp.TextView.new("chat: (no messages yet)")
					chatL:addItem(D.chatText)
					local okI, errI = pcall(function()
						local mk = api.gui.comp.TextInputField
						local ok1, inp = pcall(function() return mk.new() end)
						if not ok1 then inp = mk.new("") end
						D.input = inp
						pcall(function() D.input:setMinimumSize(api.gui.util.Size.new(280, 26)) end)
						pcall(function() D.input:setMaximumSize(api.gui.util.Size.new(400, 26)) end)
						D.input:onEnter(function()
							local t = D.input:getText()
							if t and #t > 0 then
								CM.chatSend(t)
								pcall(function() D.input:setText("", false) end)
							end
						end)
						local say = api.gui.layout.BoxLayout.new("HORIZONTAL")
						say:addItem(api.gui.comp.TextView.new("say: "))
						say:addItem(D.input)
						local sayC = api.gui.comp.Component.new("mpSay")
						sayC:setLayout(say)
						chatL:addItem(sayC)
					end)
					if not okI then print("[ls-gui] chat input field unavailable: " .. tostring(errI)) end
					D.chatBox = api.gui.comp.Component.new("mpChat")
					D.chatBox:setLayout(chatL)
					box:addItem(D.chatBox)
					pcall(function()
						D.statsBox:setVisible(CM.dashShowStats, false)
						D.rawBox:setVisible(CM.dashShowNumbers, false)
						D.chatBox:setVisible(CM.dashShowChat, false)
						D.coBox:setVisible(CM.dashShowCompanies, false)
						D.speedBox:setVisible(CM.dashShowSpeed, false)
					end)
					local body = api.gui.comp.Component.new("mpDashboard")
					body:setLayout(box)
					D.win = api.gui.comp.Window.new("Multiplayer", body)
					D.win:setPosition(20, 120)
				end
				-- A column with a fresh local file shows everything. A peer known only
				-- over the wire shows what we know of it: its game time, our skew to
				-- it, and our verdict against it.
				for _, key in ipairs(D.rows) do
					for _, letter in ipairs(D.cols) do
						local dd = fresh[letter]
						local v = dd and dd[key]
						if not v and peerInfo[letter] then
							if key == "t" then v = peerInfo[letter].t
							elseif key == "skew" then v = peerInfo[letter].skew
							elseif key == "peer" then v = own end
						end
						D.cells[key][letter]:setText(v or "-")
					end
				end
				local mine = fresh[own]
				-- the verdict and, per peer, our verdict against that peer
				local vs = {}
				for o, info in pairs(peerInfo) do vs[#vs + 1] = o .. " " .. tostring(info.verdict) end
				table.sort(vs)
				D.verdict:setText("verdict: " .. (mine and mine.verdict or "-") .. (#vs > 0 and ("   [" .. table.concat(vs, ", ") .. "]") or ""))
				if CM.statsInWords and D.statusText then
					local okW, errW = pcall(CM.statsInWords, D.statusText, D.pcells or {}, own, D.cols, fresh, peerInfo)
					if not okW and not D.statsErrLogged then D.statsErrLogged = true; print("[ls-gui] stats in words: " .. tostring(errW)) end
				end
				pcall(function()
					local eff = mine and tonumber(mine.eff) or nil
					D.eff = eff
					local req = mine and mine.speedreq
					local sync = mine and mine.sync
					local pace = mine and mine.pace
					local xfer = mine and mine.xfer
					-- plain words (2026-09-10): "lowest lever" and "PID 0.95x e=-2.40"
					-- meant nothing to players. The session speed is one number for
					-- everyone; a joiner then runs a little faster or slower than it
					-- to stay in step with the leader, and that is what "this game"
					-- reports.
					local paceTxt = ""
					if pace and pace ~= "-" then
						local mult, e = pace:match("^([%d%.]+)x e=([%+%-][%d%.]+)")
						e = tonumber(e)
						if mult and e then
							local how = e < -0.3 and string.format("catching up, %.1f steps behind the leader", -e)
								or e > 0.3 and string.format("easing off, %.1f steps ahead of the leader", e)
								or "in step with the leader"
							paceTxt = string.format("   |  this game: %sx  %s", mult, how)
						else
							paceTxt = "   |  this game: " .. pace
						end
					elseif CM.isLeader and CM.isLeader() then
						paceTxt = "   |  this game leads the clock"
					end
					D.speedText:setText(string.format("session speed: %s%s%s%s%s   ", eff and string.format("%gx", eff) or "-",
						(req and req ~= "-") and "  (set by /speed in chat; the host's speed buttons or /speed off take it back)" or "  (the host's speed buttons)",
						paceTxt,
						(sync and sync ~= "-") and ("  SYNC: " .. sync) or "",
						(xfer and xfer ~= "-") and ("  SAVE: " .. xfer) or ""))
					if D.coText and mine then
						local roster = {}
						for id in tostring(mine.roster or ""):gmatch("%d+") do roster[#roster + 1] = tonumber(id) end
						local played = {}
						for id, who in tostring(mine.played or ""):gmatch("(%d+):([%a,]+)") do played[tonumber(id)] = who end
						local locked = {}
						for id in tostring(mine.colocked or ""):gmatch("%d+") do locked[tonumber(id)] = true end
						D.coRoster, D.coPlayed, D.coMine, D.coLocked = roster, played, tonumber(mine.company), locked
						if not D.coSel then D.coSel = D.coMine end
						local sel = D.coSel or D.coMine
						local selWho = sel and played[sel]
						-- the swatches follow the ids (see D.coSwMine)
						local mineCls = "mpCo" .. tostring(math.max(1, math.min(200, D.coMine or 1)))
						local selCls = "mpCo" .. tostring(math.max(1, math.min(200, sel or D.coMine or 1)))
						if D.coSwMine and D.coSwMineCls ~= mineCls then D.coSwMineCls = mineCls; pcall(function() D.coSwMine:setStyleClassList({ mineCls }) end) end
						if D.coSwSel and D.coSwSelCls ~= selCls then D.coSwSelCls = selCls; pcall(function() D.coSwSel:setStyleClassList({ selCls }) end) end
						D.coText:setText(string.format("company: mine %s   |  %d in session   |  selected: %s%s%s   ",
							tostring(D.coMine or "?"), #roster, tostring(sel or "-"),
							(sel == D.coMine and " (mine)" or "") .. (sel and locked[sel] and " [password]" or ""), selWho and (" played by " .. selWho) or (sel and " (empty)" or "")))
						local note = mine.conote or ""
						if note ~= "" and note ~= D.coNoteSeen then D.coNoteSeen = note; D.coHint = nil end
						if D.coNote then D.coNote:setText("   " .. (D.coHint or note)) end
					end
					if D.chatText and (guiTick % 30) == 0 then
						local lines = CM.chatTail(8)
						if #lines > 0 then D.chatText:setText(table.concat(lines, string.char(10))) end
					end
				end)
				-- Ctrl+Shift+D (caught by the menu DLL's keyboard hook) flips a
				-- one-byte file; no file means shown.
				local shown = true
				local ff = io.open(K.BASE .. "tpf2mp_dash.txt", "r")
				if ff then
					local v = ff:read("*l"); ff:close()
					shown = (v ~= "0")
				end
				if D.shown ~= shown then
					D.shown = shown
					D.win:setVisible(shown, false)
				end
			end)
		end,
	}
end
