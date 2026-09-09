-- MP Lockstep -- prototype.
--
-- Replicates COMMANDS, not state. Every command carries the game time at which
-- all peers must execute it; nobody executes early, including the originator.
-- Correctness rests on the simulation being deterministic, which M3 measured:
-- two instances, same save, 79 vehicles, 59/59 state hashes identical across 58
-- in-game days (docs/M3_RESULTS.md).
--
-- WHY GAME TIME IS THE CLOCK
-- Wall clock is useless -- the two processes are never in step, and one may be
-- paused. A per-instance tick counter is no better: it starts at load and
-- counts frames, so the same tick number means different world states. Game
-- time is part of the simulation, both instances load it from the same save,
-- and M3 showed it advances identically. So "execute at game time T" names the
-- SAME sim state on every peer, which is precisely what lockstep needs.
--
-- WHAT THIS PROTOTYPE DOES NOT DO
-- It cannot intercept a command the player issues through the UI: the game
-- applies those immediately and the native hook that would cancel one is not
-- built yet (blocked on applyProposal's signature, see
-- docs/re/PROPOSAL_STRUCTURE.md). Commands here are injected through a file, so
-- this proves the lockstep LOOP -- schedule, exchange, barrier, execute in
-- agreed order, verify no desync -- not yet UI capture.
--
-- MUST NOT run alongside MP Bridge: that mod replicates state and the two would
-- fight over the same world.
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
--   gt         ground-truth sweeps
--   stops      roadside stops and native-shape stop replay
--   inject     the inject reader (pollInject) and BUYTEST
--   pacing     barrier, catch-up pacing, speed sharing, load gate
-- Every symbol used across module boundaries is a field of CM (CM.ticks,
-- CM.queue, CM.execLine, ...); a file-scope local is by construction private
-- to its module. Load-time order matters only for chunk-level statements
-- (K.* derived from CM.cfgFlag in stops, for instance); calls between modules
-- happen at runtime, after every factory has run.

-- ---------- runtime data directory ----------
-- Every runtime file (identity, events, captures, injects, status, logs) lives
-- in ONE directory shared with the bridge and slice DLLs; native/src/datadir.h
-- is the C++ half of this contract and resolves the same candidates in the same
-- order:
--   1. $TPF2MP_DATADIR             (the dev harness pins the old workshop out dir)
--   2. $LOCALAPPDATA/tpf2mp/data/  (shipping layout: Program Files is read-only
--                                   for the game process, LOCALAPPDATA is not)
--   3. the workshop literal        (the dev rig before the data dir existed)
-- The FIRST candidate holding tpf2_instance.txt wins: the bridge writes that
-- file at game start, so its presence proves the DLLs settled on that dir. If
-- none has it yet, prefer 2 when the environment was readable, else 3.
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
K.BASE = (function()
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
	add("workshop", "C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/3710243057/recon/m4/out/")
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
	-- no identity anywhere yet: the shipping default when the environment was
	-- readable, otherwise the workshop literal (always last in the list)
	local pick = cands[#cands]
	for _, c in ipairs(cands) do if c.source == "LOCALAPPDATA" then pick = c end end
	CM.baseSource = pick.source .. " (no identity file yet)"
	return pick.path
end)()
K.IDENTITY_FILE = K.BASE .. "tpf2_instance.txt"

K.INSTANCE  = nil
K.PEER      = nil
-- (was: local K.CAPTURE_FILE, K.EVENTS_FILE, K.INJECT_FILE) -- fields of K now, nil until set
local guiTick, statusWin, statusText = 0, nil, nil   -- gui-state only

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
-- K.EXEC_DELAY must exceed K.BARRIER_AHEAD, not trail it. A peer is allowed to run
-- up to K.BARRIER_AHEAD units ahead; if a command is stamped only K.EXEC_DELAY ahead
-- of the ORIGINATOR and the peer is further ahead than that, the stamp is
-- already in the peer's past and it executes early -- a desync, not a delay.
-- The original 4-vs-10 had that backwards.
K.EXEC_DELAY = 0.6

-- Pause if we are more than this far ahead of the peer. This is the tick
-- barrier: the sim cannot be blocked from Lua, but it can be paused, which
-- achieves the same thing -- nobody runs past a peer who has not caught up, so
-- no command can arrive too late to execute at its stamp. Must be comfortably
-- larger than K.EXEC_DELAY or the barrier fights normal scheduling.
-- MEASURED: the game clock is FRACTIONAL, advancing in steps of exactly 0.2
-- units (~0.22s wall clock) -- so sub-second stamps are possible. A single
-- sample at load read 55234.000000 and looked integer; it was just a round
-- value from the save. Step size is what settles resolution, not one reading.
--
-- CORRECTION to the rule above. Latency does NOT have to clear this threshold.
-- What correctness needs is K.EXEC_DELAY > the peers' ACTUAL skew; the barrier is
-- only a backstop against one instance stalling badly. Treating it as a latency
-- budget forced the delay up, and then tightening it to buy the delay back
-- deadlocked both games.
--
-- So: keep the barrier LOOSE enough that it almost never fires (5.0 = ~5.5s of
-- drift, which two instances on one machine do not reach in normal play), and
-- keep K.EXEC_DELAY small for latency. The !! LATE warning measures the real skew
-- -- if it starts firing, actual drift exceeds K.EXEC_DELAY and the delay must go
-- up. That is a measurement, not a guess.
K.BARRIER_AHEAD = 8.0   -- hard stop; above the micropause band (5.0) which paces first

-- The most peer lead a command's stamp will pay for. Bigger than K.BARRIER_AHEAD
-- on purpose: the barrier only starts acting AT that threshold, so real skew
-- overshoots it before coming back.
CM.MAX_LEAD = 15.0

-- Heartbeats cross between instances through a FILE RELAY (B is sandboxed), so
-- they are not free. At every 2 ticks the relay fell behind and instance A was
-- reading peer times ~13 units stale while B saw A correctly -- both then paused
-- against bad data. 5 ticks is a rate the relay keeps up with.
K.HEARTBEAT_EVERY = 2     -- ticks between LSTICK broadcasts (~0.37s; was 5 -- the pacer's lead reading is only as fresh as this)

-- ~4.6s without a heartbeat = do not trust the peer's clock. Declared up here
-- because scheduleLocal consults it too, long before the barrier section.
K.PEER_STALE_TICKS = 25
K.HASH_EVERY_GAMETIME = 4 -- ~2 game days between desync checks, on a small map
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
-- rules that used to read "the peer" now read over all of them: the barrier
-- holds against the SLOWEST, a command's stamp clears the FASTEST, the pacer
-- chases the fastest, and a stamp is SYNC only when every peer that reported
-- agrees. A peer is "fresh" while its last tick is within K.PEER_STALE_TICKS.
CM.peers = {}   -- origin -> { time=, at=, hashes={[stamp]=h}, details={[stamp]=d}, streak=n }
function CM.peerFor(o)
	local pr = CM.peers[o]
	if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end
	return pr
end
-- Slowest peer by the PRECISE clock, for pacing only. Falls back to the coarse
-- reading for a peer that has not sent a step yet (an older build).
function CM.peerSlowPrecise()
	local minT
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			local t = pr.step and (pr.step * K.SIM_STEP) or pr.time
			if t and (not minT or t < minT) then minT = t end
		end
	end
	return minT
end

-- Fastest peer by the PRECISE clock: the pause point everyone runs to.
function CM.peerFastPrecise()
	local maxT
	for _, pr in pairs(CM.peers) do
		if pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			local t = pr.step and (pr.step * K.SIM_STEP) or pr.time
			if t and (not maxT or t > maxT) then maxT = t end
		end
	end
	return maxT
end

function CM.peerBounds()
	local minT, maxT
	for _, pr in pairs(CM.peers) do
		if pr.time and pr.at and (CM.ticks - pr.at) <= K.PEER_STALE_TICKS then
			if not minT or pr.time < minT then minT = pr.time end
			if not maxT or pr.time > maxT then maxT = pr.time end
		end
	end
	return minT, maxT
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
CM.paused       = false
CM.pausedSince  = nil        -- tick the barrier engaged, for the watchdog
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
		or line:find("SPEED:", 1, true) or line:find("BARRIER", 1, true) or line:find("error", 1, true)
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

-- ---------- exact hashing, game time, world hash (desync detector), vehicle drift metric ----------
-- Lives in res/scripts/mp/hash.lua (see the header there).
local hash = require("mp.hash")(CM, K, log)
local worldHash, vposPrune
CM.gameTime, worldHash, vposPrune = hash.gameTime, hash.worldHash, hash.vposPrune
-- ---------- runtime files: append/read, instance detection, wire broadcast ----------
-- Lives in res/scripts/mp/io.lua.
require("mp.io")(CM, K, log)
-- ---------- multi-company mode (opt-in; co-op is the default and is untouched) ----------
-- Lives in res/scripts/mp/companies.lua (see the header there).
require("mp.companies")(CM, K, log)

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
K.STRICT_OPS = { VREV = true, VLINE = true, VSELL = true, VDEPOT = true, VREPL = true, VMAINT = true, LUPDATE = true, LDELETE = true }   -- replay on the originator too, but only when ARMED=1 (the slice cancelled it)
-- CONX/CONP have no slice cancel (the construction's module params cannot be
-- read from the proposal); the originator instead deletes its native copy and
-- replays, gated by K.CONX_STRICT rather than ARMED. See execConX.
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
-- Ticks a hole must persist before the completeness barrier stops the sim.
-- Long enough that ordinary UDP reordering never stutters the game, short
-- enough that we cannot simulate far past a command we are owed.
K.GAP_GRACE_TICKS = 5
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
require("mp.roads")(CM, K, log)
-- ---------- edge geometry: hermite, node/edge lookup, mid-span splitting (ported from mp_bridge) ----------
-- Lives in res/scripts/mp/geom.lua (see the header there).
local geom = require("mp.geom")(CM, K, log)
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
require("mp.cons")(CM, K, log)
-- ---------- vehicles: cross-peer identity, names/colours, vehicle commands, buy, replace ----------
-- Lives in res/scripts/mp/vehicles.lua.
require("mp.vehicles")(CM, K, log)
-- ---------- lines: cross-peer identity, create/update/delete ----------
-- Lives in res/scripts/mp/lines.lua.
require("mp.lines")(CM, K, log)
-- ---------- constructions: native replay (CONP / CONX), CONFAIL, LOAN ----------
-- Lives in res/scripts/mp/conx.lua.
require("mp.conx")(CM, K, log)
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
	elseif c.op == "VSELL" or c.op == "VDEPOT" or c.op == "VLINE" or c.op == "VREV" or c.op == "VMAINT" then CM.execVehCmd(c)
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
	else log("unknown op: " .. tostring(c.op)) end
end

function CM.groundAt(x, y)
	local z = 0
	pcall(function() z = game.interface.getHeight({ x, y }) or 0 end)
	return z or 0
end

-- ---------- command reliability (NACK + resend), encode/decode, scheduleLocal, onLine, pollEvents ----------
-- Lives in res/scripts/mp/net.lua.
require("mp.net")(CM, K, log)
-- ---------- ground-truth sweeps (constructions, vehicles, lines, demolish) ----------
-- Lives in res/scripts/mp/gt.lua (see the header there).
local gt = require("mp.gt")(CM, K, log)
CM.gtVehPickModel, CM.gtVehConfig, CM.runGroundTruth = gt.gtVehPickModel, gt.gtVehConfig, gt.runGroundTruth
-- ---------- roadside stops (edge objects) and native-shape stop replay ----------
-- Lives in res/scripts/mp/stops.lua.
require("mp.stops")(CM, K, log)
-- ---------- inject reader (pollInject) and BUYTEST readback ----------
-- Lives in res/scripts/mp/inject.lua.
require("mp.inject")(CM, K, log)
-- ---------- barrier, catch-up pacing, speed sharing, load gate ----------
-- Lives in res/scripts/mp/pacing.lua.
require("mp.pacing")(CM, K, log)
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

function data()
	return {
		update = function()
			CM.ticks = CM.ticks + 1
			if CM.ticks % 60 == 0 or not K.INSTANCE then
				if not CM.detectInstance() then return end
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
			CM.buytestPoll()
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
				CM.broadcast(string.format("LSTICK t=%d o=%s s=%d hi=%d ceil=%d", math.floor(now), K.INSTANCE, CM.stepOf(now), CM.seqNo, CM.myCeiling or (CM.MAX_SPEED or 4)))
			end

			CM.applyBarrier(now)
			CM.ensureRunning()

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
						f:write(string.format("eff=%s\nspeedreq=%s\n", CM.effSpeed and string.format("%g", CM.effSpeed) or "-",
							CM.spdReq and string.format("%g", CM.spdReq) or "-"))
						f:write(string.format("t=%d\npeer=%s\nskew=%s\ndesyncs=%d\nlate=%d\napplylag=%.1f\napplylate=%d\napplied=%d\nqueued=%d\npaused=%s\nspeed=%s\nverdict=%s\ndetail=%s\n",
							math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
							CM.slowT and string.format("%+.1f", now - CM.slowT) or "?",
							CM.desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
							#CM.queue, CM.paused and "yes" or "no", sp, CM.dashVerdict or "-", tostring(CM.dashLastDetail or "-")))
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
						-- outstanding = commands a peer says it issued that we have not
						-- received. With strict_barrier on, a non-zero value here is the
						-- reason the game is holding.
						f:write(string.format("outstanding=%d\n", CM.dashGaps or 0))
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
						f:write(string.format("t=%d  peer=%s  skew=%s  desyncs=%d  late=%d  applylag=%.1f/%d of %d  queued=%d%s",
							math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
							CM.slowT and string.format("%+.1f", now - CM.slowT) or "?",
							CM.desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
							#CM.queue, CM.paused and "  PAUSED" or ""))
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
						log(string.format("PERF: update avg=%.2f ms max=%.2f ms over %d ticks | hash avg=%.1f ms max=%.1f ms over %d stamps",
							u.sum / u.n, u.max, u.n, h and h.n > 0 and h.sum / h.n or 0, h and h.max or 0, h and h.n or 0))
					end
					CM.perfUpd, CM.perfHash = nil, nil
				end)
				log(string.format("alive t=%d peer=%s queued=%d desyncs=%d paused=%s",
					math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
					#CM.queue, CM.desyncs, tostring(CM.paused)))
			end
		end,

		save = function() return {} end,
		load = function(s) end,

		-- ---------- multiplayer status panel (GUI Lua state) ----------
		guiUpdate = function()
			guiTick = guiTick + 1
			if guiTick % 30 ~= 0 then return end
			local ok = pcall(function()
				-- NATIVE WIDGETS. The GUI Lua state has the game's own widget set
				-- (Window, Table, TextView, BoxLayout), so the dashboard is built
				-- from those rather than one text blob: a metrics table with a
				-- column per instance, a verdict line naming the lanes that
				-- differ, and the last few notable events harvested from the log.
				-- Everything comes from lockstep_dash_<a|b>.txt, written every
				-- 15 ticks by the game-script state.
				-- The lobby's folder, the same three candidates the menu DLL tries
				-- (resolveNetDir): %LOCALAPPDATA%\tpf2mp\netpunch, <game>\netpunch
				-- (the CWD), then the dev checkout.
				function CM.netDir()
					if CM.netDirCached ~= nil then return CM.netDirCached or nil end
					local cands = {}
					local ok, la = pcall(os.getenv, "LOCALAPPDATA")
					if ok and la then cands[#cands + 1] = la .. "/tpf2mp/netpunch" end
					cands[#cands + 1] = "netpunch"
					local ok2, up = pcall(os.getenv, "USERPROFILE")
					if ok2 and up then cands[#cands + 1] = up .. "/tpf2-multiplayer/netpunch" end
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
							CM.chatLines[#CM.chatLines + 1] = from .. ": " .. text
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
				local own = K.INSTANCE or "a"
				local ownKv = readDash(own)
				local ownWall = ownKv and tonumber(ownKv.wall) or nil
				local peerInfo = {}
				if ownKv and ownKv.peers and ownKv.peers ~= "-" then
					for o, pt, sk, vd in ownKv.peers:gmatch("(%a):([%-%d]+):([%+%-%d%.]+):([^,]+)") do
						peerInfo[o] = { t = pt, skew = sk, verdict = vd }
					end
				end
				local present, fresh = {}, {}
				for letter in ("abcdefgh"):gmatch(".") do
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
					D.labels = { t = "game time", peer = "peer time", skew = "skew", speed = "speed", paused = "held by barrier",
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
					CM.dashShowStats = (CM.dashShowStats ~= false)
					CM.dashShowChat = (CM.dashShowChat ~= false)
					local tog = api.gui.layout.BoxLayout.new("HORIZONTAL")
					tog:addItem(toggleBtn("  stats  ", function()
						CM.dashShowStats = not CM.dashShowStats
						pcall(function() D.statsBox:setVisible(CM.dashShowStats, false) end)
					end))
					tog:addItem(toggleBtn("  chat  ", function()
						CM.dashShowChat = not CM.dashShowChat
						pcall(function() D.chatBox:setVisible(CM.dashShowChat, false) end)
					end))
					local togC = api.gui.comp.Component.new("mpToggles")
					togC:setLayout(tog)
					box:addItem(togC)
					local statsL = api.gui.layout.BoxLayout.new("VERTICAL")
					statsL:addItem(D.table)
					statsL:addItem(D.verdict)
					D.statsBox = api.gui.comp.Component.new("mpStats")
					D.statsBox:setLayout(statsL)
					box:addItem(D.statsBox)
					-- ---- session speed + lobby chat (2026-09-09) ----
					-- The lobby (netpunch) keeps running behind the game; its
					-- lobby_out.jsonl carries every chat line and lobby_in.jsonl takes
					-- commands, so the in-game chat is those two files. The speed
					-- buttons SEND "/speed x" as chat: every panel writes it into the
					-- bridge ctl, the host's pacer applies it and broadcasts the
					-- session speed (LSEFF), so anyone can set it and everyone sees it.
					D.speedText = api.gui.comp.TextView.new("session speed: -")
					local function speedBtn(label, fn)
						local b = api.gui.comp.Button.new(api.gui.comp.TextView.new(label), true)
						b:onClick(fn)
						return b
					end
					local row = api.gui.layout.BoxLayout.new("HORIZONTAL")
					row:addItem(D.speedText)
					row:addItem(speedBtn("  -0.5  ", function() CM.chatSend(string.format("/speed %.1f", math.max(0.5, (D.eff or 1) - 0.5))) end))
					row:addItem(speedBtn("  +0.5  ", function() CM.chatSend(string.format("/speed %.1f", math.min(8, (D.eff or 1) + 0.5))) end))
					row:addItem(speedBtn("  levers  ", function() CM.chatSend("/speed off") end))
					local rowC = api.gui.comp.Component.new("mpSpeedRow")
					rowC:setLayout(row)
					box:addItem(rowC)
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
						D.chatBox:setVisible(CM.dashShowChat, false)
					end)
					local body = api.gui.comp.Component.new("mpDashboard")
					body:setLayout(box)
					D.win = api.gui.comp.Window.new("Multiplayer", body)
					D.win:setPosition(20, 120)
					statusWin = D.win     -- keep the old handle alive for the close/rebuild path
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
				pcall(function()
					local eff = mine and tonumber(mine.eff) or nil
					D.eff = eff
					local req = mine and mine.speedreq
					D.speedText:setText(string.format("session speed: %s%s   ", eff and string.format("%gx", eff) or "-",
						(req and req ~= "-") and "  (set)" or "  (lowest lever)"))
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
				if true then return end
				-- Both rows come from the shared data dir. Try K.BASE first, then
				-- every other discovery candidate, so a peer whose DLLs settled
				-- on a different candidate (harness-pinned vs shipping default)
				-- still shows. Same resolution as K.BASE itself -- no separate
				-- sandbox/username-derived path lives here any more.
				local bases = { K.BASE }
				for _, p in ipairs(CM.baseCandidates or {}) do
					if p ~= K.BASE then bases[#bases + 1] = p end
				end
				local lines = {}
				for _, inst in ipairs({ "a", "b" }) do
					local s
					for _, base in ipairs(bases) do
						local f = io.open(base .. "lockstep_status_" .. inst .. ".txt", "r")
						if f then
							local r = f:read("*l")
							f:close()
							if r and #r > 0 then s = r; break end
						end
					end
					if s then lines[#lines + 1] = string.upper(inst) .. "  " .. s end
				end
				statusText:setText(#lines > 0 and table.concat(lines, string.char(10)) or "no status yet")
			end)
			if not ok then
				-- window closed/destroyed: rebuild on the next round
				statusWin, statusText = nil, nil
			end
		end,
	}
end
