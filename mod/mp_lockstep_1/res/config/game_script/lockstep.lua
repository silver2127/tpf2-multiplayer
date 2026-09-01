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

-- ---------- runtime data directory ----------
-- Every runtime file (identity, events, captures, injects, status, logs) lives
-- in ONE directory shared with the bridge and slice DLLs; bridge/src/datadir.h
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
K.BARRIER_AHEAD = 5.0

-- The most peer lead a command's stamp will pay for. Bigger than K.BARRIER_AHEAD
-- on purpose: the barrier only starts acting AT that threshold, so real skew
-- overshoots it before coming back.
CM.MAX_LEAD = 15.0

-- Heartbeats cross between instances through a FILE RELAY (B is sandboxed), so
-- they are not free. At every 2 ticks the relay fell behind and instance A was
-- reading peer times ~13 units stale while B saw A correctly -- both then paused
-- against bad data. 5 ticks is a rate the relay keeps up with.
K.HEARTBEAT_EVERY = 5     -- ticks between LSTICK broadcasts (~0.93s)

-- ~4.6s without a heartbeat = do not trust the peer's clock. Declared up here
-- because scheduleLocal consults it too, long before the barrier section.
K.PEER_STALE_TICKS = 25
K.HASH_EVERY_GAMETIME = 4 -- ~2 game days between desync checks

local ticks        = 0
local eventsOffset = -1
local injectOffset = -1
local seqNo        = 0
-- EVERY PEER, keyed by its letter. The lockstep core was written for exactly
-- two players (one peer, letters a/b); this table is what makes N work. The
-- rules that used to read "the peer" now read over all of them: the barrier
-- holds against the SLOWEST, a command's stamp clears the FASTEST, the pacer
-- chases the fastest, and a stamp is SYNC only when every peer that reported
-- agrees. A peer is "fresh" while its last tick is within K.PEER_STALE_TICKS.
CM.peers = {}   -- origin -> { time=, at=, hashes={[stamp]=h}, details={[stamp]=d}, streak=n }
local function peerFor(o)
	local pr = CM.peers[o]
	if not pr then pr = { hashes = {}, details = {}, streak = 0 }; CM.peers[o] = pr end
	return pr
end
local function peerBounds()
	local minT, maxT
	for _, pr in pairs(CM.peers) do
		if pr.time and pr.at and (ticks - pr.at) <= K.PEER_STALE_TICKS then
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
local peerSeen     = false
CM.lateCount    = 0   -- commands whose game-time stamp had already passed here
local queue        = {}         -- pending commands
local executed     = {}         -- key -> true, so a command runs at most once
local lastHashAt   = nil
local myHashes     = {}         -- [stamp] = our own hash
local myDetails    = {}         -- [stamp] = our own per-component breakdown
local paused       = false
local pausedSince  = nil        -- tick the barrier engaged, for the watchdog
local desyncs      = 0

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

-- ---------- exact hashing (two Lehmer lanes, products stay under 2^53) ----------
-- Same construction as the M3 probe. A weaker hash silently collides and
-- reports agreement between genuinely different worlds, which on a desync
-- detector is the worst possible failure.
local M1, A1 = 2147483647, 48271
local M2, A2 = 2147483629, 40692
local function hashStr(s)
	local h1, h2 = 2166136261 % M1, 2166136261 % M2
	for i = 1, #s do
		local b = s:byte(i)
		h1 = (h1 * A1 + b) % M1
		h2 = (h2 * A2 + b) % M2
	end
	return string.format("%010d-%010d", h1, h2)
end

local function num(x)
	if type(x) == "number" then return string.format("%.17g", x) end
	return tostring(x)
end

local clockSamples, lastClockT = 0, nil
local function gameTime()
	local t
	pcall(function() t = game.interface.getGameTime().time end)
	-- Sub-1s latency is only possible if this clock is FRACTIONAL. At ~1.1s per
	-- whole unit, an integer clock puts a hard floor above the budget no amount
	-- of tuning can clear -- so measure it and say so once, rather than tuning
	-- against a resolution we assumed.
	if t and clockSamples < 40 then
		if t ~= lastClockT then
			clockSamples = clockSamples + 1
			if lastClockT then
				log(string.format("clock sample %d: %.6f (step %.6f)",
					clockSamples, t, t - lastClockT))
			end
			lastClockT = t
		end
	end
	return t
end

-- ---------- io ----------
local function appendLine(path, line)
	local f = io.open(path, "a")
	if not f then return false end
	f:write(line .. "\n")
	f:close()
	return true
end

local function readFrom(path, offset)
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

local function detectInstance()
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
	eventsOffset = -1
	-- inject: prime to the file's CURRENT size instead. -1 here loses the first
	-- command every time: while the file does not exist the offset stays -1, and
	-- the poll that finally opens it seeks straight to the end -- past the line
	-- it was supposed to read. Same shape as the bug mpbridge records for its
	-- events file, where the joiner primed to end-of-file and reported
	-- consumed=0 while holding every line.
	injectOffset = 0
	local f2 = io.open(K.INJECT_FILE, "r")
	if f2 then injectOffset = f2:seek("end"); f2:close() end
	-- A status file for the letter we are NOT is last session's, and it looks
	-- alive: "desyncs=832" from a previous run was read as this session's count
	-- (2026-08-31, after the two instances swapped letters on restart). Remove it
	-- so only one status file exists and it is always the live one.
	for letter in ("abcdefgh"):gmatch(".") do
		if letter ~= inst then
			pcall(function() os.remove(K.BASE .. "lockstep_status_" .. letter .. ".txt") end)
			pcall(function() os.remove(K.BASE .. "lockstep_dash_" .. letter .. ".txt") end)
		end
	end
	log("identity " .. K.INSTANCE .. " (peer " .. K.PEER .. ")")
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

-- ---------- multi-company mode (opt-in; co-op is the default and is untouched) ----------
--
-- Two multiplayer models share this mod:
--   * "coop"      : one shared company, every action replicated (the original
--                   lockstep). This is the DEFAULT and nothing below runs.
--   * "companies" : each player runs their OWN company in the same map. On this
--                   machine the local player is company CM.cmMyCompany; every remote
--                   company is a dedicated engine player entity (game.interface.
--                   addPlayer -- the same mechanism the "Multiplayer Companies"
--                   mod uses). A replicated build lands owned by the local player,
--                   then we setPlayer it to the ORIGIN company. Because companies
--                   have separate state, this does not require a bit-identical
--                   world -- only that actions appear, attributed correctly.
--
-- Config file (written by the lobby/menu): line 1 = mode, line 2 = my company id,
-- line 3 = comma-separated roster of all company ids. Absent => coop.
-- ONE table for all companies-mode state/fns: Lua 5.1 allows only 200
-- top-level locals per chunk and this file sits right at it (a 19-local batch
-- crashed both instances at script load with "too many local variables").
-- The table itself is declared at the top of the file so the K.BASE discovery
-- can record into it; everything companies-mode starts here.
CM.CM_CFG_FILE = K.BASE .. "mp_company_cfg.txt"
-- The game buffers stdout until exit, so print()-only logging is invisible while
-- a live test runs. Companies-mode diagnostics go to their own file on disk.
function CM.cmLog(s)
	log(s)
	local f = io.open(K.BASE .. "mp_company_" .. tostring(K.INSTANCE or "?") .. ".log", "a")
	if f then f:write(s, "\n"); f:close() end
end
CM.cmMode       = "coop"
CM.cmMyCompany  = nil
CM.cmRoster     = nil        -- array of all company ids in the session
CM.cmCompanyPid = {}         -- [companyId] = engine player entity id
CM.cmExpectedCompany = {}    -- conKey -> origin company of a replay about to land there
CM.cmExpectedBal0    = {}    -- conKey -> our balance right before that replay was applied
CM.cmReady      = false
CM.cmCfgStamp   = nil

function CM.cmReadConfig()
	local f = io.open(CM.CM_CFG_FILE, "r")
	if not f then CM.cmMode = "coop"; return end
	local mode = f:read("*l"); local mine = f:read("*l"); local roster = f:read("*l")
	f:close()
	CM.cmMode = (mode and mode:gsub("%s", "")) or "coop"
	CM.cmMyCompany = mine and tonumber(mine)
	CM.cmRoster = nil
	if roster then
		CM.cmRoster = {}
		for id in roster:gmatch("%d+") do CM.cmRoster[#CM.cmRoster + 1] = tonumber(id) end
	end
end

-- Lazily set up the player entities for companies mode. The local player is our
-- own company; each other company gets an addPlayer() entity, once.
function CM.cmEnsure()
	if CM.cmMode == "companies" and CM.cmReady then return end
	CM.cmReadConfig()
	if CM.cmMode ~= "companies" or not CM.cmMyCompany or not CM.cmRoster then CM.cmReady = false; return end
	pcall(function() CM.cmCompanyPid[CM.cmMyCompany] = api.engine.util.getPlayer() end)
	for _, cid in ipairs(CM.cmRoster) do
		if cid ~= CM.cmMyCompany and not CM.cmCompanyPid[cid] then
			local pid = nil
			pcall(function() pid = game.interface.addPlayer() end)
			if pid then
				CM.cmCompanyPid[cid] = pid
				pcall(function() game.interface.setMaximumLoan(pid, 100000000) end)
				CM.cmLog(string.format("CM: company %d -> AI player %s", cid, tostring(pid)))
			end
		end
	end
	if CM.cmCompanyPid[CM.cmMyCompany] then
		CM.cmReady = true
		CM.cmLog(string.format("CM: companies mode ready, me=co%d players=%d", CM.cmMyCompany, #CM.cmRoster))
	end
end

-- A replicated build landed owned by our local player; hand it to the origin
-- company and lock it so other companies cannot bulldoze it. Cost transfer
-- (refund local player, charge the origin company) is R2 -- TODO once R1
-- (ownership) is validated live. sendScriptEvent lets a co-resident
-- "Multiplayer Companies" mod track the logical ownership too (harmless if absent).
function CM.cmOwnerOf(eid)
	local ok, comp = pcall(function()
		return api.engine.getComponent(eid, api.type.ComponentType.PLAYER_OWNED)
	end)
	if ok and comp then return comp.player end
	return nil
end

-- R2: move money. A remote company's action was paid by OUR local wallet (the
-- build/buy ran as our human player); refund us and charge the origin company's
-- player. bookJournalEntry to a specific pid is the proven mechanism (the
-- Companies mod seeds inactive companies' AI players with it). Chunked because
-- TpF2 silently drops very large single journal amounts (mod-measured).
CM.CM_JOURNAL_CHUNK = 10000000
function CM.cmBookJournal(pid, amount)
	if not pid or not amount or amount == 0 then return true end
	amount = math.floor(amount + 0.5)
	local sign = amount >= 0 and 1 or -1
	local remaining = math.abs(amount)
	local okAll, errAll = true, nil
	local chunks = 0
	while remaining > 0 and chunks < 1000 do
		local chunk = math.min(remaining, CM.CM_JOURNAL_CHUNK)
		local ok, err = pcall(function()
			local cat = api.type.JournalEntryCategory.new(); cat.type = 0
			local entry = api.type.JournalEntry.new()
			entry.amount = sign * chunk; entry.category = cat; entry.time = -1
			api.cmd.sendCommand(api.cmd.make.bookJournalEntry(pid, entry, api.type.Vec3f.new(0, 0, 0)))
		end)
		if not ok then okAll, errAll = false, err end
		remaining = remaining - chunk; chunks = chunks + 1
	end
	return okAll, errAll
end

function CM.cmBalance(pid)
	local bal = nil
	pcall(function() local e = game.interface.getEntity(pid); if e and e.balance then bal = e.balance end end)
	return bal
end

-- Transfer `cost` from the origin company to us (refund local, charge origin).
function CM.cmTransferCost(cid, cost, what)
	CM.cmLog(string.format("CM: cost settle %s: cid=%s cost=%s me=%s", tostring(what), tostring(cid), tostring(cost), tostring(CM.cmMyCompany)))
	if CM.cmMode ~= "companies" or not cid or cid == CM.cmMyCompany then return end
	if not cost or cost <= 0 then CM.cmLog("CM: cost settle: nothing to move (delta " .. tostring(cost) .. ")"); return end
	local mePid, theirPid = CM.cmCompanyPid[CM.cmMyCompany], CM.cmCompanyPid[cid]
	if not mePid or not theirPid then CM.cmLog("CM: cost transfer: missing pid (me=" .. tostring(mePid) .. " co" .. tostring(cid) .. "=" .. tostring(theirPid) .. ")"); return end
	local ok1, e1 = CM.cmBookJournal(mePid, cost)       -- refund us
	local ok2, e2 = CM.cmBookJournal(theirPid, -cost)   -- charge them
	CM.cmLog(string.format("CM: cost %s: moved %d from co%d(pid %s) to me(pid %s) | refund ok=%s %s | charge ok=%s %s",
		tostring(what), cost, cid, tostring(theirPid), tostring(mePid), tostring(ok1), tostring(e1 or ""), tostring(ok2), tostring(e2 or "")))
end

-- Generic reassign for any entity type (vehicles, lines): setPlayer only.
-- setBulldozeable is CONSTRUCTION-only (asserts otherwise) so it is skipped here.
function CM.cmReassignEntity(eid, cid, kind)
	CM.cmEnsure()
	if CM.cmMode ~= "companies" or not eid or not cid or cid == CM.cmMyCompany then return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	local ok, err = pcall(function() game.interface.setPlayer(eid, pid) end)
	local after = CM.cmOwnerOf(eid)
	CM.cmLog(string.format("CM: reassigned %s eid=%s -> co%d pid=%s | owner before=%s after=%s | setPlayer ok=%s err=%s",
		tostring(kind), tostring(eid), cid, tostring(pid), tostring(before), tostring(after), tostring(ok), tostring(err)))
end

function CM.cmReassignConstruction(eid, cid)
	-- Initialise BEFORE the mode gate. The first thing that happens on a peer can
	-- be a replayed build arriving before any local action has read the config --
	-- without this it evaluated mode=coop/me=nil and silently skipped (verified in
	-- the 2026-08-29 trace: "reassign requested ... mode=coop me=nil").
	CM.cmEnsure()
	CM.cmLog(string.format("CM: reassign requested eid=%s cid=%s mode=%s me=%s",
		tostring(eid), tostring(cid), tostring(CM.cmMode), tostring(CM.cmMyCompany)))
	if CM.cmMode ~= "companies" or not eid or not cid then return end
	if cid == CM.cmMyCompany then CM.cmLog("CM: own company, skip"); return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmEnsure(); pid = CM.cmCompanyPid[cid] end
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	-- setBulldozeable CRASHES the game (ParcelSystem assertion -- pcall does NOT
	-- catch C++ asserts) on entities that can't take the Bulldozeable component.
	-- The "Multiplayer Companies" mod's rule: only call it on entities that have a
	-- CONSTRUCTION component. Same guard here.
	local hasCon = false
	pcall(function() hasCon = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) ~= nil end)
	-- Do NOT swallow errors silently: the whole point of this probe is to learn
	-- whether setPlayer works on a replicated construction. Record each result.
	local okSP, errSP = pcall(function() game.interface.setPlayer(eid, pid) end)
	local okBZ, errBZ = true, "skipped (no CONSTRUCTION component)"
	if hasCon then okBZ, errBZ = pcall(function() game.interface.setBulldozeable(eid, false) end) end
	-- (sendScriptEvent is a GUI-side API and is nil on the engine side -- the
	-- optional "tell the Companies mod" hook is dropped; ownership + lock are ours.)
	local okEV, errEV = true, nil
	local after = CM.cmOwnerOf(eid)
	CM.cmLog(string.format("CM: reassigned eid=%s -> co%d pid=%s | owner before=%s after=%s | setPlayer ok=%s err=%s | bulldozeable ok=%s err=%s | event ok=%s err=%s",
		tostring(eid), cid, tostring(pid), tostring(before), tostring(after),
		tostring(okSP), tostring(errSP), tostring(okBZ), tostring(errBZ), tostring(okEV), tostring(errEV)))
end

-- ---------- world hash (desync detector) ----------
--
-- EDGE COVERAGE. The first version of this hashed vehicle positions and a
-- CONSTRUCTION *count*, which made it structurally incapable of detecting the
-- one thing the vertical slice produces: a road built in the wrong place. A
-- gate that cannot fail the test it gates is worse than no gate, because it
-- reports success.
--
-- The enumeration API for edges is not known for this build, and guessing at
-- one is exactly how the M3 probe came to report `nv=0` on a save full of
-- vehicles. So try several and record which worked. Probing by CALLING is
-- deliberate: `type(fn) == "function"` is false for sol2 bindings, so testing
-- for existence before calling gives the wrong answer.
local edgeStrategy = nil          -- name of the strategy that worked, or nil

local function collectEdges()
	local out = {}

	local function tryGameInterface(kind)
		local t = game.interface.getEntities({ radius = 999999 },
			{ type = kind, includeData = false })
		if type(t) ~= "table" then return false end
		local n = 0
		for _, e in pairs(t) do out[#out + 1] = e; n = n + 1 end
		return n > 0
	end

	local strategies = {
		{ "gi:BASE_EDGE", function() return tryGameInterface("BASE_EDGE") end },
		{ "gi:EDGE",      function() return tryGameInterface("EDGE") end },
		{ "gi:STREET",    function() return tryGameInterface("STREET") end },
		{ "node2segment", function()
			local m = api.engine.system.streetSystem.getNode2SegmentMap()
			if type(m) ~= "table" then return false end
			local n = 0
			for _, segs in pairs(m) do
				if type(segs) == "table" then
					for _, s in pairs(segs) do out[#out + 1] = s; n = n + 1 end
				end
			end
			return n > 0
		end },
	}

	for _, s in ipairs(strategies) do
		local before = #out
		local ok, got = pcall(s[2])
		if ok and got then
			edgeStrategy = s[1]
			return out
		end
		for i = #out, before + 1, -1 do out[i] = nil end   -- discard partials
	end
	edgeStrategy = nil
	return out
end

-- Forward declarations: worldHash uses these, and they are defined further
-- down. A later `local function` would create a DIFFERENT variable and this
-- reference would resolve to a nil global at call time -- the groundAt bug.
local ser, deepcopy, isPlayerConstruction
local scheduleLocal   -- forward: called by the line registry above its definition
K.STRICT_OPS = { VREV = true, VLINE = true }   -- replay on the originator too, but only when ARMED=1 (the slice cancelled it)

local warnedNoEdges = false
-- node id -> "x,y", valid for ONE hash pass only. Ids are recycled: a node that
-- is bulldozed hands its id to whatever is built next, so a cache kept across
-- passes reports the dead node's position for the live one -- a phantom desync
-- with two provably identical worlds behind it (2026-08-30: the desync counter
-- kept climbing after the last real difference was repaired). Within one pass
-- the cache still pays for itself: every node is asked for by both its edges.
local nodePosCache = {}

-- ---------- world hash, id-free ----------
--
-- The first detector hashed vehicle POSITIONS and entity IDS and construction
-- COUNTS. Every one of those differs between peers for reasons that are not a
-- desync: positions are render-interpolated (both peers sample the same sim
-- time on different frames), ids diverge by design now that replication is
-- positional (each peer allocates its own), seeds are engine-assigned per peer,
-- and town growth adds constructions and streets at its own pace. A detector
-- that fires on all of that proves nothing when it fires and hides a real
-- divergence in the noise.
--
-- Rule: hash only what lockstep is supposed to keep identical, and hash it by
-- GEOMETRY and CONTENT, never by id.
--   v  vehicle COUNT only (a vehicle bought on one peer and not the other IS a desync)
--   c  player-owned constructions: file + position + params minus seed
--   e  edges by endpoint positions (0.1 m), regardless of which ids hold them
--   t  town/other construction count -- REPORTED, never part of the verdict
local function nodePos(nid)
	local s = nodePosCache[nid]
	if s then return s end
	local ok, c = pcall(function() return api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE) end)
	if ok and c and c.position then
		local p = c.position
		-- HEIGHT IS PART OF IDENTITY. x,y alone let two worlds hash equal while
		-- their nodes sat at different heights -- and height is exactly what the
		-- engine's slope check reads. On 2026-08-30 the hashes agreed right up to
		-- the build before the divergence, then the same one-edge proposal was
		-- accepted on one instance and refused on the other: the detector was
		-- blind to the only dimension that could still differ. Same 0.1 m
		-- resolution as x,y.
		s = string.format("%.1f,%.1f,%.1f", p.x or p[1], p.y or p[2], p.z or p[3] or 0)
		nodePosCache[nid] = s
		return s
	end
	return "?"
end

local function worldHash(now)
	-- Vehicles: a count, and -- separately -- where they are.
	--
	-- The count alone answers "did a purchase replicate", which is not what
	-- lockstep is for. Two worlds running the same commands at the same game
	-- times should have every vehicle in the same PLACE; a train that is 40 m
	-- further along on one machine is the simulations diverging, and it is the
	-- first thing a command applied at the wrong moment disturbs. It is also the
	-- one measure that drifts on its own if the clocks are not truly in step.
	--
	-- Positions ride in the DETAIL line only, never in the verdict, until we have
	-- watched how well they actually track. They are continuous and quantised to
	-- a metre here: if it turns out two honest instances differ by a metre in
	-- normal play, that must not start reporting desyncs -- it is a measurement
	-- to read, not a verdict, until the evidence says otherwise.
	local nv = 0
	local vpos = {}
	pcall(function()
		-- includeData=true hands back every vehicle's record in ONE call. The
		-- first version did a getEntity per vehicle inside its own closure --
		-- thousands of serialisations per hash tick on a late-game map, for a
		-- lane that never decides anything (review, 2026-08-31).
		local t = game.interface.getEntities({ radius = 999999 },
			{ type = "VEHICLE", includeData = true }) or {}
		for vid, e in pairs(t) do
			nv = nv + 1
			local p = type(e) == "table" and e.position or nil
			if p then
				-- sorted below, so this says nothing about WHICH vehicle is
				-- where -- ids differ between instances and always will
				vpos[#vpos + 1] = string.format("%.0f,%.0f,%.0f",
					p[1] or p.x or 0, p[2] or p.y or 0, p[3] or p.z or 0)
			end
		end
	end)
	table.sort(vpos)

	-- constructions: player-owned by content, everything else counted
	local cons, nt = {}, 0
	pcall(function()
		local t = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, cid in pairs(t) do
			local alive = false
			pcall(function() alive = api.engine.entityExists(cid) end)
			if alive then
				local co = api.engine.getComponent(cid, api.type.ComponentType.CONSTRUCTION)
				local fn = co and co.fileName and tostring(co.fileName) or ""
				if co and co.transf and isPlayerConstruction(cid, fn) then
					local e = game.interface.getEntity(cid)
					local p = (e and e.params) and deepcopy(e.params) or {}
					p.seed = nil
					cons[#cons + 1] = string.format("%s@%.1f,%.1f:%s", fn,
						co.transf[13], co.transf[14], hashStr(ser(p) or ""))
				else
					nt = nt + 1
				end
			end
		end
	end)
	-- roadside stops (edge objects with a STATION) count as player constructions
	-- for the hash: a stop one side does not have is a c-lane difference.
	pcall(function()
		local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
		for eo, _ in pairs(m) do
			local st, sg, po
			pcall(function() st = api.engine.getComponent(eo, api.type.ComponentType.STATION) end)
			pcall(function() sg = api.engine.getComponent(eo, api.type.ComponentType.SIGNAL_LIST) end)
			pcall(function() po = api.engine.getComponent(eo, api.type.ComponentType.PLAYER_OWNED) end)
			if (st or sg) and po then
				local mil = api.engine.getComponent(eo, api.type.ComponentType.MODEL_INSTANCE_LIST)
				local fi = mil and mil.fatInstances[1]
				if fi then
					cons[#cons + 1] = string.format("stop:%s@%.1f,%.1f",
						tostring(api.res.modelRep.getName(fi.modelId)), fi.transf[13], fi.transf[14])
				end
			end
		end
	end)
	table.sort(cons)

	-- edges by geometry
	nodePosCache = {}
	local edges = collectEdges()
	local egeo, egeoZ = {}, {}
	for _, eid in ipairs(edges) do
		local ok, c = pcall(function()
			return api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
		end)
		if ok and c and c.node0 then
			local a, b = nodePos(c.node0), nodePos(c.node1)
			if a > b then a, b = b, a end     -- direction-independent
			egeo[#egeo + 1] = a .. ">" .. b
			-- A second lane carrying ONLY the heights. The verdict hash already
			-- covers z (it is part of nodePos), but when the worlds disagree it
			-- matters whether they disagree about WHERE the rails are or only
			-- about how high: the first is a missing build, the second is a
			-- crossing or terrain problem. Detail-only -- never a verdict of
			-- its own.
			local za, zb = a:match("[^,]+,[^,]+,([^>]+)"), b:match("[^,]+,[^,]+,(.+)")
			egeoZ[#egeoZ + 1] = (za or "?") .. ">" .. (zb or "?")
		end
	end
	table.sort(egeo)
	table.sort(egeoZ)

	if #edges == 0 and not warnedNoEdges then
		warnedNoEdges = true
		log("WARNING: edge enumeration found NOTHING -- the desync detector is BLIND TO ROADS AND TRACK")
	elseif edgeStrategy and not warnedNoEdges then
		warnedNoEdges = true
		log("edge enumeration via " .. edgeStrategy .. ": " .. #edges .. " edges")
	end

	local hc = hashStr(table.concat(cons, "|"))
	local he = hashStr(table.concat(egeo, "|"))
	-- The verdict hash covers ONLY the components lockstep controls.
	local verdict = hashStr("v" .. nv .. "|" .. hc .. "|" .. he)
	-- p: is the vehicle-position lane. #vpos can be less than nv when a vehicle
	-- has no position to read (in a depot, mid-load), so it carries its own count.
	-- WHEN the vehicles were sampled travels with the lane. The hash fires on
	-- the first update() after the clock crosses a 4-unit stamp, but the clock
	-- moves in 0.2-unit sim steps and update() is per frame: at speed 2 or 3
	-- several sim steps pass between frames, so one instance's first look is at
	-- 1648.0 and the other's at 1648.4 -- and a train at 53 mph has moved ~10 m
	-- in between. Two samples from different sim times are not comparable, and
	-- until now the lane compared them anyway. The comparison below only calls
	-- p a difference when both sides sampled the SAME sim time.
	local detail = string.format("v%d,c%d:%s,e%d:%s,z:%s,p%d@%.1f:%s,t%d",
		nv, #cons, hc, #egeo, he, hashStr(table.concat(egeoZ, "|")),
		#vpos, now or -1, hashStr(table.concat(vpos, "|")), nt)
	return verdict, detail
end

-- ---------- command execution ----------
-- Deterministic order is mandatory. Two commands due at the same stamp must be
-- applied in the same sequence on every peer, or the worlds diverge even though
-- both "executed the same commands".
local function cmdKey(c) return c.at .. "|" .. c.origin .. "|" .. c.seq end
local function cmdLess(x, y)
	if x.at ~= y.at then return x.at < y.at end
	if x.origin ~= y.origin then return x.origin < y.origin end
	return x.seq < y.seq
end

-- The build context. Passing nil here builds for FREE: no player attribution, so
-- the game never charges for the command. Every exec path below did that, which
-- made replicated roads cost nothing.
--
-- This is not only a realism bug. Money is world state, so a build charged on one
-- peer and free on another is a divergence -- and because both peers run the same
-- command with the same hardcoded context, charging is also the DETERMINISTIC
-- choice. If a peer cannot afford it the command fails there, which is a real
-- lockstep divergence that the desync detector should report rather than something
-- to paper over by building for free.
--
-- Falls back to nil on any error: a free build is wrong, but it is far better than
-- an exception that stops the command being replicated at all.
local function buildContext()
	local ok, ctx = pcall(function()
		local c = api.type.Context:new()
		c.checkTerrainAlignment = false
		c.cleanupStreetGraph    = true
		c.gatherBuildings       = false
		c.gatherFields          = true
		c.player                = api.engine.util.getPlayer()
		return c
	end)
	if ok and ctx then return ctx end
	log("WARNING: could not build a Context -- falling back to a FREE build")
	return nil
end

local function execEdge(c)
	local isRail = (c.op == "RAIL")
	local ok, err = pcall(function()
		-- Placeholder entity ids MUST be derived from the command, never from
		-- anything local. mptest uses `-100000 - ticks`, which is fine for a
		-- single instance but fatal here: `ticks` counts frames since load and
		-- differs between peers, so each would build the same road with
		-- different placeholder ids. Deriving them from (origin, seq) makes both
		-- peers compute identical values while staying unique per command.
		local base = -(200000 + CM.originIdx(c.origin) * 100000 + c.seq * 10)   -- one namespace per origin (a..h)
		local nid0, nid1, eid = base, base - 1, base - 2

		local sp = api.type.SimpleProposal.new()
		local n0 = api.type.NodeAndEntity.new()
		n0.entity = nid0
		n0.comp.position = api.type.Vec3f.new(c.x0, c.y0, c.z0)
		local n1 = api.type.NodeAndEntity.new()
		n1.entity = nid1
		n1.comp.position = api.type.Vec3f.new(c.x1, c.y1, c.z1)
		sp.streetProposal.nodesToAdd[1] = n0
		sp.streetProposal.nodesToAdd[2] = n1

		local dx, dy, dz = c.x1 - c.x0, c.y1 - c.y0, c.z1 - c.z0
		local e = api.type.SegmentAndEntity.new()
		e.entity = eid
		e.comp.node0 = nid0
		e.comp.node1 = nid1
		e.comp.tangent0 = api.type.Vec3f.new(dx, dy, dz)
		e.comp.tangent1 = api.type.Vec3f.new(dx, dy, dz)
		-- comp.type stays 0; e.type selects street(0) vs track(1). Confusing
		-- these two is what made the harness build roads when asked for rail.
		e.comp.type = 0
		e.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1
		e.type = isRail and 1 or 0
		if isRail then
			e.trackEdge = api.type.BaseEdgeTrack.new()
			e.trackEdge.trackType = c.ttype or 1
			e.trackEdge.catenary = (c.cat == 1)
			-- A street edge is mandatory for validation even on a track --
			-- ResTypeRep<StreetType>::Get(-1) asserts without it -- and is
			-- discarded on the resulting edge.
			e.streetEdge = api.type.BaseEdgeStreet.new()
			e.streetEdge.streetType = c.stype or 16
		else
			e.streetEdge = api.type.BaseEdgeStreet.new()
			e.streetEdge.streetType = c.stype or 16
			e.streetEdge.hasBus = false
			e.streetEdge.tramTrackType = 0
		end
		sp.streetProposal.edgesToAdd[1] = e

		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, buildContext(), false),
			function(res, success)
				log(string.format("EXEC %s seq=%s origin=%s at=%s success=%s",
					c.op, tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(success)))
			end)
	end)
	if not ok then log("exec error: " .. tostring(err)) end
end

-- ---------- mid-span splitting, ported from mp_bridge ----------
--
-- buildProposal REJECTS an edge whose endpoint lands partway along an existing
-- edge. The interactive build tool splits for you; a raw proposal does not. So
-- the peer receives an edge whose endpoint sits mid-span on ITS copy, tries to
-- plant a bare node there, and is refused -- mp_bridge measured success=false on
-- 7 of 8 road edges and named it "the road intersection bug".
--
-- This is why hunting for edgesToRemove in the captured proposal was doomed:
-- when a player snaps onto an existing road the host splits nothing, because it
-- is attaching to geometry it already has. There is no removal list to find.
-- The split has to be recreated on EACH peer, from POSITIONS.
--
-- Resolving by position rather than entity id also removes the assumption that
-- ids match across peers, which nothing had ever verified.
local function hermitePos(p0, t0, p1, t1, u)
	local u2, u3 = u * u, u * u * u
	local h00, h10 = 2*u3 - 3*u2 + 1, u3 - 2*u2 + u
	local h01, h11 = -2*u3 + 3*u2, u3 - u2
	local r = {}
	for i = 1, 3 do r[i] = h00*p0[i] + h10*t0[i] + h01*p1[i] + h11*t1[i] end
	return r
end

local function hermiteTangent(p0, t0, p1, t1, u)
	local u2 = u * u
	local g00, g10 = 6*u2 - 6*u, 3*u2 - 4*u + 1
	local g01, g11 = -6*u2 + 6*u, 3*u2 - 2*u
	local r = {}
	for i = 1, 3 do r[i] = g00*p0[i] + g10*t0[i] + g01*p1[i] + g11*t1[i] end
	return r
end

local function vec3t(v)
	if not v then return { 0, 0, 0 } end
	return { v.x or v[1] or 0, v.y or v[2] or 0, v.z or v[3] or 0 }
end

-- Full position of a node, height included -- the wire needs all three.
function CM.nodePosXYZ(nid)
	local c = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
	if not c or not c.position then return nil end
	local p = c.position
	return { p.x or p[1], p.y or p[2], p.z or p[3] or 0 }
end

local function edgeGeomT(eid)
	local comp = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
	if not comp then return nil end
	local function np(nid)
		local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
		if not nc or not nc.position then return nil end
		return vec3t(nc.position)
	end
	local a, b = np(comp.node0), np(comp.node1)
	if not a or not b then return nil end
	return comp, a, b, vec3t(comp.tangent0), vec3t(comp.tangent1)
end

K.SPLIT_EPS = 5.0   -- was 3.0; a rail touching a road's EDGE sits ~4.5 m off its centreline
K.SPLIT_MIN_U = 0.08   -- nearer an end than this IS the endpoint, not a split
K.SPLIT_MIN_DIST = 0.3  -- metres from an end node: closer than this IS the node

local function edgeMaps()
	local maps = {}
	pcall(function() maps[#maps + 1] = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	pcall(function() maps[#maps + 1] = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	return maps
end

-- Node identity is HORIZONTAL only. The engine is free to settle a node at a
-- different height than we asked for (embankments, terrain smoothing), and on a
-- slope that easily exceeds a 1.5 m tolerance -- so keying on z made the second
-- segment of a chain miss the node the first had just created.
-- Kind-aware. Searching BOTH maps let a rail endpoint snap onto a nearby ROAD
-- node and weld track to street -- a road node and a rail node at the same spot
-- are different things, even though mp_bridge searched both.
local function findNodeNear(isTrack, x, y, eps)
	local best, bestD
	local m
	if isTrack then
		pcall(function() m = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	else
		pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	end
	do
		for nid, _ in pairs(m or {}) do
			local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
			if nc and nc.position then
				local p = nc.position
				local dx, dy = (p.x or p[1]) - x, (p.y or p[2]) - y
				local d = dx * dx + dy * dy
				if d < eps * eps and (not bestD or d < bestD) then best, bestD = nid, d end
			end
		end
	end
	return best
end

local function findEdgeContaining(isTrack, x, y, skipNode)
	-- Sampling is by DISTANCE, not by a fixed 19 points: a 77 m town road
	-- sampled every 7.7 m misses a point that is on it. And the end
	-- exclusion is by distance (K.SPLIT_MIN_DIST), not by fraction: the UI
	-- happily splits 1.0 m from an end node (measured: a depot snapped at
	-- u=0.013 of a 76.9 m edge), and 8% of a long edge is many metres.
	local best, bestD, bestU, bestGeom, seen = nil, nil, nil, nil, {}
	local m
	if isTrack then
		pcall(function() m = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	else
		pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	end
	for _, list in pairs(m or {}) do
		for _, eid in pairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local comp, a, b, ta, tb = edgeGeomT(eid)
				-- skipNode: an edge already ENDING at the node we are welding
				-- into sits at distance ~0 but u~1.0, and would shadow the
				-- actual street edge.
				if comp and (not skipNode
				             or (comp.node0 ~= skipNode and comp.node1 ~= skipNode)) then
					local span = (b[1]-a[1])^2 + (b[2]-a[2])^2
					local d0 = (a[1]-x)^2 + (a[2]-y)^2
					local d1 = (b[1]-x)^2 + (b[2]-y)^2
					if d0 < span * 4 + 400 or d1 < span * 4 + 400 then
						local len = math.max(math.sqrt(span),
							math.sqrt(ta[1]^2 + ta[2]^2), math.sqrt(tb[1]^2 + tb[2]^2))
						local steps = math.min(400, math.max(19, math.ceil(len / 1.0)))
						for i = 1, steps - 1 do
							local u = i / steps
							local q = hermitePos(a, ta, b, tb, u)
							local d = (q[1]-x)^2 + (q[2]-y)^2
							if d < K.SPLIT_EPS*K.SPLIT_EPS and (not bestD or d < bestD) then
								best, bestD, bestU = eid, d, u
								bestGeom = { a, ta, b, tb, steps }
							end
						end
					end
				end
			end
		end
	end
	if not best then return nil end
	-- refine u between the neighbouring samples (bisection on distance)
	local a, ta, b, tb, steps = bestGeom[1], bestGeom[2], bestGeom[3], bestGeom[4], bestGeom[5]
	local lo, hi = math.max(0, bestU - 1 / steps), math.min(1, bestU + 1 / steps)
	for _ = 1, 12 do
		local u1, u2 = lo + (hi - lo) / 3, hi - (hi - lo) / 3
		local q1, q2 = hermitePos(a, ta, b, tb, u1), hermitePos(a, ta, b, tb, u2)
		local e1 = (q1[1]-x)^2 + (q1[2]-y)^2
		local e2 = (q2[1]-x)^2 + (q2[2]-y)^2
		if e1 < e2 then hi = u2 else lo = u1 end
	end
	local u = (lo + hi) / 2
	local q = hermitePos(a, ta, b, tb, u)
	-- too close to an end IS the end node, not a split
	local dA = math.sqrt((q[1]-a[1])^2 + (q[2]-a[2])^2)
	local dB = math.sqrt((q[1]-b[1])^2 + (q[2]-b[2])^2)
	if dA < K.SPLIT_MIN_DIST or dB < K.SPLIT_MIN_DIST then return nil end
	return best, u
end

local function copyEdgeProps(dst, srcEid, isTrack, stype)
	-- A half of a split edge keeps the split edge's BRIDGE/TUNNEL type too
	-- (BaseEdge.type 0 ground / 1 bridge / 2 tunnel, typeIndex = the type's
	-- resource index, -1 on the ground). Callers stamp 0/-1 first; override.
	pcall(function()
		local be = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE)
		if be and (be.type == 1 or be.type == 2) then
			dst.comp.type = be.type
			dst.comp.typeIndex = be.typeIndex or -1
		end
	end)
	if isTrack then
		local tc = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE_TRACK)
		dst.trackEdge = api.type.BaseEdgeTrack.new()
		dst.trackEdge.trackType = (tc and tc.trackType) or 0
		dst.trackEdge.catenary = (tc and tc.catenary) and true or false
		dst.streetEdge = api.type.BaseEdgeStreet.new()
		dst.streetEdge.streetType = stype or 16
	else
		local sc = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE_STREET)
		dst.streetEdge = api.type.BaseEdgeStreet.new()
		dst.streetEdge.streetType = (sc and sc.streetType) or stype or 16
		if sc then
			dst.streetEdge.hasBus = sc.hasBus and true or false
			dst.streetEdge.tramTrackType = sc.tramTrackType or 0
		end
	end
end

-- Diagnostic export: EVAL chunks run in the global environment and cannot
-- see this file's locals. Exposing the geometry helpers lets a probe try a
-- weld or a split on the live world without a rebuild cycle.
LS = { findNodeNear = findNodeNear, findEdgeContaining = findEdgeContaining,
       edgeGeomT = edgeGeomT, hermitePos = hermitePos, hermiteTangent = hermiteTangent,
       copyEdgeProps = copyEdgeProps, buildContext = buildContext, groundAt = groundAt }

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
-- ---------- the originator's plan ----------
--
-- Both instances used to re-derive a road/rail build from the shipped polyline:
-- each hunted for crossings, chose which edge to split and where, against its
-- OWN copy of the world. Identical code over identical worlds gives identical
-- answers, but it has no tolerance for a world that has drifted even slightly,
-- and it turns one drift into a cascade. Measured 2026-08-30: from a state whose
-- hashes agreed on every tick, seq=12 (2 points, 1 edge, no removals) was
-- accepted on the originator and refused on the peer, and every later build
-- failed too, because by then the two were resolving against different worlds.
--
-- So the originator decides, once, and ships the decisions. Positions, never
-- ids -- entity ids are per-instance, positions are the shared language this
-- wire already speaks (see the rm= removals). A peer that cannot match an entry
-- says so and falls back to deriving that one itself, which is strictly no
-- worse than the old behaviour.
--
--   xv = per-VERTEX:  "i,N,x,y,z"                        resolve to the node there
--                     "i,S,px,py,pz,ax,ay,bx,by"         split the edge a--b at p
--   xh = per-LINK:    "k,N,x,y,z,u"                      route through that node
--                     "k,S,px,py,pz,ax,ay,bx,by,u"       split the edge a--b at p
function CM.planEncode(items)
	if not items or #items == 0 then return nil end
	return table.concat(items, ";")
end

function CM.planDecode(str)
	local out = {}
	for entry in tostring(str or ""):gmatch("[^;]+") do
		local f = {}
		for tok in entry:gmatch("[^,]+") do f[#f + 1] = tok end
		local idx = tonumber(f[1])
		if idx and f[2] then
			local e = { kind = f[2] }
			for i = 3, #f do e[#e + 1] = tonumber(f[i]) end
			out[idx] = out[idx] or {}
			out[idx][#out[idx] + 1] = e
		end
	end
	return out
end

-- Find THIS instance's edge with the given endpoint positions. Orientation is
-- not part of the identity: node0/node1 order is per-instance.
function CM.findEdgeByEnds(isTrack, ax, ay, bx, by, eps)
	eps = eps or 1.5
	local m
	if isTrack then
		pcall(function() m = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	else
		pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	end
	local best, bestD, seen = nil, nil, {}
	for _, list in pairs(m or {}) do
		for _, eid in pairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local comp, p, q = edgeGeomT(eid)
				if comp then
					local d1 = (p[1]-ax)^2 + (p[2]-ay)^2 + (q[1]-bx)^2 + (q[2]-by)^2
					local d2 = (p[1]-bx)^2 + (p[2]-by)^2 + (q[1]-ax)^2 + (q[2]-ay)^2
					local d = math.min(d1, d2)
					if d < (eps * eps) * 2 and (not bestD or d < bestD) then best, bestD = eid, d end
				end
			end
		end
	end
	return best
end

-- Where along an edge a point sits. The originator ships the split POINT rather
-- than its u, because u is a property of that instance's curve; the point is a
-- place in the world both agree on.
function CM.uOnEdge(eid, x, y)
	local comp, a, b, ta, tb = edgeGeomT(eid)
	if not comp then return nil end
	local bestU, bestD
	for i = 1, 399 do
		local u = i / 400
		local q = hermitePos(a, ta, b, tb, u)
		local d = (q[1]-x)^2 + (q[2]-y)^2
		if not bestD or d < bestD then bestU, bestD = u, d end
	end
	return bestU, bestD and math.sqrt(bestD) or nil
end

local function execPolyline(c, planOnly)
	-- ROADC companion: the ORIGINATOR's engine integrated the street as part of
	-- the construction placement itself, so replaying here would double-build
	-- the connector. Peers execute; the originator skips -- same shape as CONP.
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then
		log(string.format("ROADP seq=%s: skipOrigin -- placement already integrated here",
			tostring(c.seq)))
		return
	end
	local planV, planH = {}, {}          -- what THIS pass decided, for the wire
	local usePlanV = CM.planDecode(c.xv)    -- what the originator decided, if it said
	local usePlanH = CM.planDecode(c.xh)
	local ok, err = pcall(function()
		local isTrack = (tonumber(c.etype) or 0) == 1
		local stype = tonumber(c.stype) or 16

		local pts = {}
		for tok in tostring(c.pts or ""):gmatch("[^,]+") do pts[#pts + 1] = tonumber(tok) end
		local links = {}
		for tok in tostring(c.links or ""):gmatch("[^,]+") do links[#links + 1] = tonumber(tok) end
		local tans = {}
		for tok in tostring(c.tans or ""):gmatch("[^,]+") do tans[#tans + 1] = tonumber(tok) end
		-- Bridge/tunnel per LINK: bt = "type,idx,type,idx,..." parallel to links.
		-- Absent (old capture) => ground. Only 1 (bridge) / 2 (tunnel) are
		-- honoured; anything else is treated as ground, so a mis-decoded field
		-- can never produce an unbuildable proposal.
		local bts = {}
		for tok in tostring(c.bt or ""):gmatch("[^,]+") do bts[#bts + 1] = tonumber(tok) end
		local function bridgeOf(k)
			local bT, bI = bts[k * 2 - 1] or 0, bts[k * 2] or -1
			if bT ~= 1 and bT ~= 2 then return 0, -1 end
			return bT, bI
		end

		local np = math.floor(#pts / 3)
		local ne = math.floor(#links / 2)
		local welds = {}
		for tok in tostring(c.weld or ""):gmatch("[^,]+") do welds[#welds + 1] = tonumber(tok) end
		local nw = math.floor(#welds / 3)
		-- UPGRADE removals (upgrade tool: change road/track type, add catenary).
		-- That tool REPLACES edges in place -- nodesToAdd=0, edgesToAdd=N,
		-- edgesToRemove=N, every endpoint an existing node -- so unlike a split,
		-- the peer cannot regenerate the removal from geometry: nothing about the
		-- world says "this edge was upgraded". The removals therefore travel, as
		-- endpoint POSITIONS like everything else on this wire:
		--   rm = "x0,y0,x1,y1;x0,y0,x1,y1;..."   (one entry per removed edge)
		-- and each is matched to THIS peer's own edge below.
		local rms = {}
		for tok in tostring(c.rm or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 4 then rms[#rms + 1] = f end
		end
		-- np may be 0: a road joining two existing junctions carries only edges,
		-- whose endpoints are all positive existing ids resolved by realPos().
		-- ne may ALSO be 0 when the command is pure WELDs: a depot placed with
		-- its mouth ON the street ships no connector at all, only the split.
		if ne < 1 and nw < 1 then
			log(string.format("ROADP: no edges, no welds (np=%d ne=%d) -- ignoring", np, ne))
			return
		end

		-- One split per road edge per proposal. The vertex path (resolve) and the
		-- segment path (crossingsFor) both split roads; the same edge split twice
		-- = two removals of one entity = the ENGINE REJECTS THE WHOLE PROPOSAL
		-- (trace: "vertex 3 split road 182049" then "seg 5 ... 182049 CROSSING",
		-- then build success=false). Second hit reuses the first split's node.
		local base = -(1000000 + CM.originIdx(c.origin) * 10000000 + c.seq * 1000)
		local nextNew, nextEdge = 0, 0
		local sp = api.type.SimpleProposal.new()
		local addNodes, addEdges, removeEdges, removeNodes = {}, {}, {}, {}
		local resolved = {}

		-- ONE removal per edge, ever. A split (regenerated here) and a shipped
		-- upgrade removal can name the same edge, and two removals of one entity
		-- make the engine reject the ENTIRE proposal -- the same failure the
		-- double-split guard exists for. Every removal goes through here.
		local removeSet = {}
		local function dropEdge(eid)
			if removeSet[eid] then return false end
			removeSet[eid] = true
			removeEdges[#removeEdges + 1] = eid
			return true
		end

		-- Match each shipped removal to a LOCAL edge: a node within 1.5 m of each
		-- endpoint (same kind as the command, exactly as resolve() snaps), then
		-- the edge that joins EXACTLY those two nodes. Ids never travel, so this
		-- is the only way a peer can name the edge the originator removed.
		--
		-- An unmatched removal aborts the whole command. Adding an upgrade's new
		-- edges without removing the old ones leaves two edges between the same
		-- pair of nodes -- a doubled road that no later command can clean up --
		-- and that is strictly worse than the upgrade simply not happening.
		for _, r in ipairs(rms) do
			local nA = findNodeNear(isTrack, r[1], r[2], 1.5)
			local nB = findNodeNear(isTrack, r[3], r[4], 1.5)
			local eid
			if nA and nB and nA ~= nB then
				local mm
				if isTrack then
					pcall(function() mm = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
				else
					pcall(function() mm = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
				end
				for _, cand in pairs((mm and mm[nA]) or {}) do
					local be
					pcall(function() be = api.engine.getComponent(cand, api.type.ComponentType.BASE_EDGE) end)
					if be and ((be.node0 == nA and be.node1 == nB)
					           or (be.node0 == nB and be.node1 == nA)) then
						eid = cand
						break
					end
				end
			end
			if not eid then
				local msg = string.format("ROADP seq=%s: removal (%.1f,%.1f)-(%.1f,%.1f) matched no "
					.. "local %s edge (endpoint nodes %s / %s) -- COMMAND SKIPPED (building the "
					.. "replacement without the removal would double the edge)",
					tostring(c.seq), r[1], r[2], r[3], r[4], isTrack and "track" or "street",
					tostring(nA), tostring(nB))
				log(msg)
				CM.cmLog(msg)
				return
			end
			dropEdge(eid)
			log(string.format("ROADP: removal (%.1f,%.1f)-(%.1f,%.1f) -> local edge %d (nodes %d/%d)",
				r[1], r[2], r[3], r[4], eid, nA, nB))
		end

		local function newNodeAt(x, y, z)
			nextNew = nextNew + 1
			local id = base - nextNew
			local n = api.type.NodeAndEntity.new()
			n.entity = id
			n.comp.position = api.type.Vec3f.new(x, y, z)
			addNodes[#addNodes + 1] = n
			return id
		end

		local function newEdge()
			nextEdge = nextEdge + 1
			local e = api.type.SegmentAndEntity.new()
			e.entity = base - 500 - nextEdge
			return e
		end

		local splitRoads = {}   -- ANY existing edge (road or track) eid -> its split node, once per proposal
		-- ONE split for every "new edge crosses an existing edge mid-span" case.
		-- Three separate implementations (vertex/road, vertex/track, segment/road)
		-- drifted: one stamped halves with the NEW polyline's kind, one never
		-- registered its node, and the segment pass then split the same edge
		-- again -> a self-loop edge (n0==n1) and road halves emitted as TRACK
		-- (proposal dump 2026-08-29) -> "Construction not possible". The halves
		-- ALWAYS take the CROSSED edge's kind and props; the node is registered
		-- so nothing splits that edge twice. Single shared node: the form that
		-- builds a real crossing at a road end (user-verified).
		local XING_END_SNAP = 2.5   -- a split this close to an end node IS that node
		local splitShape = {}   -- split node -> {px,py,pz, ax,ay, bx,by} for the wire
		local function splitEdgeAt(eid, u, why, zWant)
			if splitRoads[eid] then return splitRoads[eid] end
			local comp, a, b, ta, tb = edgeGeomT(eid)
			if not comp then return nil end
			-- DEGENERATE SPLIT GUARD (proposal dump 2026-08-29): a vertex 0.5 m from
			-- the crossed edge's END node fell outside findNodeNear's 1.5 m, then
			-- findEdgeContaining returned u~1, and the second half was zero-length:
			-- a self-loop edge (n0==n1 by position) -> "Construction not possible".
			-- Splitting that close to an end is meaningless: use the end node.
			do
				local q = hermitePos(a, ta, b, tb, u)
				local dA = math.sqrt((q[1]-a[1])^2 + (q[2]-a[2])^2)
				local dB = math.sqrt((q[1]-b[1])^2 + (q[2]-b[2])^2)
				if dA < XING_END_SNAP then CM.cmLog(string.format("XING: split of %d at u=%.2f is %.1f m from node0 -> snapping to node %d", eid, u, dA, comp.node0)); return comp.node0 end
				if dB < XING_END_SNAP then CM.cmLog(string.format("XING: split of %d at u=%.2f is %.1f m from node1 -> snapping to node %d", eid, u, dB, comp.node1)); return comp.node1 end
			end
			local crossedIsTrack = false
			pcall(function() crossedIsTrack = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
			local pm = hermitePos(a, ta, b, tb, u)
			local tm = hermiteTangent(a, ta, b, tb, u)
			-- HEIGHT AT A LEVEL CROSSING. x,y come from the crossed edge, but the
			-- HEIGHT must not: at a crossing the originator's node is shared by the
			-- road and the rail, and it is the RAIL that dictates the height there.
			-- Taking the road's interpolated z put the node metres above the rail
			-- line it belongs to -- node z=29.6 between rail vertices at 19.9 and
			-- 24.1 -- and the engine refused the whole proposal with 'Too much
			-- slope' (seq=10, 2026-08-30). Every later replay then referenced
			-- geometry the peer did not have, so one refusal cost five.
			-- zWant is the height the ORIGINATOR's own vertex ended up at, shipped
			-- in the polyline, so using it reproduces A's node exactly.
			local zAt = pm[3]
			if zWant and math.abs(zWant - pm[3]) > 0.05 then
				CM.cmLog(string.format("XING: crossing node z %.2f (road) -> %.2f (shipped rail vertex)", pm[3], zWant))
				zAt = zWant
			end
			local mid = newNodeAt(pm[1], pm[2], zAt)
			-- Keep the shape of this split in world terms (the point, and the
			-- edge it cut named by its endpoints) so it can travel to the peer.
			splitShape[mid] = { pm[1], pm[2], zAt, a[1], a[2], b[1], b[2] }
			dropEdge(eid)
			local function half(nA, nB, tA, tB, sc)
				if nA == nB then return end
				if sc < 0.02 then return end   -- a half under ~2% of the edge is degenerate; never emit it
				local h = newEdge()
				h.comp.node0 = nA
				h.comp.node1 = nB
				h.comp.tangent0 = api.type.Vec3f.new(tA[1]*sc, tA[2]*sc, tA[3]*sc)
				h.comp.tangent1 = api.type.Vec3f.new(tB[1]*sc, tB[2]*sc, tB[3]*sc)
				h.comp.type = 0
				h.comp.typeIndex = -1
				h.type = crossedIsTrack and 1 or 0
				copyEdgeProps(h, eid, crossedIsTrack, nil)   -- the CROSSED edge's own props
				addEdges[#addEdges + 1] = h
			end
			half(comp.node0, mid, ta, tm, u)
			half(mid, comp.node1, tm, tb, 1 - u)
			splitRoads[eid] = mid
			log(string.format("ROADP: split %s edge %d at u=%.2f (%s)", crossedIsTrack and "track" or "road", eid, u, tostring(why)))
			CM.cmLog(string.format("XING: split %s edge %d at u=%.2f -> node %d (%.1f,%.1f) [%s]", crossedIsTrack and "TRACK" or "road", eid, u, mid, pm[1], pm[2], tostring(why)))
			return mid
		end

		-- Resolve one endpoint, splitting if it lands mid-span.
		local function resolve(i)
			if resolved[i] then return resolved[i] end
			local x, y, z = pts[i * 3 - 2], pts[i * 3 - 1], pts[i * 3]

			-- The originator already decided this one. Follow it rather than
			-- deriving our own answer from a world that may have drifted.
			local told = usePlanV[i] and usePlanV[i][1]
			if told then
				if told.kind == "N" then
					local n = findNodeNear(isTrack, told[1], told[2], 1.5)
						or findNodeNear(not isTrack, told[1], told[2], 1.5)
					if n then
						CM.cmLog(string.format("PLAN: vertex %d -> node %d at %.1f,%.1f (as the originator resolved it)", i, n, told[1], told[2]))
						resolved[i] = n; return n
					end
					CM.cmLog(string.format("PLAN: vertex %d: no node at %.1f,%.1f -- deriving locally", i, told[1], told[2]))
				elseif told.kind == "S" then
					local eid = CM.findEdgeByEnds(false, told[4], told[5], told[6], told[7])
						or CM.findEdgeByEnds(true, told[4], told[5], told[6], told[7])
					if eid then
						local u = CM.uOnEdge(eid, told[1], told[2])
						if u then
							local mid = splitEdgeAt(eid, u, "vertex " .. i .. " (originator's split)", told[3])
							if mid then
								CM.cmLog(string.format("PLAN: vertex %d -> split edge %d at %.1f,%.1f z=%.2f (as the originator did)", i, eid, told[1], told[2], told[3]))
								resolved[i] = mid; return mid
							end
						end
					end
					CM.cmLog(string.format("PLAN: vertex %d: no edge %.1f,%.1f--%.1f,%.1f here -- deriving locally", i, told[4], told[5], told[6], told[7]))
				end
			end

			local existing = findNodeNear(isTrack, x, y, 1.5)
			if existing then
				planV[#planV + 1] = string.format("%d,N,%.2f,%.2f,%.2f", i, x, y, z)
				resolved[i] = existing; return existing
			end

			-- LEVEL CROSSING (rail over road). The build tool splits the RAIL at
			-- the crossing, so the crossing is always a rail VERTEX sitting on the
			-- road -- never mid-segment (trace: every hit at rail u=0.00/1.00).
			-- Same-kind snapping above never sees a road, so the vertex was planted
			-- as a fresh node 0 m from the road node, unshared => no crossing (seen
			-- on A and B). For a rail vertex: snap to a STREET node, else split the
			-- street edge underfoot (road-typed halves) and route through it.
			if isTrack then
				local rnode = findNodeNear(false, x, y, 4.0)
				if rnode then
					log(string.format("ROADP: level crossing -- rail vertex %d shares road node %d", i, rnode))
					CM.cmLog(string.format("XING: vertex %d snapped to road node %d (%.1f,%.1f)", i, rnode, x, y))
					-- SHARING A NODE MEANS SHARING ITS HEIGHT, and the road's height
					-- is not the rail's. Measured 2026-08-31: the shipped vertex sat
					-- at z=27.17 and the road node 0.8 m away at z=29.20, so the rail
					-- had to climb 3.07 m over 31.6 m (9.7%) into it instead of
					-- 1.04 m (3.3%) -- 'Too much slope', and the player's build
					-- vanished on their own screen. The originator's engine moves the
					-- road to meet the rail at a crossing; the shipped z IS the
					-- height its build settled on, so ask for the same move by
					-- carrying the existing node into the proposal at the rail's
					-- height. Small differences are left alone: re-heighting every
					-- crossing by a centimetre is churn the engine does not need.
					local rp = CM.nodePosXYZ(rnode)
					if rp and math.abs(rp[3] - z) > 0.25 then
						local mv = api.type.NodeAndEntity.new()
						mv.entity = rnode                     -- an EXISTING id: move, not add
						mv.comp.position = api.type.Vec3f.new(rp[1], rp[2], z)
						addNodes[#addNodes + 1] = mv
						CM.cmLog(string.format("XING: road node %d re-heighted %.2f -> %.2f to meet the rail", rnode, rp[3], z))
					end
					planV[#planV + 1] = string.format("%d,N,%.2f,%.2f,%.2f", i, x, y, z)
					resolved[i] = rnode; return rnode
				end
				local reid, ru
				pcall(function() reid, ru = findEdgeContaining(false, x, y) end)
				if reid then
					local mid = splitEdgeAt(reid, ru, "rail vertex " .. i .. " on road", z)
					if mid then
						local sh = splitShape[mid]
						if sh then planV[#planV + 1] = string.format("%d,S,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", i, sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7]) end
						resolved[i] = mid; return mid
					end
				end
			end

			local eid, u
			local okFind = pcall(function() eid, u = findEdgeContaining(isTrack, x, y) end)
			if okFind and eid then
				local mid = splitEdgeAt(eid, u, "vertex " .. i .. " mid-span")
				if mid then
					local sh = splitShape[mid]
					if sh then planV[#planV + 1] = string.format("%d,S,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", i, sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7]) end
					resolved[i] = mid; return mid
				end
			end

			local id = newNodeAt(x, y, z)
			resolved[i] = id
			return id
		end

		-- WELD v2: reproduce the ORIGINATOR's street integration exactly.
		--
		-- buildConstruction runs the template, and the template builds its own
		-- apron -- at RAW, unsnapped coordinates (measured: outer end 0.6 m off
		-- the street; the engine's real placement SNAPS it onto the split node).
		-- Welding into that raw node kinks the road: one depot took it, two got
		-- 'Construction not possible', and even the success diverges the e-hash
		-- by the snap delta forever. So: REPLACE the raw apron -- remove its edge
		-- and orphan node, plant the split node at the originator's exact snapped
		-- position, rebuild apron and halves into it. Positions in, positions
		-- out; both worlds end identical in endpoint space.
		local ap = {}
		for tok in tostring(c.apron or ""):gmatch("[^,]+") do ap[#ap + 1] = tonumber(tok) end
		if #ap ~= 9 then ap = nil end
		for k = 1, nw do
			local wx, wy, wz = welds[k * 3 - 2], welds[k * 3 - 1], welds[k * 3]
			local Mout = findNodeNear(isTrack, wx, wy, 1.5)
			local eid, u
			pcall(function() eid, u = findEdgeContaining(isTrack, wx, wy, Mout) end)
			if not eid then
				log(string.format("WELD: no street edge under %.1f,%.1f -- skipped", wx, wy))
			else
				local comp, a, b, ta, tb = edgeGeomT(eid)
				if comp then
					local tm = hermiteTangent(a, ta, b, tb, u)
					local X = newNodeAt(wx, wy, wz)
					dropEdge(eid)
					local function halfW(nA, nB, tA, tB, sc)
						local h = newEdge()
						h.comp.node0 = nA
						h.comp.node1 = nB
						h.comp.tangent0 = api.type.Vec3f.new(tA[1]*sc, tA[2]*sc, tA[3]*sc)
						h.comp.tangent1 = api.type.Vec3f.new(tB[1]*sc, tB[2]*sc, tB[3]*sc)
						h.comp.type = 0
						h.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
						h.type = isTrack and 1 or 0
						copyEdgeProps(h, eid, isTrack, stype)
						addEdges[#addEdges + 1] = h
					end
					halfW(comp.node0, X, ta, tm, u)
					halfW(X, comp.node1, tm, tb, 1 - u)
					-- retire the template's RAW apron, if it is where we expect:
					-- a node at the weld position whose ONE edge heads inward.
					-- Anything else stays untouched.
					if Mout and Mout ~= comp.node0 and Mout ~= comp.node1 then
						local lst
						pcall(function()
							local mm = isTrack
								and api.engine.system.streetSystem.getNode2TrackEdgeMap()
								or api.engine.system.streetSystem.getNode2StreetEdgeMap()
							lst = mm[Mout]
						end)
						local raw = {}
						for _, reid in pairs(lst or {}) do raw[#raw + 1] = reid end
						if #raw == 1 then
							dropEdge(raw[1])
							removeNodes[#removeNodes + 1] = Mout
							log(string.format("WELD: replacing raw apron edge %d + node %d",
								raw[1], Mout))
						elseif #raw > 1 then
							log(string.format("WELD: raw node %d has %d edges -- left alone",
								Mout, #raw))
						end
					end
					-- rebuild the apron at the originator's EXACT geometry
					if ap then
						local Min = findNodeNear(isTrack, ap[1], ap[2], 1.5)
						            or newNodeAt(ap[1], ap[2], ap[3])
						local e2 = newEdge()
						e2.comp.node0 = Min
						e2.comp.node1 = X
						e2.comp.tangent0 = api.type.Vec3f.new(ap[4], ap[5], ap[6])
						e2.comp.tangent1 = api.type.Vec3f.new(ap[7], ap[8], ap[9])
						e2.comp.type = 0
						e2.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
						e2.type = isTrack and 1 or 0
						copyEdgeProps(e2, eid, isTrack, stype)
						addEdges[#addEdges + 1] = e2
					end
					log(string.format("WELD v2: edge %d split at u=%.2f into new node at "
						.. "%.1f,%.1f apron=%s", eid, u, wx, wy, tostring(ap ~= nil)))
				end
			end
		end

		-- ---------- LEVEL CROSSINGS (rail over road) ----------
		-- resolve() only splits an edge that an ENDPOINT lands on, and only an
		-- edge of the SAME kind. A rail that passes THROUGH a road mid-segment
		-- touches neither case: the originator's build tool split the road at
		-- the crossing and fused the rail into it, but the peer replayed a bare
		-- polyline over an un-split road -- rejected, or built with no crossing.
		-- Recreate it from positions, like every other split: sample each rail
		-- segment's real Hermite curve, find a STREET edge under it interior to
		-- both, split that road there (ROAD-typed halves), and route the rail
		-- through the new node as two edges so the engine makes a real crossing.
		-- Analytic crossing finder. The sampled version (3 m steps, 3 m band, plus
		-- findEdgeContaining's coarse end-distance pre-filter) missed a real
		-- crossing: a rail crosses a road's centreline at ONE point, and a 3 m
		-- step straddles a 3 m band. So: enumerate street edges near the segment
		-- directly from the node->edge map, sample the ROAD's Hermite curve finely,
		-- and find the closest approach between the two curves. Accept when they
		-- come within CROSS_BAND, interior to both.
		local CROSS_BAND, CROSS_END_MIN = 5.0, 4.0   -- band = road half-width + margin: A and B both measured the rail 4.5 m off the centreline at a real crossing (2026-08-29)
		local xingNodes = {}   -- nodes the rail was routed through as level crossings (probe below)

		-- ONE-SHOT API PROBE: does the Lua proposal expose a railroad-crossing list?
		-- Crossings are NOT inferred by the engine -- they are an explicit
		-- RailroadCrossingProposalData carried beside the StreetProposal (RE:
		-- AddRailroadCrossings consumes that list; every fn touching it is a
		-- consumer). If Lua can reach it, filling it is the fix.
		if not CM.xingApiProbed then
			CM.xingApiProbed = true
			pcall(function()
				local sp0 = api.type.SimpleProposal.new()
				local f = {}
				pcall(function() for k, _ in pairs(sp0) do f[#f + 1] = tostring(k) end end)
				CM.cmLog("XING-API: SimpleProposal fields: " .. table.concat(f, ", "))
				local g = {}
				pcall(function() for k, _ in pairs(sp0.streetProposal) do g[#g + 1] = tostring(k) end end)
				CM.cmLog("XING-API: streetProposal fields: " .. table.concat(g, ", "))
				local names = {}
				pcall(function() for k, _ in pairs(api.type) do if tostring(k):lower():find("cross") then names[#names + 1] = tostring(k) end end end)
				CM.cmLog("XING-API: api.type *cross*: " .. table.concat(names, ", "))
				local cmdn = {}
				pcall(function() for k, _ in pairs(api.cmd.make) do if tostring(k):lower():find("cross") or tostring(k):lower():find("street") or tostring(k):lower():find("track") then cmdn[#cmdn + 1] = tostring(k) end end end)
				CM.cmLog("XING-API: api.cmd.make *cross/street/track*: " .. table.concat(cmdn, ", "))
			end)
		end
		local function crossingsFor(k, n0, n1, x0, y0, z0, x1, y1, z1, T0, T1)
			local hits = {}
			if not isTrack then return hits end
			local a, b = { x0, y0, z0 }, { x1, y1, z1 }
			local chord = math.sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2)
			if chord < 2 * CROSS_END_MIN then return hits end
			-- rail samples (fine: every ~1 m)
			local rs = math.max(8, math.floor(chord / 1.0))
			local rail = {}
			for si = 0, rs do rail[si] = hermitePos(a, T0, b, T1, si / rs) end
			-- candidate street edges: any whose node lies within reach of the segment's bbox
			local minx, maxx = math.min(x0, x1) - 60, math.max(x0, x1) + 60
			local miny, maxy = math.min(y0, y1) - 60, math.max(y0, y1) + 60
			-- candidates from BOTH maps: streets (level crossing) and tracks (a rail
			-- crossing another rail is a plain junction -- same split, no crossing
			-- component). Placeholder ids (<0) are never in these maps.
			local cand, considered = {}, 0
			local nMap, nIn, minD = 0, 0, 1e9
			local mx, my = (x0 + x1) / 2, (y0 + y1) / 2
			for _, getter in ipairs({ api.engine.system.streetSystem.getNode2StreetEdgeMap, api.engine.system.streetSystem.getNode2TrackEdgeMap }) do
				local m
				pcall(function() m = getter() end)
				if m then
					for nid, edges in pairs(m) do
						nMap = nMap + 1
						local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
						local pnode = nc and nc.position
						if pnode then
							local px, py = pnode.x or pnode[1], pnode.y or pnode[2]
							local d = math.sqrt((px - mx) ^ 2 + (py - my) ^ 2); if d < minD then minD = d end
							if px >= minx and px <= maxx and py >= miny and py <= maxy then
								nIn = nIn + 1
								for _, eid in pairs(edges) do if eid > 0 then cand[eid] = true end end
							end
						end
					end
				end
			end
			CM.cmLog(string.format("XING: seg %d bbox x[%.0f,%.0f] y[%.0f,%.0f] mid=(%.0f,%.0f) mapNodes=%d inBox=%d nearestNode=%.1f m",
				k, minx, maxx, miny, maxy, mx, my, nMap, nIn, minD))
			for eid in pairs(cand) do
				considered = considered + 1
				local comp, ra, rb, rta, rtb = edgeGeomT(eid)
				if comp then
					local rlen = math.max(math.sqrt((rb[1]-ra[1])^2 + (rb[2]-ra[2])^2), 1)
					local ss = math.min(600, math.max(10, math.floor(rlen / 0.5)))
					local bestD, bestRu, bestU = nil, nil, nil
					for sj = 1, ss - 1 do
						local ru = sj / ss
						local q = hermitePos(ra, rta, rb, rtb, ru)
						for si = 0, rs do
							local r = rail[si]
							local d = (q[1]-r[1])^2 + (q[2]-r[2])^2
							if not bestD or d < bestD then bestD, bestRu, bestU = d, ru, si / rs end
						end
					end
					local dist = bestD and math.sqrt(bestD) or 1e9
					local reason
					if dist > CROSS_BAND then reason = "too far"
					elseif splitRoads[eid] or comp.node0 == n0 or comp.node1 == n0 or comp.node0 == n1 or comp.node1 == n1 then
						-- ADJACENCY, not a crossing. A branch never crosses the edge it
						-- branches FROM: a track joining a bridge 6 m before the bridge's
						-- end node diverged at 13 deg, sat inside the band, and was routed
						-- THROUGH that end node -- duplicating the split's second half ->
						-- 'Construction not possible' (proposal dump 2026-08-29). An edge
						-- this proposal split (the parent) or one sharing an endpoint with
						-- the segment only ever TOUCHES it.
						reason = "adjacent (split parent / shares an endpoint) -- not a crossing"
					else
						local q = hermitePos(ra, rta, rb, rtb, bestRu)
						local dA = math.sqrt((q[1]-ra[1])^2 + (q[2]-ra[2])^2)
						local dB = math.sqrt((q[1]-rb[1])^2 + (q[2]-rb[2])^2)
						local r = rail[math.floor(bestU * rs + 0.5)] or rail[0]
						local dEnd = math.min(math.sqrt((r[1]-x0)^2 + (r[2]-y0)^2), math.sqrt((r[1]-x1)^2 + (r[2]-y1)^2))
						if dEnd < CROSS_END_MIN then reason = "at rail end (endpoint case, handled by resolve)"
						elseif dA < CROSS_END_MIN or dB < CROSS_END_MIN then
							-- The rail crosses the road AT ONE OF ITS NODES (the road is
							-- already split there, e.g. a junction or a prior crossing).
							-- Nothing to split -- but the rail MUST be routed THROUGH that
							-- node so the engine fuses a level crossing. Building the rail
							-- edge over the node without sharing it = no crossing (seen
							-- live on A and B: closest 0.00 m at road u=0.01/0.99).
							local nid = (dA < dB) and comp.node0 or comp.node1
							hits[#hits + 1] = { node = nid, u = bestU }; reason = "CROSSING at road node " .. tostring(nid)
						else hits[#hits + 1] = { eid = eid, ru = bestRu, u = bestU }; reason = "CROSSING (mid-edge split)" end
					end
					CM.cmLog(string.format("XING: seg %d vs street edge %d: closest %.2f m (road u=%.2f, rail u=%.2f) -> %s",
						k, eid, dist, bestRu or -1, bestU or -1, reason))
				end
			end
			CM.cmLog(string.format("XING: seg %d: %d street edge(s) considered, %d crossing(s)", k, considered, #hits))
			-- the two road halves meeting at a crossing node BOTH report that node;
			-- keep one hit per node so the rail is not split twice at the same spot.
			local seenNode, uniq = {}, {}
			for _, h in ipairs(hits) do
				if not h.node or not seenNode[h.node] then
					if h.node then seenNode[h.node] = true end
					uniq[#uniq + 1] = h
				end
			end
			hits = uniq
			table.sort(hits, function(p, q) return p.u < q.u end)
			return hits
		end
		-- NATIVE CROSSING SHAPE (read live from 7 crossings the game built itself,
		-- 2026-08-29): a level crossing is NOT one shared node. The road is cut
		-- into THREE pieces -- half, a short CONNECTOR spanning the rail's
		-- footprint, half -- and the rail is cut at BOTH connector ends. Every
		-- node: 2 street + 2 track edges, all type=0 typeIndex=-1. Connector
		-- length = W / sin(angle) (W ~ 5 m single track: measured 5.2/4.9 m;
		-- 9.8 m double). Near-perpendicular (> ~80 deg) collapses to ONE node.
		-- A single shared node where the road halves MEET overlaps road on both
		-- sides of the rail -> the engine's collision check rejects it ("Collision"
		-- even for a minimal control). Returns the list of crossing nodes in rail
		-- order, each with its u along the rail.
		-- segment-pass crossing: split the crossed edge ONCE via splitEdgeAt; an
		-- edge already split (by a vertex, or an earlier segment) is SKIPPED, never
		-- reused -- reuse is what produced the self-loop.
		local function splitRoadAt(eid, ru)
			if splitRoads[eid] then
				CM.cmLog(string.format("XING: edge %d already split this proposal -> skip", eid))
				return nil
			end
			return splitEdgeAt(eid, ru, "segment crossing")
		end

		for k = 1, ne do
			local i, j = links[k * 2 - 1], links[k * 2]
			local n0, n1
			if i and j and i >= 1 and j >= 1 and i <= np and j <= np and i ~= j then n0, n1 = resolve(i), resolve(j) end
			-- The game splits a rail at a crossing into two vertices a few metres
			-- apart; both resolve to the SAME split node (the second reuses it), so
			-- the short edge between them collapses to mid->mid: a self-loop the
			-- engine rejects (proposal dump 2026-08-29). Skip such a link.
			if n0 and n1 and n0 == n1 then
				CM.cmLog(string.format("XING: link %d (%d->%d) resolves to the same node %s -> skipped", k, i, j, tostring(n0)))
			elseif n0 and n1 then
				local x0, y0, z0 = pts[i * 3 - 2], pts[i * 3 - 1], pts[i * 3]
				local x1, y1, z1 = pts[j * 3 - 2], pts[j * 3 - 1], pts[j * 3]
				-- crossing pass: split the rail segment into a chain through every
				-- road it crosses. Tangents come from the capture (or the chord).
				local tb0 = (k - 1) * 6
				local T0 = tans[tb0 + 6] and { tans[tb0+1], tans[tb0+2], tans[tb0+3] } or { x1 - x0, y1 - y0, z1 - z0 }
				local T1 = tans[tb0 + 6] and { tans[tb0+4], tans[tb0+5], tans[tb0+6] } or { x1 - x0, y1 - y0, z1 - z0 }
				local hits
				if usePlanH[k] then
					-- The originator already found this segment's crossings. Match
					-- each to a local edge/node by position; anything we cannot
					-- place, we simply do not invent.
					hits = {}
					for _, told in ipairs(usePlanH[k]) do
						if told.kind == "N" then
							local n = findNodeNear(false, told[1], told[2], 1.5)
							if n then hits[#hits + 1] = { node = n, u = told[4] or 0.5 }
							else CM.cmLog(string.format("PLAN: link %d crossing node %.1f,%.1f absent here -- skipped", k, told[1], told[2])) end
						elseif told.kind == "S" then
							local eid = CM.findEdgeByEnds(false, told[4], told[5], told[6], told[7])
							if eid then
								local ru = CM.uOnEdge(eid, told[1], told[2])
								if ru then hits[#hits + 1] = { eid = eid, ru = ru, u = told[8] or 0.5, zWant = told[3] } end
							else CM.cmLog(string.format("PLAN: link %d crossing edge %.1f,%.1f--%.1f,%.1f absent here -- skipped", k, told[4], told[5], told[6], told[7])) end
						end
					end
					CM.cmLog(string.format("PLAN: link %d -> %d crossing(s) from the originator", k, #hits))
				else
					hits = crossingsFor(k, n0, n1, x0, y0, z0, x1, y1, z1, T0, T1)
				end
				if #hits > 0 then
					local chain = { { node = n0, u = 0 } }
					local railLen = math.sqrt((x1 - x0) ^ 2 + (y1 - y0) ^ 2)
					for _, h in ipairs(hits) do
						if h.node then
							chain[#chain + 1] = { node = h.node, u = h.u }      -- existing road node
							xingNodes[#xingNodes + 1] = h.node
							local np2 = CM.nodePosXYZ(h.node)
							if np2 then planH[#planH + 1] = string.format("%d,N,%.2f,%.2f,%.2f,%.4f", k, np2[1], np2[2], np2[3], h.u or 0.5) end
						else
							local mid = splitRoadAt(h.eid, h.ru)
							if mid then
								chain[#chain + 1] = { node = mid, u = math.max(0.001, math.min(0.999, h.u)) }
								xingNodes[#xingNodes + 1] = mid
								local sh = splitShape[mid]
								if sh then planH[#planH + 1] = string.format("%d,S,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f", k, sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7], h.u or 0.5) end
							end
						end
					end
					table.sort(chain, function(p, q) return p.u < q.u end)
					chain[#chain + 1] = { node = n1, u = 1 }
					-- GUARD: never two consecutive identical nodes (=> a self-loop edge)
					local dedup = { chain[1] }
					for ci = 2, #chain do if chain[ci].node ~= dedup[#dedup].node then dedup[#dedup + 1] = chain[ci] end end
					chain = dedup
					for ci = 1, #chain - 1 do
						local ua, ub = chain[ci].u, chain[ci + 1].u
						local ta = hermiteTangent({ x0, y0, z0 }, T0, { x1, y1, z1 }, T1, ua)
						local tb2 = hermiteTangent({ x0, y0, z0 }, T0, { x1, y1, z1 }, T1, ub)
						local sc = ub - ua
						local e = newEdge()
						e.comp.node0 = chain[ci].node
						e.comp.node1 = chain[ci + 1].node
						e.comp.tangent0 = api.type.Vec3f.new(ta[1]*sc, ta[2]*sc, ta[3]*sc)
						e.comp.tangent1 = api.type.Vec3f.new(tb2[1]*sc, tb2[2]*sc, tb2[3]*sc)
						e.comp.type, e.comp.typeIndex = bridgeOf(k)
						e.type = 1
						e.trackEdge = api.type.BaseEdgeTrack.new()
						e.trackEdge.trackType = tonumber(c.ttype) or 1
						e.trackEdge.catenary = (tonumber(c.cat) or 0) == 1
						e.streetEdge = api.type.BaseEdgeStreet.new()
						e.streetEdge.streetType = stype or 16
						addEdges[#addEdges + 1] = e
					end
					log(string.format("ROADP: segment %d routed through %d crossing node(s)", k, #hits))
				else
				local e = newEdge()
				e.comp.node0 = n0
				e.comp.node1 = n1
				-- Real tangents from the capture. The chord (x1-x0, ...) makes a
				-- straight Hermite segment, which is why curved rail replicated as
				-- a polygon of its control points. Fall back to the chord only if
				-- the capture did not carry them.
				local tb = (k - 1) * 6
				if tans[tb + 6] then
					e.comp.tangent0 = api.type.Vec3f.new(tans[tb+1], tans[tb+2], tans[tb+3])
					e.comp.tangent1 = api.type.Vec3f.new(tans[tb+4], tans[tb+5], tans[tb+6])
				else
					e.comp.tangent0 = api.type.Vec3f.new(x1 - x0, y1 - y0, z1 - z0)
					e.comp.tangent1 = api.type.Vec3f.new(x1 - x0, y1 - y0, z1 - z0)
				end
				-- ground: type 0 / typeIndex -1 (native edges, road AND rail; 0 broke
				-- the crossing tests). Bridge/tunnel: from the capture's tail.
				e.comp.type, e.comp.typeIndex = bridgeOf(k)
				if e.comp.type ~= 0 then
					CM.cmLog(string.format("BRIDGE: ROADP seq=%s link %d -> type=%d typeIndex=%d", tostring(c.seq), k, e.comp.type, e.comp.typeIndex))
				end
				e.type = isTrack and 1 or 0
				if isTrack then
					e.trackEdge = api.type.BaseEdgeTrack.new()
					e.trackEdge.trackType = tonumber(c.ttype) or 1
					-- Catenary: low byte of edge record +0x64, established by a
					-- ground-truth sweep (8 paired samples, on=01 off=00 every time).
					e.trackEdge.catenary = (tonumber(c.cat) or 0) == 1
					-- A street edge is mandatory even on a track:
					-- ResTypeRep<StreetType>::Get(-1) asserts without one.
					e.streetEdge = api.type.BaseEdgeStreet.new()
					e.streetEdge.streetType = 16
				else
					e.streetEdge = api.type.BaseEdgeStreet.new()
					e.streetEdge.streetType = stype
					e.streetEdge.hasBus = false
					e.streetEdge.tramTrackType = 0
				end
				addEdges[#addEdges + 1] = e
				end   -- (no crossings: the original single-edge build)
			end
		end

		-- NO TWO EDGES BETWEEN THE SAME PAIR OF NODES. Splitting an edge emits
		-- its two halves; if the polyline then runs from the split point back to
		-- the node one of those halves already reaches, the proposal carries the
		-- same connection twice and the engine refuses the whole thing with
		-- 'Construction not possible'. Measured 2026-08-31 on both machines at
		-- once: edge[1] 47902->mid and edge[2] mid->47903 (the halves), then
		-- edge[3] mid->47902 -- the same pair as edge[1], reversed. It happens
		-- when a rail branches off an existing track just too far from a node to
		-- snap to it, so the vertex splits the edge instead.
		local seenPair, keep = {}, {}
		for _, e in ipairs(addEdges) do
			local a, b
			pcall(function() a, b = e.comp.node0, e.comp.node1 end)
			local key = nil
			if a and b then key = (a < b) and (a .. ":" .. b) or (b .. ":" .. a) end
			if key and seenPair[key] then
				CM.cmLog(string.format("XING: dropped a second edge between nodes %s and %s "
					.. "-- the split halves already connect them", tostring(a), tostring(b)))
			else
				if key then seenPair[key] = true end
				keep[#keep + 1] = e
			end
		end
		addEdges = keep

		for i, n in ipairs(addNodes) do sp.streetProposal.nodesToAdd[i] = n end
		for i, e in ipairs(addEdges) do sp.streetProposal.edgesToAdd[i] = e end
		-- Removals go in the SAME proposal as the halves that replace them.
		-- Split across two commands the world is briefly inconsistent, and on a
		-- peer that is a desync rather than a flicker. These ids are LOCAL --
		-- found by this peer on its own copy -- so nothing depends on ids
		-- matching across instances.
		for i, rid in ipairs(removeEdges) do sp.streetProposal.edgesToRemove[i] = rid end
		for i, rid in ipairs(removeNodes) do sp.streetProposal.nodesToRemove[i] = rid end

		-- PLAN PASS. The originator runs this same code once at schedule time
		-- purely to find out what it will do, so the decisions can travel with
		-- the command. Nothing is built here: same resolution, same splits, no
		-- proposal. (This guard first landed in execEdge by a replace-first
		-- mistake, where it was dead -- and the plan pass BUILT the road at
		-- click time. Review, 2026-08-31.)
		if planOnly then return end

		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, buildContext(), false),
			function(res, success)
				-- LEVEL-CROSSING PROBE. The engine models a crossing as its own ECS
				-- component (RAILROAD_CROSSING, added by construction_util_engine).
				-- Log whether the node we routed the rail through actually got it,
				-- and once, dump a NATIVE crossing's fields as ground truth.
				if #xingNodes > 0 then
					CM.cmLog(string.format("XING: build success=%s, probing %d crossing node(s)", tostring(success), #xingNodes))
					for _, nid in ipairs(xingNodes) do
						local negId = nid < 0
						local realId = nid
						if negId and res and res.resultEntities then
							-- a placeholder id resolves to a real entity in resultEntities; we
							-- can't map it precisely here, so probe by position instead
							realId = nil
						end
						local comp, err = nil, nil
						if realId and realId > 0 then
							local ok, e = pcall(function() return api.engine.getComponent(realId, api.type.ComponentType.RAILROAD_CROSSING) end)
							if ok then comp = e else err = e end
						end
						CM.cmLog(string.format("XING: node %s -> RAILROAD_CROSSING %s%s", tostring(nid),
							(realId and realId > 0) and (comp and "PRESENT" or "ABSENT") or "(placeholder id, probe by position on next poll)",
							err and (" err=" .. tostring(err)) or ""))
						if comp then
							local fields = {}
							pcall(function() for k, v in pairs(comp) do fields[#fields + 1] = tostring(k) .. "=" .. tostring(v) end end)
							CM.cmLog("XING:   fields: " .. table.concat(fields, ", "))
						end
					end
				end
				log(string.format("EXEC ROADP seq=%s origin=%s at=%s pts=%d edges=%d " ..
					"removed=%d (shipped rm=%d) %s=%d success=%s",
					tostring(c.seq), tostring(c.origin), tostring(c.at), np, #addEdges,
					#removeEdges, #rms, isTrack and "trackType" or "streetType",
					isTrack and (tonumber(c.ttype) or 1) or stype, tostring(success)))
				if not success then
					-- best-effort: surface WHY. "success=false" alone cost a
					-- full capture-and-stare cycle to diagnose a Narrow angle.
					pcall(function()
						local pd = res.resultProposalData
						local es = pd and pd.errorState
						if es then
							local msgs = ""
							pcall(function()
								for i = 1, #es.messages do
									msgs = msgs .. " '" .. tostring(es.messages[i]) .. "'"
								end
							end)
							log(string.format("ROADP FAIL detail: critical=%s%s",
								tostring(es.critical), msgs))
							CM.cmLog(string.format("XING: ROADP FAIL detail: critical=%s%s", tostring(es.critical), msgs))
							-- Dump EVERYTHING: every field of the error state, and the full
							-- proposal (nodes, edges, removals). Every theory so far was
							-- reconstructed after the fact; this is the ground truth.
							pcall(function()
								local ef = {}
								for _, k in ipairs({"critical","messages","warnings","collisionEntities","errorEntities","entities","info"}) do
									local v = nil; pcall(function() v = es[k] end)
									if v ~= nil then
										local sv = tostring(v)
										if type(v) == "userdata" or type(v) == "table" then
											local parts = {}; pcall(function() for j = 1, 12 do local x = v[j]; if x == nil then break end; parts[#parts + 1] = tostring(x) end end)
											if #parts > 0 then sv = "[" .. table.concat(parts, ",") .. "]" end
										end
										ef[#ef + 1] = k .. "=" .. sv
									end
								end
								CM.cmLog("XING: errorState fields: " .. table.concat(ef, " | "))
							end)
							pcall(function()
								for i, n in ipairs(addNodes) do
									local p = n.comp.position
									CM.cmLog(string.format("XING: PROPOSAL node[%d] id=%d pos=(%.2f,%.2f,%.2f)", i, n.entity, p.x, p.y, p.z))
								end
								for i, e in ipairs(addEdges) do
									local t0, t1 = e.comp.tangent0, e.comp.tangent1
									CM.cmLog(string.format("XING: PROPOSAL edge[%d] id=%d kind=%s type=%d typeIndex=%d n0=%d n1=%d t0=(%.1f,%.1f,%.2f) t1=(%.1f,%.1f,%.2f)",
										i, e.entity, (e.type == 1) and "TRACK" or "street", e.comp.type, e.comp.typeIndex, e.comp.node0, e.comp.node1, t0.x, t0.y, t0.z, t1.x, t1.y, t1.z))
								end
								for i, rid in ipairs(removeEdges) do CM.cmLog(string.format("XING: PROPOSAL removeEdge[%d] = %d", i, rid)) end
								for i, rid in ipairs(removeNodes) do CM.cmLog(string.format("XING: PROPOSAL removeNode[%d] = %d", i, rid)) end
							end)
						end
					end)
				end
			end)
	end)
	if not ok then
		log("execPolyline error: " .. tostring(err))
		-- the pcall swallowed a fault mid-polyline: every crossing trace stopped
		-- after seg 1 with no error on disk (2026-08-29). Name it.
		CM.cmLog("XING: execPolyline ERROR: " .. tostring(err))
	end
	-- What this pass decided, for the originator to put on the wire. Empty on a
	-- peer that just followed a plan -- it has nothing to tell anyone.
	return CM.planEncode(planV), CM.planEncode(planH)
end

-- ---------- constructions: HYBRID replication ----------
--
-- Stations and depots cannot go through the strict capture-cancel-replay path
-- that roads use. Measured today: a script-built construction proposal is
-- rejected by make_cmd::BuildProposal itself (error `false`, hook never fires),
-- and the params.modules map is a native map<int, ModuleInfo> that is not in
-- the proposal bytes at all. So for constructions:
--
--   1. the ORIGINATOR lets the build happen locally (the hook does not cancel
--      caller 419f62), then reads fileName / params / transf back off the
--      resulting entity and schedules a CONP command carrying all three;
--   2. every peer replays it at the stamp with game.interface.buildConstruction
--      + setPlayer -- the path mp_bridge measured live, including a 16-module
--      modular station -- and the originator skips its own command.
--
-- Cost: the originator applies at T0 and peers at T (~0.7s later), so this is
-- not strict lockstep for constructions. Entity ids may differ across peers as
-- a result, which is consistent with the rest of the design: nothing addresses
-- a construction by id on the wire.
K.MAX_SER_DEPTH = 8
function ser(v, depth)
	depth = depth or 0
	local t = type(v)
	if t == "number" or t == "boolean" then return tostring(v) end
	if t == "string" then return string.format("%q", v) end
	if t == "table" then
		if depth >= K.MAX_SER_DEPTH then return "{}" end
		-- Sorted keys: the same table always serialises to the same text.
		local keys = {}
		for k in pairs(v) do keys[#keys + 1] = k end
		table.sort(keys, function(a, b)
			local ta, tb = type(a), type(b)
			if ta ~= tb then return ta < tb end
			if ta == "number" or ta == "string" then return a < b end
			return tostring(a) < tostring(b)
		end)
		local parts = {}
		for _, k in ipairs(keys) do
			local key = (type(k) == "number") and ("[" .. k .. "]")
			                                   or ("[" .. string.format("%q", k) .. "]")
			local inner = ser(v[k], depth + 1)
			if inner then parts[#parts + 1] = key .. "=" .. inner end
		end
		return "{" .. table.concat(parts, ",") .. "}"
	end
	return nil   -- functions/userdata: omit rather than substitute a wrong type
end

local function deserParams(pstr)
	if not pstr or pstr == "" then return nil end
	local chunk = load("return " .. pstr, "params")
	if not chunk then return nil end
	local ok, v = pcall(chunk)
	if ok and type(v) == "table" then return v end
	return nil
end

local knownCons    = {}      -- construction ids already seen (or primed)
local ownershipPending = {}  -- buildable con id -> polls waited for PLAYER_OWNED
local consPrimed   = false   -- first poll only records what exists
-- Names travel percent-escaped: the wire is whitespace-tokenised and a
-- construction name ("Neckargemünd Train depot") has spaces.
local function escName(s)
	return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end
local function unescName(s)
	return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

local expectedCons = {}      -- posKey -> true: our own replay is about to land here
-- Pairing buffers for CONX: the hook's ROADC street payload arrives within a
-- tick of the placement; the construction itself is noticed by a poll up to
-- K.CON_POLL_EVERY ticks later. Whichever comes first waits for the other.
local pendingRoadc = {}      -- { at, posOf, adds, rms, spos, etype, stype, ttype, cat }
local pendingCons  = {}      -- { at, file, t, params, x, y }

-- Horizontal only, same reasoning as node matching: the engine settles z.
local function conKey(x, y) return string.format("%.1f/%.1f", x, y) end

local function execConP(c)
	if c.origin == K.INSTANCE then
		log(string.format("CONP seq=%d: originator already built it locally, skipping", c.seq))
		return
	end
	local ok, err = pcall(function()
		local t = {}
		for tok in tostring(c.t or ""):gmatch("[^,]+") do t[#t + 1] = tonumber(tok) end
		if #t ~= 16 then log("CONP: bad transf, " .. #t .. " numbers"); return end
		local params = deserParams(c.params) or {}
		-- A used seed drives errorState critical, which is a fatal assert rather
		-- than a rejected build. Strip it; the engine assigns a fresh one.
		params.seed = nil
		local key = conKey(t[13], t[14])
		expectedCons[key] = true
		if c.company then CM.cmExpectedCompany[key] = tonumber(c.company); CM.cmEnsure(); CM.cmExpectedBal0[key] = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
			CM.cmLog(string.format("CM: CONP bal0 snapshot key=%s mePid=%s bal0=%s", key, tostring(CM.cmCompanyPid[CM.cmMyCompany]), tostring(CM.cmExpectedBal0[key]))) end
		local built, newId = pcall(game.interface.buildConstruction, c.file, params, t)
		if built and newId then
			-- buildConstruction takes no player, so the result is unowned and the
			-- UI will not let you click it. setPlayer fixes that (mp_bridge).
			pcall(function() game.interface.setPlayer(newId, api.engine.util.getPlayer()) end)
		else
			expectedCons[key] = nil
		end
		log(string.format("EXEC CONP seq=%s origin=%s at=%s file=%s ok=%s id=%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.file),
			tostring(built and newId ~= nil), tostring(newId)))
	end)
	if not ok then log("execConP error: " .. tostring(err)) end
end

-- ---------- station EDITS ----------
--
-- Two shapes of edit, both measured by mp_bridge: adding a module through the
-- UI changes params IN PLACE (same entity id), while upgradeConstruction --
-- what the peer uses to replay -- REPLACES the entity (old id retires, a new
-- one appears at the same spot). So constructions are tracked by POSITION, and
-- a new id at an already-known position is an edit, never a new build.
local consByKey    = {}   -- conKey -> { id=, file=, params=<ser string> }
local expectedEdit = {}   -- conKey -> true: our own replayed edit is about to replace the entity
local primeQueue   = {}   -- ids from the first poll, classified a few per tick

local function findConNear(file, x, y, maxDist)
	local best, bestD
	for key, rec in pairs(consByKey) do
		if rec.file == file then
			local kx, ky = key:match("^([-%d.]+)/([-%d.]+)$")
			kx, ky = tonumber(kx), tonumber(ky)
			if kx and ky then
				local d = (kx - x) ^ 2 + (ky - y) ^ 2
				if d <= maxDist * maxDist and (not bestD or d < bestD) then best, bestD = rec, d end
			end
		end
	end
	return best
end

local function execConU(c)
	if c.origin == K.INSTANCE then
		log(string.format("CONU seq=%d: originator already applied it, skipping", c.seq))
		return
	end
	local ok, err = pcall(function()
		local x, y = tonumber(c.x), tonumber(c.y)
		local rec = findConNear(c.file, x, y, 10)
		if not rec then
			log(string.format("CONU: no %s within 10 m of %.1f,%.1f -- ignoring", tostring(c.file), x, y))
			return
		end
		local alive = false
		pcall(function() alive = api.engine.entityExists(rec.id) end)
		if not alive then log("CONU: target id " .. rec.id .. " is gone -- ignoring"); return end
		local params = deserParams(c.params) or {}
		params.seed = nil
		local key = conKey(x, y)
		expectedEdit[key] = true
		local uok, uerr = pcall(game.interface.upgradeConstruction, rec.id, c.file, params)
		log(string.format("EXEC CONU seq=%s origin=%s at=%s file=%s target=%d ok=%s%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.file), rec.id,
			tostring(uok), uok and "" or (" err=" .. tostring(uerr))))
		if uok then
			rec.params = c.params
		else
			expectedEdit[key] = nil
			-- Which of the two causes? A no-op self-upgrade with the OWN params of
			-- the entity must succeed on anything the engine will upgrade at all.
			-- If it fails, the construction itself cannot be upgraded here --
			-- mp_bridge saw exactly that on joiner-side stations created by
			-- buildConstruction -- and the fix belongs in the BUILD replay.
			local sok, serr = pcall(function()
				local e = game.interface.getEntity(rec.id)
				local own = e and e.params or {}
				own.seed = nil
				return game.interface.upgradeConstruction(rec.id, c.file, own)
			end)
			log(string.format("CONU diagnostic self-upgrade ok=%s%s -> %s", tostring(sok),
				sok and "" or (" err=" .. tostring(serr)),
				sok and "our wire params are bad for this construction"
				    or "this construction cannot be upgraded here at all (build-replay problem)"))
		end
	end)
	if not ok then log("execConU error: " .. tostring(err)) end
end

local function transfAt(x, y, z)
	return { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 }
end

-- Build a construction (depot, station, ...).
--
-- Params travel in the command rather than being looked up locally. Two peers
-- must feed buildConstruction byte-identical params or they get different
-- buildings from the "same" command -- and `seed` in particular must be absent,
-- since reusing one drives errorState critical, which is a fatal assert rather
-- than a rejected proposal.
local function execCon(c)
	local ok, err = pcall(function()
		local params = { year = 1850, paramX = 0, paramY = 0 }
		local built, id = pcall(game.interface.buildConstruction,
			c.file, params, transfAt(c.x, c.y, c.z))
		log(string.format("EXEC CON seq=%s origin=%s at=%s file=%s success=%s id=%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.file),
			tostring(built and id ~= nil), tostring(id)))
	end)
	if not ok then log("exec CON error: " .. tostring(err)) end
end

-- Demolish whatever construction sits nearest a position.
--
-- Targeting by POSITION, not entity id, on purpose. Ids are assigned in
-- creation order; under lockstep with an identical command history they should
-- match across peers, but that is an assumption this prototype has not verified,
-- and a wrong id here bulldozes the wrong building. A position is derived from
-- the command itself and cannot drift. The id tie-break below only matters for
-- exact-distance ties, which are vanishingly rare.
-- conKey -> true: a bulldoze we are about to perform on THIS peer as a replay,
-- so the removal-detector must not ship it straight back.
local expectedDemolish = {}

-- Rejoin a road at a node our replay split, when nothing is attached there any
-- more.
--
-- A construction's own split belongs to the construction: the engine froze it,
-- so removing the depot heals the road back into one edge. Our replayed split is
-- a plain pair of street edges that no construction owns, so a peer that splits
-- for a depot which then fails, gets rolled back or is demolished keeps a
-- degree-2 node forever -- and every world hash from then on differs by exactly
-- those edges. Measured 2026-08-30 on a live two-machine session: four such
-- nodes on the peer, 1193 edges against the host's 1189, desyncs climbing on
-- every hash tick with nothing left actually diverging.
--
-- Only ever touches a plain mid-road node: exactly two street edges, no track,
-- nothing else hanging off it. Anything else is left alone and logged.
function CM.healNodeAt(x, y, why)
	local healed = false
	pcall(function()
		local nid = findNodeNear(false, x, y, 1.5)
		if not nid then return end
		local tm
		pcall(function() tm = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
		if tm and tm[nid] then
			for _ in pairs(tm[nid]) do
				log(string.format("HEAL(%s): node %d carries track too -- left alone", why, nid)); return
			end
		end
		local m
		pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
		local ids = {}
		if m and m[nid] then for _, eid in pairs(m[nid]) do ids[#ids + 1] = eid end end
		if #ids ~= 2 then
			log(string.format("HEAL(%s): node %d has %d street edge(s), not 2 -- left alone", why, nid, #ids))
			return
		end
		-- Far endpoints, and the tangent at each pointing along far1 -> far2.
		-- Tangents in a BaseEdge always run node0 -> node1, so a half whose node0
		-- is the split node has to be read backwards.
		local far, tng, len = {}, {}, {}
		for i, eid in ipairs(ids) do
			local comp, a, b, ta, tb = edgeGeomT(eid)
			if not comp then return end
			local fn, fp, ft
			if comp.node0 == nid then
				fn, fp, ft = comp.node1, b, tb
				if i == 1 then ft = { -ft[1], -ft[2], -ft[3] } end   -- far1: point back at the split
			else
				fn, fp, ft = comp.node0, a, ta
				if i == 2 then ft = { -ft[1], -ft[2], -ft[3] } end   -- far2: point away from the split
			end
			far[i], tng[i] = { fn, fp }, ft
			local dx, dy, dz = b[1] - a[1], b[2] - a[2], b[3] - a[3]
			len[i] = math.sqrt(dx * dx + dy * dy + dz * dz)
		end
		if far[1][1] == far[2][1] then
			log(string.format("HEAL(%s): node %d joins one edge to itself -- left alone", why, nid)); return
		end
		-- Splitting an edge at parameter u scales the halves' tangents by u and
		-- (1-u). Undo that so the rejoined edge keeps the road's curve instead of
		-- straightening it; u is recovered from the halves' lengths.
		local total = (len[1] or 0) + (len[2] or 0)
		local u = (total > 0.001) and (len[1] / total) or 0.5
		local s1 = (u > 0.01) and (1.0 / u) or 1.0
		local s2 = ((1 - u) > 0.01) and (1.0 / (1 - u)) or 1.0
		local sp = api.type.SimpleProposal.new()
		local e = api.type.SegmentAndEntity.new()
		e.entity = -1
		e.comp.node0 = far[1][1]
		e.comp.node1 = far[2][1]
		e.comp.tangent0 = api.type.Vec3f.new(tng[1][1] * s1, tng[1][2] * s1, tng[1][3] * s1)
		e.comp.tangent1 = api.type.Vec3f.new(tng[2][1] * s2, tng[2][2] * s2, tng[2][3] * s2)
		e.comp.type = 0
		e.comp.typeIndex = -1
		e.type = 0
		copyEdgeProps(e, ids[1], false, nil)
		sp.streetProposal.edgesToAdd[1] = e
		sp.streetProposal.edgesToRemove[1] = ids[1]
		sp.streetProposal.edgesToRemove[2] = ids[2]
		-- The node has to go with them. Dropping only the two edges leaves the
		-- engine to reconcile an orphan node and it refuses the whole proposal
		-- with critical=true and 'Internal error (see console for details)'
		-- (measured on the live peer, 2026-08-30).
		sp.streetProposal.nodesToRemove[1] = nid
		local cmd = api.cmd.make.buildProposal(sp, nil, true)
		if not cmd then return end
		healed = true
		api.cmd.sendCommand(cmd, function(res, ok2)
			local extra = ""
			if not ok2 then
				pcall(function()
					local es = res.resultProposalData and res.resultProposalData.errorState
					if es then
						extra = " critical=" .. tostring(es.critical)
						for i = 1, #es.messages do extra = extra .. " '" .. tostring(es.messages[i]) .. "'" end
					end
				end)
			end
			log(string.format("HEAL(%s): node %d at %.1f,%.1f rejoined: %s%s",
				why, nid, x, y, tostring(ok2), extra))
		end)
	end)
	return healed
end

-- Every node a replay of ours cut into a road, with the tick it was cut at.
CM.splitWatch = {}
function CM.watchSplit(x, y)
	CM.splitWatch[string.format("%.1f/%.1f", x, y)] = { x, y, ticks }
end

-- Sweep the watched splits. A split that is doing its job carries the
-- construction's access as a third edge; one left with exactly two is a scar
-- from a construction that never landed, was rolled back, or has since been
-- demolished, and the other instance does not have it. Healing is symmetric --
-- both instances watch their own splits -- so the worlds converge either way.
--
-- The delay keeps the sweep off builds that are still in flight; a construction
-- and its split always arrive in one proposal, but a retry may be queued behind.
CM.SPLIT_SETTLE = 27       -- ticks before a scar counts as one: ~0.19 s each, so ~5 s
-- Something was demolished at x,y: every split we ever cut nearby goes back
-- under watch, so a scar it leaves is healed by the next sweep.
function CM.rearmSplitsNear(x, y)
	for k, site in pairs(CM.splitSites or {}) do
		local dx, dy = site[1] - x, site[2] - y
		if dx * dx + dy * dy < 60 * 60 then CM.splitWatch[k] = { site[1], site[2], ticks } end
	end
end

function CM.sweepSplits()
	for k, w in pairs(CM.splitWatch) do
		if ticks - w[3] > CM.SPLIT_SETTLE then
			local nid = findNodeNear(false, w[1], w[2], 1.5)
			local n = 0
			if nid then
				local m
				pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
				if m and m[nid] then for _ in pairs(m[nid]) do n = n + 1 end end
			end
			if not nid then
				CM.splitWatch[k] = nil                       -- already gone
			elseif n ~= 2 then
				-- In use today. But the construction it serves can be demolished
				-- later, and on this instance nothing owns the split -- keep the
				-- site, and re-arm the watch whenever something near it is
				-- demolished (review, 2026-08-31: the sweep forgot it forever).
				CM.splitSites = CM.splitSites or {}
				CM.splitSites[k] = { w[1], w[2] }
				CM.splitWatch[k] = nil
			else
				CM.splitWatch[k] = nil
				CM.healNodeAt(w[1], w[2], "orphaned split")
			end
		end
	end
end


local function execDemolish(c)
	-- The originator already bulldozed its own construction (the removal
	-- detector fired BECAUSE it was gone locally); only the peer replays.
	if c.origin == K.INSTANCE then
		log(string.format("DEMOLISH seq=%s: originator already bulldozed locally, skipping", tostring(c.seq)))
		return
	end
	local ok, err = pcall(function()
		local best, bestD
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, id in pairs(ents) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.transf then
				local dx, dy = co.transf[13] - c.x, co.transf[14] - c.y
				local d = dx * dx + dy * dy
				if d < 900 and (not bestD or d < bestD or (d == bestD and id < best)) then
					best, bestD = id, d
				end
			end
		end
		if not best then
			log(string.format("EXEC DEMOLISH seq=%s: nothing within 30m of %.1f,%.1f",
				tostring(c.seq), c.x, c.y))
			return
		end
		-- Mark the spot so our own removal-detector recognises this as a replay
		-- (our construction is about to vanish) and does not echo it back.
		expectedDemolish[conKey(c.x, c.y)] = true
		-- companies mode: a remote company's copy was setBulldozeable(false)'d to
		-- stop the local HUMAN removing it -- but that lock also refuses OUR
		-- scripted replay of the owner's own demolish. Unlock first, exactly as
		-- the "Multiplayer Companies" mod does before every removal (it always
		-- setBulldozeable(true)s ahead of bulldoze). Guarded by the CONSTRUCTION
		-- check since setBulldozeable asserts (uncatchable) on other entity types.
		local owner = CM.cmOwnerOf(best)
		local unlocked = "n/a"
		if CM.cmMode == "companies" then
			local hasCon = false
			pcall(function() hasCon = api.engine.getComponent(best, api.type.ComponentType.CONSTRUCTION) ~= nil end)
			if hasCon then
				local okU, errU = pcall(function() game.interface.setBulldozeable(best, true) end)
				unlocked = tostring(okU) .. (errU and (" " .. tostring(errU)) or "")
			else unlocked = "skipped(noCON)" end
		end
		local dok, derr = pcall(game.interface.bulldoze, best)
		CM.rearmSplitsNear(c.x, c.y)
		log(string.format("EXEC DEMOLISH seq=%s origin=%s at=%s id=%d success=%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), best, tostring(dok)))
		CM.cmLog(string.format("CM: DEMOLISH seq=%s id=%d owner=%s unlock=%s bulldoze ok=%s err=%s",
			tostring(c.seq), best, tostring(owner), unlocked, tostring(dok), tostring(derr)))
	end)
	if not ok then log("exec DEMOLISH error: " .. tostring(err)) end
end

-- ---------- vehicles: cross-peer identity ----------
--
-- Entity ids differ between instances, so nothing on the wire names a vehicle
-- by id. A replicated purchase gets a shared KEY, origin:seq; each peer records
-- which LOCAL vehicle that purchase produced -- the vehicle in the target depot
-- that was not known before -- and later commands ship keys. A vehicle that was
-- in the save carries the same id on both peers (same file) and is keyed s:<id>.
local vehKeyOf, vehIdOf = {}, {}       -- localId -> key, key -> localId
local knownVeh = {}                    -- every vehicle id seen in a depot so far
local primedVeh = {}                   -- vehicles that existed at load: same ids on both peers
local pendingVehKeys = {}              -- { key=, depot=<child>, since= }
local vehPrimed = false

-- Vehicles parked in a construction's depot. NOT game.interface.getDepotVehicles:
-- that call errors for the construction AND for the VEHICLE_DEPOT child
-- (measured, probe P10), which is why every purchase key timed out. A parked
-- vehicle's getEntity().depot is the CONSTRUCTION id (mp_bridge depotPosOf),
-- so enumerate vehicles and filter.
-- A depot-parked vehicle is NOT a world entity: getEntities(type="VEHICLE")
-- never lists it (measured: 79 before and after a purchase) and
-- game.interface.getDepotVehicles errors for construction and child alike.
-- transportVehicleSystem.getVehiclesWithState(IN_DEPOT) does list them; the
-- vehicle's TRANSPORT_VEHICLE.depot names its depot -- accept the construction
-- id or any of its VEHICLE_DEPOT children, whichever the engine stores.
local function depotVehicles(constructionId)
	local ids = {}
	pcall(function()
		local accept = { [constructionId] = true }
		local co = api.engine.getComponent(constructionId, api.type.ComponentType.CONSTRUCTION)
		if co and co.depots then
			for i = 1, #co.depots do accept[co.depots[i]] = true end
		end
		local tvs = api.engine.system.transportVehicleSystem
		local parked = tvs.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
		for i = 1, #parked do
			local v = parked[i]
			local ok, tv = pcall(function() return api.engine.getComponent(v, api.type.ComponentType.TRANSPORT_VEHICLE) end)
			if ok and tv and accept[tv.depot] then ids[#ids + 1] = v end
		end
	end)
	table.sort(ids)
	return ids
end

local function registerVehKey(key, vid)
	vehKeyOf[vid] = key
	vehIdOf[key] = vid
	knownVeh[vid] = true
	log(string.format("veh: %s <-> local vehicle %d", key, vid))
end

-- s:<id> is only valid for a vehicle both peers loaded from the save. A new
-- vehicle whose purchase key never registered must NOT fall back to its id:
-- the same number on the peer is a different vehicle (or none -- measured:
-- the peer's sellVehicle threw on a stale id).
local function vehKeyFor(vid)
	if vehKeyOf[vid] then return vehKeyOf[vid] end
	if primedVeh[vid] then return "s:" .. tostring(vid) end
	log(string.format("veh: local vehicle %s has no cross-peer key -- not shipped", tostring(vid)))
	return nil
end

local function vehIdFor(key)
	if vehIdOf[key] then return vehIdOf[key] end
	local s = tostring(key):match("^s:(%-?%d+)$")
	local id = s and tonumber(s) or nil
	if id and primedVeh[id] then return id end
	return nil
end

-- Prime knownVeh from every player depot once constructions are primed.
local function primeVehKeys()
	-- consByKey fills at K.PRIME_PER_TICK per tick after consPrimed; priming the
	-- depot lists before that drained saw an empty table ('primed 0'), and a
	-- purchase would then have resolved to an OLD parked vehicle. Wait for the
	-- queue, and treat every save vehicle as known regardless.
	if vehPrimed or not consPrimed or #primeQueue > 0 then return end
	vehPrimed = true
	local n = 0
	pcall(function()
		local all = game.interface.getEntities({ radius = 999999 },
			{ type = "VEHICLE", includeData = false }) or {}
		for _, v in pairs(all) do primedVeh[v] = true; knownVeh[v] = true end
	end)
	local np = 0
	for _ in pairs(primedVeh) do np = np + 1 end
	log(string.format("veh: primed %d save vehicle(s) as known / s:<id>", np))
end

-- Resolve pending purchase keys: the depot's vehicle that is not yet known.
local function pollVehKeys()
	if #pendingVehKeys == 0 then return end
	local now = gameTime()
	if not now then return end
	-- Oldest pending key first, each taking the smallest unknown id: two
	-- purchases landing in one tick keep their identities in order.
	local resolved = {}
	for i = 1, #pendingVehKeys do
		local p = pendingVehKeys[i]
		local fresh, total = {}, 0
		for _, v in ipairs(depotVehicles(p.depot)) do
			total = total + 1
			if not knownVeh[v] then fresh[#fresh + 1] = v end
		end
		table.sort(fresh)
		if #fresh >= 1 then
			registerVehKey(p.key, fresh[1])       -- oldest new id first
			-- companies mode: a remote company's purchase landed on our player;
			-- hand the vehicle over and move the cost (balance delta since apply).
			if p.company then
				-- A vehicle ON A LINE inherits its line's owner; setPlayer on it
				-- trips a FATAL engine assert (interface.cpp:2340, seen live). The
				-- Companies mod's rule (v10 followsLine): reassign the LINE, skip the
				-- vehicle. Only a depot/unassigned vehicle needs its own setPlayer.
				local onLine = false
				pcall(function()
					local tv = api.engine.getComponent(fresh[1], api.type.ComponentType.TRANSPORT_VEHICLE)
					onLine = (tv ~= nil and tv.line ~= nil and tv.line ~= -1 and tv.line ~= 0)
				end)
				if onLine then CM.cmLog(string.format("CM: vehicle %d is on a line -> follows its line, setPlayer skipped", fresh[1]))
				else CM.cmReassignEntity(fresh[1], p.company, "vehicle") end
				local nowBal = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
				if p.bal0 and nowBal then CM.cmTransferCost(p.company, p.bal0 - nowBal, "VBUY " .. p.key) end
			end
			resolved[#resolved + 1] = i
		elseif now - p.since > 6 then
			log(string.format("veh: key %s never produced a vehicle in depot %s (%d listed) -- dropped",
				p.key, tostring(p.depot), total))
			resolved[#resolved + 1] = i
		end
	end
	for k = #resolved, 1, -1 do table.remove(pendingVehKeys, resolved[k]) end
end

-- A sold vehicle's key must not outlive it: entity ids get reused.
local function forgetVehicle(vid)
	local key = vehKeyOf[vid]
	if key then vehIdOf[key] = nil end
	vehKeyOf[vid] = nil
end

local function expectVehicle(key, depotChild, company)
	-- companies mode: remember the origin company and our balance BEFORE the
	-- purchase lands, so the bind step can hand the vehicle over and move the
	-- exact cost (balance delta) to that company.
	local bal0 = company and CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany]) or nil
	pendingVehKeys[#pendingVehKeys + 1] = { key = key, depot = depotChild, since = gameTime() or 0, company = company, bal0 = bal0 }
end

-- ---------- vehicles: BuyVehicle replication ----------
--
-- Optimistic-local like constructions: the originator's buy proceeds natively
-- (never cancelled -- a cancelled BuyVehicle is untested territory and a wrong
-- vehicle type in a depot is an uncatchable native assert), and the peer buys
-- the SAME config into the depot found at the SAME position. Model ids travel
-- as file names, depots as positions; nothing on the wire is an entity id.
-- Vehicle identity for later commands (sell / line / send-to-depot) is a
-- separate problem, not solved here.
-- ---------- lines: cross-peer identity + Create / Update / Delete ----------
--
-- Same shape as vehicles: a created line gets the key origin:seq, each peer
-- records which LOCAL line entity appeared for it (lineSystem.getLines() minus
-- the known set), save lines are s:<id>. Content is READ BACK from the entity
-- (name, colour, waitingTime, stops) and stops travel as station-group
-- POSITIONS; the peer finds its own station group within 20 m.
local lineKeyOf, lineIdOf = {}, {}
local knownLines, primedLines = {}, {}
local pendingLineKeys = {}          -- { key=, since= }   (peer: waiting for its replayed line)
local pendingLineCreates = {}       -- { since= }         (originator: waiting to read the new line)
local linesPrimed = false

local function allLines()
	local ids = {}
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do ids[#ids + 1] = ls[i] end
	end)
	table.sort(ids)
	return ids
end

local function registerLineKey(key, lid)
	lineKeyOf[lid] = key
	lineIdOf[key] = lid
	knownLines[lid] = true
	log(string.format("line: %s <-> local line %d", key, lid))
end

local function lineKeyFor(lid)
	if lineKeyOf[lid] then return lineKeyOf[lid] end
	if primedLines[lid] then return "s:" .. tostring(lid) end
	log(string.format("line: local line %s has no cross-peer key -- not shipped", tostring(lid)))
	return nil
end

local function lineIdFor(key)
	if lineIdOf[key] then return lineIdOf[key] end
	local s = tostring(key):match("^s:(%-?%d+)$")
	local id = s and tonumber(s) or nil
	if id and primedLines[id] then return id end
	return nil
end

local function forgetLine(lid)
	local key = lineKeyOf[lid]
	if key then lineIdOf[key] = nil end
	lineKeyOf[lid] = nil
	-- ids get reused: a deleted line's id must not stay 'known', or the next
	-- line to reuse it is invisible to pairing and never replicates.
	knownLines[lid] = nil
	primedLines[lid] = nil
end

local function stationGroupPos(sg)
	local ok, e = pcall(game.interface.getEntity, sg)
	if ok and e and e.position then return e.position[1] or e.position.x, e.position[2] or e.position.y end
	return nil
end

local function findStationGroupNear(x, y)
	local best, bestD
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "STATION_GROUP", includeData = false }) or {}
		for _, sg in pairs(ents) do
			local sx, sy = stationGroupPos(sg)
			if sx then
				local d = (sx - x) ^ 2 + (sy - y) ^ 2
				if d < 400 and (not bestD or d < bestD) then best, bestD = sg, d end
			end
		end
	end)
	return best
end

-- name / colour / wait / stops of a line, ready for the wire; nil if a stop
-- cannot be located (it would be unreplayable anyway)
local function lineSnapshot(lid)
	local snap
	pcall(function()
		local lc = api.engine.getComponent(lid, api.type.ComponentType.LINE)
		if not lc or not lc.stops then return end
		local stops = {}
		for i = 1, #lc.stops do
			local s = lc.stops[i]
			local x, y = stationGroupPos(s.stationGroup)
			if not x then return end
			stops[#stops + 1] = string.format("%.2f,%.2f,%d,%d,%d,%d,%d", x, y,
				tonumber(s.station) or 0, tonumber(s.terminal) or 0, tonumber(s.loadMode) or 0,
				tonumber(s.minWaitingTime) or 0, tonumber(s.maxWaitingTime) or 180)
		end
		local name = ""
		pcall(function() name = game.interface.getName(lid) or "" end)
		local r, g, b = 0.9, 0.2, 0.2
		pcall(function()
			local cc = api.engine.getComponent(lid, api.type.ComponentType.COLOR)
			if cc and cc.color then r, g, b = cc.color.x or cc.color[1], cc.color.y or cc.color[2], cc.color.z or cc.color[3] end
		end)
		snap = { name = escName(name), color = string.format("%.3f,%.3f,%.3f", r, g, b),
		         wait = tonumber(lc.waitingTime) or 180, stops = table.concat(stops, ";") }
	end)
	return snap
end

local function primeLineKeys()
	if linesPrimed then return end
	linesPrimed = true
	local n = 0
	for _, lid in ipairs(allLines()) do primedLines[lid] = true; knownLines[lid] = true; n = n + 1 end
	log(string.format("line: primed %d save line(s) as known / s:<id>", n))
end

local function pollLineKeys()
	if #pendingLineKeys == 0 and #pendingLineCreates == 0 then return end
	local now = gameTime()
	if not now then return end
	local fresh = {}
	for _, lid in ipairs(allLines()) do if not knownLines[lid] then fresh[#fresh + 1] = lid end end
	-- PEER FIRST, matched by CONTENT: a replayed line is the fresh one whose
	-- stops signature equals the LCREATE we replayed. This is the discriminator
	-- the vehicle path gets from the depot -- without it, a line created
	-- locally on the same instance that is also replaying a peer's line could
	-- be paired with the wrong key (review 2026-08-28).
	for pk = #pendingLineKeys, 1, -1 do
		local p = pendingLineKeys[pk]
		local hit
		for fi = 1, #fresh do
			local snap = lineSnapshot(fresh[fi])
			if snap and (not p.sig or snap.stops == p.sig) then hit = fi; break end
		end
		if hit then
			registerLineKey(p.key, fresh[hit])
			if p.company then CM.cmReassignEntity(fresh[hit], p.company, "line") end   -- companies mode
			table.remove(fresh, hit)
			table.remove(pendingLineKeys, pk)
		end
	end
	-- ORIGINATOR: any remaining fresh line is a local UI creation -> ship it
	while #pendingLineCreates > 0 and #fresh > 0 do
		local lid = table.remove(fresh, 1)
		table.remove(pendingLineCreates, 1)
		local snap = lineSnapshot(lid)
		if snap then
			scheduleLocal("LCREATE", { name = snap.name, color = snap.color, wait = snap.wait,
			                           stops = snap.stops, skipOrigin = 1 })
			registerLineKey(K.INSTANCE .. ":" .. tostring(seqNo), lid)
		else
			knownLines[lid] = true
			log(string.format("line: new line %d could not be read back -- not replicated", lid))
		end
	end
	-- Absorb any straggler: a fresh line nothing claimed must not linger to
	-- mis-pair with a future pending entry.
	for _, lid in ipairs(fresh) do
		if not knownLines[lid] then
			knownLines[lid] = true
			log(string.format("line: unclaimed line %d absorbed as known (unkeyed)", lid))
		end
	end
	for i = #pendingLineCreates, 1, -1 do
		if now - pendingLineCreates[i].since > 6 then table.remove(pendingLineCreates, i); log("line: LCREATE never produced a line -- dropped") end
	end
	for i = #pendingLineKeys, 1, -1 do
		if now - pendingLineKeys[i].since > 6 then log("line: key " .. pendingLineKeys[i].key .. " never produced a line -- dropped"); table.remove(pendingLineKeys, i) end
	end
end

local function buildLineObject(c)
	local lineObj = api.type.Line.new()
	lineObj.waitingTime = tonumber(c.wait) or 180
	local n = 0
	for rec in tostring(c.stops or ""):gmatch("[^;]+") do
		local f = {}
		for v in rec:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
		if #f < 7 then error("bad stop record " .. rec) end
		local sg = findStationGroupNear(f[1], f[2])
		if not sg then error(string.format("no station group within 20 m of %.1f,%.1f", f[1], f[2])) end
		local s = api.type.Line.Stop.new()
		s.stationGroup = sg
		s.station = f[3]
		s.terminal = f[4]
		s.loadMode = f[5]
		s.minWaitingTime = f[6]
		s.maxWaitingTime = f[7]
		n = n + 1
		lineObj.stops[n] = s
	end
	return lineObj, n
end

local function execLine(c)
	if c.origin == K.INSTANCE then
		log(string.format("%s seq=%s: originator already applied locally, skipping", c.op, tostring(c.seq)))
		return
	end
	local ok, err = pcall(function()
		if c.op == "LCREATE" then
			local lineObj, n = buildLineObject(c)
			local r, g, b = tostring(c.color or ""):match("^([^,]+),([^,]+),([^,]+)$")
			local color = api.type.Vec3f.new(tonumber(r) or 0.9, tonumber(g) or 0.2, tonumber(b) or 0.2)
			local name = unescName(c.name)
			local key = tostring(c.origin) .. ":" .. tostring(c.seq)
			api.cmd.sendCommand(api.cmd.make.createLine(name, color, api.engine.util.getPlayer(), lineObj),
				function(res, success)
					log(string.format("EXEC LCREATE seq=%s origin=%s at=%s '%s' stops=%d success=%s",
						tostring(c.seq), tostring(c.origin), tostring(c.at), name, n, tostring(success)))
					if success then pendingLineKeys[#pendingLineKeys + 1] = { key = key, sig = c.stops, since = gameTime() or 0, company = c.company and tonumber(c.company) or nil } end
				end)
		elseif c.op == "LUPDATE" then
			local lid = lineIdFor(c.key)
			if not lid then log(string.format("LUPDATE seq=%s: unknown line key %s", tostring(c.seq), tostring(c.key))); return end
			local lineObj, n = buildLineObject(c)
			api.cmd.sendCommand(api.cmd.make.updateLine(lid, lineObj), function(res, success)
				log(string.format("EXEC LUPDATE seq=%s origin=%s at=%s %s stops=%d success=%s",
					tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.key), n, tostring(success)))
			end)
		elseif c.op == "LDELETE" then
			local lid = lineIdFor(c.key)
			if not lid then log(string.format("LDELETE seq=%s: unknown line key %s", tostring(c.seq), tostring(c.key))); return end
			api.cmd.sendCommand(api.cmd.make.deleteLine(lid), function(res, success)
				log(string.format("EXEC LDELETE seq=%s origin=%s at=%s %s success=%s",
					tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.key), tostring(success)))
				if success then forgetLine(lid) end
			end)
		end
	end)
	if not ok then log(string.format("exec%s error: %s", tostring(c.op), tostring(err))) end
end

-- ---------- names and colours ----------
--
-- SetName and SetColor take an entity, and an entity id means nothing on the
-- other machine -- so what travels is the same key the vehicle and line channels
-- already use, or a position for a construction. kind says which registry to ask,
-- because a vehicle and a line can hold the same number.
local function targetFor(kind, key)
	if kind == "veh" then return vehIdFor(key) end
	if kind == "line" then return lineIdFor(key) end
	if kind == "con" then
		local rec = consByKey[key]
		if rec and rec.id then
			local alive = false
			pcall(function() alive = api.engine.entityExists(rec.id) end)
			if alive then return rec.id end
		end
		local kx, ky = tostring(key):match("^(%-?[%d%.]+)/(%-?[%d%.]+)$")
		if kx then return CM.constructionAt(tonumber(kx), tonumber(ky)) end
	end
	return nil
end

local function execSetName(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		local id = targetFor(tostring(c.kind or ""), tostring(c.key or ""))
		if not id then
			log(string.format("VNAME seq=%s: no local %s for key %s -- skipped",
				tostring(c.seq), tostring(c.kind), tostring(c.key)))
			return
		end
		local name = unescName(tostring(c.name or ""))
		api.cmd.sendCommand(api.cmd.make.setName(id, name), function(_, okc)
			log(string.format("EXEC VNAME seq=%s %s %s -> %d name=%q success=%s",
				tostring(c.seq), tostring(c.kind), tostring(c.key), id, name, tostring(okc)))
		end)
	end)
    if not ok then log("exec VNAME error: " .. tostring(err)) end
end

local function execSetColor(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		local id = targetFor(tostring(c.kind or ""), tostring(c.key or ""))
		if not id then
			log(string.format("VCOLOR seq=%s: no local %s for key %s -- skipped",
				tostring(c.seq), tostring(c.kind), tostring(c.key)))
			return
		end
		local r, g, b = tonumber(c.r) or 0, tonumber(c.g) or 0, tonumber(c.b) or 0
		api.cmd.sendCommand(api.cmd.make.setColor(id, api.type.Vec3f.new(r, g, b)), function(_, okc)
			log(string.format("EXEC VCOLOR seq=%s %s %s -> %d rgb=%.2f,%.2f,%.2f success=%s",
				tostring(c.seq), tostring(c.kind), tostring(c.key), id, r, g, b, tostring(okc)))
		end)
	end)
	if not ok then log("exec VCOLOR error: " .. tostring(err)) end
end

local function execVehCmd(c)
	-- Strict ops replay on the originator ONLY if the slice actually cancelled
	-- the local command (armed=1). A Reverse left to run natively and then
	-- replayed is a toggle applied twice.
	if c.origin == K.INSTANCE and (not K.STRICT_OPS[c.op] or tonumber(c.armed or 1) == 0) then
		log(string.format("%s seq=%s: originator already applied locally, skipping", c.op, tostring(c.seq)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("%s seq=%s: STRICT -- originator replaying at stamp (local was cancelled)", c.op, tostring(c.seq)))
	end
	local ok, err = pcall(function()
		local function resolve(key)
			local id = vehIdFor(key)
			if not id then log(string.format("%s seq=%s: unknown vehicle key %s", c.op, tostring(c.seq), tostring(key))) end
			return id
		end
		local cmds = {}
		if c.op == "VSELL" then
			for key in tostring(c.keys or ""):gmatch("[^,]+") do
				local id = resolve(key)
				if id then cmds[#cmds + 1] = { api.cmd.make.sellVehicle(id), "sell " .. key, id } end
			end
		elseif c.op == "VDEPOT" then
			local id = resolve(c.key)
			if id then cmds[#cmds + 1] = { api.cmd.make.sendToDepot(id, tonumber(c.sell) == 1), "sendToDepot " .. tostring(c.key) } end
		elseif c.op == "VREV" then
			local id = resolve(c.key)
			if id then cmds[#cmds + 1] = { api.cmd.make.reverseVehicle(id), "reverse " .. tostring(c.key) } end
		elseif c.op == "VLINE" then
			local id = resolve(c.key)
			local line = lineIdFor(c.line)
			-- A line the peer has not finished building yet is not a lost cause:
			-- LCREATE and its LUPDATE stops can still be in flight, or waiting on
			-- a station that has not replicated. Retry for a while instead of
			-- dropping the assignment, which leaves that vehicle unassigned on
			-- this instance for good (seen live 2026-08-31: the line's stop could
			-- not be resolved, the line was dropped, and every VLINE for it then
			-- failed).
			if id and not line then
				c.tries = (tonumber(c.tries) or 0) + 1
				if c.tries <= 20 then
					local nowG = gameTime() or 0
					c.at = nowG + 1.0
					-- NOT straight back onto `queue`: this runs from inside the
					-- pump's loop over that table, which rebuilds it afterwards
					-- (`queue = keep`) and would discard the append -- and the
					-- pump also remembers this seq as executed. The retry list is
					-- merged in, and the seq forgiven, at the top of the next pump.
					CM.retryQueue = CM.retryQueue or {}
					CM.retryQueue[#CM.retryQueue + 1] = c
					if c.tries == 1 or c.tries % 10 == 0 then
						log(string.format("VLINE seq=%s: line %s not here yet -- retry %d",
							tostring(c.seq), tostring(c.line), c.tries))
					end
				else
					log(string.format("VLINE seq=%s: line %s never arrived -- vehicle %s left unassigned (DIVERGENCE)",
						tostring(c.seq), tostring(c.line), tostring(c.key)))
				end
				return
			end
			if id and line then
				-- Types, because a wrong one here surfaced as an unreadable Lua
				-- error ("execVLINE error: function: 0000018D4AC0FF60") with
				-- nothing to say which argument was wrong.
				local okMake, made = pcall(api.cmd.make.setLine, id, line, tonumber(c.stop) or 0)
				if okMake and made then
					cmds[#cmds + 1] = { made, "setLine " .. tostring(c.key) }
				else
					log(string.format("VLINE seq=%s: setLine(%s:%s, %s:%s, %s) refused by the maker: %s",
						tostring(c.seq), type(id), tostring(id), type(line), tostring(line),
						tostring(tonumber(c.stop) or 0), tostring(made)))
				end
			end
		end
		for _, pair in ipairs(cmds) do
			local what, vid = pair[2], pair[3]
			api.cmd.sendCommand(pair[1], function(res, success)
				log(string.format("EXEC %s seq=%s origin=%s at=%s %s success=%s",
					c.op, tostring(c.seq), tostring(c.origin), tostring(c.at), what, tostring(success)))
				if success and c.op == "VSELL" and vid then forgetVehicle(vid) end
			end)
		end
	end)
	if not ok then log(string.format("exec%s error: %s", tostring(c.op), tostring(err))) end
end

-- The TransportVehicleConfig a command carries, rebuilt on this instance.
--
-- VBUY and VREPL ship the SAME encoding (name~loads~colour~autoloads;... plus a
-- vehicleGroups list), so they decode it with the same code: a second copy of
-- this would drift the moment one op learned about a new field, and a wrong
-- config is a wrong vehicle in a depot -- an uncatchable native assert away.
-- A global (not a `local function`): the chunk is at Lua 5.1's 200-local limit.
--
-- RAISES on a part this peer cannot build (unknown model, malformed spec). Both
-- callers run it inside their pcall, so the command is logged and skipped
-- instead of half-applied.
function buildVehConfig(c)
	local config = api.type.TransportVehicleConfig.new()
	local u = 0
	for spec in tostring(c.parts or ""):gmatch("[^;]+") do
		local name, loads, col, autos = spec:match("^([^~]*)~([^~]*)~([^~]*)~([^~]*)$")
		if not name then error("bad part spec: " .. spec) end
		local mid = tonumber(name:match("^#(%-?%d+)$") or "")
		if not mid then pcall(function() mid = api.res.modelRep.find(name) end) end
		if not mid or mid < 0 then error("model not found on this peer: " .. name) end
		local part = api.type.VehiclePart.new()
		part.modelId = mid
		local lc = part.loadConfig
		local n = 0
		for v in loads:gmatch("[^/]+") do n = n + 1; lc[n] = tonumber(v) or 0 end
		part.loadConfig = lc
		part.reversed = false     -- offset not yet decoded; TODO sweep
		local r, g, b = col:match("^([^,]+),([^,]+),([^,]+)$")
		part.color = api.type.Vec3f.new(tonumber(r) or -1, tonumber(g) or -1, tonumber(b) or -1)
		part.logo = ""
		local tvp = api.type.TransportVehiclePart.new()
		tvp.purchaseTime = 0
		tvp.maintenanceState = 1.0
		tvp.targetMaintenanceState = 0
		local alc = tvp.autoLoadConfig
		n = 0
		for v in autos:gmatch("[^/]+") do n = n + 1; alc[n] = tonumber(v) or 0 end
		tvp.autoLoadConfig = alc
		tvp.part = part
		u = u + 1
		config.vehicles[u] = tvp
	end
	if u == 0 then error("no parts") end
	local grp = config.vehicleGroups
	local ng = 0
	for v in tostring(c.groups or ""):gmatch("[^/]+") do ng = ng + 1; grp[ng] = tonumber(v) or 1 end
	if ng == 0 then grp[1] = u end
	config.vehicleGroups = grp
	return config, u
end

local function execVBuy(c)
	if c.origin == K.INSTANCE and not K.STRICT_OPS.VBUY then
		log(string.format("VBUY seq=%s: originator already bought locally, skipping", tostring(c.seq)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("VBUY seq=%s: STRICT -- originator replaying buy at stamp (local was cancelled)", tostring(c.seq)))
	end
	local ok, err = pcall(function()
		local x, y = tonumber(c.x), tonumber(c.y)
		if not (x and y) then log("VBUY: no depot position"); return end
		local rec = consByKey[conKey(x, y)]
		local depot = rec and rec.id
		local alive = false
		if depot then pcall(function() alive = api.engine.entityExists(depot) end) end
		if not alive then
			depot = nil
			pcall(function()
				local list = game.interface.getEntities({ pos = { x, y }, radius = 6 },
					{ type = "CONSTRUCTION", includeData = false }) or {}
				for _, id in pairs(list) do depot = depot or id end
			end)
		end
		if not depot then
			log(string.format("EXEC VBUY seq=%s: no depot at %.1f,%.1f -- vehicle NOT bought", tostring(c.seq), x, y))
			return
		end
		-- The depot must be the same KIND as the originator's: a train into a
		-- road depot is a native assert (measured), so refuse rather than risk.
		local want = tostring(c.file or "?")
		if want ~= "?" then
			local fn = ""
			pcall(function()
				local co = api.engine.getComponent(depot, api.type.ComponentType.CONSTRUCTION)
				fn = co and co.fileName and tostring(co.fileName) or ""
			end)
			if fn ~= want then
				log(string.format("EXEC VBUY seq=%s: depot at %.1f,%.1f is '%s', expected '%s' -- refusing",
					tostring(c.seq), x, y, fn, want))
				return
			end
		end
		-- buyVehicle wants the VEHICLE_DEPOT child, not the construction
		local target
		pcall(function()
			local co = api.engine.getComponent(depot, api.type.ComponentType.CONSTRUCTION)
			if co and co.depots and #co.depots >= 1 then target = co.depots[1] end
		end)
		if not target then
			log(string.format("EXEC VBUY seq=%s: construction %d has no depot child -- vehicle NOT bought", tostring(c.seq), depot))
			return
		end
		local config, u = buildVehConfig(c)
		local seq, origin, at = c.seq, c.origin, c.at
		local okM, cmd = pcall(function() return api.cmd.make.buyVehicle(api.engine.util.getPlayer(), target, config) end)
		if not okM or not cmd then
			log(string.format("EXEC VBUY seq=%s: make.buyVehicle refused: %s", tostring(seq), tostring(cmd)))
			return
		end
		api.cmd.sendCommand(cmd, function(res, success)
			log(string.format("EXEC VBUY seq=%s origin=%s at=%s construction=%d depot=%s parts=%d success=%s",
				tostring(seq), tostring(origin), tostring(at), depot, tostring(target), u, tostring(success)))
			if success then expectVehicle(tostring(origin) .. ":" .. tostring(seq), depot, c.company and tonumber(c.company) or nil) end
		end)
	end)
	if not ok then log("execVBuy error: " .. tostring(err)) end
end

-- ---------- vehicles: ReplaceVehicle ----------
--
-- The vehicle window's "replace" (command factory 4: r8 = the vehicle, r9 = a
-- TransportVehicleConfig with the layout VBUY already serialises) swaps a
-- vehicle's configuration in place.
--
-- OPTIMISTIC, like BuyVehicle and for the same reason: the UI waits for the
-- command's result, and cancelling a vehicle command that the UI is waiting on
-- is exactly what wedged the build tool. So the originator's replace applies
-- natively at T0, the peers apply at the stamp, and the originator skips its
-- own replay.
--
-- Identity: the vehicle travels as a cross-peer KEY (VSELL/VDEPOT/VREV all do
-- this), never an entity id. If the engine mints a NEW entity for the replaced
-- vehicle, the key must follow it or every later command for that vehicle
-- resolves to a dead id -- hence the re-registration in the callback.
--
-- A global, not a `local function`: the chunk is at Lua 5.1's 200-local limit.
function execVReplace(c)
	if c.origin == K.INSTANCE and not K.STRICT_OPS.VREPL then
		log(string.format("VREPL seq=%s: originator already replaced locally, skipping",
			tostring(c.seq)))
		return
	end
	local ok, err = pcall(function()
		local key = tostring(c.veh or "")
		local veh = vehIdFor(key)
		if not veh then
			log(string.format("EXEC VREPL seq=%s: unknown vehicle key %s -- replace NOT applied",
				tostring(c.seq), key))
			return
		end
		local config, u = buildVehConfig(c)
		local seq, origin, at = c.seq, c.origin, c.at
		local okM, cmd = pcall(function() return api.cmd.make.replaceVehicle(veh, config) end)
		if not okM or not cmd then
			log(string.format("EXEC VREPL seq=%s: make.replaceVehicle refused: %s",
				tostring(seq), tostring(cmd)))
			return
		end
		-- TODO companies mode: a replace bills the executing player, so the cost
		-- has to move to c.company the way VBUY does it (balance delta captured
		-- around the apply, then CM.cmTransferCost). Not wired yet -- in coop this
		-- is a no-op, in companies mode the buyer's company is under-charged.
		api.cmd.sendCommand(cmd, function(res, success)
			-- The replace may hand back a NEW entity. Take it from the result
			-- rather than assume either way; both shapes have been seen for
			-- entity-returning commands.
			local nid
			pcall(function()
				local r = res and res.resultEntity
				if type(r) == "number" then nid = r
				elseif r ~= nil then nid = tonumber(tostring(r)) end
			end)
			log(string.format("EXEC VREPL seq=%s origin=%s at=%s %s vehicle=%s parts=%d "
				.. "result=%s success=%s",
				tostring(seq), tostring(origin), tostring(at), key, tostring(veh), u,
				tostring(nid), tostring(success)))
			if success and nid and nid > 0 and nid ~= veh then
				forgetVehicle(veh)          -- the old id is dead; ids get reused
				registerVehKey(key, nid)
				log(string.format("VREPL: key %s now names entity %d (was %d)", key, nid, veh))
			end
		end)
	end)
	if not ok then log("execVReplace error: " .. tostring(err)) end
end

-- ---------- constructions: NATIVE replay (CONP / CONX) ----------
--
-- api.cmd.make.buildProposal DOES accept a script-built ConstructionEntity --
-- measured 2026-08-28 (E2c: ok=true, entity created) -- as long as params
-- carry a seed. The 'factory rejects script constructions' finding of
-- 2026-08-17 was a stripped seed, nothing more. So the peer now issues the
-- SAME proposal the originator's UI issued: the construction plus, for CONX,
-- the captured street vectors verbatim (nodes, edges, removed edges). The
-- engine then does its own snapping/integration exactly as it did on the
-- originator, which is what buildConstruction (template at raw coordinates,
-- no integration) could never reproduce. Only positive ids need mapping:
-- the removed street edge is found under the split position, and its two
-- endpoints stand in for the originator's endpoint ids.
-- forward: execConX retries itself once after clearing a colliding town building.
-- The retry must NOT run in the same tick as the clearing bulldozes: those are
-- async commands, so an immediate retry validates against a world where the
-- buildings STILL EXIST (measured: 'cleared 3 -- retrying' then an instant
-- second Collision, while the later diagnostic scan found the footprint empty).
-- ONE construction replay in flight at a time. Placements are asynchronous: a
-- batch dropped in the same tick has every proposal validated against the world
-- as it was at submit time, so two depots splitting the same road (or adjacent
-- pieces of it) fight -- the second removes an edge the first has already
-- replaced and the engine rejects it with critical=true and NO message.
-- Measured 2026-08-30: five depots all stamped at=6.8, numbers 1, 2 and 5 built,
-- 3 and 4 did not. Queueing costs a few tenths of a game unit and the peer's
-- world ends up identical either way.
local conxBusy, conxBusyAt = false, nil
-- Construction replays run STRICTLY IN ORDER, oldest first. They used to share the
-- retry table, which the pump drained back-to-front, so a batch queued as 12,13,14,15
-- replayed 15,14,13,12 and every retry landed after later depots had already built --
-- each one then validated against a world its predecessor had not shaped yet, and the
-- engine rejected it with critical=true and no message ("builds land out of order",
-- 2026-08-30). Entries are { c = command, notBefore = game time }; a retry goes to the
-- FRONT so nothing newer overtakes it.
local conxQueue = {}
local execConX
execConX = function(c)
	if c.origin == K.INSTANCE then
		log(string.format("%s seq=%d: originator already built it locally, skipping", tostring(c.op), c.seq))
		return
	end
	local nowB = gameTime() or 0
	if conxBusy and conxBusyAt and (nowB - conxBusyAt) > 3.0 then
		log(string.format("CONX: previous replay never reported back (%.1f units) -- releasing the queue", nowB - conxBusyAt))
		conxBusy = false
	end
	if conxBusy then
		conxQueue[#conxQueue + 1] = { c = c, notBefore = nowB }
		log(string.format("%s seq=%s: another construction replay is in flight -- queued (%d waiting)",
			tostring(c.op), tostring(c.seq), #conxQueue))
		return
	end
	conxBusy, conxBusyAt = true, nowB
	local ok, err = pcall(function()
		local t = {}
		for tok in tostring(c.t or ""):gmatch("[^,]+") do t[#t + 1] = tonumber(tok) end
		if #t ~= 16 then log("CONX: bad transf, " .. #t .. " numbers"); return end
		local params = deserParams(c.params) or {}
		local key = conKey(t[13], t[14])
		local isTrack = (tonumber(c.etype) or 0) == 1
		local stype = tonumber(c.stype) or 16

		local sp = api.type.SimpleProposal.new()
		local ce = api.type.SimpleProposal.ConstructionEntity.new()
		ce.fileName = tostring(c.file)
		ce.params = params
		ce.transf = api.type.Mat4f.new(
			api.type.Vec4f.new(t[1], t[2], t[3], t[4]),
			api.type.Vec4f.new(t[5], t[6], t[7], t[8]),
			api.type.Vec4f.new(t[9], t[10], t[11], t[12]),
			api.type.Vec4f.new(t[13], t[14], t[15], t[16]))
		ce.playerEntity = api.engine.util.getPlayer()
		-- The name goes IN the proposal. Measured (probe P9, 2026-08-28): with
		-- ce.name set, the apply gives the construction AND its child entities
		-- (VEHICLE_DEPOT / stations) their NAME and PLAYER_OWNED -- exactly a
		-- UI-built depot. Without it the child has neither, and the GUI select
		-- handler dereferences the missing NAME (client crash on click). An
		-- earlier run blamed the name for 'Construction not possible'; the real
		-- cause was the halves' street type.
		ce.name = unescName(c.name)
		sp.constructionsToAdd[1] = ce

		-- street payload (absent for a free-standing CONP)
		local nodes, adds, rms, spos = {}, {}, {}, {}
		local rawPos = {}   -- every shipped street-node position: spans the footprint
		for tok in tostring(c.snodes or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 4 then
				nodes[f[1]] = { f[2], f[3], f[4] }
				rawPos[#rawPos + 1] = { f[2], f[3] }
			end
		end
		for tok in tostring(c.sedges or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			-- 8 fields (legacy) or 10 (+ bridge/tunnel type, typeIndex)
			if #f == 8 or #f == 10 then adds[#adds + 1] = f end
		end
		for tok in tostring(c.srm or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 8 then rms[#rms + 1] = f end
		end
		for tok in tostring(c.spos or ""):gmatch("[^;]+") do
			local f = {}
			for v in tok:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
			if #f == 4 then spos[f[1]] = { f[2], f[3], f[4] } end
		end

		local idmap, nRm = {}, 0
		local splitEdgeOf = {}    -- split node id -> the peer edge it splits
		-- Kept empty: snapping a split onto an existing node was tried and reverted
		-- (see the STUB NUDGE note below). The lookups downstream are no-ops now and
		-- are left in place only so the intent -- 'a split node may alias an existing
		-- one' -- stays visible if the idea is ever revisited with a way to keep the
		-- originator's topology.
		local snapNode = {}
		for _, r in ipairs(rms) do
			-- the split node: the new node with added edges to BOTH endpoints
			local X
			for id, _ in pairs(nodes) do
				local hitA, hitB = false, false
				for _, e in ipairs(adds) do
					if (e[1] == id and e[2] == r[1]) or (e[2] == id and e[1] == r[1]) then hitA = true end
					if (e[1] == id and e[2] == r[2]) or (e[2] == id and e[1] == r[2]) then hitB = true end
				end
				if hitA and hitB then X = id; break end
			end
			local p = X and nodes[X]
			local eid
			if p then pcall(function() eid = findEdgeContaining(isTrack, p[1], p[2]) end) end
			if eid then
				local comp, a, b, ta, tb = edgeGeomT(eid)
				if comp then
					-- orient: the removed edge's start tangent points from r[1]
					-- toward r[2]; our edge runs a -> b.
					local dot = (b[1] - a[1]) * r[3] + (b[2] - a[2]) * r[4]
					if dot >= 0 then idmap[r[1]], idmap[r[2]] = comp.node0, comp.node1
					else idmap[r[1]], idmap[r[2]] = comp.node1, comp.node0 end
					-- STUB GUARD. The originator split ITS road; this peer's road is
					-- nodded differently (town growth drifts the two worlds apart), so
					-- the same world position can land a metre or two from an existing
					-- node here. Splitting there leaves a 1-4 m stub and the engine
					-- rejects the whole proposal with critical=true and NO message --
					-- every failed depot measured on 2026-08-30 was exactly this
					-- (stubs of 1.2, 1.7, 1.9, 2.6, 3.1, 3.9 m). Snap to that node
					-- instead: no split, no halves, no removal, and the depot's own
					-- connector is re-pointed at it below -- the shape the UI itself
					-- produces when you place a depot next to a junction.
					-- Same rule, same reason as execPolyline's XING_END_SNAP.
					-- STUB NUDGE. The originator split ITS road; this peer's road can be
					-- nodded differently (town roads drift between the two worlds), so the
					-- same world position can land a metre or two from an existing node
					-- here and the split would leave a stub the engine refuses -- rejecting
					-- the whole proposal with critical=true and NO message.
					--
					-- SNAPPING to that node was tried first and is WRONG (2026-08-30): it
					-- skips a split the originator made, so the peer ends up with fewer
					-- road nodes, the worlds drift further, and the NEXT depot lands even
					-- closer to a node -- failures multiplied instead of stopping. Keep the
					-- topology identical (same nodes, same edges, same removal) and move
					-- the split point along the edge until both halves clear MIN_STUB.
					-- A couple of metres of driveway is invisible; a missing node is not.
					-- 12 m, not 6: two failures measured 2026-08-30 had their split clamped
					-- to EXACTLY 6.0 m from a node and the engine still answered
					-- 'Construction not possible' (harvested by the strict probe below),
					-- so the engine's own minimum is above that. If the peer's edge is too
					-- short to host a 12 m stub at both ends there is no valid split at all
					-- -- fall back to snapping the connector onto the nearer node, which
					-- costs one road node of topology but leaves a depot that is connected
					-- rather than absent.
					-- EXPERIMENT 2026-08-30: nudging is OFF (0 = keep the originator's exact
					-- split point). Every threshold tried so far -- 6 m, then 12 m -- still
					-- produced 'Construction not possible', and the last failure was a split
					-- the originator put 11.1 m from a node that the 12 m rule moved by
					-- 0.9 m: the rule was creating the mismatch it was meant to avoid (it
					-- also shifts the node's height off the depot's platform level). Replay
					-- the position as sent and measure the true failure rate before adding
					-- any correction back.
					local MIN_STUB = 0.0
					local function at(u) return hermitePos(a, ta, b, tb, u) end
					local total = 0
					do
						local prev = at(0)
						for k = 1, 40 do
							local q = at(k / 40)
							total = total + math.sqrt((q[1] - prev[1]) ^ 2 + (q[2] - prev[2]) ^ 2)
							prev = q
						end
					end
					if total <= 2 * MIN_STUB + 1 then
						local dA0 = math.sqrt((p[1] - a[1]) ^ 2 + (p[2] - a[2]) ^ 2)
						local dB0 = math.sqrt((p[1] - b[1]) ^ 2 + (p[2] - b[2]) ^ 2)
						snapNode[X] = (dA0 < dB0) and comp.node0 or comp.node1
						log(string.format("CONX: peer edge %d is only %.0f m -- no room for a %.0f m stub either side, snapping the connector to node %d",
							eid, total, MIN_STUB, snapNode[X]))
					end
					if not snapNode[X] and total > 2 * MIN_STUB + 1 then
						local uLo, uHi = MIN_STUB / total, 1 - MIN_STUB / total
						local bestU, bestD
						for k = 0, 200 do
							local u = k / 200
							local q = at(u)
							local d = (q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2
							if not bestD or d < bestD then bestD, bestU = d, u end
						end
						local uu = math.max(uLo, math.min(uHi, bestU or 0.5))
						-- Cap how far the split may move. The depot's driveway still starts
						-- at the mouth the originator chose, so moving the road end of it
						-- stretches and skews that driveway: nudges of 0.9-3 m replayed
						-- fine, 11 m ones were refused ('Construction not possible', merge
						-- pairing distance d=11.17 m, 2026-08-30). Beyond the cap there is
						-- no faithful placement on this peer -- attempt the original split
						-- and let it fail loudly rather than build something distorted.
						local MAX_NUDGE = 3.0
						do
							local q = at(uu)
							if math.sqrt((q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2) > MAX_NUDGE then
								log(string.format("CONX: peer edge %d would need a %.1f m nudge (cap %.0f m) -- this peer's road is nodded differently, replaying the split as sent",
									eid, math.sqrt((q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2), MAX_NUDGE))
								uu = bestU or uu
							end
						end
						if math.abs(uu - (bestU or uu)) > 0.0001 then
							local q = at(uu)
							log(string.format("CONX: split point was %.1f m from an end of peer edge %d -- nudged %.1f m along it to keep both halves >= %.0f m",
								math.min(math.sqrt((p[1]-a[1])^2 + (p[2]-a[2])^2), math.sqrt((p[1]-b[1])^2 + (p[2]-b[2])^2)),
								eid, math.sqrt((q[1]-p[1])^2 + (q[2]-p[2])^2), MIN_STUB))
							p[1], p[2], p[3] = q[1], q[2], q[3]
							nodes[X] = p
						end
					end
					if not snapNode[X] then
						nRm = nRm + 1
						sp.streetProposal.edgesToRemove[nRm] = eid
						splitEdgeOf[X] = eid
						-- Watch the node this cut creates. Nothing owns a replayed split,
						-- so if the construction it was cut for never lands (or is later
						-- removed) the node stays behind for good -- see healNodeAt.
						CM.watchSplit(p[1], p[2])
					end
				end
			else
				log(string.format("CONX: removed edge %d->%d: no peer edge under its split -- removal skipped", r[1], r[2]))
			end
		end
		for id, p in pairs(spos) do
			if not idmap[id] then
				local n = findNodeNear(isTrack, p[1], p[2], 1.5)
				if n then idmap[id] = n end
			end
		end

		-- Drop OUR copy of the template's own connector, because the template
		-- rebuilds it on the peer and shipping both duplicates it ('Collision').
		--
		-- That connector is a DEPOT-shaped payload: a couple of brand-new nodes
		-- and ONE both-new edge alongside a split of an existing road. A rail
		-- STATION is the opposite shape -- its whole platform network is
		-- both-new (25 nodes / 24 edges, no removals) and the template does NOT
		-- rebuild it, so dropping both-new edges there deleted every track and
		-- the station replayed bare (nodes=0 edges=0) and collided. So only
		-- apply the filter when the payload actually looks like a connector:
		-- there is a split to attach to (rms) and just a handful of edges.
		-- (This drops a rail station's ENTIRE platform network too -- measured:
		-- every station replays nodes=0 edges=0 -- and that is CORRECT: those
		-- tracks are template content the .con rebuilds on the peer, same as
		-- the depot's apron. Open-ground stations succeed exactly this way.)
		local used = {}
		local skippedApron = 0
		-- A snapped split node (see the STUB GUARD above) behaves like an EXISTING
		-- node from here on: the depot's own connector keeps it as its far end (that
		-- is the weld shape the merge adopts), while the two halves of the split that
		-- never happened are dropped along with their removal.
		local function isNew(id) return id < 0 and not snapNode[id] end
		local droppedHalves = 0
		for i = #adds, 1, -1 do
			local e = adds[i]
			if snapNode[e[1]] and e[2] >= 0 or snapNode[e[2]] and e[1] >= 0 then
				table.remove(adds, i); droppedHalves = droppedHalves + 1
			elseif isNew(e[1]) and isNew(e[2]) then
				table.remove(adds, i); skippedApron = skippedApron + 1
			else
				used[e[1]] = true; used[e[2]] = true
			end
		end
		if droppedHalves > 0 then
			log(string.format("CONX: dropped %d half/halves of a split that snapped to an existing node", droppedHalves))
		end
		for x, _ in pairs(snapNode) do nodes[x] = nil end
		for id, _ in pairs(nodes) do if not used[id] then nodes[id] = nil end end
		if skippedApron > 0 then
			log(string.format("CONX: %d apron edge(s) left to the template; shipping split node + halves", skippedApron))
		end
		local ni, minId = 0, 0
		for id, p in pairs(nodes) do
			ni = ni + 1
			local n = api.type.NodeAndEntity.new()
			n.entity = id
			n.comp.position = api.type.Vec3f.new(p[1], p[2], p[3])
			sp.streetProposal.nodesToAdd[ni] = n
			if id < minId then minId = id end
		end
		-- Placeholder ids share ONE namespace with the template's own, and the
		-- engine allocates the template's BELOW the lowest id already in use.
		-- The UI numbers -1..-5; a -100001 base pushed the template's nodes to
		-- -100004/-100005 and the apply asserted on
		-- 'it != result.result.boundingVolumes.end()' (create_proposal_data.cpp
		-- :897). Continue the UI's numbering: segments right after the nodes.
		local ei, dropped = 0, 0
		for _, e in ipairs(adds) do
			local n0 = snapNode[e[1]] or ((e[1] < 0) and e[1] or idmap[e[1]])
			local n1 = snapNode[e[2]] or ((e[2] < 0) and e[2] or idmap[e[2]])
			if n0 and n1 and n0 ~= n1 then
				ei = ei + 1
				local s = api.type.SegmentAndEntity.new()
				s.entity = minId - ei
				s.comp.node0 = n0
				s.comp.node1 = n1
				s.comp.tangent0 = api.type.Vec3f.new(e[3], e[4], e[5])
				s.comp.tangent1 = api.type.Vec3f.new(e[6], e[7], e[8])
				s.comp.type = 0
				s.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
				if e[9] == 1 or e[9] == 2 then   -- bridge / tunnel connector, from the capture's tail
					s.comp.type = e[9]
					s.comp.typeIndex = e[10] or -1
				end
				s.type = isTrack and 1 or 0
				if isTrack then
					s.trackEdge = api.type.BaseEdgeTrack.new()
					s.trackEdge.trackType = tonumber(c.ttype) or 1
					s.trackEdge.catenary = (tonumber(c.cat) or 0) == 1
					s.streetEdge = api.type.BaseEdgeStreet.new()
					s.streetEdge.streetType = 16
				else
					s.streetEdge = api.type.BaseEdgeStreet.new()
					s.streetEdge.streetType = stype
					s.streetEdge.hasBus = false
					s.streetEdge.tramTrackType = 0
				end
				-- A half of a split keeps the SPLIT edge's type, not the
				-- construction's: the UI's halves carry the town road's type
				-- 16 while the apron is 29.
				local se = splitEdgeOf[e[1]] or splitEdgeOf[e[2]]
				if se then
					copyEdgeProps(s, se, isTrack, stype)
					-- ...and its TANGENTS come from THIS peer's copy of the split edge,
					-- never from the originator's. The shipped tangents describe the
					-- originator's edge, and the two worlds' town roads drift apart
					-- (different node spacing). Measured 2026-08-30: A split a 55 m
					-- edge, the peer's matching edge was 28 m, so the second half was
					-- 7 m long carrying 12 m tangents -- a Hermite that doubles back on
					-- itself. The engine rejected the whole proposal with critical=true
					-- and an EMPTY message list, so a depot placed near another one
					-- silently never appeared on the peer. Re-deriving from the local
					-- curve is identical when the worlds agree, and right when they do not.
					pcall(function()
						local xid = splitEdgeOf[e[1]] and e[1] or e[2]
						local xp = nodes[xid]
						local comp, aP, bP, taP, tbP = edgeGeomT(se)
						if not (comp and xp) then return end
						local bestU, bestD
						for k = 0, 200 do
							local u = k / 200
							local q = hermitePos(aP, taP, bP, tbP, u)
							local d = (q[1] - xp[1]) ^ 2 + (q[2] - xp[2]) ^ 2
							if not bestD or d < bestD then bestD, bestU = d, u end
						end
						if not bestU or bestU <= 0.001 or bestU >= 0.999 then return end
						-- Parameter of each endpoint along the peer's curve. The
						-- difference carries the sign, so a half stored end-to-start
						-- gets correctly negated tangents.
						local function uOf(nid)
							if nid == xid then return bestU end
							if nid == comp.node0 then return 0 end
							if nid == comp.node1 then return 1 end
							return nil
						end
						local u0, u1 = uOf(n0), uOf(n1)
						if not (u0 and u1) then return end
						local sc = u1 - u0
						if math.abs(sc) < 0.001 then return end
						local t0 = hermiteTangent(aP, taP, bP, tbP, u0)
						local t1 = hermiteTangent(aP, taP, bP, tbP, u1)
						s.comp.tangent0 = api.type.Vec3f.new(t0[1] * sc, t0[2] * sc, t0[3] * sc)
						s.comp.tangent1 = api.type.Vec3f.new(t1[1] * sc, t1[2] * sc, t1[3] * sc)
					end)
				end
				sp.streetProposal.edgesToAdd[ei] = s
			else
				dropped = dropped + 1
				log(string.format("CONX: edge %d->%d dropped (%s)", e[1], e[2],
					(n0 and n1) and "both ends resolve to the same node" or "an endpoint is unmapped"))
			end
		end

		expectedCons[key] = true
		if c.company then CM.cmExpectedCompany[key] = tonumber(c.company); CM.cmEnsure(); CM.cmExpectedBal0[key] = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
			CM.cmLog(string.format("CM: CONX bal0 snapshot key=%s mePid=%s bal0=%s", key, tostring(CM.cmCompanyPid[CM.cmMyCompany]), tostring(CM.cmExpectedBal0[key]))) end
		-- Context: nil, like the one native placement that has APPLIED (probe
		-- E2c). buildContext() forces checkTerrainAlignment=false; the UI's
		-- context carries it true, and its whole layout differs from ours.
		-- Retry after an obstacle clear passes ignoreErrors=true: the
		-- originator's UI already validated this exact placement, and what can
		-- remain (vegetation) is not enumerable to bulldoze first.
		-- ignoreErrors=true on the FIRST attempt too (2026-08-29). The peer used to
		-- pre-clear a guessed 60x30 m pad and over-demolished (B lost 20 more town
		-- buildings than A: 101 vs 121 within 200 m). The native tool demolishes
		-- exactly what its footprint collides with; ignoreErrors lets the engine do
		-- the same here, and the real-BOUNDING_VOLUME sweep after success catches
		-- anything modular layouts leave under the floor.
		-- The engine's octree pre-check (street_builder_util.cpp CheckGraph ->
		-- FUN_1421e2330, see docs/re/STREET_PROPOSAL_VALIDATION.md) queries a
		-- +-0.01 m box around EVERY added node and fails SILENTLY -- no message, just
		-- critical=true -- when something is already there. Log how close each added
		-- node sits to an existing one so a rejection can be attributed instead of
		-- guessed at. 1 cm is the engine's own tolerance; anything under a metre is
		-- worth seeing.
		pcall(function()
			for i = 1, ni do
				local nn = sp.streetProposal.nodesToAdd[i]
				local px, py = nn.comp.position.x, nn.comp.position.y
				local near = findNodeNear(isTrack, px, py, 1.0)
				if near then
					local nc = api.engine.getComponent(near, api.type.ComponentType.BASE_NODE)
					local d = nc and math.sqrt((nc.position.x - px) ^ 2 + (nc.position.y - py) ^ 2) or -1
					log(string.format("%s seq=%s: added node %s at (%.2f,%.2f) is %.3f m from EXISTING node %d%s",
						tostring(c.op), tostring(c.seq), tostring(nn.entity), px, py, d, near,
						(d >= 0 and d < 0.01) and "  <-- inside the engine's 0.01 m octree box, this alone refuses the build" or ""))
				end
			end
		end)
		local okMake, cmd = pcall(function() return api.cmd.make.buildProposal(sp, nil, true) end)
		if not okMake or not cmd then
			-- Fallback: the old template path. Loses street integration but
			-- still puts the building down; logged loudly so it is visible.
			log(string.format("CONX: make.buildProposal refused (%s) -- falling back to buildConstruction",
				tostring(cmd)))
			params.seed = nil
			local built, newId = pcall(game.interface.buildConstruction, tostring(c.file), params, t)
			if built and newId then
				pcall(function() game.interface.setPlayer(newId, api.engine.util.getPlayer()) end)
			else
				expectedCons[key] = nil
				-- Even the template fallback could not place it: this peer will never
				-- have the building, so the originator must not keep its copy.
				scheduleLocal("CONFAIL", { target = tostring(c.origin), x = t[13], y = t[14],
				                           file = tostring(c.file), failedSeq = tostring(c.seq) })
				log(string.format("CONX seq=%s: fallback failed too -- asked %s to roll its copy back",
					tostring(c.seq), tostring(c.origin)))
			end
			log(string.format("EXEC %s seq=%s origin=%s at=%s file=%s ok=%s id=%s (fallback)",
				tostring(c.op), tostring(c.seq), tostring(c.origin), tostring(c.at),
				tostring(c.file), tostring(built and newId ~= nil), tostring(newId)))
			conxBusy = false
			return
		end
		local seq, origin, at, file, op = c.seq, c.origin, c.at, c.file, c.op
		api.cmd.sendCommand(cmd, function(res, success)
			-- The engine has answered: the next queued construction may go.
			conxBusy = false
			local ent = "?"
			pcall(function() ent = tostring(res.resultEntities[1]) end)
			log(string.format("EXEC %s seq=%s origin=%s at=%s file=%s nodes=%d edges=%d rm=%d dropped=%d success=%s ent=%s",
				tostring(op), tostring(seq), tostring(origin), tostring(at), tostring(file),
				ni, ei, nRm, dropped, tostring(success), ent))
			if success then
				-- (companies-mode reassign happens in pollNewConstructions, which every
				-- replay path reaches -- NOT here, where the buildConstruction fallback
				-- path never arrives.)
				-- After a RETRIED (ignoreErrors) build: the pre-clear pads are an
				-- approximation -- a MODULAR station's module layout can put
				-- floor outside them (user report). The built entity's
				-- BOUNDING_VOLUME (component 56, probe P33) is the EXACT
				-- footprint for any layout: sweep town constructions and asset
				-- groups still under it.
				-- Sweep after EVERY successful build, not only a retried one (2026-08-29):
				-- with ignoreErrors on the first attempt the engine left 6 town
				-- buildings standing INSIDE the station's bounding box on B (A had
				-- demolished them), and the retry-gated sweep never ran.
				-- The BOUNDING_VOLUME sweep is DISABLED (2026-08-29): a modular station's
				-- bbox spans its whole module grid (~170x120 m), far beyond the built
				-- footprint. Sweeping it removed 13 town buildings on B that A's engine
				-- collision-demolish had KEPT (all 13 sat inside the bbox). The engine's
				-- own ignoreErrors demolish is the correct set, identical on A and B.
				-- TRACK-CORRIDOR CLEAR (2026-08-29). ignoreErrors on a raw proposal does
				-- NOT demolish colliding buildings the way the native tool does -- it
				-- just suppresses the error and builds THROUGH them: after an
				-- ignoreErrors build B had 7 town buildings standing on live platform
				-- track (each within 12 m of a station track node) that A's native
				-- placement had removed; the stations were otherwise identical (14
				-- modules, 28 track nodes). The whole-bbox sweep over-cleared (13
				-- buildings A kept, 30-40 m from any track). So: bulldoze exactly the
				-- town constructions/assets within CORRIDOR m of a track EDGE of the
				-- built station -- the platforms' true footprint.
				pcall(function()
					local bid = res.resultEntities[1]
					local co = api.engine.getComponent(bid, api.type.ComponentType.CONSTRUCTION)
					local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
					local bb = bv and bv.bbox
					if not (co and bb) then return end
					local CORRIDOR = 10
					-- the station's own track edges: track nodes inside its bbox
					local segs = {}
					local tmap = api.engine.system.streetSystem.getNode2TrackEdgeMap()
					local seen = {}
					for nid, edges in pairs(tmap) do
						local q = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE).position
						if q.x >= bb.min.x - 5 and q.x <= bb.max.x + 5 and q.y >= bb.min.y - 5 and q.y <= bb.max.y + 5 then
							for _, e in pairs(edges) do
								if not seen[e] then
									seen[e] = true
									local c = api.engine.getComponent(e, api.type.ComponentType.BASE_EDGE)
									local a = api.engine.getComponent(c.node0, api.type.ComponentType.BASE_NODE).position
									local b2 = api.engine.getComponent(c.node1, api.type.ComponentType.BASE_NODE).position
									segs[#segs + 1] = { a.x, a.y, b2.x, b2.y }
								end
							end
						end
					end
					local function distToSegs(px, py)
						local best = 1e9
						for _, sg in ipairs(segs) do
							local ax, ay, bx, by = sg[1], sg[2], sg[3], sg[4]
							local dx, dy = bx - ax, by - ay
							local L = dx * dx + dy * dy
							local t = L > 0 and math.max(0, math.min(1, ((px - ax) * dx + (py - ay) * dy) / L)) or 0
							local d = math.sqrt((px - (ax + t * dx)) ^ 2 + (py - (ay + t * dy)) ^ 2)
							if d < best then best = d end
						end
						return best
					end
					local cx, cy = (bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2
					local r = math.sqrt((bb.max.x - bb.min.x) ^ 2 + (bb.max.y - bb.min.y) ^ 2) / 2 + 10
					local cleared = 0
					-- EXACT path: the originator shipped the town buildings its world still
					-- has. Anything inside this station's bbox that is NOT on that list is a
					-- building the originator lost to the placement -> remove it here too.
					if c.survivors then
						local survPts = {}
						for sx, sy in tostring(c.survivors):gmatch("([%-%d%.]+):([%-%d%.]+)") do survPts[#survPts + 1] = { tonumber(sx), tonumber(sy) } end
						local function isSurvivor(px, py)
							for _, sp in ipairs(survPts) do if (px - sp[1]) ^ 2 + (py - sp[2]) ^ 2 <= 9 then return true end end
							return false
						end
							-- diff region: the WHOLE gather disk, no bbox term. The bbox gate was
							-- the wrong knob once the diff became survivor-keyed: a depot's native
							-- placement demolished two buildings 45-58 m from its centre (outside
							-- bbox+10) and the peer kept them (2026-08-29). Since the peer only
							-- removes what the originator's snapshot LACKS, the region is bounded by
							-- the snapshot's coverage, not by the footprint. Anchor on the built
							-- construction's own position (what gatherSurvivors used on the
							-- originator), 190 m inside the 200 m gather so nothing unlisted is judged.
							local gx, gy = cx, cy
							pcall(function()
								local bc = api.engine.getComponent(bid, api.type.ComponentType.CONSTRUCTION)
								if bc and bc.transf then gx, gy = bc.transf[13], bc.transf[14] end
							end)
							local DIFF_R = 190
							local inb = function(px, py)
								return (px - gx) ^ 2 + (py - gy) ^ 2 <= DIFF_R * DIFF_R
							end
							-- Sanity cap: a placement clears a handful of buildings. A diff wanting
							-- far more means the snapshot is stale/foreign -> log and refuse rather
							-- than level a town.
							local MAX_DIFF_REMOVALS = 40
							local victims = {}
							local kept = 0
							for _, id in pairs(game.interface.getEntities({ pos = { gx, gy }, radius = DIFF_R }, { type = "CONSTRUCTION", includeData = false }) or {}) do
								if id ~= bid then
									local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
									local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
									if cco and po == nil and cco.transf and inb(cco.transf[13], cco.transf[14]) then
										if isSurvivor(cco.transf[13], cco.transf[14]) then kept = kept + 1
										else victims[#victims + 1] = { id, cco.transf[13], cco.transf[14], "CONSTRUCTION" } end
									end
								end
							end
							for _, id in pairs(game.interface.getEntities({ pos = { gx, gy }, radius = DIFF_R }, { type = "ASSET_GROUP", includeData = false }) or {}) do
								local okE, e = pcall(game.interface.getEntity, id)
								local px = okE and e and e.position and (e.position[1] or e.position.x)
								local py = okE and e and e.position and (e.position[2] or e.position.y)
								if px and py and inb(px, py) and not isSurvivor(px, py) then
									victims[#victims + 1] = { id, px, py, "ASSET_GROUP" }
								end
							end
							if #victims > MAX_DIFF_REMOVALS then
								CM.cmLog(string.format("STN: %s seq=%s survivor-diff wants %d removals (> %d) -> REFUSED, snapshot looks stale", tostring(op), tostring(seq), #victims, MAX_DIFF_REMOVALS))
							else
								for _, v in ipairs(victims) do
									if pcall(game.interface.bulldoze, v[1]) then cleared = cleared + 1
										CM.cmLog(string.format("STN: survivor-diff bulldozed town %s %d at (%.1f,%.1f)", v[4], v[1], v[2], v[3])) end
								end
							end
							CM.cmLog(string.format("STN: %s seq=%s survivor-diff: %d survivor(s) shipped, %d kept in %d m disk, %d removed", tostring(op), tostring(seq), #survPts, kept, DIFF_R, cleared))
							return
					end
					CM.cmLog(string.format("STN: %s seq=%s NO survivors shipped -> falling back to the %d m track corridor", tostring(op), tostring(seq), CORRIDOR))
					for _, id in pairs(game.interface.getEntities({ pos = { cx, cy }, radius = r }, { type = "CONSTRUCTION", includeData = false }) or {}) do
						if id ~= bid then
							local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
							local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
							if cco and po == nil and cco.transf and distToSegs(cco.transf[13], cco.transf[14]) <= CORRIDOR then
								if pcall(game.interface.bulldoze, id) then cleared = cleared + 1
									CM.cmLog(string.format("STN: corridor-clear town CONSTRUCTION %d at (%.1f,%.1f)", id, cco.transf[13], cco.transf[14])) end
							end
						end
					end
					for _, id in pairs(game.interface.getEntities({ pos = { cx, cy }, radius = r }, { type = "ASSET_GROUP", includeData = false }) or {}) do
						local okE, e = pcall(game.interface.getEntity, id)
						local px = okE and e and e.position and (e.position[1] or e.position.x)
						local py = okE and e and e.position and (e.position[2] or e.position.y)
						if px and py and distToSegs(px, py) <= CORRIDOR then
							if pcall(game.interface.bulldoze, id) then cleared = cleared + 1 end
						end
					end
					CM.cmLog(string.format("STN: %s seq=%s track-corridor clear: %d track segment(s), %d cleared within %d m", tostring(op), tostring(seq), #segs, cleared, CORRIDOR))
				end)
				if false then
					pcall(function()
						local bid = res.resultEntities[1]
						local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
						local bb = bv and bv.bbox
						if not bb then return end
						local bx0, by0 = bb.min.x - 3, bb.min.y - 3
						local bx1, by1 = bb.max.x + 3, bb.max.y + 3
						local scx, scy = (bx0 + bx1) / 2, (by0 + by1) / 2
						local scr = math.sqrt((bx1 - bx0) ^ 2 + (by1 - by0) ^ 2) / 2 + 5
						local swept = 0
						local near = game.interface.getEntities({ pos = { scx, scy }, radius = scr },
							{ type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							if id ~= bid then
								local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
								local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
								if cco and po == nil and cco.transf
									and cco.transf[13] >= bx0 and cco.transf[13] <= bx1
									and cco.transf[14] >= by0 and cco.transf[14] <= by1 then
									if pcall(game.interface.bulldoze, id) then swept = swept + 1 end
								end
							end
						end
						local assets = game.interface.getEntities({ pos = { scx, scy }, radius = scr },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _, id in pairs(assets) do
							local okE, e = pcall(game.interface.getEntity, id)
							local px = okE and e and e.position and (e.position[1] or e.position.x)
							local py = okE and e and e.position and (e.position[2] or e.position.y)
							if px and py and px >= bx0 and px <= bx1 and py >= by0 and py <= by1 then
								if pcall(game.interface.bulldoze, id) then swept = swept + 1 end
							end
						end
						if swept > 0 then
							log(string.format("%s seq=%s: post-build sweep removed %d leftover(s) under the real bounding box",
								tostring(op), tostring(seq), swept))
							CM.cmLog(string.format("STN: %s seq=%s post-build sweep removed %d leftover(s)", tostring(op), tostring(seq), swept))
						end
					end)
				end
				-- STN trace: how many town buildings remain near the station after the
				-- engine's own collision-demolish (ignoreErrors) -- compare A vs B.
				pcall(function()
					local bid = res.resultEntities[1]
					local bv = api.engine.getComponent(bid, api.type.ComponentType.BOUNDING_VOLUME)
					local bb = bv and bv.bbox
					if bb then
						local cx, cy = (bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2
						local left = 0
						local near = game.interface.getEntities({ pos = { cx, cy }, radius = 200 }, { type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							if api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED) == nil then left = left + 1 end
						end
						CM.cmLog(string.format("STN: %s seq=%s built (retried=%s); town constructions within 200 m of the station now: %d",
							tostring(op), tostring(seq), tostring(c.retried), left))
					end
				end)
				-- verification only: the proposal's name should have given the
				-- child its NAME and PLAYER_OWNED (probe P9)
				pcall(function()
					local eid = res.resultEntities[1]
					local co = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION)
					local nd = (co and co.depots) and #co.depots or 0
					local ns = (co and co.stations) and #co.stations or 0
					local child = (nd > 0 and co.depots[1]) or (ns > 0 and co.stations[1]) or nil
					local cn = child and api.engine.getComponent(child, api.type.ComponentType.NAME)
					local cp = child and api.engine.getComponent(child, api.type.ComponentType.PLAYER_OWNED)
					log(string.format("%s seq=%s: entity %s child %s childNAME=%s childOWNED=%s",
						tostring(op), tostring(seq), tostring(eid), tostring(child),
						tostring(cn ~= nil), tostring(cp ~= nil)))
				end)
			end
			if not success then
				expectedCons[key] = nil
				-- Dump what we actually submitted. A depot replay that fails with
				-- critical=true and an EMPTY message list (2026-08-30) leaves no
				-- other evidence, and every theory about WHY has to be checked
				-- against the proposal rather than guessed from the outcome.
				pcall(function()
					log(string.format("%s seq=%s PROPOSAL: %d node(s) %d edge(s) %d removal(s), dropped=%d",
						tostring(op), tostring(seq), ni, ei, nRm, dropped))
					for i = 1, ni do
						local nn = sp.streetProposal.nodesToAdd[i]
						log(string.format("  node[%d] id=%s pos=(%.2f,%.2f,%.2f)", i, tostring(nn.entity),
							nn.comp.position.x, nn.comp.position.y, nn.comp.position.z))
					end
					for i = 1, ei do
						local se2 = sp.streetProposal.edgesToAdd[i]
						log(string.format("  edge[%d] id=%s %s->%s type=%s streetType=%s", i, tostring(se2.entity),
							tostring(se2.comp.node0), tostring(se2.comp.node1), tostring(se2.type),
							tostring(se2.streetEdge and se2.streetEdge.streetType)))
					end
					for i = 1, nRm do
						local reid = sp.streetProposal.edgesToRemove[i]
						local rc = reid and api.engine.getComponent(reid, api.type.ComponentType.BASE_EDGE)
						local p0 = rc and api.engine.getComponent(rc.node0, api.type.ComponentType.BASE_NODE)
						local p1 = rc and api.engine.getComponent(rc.node1, api.type.ComponentType.BASE_NODE)
						-- Is this edge FROZEN into a construction (its apron, or a piece a
						-- previous depot's weld adopted)? A plain street proposal cannot
						-- remove a construction-owned edge, and the engine rejects it with
						-- critical=true and NO message -- the exact shape seen when two
						-- depots are placed close together (2026-08-30).
						local owner, ownerFile = nil, nil
						pcall(function()
							local ax = p0 and p0.position.x or (rc and 0) or 0
							local ay = p0 and p0.position.y or 0
							for _, cid in pairs(game.interface.getEntities({ pos = { ax, ay }, radius = 80 },
									{ type = "CONSTRUCTION", includeData = false }) or {}) do
								local cc = api.engine.getComponent(cid, api.type.ComponentType.CONSTRUCTION)
								if cc and cc.frozenEdges then
									for _, fe in pairs(cc.frozenEdges) do
										if fe == reid then owner = cid; ownerFile = tostring(cc.fileName) end
									end
								end
							end
						end)
						if owner then
							log(string.format("  remove[%d] edge=%s is FROZEN into construction %s (%s) -- a street proposal cannot remove it",
								i, tostring(reid), tostring(owner), tostring(ownerFile)))
						end
						log(string.format("  remove[%d] edge=%s n0=%s%s n1=%s%s", i, tostring(reid),
							tostring(rc and rc.node0),
							p0 and string.format("(%.1f,%.1f)", p0.position.x, p0.position.y) or "",
							tostring(rc and rc.node1),
							p1 and string.format("(%.1f,%.1f)", p1.position.x, p1.position.y) or ""))
					end
				end)
				-- LAST RESORT + BISECTION. When even the retry failed, rebuild the SAME
				-- construction with NO street payload at all. Two things come out of it:
				-- the player gets their depot (unconnected, but present, instead of the
				-- building silently missing on this peer), and the result localises the
				-- fault -- if the construction alone builds, the rejection is in the
				-- street vectors we ship, not in the construction or its template.
				-- (docs/re/STREET_PROPOSAL_VALIDATION.md narrows it to "something we add
				-- already exists and we did not remove it"; this says which half.)
				if c.retried and not c.bare then
					pcall(function()
						local sp2 = api.type.SimpleProposal.new()
						local ce2 = api.type.SimpleProposal.ConstructionEntity.new()
						ce2.fileName = tostring(c.file)
						ce2.params = params
						ce2.transf = api.type.Mat4f.new(
							api.type.Vec4f.new(t[1], t[2], t[3], t[4]),
							api.type.Vec4f.new(t[5], t[6], t[7], t[8]),
							api.type.Vec4f.new(t[9], t[10], t[11], t[12]),
							api.type.Vec4f.new(t[13], t[14], t[15], t[16]))
						ce2.playerEntity = api.engine.util.getPlayer()
						ce2.name = unescName(c.name)
						sp2.constructionsToAdd[1] = ce2
						local cmd2 = api.cmd.make.buildProposal(sp2, nil, true)
						if not cmd2 then return end
						expectedCons[key] = true
						api.cmd.sendCommand(cmd2, function(res3, ok3)
							local e3 = "?"
							pcall(function() e3 = tostring(res3.resultEntities[1]) end)
							log(string.format("%s seq=%s BARE probe (construction only, no street payload): success=%s ent=%s -- %s",
								tostring(op), tostring(seq), tostring(ok3), e3,
								ok3 and "the STREET payload is what the engine refuses" or "the CONSTRUCTION itself is refused here"))
							if not ok3 then
								expectedCons[key] = nil
								-- Nothing of this placement exists here. Tell the originator to
								-- undo its own copy so the worlds stay identical; it is the only
								-- side that can, and leaving it standing diverges us forever.
								scheduleLocal("CONFAIL", { target = tostring(origin), x = t[13], y = t[14],
								                           file = tostring(c.file), failedSeq = tostring(seq) })
								log(string.format("%s seq=%s: asked %s to roll its copy back",
									tostring(op), tostring(seq), tostring(origin)))
							else
								-- The bare construction stands but WITHOUT its road connection,
								-- while the originator has the connector edges: still a
								-- divergence, just a visible one. Roll both sides back.
								pcall(function() game.interface.bulldoze(tonumber(e3)) end)
								expectedCons[key] = nil
								scheduleLocal("CONFAIL", { target = tostring(origin), x = t[13], y = t[14],
								                           file = tostring(c.file), failedSeq = tostring(seq) })
								log(string.format("%s seq=%s: bare copy removed again and %s asked to roll back -- an unconnected depot on one side only is still a divergence",
									tostring(op), tostring(seq), tostring(origin)))
							end
						end)
					end)
				end
				-- WHY did it fail? The first attempt runs with ignoreErrors=TRUE, and in
				-- that mode the engine returns critical=true with an EMPTY message list,
				-- which tells us nothing. Re-submit the identical proposal with
				-- ignoreErrors=FALSE purely to harvest the message -- stricter than the
				-- attempt that already failed, so it cannot build anything by accident.
				pcall(function()
					local strict = api.cmd.make.buildProposal(sp, nil, false)
					if not strict then return end
					api.cmd.sendCommand(strict, function(res2, ok2)
						local msgs = ""
						pcall(function()
							local es2 = res2.resultProposalData and res2.resultProposalData.errorState
							if es2 then
								for i = 1, #es2.messages do msgs = msgs .. " '" .. tostring(es2.messages[i]) .. "'" end
								for i = 1, #(es2.warnings or {}) do msgs = msgs .. " warn:'" .. tostring(es2.warnings[i]) .. "'" end
								msgs = msgs .. " critical=" .. tostring(es2.critical)
							end
						end)
						log(string.format("%s seq=%s STRICT probe: success=%s%s", tostring(op), tostring(seq), tostring(ok2), msgs))
					end)
				end)
				local collided = false
				pcall(function()
					local es = res.resultProposalData and res.resultProposalData.errorState
					if es then
						local msgs = ""
						pcall(function()
							for i = 1, #es.messages do
								local m = tostring(es.messages[i])
								msgs = msgs .. " '" .. m .. "'"
								if m:find("Collision", 1, true) then collided = true end
							end
						end)
						log(string.format("%s FAIL detail: critical=%s%s", tostring(op), tostring(es.critical), msgs))
					end
				end)
				-- The originator's game AUTO-DEMOLISHED the town buildings under
				-- the footprint (they are in the UI proposal's toRemove, which the
				-- replay cannot carry). Do the same here: bulldoze the UNOWNED
				-- (town) constructions overlapping the footprint, then retry once.
				-- Only unowned ones, and only on a Collision, so this can never
				-- eat a player's building.
				-- Retry on ANY failure, not just a "Collision" message. The first
				-- attempt already runs with ignoreErrors=true, so a failure here is
				-- a hard reject -- and a road depot dropped where the peer still had
				-- two ASSET_GROUPs under the footprint failed with critical=true and
				-- an EMPTY message list (2026-08-30), so the Collision-only gate did
				-- nothing at all and the depot never appeared on the peer. The clear
				-- itself is unchanged and stays conservative: unowned, non-survivor,
				-- inside the station-local footprint box, once.
				if not c.retried then
					if not collided then
						log(string.format("%s seq=%s: failure carried no 'Collision' message -- clearing the footprint anyway",
							tostring(op), tostring(seq)))
					end
					-- Clear by FOOTPRINT, not a fixed disc: a modular station
					-- extends far beyond 40 m of its centre, and with many
					-- buildings under it the outer ones survived the old clear,
					-- so the ignoreErrors retry built ON TOP of them (user
					-- report 2026-08-28). The shipped platform-track nodes span
					-- the whole footprint; use their bounding box, padded 25 m.
					-- STATION-LOCAL frame, not an axis-aligned box: the head
					-- building extends ~50 m past the last track node ALONG the
					-- station, while sideways 25 m already over-reaches. Measured
					-- (P32): survivors sat 17-27 m beyond the 25 m pad at the
					-- south end; an untouched town building sat 43 m to the west.
					local ux, uy = t[1], t[2]      -- local X axis (across)
					local vx, vy = t[5], t[6]      -- local Y axis
					local ul = math.sqrt(ux * ux + uy * uy); if ul > 0 then ux, uy = ux / ul, uy / ul end
					local vl = math.sqrt(vx * vx + vy * vy); if vl > 0 then vx, vy = vx / vl, vy / vl end
					local cx0, cy0 = t[13], t[14]
					local maxU, maxV = 10, 10
					for _, pp in ipairs(rawPos) do
						local dx, dy = pp[1] - cx0, pp[2] - cy0
						local pu = math.abs(dx * ux + dy * uy)
						local pv = math.abs(dx * vx + dy * vy)
						if pu > maxU then maxU = pu end
						if pv > maxV then maxV = pv end
					end
					local PAD_ALONG, PAD_ACROSS = 60, 30
					local limU = maxU + ((maxU >= maxV) and PAD_ALONG or PAD_ACROSS)
					local limV = maxV + ((maxV > maxU) and PAD_ALONG or PAD_ACROSS)
					local qx, qy = cx0, cy0
					local qr = math.sqrt(limU * limU + limV * limV) + 5
					-- Survivors the originator's game left standing: NEVER clear these.
					-- Matched by position (ids differ per peer), 3 m tolerance.
					local survPts = {}
					if c.survivors then
						for sx, sy in tostring(c.survivors):gmatch("([%-%d%.]+):([%-%d%.]+)") do
							survPts[#survPts + 1] = { tonumber(sx), tonumber(sy) }
						end
					end
					local function isSurvivor(px, py)
						for _, sp in ipairs(survPts) do
							local dx, dy = px - sp[1], py - sp[2]
							if dx * dx + dy * dy <= 9 then return true end
						end
						return false
					end
					local function inBox(px, py)
						if not px or not py then return false end
						if isSurvivor(px, py) then return false end
						local dx, dy = px - qx, py - qy
						return math.abs(dx * ux + dy * uy) <= limU
						   and math.abs(dx * vx + dy * vy) <= limV
					end
					local cleared = 0
					pcall(function()
						-- Town CONSTRUCTIONS (buildings) inside the footprint box.
						local near = game.interface.getEntities({ pos = { qx, qy }, radius = qr },
							{ type = "CONSTRUCTION", includeData = false }) or {}
						for _, id in pairs(near) do
							local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
							local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
							if cco and po == nil and cco.transf and inBox(cco.transf[13], cco.transf[14]) then
								if pcall(game.interface.bulldoze, id) then cleared = cleared + 1
									CM.cmLog(string.format("STN: pre-clear bulldozed town CONSTRUCTION %d at (%.1f,%.1f)", id, cco.transf[13], cco.transf[14])) end
							end
						end
						-- ASSET_GROUPs (trees, rocks, props): also cleared by the
						-- game on placement; not constructions, not edges.
						local assets = game.interface.getEntities({ pos = { qx, qy }, radius = qr },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _, id in pairs(assets) do
							local okE, e = pcall(game.interface.getEntity, id)
							local px = okE and e and e.position and (e.position[1] or e.position.x)
							local py = okE and e and e.position and (e.position[2] or e.position.y)
							if inBox(px, py) then
								if pcall(game.interface.bulldoze, id) then cleared = cleared + 1 end
							end
						end
					end)
					-- Retry once EVEN IF nothing was cleared. Builds and bulldozes are
					-- async: two depots placed in the same tick split the same road, and
					-- the second proposal is validated against a world where the first
					-- split has not landed yet -- it removes an edge that is already
					-- gone and fails with critical=true and no message (measured
					-- 2026-08-30: seq=3 and seq=4 both stamped t=41, the first built,
					-- the second did not). Re-running rebuilds the proposal from the
					-- CURRENT world, so the retry resolves the edge that actually
					-- exists by then.
					if cleared > 0 then
						log(string.format("%s seq=%s: cleared %d town obstacle(s) under the footprint -- retry in 1.5",
							tostring(op), tostring(seq), cleared))
						CM.cmLog(string.format("STN: %s seq=%s pre-clear: %d obstacle(s) bulldozed, %d survivor(s) protected (box %.0fx%.0f m)",
							tostring(op), tostring(seq), cleared, #survPts, 2 * limU, 2 * limV))
					else
						log(string.format("%s seq=%s: nothing to clear -- retrying anyway in case the world was mid-change",
							tostring(op), tostring(seq)))
					end
					local again = {}
					for k, v in pairs(c) do again[k] = v end
					again.retried = 1
					table.insert(conxQueue, 1, { c = again, notBefore = (gameTime() or 0) + 1.5 })
				end
				-- Diagnostic: what player/town constructions sit near the footprint?
				-- A station placed over town buildings auto-demolishes them on the
				-- originator; if the replay collides, these are the obstacles.
				pcall(function()
					local near = game.interface.getEntities({ pos = { t[13], t[14] }, radius = 40 },
						{ type = "CONSTRUCTION", includeData = false }) or {}
					local n = 0
					for _, id in pairs(near) do
						local cco = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
						if cco and cco.transf then
							n = n + 1
							if n <= 8 then
								local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
								log(string.format("%s obstacle: %s @%.1f,%.1f owned=%s", tostring(op),
									tostring(cco.fileName), cco.transf[13], cco.transf[14], tostring(po ~= nil)))
							end
						end
					end
					local na = 0
					pcall(function()
						local ag = game.interface.getEntities({ pos = { t[13], t[14] }, radius = 40 },
							{ type = "ASSET_GROUP", includeData = false }) or {}
						for _ in pairs(ag) do na = na + 1 end
					end)
					log(string.format("%s FAIL: %d construction(s), %d asset group(s) within 40m of the footprint",
						tostring(op), n, na))
				end)
			end
		end)
	end)
	-- Any early return inside the body (bad transf, no command built) leaves the
	-- gate held; the watchdog above would clear it after 3 units, but releasing
	-- it here keeps the queue moving. A live sendCommand has already cleared it.
	if not ok then log("execConX error: " .. tostring(err)); conxBusy = false end
end

-- The peer could not build a construction this instance placed: undo it here so
-- the two worlds stay identical. Sent by the peer as CONFAIL with target = the
-- ORIGINATING instance's letter; only that instance acts on it. Without this a
-- refused replay left the building standing on one side forever -- a permanent,
-- silent divergence that every later command near it inherited.
local function execConFail(c)
	if tostring(c.target) ~= K.INSTANCE then return end
	local ok, err = pcall(function()
		local key = conKey(c.x, c.y)
		-- EXACT match only. The first version took the nearest player construction
		-- within 5 m, and with depots placed a few metres apart it rolled back a
		-- NEIGHBOUR that the peer had built fine -- so the peer kept that depot and
		-- its road split while this side lost both (edge counts 1116 vs 1114 with
		-- construction counts equal, 2026-08-30). The originator placed this exact
		-- construction from this exact transform, so its own copy is at the position
		-- the peer echoed back, to the centimetre. Prefer our own registry entry;
		-- otherwise accept only a same-file construction within 1 m. No candidate
		-- means it is already gone -- never guess at a neighbour.
		local best, bestD
		local rec = consByKey[key]
		local recAlive = false
		if rec and rec.id then pcall(function() recAlive = api.engine.entityExists(rec.id) end) end
		if recAlive then
			best, bestD = rec.id, 0
		else
			for _, id in pairs(game.interface.getEntities({ pos = { c.x, c.y }, radius = 5 },
					{ type = "CONSTRUCTION", includeData = false }) or {}) do
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
				if co and po and co.transf and (not c.file or tostring(co.fileName) == tostring(c.file)) then
					local dx, dy = co.transf[13] - c.x, co.transf[14] - c.y
					local d = dx * dx + dy * dy
					if d < 1.0 and (not bestD or d < bestD) then best, bestD = id, d end
				end
			end
		end
		if not best then
			log(string.format("EXEC CONFAIL seq=%s: no %s of ours within 1 m of %.1f,%.1f -- already gone, nothing rolled back",
				tostring(c.seq), tostring(c.file), c.x, c.y))
			return
		end
		-- Our own removal detector must not echo this back to the peer as a
		-- player demolish: mark the spot first, exactly as execDemolish does.
		expectedDemolish[key] = true
		consByKey[key] = nil
		if CM.cmMode == "companies" then
			local hasCon = false
			pcall(function() hasCon = api.engine.getComponent(best, api.type.ComponentType.CONSTRUCTION) ~= nil end)
			if hasCon then pcall(game.interface.setBulldozeable, best, true) end
		end
		local done = pcall(game.interface.bulldoze, best)
		log(string.format("EXEC CONFAIL seq=%s: %s rolled back locally (entity %d at %.1f,%.1f) -- the peer refused it",
			tostring(c.seq), tostring(c.file), best, c.x, c.y))
		if not done then log(string.format("EXEC CONFAIL seq=%s: bulldoze REFUSED -- worlds now differ", tostring(c.seq))) end
	end)
	if not ok then log("execConFail error: " .. tostring(err)) end
end

local function execute(c)
	if c.op == "CONP" or c.op == "CONX" then execConX(c)
	elseif c.op == "CONU" then execConU(c)
	elseif c.op == "ROADP" then execPolyline(c)
	elseif c.op == "ROAD" or c.op == "RAIL" then execEdge(c)
	elseif c.op == "CON" then execCon(c)
	elseif c.op == "DEMOLISH" then execDemolish(c)
	elseif c.op == "CONFAIL" then execConFail(c)
	elseif c.op == "VBUY" then execVBuy(c)
	elseif c.op == "VREPL" then execVReplace(c)
	elseif c.op == "VSELL" or c.op == "VDEPOT" or c.op == "VLINE" or c.op == "VREV" then execVehCmd(c)
	elseif c.op == "STOPADD" then CM.execStopAdd(c)
	elseif c.op == "STOPDEL" then CM.execStopDel(c)
	elseif c.op == "VNAME" then execSetName(c)
	elseif c.op == "VCOLOR" then execSetColor(c)
	elseif c.op == "LCREATE" or c.op == "LUPDATE" or c.op == "LDELETE" then execLine(c)
	else log("unknown op: " .. tostring(c.op)) end
end

local function groundAt(x, y)
	local z = 0
	pcall(function() z = game.interface.getHeight({ x, y }) or 0 end)
	return z or 0
end

-- ---------- wire ----------
local function broadcast(line)
	if K.CAPTURE_FILE then appendLine(K.CAPTURE_FILE, line) end
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

function scheduleLocal(op, args)
	local now = gameTime()
	if not now then return end
	seqNo = seqNo + 1
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
	local _, fastT = peerBounds()
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
	local at = tonumber(string.format("%.4f",
		now + delay + (tonumber(args and args.delay or 0) or 0)))
	local c = { op = op, at = at, origin = K.INSTANCE, seq = seqNo }
	for k, v in pairs(args) do c[k] = v end
	-- companies mode: stamp the originating company so the peer can attribute the
	-- resulting entity. No-op in coop => the wire form is unchanged there.
	CM.cmEnsure()
	if CM.cmMode == "companies" and CM.cmMyCompany then c.company = CM.cmMyCompany end
	queue[#queue + 1] = c
	-- The originator does NOT execute now. It queues for the same stamp as
	-- everyone else -- that is the whole point. Applying locally and shipping a
	-- copy is what the old state-diff design did, and it is why the two worlds
	-- were never actually in step.
	broadcast(encodeCmd(c))
	log(string.format("SCHED %s seq=%d at=%.4f (now=%.4f)", op, seqNo, at, now))
end

-- Compare our hash against the peer's for one stamp, whichever arrived last.
-- Declared ABOVE onLine because it is called from there: a local declared later
-- resolves to a nil global at the call site, which is how an entire sweep in
-- mpbridge silently aborted for hours (see the lastReplayTick note there).
local comparedAt = {}
-- One peer's hash for one stamp against ours. compareAt (below) runs this for
-- every peer that has reported the stamp, once each.
local function compareOne(stamp, origin, theirs, dt)
	local mine = myHashes[stamp]
	if not mine or not theirs then return end
	local pr = peerFor(origin)
	if mine == theirs then
		pr.streak = 0
		log(string.format("SYNC t=%d hash=%s (%s)", stamp, mine, origin))
		if not (comparedAt[stamp] and comparedAt[stamp].bad) then CM.dashVerdict = "SYNC" end
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
		desyncs = desyncs + 1
		comparedAt[stamp].bad = true
		log(string.format("!! DESYNC t=%d mine=%s peer %s=%s (total %d)",
			stamp, mine, origin, theirs, desyncs))
		-- WHICH component diverged. A single opaque number proves the worlds
		-- differ but says nothing about where, and the two candidate causes need
		-- opposite responses: a real divergence in the simulation is a bug in
		-- replication, whereas a difference confined to entity IDs means the
		-- worlds agree and the DETECTOR is over-sensitive. Reporting per-component
		-- counts and hashes separates them on sight.
		local dm = myDetails[stamp]
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
			local pr = peerFor(o)
			pr.time = t; pr.at = ticks
			peerSeen = true
		end
	elseif op == "LSSPEED" then
		-- The other player moved the speed lever: follow. Speed is local pacing,
		-- not simulated state, so it is applied on arrival, not at a stamp.
		local v = tonumber(line:match("v=(%d+)"))
		if v then
			local s0
			pcall(function() s0 = game.interface.getGameSpeed() end)
			if s0 ~= v then
				CM.baseSpeed = v
				CM.catchingUp = false
				CM.lastSeenSpeed = v      -- so shareSpeed does not echo it back
				CM.setSpeed(v, "the other player set it")
			end
		end
	elseif op == "LSCMD" then
		local c = decodeCmd(line)
		if c then
			-- our own command coming back off the wire; already queued
			if c.origin ~= K.INSTANCE then
				queue[#queue + 1] = c
				-- A command whose stamp has already passed here will execute at a
				-- DIFFERENT sim time than it did on the originator, which is a
				-- desync rather than a late delivery. It is the exact failure
				-- K.EXEC_DELAY > K.BARRIER_AHEAD exists to prevent, so say so loudly
				-- if it ever happens instead of letting it look like a mystery
				-- hash mismatch later.
				local now = gameTime()
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
	elseif op == "LSHASH" then
		local t = tonumber(line:match("t=(%-?%d+)"))
		local h = line:match("h=(%S+)")
		if t and h then
			local o = line:match(" o=(%a)") or "?"
			local pr = peerFor(o)
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
			compareAt(t)
		end
	end
end

local function pollEvents()
	if not K.EVENTS_FILE then return end
	local data, newOff = readFrom(K.EVENTS_FILE, eventsOffset)
	eventsOffset = newOff
	if not data then return end
	for line in data:gmatch("[^\r\n]+") do
		local ok, err = pcall(onLine, line)
		if not ok then log("parse error: " .. tostring(err)) end
	end
end

-- Test injection. Real UI capture needs the native hook that can cancel a local
-- command before the engine applies it; until that exists, commands enter here.
-- ---------- ground-truth sweeps ----------
--
-- Drive KNOWN values into the proposal factory and let the native hook record
-- where they land. This inverts the method that failed repeatedly this session:
-- instead of watching one player build and inferring a field's meaning from a
-- single sample, sweep a parameter and see which offset tracks it.
--
-- NON-DESTRUCTIVE. api.cmd.make.buildProposal IS the factory call -- it builds
-- the Command and fires the hook. Without a matching sendCommand, nothing is
-- ever applied, so a sweep of hundreds of values leaves the world untouched.
--
-- Samples are self-identifying: node 0's X carries 900000 + testId*1000 + index,
-- so every capture is labelled in-band and nothing depends on matching by order.
K.GT_BASE_X = 900000

local function gtProposal(testId, index, isTrack, streetType, trackType, catenary)
	local sp = api.type.SimpleProposal.new()
	local x = K.GT_BASE_X + testId * 1000 + index
	local n0 = api.type.NodeAndEntity.new()
	n0.entity = -900001
	n0.comp.position = api.type.Vec3f.new(x, 0, 0)
	local n1 = api.type.NodeAndEntity.new()
	n1.entity = -900002
	n1.comp.position = api.type.Vec3f.new(x + 20, 0, 0)
	sp.streetProposal.nodesToAdd[1] = n0
	sp.streetProposal.nodesToAdd[2] = n1

	local e = api.type.SegmentAndEntity.new()
	e.entity = -900003
	e.comp.node0 = -900001
	e.comp.node1 = -900002
	e.comp.tangent0 = api.type.Vec3f.new(20, 0, 0)
	e.comp.tangent1 = api.type.Vec3f.new(20, 0, 0)
	e.comp.type = 0
	e.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
	e.type = isTrack and 1 or 0
	if isTrack then
		e.trackEdge = api.type.BaseEdgeTrack.new()
		e.trackEdge.trackType = trackType
		e.trackEdge.catenary = catenary and true or false
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = 16
	else
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = streetType
		e.streetEdge.hasBus = false
		e.streetEdge.tramTrackType = 0
	end
	sp.streetProposal.edgesToAdd[1] = e

	-- Factory only. No sendCommand: the world never sees this.
	api.cmd.make.buildProposal(sp, nil, false)
end

-- ---------- ground truth: CONSTRUCTIONS ----------
--
-- STEP 1 of the skeptic's plan: does a script-built construction proposal reach
-- make_cmd::BuildProposal at all, and with which API shape? M5 recorded that
-- every constructionsToAdd configuration threw, but none of those attempts
-- survive, and the serialiser that fed them was later found to truncate module
-- tables to "?" (mpbridge.lua 109-113) -- so the negative may have been the
-- serialiser's, not the API's. Every statement is pcall'd separately so the
-- exact failing call and its message are on record, not just "it threw".
--
-- Factory only, no sendCommand: nothing touches the world, so used-ground and
-- seed hazards do not apply.
local function gtLoadStationParams()
	local f = io.open(K.BASE .. "gt_station_params.lua", "r")
	if not f then return nil, "no gt_station_params.lua" end
	local s = f:read("*a"); f:close()
	local fn, err = load("return " .. s)
	if not fn then return nil, "params parse: " .. tostring(err) end
	local ok, t = pcall(fn)
	if not ok then return nil, "params eval: " .. tostring(t) end
	t.seed = nil            -- reusing a seed drives errorState critical elsewhere
	return t
end

local function gtTransf(x, y, z)
	-- Try the typed Mat4f first (public-API shape, unverified on this build),
	-- then a plain 16-number column-major table. Report which one took.
	local m
	local ok = pcall(function()
		m = api.type.Mat4f.new(
			api.type.Vec4f.new(1, 0, 0, 0), api.type.Vec4f.new(0, 1, 0, 0),
			api.type.Vec4f.new(0, 0, 1, 0), api.type.Vec4f.new(x, y, z, 1))
	end)
	if ok and m then return m, "Mat4f.new(4xVec4f)" end
	return { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }, "table16"
end

-- One construction proposal through the factory. Returns (fired?, detail).
local function gtConFile(testId, index, params, mutate, fileName)
	local x = K.GT_BASE_X + testId * 1000 + index
	local y, z = -7890.25, 33.125          -- odd values: cannot be confused with anything else
	local steps = {}
	local function step(name, fn)
		local ok, err = pcall(fn)
		steps[#steps + 1] = name .. (ok and " ok" or (" FAIL: " .. tostring(err)))
		return ok
	end
	local sp, ce, tr, trKind
	if not step("SimpleProposal.new", function() sp = api.type.SimpleProposal.new() end) then return false, steps end
	if not step("ConstructionEntity.new", function() ce = api.type.SimpleProposal.ConstructionEntity.new() end) then
		return false, steps
	end
	step("fileName", function() ce.fileName = fileName or "station/rail/modular_station/modular_station.con" end)
	step("params", function()
		local p = params
		if mutate then p = mutate(params, index) end
		ce.params = p
	end)
	step("transf", function() tr, trKind = gtTransf(x, y, z); ce.transf = tr end)
	steps[#steps] = steps[#steps] .. " (" .. tostring(trKind) .. ")"
	step("playerEntity", function() ce.playerEntity = api.engine.util.getPlayer() end)
	step("name", function() ce.name = "gt" end)
	if not step("constructionsToAdd[1]", function() sp.constructionsToAdd[1] = ce end) then return false, steps end
	local fired = step("make.buildProposal", function() api.cmd.make.buildProposal(sp, nil, false) end)
	return fired, steps
end

local function gtCon(testId, index, params, mutate)
	return gtConFile(testId, index, params, mutate, nil)
end

-- Deep-copy a params table so a per-sample mutation cannot leak into the next.
function deepcopy(t)
	if type(t) ~= "table" then return t end
	local r = {}
	for k, v in pairs(t) do r[k] = deepcopy(v) end
	return r
end

local function runGroundTruthCon()
	local params, perr = gtLoadStationParams()
	if not params then log("GT con: " .. tostring(perr)); return end
	local nMods = 0
	for _ in pairs(params.modules or {}) do nMods = nMods + 1 end
	log(string.format("GT con: params loaded, %d modules, seed stripped", nMods))

	-- test 10: the GATE plus translation sweep. Four samples, only x varies.
	local fired, steps = gtCon(10, 0, params)
	for _, s in ipairs(steps) do log("GT con   " .. s) end
	if not fired then
		log("GT con: GATE FAILED -- constructionsToAdd does not reach the factory " ..
			"with this shape. See the FAIL line above for the exact statement.")
		-- Variants, for the record: is it the payload or the API?
		local withSeed = deepcopy(params); withSeed.seed = 1
		local depot = { year = 1850, paramX = 0, paramY = 0 }
		local depotSeed = { year = 1850, paramX = 0, paramY = 0, seed = 1 }
		local function try(label, p, file)
			local f2, st2 = gtConFile(12, 0, p, nil, file)
			log(string.format("GT con   variant %-22s -> %s", label, f2 and "FIRED" or st2[#st2]))
		end
		try("station+seed", withSeed, "station/rail/modular_station/modular_station.con")
		try("depot minimal", depot, "depot/road_depot_era_a.con")
		try("depot+seed", depotSeed, "depot/road_depot_era_a.con")
		try("station params={}", {}, "station/rail/modular_station/modular_station.con")
		return
	end
	for i = 1, 3 do gtCon(10, i, params) end
	log("GT con: gate PASSED, translation sweep 4 samples issued")

	-- test 11: module-identity sentinel. Same station, but slot 8401000's
	-- updateScript.fileName carries a string sentinel and its variant an int
	-- one. Strings are what the hook's pointer chase finds most reliably; if
	-- "GTSENT_<i>.lua" is reachable from a3 the ModuleInfo map is too, and its
	-- layout falls out of the chase path. If it is nowhere in the tree, module
	-- params never reach this command and must travel out-of-band.
	local function mutate(p, i)
		local q = deepcopy(p)
		local m = q.modules and q.modules[8401000]
		if m then
			m.variant = 100 + i
			m.updateScript = m.updateScript or {}
			m.updateScript.fileName = string.format("GTSENT_%d.lua", i)
			m.updateScript.params = { zz = 4242.5 + i }
		end
		return q
	end
	for i = 0, 3 do gtCon(11, i, params, mutate) end
	log("GT con: module-sentinel sweep 4 samples issued (world untouched)")
end

-- pcall-per-statement runner shared by the vehicle/line/demolish sweeps below,
-- mirroring gtCon: the FIRST failing API call is named in the log, not just
-- "it threw". buildAndFire(step) returns whether the factory call fired.
local function gtSweepSample(chan, test, i, buildAndFire)
	local steps = {}
	local function step(name, fn)
		local ok, err = pcall(fn)
		steps[#steps + 1] = name .. (ok and " ok" or (" FAIL: " .. tostring(err)))
		return ok
	end
	local fired = buildAndFire(step)
	if not fired then
		log(string.format("GT %s t%d.%d FAILED:", chan, test, i))
		for _, s in ipairs(steps) do log("GT " .. chan .. "   " .. s) end
	end
	return fired and true or false
end

-- ---------- ground truth: VEHICLES (tests 30..40) ----------
--
-- Factory-only: api.cmd.make.buyVehicle/sellVehicle/sendToDepot/replaceVehicle
-- with NO sendCommand -- the factory builds the Command and fires the native
-- hook, nothing is ever applied. The buy sentinel rides in depotEntity (the
-- factory only asserts != -1). The config recipe is copied verbatim from the
-- measured-working mpbridge builder (mpbridge.lua:1651-1711): read-modify-write
-- every vector property (sol2 hands them back BY VALUE; in-place writes are
-- silently lost), vehicleGroups summing to #vehicles. One field varies per
-- test; everything else stays at the known-good defaults.
local function gtVehPickModel()
	-- A REAL model id + its compartments count, from api.res.modelRep. Ids are
	-- resolved through find(fileName) so no assumption is made about getAll()'s
	-- key base. If nothing resolves, the config sweeps are SKIPPED, not guessed.
	local mid, nComp, kind
	local ok, err = pcall(function()
		local all = api.res.modelRep.getAll()
		for _, fn in pairs(all) do
			if type(fn) == "string"
				and (fn:find("vehicle/train/", 1, true) or fn:find("vehicle/road/", 1, true)) then
				local cand = api.res.modelRep.find(fn)
				if cand and cand >= 0 then
					local md = api.res.modelRep.get(cand)
					local tv = md and md.metadata and md.metadata.transportVehicle
					if tv and tv.compartments then
						local c = 0
						for _ in pairs(tv.compartments) do c = c + 1 end
						if c > 0 then mid = cand; nComp = c; kind = fn:find("vehicle/train/", 1, true) and "train" or "road"; return end
					end
				end
			end
		end
	end)
	if not ok then return nil, nil, "modelRep scan FAIL: " .. tostring(err) end
	if not mid then return nil, nil, "no train/road vehicle with compartments in modelRep" end
	return mid, nComp, nil, kind
end

-- Build a TransportVehicleConfig with nUnits units, defaults from the mpbridge
-- recipe, then apply the per-test overrides in mut. Returns nil on any failure
-- (the failing statement is already in steps via step()).
local function gtVehConfig(step, mid, nComp, nUnits, mut)
	local config
	if not step("TransportVehicleConfig.new", function()
		config = api.type.TransportVehicleConfig.new()
	end) then return nil end
	for u = 1, nUnits do
		local part, tvp
		if not step("VehiclePart.new", function() part = api.type.VehiclePart.new() end) then return nil end
		step("part.modelId", function() part.modelId = mut.modelId or mid end)
		step("part.loadConfig", function()
			local lc = part.loadConfig
			if mut.loadConfig then
				for c = 1, #mut.loadConfig do lc[c] = mut.loadConfig[c] end
			else
				for c = 1, nComp do lc[c] = 0 end
			end
			part.loadConfig = lc
		end)
		step("part.reversed", function() part.reversed = mut.reversed or false end)
		step("part.color", function()
			local cv = mut.color or { -1, -1, -1 }
			part.color = api.type.Vec3f.new(cv[1], cv[2], cv[3])
		end)
		step("part.logo", function() part.logo = mut.logo or "" end)
		if not step("TransportVehiclePart.new", function()
			tvp = api.type.TransportVehiclePart.new()
		end) then return nil end
		step("tvp.purchaseTime", function() tvp.purchaseTime = mut.purchaseTime or 0 end)
		step("tvp.maintenanceState", function() tvp.maintenanceState = mut.ms or 1.0 end)
		step("tvp.targetMaintenanceState", function() tvp.targetMaintenanceState = mut.tms or 0 end)
		step("tvp.autoLoadConfig", function()
			local alc = tvp.autoLoadConfig
			if mut.autoLoadConfig then
				for c = 1, #mut.autoLoadConfig do alc[c] = mut.autoLoadConfig[c] end
			else
				for c = 1, nComp do alc[c] = 1 end
			end
			tvp.autoLoadConfig = alc
		end)
		step("tvp.part", function() tvp.part = part end)
		step("config.vehicles[" .. u .. "]", function() config.vehicles[u] = tvp end)
	end
	step("config.vehicleGroups", function()
		local grp = config.vehicleGroups
		if mut.groups then
			for g = 1, #mut.groups do grp[g] = mut.groups[g] end
		else
			grp[1] = nUnits
		end
		config.vehicleGroups = grp
	end)
	return config
end

local function runGroundTruthVeh()
	local mid, nComp, perr = gtVehPickModel()
	if not mid then
		log("GT veh: " .. tostring(perr) .. " -- config sweeps t30-t36/t40 SKIPPED")
	else
		log(string.format("GT veh: model id %d, %d compartments", mid, nComp))
	end
	local function buySweep(test, label, nUnits, mutFor)
		if not mid then
			log(string.format("GT veh t%d (%s): skipped (no model)", test, label))
			return
		end
		local fired = 0
		for i = 0, 7 do
			if gtSweepSample("veh", test, i, function(step)
				local cfg = gtVehConfig(step, mid, nComp, nUnits, mutFor(i))
				if not cfg then return false end
				return step("make.buyVehicle", function()
					api.cmd.make.buyVehicle(api.engine.util.getPlayer(),
						K.GT_BASE_X + test * 1000 + i, cfg)
				end)
			end) then fired = fired + 1 end
		end
		log(string.format("GT veh t%d (%s): %d/8 fired", test, label, fired))
	end
	buySweep(30, "modelId", 1, function(i) return { modelId = 700000 + i } end)
	buySweep(31, "loadConfig", 1, function(i) return { loadConfig = { i, i + 1 } } end)
	buySweep(32, "purchaseTime", 1, function(i) return { purchaseTime = 123456000 + i } end)
	buySweep(33, "maint floats", 1, function(i) return { ms = i / 10, tms = 1 - i / 10 } end)
	buySweep(34, "autoLoadConfig", 1, function(i) return { autoLoadConfig = { i } } end)
	buySweep(35, "vehicleGroups", 3, function(i) return { groups = { i, 3 - i } } end)
	buySweep(36, "rev/color/logo", 1, function(i)
		return { reversed = (i % 2 == 1), color = { i, i, i }, logo = "gt" .. i }
	end)
	log("GT veh t37: skipped here on purpose -- live readback (sendCommand + real"
		.. " depot), belongs to STEP 5, not a factory-only sweep")
	-- t38 SellVehicle: sentinel is the vehicle entity (ends up INSIDE the
	-- native vector<Entity>; the hook reads *(int*)[r8]).
	local f38 = 0
	for i = 0, 7 do
		local ok, err = pcall(function()
			api.cmd.make.sellVehicle(K.GT_BASE_X + 38 * 1000 + i)
		end)
		if ok then f38 = f38 + 1 else log("GT veh t38." .. i .. " FAIL: " .. tostring(err)) end
	end
	log(string.format("GT veh t38 (sellVehicle): %d/8 fired", f38))
	-- t39 SendToDepot(sentinel, bool): r8 = entity, r9 = bool tracks i%2
	local f39 = 0
	for i = 0, 7 do
		local ok, err = pcall(function()
			api.cmd.make.sendToDepot(K.GT_BASE_X + 39 * 1000 + i, i % 2 == 1)
		end)
		if ok then f39 = f39 + 1 else log("GT veh t39." .. i .. " FAIL: " .. tostring(err)) end
	end
	log(string.format("GT veh t39 (sendToDepot): %d/8 fired", f39))
	-- t40 ReplaceVehicle(sentinel, cfg) -- only if the Lua maker exists here
	local mkRep
	pcall(function() mkRep = api.cmd.make.replaceVehicle end)
	if mkRep == nil then
		log("GT veh t40: skipped (api.cmd.make.replaceVehicle not present on this build)")
	elseif not mid then
		log("GT veh t40: skipped (no model for config)")
	else
		local f40 = 0
		for i = 0, 7 do
			if gtSweepSample("veh", 40, i, function(step)
				local cfg = gtVehConfig(step, mid, nComp, 1, {})
				if not cfg then return false end
				return step("make.replaceVehicle", function()
					mkRep(K.GT_BASE_X + 40 * 1000 + i, cfg)
				end)
			end) then f40 = f40 + 1 end
		end
		log(string.format("GT veh t40 (replaceVehicle): %d/8 fired", f40))
	end
end

-- ---------- ground truth: LINES (tests 10..18, T1..T9) ----------
--
-- Factory-only api.cmd.make.updateLine(sentinelLineEntity, line): the factory
-- asserts only lineEntity != -1, so the sentinel passes and nothing touches
-- the world. Line shape copied from the measured mptest buildLineObject.
-- NOTE (r8 plan step 0): CreateLine/UpdateLine must NEVER be cancelled live;
-- this sweep never sends, so no cancel can arise here either.
local function gtLineObject(step, nStops, wait, mutStop)
	local line
	if not step("Line.new", function() line = api.type.Line.new() end) then return nil end
	step("waitingTime", function() line.waitingTime = wait or 180 end)
	for k = 1, nStops do
		local s
		if not step("Line.Stop.new", function() s = api.type.Line.Stop.new() end) then return nil end
		step("stationGroup", function() s.stationGroup = 800000 + k end)
		step("station", function() s.station = 0 end)
		step("terminal", function() s.terminal = 0 end)
		step("loadMode", function() s.loadMode = 0 end)
		step("minWaitingTime", function() s.minWaitingTime = 0 end)
		step("maxWaitingTime", function() s.maxWaitingTime = 180 end)
		if mutStop then mutStop(step, s, k) end
		step("stops[" .. k .. "]", function() line.stops[k] = s end)
	end
	return line
end

local function runGroundTruthLine()
	local function sweep(test, label, nStopsFor, waitFor, mutFor)
		local fired = 0
		for i = 0, 7 do
			if gtSweepSample("line", test, i, function(step)
				local line = gtLineObject(step, nStopsFor(i),
					waitFor and waitFor(i) or nil, mutFor and mutFor(i) or nil)
				if not line then return false end
				return step("make.updateLine", function()
					api.cmd.make.updateLine(K.GT_BASE_X + test * 1000 + i, line)
				end)
			end) then fired = fired + 1 end
		end
		log(string.format("GT line t%d (%s): %d/8 fired", test, label, fired))
	end
	local one = function() return 1 end
	-- T1 (10): stop count = i -> stops-vector span tracks 0xa8*i
	sweep(10, "stop count", function(i) return i end)
	-- T2 (11): waitingTime = 100+i -> Line+0x18
	sweep(11, "waitingTime", one, function(i) return 100 + i end)
	-- T3 (12): stop.terminal = i  \ these two DECIDE the +0x04/+0x08 order --
	-- T4 (13): stop.station  = i  / nothing ships before they disagree/agree
	sweep(12, "terminal", one, nil, function(i)
		return function(step, s) step("mut terminal", function() s.terminal = i end) end
	end)
	sweep(13, "station", one, nil, function(i)
		return function(step, s) step("mut station", function() s.station = i end) end
	end)
	-- T5 (14): loadMode = i%3 -> +0x28
	sweep(14, "loadMode", one, nil, function(i)
		return function(step, s) step("mut loadMode", function() s.loadMode = i % 3 end) end
	end)
	-- T6 (15): minWaitingTime = i, maxWaitingTime = 100+i -> +0x2c/+0x30
	sweep(15, "min/maxWait", one, nil, function(i)
		return function(step, s)
			step("mut minWaitingTime", function() s.minWaitingTime = i end)
			step("mut maxWaitingTime", function() s.maxWaitingTime = 100 + i end)
		end
	end)
	-- T7 (16): #alternativeTerminals = i with {station=i, terminal=7-i}
	-- -> span 8*i at stop+0x10; StationTerminal.new existence is INFERRED,
	-- a FAIL line here is itself the measurement.
	sweep(16, "altTerminals", one, nil, function(i)
		return function(step, s)
			step("mut alternativeTerminals", function()
				local at = s.alternativeTerminals
				for a = 1, i do
					local t = api.type.StationTerminal.new()
					t.station = i
					t.terminal = 7 - i
					at[a] = t
				end
				s.alternativeTerminals = at
			end)
		end
	end)
	-- T8 (17): stationGroup = 800000+i -> stop+0x00
	sweep(17, "stationGroup", one, nil, function(i)
		return function(step, s) step("mut stationGroup", function() s.stationGroup = 800000 + i end) end
	end)
	-- T9 (18): createLine, name length crossing the SSO boundary (4 chars even
	-- i, 20 chars odd i -> heap), colour=(i,0,0), player sentinel in r9; plus
	-- deleteLine/setLine sentinels (args register-visible in [cap], no dump).
	local f18 = 0
	for i = 0, 7 do
		local sent = K.GT_BASE_X + 18 * 1000 + i
		if gtSweepSample("line", 18, i, function(step)
			local line = gtLineObject(step, 1)
			if not line then return false end
			local name = "gtl" .. i                              -- 4 chars: SSO
			if i % 2 == 1 then name = name .. string.rep("z", 16) end -- 20: heap
			return step("make.createLine", function()
				api.cmd.make.createLine(name, api.type.Vec3f.new(i, 0, 0), sent, line)
			end)
		end) then f18 = f18 + 1 end
		local okd, errd = pcall(function() api.cmd.make.deleteLine(sent) end)
		if not okd then log("GT line t18." .. i .. " deleteLine FAIL: " .. tostring(errd)) end
		local oks, errs = pcall(function() api.cmd.make.setLine(sent, 800000 + i, i) end)
		if not oks then log("GT line t18." .. i .. " setLine FAIL: " .. tostring(errs)) end
	end
	log(string.format("GT line t18 (createLine SSO + deleteLine/setLine): %d/8 fired", f18))
end

-- ---------- ground truth: DEMOLISH (tests 4..6) ----------
--
-- Factory-only buildProposal carrying the SAME valid 2-node/1-edge street the
-- existing street sweep uses (so scripting::Convert accepts the proposal) with
-- the sentinel in node0.position.x for in-band labelling, PLUS removal entries
-- whose entity id IS the sentinel. Predictions (r9 step 2): t4 span48==120,
-- r48+0x00==sentinel (sample 8 carries TWO edges -> 240); t5 span30==24,
-- r30+0x14==sentinel; t6 span1e0==4, r1e0[0]==sentinel or 'refused'.
local function gtDemol(test, i, mut)
	return gtSweepSample("demolish", test, i, function(step)
		local x = K.GT_BASE_X + test * 1000 + i
		local sp
		if not step("SimpleProposal.new", function() sp = api.type.SimpleProposal.new() end) then
			return false
		end
		step("nodesToAdd", function()
			local n0 = api.type.NodeAndEntity.new()
			n0.entity = -900001
			n0.comp.position = api.type.Vec3f.new(x, 0, 0)
			local n1 = api.type.NodeAndEntity.new()
			n1.entity = -900002
			n1.comp.position = api.type.Vec3f.new(x + 20, 0, 0)
			sp.streetProposal.nodesToAdd[1] = n0
			sp.streetProposal.nodesToAdd[2] = n1
		end)
		step("edgesToAdd", function()
			local e = api.type.SegmentAndEntity.new()
			e.entity = -900003
			e.comp.node0 = -900001
			e.comp.node1 = -900002
			e.comp.tangent0 = api.type.Vec3f.new(20, 0, 0)
			e.comp.tangent1 = api.type.Vec3f.new(20, 0, 0)
			e.comp.type = 0
			e.comp.typeIndex = -1   -- native road edges: typeIndex=-1 (live probe)
			e.type = 0
			e.streetEdge = api.type.BaseEdgeStreet.new()
			e.streetEdge.streetType = 16
			e.streetEdge.hasBus = false
			e.streetEdge.tramTrackType = 0
			sp.streetProposal.edgesToAdd[1] = e
		end)
		-- If the removal entry cannot be set, do NOT fire: a sample without its
		-- removal entry would show span 0 and pollute the correlation.
		if not mut(step, sp, x) then return false end
		return step("make.buildProposal", function() api.cmd.make.buildProposal(sp, nil, false) end)
	end)
end

local function runGroundTruthDemol()
	-- test 4: edgesToRemove -- 8 single-edge samples + sample 8 with two edges
	local f4 = 0
	for i = 0, 8 do
		if gtDemol(4, i, function(step, sp, x)
			return step("edgesToRemove", function()
				sp.streetProposal.edgesToRemove[1] = x
				if i == 8 then sp.streetProposal.edgesToRemove[2] = x + 100 end
			end)
		end) then f4 = f4 + 1 end
	end
	log(string.format("GT demolish t4 (edgesToRemove): %d/9 fired (sample 8 = two edges)", f4))
	-- test 5: nodesToRemove
	local f5 = 0
	for i = 0, 7 do
		if gtDemol(5, i, function(step, sp, x)
			return step("nodesToRemove", function()
				sp.streetProposal.nodesToRemove[1] = x
			end)
		end) then f5 = f5 + 1 end
	end
	log(string.format("GT demolish t5 (nodesToRemove): %d/8 fired", f5))
	-- test 6: constructionsToRemove -- may be refused like constructionsToAdd
	-- was; the refusal IS the recorded answer (fall back to the 3-sample UI
	-- differential, r9 step 2).
	local f6 = 0
	for i = 0, 7 do
		if gtDemol(6, i, function(step, sp, x)
			return step("constructionsToRemove", function()
				sp.constructionsToRemove[1] = x
			end)
		end) then f6 = f6 + 1 end
	end
	if f6 == 0 then
		log("GT demolish t6: refused -- constructionsToRemove never reached the factory")
	else
		log(string.format("GT demolish t6 (constructionsToRemove): %d/8 fired", f6))
	end
end
local function runGroundTruth(what)
	local n = 0
	local ok, err = pcall(function()
		if what == "track" then
			-- test 1: trackType 0..7 with catenary off
			for v = 0, 7 do gtProposal(1, v, true, 16, v, false); n = n + 1 end
			-- test 2: SAME trackType, catenary on -- isolates the catenary bit
			for v = 0, 7 do gtProposal(2, v, true, 16, v, true); n = n + 1 end
		elseif what == "con" then
			runGroundTruthCon(); return
		elseif what == "street" then
			-- test 3: streetType 0..39
			for v = 0, 39 do gtProposal(3, v, false, v, 1, false); n = n + 1 end
		elseif what == "vehicle" then
			runGroundTruthVeh(); return
		elseif what == "line" then
			runGroundTruthLine(); return
		elseif what == "demolish" then
			runGroundTruthDemol(); return
		else
			log("GT: unknown sweep '" .. tostring(what)
				.. "' (try track|street|con|vehicle|line|demolish)")
			return
		end
	end)
	if ok then
		log(string.format("GT: %s sweep issued %d samples (world untouched)", what, n))
	else
		log("GT error: " .. tostring(err))
	end
end
-- Watch for constructions the player just built and ship them. Runs on every
-- peer; a construction that WE replayed is recognised by its position and not
-- echoed back, otherwise two peers would ping-pong the same station forever.
-- Only the PLAYER's constructions replicate. Town growth creates CONSTRUCTION
-- entities continuously (building/era_b/res_1_2x2_01.con and friends -- about
-- one every ten seconds per town), and the first version of this poll shipped
-- twenty of them from EACH side in three minutes: every peer would have
-- replayed the other's town growth on top of its own. A blacklist of guessed
-- prefixes was the wrong shape; ownership is the real discriminator, with a
-- whitelist of things a player can actually place as the fallback.
function isPlayerConstruction(id, fileName)
	local owned = nil
	pcall(function()
		local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
		if po ~= nil then
			local me = api.engine.util.getPlayer()
			owned = (po.player == me)
			-- companies mode: a REMOTE company's copy sits on that company's AI
			-- player, not on me. It is still a player construction we must keep
			-- tracking (consByKey) -- otherwise the very act of reassigning it
			-- evicts it, and the owner's later demolish/edit finds nothing.
			if not owned and CM.cmMode == "companies" then
				for _, pid in pairs(CM.cmCompanyPid) do
					if po.player == pid then owned = true; break end
				end
			end
		else
			owned = false
		end
	end)
	if owned ~= nil then return owned end
	-- component check unavailable: fall back to what a player can build
	for _, p in ipairs({ "station/", "depot/", "asset/", "airport/", "harbor/", "harbour/" }) do
		if fileName:sub(1, #p) == p then return true end
	end
	return false
end

K.CON_POLL_EVERY = 10
K.CON_EDIT_SCAN_EVERY = 30
K.PRIME_PER_TICK = 100

-- Record (or refresh) what we know about a live construction at its position.
-- Returns the previous record at that position, if any.
local function noteCon(id, fn, key, pstr)
	local prev = consByKey[key]
	consByKey[key] = { id = id, file = fn, params = pstr }
	return prev
end

local function shipEdit(fn, key, pstr)
	local kx, ky = key:match("^([-%d.]+)/([-%d.]+)$")
	scheduleLocal("CONU", { file = fn, x = tonumber(kx), y = tonumber(ky), params = pstr })
	log(string.format("con: edit captured %s at %s params=%dB", fn, key, #pstr))
end

-- Town constructions + asset groups still standing within 120 m of (cx,cy),
-- as "x:y;x:y;..." (nil if none). Shipped with a CONX so the peer removes
-- exactly the in-footprint town buildings that the originator's world no longer
-- has -- no pad, no bbox, no corridor width to guess. Used by BOTH CONX emit
-- paths (queueConCapture and findConstructionForRoadc: the latter never carried
-- survivors, which is why every station shipped `survivors=0`, 2026-08-29).
local function gatherSurvivors(cx, cy, selfId)
	local surv = {}
	pcall(function()
		local near = game.interface.getEntities({ pos = { cx, cy }, radius = 200 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, sid in pairs(near) do
			if sid ~= selfId then
				local cco = api.engine.getComponent(sid, api.type.ComponentType.CONSTRUCTION)
				local po = api.engine.getComponent(sid, api.type.ComponentType.PLAYER_OWNED)
				if cco and po == nil and cco.transf then surv[#surv + 1] = string.format("%.1f:%.1f", cco.transf[13], cco.transf[14]) end
			end
		end
		local assets = game.interface.getEntities({ pos = { cx, cy }, radius = 200 },
			{ type = "ASSET_GROUP", includeData = false }) or {}
		for _, sid in pairs(assets) do
			local okE, e = pcall(game.interface.getEntity, sid)
			local px = okE and e and e.position and (e.position[1] or e.position.x)
			local py = okE and e and e.position and (e.position[2] or e.position.y)
			if px and py then surv[#surv + 1] = string.format("%.1f:%.1f", px, py) end
		end
	end)
	CM.cmLog(string.format("STN: gathered %d survivor(s) around (%.1f,%.1f)", #surv, cx, cy))
	return (#surv > 0) and table.concat(surv, ";") or nil
end

local function queueConCapture(fn, key, pstr, transf, id)
	local t = {}
	for i = 1, 16 do t[i] = string.format("%.4f", transf[i]) end
	-- The UI names a construction as part of placing it ("<town> Train
	-- depot"); a replica without that name is the one visible difference
	-- left against a UI-built one, and clicking unnamed script-built
	-- constructions crashed the client. Ship the originator's name.
	local name = ""
	pcall(function()
		local nc = api.engine.getComponent(id, api.type.ComponentType.NAME)
		if nc and nc.name then name = tostring(nc.name) end
	end)
	-- EXACT obstacle clearing: the game's placement demolished only what its real
	-- footprint collided with. We cannot see that list after the fact -- but its
	-- COMPLEMENT is visible: the town constructions/assets still standing nearby.
	-- Ship those survivors (by position); the peer clears only obstacles that are
	-- NOT survivors, so the two towns lose exactly the same buildings. This
	-- replaces the old 60x30 m pad, which over-demolished on the peer.
	local survivors = gatherSurvivors(transf[13], transf[14], id)
	pendingCons[#pendingCons + 1] = { at = gameTime() or 0, file = fn, key = key,
	                                  t = table.concat(t, ","), params = pstr,
	                                  x = transf[13], y = transf[14], name = escName(name),
	                                  survivors = survivors }
end

-- Pair each captured construction with the street payload the hook shipped
-- for the same placement (nearest, within 150 m), and ship ONE command.
-- A construction that nothing pairs with inside a game unit was placed
-- free-standing and ships as plain CONP. A street payload nothing claims
-- inside 6 units is dropped -- loudly.
-- Build and ship the CONX for a construction (cn: {file,t,params,name}) plus its
-- street payload (rc: a parked ROADC).
local function shipConxPair(cn, rc)
	local sn, se, sr, spz = {}, {}, {}, {}
	for id, p in pairs(rc.posOf) do
		sn[#sn + 1] = string.format("%d,%.4f,%.4f,%.4f", id, p[1], p[2], p[3])
	end
	for _, e in ipairs(rc.adds) do
		-- 10 fields: endpoints, tangents, then bridge/tunnel type + typeIndex
		se[#se + 1] = string.format("%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d", e[1], e[2],
			e[3][1], e[3][2], e[3][3], e[3][4], e[3][5], e[3][6], e[4] or 0, e[5] or -1)
	end
	for _, e in ipairs(rc.rms) do
		sr[#sr + 1] = string.format("%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", e[1], e[2],
			e[3][1], e[3][2], e[3][3], e[3][4], e[3][5], e[3][6])
	end
	for id, p in pairs(rc.spos) do
		spz[#spz + 1] = string.format("%d,%.4f,%.4f,%.4f", id, p[1], p[2], p[3])
	end
	scheduleLocal("CONX", { file = cn.file, t = cn.t, params = cn.params, name = cn.name, survivors = cn.survivors,
	                        snodes = table.concat(sn, ";"), sedges = table.concat(se, ";"),
	                        srm = table.concat(sr, ";"), spos = table.concat(spz, ";"),
	                        etype = rc.etype, stype = rc.stype, ttype = rc.ttype, cat = rc.cat })
	log(string.format("con: captured %s + street (%d nodes, %d edges, %d removals) -> CONX",
		cn.file, #sn, #se, #sr))
end

-- Find the player construction that owns a parked ROADC's tracks, by POSITION,
-- regardless of knownCons. This rescues the station-over-buildings case, where
-- the station reuses a demolished building's entity id that is already in
-- knownCons, so pollNewConstructions never captures it.
local function findConstructionForRoadc(rc)
	-- centroid of the street nodes
	local cx, cy, n = 0, 0, 0
	for _, p in pairs(rc.posOf) do cx = cx + p[1]; cy = cy + p[2]; n = n + 1 end
	if n == 0 then return nil end
	cx, cy = cx / n, cy / n
	local best, bestId, bestD
	pcall(function()
		local list = game.interface.getEntities({ pos = { cx, cy }, radius = 120 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, id in pairs(list) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.fileName and co.transf and isPlayerConstruction(id, tostring(co.fileName)) then
				local key = conKey(co.transf[13], co.transf[14])
				-- not one we already track (avoid re-shipping an existing station)
				if not consByKey[key] then
					local d = (co.transf[13] - cx) ^ 2 + (co.transf[14] - cy) ^ 2
					if not bestD or d < bestD then best, bestId, bestD = co, id, d end
				end
			end
		end
	end)
	if not best then return nil end
	local t = {}
	for i = 1, 16 do t[i] = string.format("%.4f", best.transf[i]) end
	local e = game.interface.getEntity(bestId)
	local pstr = (e and e.params) and ser(e.params) or "{}"
	local nm = ""
	pcall(function()
		local nc = api.engine.getComponent(bestId, api.type.ComponentType.NAME)
		if nc and nc.name then nm = tostring(nc.name) end
	end)
	-- register so the demolish/edit trackers see it, and so we do not re-ship it
	local key = conKey(best.transf[13], best.transf[14])
	knownCons[bestId] = true
	noteCon(bestId, tostring(best.fileName), key, pstr)
	return { file = tostring(best.fileName), t = table.concat(t, ","), params = pstr,
	         name = escName(nm), x = best.transf[13], y = best.transf[14], id = bestId,
	         survivors = gatherSurvivors(best.transf[13], best.transf[14], bestId) }
end

local function flushConPairs()
	local now = gameTime()
	if not now then return end
	for ci = #pendingCons, 1, -1 do
		local cn = pendingCons[ci]
		local best, bestD
		for ri, rc in ipairs(pendingRoadc) do
			local d
			for _, p in pairs(rc.posOf) do
				local dd = (p[1] - cn.x) ^ 2 + (p[2] - cn.y) ^ 2
				if not d or dd < d then d = dd end
			end
			if d and d < 150 * 150 and (not bestD or d < bestD) then best, bestD = ri, d end
		end
		if best then
			local rc = table.remove(pendingRoadc, best)
			table.remove(pendingCons, ci)
			shipConxPair(cn, rc)
		elseif now - cn.at > 1.0 then
			table.remove(pendingCons, ci)
			scheduleLocal("CONP", { file = cn.file, t = cn.t, params = cn.params, name = cn.name })
			log(string.format("con: captured %s (free-standing) -> CONP", cn.file))
		end
	end
	for ri = #pendingRoadc, 1, -1 do
		local rc = pendingRoadc[ri]
		-- Active rescue: a ROADC that has waited a beat without a pollNewConstructions
		-- capture (id reuse) -- look for its construction directly by position.
		if now - rc.at > 1.0 and now - rc.at <= 15 then
			local cn = findConstructionForRoadc(rc)
			if cn then
				table.remove(pendingRoadc, ri)
				log(string.format("con: ROADC rescued its construction %s by position (id-reuse case)", cn.file))
				shipConxPair(cn, rc)
			end
		elseif now - rc.at > 15 then
			table.remove(pendingRoadc, ri)
			log("ROADC: no construction paired within 15 units -- street payload dropped")
		end
	end
end

-- ---------- roadside stops (edge objects) ----------
--
-- A small bus or truck stop placed on a street is not a construction. It is an
-- EDGE OBJECT: an entity attached to a street edge at a parameter along it, on
-- one side, carrying STATION + NAME + PLAYER_OWNED and a model
-- (station/bus/small_mid.mdl, station/road/small_cargo.mdl). Measured 2026-08-31
-- through streetSystem.getEdgeObject2EdgeMap. Nothing in the construction or
-- road channels ever saw one, so every stop was a permanent one-sided
-- divergence -- and a line stopping at one could not replicate either ("no
-- station group within 20 m").
--
-- The native placement comes through BuildProposal from caller 0x460e0b, which
-- the slice ignores, so the originator keeps its native stop; peers replay it
-- through SimpleStreetProposal.edgeObjectsToAdd, the same API a Lua mod would
-- use to place a signal. The wire carries positions, never ids: the edge by its
-- two endpoints, the stop by its parameter along that edge and which side.
CM.knownStop   = {}      -- edge-object entity -> {x, y} while it exists
CM.stopPrimed  = false
CM.expectStop  = {}      -- "x/y" -> true : a replay is about to create one here
CM.expectStopDel = {}    -- "x/y" -> true : a replay is about to remove one here

local function stopKey(x, y) return string.format("%.0f/%.0f", x, y) end

-- Everything the wire needs about one edge object, from this instance's world.
-- KIND is what the edge lists the object as, and the engine is strict about
-- it: measured on a live world, a truck stop is {id, 0} and a bus stop {id, 1}
-- (STATION.cargo decides), a signal is {id, 2} (EdgeObjectType.SIGNAL). Listing
-- a truck stop as 1 took the game down in StreetGeometry::CreateLanes.
local function describeStop(eo, eid)
	local d = {}
	local ok = pcall(function()
		local mil = api.engine.getComponent(eo, api.type.ComponentType.MODEL_INSTANCE_LIST)
		local fi = mil.fatInstances[1]
		d.x, d.y, d.z = fi.transf[13], fi.transf[14], fi.transf[15]
		d.model = api.res.modelRep.getName(fi.modelId)
		local nm = api.engine.getComponent(eo, api.type.ComponentType.NAME)
		d.name = nm and nm.name or ""
		local st, sg
		pcall(function() st = api.engine.getComponent(eo, api.type.ComponentType.STATION) end)
		pcall(function() sg = api.engine.getComponent(eo, api.type.ComponentType.SIGNAL_LIST) end)
		if st then
			d.kind = st.cargo and 0 or 1
		elseif sg then
			d.kind = 2
			-- SIGNAL_LIST carries one signal per edge object. Measured on a live
			-- pair (2026-08-31): signals[1].type is 0 for a normal signal and 1
			-- for a one-way one (SignalType: SIGNAL=0, WAYPOINT=2).
			pcall(function() d.stype = sg.signals[1].type end)
			d.oneWay = (tonumber(d.stype) == 1)
		else
			d.kind = 1
		end
		pcall(function() d.track = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
		local comp, a, b, ta, tb = edgeGeomT(eid)
		d.ax, d.ay, d.bx, d.by = a[1], a[2], b[1], b[2]
		local u = CM.uOnEdge(eid, d.x, d.y) or 0.5
		d.u = u
		-- which side of the direction of travel node0 -> node1 the model sits
		local q = hermitePos(a, ta, b, tb, u)
		local t = hermiteTangent(a, ta, b, tb, u)
		local cross = t[1] * (d.y - q[2]) - t[2] * (d.x - q[1])
		d.left = cross > 0
	end)
	return ok and d.model and d
end

local function isPlayerStop(eo)
	local st, sg, po
	pcall(function() st = api.engine.getComponent(eo, api.type.ComponentType.STATION) end)
	pcall(function() sg = api.engine.getComponent(eo, api.type.ComponentType.SIGNAL_LIST) end)
	if not st and not sg then return false end
	pcall(function() po = api.engine.getComponent(eo, api.type.ComponentType.PLAYER_OWNED) end)
	return po ~= nil
end

function CM.pollStops()
	local ok, err = pcall(function()
		local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
		if not CM.stopPrimed then
			-- what the save already had is known, not new
			for eo, eid in pairs(m) do
				if isPlayerStop(eo) then
					local d = describeStop(eo, eid)
					CM.knownStop[eo] = d and { d.x, d.y, model = d.model, kind = d.kind, oneWay = d.oneWay } or { 0, 0 }
				end
			end
			CM.stopPrimed = true
			local n = 0; for _ in pairs(CM.knownStop) do n = n + 1 end
			log(string.format("stops: primed %d roadside stop(s) from the save", n))
			return
		end
		-- Whenever a stop is added to or removed from an edge, the engine
		-- REBUILDS the edge and every stop on it gets a new entity id. A stop
		-- that "vanished" and a stop that "appeared" within a metre of it in the
		-- same poll are the same stop: move the id and say nothing, or every
		-- placement on a shared edge would ship a STOPDEL + STOPADD for each
		-- neighbour (measured live, 2026-08-31).
		local gone = {}
		for eo, pos in pairs(CM.knownStop) do
			if not m[eo] then gone[eo] = pos end
		end
		local fresh = {}
		for eo, eid in pairs(m) do
			if not CM.knownStop[eo] and isPlayerStop(eo) then fresh[#fresh + 1] = { eo, eid } end
		end
		for _, pair in ipairs(fresh) do
			local eo, eid = pair[1], pair[2]
			local d = describeStop(eo, eid)
			if d then
				-- Same spot AND same object: an in-place edit (one-way toggled, model
				-- changed) is a remove + add, not a rebind (review, 2026-09-01).
				local rebound = nil
				for geo, pos in pairs(gone) do
					if (pos[1] - d.x) ^ 2 + (pos[2] - d.y) ^ 2 < 1.0
					   and (pos.model == nil or pos.model == d.model)
					   and (pos.kind == nil or pos.kind == d.kind)
					   and (pos.oneWay == nil or pos.oneWay == d.oneWay) then rebound = geo; break end
				end
				if rebound then
					gone[rebound] = nil
					CM.knownStop[rebound] = nil
					CM.knownStop[eo] = { d.x, d.y, model = d.model, kind = d.kind, oneWay = d.oneWay }
				else
					CM.knownStop[eo] = { d.x, d.y, model = d.model, kind = d.kind, oneWay = d.oneWay }
					local k = stopKey(d.x, d.y)
					if CM.expectStop[k] then
						CM.expectStop[k] = nil          -- our own replay landing
					else
						scheduleLocal("STOPADD", {
							ax = d.ax, ay = d.ay, bx = d.bx, by = d.by,
							u = d.u, left = d.left and 1 or 0,
							x = d.x, y = d.y, kind = d.kind, track = d.track and 1 or 0,
							oneWay = d.oneWay and 1 or 0,
							model = escName(d.model), name = escName(d.name),
							skipOrigin = 1 })
						log(string.format("stops: captured %s '%s' kind=%d %s at %.1f,%.1f u=%.3f left=%s -> STOPADD",
							d.model, d.name, d.kind, d.track and "track" or "street", d.x, d.y, d.u, tostring(d.left)))
					end
				end
			end
		end
		-- genuinely gone (nothing reappeared at the same spot)
		for eo, pos in pairs(gone) do
			CM.knownStop[eo] = nil
			local k = stopKey(pos[1], pos[2])
			if CM.expectStopDel[k] then
				CM.expectStopDel[k] = nil
			else
				scheduleLocal("STOPDEL", { x = pos[1], y = pos[2], skipOrigin = 1 })
				log(string.format("stops: roadside stop at %.1f,%.1f removed -> STOPDEL", pos[1], pos[2]))
			end
		end
	end)
	if not ok then log("stops poll error: " .. tostring(err)) end
end

-- Rebuild a street edge with a given set of stops on it. This is how the game
-- itself does it, and the only shape it accepts (measured 2026-08-31, four
-- variants): an edge object belongs to its edge, so a stop cannot be attached
-- to an EXISTING edge entity -- the proposal removes the edge and re-adds a
-- copy as entity -1 carrying the object list, and every stop on it is re-added
-- with it. Edge objects have their own negative numbering: the k-th entry of
-- edgeObjectsToAdd is entity -k, and the edge lists it as {-k, 1} (kind 1 is a
-- stop; EdgeObjectType.SIGNAL is 2). Listing the object under any other id, or
-- leaving it off the edge, fails silently -- or asserts inside CalcNodeIndex,
-- which walks that very list looking for the object and writes a crash dump.
--
-- stops: list of { u=, left=, model=, name= } relative to node0 -> node1.
local function rebuildEdgeWithStops(eid, stops, why, onDone)
	local comp, a, b, ta, tb = edgeGeomT(eid)
	if not comp then return false, "edge gone" end
	local isTrack = false
	pcall(function() isTrack = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
	-- An object we cannot describe (not a stop) would be destroyed with the
	-- edge. Refuse rather than delete something silently.
	local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
	for eo, e2 in pairs(m) do
		if e2 == eid and not isPlayerStop(eo) then
			return false, string.format("edge %d carries edge object %d that is not a stop", eid, eo)
		end
	end
	table.sort(stops, function(p, q) return p.u < q.u end)
	local sp = api.type.SimpleProposal.new()
	local e = api.type.SegmentAndEntity.new()
	e.entity = -1
	e.comp.node0 = comp.node0
	e.comp.node1 = comp.node1
	e.comp.tangent0 = api.type.Vec3f.new(ta[1], ta[2], ta[3])
	e.comp.tangent1 = api.type.Vec3f.new(tb[1], tb[2], tb[3])
	e.comp.type = comp.type or 0
	e.comp.typeIndex = comp.typeIndex or -1
	e.type = isTrack and 1 or 0
	copyEdgeProps(e, eid, isTrack, nil)
	local objs = {}
	for k, st in ipairs(stops) do objs[#objs + 1] = { -k, tonumber(st.kind) or 1 } end
	e.comp.objects = objs
	sp.streetProposal.edgesToAdd[1] = e
	sp.streetProposal.edgesToRemove[1] = eid
	for k, st in ipairs(stops) do
		local eo = api.type.SimpleStreetProposal.EdgeObject.new()
		eo.edgeEntity = -1
		eo.param = st.u
		eo.oneWay = st.oneWay and true or false
		eo.left = st.left and true or false
		eo.model = st.model
		eo.playerEntity = api.engine.util.getPlayer()
		eo.name = st.name or ""
		sp.streetProposal.edgeObjectsToAdd[k] = eo
	end
	-- Every stop on this edge comes back with a new entity id; the poller must
	-- not read that as remove + add. Announce each position both ways.
	for _, st in ipairs(stops) do
		if st.x then
			CM.expectStop[stopKey(st.x, st.y)] = true
			CM.expectStopDel[stopKey(st.x, st.y)] = true
		end
	end
	local cmd = api.cmd.make.buildProposal(sp, buildContext(), true)
	api.cmd.sendCommand(cmd, function(res, success)
		local msg = ""
		if not success then
			pcall(function()
				local es = res.resultProposalData and res.resultProposalData.errorState
				if es then
					msg = " critical=" .. tostring(es.critical)
					for i = 1, #es.messages do msg = msg .. " '" .. tostring(es.messages[i]) .. "'" end
				end
			end)
			for _, st in ipairs(stops) do
				if st.x then CM.expectStop[stopKey(st.x, st.y)] = nil; CM.expectStopDel[stopKey(st.x, st.y)] = nil end
			end
		end
		log(string.format("EXEC %s: %s edge %d rebuilt with %d object(s) success=%s%s", why, isTrack and "track" or "street", eid, #stops, tostring(success), msg))
		if onDone then onDone(success) end
	end)
	return true
end

-- The stops already on an edge, described relative to that edge.
local function stopsOnEdge(eid)
	local list = {}
	local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
	for eo, e2 in pairs(m) do
		if e2 == eid and isPlayerStop(eo) then
			local d = describeStop(eo, eid)
			if d then list[#list + 1] = { eo = eo, u = d.u, left = d.left, model = d.model, name = d.name, x = d.x, y = d.y, kind = d.kind, oneWay = d.oneWay } end
		end
	end
	return list
end

function CM.execStopAdd(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		if c.kind == nil then
			log(string.format("STOPADD seq=%s: no kind on the wire (older peer build) -- skipped rather than guessed", tostring(c.seq)))
			return
		end
		local wantTrack = tonumber(c.track) == 1
		local eid = CM.findEdgeByEnds(wantTrack, c.ax, c.ay, c.bx, c.by, 2.0)
		if not eid then
			log(string.format("STOPADD seq=%s: no %s edge %.1f,%.1f--%.1f,%.1f here -- skipped",
				tostring(c.seq), wantTrack and "track" or "street", c.ax, c.ay, c.bx, c.by))
			return
		end
		-- The wire's u and side are relative to the originator's node0 -> node1.
		-- Ours may run the other way.
		local comp, a = edgeGeomT(eid)
		local u, left = tonumber(c.u) or 0.5, tonumber(c.left) == 1
		local da = (a[1] - c.ax) ^ 2 + (a[2] - c.ay) ^ 2
		local db = (a[1] - c.bx) ^ 2 + (a[2] - c.by) ^ 2
		if db < da then u = 1 - u; left = not left end
		local stops = stopsOnEdge(eid)
		for _, st in ipairs(stops) do
			if (st.x - c.x) ^ 2 + (st.y - c.y) ^ 2 < 1.0 then
				log(string.format("STOPADD seq=%s: a stop already stands at %.1f,%.1f -- nothing to do", tostring(c.seq), c.x, c.y))
				return
			end
		end
		-- TWO OBJECTS ON ONE EDGE is not yet understood. With the kinds right the
		-- engine still asserts in StreetGeometry::CreateLanes
		-- (edgeObjects[0].second == -1) on a second object, and one form of that
		-- assert was fatal. Refuse loudly until the rule is read out of the
		-- decompile: a stop the peer lacks is a visible c-lane difference, a
		-- crashed game is not.
		if #stops > 0 then
			log(string.format("STOPADD seq=%s: edge %d already carries %d object(s) -- a second is NOT supported yet, skipped (DIVERGENCE)",
				tostring(c.seq), eid, #stops))
			return
		end
		stops[#stops + 1] = { u = u, left = left, model = unescName(c.model), name = unescName(c.name), x = c.x, y = c.y,
			kind = tonumber(c.kind) or 1, oneWay = tonumber(c.oneWay) == 1 }
		local ok2, why = rebuildEdgeWithStops(eid, stops, string.format("STOPADD seq=%s origin=%s '%s'",
			tostring(c.seq), tostring(c.origin), unescName(c.name)))
		if not ok2 then log(string.format("STOPADD seq=%s: %s -- skipped", tostring(c.seq), tostring(why))) end
	end)
	if not ok then log("exec STOPADD error: " .. tostring(err)) end
end

function CM.execStopDel(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local ok, err = pcall(function()
		local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
		local best, bestD
		for eo, eid in pairs(m) do
			if isPlayerStop(eo) then
				local d = describeStop(eo, eid)
				if d then
					local dd = (d.x - c.x) ^ 2 + (d.y - c.y) ^ 2
					if dd < 4 and (not bestD or dd < bestD) then best, bestD = eo, dd end
				end
			end
		end
		if not best then
			log(string.format("STOPDEL seq=%s: no roadside stop within 2 m of %.1f,%.1f -- skipped",
				tostring(c.seq), c.x, c.y))
			return
		end
		local eid = m[best]
		local keep = {}
		for _, st in ipairs(stopsOnEdge(eid)) do
			if st.eo ~= best then keep[#keep + 1] = st end
		end
		CM.expectStopDel[stopKey(c.x, c.y)] = true
		local ok2, why = rebuildEdgeWithStops(eid, keep, string.format("STOPDEL seq=%s origin=%s", tostring(c.seq), tostring(c.origin)),
			function(success) if not success then CM.expectStopDel[stopKey(c.x, c.y)] = nil end end)
		if not ok2 then
			CM.expectStopDel[stopKey(c.x, c.y)] = nil
			log(string.format("STOPDEL seq=%s: %s -- skipped", tostring(c.seq), tostring(why)))
		end
	end)
	if not ok then log("exec STOPDEL error: " .. tostring(err)) end
end

local function pollNewConstructions()
	local ok, err = pcall(function()
		local list = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		if not consPrimed then
			-- Existing constructions get classified a few per tick, so a station
			-- that was in the save can still have its EDITS replicated, without
			-- thousands of component reads in one frame.
			for _, id in pairs(list) do knownCons[id] = true; primeQueue[#primeQueue + 1] = id end
			consPrimed = true
			return
		end
		for _, id in pairs(list) do
			if not knownCons[id] then
				-- A player-buildable construction (station/depot/...) can appear a
				-- tick or two BEFORE its PLAYER_OWNED component is assigned --
				-- notably when it is placed OVER buildings, whose demolish delays
				-- ownership. Marking it known immediately would classify it as a
				-- town building forever (the station-over-buildings replication
				-- bug). So for a buildable file with ownership still nil, DON'T
				-- mark it known -- retry (bounded) until owned.
				local co0 = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				local fn0 = co0 and co0.fileName and tostring(co0.fileName) or ""
				local buildable = false
				for _, p in ipairs({ "station/", "depot/", "asset/", "airport/", "harbor/", "harbour/" }) do
					if fn0:sub(1, #p) == p then buildable = true; break end
				end
				local ownedComp = nil
				pcall(function() ownedComp = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED) end)
				local waitOwnership = false
				if buildable and ownedComp == nil then
					ownershipPending[id] = (ownershipPending[id] or 0) + 1
					waitOwnership = ownershipPending[id] < 30   -- retry next poll (bounded)
				end
				if buildable then
					log(string.format("con DEBUG: buildable id=%d file=%s owned=%s isPlayer=%s wait=%s",
						id, fn0, tostring(ownedComp ~= nil),
						tostring(isPlayerConstruction(id, fn0)), tostring(waitOwnership)))
				end
				if not waitOwnership then
				ownershipPending[id] = nil
				knownCons[id] = true
				local alive = false
				pcall(function() alive = api.engine.entityExists(id) end)
				if alive then
					local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
					if co and co.fileName and co.transf and isPlayerConstruction(id, tostring(co.fileName)) then
						local fn = tostring(co.fileName)
						local key = conKey(co.transf[13], co.transf[14])
						local e = game.interface.getEntity(id)
						local pstr = (e and e.params) and ser(e.params) or "{}"
						if expectedCons[key] then
							expectedCons[key] = nil
							noteCon(id, fn, key, pstr)
							log(string.format("con: replayed %s landed as id %d", fn, id))
							-- companies mode: the replay landed owned by our local player;
							-- hand it to the ORIGIN company and lock it. Done here (the poll),
							-- not in a build callback, because every replay path -- buildProposal
							-- AND the buildConstruction fallback -- funnels through this branch.
							local ocid = CM.cmExpectedCompany[key]
							if ocid then
								CM.cmExpectedCompany[key] = nil
								pcall(function() CM.cmReassignConstruction(id, ocid) end)
								-- R2: the build was charged to OUR wallet; the balance
								-- drop since apply is the exact cost -> move it to co<ocid>.
								local bal0 = CM.cmExpectedBal0[key]; CM.cmExpectedBal0[key] = nil
								local nowBal = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
								if bal0 and nowBal then pcall(function() CM.cmTransferCost(ocid, bal0 - nowBal, "CON " .. fn) end) end
							end
						elseif expectedEdit[key] then
							expectedEdit[key] = nil
							noteCon(id, fn, key, pstr)
							log(string.format("con: replayed edit landed as id %d", id))
						elseif consByKey[key] and consByKey[key].file == fn then
							-- Same spot, new id: the entity was REPLACED, which is
							-- what an upgrade does. An edit, not a build.
							log(string.format("con DEBUG: %s id=%d treated as EDIT (consByKey had this spot)", fn, id))
							local prev = noteCon(id, fn, key, pstr)
							if prev.params ~= pstr then shipEdit(fn, key, pstr) end
						else
							noteCon(id, fn, key, pstr)
							queueConCapture(fn, key, pstr, co.transf, id)
							log(string.format("con: captured %s id=%d params=%dB -- pairing", fn, id, #pstr))
						end
					end
				end
				end   -- close: if not waitOwnership
			end
		end
	end)
	if not ok then log("con poll error: " .. tostring(err)) end
end

-- Classify a slice of the primed ids each tick.
local function primeConstructions()
	if #primeQueue == 0 then return end
	local ok, err = pcall(function()
		local n = 0
		while #primeQueue > 0 and n < K.PRIME_PER_TICK do
			local id = table.remove(primeQueue)
			n = n + 1
			local alive = false
			pcall(function() alive = api.engine.entityExists(id) end)
			if alive then
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				if co and co.fileName and co.transf and isPlayerConstruction(id, tostring(co.fileName)) then
					local e = game.interface.getEntity(id)
					noteCon(id, tostring(co.fileName), conKey(co.transf[13], co.transf[14]),
						(e and e.params) and ser(e.params) or "{}")
				end
			end
		end
		if #primeQueue == 0 then
			local c = 0; for _ in pairs(consByKey) do c = c + 1 end
			log("con: primed " .. c .. " player construction(s) for edit tracking")
		end
	end)
	if not ok then log("con prime error: " .. tostring(err)) end
end

-- In-place edits: params changed on an id we already know. Only player
-- constructions are in consByKey, so this walks a handful of entities.
-- conKey -> tick first seen dead. A construction that is gone for two
-- consecutive scans (and not replaced at the same spot) was DEMOLISHED; a
-- one-scan gap is the window of an UPGRADE (old entity removed, new one about
-- to appear), which pollNewConstructions handles as an edit.
-- Fast demolish detector. A tracked construction that is gone is either
-- DEMOLISHED or UPGRADED (removed and re-created at the same spot). Instead of
-- waiting out a long debounce, ASK: is there still a construction at that
-- position? If yes it was an upgrade (pollNewConstructions re-adopts it as an
-- edit) -- skip. If no, it is a demolish -- but require TWO consecutive misses
-- a few ticks apart so a one-frame remove/re-add gap is not read as a demolish.
local demolishMiss = {}   -- conKey -> consecutive scans seen gone-with-nothing-there
K.REMOVAL_POLL_EVERY = 3

-- Is a PLAYER construction still standing here? Used to tell an upgrade (the
-- old entity vanishes, a new one appears in its place) from a demolish (nothing
-- replaces it).
--
-- PLAYER-owned only, and that is the whole point. Town buildings are
-- CONSTRUCTION entities too, and a truck station sits in a town surrounded by
-- them -- so with a bare type filter, demolishing one found a house within 6 m,
-- called it an upgrade, and never shipped the DEMOLISH. The peer kept the
-- station forever. Measured 2026-08-31 in a live game: the host's world had 8
-- player constructions to the joiner's 7, seq 1..51 arrived from the joiner with
-- no gaps and not one DEMOLISH among them -- nothing was lost in flight, the
-- detector simply never fired.
function CM.constructionAt(x, y)
	local found
	pcall(function()
		local list = game.interface.getEntities({ pos = { x, y }, radius = 6 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, id in pairs(list) do
			if not found then
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				local fn = co and co.fileName and tostring(co.fileName) or ""
				if isPlayerConstruction(id, fn) then found = id end
			end
		end
	end)
	return found
end

local function pollConstructionRemovals()
	local ok, err = pcall(function()
		for key, rec in pairs(consByKey) do
			local alive = false
			pcall(function() alive = api.engine.entityExists(rec.id) end)
			if alive then
				demolishMiss[key] = nil
			else
				local kx, ky = tostring(key):match("^(%-?[%d%.]+)/(%-?[%d%.]+)$")
				local x, y = tonumber(kx), tonumber(ky)
				if x and CM.constructionAt(x, y) then
					-- something is still there: an upgrade replacement; let
					-- pollNewConstructions re-adopt it. Not a demolish.
					demolishMiss[key] = nil
				else
					demolishMiss[key] = (demolishMiss[key] or 0) + 1
					if demolishMiss[key] >= 2 then
						demolishMiss[key] = nil
						if expectedEdit[key] or expectedCons[key] then
							-- upgrade/replay in flight -- but a STALE flag here
							-- silently suppresses a real demolish forever
							-- (suspected one-off 2026-08-29: bulldoze logged by
							-- the hook, mod said nothing). Tripwire it.
							log(string.format("con: %s is gone but edit/replay flags block the demolish (edit=%s cons=%s) -- will re-check",
								key, tostring(expectedEdit[key] ~= nil), tostring(expectedCons[key] ~= nil)))
						elseif expectedDemolish[key] then
							expectedDemolish[key] = nil
							consByKey[key] = nil
							log(string.format("con: %s bulldozed by replay -- not echoed", key))
						else
							consByKey[key] = nil
							if x and y then
								CM.rearmSplitsNear(x, y)
								scheduleLocal("DEMOLISH", { x = x, y = y })
								log(string.format("con: DEMOLISH captured at %.1f,%.1f (%s)", x, y, tostring(rec.file)))
							else
								log("con: a construction vanished but its position is unknown (key=" .. tostring(key) .. ")")
							end
						end
					end
				end
			end
		end
	end)
	if not ok then log("con removal poll error: " .. tostring(err)) end
end

local function scanConstructionEdits()
	local ok, err = pcall(function()
		for key, rec in pairs(consByKey) do
			local alive = false
			pcall(function() alive = api.engine.entityExists(rec.id) end)
			if alive then
				local e = game.interface.getEntity(rec.id)
				local pstr = (e and e.params) and ser(e.params) or "{}"
				if pstr ~= rec.params then
					rec.params = pstr
					if expectedEdit[key] then
						expectedEdit[key] = nil      -- our own replay changed it in place
					else
						shipEdit(rec.file, key, pstr)
					end
				end
			end
		end
	end)
	if not ok then log("con edit scan error: " .. tostring(err)) end
end

-- ---------- BUYTEST: live vehicle-identity readback (STEP 5) ----------
--
-- Three questions a factory-only sweep cannot answer, all needed before vehicle
-- replication can pick a cross-peer identity scheme:
--   1. does the sendCommand callback hand back the new vehicle's entity id?
--   2. does purchaseTime survive apply, or does the engine restamp it?
--   3. is getDepotVehicles order stable (usable as an ordinal key)?
-- This DOES touch the world (a real vehicle is bought, real money spent) -- it
-- is a one-shot diagnostic, not a sweep. Runs on whichever instance injects it.
local buytestPending = nil    -- { depot=, want=, at= } awaiting readback

local function findDepotForKind(kind)
	-- kind: "train" -> train_depot, "road" -> road_depot. First matching depot.
	local want = (kind == "train") and "train_depot" or "road_depot"
	local found
	pcall(function()
		local list = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		for _, id in pairs(list) do
			local alive = false
			pcall(function() alive = api.engine.entityExists(id) end)
			if alive and not found then
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				if co and co.fileName and tostring(co.fileName):find(want, 1, true) then
					found = id
				end
			end
		end
	end)
	return found
end

local function vehiclePurchaseTime(vid)
	local pt
	pcall(function()
		local tv = api.engine.getComponent(vid, api.type.ComponentType.TRANSPORT_VEHICLE)
		-- purchaseTime lives per-part; read the first part's
		if tv and tv.transportVehicleConfig and tv.transportVehicleConfig.vehicles then
			local v0 = tv.transportVehicleConfig.vehicles[1]
			if v0 then pt = v0.purchaseTime end
		end
	end)
	return pt
end

local function depotVehicleOrder(depot)
	local ids = {}
	pcall(function()
		local vs = game.interface.getDepotVehicles(depot)
		if type(vs) == "table" then for _, v in ipairs(vs) do ids[#ids + 1] = v end end
	end)
	return ids
end

local function runBuyTest()
	local mid, nComp, merr, kind = gtVehPickModel()
	if not mid then log("BUYTEST: " .. tostring(merr)); return end
	-- Match the depot to the model KIND (train/road). The numeric carrier enum
	-- must NEVER be guessed here: buying a train into a road depot is a native
	-- assert that pcall cannot catch and wedges the sim thread (measured).
	kind = kind or "road"
	local depot = findDepotForKind(kind)
	if not depot then log("BUYTEST: no " .. kind .. " depot found in save"); return end
	-- Re-confirm the match right before buying. A native type mismatch here is an
	-- uncatchable assert, so refuse rather than risk it.
	local okType = false
	pcall(function()
		local co = api.engine.getComponent(depot, api.type.ComponentType.CONSTRUCTION)
		local fn = co and co.fileName and tostring(co.fileName) or ""
		okType = fn:find((kind == "train") and "train_depot" or "road_depot", 1, true) ~= nil
	end)
	if not okType then log("BUYTEST: depot " .. depot .. " does not match kind " .. kind .. " -- refusing"); return end
	log(string.format("BUYTEST: model=%d comp=%d kind=%s depot=%d", mid, nComp, kind, depot))

	local steps = {}
	local function step(name, fn) local ok, e = pcall(fn); steps[#steps + 1] = name .. (ok and "" or (" FAIL:" .. tostring(e))); return ok end
	local KNOWN_PT = 777000001         -- a sentinel purchaseTime we can recognise on readback
	local config = gtVehConfig(step, mid, nComp, 1, { purchaseTime = KNOWN_PT })
	if not config then log("BUYTEST: config build failed: " .. table.concat(steps, " | ")); return end

	local before = depotVehicleOrder(depot)
	log("BUYTEST: depot had " .. #before .. " vehicles before")

	local pid = api.engine.util.getPlayer()
	local ok = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buyVehicle(pid, depot, config), function(res, success)
			-- Log EVERY plausible result field; we do not know which the binding exposes.
			local rv, re, ve, tp = "nil", "nil", "nil", "nil"
			pcall(function() if res then rv = tostring(res.resultVehicleEntity) end end)
			pcall(function() if res then re = tostring(res.resultEntity) end end)
			pcall(function() if res then ve = tostring(res.vehicleEntity) end end)
			pcall(function() if res then tp = tostring(res.type) end end)
			log(string.format("BUYTEST CALLBACK success=%s resultVehicleEntity=%s resultEntity=%s vehicleEntity=%s type=%s",
				tostring(success), rv, re, ve, tp))
			-- Arm a readback a few stamps later (the vehicle needs to exist).
			local now = gameTime()
			buytestPending = { depot = depot, before = before, want = KNOWN_PT,
			                   at = (now and now + 3) or nil, res_rv = rv }
		end)
	end)
	if not ok then log("BUYTEST: sendCommand threw"); return end
	log("BUYTEST: buy issued (steps: " .. #steps .. " ok)")
end

-- Called each tick from the main loop; fires the readback once the vehicle exists.
local function buytestPoll()
	if not buytestPending then return end
	local now = gameTime()
	if not now or (buytestPending.at and now < buytestPending.at) then return end
	local p = buytestPending
	buytestPending = nil
	local after = depotVehicleOrder(p.depot)
	log(string.format("BUYTEST READBACK depot vehicles: before=%d after=%d", #p.before, #after))
	-- The new vehicle is the id in `after` not in `before`.
	local beforeSet = {}
	for _, v in ipairs(p.before) do beforeSet[v] = true end
	local newId
	for _, v in ipairs(after) do if not beforeSet[v] then newId = v; break end end
	log("BUYTEST READBACK newVehicle(by diff)=" .. tostring(newId) ..
		"  callback_resultVehicleEntity=" .. tostring(p.res_rv))
	if newId then
		local pt = vehiclePurchaseTime(newId)
		log(string.format("BUYTEST READBACK purchaseTime: set=%d read=%s -> %s",
			p.want, tostring(pt),
			(pt == p.want) and "SURVIVES (usable key)" or "RESTAMPED (not a key)"))
	end
	-- Depot order: print both so a cross-peer comparison can be eyeballed.
	local ord = {}
	for _, v in ipairs(after) do ord[#ord + 1] = tostring(v) end
	log("BUYTEST READBACK depot order = [" .. table.concat(ord, ",") .. "]")
end

-- SOLO IS SOLO. The slice leaves a build alone when no peer is playing (see
-- SessionLive in slice_hook.cpp), so the engine has already built it -- replaying
-- it here would build it a second time. Reading the file and dropping the lines
-- keeps the offset moving, so nothing is replayed later when somebody does join.
--
-- Between them, the two halves mean an installed copy of this mod changes
-- nothing at all in a single-player game: the build is not cancelled and not
-- replayed, and the player gets stock behaviour.
CM.warnedSolo = false
function CM.soloDrop(line)
	if not CM.warnedSolo then
		CM.warnedSolo = true
		log("inject: no peer in this session -- captures dropped ("
			.. tostring(line):sub(1, 40) .. "...); single player is left to the engine")
	end
end

local function pollInject()
	if not K.INJECT_FILE then return end
	local data, newOff = readFrom(K.INJECT_FILE, injectOffset)
	injectOffset = newOff
	if not data then return end

	for line in data:gmatch("[^\r\n]+") do
		line = line:gsub("^%s+", ""):gsub("%s+$", "")
		if line ~= "" and line:sub(1, 1) ~= "#" then
			local w = {}
			for tok in line:gmatch("%S+") do w[#w + 1] = tok end
			local o = w[1]
			-- Same protection pollEvents has had all along: one malformed line
			-- (or one bug in a parser branch) must cost that line, not the tick.
			local okLine, errLine = pcall(function()
			-- Diagnostics (EVAL, HEAL, BUYTEST) always run; a CAPTURE is dropped
			-- when nobody is playing with us, because the engine already built it.
			-- Inside the per-line pcall on purpose: a `return` here skips THIS
			-- line. Outside it, the first dropped capture abandoned every line
			-- after it in the same read -- a diagnostic queued behind a build
			-- never ran (review, 2026-08-31).
			-- The slice says, per capture, whether it cancelled the local build.
			if o == "ARMED" then CM.lastArmed = tonumber(w[2]) or 0; return end
			-- A capture whose local build was CANCELLED must always be replayed,
			-- peer or no peer -- dropping it deletes the player's own work.
			if not peerSeen and (CM.lastArmed or 0) == 0
			   and o ~= "EVAL" and o ~= "HEAL" and o ~= "BUYTEST" then
				CM.soloDrop(line)
				return
			end

			-- ROADN n x0 y0 x1 y1 ...   (written by slice_hook from a captured
			-- player build; carries every tessellated node)
			if o == "HEAL" then
				-- Manual repair: rejoin a road at x,y if a scar from a replayed
				-- split is all that is left there. Same rules as the sweep.
				CM.healNodeAt(tonumber(w[2]) or 0, tonumber(w[3]) or 0, "manual")

			elseif o == "EVAL" then
				-- Diagnostic probe: run a chunk from the inject file, log the
				-- result. Exists so questions like "is the frozen mouth node in
				-- node2StreetEdgeMap on B?" cost one file append, not a rebuild
				-- cycle. loadstring may be sandboxed away; fail loudly then.
				local chunk = line:sub(6)
				local fn, cerr = (loadstring or load)(chunk)
				if fn then
					local okE, res = pcall(fn)
					log("EVAL -> " .. tostring(res) .. (okE and "" or " (ERROR)"))
				else
					log("EVAL compile: " .. tostring(cerr))
				end

			elseif o == "BUYTEST" then
				runBuyTest()

			-- GT <track|street>  -- ground-truth sweep, non-destructive
			elseif o == "GT" and #w >= 2 then
				runGroundTruth(w[2])

			-- ROADE <N> <etype> <stype> <ttype> <cat> <M> <rn> <re>
			--       <id x y z>*N <a1 a2 t0x t0y t0z t1x t1y t1z>*M
			--       <rmnodeid>*rn <a1 a2 t0x t0y t0z t1x t1y t1z>*re
			--       [<btype bidx>*M]
			-- Carries real edge topology, so a road CONNECTING to existing
			-- infrastructure replicates. Negative endpoints are the proposal's
			-- own placeholders; positive ones are real entities in the world.
			-- Removed EDGES are full 8-token records now (same shape ROADC uses),
			-- which is what makes the UPGRADE tool (caller 4790fc: N=0 added nodes,
			-- M added edges, M removed edges, every endpoint an existing node)
			-- replicable at all -- see the rm list built below.
			--
			-- Converted here into a purely POSITIONAL command. This runs on the
			-- originating peer, which still has every entity the capture refers
			-- to, so a real node id can be turned into coordinates now -- and the
			-- peer never has to trust that ids match, which nothing verified.
			--
			-- Split halves are DROPPED. When a road lands mid-span the game emits
			-- both halves of the edge it cut, but the receiving peer regenerates
			-- them by splitting its own copy; replaying the captured halves too
			-- would duplicate them. A half is identified by shape: its new-node
			-- endpoint is shared with another positive-endpoint edge.
			elseif o == "ROADE" and #w >= 9 then
				local n     = tonumber(w[2]) or 0
				local etype = tonumber(w[3]) or 0
				local stype = tonumber(w[4]) or 16
				local ttype = tonumber(w[5]) or 1
				local cat   = tonumber(w[6]) or 0
				local m     = tonumber(w[7]) or 0
				local rn    = tonumber(w[8]) or 0
				local re    = tonumber(w[9]) or 0
				-- n may be 0: an UPGRADE adds no nodes at all (it replaces edges
				-- between nodes that already exist). Requiring n >= 1 is what made
				-- an upgrade look like a malformed line.
				-- rn / re are also floors, not just lengths: a negative count would
				-- SHRINK the required width and then walk the bridge/tunnel tail off
				-- into the removal records.
				local ok = (n >= 0 and m >= 1 and rn >= 0 and re >= 0
				            and #w >= 9 + n * 4 + m * 8 + rn + re * 8)

				local posOf, order = {}, {}
				if ok then
					for i = 1, n do
						local b = 9 + (i - 1) * 4
						local id, x, y, z = tonumber(w[b + 1]), tonumber(w[b + 2]), tonumber(w[b + 3]), tonumber(w[b + 4])
						if not (id and x and y and z) then ok = false; break end
						-- z from the CAPTURE, not the terrain: bridges and embankments
						-- are not at ground level.
						posOf[id] = { x, y, z }
						order[#order + 1] = id
					end
				end

				local raw = {}
				if ok then
					local b = 9 + n * 4
					for i = 1, m do
						local o = b + (i - 1) * 8
						local a1, a2 = tonumber(w[o + 1]), tonumber(w[o + 2])
						if not (a1 and a2) then ok = false; break end
						local t = {}
						for k = 1, 6 do
							t[k] = tonumber(w[o + 2 + k])
							if not t[k] then ok = false; break end
						end
						if not ok then break end
						raw[#raw + 1] = { a1, a2, t, 0, -1 }
					end
				end
				-- Removed EDGES, 8-token records like the added ones. Only the two
				-- endpoint ids are used (the removal is named by POSITION on the
				-- wire); the tangents are consumed to keep the offsets right.
				local rmv = {}
				if ok then
					local b = 9 + n * 4 + m * 8 + rn
					for i = 1, re do
						local o = b + (i - 1) * 8
						local a1, a2 = tonumber(w[o + 1]), tonumber(w[o + 2])
						if not (a1 and a2) then ok = false; break end
						rmv[#rmv + 1] = { a1, a2 }
					end
				end
				-- Bridge/tunnel tail: <type idx> per added edge, appended AFTER the
				-- legacy payload (old captures simply lack it -> ground).
				if ok then
					local tb = 9 + n * 4 + m * 8 + rn + re * 8
					if #w >= tb + m * 2 then
						for i = 1, m do
							raw[i][4] = tonumber(w[tb + (i - 1) * 2 + 1]) or 0
							raw[i][5] = tonumber(w[tb + (i - 1) * 2 + 2]) or -1
						end
					end
				end

				if ok then
					-- Which new nodes are SPLIT points, vs bridge midpoints?
					--
					-- The old test -- "a new node with >=2 positive-endpoint edges"
					-- -- was wrong. A road that BRIDGES two existing road ends
					-- through a new midpoint gives that midpoint two
					-- positive-endpoint edges too, with NO split, so both edges
					-- were dropped as "halves" and the road vanished (the triangle
					-- closing edge failed exactly this way).
					--
					-- The real discriminator is GEOMETRY: a split point lies ON an
					-- existing edge; a bridge midpoint sits in open space. The
					-- originator has cancelled its build, so the original edges are
					-- intact in its world -- findEdgeContaining answers directly.
					local isTrack = (etype == 1)
					local splitNode = {}   -- new node id -> { node0, node1 } of the edge it sits on
					for id, xyz in pairs(posOf) do
						local hitEid
						-- EITHER kind: a rail vertex landing on a ROAD is a split point too
						-- (level crossing). Same-kind only let the road's halves through as
						-- rail edges -> duplicated, track-typed road halves in the proposal
						-- -> "Construction not possible" (proposal dump 2026-08-29).
						pcall(function() hitEid = findEdgeContaining(isTrack, xyz[1], xyz[2]) end)
						if not hitEid then pcall(function() hitEid = findEdgeContaining(not isTrack, xyz[1], xyz[2]) end) end
						if hitEid then
							local ends = { -1, -1 }
							pcall(function()
								local be = api.engine.getComponent(hitEid, api.type.ComponentType.BASE_EDGE)
								if be then ends = { be.node0, be.node1 } end
							end)
							splitNode[id] = ends
						end
					end

					-- Resolve a real entity id to a position, on this peer, now.
					local function realPos(id)
						local p
						pcall(function()
							local nc = api.engine.getComponent(id, api.type.ComponentType.BASE_NODE)
							if nc and nc.position then
								p = { nc.position.x or nc.position[1],
								      nc.position.y or nc.position[2],
								      nc.position.z or nc.position[3] }
							end
						end)
						return p
					end

					local pts, links, tans, index, bts = {}, {}, {}, {}, {}
					local function pointFor(key, xyz)
						if index[key] then return index[key] end
						pts[#pts + 1] = string.format("%.4f", xyz[1])
						pts[#pts + 1] = string.format("%.4f", xyz[2])
						pts[#pts + 1] = string.format("%.4f", xyz[3])
						index[key] = #pts / 3
						return index[key]
					end

					local dropped = 0
					for _, e in ipairs(raw) do
						local a1, a2 = e[1], e[2]
						-- A split half is an edge from an existing node to a new
						-- node that sits on an existing edge; the peer regenerates
						-- it by splitting its own copy. A bridge edge touches a new
						-- node in open space and must be kept.
						-- A half runs from the split node to one of the ENDPOINTS of the
						-- edge it splits. Any other existing->new edge is a CONNECTOR: a
						-- track MERGING from an existing node onto a bridge mid-span
						-- (2026-08-29: '281946 -> -1' plus the two halves) was dropped as
						-- a third half -> "no usable edges" -> nothing built anywhere.
						local function isHalfOf(ex, nw)
							local ends = splitNode[nw]
							return ends ~= nil and (ex == ends[1] or ex == ends[2])
						end
						local isHalf = (a1 >= 0 and a2 < 0 and isHalfOf(a1, a2))
						                or (a2 >= 0 and a1 < 0 and isHalfOf(a2, a1))
						if isHalf then
							dropped = dropped + 1
						else
							local p1 = (a1 < 0) and posOf[a1] or realPos(a1)
							local p2 = (a2 < 0) and posOf[a2] or realPos(a2)
							if p1 and p2 then
								local i1 = pointFor(a1, p1)
								local i2 = pointFor(a2, p2)
								if i1 ~= i2 then
									links[#links + 1] = tostring(i1)
									links[#links + 1] = tostring(i2)
									-- the captured tangents, so curves stay curves
									for k = 1, 6 do
										tans[#tans + 1] = string.format("%.4f", e[3][k])
									end
									bts[#bts + 1] = tostring(e[4] or 0)
									bts[#bts + 1] = tostring(e[5] or -1)
								end
							end
						end
					end

					-- ---------- removals -> positional rm list ----------
					--
					-- Only removals the peer CANNOT regenerate travel. A removal
					-- whose two endpoints are the ends of the edge some new node
					-- sits on is a SPLIT PARENT: execPolyline splits its own copy of
					-- that edge and removes it there, so shipping the removal too
					-- would remove one entity twice and the engine rejects the whole
					-- proposal. What is left is the UPGRADE case -- an edge replaced
					-- in place between two existing nodes, invisible to any
					-- geometric test the peer could run.
					--
					-- The build was CANCELLED here, so every id in the capture still
					-- resolves; positions are read now and ids never leave.
					local rmpos, rmbad, rmskip = {}, nil, 0
					for _, r in ipairs(rmv) do
						local isSplitParent = false
						for _, ends in pairs(splitNode) do
							if (ends[1] == r[1] and ends[2] == r[2])
							   or (ends[1] == r[2] and ends[2] == r[1]) then
								isSplitParent = true
								break
							end
						end
						if isSplitParent then
							rmskip = rmskip + 1
						else
							local q1 = (r[1] < 0) and posOf[r[1]] or realPos(r[1])
							local q2 = (r[2] < 0) and posOf[r[2]] or realPos(r[2])
							if q1 and q2 then
								rmpos[#rmpos + 1] = string.format("%.4f,%.4f,%.4f,%.4f",
									q1[1], q1[2], q2[1], q2[2])
							else
								rmbad = string.format("removed edge %d->%d has no resolvable "
									.. "endpoint position on this instance", r[1], r[2])
								break
							end
						end
					end

					if rmbad then
						-- Ship nothing. An upgrade whose removal is missing replays as
						-- a pure ADD on the peer: a second edge between the same two
						-- nodes, permanently diverged.
						log("ROADE: " .. rmbad .. " -- command NOT replicated")
					elseif #links >= 2 then
						if dropped > 0 then
							log(string.format("ROADE: dropped %d split half/halves " ..
								"-- the peer regenerates them locally", dropped))
						end
						if #rmpos > 0 or rmskip > 0 then
							log(string.format("ROADE: %d removal(s) shipped as positions, "
								.. "%d left to the peer's own split", #rmpos, rmskip))
						end
						local sargs = { pts = table.concat(pts, ","),
						                links = table.concat(links, ","),
						                tans = table.concat(tans, ","),
						                bt = table.concat(bts, ","),
						                etype = etype, stype = stype, ttype = ttype,
						                cat = cat }
						-- omitted entirely when there is nothing to remove: an empty
						-- 'rm=' token would not survive decodeCmd's key=value scan
						if #rmpos > 0 then sargs.rm = table.concat(rmpos, ";") end
						-- Decide HERE, once, and put the decisions on the wire.
						-- This runs the real replay in plan-only mode: same
						-- resolution, same splits, nothing built. Both instances
						-- then execute the originator's answer at the stamp instead
						-- of each re-deriving one against its own world.
						local okPlan, xv, xh = pcall(function()
							return execPolyline({ pts = sargs.pts, links = sargs.links,
								tans = sargs.tans, bt = sargs.bt, etype = sargs.etype,
								stype = sargs.stype, ttype = sargs.ttype, cat = sargs.cat,
								rm = sargs.rm, seq = "plan" }, true)
						end)
						if okPlan then
							-- pcall folds multiple returns; re-run shape: xv is the
							-- first value, xh the second (nil when there is nothing).
							if xv then sargs.xv = xv end
							if xh then sargs.xh = xh end
							local function entries(str)
								local n = 0
								for _ in tostring(str or ""):gmatch("[^;]+") do n = n + 1 end
								return n
							end
							log(string.format("ROADP plan: %d vertex decision(s), %d crossing decision(s) shipped",
								entries(xv), entries(xh)))
						else
							log("ROADP plan pass failed (" .. tostring(xv) .. ") -- peers will derive their own")
						end
						-- Not cancelled here (no live session at the time): the engine
						-- built it natively, so this instance must not replay it.
						if (CM.lastArmed or 1) == 0 then sargs.skipOrigin = 1 end
						scheduleLocal("ROADP", sargs)
					else
						log("inject: ROADE produced no usable edges: " .. line:sub(1, 70))
					end
				else
					log("inject: bad ROADE line: " .. line:sub(1, 70))
				end
			elseif o == "ROADC" and #w >= 8 then
				-- Street companion of a construction placement (hook caller 419f62).
				-- Classification is ID-ANCHORED, never world-resolved: by the time
				-- this line is read the placement has APPLIED, and the apply
				-- recycles node ids (measured: removed-edge endpoint 217763 was
				-- already dead at conversion time) -- so any test that resolves a
				-- captured id against the live world silently misclassifies.
				--   split point -- a new node with added edges to BOTH endpoints of
				--                  one removed edge; those two edges are HALVES.
				--                  Peer regenerates the split, so drop them and ship
				--                  the split position as a WELD instead.
				--   frozen stub -- both endpoints new, neither a split point;
				--                  buildConstruction creates it on the peer. Drop.
				--   connector   -- everything else: mouth-to-street. Ship.
				local n     = tonumber(w[2]) or 0
				local etype = tonumber(w[3]) or 0
				local stype = tonumber(w[4]) or 16
				local ttype = tonumber(w[5]) or 1
				local cat   = tonumber(w[6]) or 0
				local m     = tonumber(w[7]) or 0
				local re    = tonumber(w[8]) or 0
				local ok = (m >= 1 and #w >= 8 + n * 4 + m * 8 + re * 8)
				local posOf = {}
				if ok then
					for i = 1, n do
						local b = 8 + (i - 1) * 4
						local id, x, y, z = tonumber(w[b + 1]), tonumber(w[b + 2]),
						                    tonumber(w[b + 3]), tonumber(w[b + 4])
						if not (id and x and y and z) then ok = false; break end
						posOf[id] = { x, y, z }
					end
				end
				local function rec8(base, i)
					local o8 = base + (i - 1) * 8
					local a1, a2 = tonumber(w[o8 + 1]), tonumber(w[o8 + 2])
					if not (a1 and a2) then return nil end
					local t = {}
					for k = 1, 6 do
						t[k] = tonumber(w[o8 + 2 + k])
						if not t[k] then return nil end
					end
					return { a1, a2, t }
				end
				local adds, rms = {}, {}
				if ok then
					for i = 1, m do
						local r = rec8(8 + n * 4, i)
						if not r then ok = false; break end
						adds[#adds + 1] = r
					end
				end
				if ok then
					for i = 1, re do
						local r = rec8(8 + n * 4 + m * 8, i)
						if not r then ok = false; break end
						rms[#rms + 1] = r
					end
				end
				-- Bridge/tunnel tail: <type idx> per ADDED edge after the legacy payload.
				if ok then
					local tb = 8 + n * 4 + m * 8 + re * 8
					for i = 1, m do
						adds[i][4], adds[i][5] = 0, -1
						if #w >= tb + m * 2 then
							adds[i][4] = tonumber(w[tb + (i - 1) * 2 + 1]) or 0
							adds[i][5] = tonumber(w[tb + (i - 1) * 2 + 2]) or -1
						end
					end
				end
				if ok then
					-- No classification here any more: the whole street payload
					-- replays natively on the peer (CONX). Positive ids that still
					-- resolve get their positions attached for the peer's node
					-- lookup; the removed edge's endpoints are mapped peer-side.
					local spos = {}
					for _, e in ipairs(adds) do
						for k = 1, 2 do
							local id = e[k]
							if id >= 0 and not spos[id] then
								pcall(function()
									local nc = api.engine.getComponent(id, api.type.ComponentType.BASE_NODE)
									if nc and nc.position then
										spos[id] = { nc.position.x or nc.position[1],
										             nc.position.y or nc.position[2],
										             nc.position.z or nc.position[3] }
									end
								end)
							end
						end
					end
					pendingRoadc[#pendingRoadc + 1] = { at = gameTime() or 0, posOf = posOf,
						adds = adds, rms = rms, spos = spos, etype = etype, stype = stype,
						ttype = ttype, cat = cat }
					log(string.format("ROADC: parked street payload (%d nodes, %d edges, %d removals) for pairing",
						n, #adds, #rms))
				else
					log("inject: bad ROADC line: " .. line:sub(1, 70))
				end

			elseif (o == "VBUY" or o == "VREPL") and #w >= 3 then
				-- A player's BuyVehicle or ReplaceVehicle, from the hook. Convert
				-- NOW, on this instance, while the ids still mean something: depot
				-- id -> position + file, vehicle id -> cross-peer key, model ids ->
				-- file names.
				--
				--   VBUY  <depotChild>    <n> <model nl loads.. r g b na autos..>*n [ng groups..]
				--   VREPL <vehicleEntity> <n> <model nl loads.. r g b na autos..>*n [ng groups..]
				--
				-- The config encoding is byte-identical after the first field, so
				-- both ops share this parser: two copies of it drifted apart the
				-- moment one of them learned about vehicleGroups.
				local depot = tonumber(w[2])   -- VBUY: the depot child; VREPL: the vehicle
				local n = tonumber(w[3]) or 0
				local i = 4
				local parts, ok = {}, (depot ~= nil and n >= 1)
				for k = 1, n do
					if not ok then break end
					local model, nl = tonumber(w[i]), tonumber(w[i + 1])
					if not (model and nl) then ok = false; break end
					i = i + 2
					local loads = {}
					for j = 1, nl do loads[j] = tonumber(w[i]) or 0; i = i + 1 end
					local r, g, b = tonumber(w[i]), tonumber(w[i + 1]), tonumber(w[i + 2])
					i = i + 3
					local na = tonumber(w[i]) or 0
					i = i + 1
					local autos = {}
					for j = 1, na do autos[j] = tonumber(w[i]) or 0; i = i + 1 end
					if not (r and g and b) then ok = false; break end
					parts[#parts + 1] = { model = model, loads = loads, color = { r, g, b }, autos = autos }
				end
				local ng = tonumber(w[i]) or 0
				i = i + 1
				local groups = {}
				for j = 1, ng do groups[j] = tonumber(w[i]) or 0; i = i + 1 end
				if ok and #parts >= 1 then
					-- Model ids are per-instance resource indices; the wire carries
					-- file names. Encoded once here for whichever op we are in.
					local enc = {}
					for _, p in ipairs(parts) do
						local name
						pcall(function() name = api.res.modelRep.getName(p.model) end)
						if type(name) ~= "string" or name == "" then name = "#" .. p.model end
						enc[#enc + 1] = table.concat({ name,
							table.concat(p.loads, "/"),
							string.format("%.4f,%.4f,%.4f", p.color[1], p.color[2], p.color[3]),
							table.concat(p.autos, "/") }, "~")
					end

					if o == "VREPL" then
						-- ReplaceVehicle: the first field is the VEHICLE, so it maps
						-- to a cross-peer key exactly like VSELL / VDEPOT / VREV do.
						-- Without a key the peer cannot name the vehicle either, so
						-- the replace stays local and the worlds diverge -- say so
						-- loudly rather than ship a guess.
						--
						-- OPEN ITEM (needs a two-instance run to settle, not a
						-- guess): if the engine mints a NEW entity for a replaced
						-- vehicle, the K.PEER rebinds the key from its command result
						-- (execVReplace) but the ORIGINATOR -- whose replace applied
						-- natively, outside our command -- has no result to rebind
						-- from, and its key would still name the dead id. The log
						-- lines to compare are 'EXEC VREPL ... result=' on the peer
						-- and the next 'veh: local vehicle N has no cross-peer key'
						-- here. Do not paper over it with a poll until the capture
						-- shows the id actually changes.
						local k = vehKeyFor(depot)
						if k then
							log(string.format("VREPL: %s, %d part(s): %s", k, #parts, enc[1]:sub(1, 60)))
							scheduleLocal("VREPL", { veh = k,
							                         parts = table.concat(enc, ";"),
							                         groups = table.concat(groups, "/"),
							                         skipOrigin = 1 })
						else
							log(string.format("VREPL: vehicle %d has no cross-peer key -- "
								.. "the replace stays LOCAL (divergence)", depot))
						end
					else
						-- The command's depot is the VEHICLE_DEPOT CHILD entity, not the
						-- construction (measured: r9=281727, no CONSTRUCTION component).
						-- Find the parent construction -- the one whose depots list
						-- holds the child -- and ship ITS position and file.
						local dx, dy, dfile, dparent
						pcall(function()
							local parent
							for _, rec in pairs(consByKey) do
								local co = api.engine.getComponent(rec.id, api.type.ComponentType.CONSTRUCTION)
								if co and co.depots then
									for i = 1, #co.depots do
										if co.depots[i] == depot then parent = rec.id; break end
									end
								end
								if parent then break end
							end
							if not parent then
								-- construction not in our table (e.g. from the save):
								-- scan every construction once
								local list = game.interface.getEntities({ radius = 999999 },
									{ type = "CONSTRUCTION", includeData = false }) or {}
								for _, id in pairs(list) do
									local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
									if co and co.depots then
										for i = 1, #co.depots do
											if co.depots[i] == depot then parent = id; break end
										end
									end
									if parent then break end
								end
							end
							if parent then
								dparent = parent
								local co = api.engine.getComponent(parent, api.type.ComponentType.CONSTRUCTION)
								if co and co.transf then dx, dy = co.transf[13], co.transf[14] end
								if co and co.fileName then dfile = tostring(co.fileName) end
							end
						end)
						if not (dx and dy) then
							log(string.format("VBUY: depot %d has no position -- NOT replicated", depot))
						else
							log(string.format("VBUY: depot %d at %.1f,%.1f (%s), %d part(s): %s",
								depot, dx, dy, tostring(dfile), #parts, enc[1]:sub(1, 60)))
							scheduleLocal("VBUY", { x = dx, y = dy, file = dfile or "?",
							                        parts = table.concat(enc, ";"),
							                        groups = table.concat(groups, "/"),
							                        skipOrigin = 1 })
							-- our own new vehicle gets the same key the peer will use
							expectVehicle(K.INSTANCE .. ":" .. tostring(seqNo), dparent or depot)
						end
					end
				else
					log("inject: bad " .. tostring(o) .. " line: " .. line:sub(1, 70))
				end

			elseif o == "VSELL" and #w >= 2 then
				local n = tonumber(w[2]) or 0
				local keys = {}
				for i = 1, n do
					local id = tonumber(w[2 + i])
					local k = id and vehKeyFor(id)
					if k then keys[#keys + 1] = k end
				end
				if #keys > 0 then
					log("VSELL: " .. table.concat(keys, ","))
					scheduleLocal("VSELL", { keys = table.concat(keys, ","), skipOrigin = 1 })
					for i = 1, n do local id = tonumber(w[2 + i]); if id then forgetVehicle(id) end end
				else
					log(string.format("VSELL: %d id(s) but none shippable -- the sale stays LOCAL (divergence)", n))
				end

			elseif (o == "VNAME" and #w >= 3) or (o == "VCOLOR" and #w >= 5) then
				-- The slice ships a LOCAL entity id. Work out what kind of thing it
				-- is here, while we can still ask the engine, and put the shared key
				-- on the wire instead: a vehicle key, a line key, or a position for
				-- a construction. Anything else (a town building, an industry) is
				-- not ours to rename.
				local id = tonumber(w[2])
				local kind, key
				if id then
					key = vehKeyOf[id] and vehKeyFor(id) or nil
					if key then kind = "veh" end
					if not key then
						key = lineKeyOf[id] and lineKeyFor(id) or nil
						if key then kind = "line" end
					end
					if not key then
						local co
						pcall(function() co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION) end)
						if co and co.transf then
							local ck = conKey(co.transf[13], co.transf[14])
							if consByKey[ck] then key, kind = ck, "con" end
						end
					end
					-- a vehicle or line that came out of the save: primed, not registered.
					-- Lines first -- forgetVehicle does not clear primedVeh, so a stale
					-- vehicle id could otherwise shadow a live line (review, 2026-08-31).
					if not key and primedLines[id] then
						key = lineKeyFor(id); if key then kind = "line" end
					end
					if not key and primedVeh[id] then
						key = vehKeyFor(id); if key then kind = "veh" end
					end
				end
				if key then
					if o == "VNAME" then
						log(string.format("VNAME: %s %s = %s", kind, key, tostring(w[3])))
						scheduleLocal("VNAME", { kind = kind, key = key, name = w[3], skipOrigin = 1 })
					else
						log(string.format("VCOLOR: %s %s = %s,%s,%s", kind, key, w[3], w[4], w[5]))
						scheduleLocal("VCOLOR", { kind = kind, key = key,
							r = tonumber(w[3]), g = tonumber(w[4]), b = tonumber(w[5]), skipOrigin = 1 })
					end
				else
					log(string.format("%s: entity %s is not a tracked vehicle, line or construction -- not shipped",
						o, tostring(w[2])))
				end

			elseif o == "VREV" and #w >= 2 then
				local id = tonumber(w[2])
				local k = id and vehKeyFor(id)
				if k then
					log("VREV: " .. k)
					scheduleLocal("VREV", { key = k, armed = CM.lastArmed or 0 })
				end

			elseif o == "VDEPOT" and #w >= 3 then
				local id, sell = tonumber(w[2]), tonumber(w[3]) or 0
				local k = id and vehKeyFor(id)
				if k then
					log(string.format("VDEPOT: %s sell=%d", k, sell))
					scheduleLocal("VDEPOT", { key = k, sell = sell, skipOrigin = 1 })
				end

			elseif o == "VLINE" and #w >= 4 then
				local id, line, stop = tonumber(w[2]), tonumber(w[3]), tonumber(w[4]) or 0
				local k = id and vehKeyFor(id)
				local lk = line and lineKeyFor(line)
				if k and lk then
					log(string.format("VLINE: %s -> line %s stop %d", k, lk, stop))
					scheduleLocal("VLINE", { key = k, line = lk, stop = stop, armed = CM.lastArmed or 0 })
				end

			elseif o == "LCREATE" then
				pendingLineCreates[#pendingLineCreates + 1] = { since = gameTime() or 0 }

			elseif o == "LUPDATE" and #w >= 2 then
				local lid = tonumber(w[2])
				-- The UI fires UpdateLine right after CreateLine; this line is
				-- parsed BEFORE the tick's pollLineKeys would key the new line.
				-- Key it now so the first stops are not dropped.
				if lid and not lineKeyOf[lid] and not primedLines[lid] then pollLineKeys() end
				local lk = lid and lineKeyFor(lid)
				if lk then
					local snap = lineSnapshot(lid)
					if snap then
						log(string.format("LUPDATE: %s '%s'", lk, unescName(snap.name)))
						scheduleLocal("LUPDATE", { key = lk, name = snap.name, color = snap.color, wait = snap.wait,
						                           stops = snap.stops, skipOrigin = 1 })
					else
						log("LUPDATE: line " .. tostring(lid) .. " could not be read back -- not replicated")
					end
				end

			elseif o == "LDELETE" and #w >= 2 then
				local lid = tonumber(w[2])
				if lid and not lineKeyOf[lid] and not primedLines[lid] then pollLineKeys() end
				local lk = lid and lineKeyFor(lid)
				if lk then
					log("LDELETE: " .. lk)
					scheduleLocal("LDELETE", { key = lk, skipOrigin = 1 })
					forgetLine(lid)
				end

			elseif o == "ROADN" and #w >= 9 then
				local n     = tonumber(w[2]) or 0
				local etype = tonumber(w[3]) or 0
				local stype = tonumber(w[4]) or 16
				local ttype = tonumber(w[5]) or 1
				-- The hook writes x,y pairs; z is sampled HERE, where groundAt is
				-- in scope, and travels with the command so both peers use the
				-- same heights.
				local coords = {}
				for i = 1, n do
					local x, y = tonumber(w[4 + i * 2]), tonumber(w[5 + i * 2])
					if not (x and y) then coords = nil; break end
					coords[#coords + 1] = string.format("%.4f", x)
					coords[#coords + 1] = string.format("%.4f", y)
					coords[#coords + 1] = string.format("%.4f", groundAt(x, y))
				end
				if coords and #coords == n * 3 and n >= 2 then
					scheduleLocal("ROADN", { pts = table.concat(coords, ","),
					                         etype = etype, stype = stype, ttype = ttype })
				else
					log("inject: bad ROADN line: " .. line:sub(1, 60))
				end

			-- ROAD/RAIL x0 y0 x1 y1
			elseif (o == "ROAD" or o == "RAIL") and #w >= 5 then
				local x0, y0 = tonumber(w[2]), tonumber(w[3])
				local x1, y1 = tonumber(w[4]), tonumber(w[5])
				if x0 and y0 and x1 and y1 then
					local a = { x0 = x0, y0 = y0, z0 = groundAt(x0, y0),
					            x1 = x1, y1 = y1, z1 = groundAt(x1, y1), stype = 16 }
					if o == "RAIL" then
						a.ttype = tonumber(w[6]) or 1
						a.cat   = tonumber(w[7]) or 0
					end
					scheduleLocal(o, a)
				end

			-- CON <file> x y
			elseif o == "CON" and #w >= 4 then
				local x, y = tonumber(w[3]), tonumber(w[4])
				if x and y then
					scheduleLocal("CON", { file = w[2], x = x, y = y, z = groundAt(x, y) })
				end

			-- DEMOLISH x y
			elseif o == "DEMOLISH" and #w >= 3 then
				local x, y = tonumber(w[2]), tonumber(w[3])
				if x and y then
					scheduleLocal("DEMOLISH", { x = x, y = y, z = groundAt(x, y) })
				end

			else
				log("inject: unparsed line: " .. line:sub(1, 60))
			end
			end)
			if not okLine then
				log("inject dispatch error: " .. tostring(errLine) .. " -- " .. line:sub(1, 60))
			end
		end
	end
end

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

function CM.setSpeed(v, why)
	CM.lastSetSpeed = v
	CM.paceSetTick = ticks
	CM.paceApplied = false
	CM.paceQuietUntil = ticks + CM.PACE_COOLDOWN
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
	CM.baseSpeed = s
	CM.catchingUp = false
	broadcast(string.format("LSSPEED v=%d o=%s", s, K.INSTANCE))
	log(string.format("SPEED: player set %d -- shared with the peer", s))
end

function CM.pace(ahead)
	if paused then return end                       -- the barrier owns the speed
	local behind = -ahead
	-- A gap this size is not pacing: it is two instances on different saves, or
	-- one still loading. Seen live at 55156 units. Speeding up cannot fix that
	-- and pretending otherwise just runs somebody's game at double speed.
	if behind > 60 or behind < -60 then return end
	local s
	if not pcall(function() s = game.interface.getGameSpeed() end) or s == nil then return end
	if s == 0 then                                   -- player paused on purpose
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
	local settled = CM.paceApplied or (ticks > (CM.paceSetTick or 0) + 8)   -- ~1.5 s
	-- The player moved the lever: that is now the speed they want. Adopt it,
	-- stop any catch-up in progress, and do not argue.
	if CM.lastSetSpeed and settled and s ~= CM.lastSetSpeed then
		if CM.catchingUp then
			log(string.format("PACE: player set speed %s while catching up -- theirs wins", tostring(s)))
		end
		CM.baseSpeed = s
		CM.catchingUp = false
		CM.lastSetSpeed = nil
		CM.paceQuietUntil = ticks + CM.PACE_COOLDOWN
		return
	end
	-- The cooldown exists to stop us raising the speed over and over. Coming
	-- back down is the opposite: delaying it by three seconds at double speed
	-- overshot the peer by 3.8 units, which made the OTHER instance the one
	-- behind, and the two took turns chasing each other.
	if CM.catchingUp and behind <= CM.CATCHUP_DONE then
		CM.catchingUp = false
		CM.setSpeed(CM.baseSpeed or 1, string.format("caught up, %.2f behind", behind))
		return
	end
	if CM.paceQuietUntil and ticks < CM.paceQuietUntil then return end
	if not CM.catchingUp then
		if behind > CM.CATCHUP_BEHIND and s < CM.MAX_SPEED and CM.SPEED_UP[s] then
			CM.baseSpeed = s
			CM.catchingUp = true
			CM.setSpeed(CM.SPEED_UP[s], string.format("%.2f behind the peer", behind))
		elseif behind > CM.CATCHUP_BEHIND then
			-- Already at the top speed: the peer must come down to us, which the
			-- barrier does at K.BARRIER_AHEAD. Nothing to do but say so.
			if not CM.pacedTopWarned then
				CM.pacedTopWarned = true
				log(string.format("PACE: %.2f behind at speed %s -- no notch left, "
					.. "waiting for the barrier", behind, tostring(s)))
			end
		else
			-- In step. Only adopt this as the player's speed if it is not one WE
			-- set: recording our own catch-up 2 as "the player wants 2" is what
			-- made the barrier release to 2, race ahead, hold at 5.2, release to
			-- 2 again -- the loop seen live at 02:30.
			if not CM.lastSetSpeed then CM.baseSpeed = s end
			CM.pacedTopWarned = false
		end
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

local function applyBarrier(now)
	local slowT, fastT = peerBounds()
	CM.slowT, CM.fastT = slowT, fastT
	if not peerSeen then return end

	-- Watchdog first, so it runs even when the conditions below would hold.
	if paused and pausedSince and (ticks - pausedSince) > K.MAX_PAUSE_TICKS then
		paused = false
		pausedSince = nil
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
		if paused then
			paused = false
			pausedSince = nil
			CM.releaseSpeed("peer time stale")
			log("BARRIER release: peer time is stale, not holding against it")
		end
		return
	end

	local ahead = now - slowT          -- the barrier holds against the SLOWEST peer
	CM.shareSpeed()
	CM.pace(now - fastT)               -- the pacer chases the FASTEST
	if ahead > K.BARRIER_AHEAD and not paused then
		paused = true
		pausedSince = ticks
		CM.setSpeed(0, string.format("barrier hold, %.2f ahead of peer", ahead))
		log(string.format("BARRIER hold: %.2f ahead of the slowest peer (%.2f)", ahead, slowT))
	elseif ahead <= K.BARRIER_AHEAD / 2 and paused then
		paused = false
		pausedSince = nil
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
local function ensureRunning()
	if didInitialUnpause or paused then return end
	if ticks < 100 then return end            -- let the world finish loading
	local s
	local ok = pcall(function() s = game.interface.getGameSpeed() end)
	if not ok or s == nil then return end
	didInitialUnpause = true
	if s == 0 then
		pcall(function() api.cmd.sendCommand(api.cmd.make.setGameSpeed(1)) end)
		log("initial unpause (speed 0 -> 1); speed is yours from here")
	else
		log("already running at speed " .. tostring(s))
	end
end

-- ---------- desync check ----------
function compareAt(stamp)
	comparedAt[stamp] = comparedAt[stamp] or {}
	if not myHashes[stamp] then return end
	for o, pr in pairs(CM.peers) do
		local h = pr.hashes[stamp]
		if h and not comparedAt[stamp][o] then
			comparedAt[stamp][o] = true
			compareOne(stamp, o, h, pr.details[stamp])
		end
	end
end

local function checkHash(now)
	local stamp = math.floor(now / K.HASH_EVERY_GAMETIME) * K.HASH_EVERY_GAMETIME
	if lastHashAt == stamp then return end
	lastHashAt = stamp
	local h, detail = worldHash(now)
	myHashes[stamp] = h
	myDetails[stamp] = detail
	broadcast(string.format("LSHASH t=%d h=%s d=%s o=%s", stamp, h, detail or "-", K.INSTANCE))
	CM.dashLastDetail = detail
	-- verdict is set by compareAt; a fresh agreeing tick clears it there
	-- One shared comparison, used from here and from the LSHASH handler, so the
	-- check fires whichever side's hash lands second.
	compareAt(stamp)
end

function data()
	return {
		update = function()
			ticks = ticks + 1
			if ticks % 60 == 0 or not K.INSTANCE then
				if not detectInstance() then return end
			end
			if not K.INSTANCE then return end

			local now = gameTime()
			if not now then return end

			-- Both every tick. pollInject at every 10th tick added up to 1.9s of
			-- pure dead time before a build was even scheduled; a file stat per
			-- tick is far cheaper than that.
			pollEvents()
			pollInject()
			if ticks % K.CON_POLL_EVERY == 0 then pollNewConstructions() end
			if ticks % K.CON_POLL_EVERY == 3 then CM.pollStops() end
			flushConPairs()
			buytestPoll()
			primeConstructions()
			primeVehKeys()
			pollVehKeys()
			primeLineKeys()
			pollLineKeys()
			if not conxBusy and #conxQueue > 0 then
				local nowG = gameTime() or 0
				local head = conxQueue[1]
				if not head.notBefore or nowG >= head.notBefore then
					table.remove(conxQueue, 1)
					execConX(head.c)
				end
			end
			if ticks % K.REMOVAL_POLL_EVERY == 0 then pollConstructionRemovals() end
			-- Cheap: the watch list is empty unless a replay has cut a road, and
			-- each entry is looked at once, CM.SPLIT_SETTLE ticks after the cut.
			if ticks % 60 == 0 and not conxBusy then CM.sweepSplits() end
			if ticks % K.CON_EDIT_SCAN_EVERY == 0 then scanConstructionEdits() end

			if ticks % K.HEARTBEAT_EVERY == 0 then
				broadcast(string.format("LSTICK t=%d o=%s", math.floor(now), K.INSTANCE))
			end

			applyBarrier(now)
			ensureRunning()

			-- Commands that asked to be tried again (a VLINE whose line has not
			-- arrived yet). They were executed once as far as the pump knows, so
			-- that mark is lifted before they go back in.
			if CM.retryQueue and #CM.retryQueue > 0 then
				for _, rc in ipairs(CM.retryQueue) do
					executed[cmdKey(rc)] = nil
					queue[#queue + 1] = rc
				end
				CM.retryQueue = {}
			end

			-- run everything due, in the agreed order
			if #queue > 0 then
				table.sort(queue, cmdLess)
				local keep = {}
				for _, c in ipairs(queue) do
					if c.at <= now then
						local k = cmdKey(c)
						if not executed[k] then
							executed[k] = true
							-- MEASUREMENT: how far past its stamp is a command actually
							-- issued? update() runs per frame while the clock moves in
							-- 0.2-unit sim steps, so at speed 2-3 the first frame past a
							-- stamp can be several steps late -- and differently late on
							-- each instance. Logged on every command, worst case kept in
							-- the status line (applylag=). If this is routinely > 0 the
							-- sim-step gate is justified.
							local lag = now - c.at
							if lag > (CM.applyLagMax or 0) then CM.applyLagMax = lag end
							CM.applyCount = (CM.applyCount or 0) + 1
							if lag > 0.01 then CM.applyLate = (CM.applyLate or 0) + 1 end
							log(string.format("APPLY %s seq=%s origin=%s at=%.1f now=%.1f lag=%.1f",
								tostring(c.op), tostring(c.seq), tostring(c.origin), c.at, now, lag))
							execute(c)
						end
					else
						keep[#keep + 1] = c
					end
				end
				queue = keep
			end

			-- EVERY tick, not every 50th: checkHash itself dedupes to one hash
			-- per K.HASH_EVERY_GAMETIME stamp. Sampling on a tick modulus put each
			-- instance on its own phase of the stamp grid (A hashed t%20 in {0,8},
			-- B in {4,12}) so the stamp sets were DISJOINT: one SYNC verdict in an
			-- entire session, and a real 3-edge divergence sat invisible behind it.
			checkHash(now)

			if ticks % 15 == 0 then
				-- The dashboard file: one key=value per line, then the recent
				-- events. Read by guiUpdate in the GUI Lua state.
				pcall(function()
					local f = io.open(K.BASE .. "lockstep_dash_" .. K.INSTANCE .. ".txt", "w")
					if f then
						local sp = "?"
						pcall(function() sp = tostring(game.interface.getGameSpeed()) end)
						f:write(string.format("t=%d\npeer=%s\nskew=%s\ndesyncs=%d\nlate=%d\napplylag=%.1f\napplylate=%d\napplied=%d\nqueued=%d\npaused=%s\nspeed=%s\nverdict=%s\ndetail=%s\n",
							math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
							CM.slowT and string.format("%+.1f", now - CM.slowT) or "?",
							desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
							#queue, paused and "yes" or "no", sp, CM.dashVerdict or "-", tostring(CM.dashLastDetail or "-")))
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
							desyncs, CM.lateCount, CM.applyLagMax or 0, CM.applyLate or 0, CM.applyCount or 0,
							#queue, paused and "  PAUSED" or ""))
						f:close()
					end
				end)
			end
			if ticks % 300 == 0 then
				log(string.format("alive t=%d peer=%s queued=%d desyncs=%d paused=%s",
					math.floor(now), tostring(CM.slowT and math.floor(CM.slowT) or "?"),
					#queue, desyncs, tostring(paused)))
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
				-- which instances are on this machine's data dirs right now
				local present = {}
				for letter in ("abcdefgh"):gmatch(".") do
					local bases = { K.BASE }
					for _, pth in ipairs(CM.baseCandidates or {}) do if pth ~= K.BASE then bases[#bases + 1] = pth end end
					for _, base in ipairs(bases) do
						local ff = io.open(base .. "lockstep_dash_" .. letter .. ".txt", "r")
						if ff then ff:close(); present[#present + 1] = letter; break end
					end
				end
				if #present == 0 then present = { K.INSTANCE or "a" } end
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
					D.rows = { "t", "peer", "skew", "speed", "paused", "queued", "desyncs", "late", "applylag", "applied" }
					D.labels = { t = "game time", peer = "peer time", skew = "skew", speed = "speed", paused = "held by barrier",
					             queued = "queued", desyncs = "desyncs", late = "late arrivals", applylag = "worst apply lag", applied = "commands applied" }
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
					box:addItem(D.table)
					box:addItem(D.verdict)
					local body = api.gui.comp.Component.new("mpDashboard")
					body:setLayout(box)
					D.win = api.gui.comp.Window.new("Multiplayer", body)
					D.win:setPosition(20, 120)
					statusWin = D.win     -- keep the old handle alive for the close/rebuild path
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
				local dashes = {}
				for _, letter in ipairs(D.cols) do dashes[letter] = readDash(letter) end
				for _, key in ipairs(D.rows) do
					for _, letter in ipairs(D.cols) do
						local dd = dashes[letter]
						D.cells[key][letter]:setText(dd and dd[key] or "-")
					end
				end
				local mine = dashes[K.INSTANCE or "a"]
				-- just the verdict and the lane names; the hash detail is in the log
				D.verdict:setText("verdict: " .. (mine and mine.verdict or "-"))
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
