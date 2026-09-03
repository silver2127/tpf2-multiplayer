-- MP Bridge (single mod for all instances).
-- Identity: read from tpf2_instance.txt (written by the bridge DLL at init).
-- Capture: new constructions (fileName+transf+params) and vehicles -> capture file.
-- Replay: peer events from events file -> buildConstruction / buyVehicle.
-- Loop guard: replayed entities tagged remote, never re-shipped.
-- ---------- runtime data directory ----------
-- The SAME contract as lockstep.lua and bridge/src/datadir.h, resolved in the
-- same order, because all three halves have to land on one directory or they
-- read and write past each other:
--   1. $TPF2MP_DATADIR             (the dev harness pins the old workshop out dir)
--   2. $LOCALAPPDATA/tpf2mp/data/  (shipping layout: Program Files is read-only
--                                   for the game process, LOCALAPPDATA is not)
--   3. the workshop literal        (the dev rig before the data dir existed)
-- The FIRST candidate holding tpf2_instance.txt wins: the bridge DLL writes
-- that file at game start, so its presence proves the DLLs settled there.
--
-- This was candidate 3 alone, hardcoded. That workshop path is dead -- nothing
-- has written to it since the data dir landed -- so on any shipping install
-- IDENTITY_FILE pointed at a file that does not exist, detectInstance never
-- returned true, and the mod ran forever with INSTANCE = nil: no capture, no
-- replay, and a log line saying only "[mpb-?]".
-- The game's Lua may lack 'os' entirely, hence the pcall around every getenv.
local BASE_SOURCE = nil
local BASE = (function()
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
	for _, c in ipairs(cands) do
		local f = io.open(c.path .. "tpf2_instance.txt", "r")
		if f then
			f:close()
			BASE_SOURCE = c.source .. " (identity file found)"
			return c.path
		end
	end
	-- no identity anywhere yet: the shipping default when the environment was
	-- readable, otherwise the workshop literal (always last in the list)
	local pick = cands[#cands]
	for _, c in ipairs(cands) do if c.source == "LOCALAPPDATA" then pick = c end end
	BASE_SOURCE = pick.source .. " (no identity file yet)"
	return pick.path
end)()
local IDENTITY_FILE = BASE .. "tpf2_instance.txt"

local INSTANCE = nil
local CAPTURE_FILE, EVENTS_FILE
-- purchases arrive here from the native buyVehicle hook; declared up top so
-- detectInstance assigns the local rather than silently creating a global
local BUY_FILE = nil
local buyOffset = -1
local suppressBuyUntil = 0
local ticks = 0
local known = {}
local primed = false
local eventsOffset = -1   -- -1 = seek to end on first poll (skip stale history)
local knownVehs = {}
local vehPrimed = false

-- Constructions create their own edges (a road depot spawns an access road).
-- Those edges get picked up by pollEdges and shipped separately, but the peer's
-- buildConstruction already made an identical one, so the replay collides and
-- fails. Remember where constructions recently appeared -- local or replayed --
-- and don't capture edges that show up next to them.
local conPos = {}              -- [constructionId] = {x,y,z}, for demolish detection
local remoteDemolished = {}    -- coarse pos key -> true, demolitions we replayed
local function demolishKey(x, y)
	return string.format("%d,%d", math.floor(x + 0.5), math.floor(y + 0.5))
end

-- Set whenever we replay ANYTHING from the peer; pollEdges gates edge removals
-- on it (see REPLAY_QUIET_TICKS).
--
-- MUST be declared HERE, above pollEdges. It was originally declared just above
-- replayLine, far below pollEdges, so inside pollEdges it was not a local at
-- all -- it resolved to a nil global and `ticks - lastReplayTick` threw. The
-- sweep's pcall swallowed it, and because the guard sits inside `#gone > 0`,
-- every sweep containing a REMOVAL aborted while addition-only sweeps passed.
-- Net effect: additions replicated, removals never did, and the peer ended up
-- with the old road plus both new halves -- the "roads overlap" bug.
local lastReplayTick = -10000

local recentCons = {}          -- { {x=, y=, tick=} }
local CON_EDGE_RADIUS = 60     -- metres
local CON_EDGE_WINDOW = 30     -- ticks (3 s at 10 Hz)

local function noteConstruction(x, y)
	if not x or not y then return end
	recentCons[#recentCons + 1] = { x = x, y = y, tick = ticks }
end

local function nearRecentConstruction(x, y)
	local keep, hit = {}, false
	for _, c in ipairs(recentCons) do
		if ticks - c.tick <= CON_EDGE_WINDOW then
			keep[#keep + 1] = c
			local dx, dy = c.x - x, c.y - y
			if dx * dx + dy * dy <= CON_EDGE_RADIUS * CON_EDGE_RADIUS then hit = true end
		end
	end
	recentCons = keep
	return hit
end

local function log(s) print("[mpb-" .. (INSTANCE or "?") .. "] " .. s) end

-- ---------- channel ownership guard ----------
-- This mod and mp_lockstep_1 compute the SAME filenames: tpf2_capture_<inst>.txt
-- and tpf2_events_<inst>.txt. Enable both and two writers interleave lines into
-- one capture file while two readers race each other's byte offset on the events
-- file -- the wire then carries a shuffle of two different protocols and neither
-- side can parse it. "Run this INSTEAD of MP Bridge" was enforced by nothing but
-- a sentence in mod.lua's description, so a player who ticks both boxes gets
-- silent corruption that looks like a transport bug.
--
-- Two detectors, because neither covers the other's case:
--   * lockstep's global LS. It is assigned at TOP LEVEL in that chunk, so it
--     exists the instant the mod loads, and game scripts share one Lua state
--     (which is why that global is visible here at all).
--   * lockstep_status_<inst>.txt CHANGING between two checks. That catches a
--     lockstep that is not in this Lua state but IS sharing this data dir.
--     Change, not mere existence: the file outlives the session that wrote it,
--     and tripping on a leftover would disable this mod permanently.
--
-- Latching, not toggling: once the channel has been shared it may already hold
-- interleaved lines, so there is nothing safe to resume into.
local conflict = nil            -- reason string once tripped; nil = clear to run
local lsStatusPrev = nil
local conflictLogged = -10000
local function channelConflict()
	if conflict then return true end
	if type(LS) == "table" and LS.findNodeNear ~= nil then
		conflict = "mp_lockstep_1 is loaded in this Lua state (its global LS is set)"
	elseif INSTANCE then
		local f = io.open(BASE .. "lockstep_status_" .. INSTANCE .. ".txt", "r")
		if f then
			local cur = f:read("*a")
			f:close()
			if lsStatusPrev ~= nil and cur ~= lsStatusPrev then
				conflict = "mp_lockstep_1 is live on this data dir (lockstep_status_"
					.. INSTANCE .. ".txt is being rewritten)"
			end
			lsStatusPrev = cur
		end
	end
	return conflict ~= nil
end

-- Loud, and it repeats: a one-shot line scrolls out of the log long before
-- anyone looks, and the symptom (garbage on the wire) gives no hint of the
-- cause.
local function reportConflict()
	if ticks - conflictLogged < 100 then return end
	conflictLogged = ticks
	log("!! DISABLED -- " .. tostring(conflict))
	log("!! Both mods own tpf2_capture_" .. tostring(INSTANCE) .. ".txt and"
		.. " tpf2_events_" .. tostring(INSTANCE) .. ".txt. Running them together"
		.. " interleaves two protocols into one file and corrupts the wire.")
	log("!! Disable ONE of them in the mod list (MP Bridge is the older one;"
		.. " MP Lockstep supersedes it) and reload the save.")
end

local function detectInstance()
	local f = io.open(IDENTITY_FILE, "r")
	if not f then return false end
	local s = f:read("*l")
	f:close()
	if s and #s > 0 then
		local inst = s:gsub("%s", "")
		if inst == INSTANCE then return true end
		-- Identity used to be latched once and never revisited, so if the
		-- wrong bridge wrote this file we silently polled the wrong pair of
		-- files for the rest of the session and looked simply dead. Adopt the
		-- change loudly instead.
		if INSTANCE then
			log("!! IDENTITY CHANGED: " .. INSTANCE .. " -> " .. inst
				.. " -- bridge injected into the wrong process? re-homing")
			eventsOffset = -1
		end
		INSTANCE = inst
		CAPTURE_FILE = BASE .. "tpf2_capture_" .. INSTANCE .. ".txt"
		EVENTS_FILE = BASE .. "tpf2_events_" .. INSTANCE .. ".txt"
		BUY_FILE = BASE .. "tpf2_buy_" .. INSTANCE .. ".txt"
		buyOffset = -1
		log("identity detected: instance " .. INSTANCE)
		log("  capture -> " .. CAPTURE_FILE)
		log("  events  <- " .. EVENTS_FILE)
		return true
	end
	return false
end

local function appendLine(path, line)
	-- The one place anything reaches the capture file, so the conflict guard
	-- sits here as well as in update(): a poll that is already in flight when
	-- the conflict is detected must not get one last line onto the shared wire.
	if conflict then return false end
	local f = io.open(path, "a")
	if not f then return false end
	f:write(line .. "\n")
	f:close()
	return true
end

-- ---------- serialization ----------
-- Depth 4 was too shallow for modular stations: nested module params came out
-- as the string "?" where the engine expects a table, so replay fed
-- buildConstruction structurally invalid params. Go deeper, and when we do hit
-- the limit emit an empty table rather than a string of the wrong type.
local MAX_SER_DEPTH = 8
local function ser(v, depth)
	depth = depth or 0
	local t = type(v)
	if t == "number" or t == "boolean" then return tostring(v) end
	if t == "string" then return string.format("%q", v) end
	if t == "table" then
		if depth >= MAX_SER_DEPTH then return "{}" end
		-- Keys are SORTED, so the same table always serializes to the same
		-- string. pairs() order is arbitrary and can differ between two calls on
		-- the same table, which would make a param diff report a change on every
		-- sweep and spam the peer with meaningless station edits.
		local keys = {}
		for k in pairs(v) do keys[#keys + 1] = k end
		table.sort(keys, function(a, b)
			local ta, tb = type(a), type(b)
			if ta ~= tb then return ta < tb end       -- numbers before strings
			if ta == "number" or ta == "string" then return a < b end
			return tostring(a) < tostring(b)
		end)
		local parts = {}
		for _, k in ipairs(keys) do
			local key = type(k) == "number" and ("[" .. k .. "]") or ("[" .. string.format("%q", k) .. "]")
			local inner = ser(v[k], depth + 1)
			-- omit values we cannot represent (functions, userdata) instead of
			-- substituting a placeholder of the wrong type
			if inner then parts[#parts + 1] = key .. "=" .. inner end
		end
		return "{" .. table.concat(parts, ",") .. "}"
	end
	return nil
end

-- ---------- feature flags for the channels added 2026-08-10 ----------
--
-- Both of these are DESTRUCTIVE by nature: EDGEDEL deletes geometry, and
-- CONMOD calls upgradeConstruction, which REPLACES a construction rather than
-- editing it. One echo bug in the removal path already destroyed real player
-- track this session, and a report of "it builds, then nothing happens, and the
-- next one is broken" points at exactly this pair.
--
-- Off by default. Turn ONE on at a time and retest -- the older channels
-- (BUILD, EDGE, DEMOLISH, TIME, VEH, LINE) are unaffected and keep working.
-- ON again 2026-08-10. It was disabled to bisect a "cannot build in the joiner"
-- bug and turned out to be the WRONG SUSPECT -- that was a native crash in an
-- edge-adoption scan, since removed. Junctions genuinely need this: building
-- one makes the game SPLIT the existing edge (remove + two halves), so with
-- removals off the peer keeps the original edge AND both halves, which is the
-- overlapping-geometry "road intersection bug".
-- The destructive feedback that originally justified turning it off is now
-- guarded by REPLAY_QUIET_TICKS below.
-- CONMOD ON 2026-08-10. It is the only channel that can see a modular station
-- being upgraded: adding a platform or a module rewrites the construction's
-- params in place without creating or destroying an entity, so BUILD, DEMOLISH
-- and EDGE are all blind to it. With it off, a truck-terminal upgrade
-- replicated nothing at all, and a rail platform added on B reached A as bare
-- track that was not part of any station -- both reported from real play.
--
-- What made it unsafe before was NOT the edit itself: upgradeConstruction
-- REPLACES the construction, and the two capture paths that see a replacement
-- (id vanished, id appeared) had no echo guard, so replaying a peer's edit sent
-- a DEMOLISH back that would bulldoze their station. conRewrite below closes
-- that; see the comment there.
local EDGEDEL_ENABLED = true    -- capture AND replay of edge removals
local CONMOD_ENABLED  = true    -- capture AND replay of station/param edits

-- ---------- station / construction edits ----------
--
-- Editing a station (adding a platform, changing a module) does NOT create a
-- new entity and does not remove the old one -- it rewrites the construction's
-- `params` in place. The capture side only ever asked "is this id new?", so
-- every edit was invisible, which is why "only some of the edits carried over"
-- (the ones that happened to also spawn edges).
--
-- The replay side is game.interface.upgradeConstruction(id, fileName, params),
-- which is exactly what the shipped constructionupgrader.lua does -- including
-- clearing `seed`, which is fatal to reuse.
local conParams = {}          -- [id] = canonical serialized params
local conEditable = {}        -- [id] = true for constructions worth diffing
local conModCursor = nil
-- Constructions the edit scan skipped because they did not look alive. Reported
-- by the beacon: a steady non-zero here means stations are dropping out of the
-- scan and their edits are silently not replicating.
local deadSkips = 0

-- Echo guard, keyed like demolishKey. MUST expire: a station can be edited
-- again and again, and an entry that lives forever makes every previously
-- replayed position a permanent dead spot where the local player's own edits
-- are silently dropped. Exactly the bug that keeping edge signatures forever
-- caused -- a replayed road suppressed a later rail along the same alignment.
-- Keyed by position AND by the exact params we asked the peer to apply.
--
-- Position-plus-timeout was wrong, and it produced a bug with a very specific
-- shape: once A edited a station, that spot on B went deaf to the player's OWN
-- edits until the timer expired -- "the edits don't carry over, but only if A
-- originally placed and edited the station". B logged three consecutive
-- "skipped echo of replayed station edit" for edits the player had just made.
--
-- The timeout was also far longer than it read. CONMOD_SIG_TTL was commented
-- "5 s at 10 Hz", but update() only reaches 10 Hz while the sim is RUNNING; in
-- practice it ticks at ~4 Hz, so the window was ~12 s of real time. Every
-- tick-based TTL in this file has that same optimism -- and while paused, ticks
-- barely advance at all.
--
-- Content matching removes the guesswork: the echo is the one params value we
-- just applied, so swallow exactly that and ship anything else, however soon it
-- arrives. The tick TTL survives only to stop the table growing forever.
local remoteConMod = {}
local CONMOD_SIG_TTL = 3000   -- generous upper bound; content is the real test

-- `seed` is regenerated by the engine and is deliberately stripped before
-- replay, so it can never match and must be excluded from the comparison.
local function serStable(params)
	if type(params) ~= "table" then return nil end
	local copy = {}
	for kk, vv in pairs(params) do
		if kk ~= "seed" then copy[kk] = vv end
	end
	return ser(copy)
end

local function markRemoteConMod(k, sig) remoteConMod[k] = { sig = sig, tick = ticks } end
local function isRemoteConMod(k, sig)
	local e = remoteConMod[k]
	if not e then return false end
	if ticks - e.tick > CONMOD_SIG_TTL then remoteConMod[k] = nil return false end
	-- Only the params we actually pushed count as an echo. A different value is
	-- the local player editing the same station, and it must ship.
	if e.sig and sig and e.sig ~= sig then return false end
	return true
end

-- The echo guard that makes CONMOD safe to enable at all.
--
-- upgradeConstruction REPLACES a construction rather than editing it: the old
-- entity id disappears and a new one appears at the same spot. To the capture
-- sweep that is indistinguishable from the player bulldozing a station and
-- building a replacement, so replaying a peer's station edit made this side
-- emit a DEMOLISH *and* a BUILD straight back at them -- and that DEMOLISH
-- would bulldoze the very station they had just edited.
--
-- isRemoteConMod alone does not cover it: it is only consulted by the params
-- diff. The new-construction path asked nothing but `known[id]`, and the id is
-- brand new, so it had no echo guard whatsoever. This one is consulted by all
-- three construction capture paths.
--
-- TTL, like every other signature table here: a permanent entry turns that spot
-- into a dead zone where the local player's own later edits are dropped.
-- 300 ticks (30 s), matching REPLAY_QUIET_TICKS rather than the 2 sweeps the
-- demolish path actually needs. upgradeConstruction may not take effect on the
-- same tick it is called, and the two failure modes are not symmetric: too long
-- a TTL drops a genuine local demolish at that exact spot within the window,
-- which loses a delete; too short a TTL ships a delete the peer never asked for
-- and destroys their station. Err long.
local conRewrite = {}
local CON_REWRITE_TTL = 300   -- ticks (30 s at 10 Hz)
local function markConRewrite(x, y) conRewrite[demolishKey(x, y)] = ticks end
local function isConRewrite(x, y)
	local k = demolishKey(x, y)
	local t = conRewrite[k]
	if not t then return false end
	if ticks - t > CON_REWRITE_TTL then conRewrite[k] = nil return false end
	return true
end
-- Ceiling on the edit scan, NOT a target. It must exceed the number of editable
-- constructions or the scan round-robins and station edits ship LATE.
--
-- That is not a latency nicety, it corrupts the order on the wire. Adding a
-- platform to a rail station changes params AND creates the platform's track
-- edges. pollConstructions runs before pollEdges, so a same-sweep detection
-- puts CONMOD ahead of its edges, which is the order the peer needs. At 120
-- against an editable set of 339 the diff lagged ~3 sweeps: the peer built 24
-- free-standing rails first and the station edit that would have claimed them
-- arrived afterwards, into ground now occupied by that very track. Measured --
-- 24 x "edge replay (rail): success=true" followed by "station edit replay
-- ok=false err=internal error".
--
-- The set is only constructions that passed isEditable (no town buildings), so
-- this is a few hundred, not the ~5,350 constructions on the map.
local CONMOD_SCAN_PER_SWEEP = 2000

-- api.engine.getComponent on an id that no longer exists is a NATIVE CRASH, not
-- a Lua error: it takes the process down and pcall cannot catch it. That has
-- now bricked an instance twice -- once via an edge-adoption scan (removed) and
-- once here, on the sweep right after upgradeConstruction retired a
-- construction id, with the crash trace pointing straight at
-- mpbridge.lua_update().
--
-- Anywhere an id can outlive the entity -- a persistent table, a stale spatial
-- query, an id captured before a replay -- it must go through this first.
-- Probed LAZILY on first use, not in init().
--
-- init() only runs when a NEW GAME starts. Continuing a save calls load()
-- instead, which is how this project is tested and played every single time --
-- so a probe placed in init() never ran, HAS_ENTITY_EXISTS stayed false, and
-- the guard silently degraded to the unguarded behaviour it was written to
-- replace. It logged nothing either way, so it looked installed.
local HAS_ENTITY_EXISTS = nil   -- nil = not probed yet
local function entityAlive(id)
	if type(id) ~= "number" or id < 0 then return false end
	if HAS_ENTITY_EXISTS == nil then
		-- Probe by CALLING it, never by type().
		--
		-- `type(api.engine.entityExists) == "function"` reported MISSING for a
		-- function that is plainly present -- the shipped
		-- res/scripts/selectortooltip.lua calls it. sol2 exposes bound calls as
		-- userdata with a __call metamethod, so the type test is simply the
		-- wrong question to ask of anything on the api table.
		local fn = api and api.engine and api.engine.entityExists
		if fn == nil then
			HAS_ENTITY_EXISTS = false
		else
			local ok, res = pcall(fn, id)
			HAS_ENTITY_EXISTS = ok and type(res) == "boolean"
		end
		if HAS_ENTITY_EXISTS then
			log("api.engine.entityExists available -- stale-id crash guard ACTIVE")
		else
			log("!! api.engine.entityExists MISSING -- stale-id crash guard INACTIVE;"
				.. " getComponent on a retired id will hard-crash this process")
		end
	end
	if not HAS_ENTITY_EXISTS then return true end   -- no probe available; old behaviour
	local ok, res = pcall(api.engine.entityExists, id)
	return ok and res == true
end

-- Which construction OWNS an edge, or nil if nothing does.
--
-- This is the discriminator the edge channel was missing. A station generates
-- its own platform track, and the peer's copy of that station generates the
-- same track when the BUILD or CONMOD is replayed -- so shipping those edges
-- lays a second set of rails on top of the first. Measured: 18 of the peer's
-- replayed rail edges deduped as "already present", 12 built anyway.
--
-- Crucially this is OWNERSHIP, not proximity. Skipping edges merely NEAR a
-- construction was tried before and was wrong: it dropped the road SPLIT edges
-- created when a depot snaps onto an existing road, which the peer cannot
-- regenerate, so the depot arrived unconnected. Those split edges belong to the
-- road, not to the construction, so they still ship.
-- When each construction last generated edges of its own: placed, replayed, or
-- upgraded. Ownership ALONE is not a safe reason to drop an edge.
--
-- Track a player lays INTO a station gets adopted by that station's
-- construction, so getConstructionEntity names the station as its owner -- and
-- the peer's copy of that station does NOT regenerate player track. Suppressing
-- on ownership alone therefore ate the connecting rail: the peer replayed the
-- junction's split removal and then had nothing to reconnect with. Measured 281
-- edges suppressed against 16 shipped.
--
-- Only the moment a construction is created or upgraded does it emit its own
-- geometry, and only then does the peer independently produce the same edges.
-- Outside that window an owned edge is the player's, and it must ship: a
-- duplicate edge is harmless, missing geometry is not.
local conEdgeGrace = {}
local CON_EDGE_GRACE = 100    -- ticks
local function noteConEdges(conId)
	if type(conId) == "number" and conId >= 0 then conEdgeGrace[conId] = ticks end
end
local function inConEdgeGrace(conId)
	local t = conEdgeGrace[conId]
	if not t then return false end
	if ticks - t > CON_EDGE_GRACE then conEdgeGrace[conId] = nil return false end
	return true
end

local HAS_CON_OWNER = nil
local function constructionOwnerOf(eid)
	if type(eid) ~= "number" then return nil end
	-- entityAlive FIRST. This is an engine call on an id that came from a
	-- getEntities sweep, and a track MERGE retires edge ids -- exactly the case
	-- where that list goes stale. An engine call on a retired id is a native
	-- crash that pcall cannot catch, which has already taken an instance down
	-- twice in this project.
	if not entityAlive(eid) then return nil end
	local fn = game and game.interface and game.interface.getConstructionEntity
	if HAS_CON_OWNER == nil then
		-- Probed by calling, and lazily -- same two traps as entityAlive:
		-- sol2 bindings are userdata not "function", and init() never runs when
		-- continuing a save.
		if fn == nil then
			HAS_CON_OWNER = false
		else
			local ok, res = pcall(fn, eid)
			HAS_CON_OWNER = ok and (res == nil or type(res) == "number")
		end
		if HAS_CON_OWNER then
			log("game.interface.getConstructionEntity available -- construction-owned"
				.. " edges will not be shipped (the peer's own construction makes them)")
		else
			log("!! game.interface.getConstructionEntity MISSING -- construction-owned"
				.. " edges will be shipped and may duplicate on the peer")
		end
	end
	if not HAS_CON_OWNER or fn == nil then return nil end
	local ok, res = pcall(fn, eid)
	if ok and type(res) == "number" and res >= 0 and res ~= eid then return res end
	return nil
end

-- What counts as an editable construction.
--
-- NOT "has PLAYER_OWNED". That was the obvious filter and it was wrong:
-- buildConstruction takes no player argument and produces an UNOWNED
-- construction (which is exactly why the replay path has to call setPlayer
-- afterwards). So every construction this harness or a replay creates fails
-- that test, and no station edit was ever captured.
--
-- Use the same rule the shipped constructionupgrader.lua uses instead: skip
-- anything with town buildings attached. Towns grow constantly, and shipping
-- that churn would be pure noise.
local function isEditable(e)
	if not e or not e.params then return false end
	local tb = e.townBuildings
	if tb and #tb > 0 then return false end
	return true
end

-- Inverse of ser(). Returns nil (not a partial table) on anything unparseable,
-- so a corrupted line is dropped rather than replayed as structurally invalid
-- params -- which the engine turns into a fatal assert, not an error return.
local function deserParams(pstr)
	if not pstr then return nil end
	local chunk = load("return " .. pstr, "params")
	if not chunk then return nil end
	local ok, v = pcall(chunk)
	if ok and type(v) == "table" then return v end
	return nil
end

-- ---------- capture ----------
local function pollConstructions()
	local ok, err = pcall(function()
		local entities = game.interface.getEntities(
			{ radius = 999999 }, { type = "CONSTRUCTION", includeData = false })
		local seenNow = {}
		for _, id in pairs(entities) do
			-- getEntities can still list an id the engine has just retired, and
			-- every getComponent below would then hard-crash the process. Filter
			-- at the door so nothing downstream has to remember to check.
			if entityAlive(id) then
			seenNow[id] = true
			if not known[id] then
				known[id] = "local"
				if primed then
					local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
					local e = game.interface.getEntity(id)
					if co and co.fileName then
						local parts = {}
						for i = 1, 16 do parts[#parts + 1] = string.format("%.4f", co.transf[i]) end
						-- column-major 4x4: translation is elements 13,14,15
						noteConstruction(co.transf[13], co.transf[14])
						local pstr = ""
						if e and e.params then
							local s = ser(e.params)
							if s then pstr = " params=" .. s end
						end
						if isConRewrite(co.transf[13], co.transf[14]) then
							-- our own upgradeConstruction replacing a construction we
							-- were told to edit. Shipping this back asks the peer to
							-- build a duplicate station on top of the one they have.
							log("skipped echo of construction replaced by a replayed edit")
						else
							appendLine(CAPTURE_FILE,
								"BUILD file=" .. co.fileName .. " t=" .. table.concat(parts, ",") .. pstr)
							log("captured: " .. co.fileName)
						end
					end
				end
				-- A construction we have only just seen is emitting its own
				-- edges right now, so the peer's copy will emit them too. This is
				-- the only window in which dropping an owned edge is safe.
				noteConEdges(id)
				-- remember where it is so we can report it if it disappears
				local co2 = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				if co2 and co2.transf then
					conPos[id] = { x = co2.transf[13], y = co2.transf[14],
						z = co2.transf[15], file = co2.fileName }
				end
				-- Prime the edit baseline NOW, while we are already holding this
				-- entity. The throttled edit scan walks ~120 constructions per
				-- sweep, so on this save a full cycle takes about 22 s -- a
				-- station built and then edited inside that window would first
				-- be sampled AFTER the edit, leaving nothing to diff against and
				-- losing the edit silently. Measured exactly that: a depot
				-- edited 4 s after being placed produced no CONMOD at all.
				local e2 = game.interface.getEntity(id)
				if isEditable(e2) then
					-- Cache the verdict, not just the baseline. isEditable was
					-- the only reason the edit scan had to touch all ~5,350
					-- constructions: it called getEntity on every one just to
					-- rediscover that a town building is uninteresting. Town
					-- buildings never turn into stations, so the answer is
					-- stable, and caching it shrinks the scan set to player
					-- builds -- see the edit scan below.
					conEditable[id] = true
					conParams[id] = ser(e2.params)
				end
			end
			end   -- entityAlive(id)
		end

		-- Demolition. The capture side only ever asked "is this id new?", so
		-- anything REMOVED was invisible and bulldozing never replicated.
		-- Compare against the previous sweep to find ids that vanished.
		if primed then
			for id, p in pairs(conPos) do
				if seenNow[id] then
					p.missing = nil
				else
					-- Require the id to be absent from TWO consecutive sweeps.
					-- getEntities is a spatial query over ~200k entities; one
					-- transient miss would otherwise fire a demolish, and a
					-- false demolish destroys something real on the peer.
					p.missing = (p.missing or 0) + 1
					if p.missing >= 2 then
						conPos[id] = nil
						known[id] = nil
						-- This is the ONE place allowed to retire an entity from
						-- the edit scan, because it is the only one that waits for
						-- two consecutive misses. The scan itself must never do it
						-- (see the note there).
						conEditable[id] = nil
						conParams[id] = nil
						if remoteDemolished[demolishKey(p.x, p.y)] then
							log("skipped echo of replayed demolish")
						elseif isConRewrite(p.x, p.y) then
							-- The old id of a construction OUR OWN replayed edit
							-- replaced. Nothing was demolished; without this the
							-- peer's station edit came back as an order to
							-- bulldoze the station they had just edited.
							log("skipped echo of construction replaced by a replayed edit")
						else
							-- carry the fileName so the peer can refuse to
							-- bulldoze if it finds something different there
							appendLine(CAPTURE_FILE, string.format(
								"DEMOLISH p=%.3f,%.3f,%.3f file=%s",
								p.x, p.y, p.z, p.file or "?"))
							log(string.format("captured demolish at %.1f,%.1f (%s)",
								p.x, p.y, tostring(p.file)))
						end
					end
				end
			end
		end
		-- ---- edits ----
		-- Walks conEditable, NOT every construction on the map.
		--
		-- This used to round-robin 120 of ~5,350 constructions per sweep, so a
		-- full cycle took about 22 s -- and an edit was only noticed when the
		-- cursor happened to reach it. That lag is not cosmetic, it reorders the
		-- wire: adding a platform to a rail station creates its track edges,
		-- pollEdges ships those on the NEXT sweep, and the CONMOD that explains
		-- them arrived up to 22 s later. The peer therefore built bare track and
		-- only afterwards learned it was meant to be part of a station -- exactly
		-- the reported "the rail showed on A but not as part of the station".
		--
		-- conEditable holds only constructions that passed isEditable when first
		-- seen (no town buildings), which is a small fraction of the map, so the
		-- whole set fits in one sweep and the CONMOD ships alongside its edges.
		-- CONMOD_SCAN_PER_SWEEP stays as a ceiling in case that set is large.
		if primed then
			local scanned = 0
			local k = conModCursor
			if k ~= nil and not (seenNow[k] and conEditable[k]) then k = nil end
			while scanned < CONMOD_SCAN_PER_SWEEP do
				local nk
				if k == nil then nk = next(conEditable) else nk = next(conEditable, k) end
				if nk == nil then k = nil break end
				k = nk
				scanned = scanned + 1
				if not seenNow[nk] or not entityAlive(nk) then
					-- SKIP this sweep -- do NOT delete.
					--
					-- Deleting here was a one-way door: the only place that adds
					-- to conEditable is the new-construction path, gated on
					-- `not known[id]`, and known[id] is still set. So a single
					-- transient miss -- one getEntities query that did not list
					-- an entity that is perfectly alive -- removed a station from
					-- the edit scan FOREVER, silently. Its edits then stopped
					-- replicating with no error anywhere, which is exactly the
					-- "works a->b but not b->a" asymmetry: whichever side happens
					-- to miss first stops shipping edits.
					--
					-- The demolish path already owns real removals: it requires
					-- two consecutive misses before believing an entity is gone,
					-- and it now clears conEditable/conParams too.
					deadSkips = deadSkips + 1
				else
					local e = game.interface.getEntity(nk)
					local co = api.engine.getComponent(nk, api.type.ComponentType.CONSTRUCTION)
					if CONMOD_ENABLED and isEditable(e) and co and co.fileName then
						local s = ser(e.params)
						local prev = conParams[nk]
						conParams[nk] = s
						if s and not prev then
							-- No baseline to diff against, so this edit cannot be
							-- shipped -- it only establishes one. Silent before,
							-- and indistinguishable from "no edit happened",
							-- which is the worst way for a channel to fail. If
							-- this appears when the player edits a station, the
							-- construction had fallen out of conParams and the
							-- real bug is upstream of here.
							log("station edit NOT shipped: no baseline for "
								.. co.fileName .. " -- baselining now")
						elseif prev and s and s ~= prev then
							local x, y, z = co.transf[13], co.transf[14], co.transf[15]
							if isRemoteConMod(demolishKey(x, y), serStable(e.params)) then
								log("skipped echo of replayed station edit")
							else
								appendLine(CAPTURE_FILE, string.format(
									"CONMOD p=%.3f,%.3f,%.3f file=%s params=%s",
									x, y, z, co.fileName, s))
								noteConEdges(nk)
								log("captured station edit: " .. co.fileName)
							end
						end
					end
				end
			end
			conModCursor = k
		end

		primed = true
	end)
	if not ok then log("capture error: " .. tostring(err)) end
end

-- position of the construction a vehicle belongs to, so the peer can buy from
-- the equivalent depot (entity ids are not comparable across instances)
local function depotPosOf(e)
	-- depot is -1 for a vehicle that is out on a line rather than in a depot.
	-- getComponent(-1, ...) throws, which aborted the whole (unlogged) pcall in
	-- pollVehicles and silently captured nothing at all.
	if not e or type(e.depot) ~= "number" or e.depot < 0 then return nil end
	local ok, co = pcall(function()
		return api.engine.getComponent(e.depot, api.type.ComponentType.CONSTRUCTION)
	end)
	if ok and co and co.transf then return co.transf[13], co.transf[14] end
	return nil
end

local function pollVehicles()
	-- log failures. An unlogged pcall here hid the fact that this poll was
	-- throwing on every call, which is why vehicles never replicated.
	local ok, err = pcall(function()
		-- NOT transportVehicleSystem.getVehicles(): that system exposes no
		-- functions at all, so the call failed inside the pcall and quietly
		-- yielded an empty list -- vehicle capture never once fired, despite
		-- the save holding 78 of them. getEntities with type=VEHICLE works.
		local vehs = game.interface.getEntities(
			{ radius = 999999 }, { type = "VEHICLE", includeData = false }) or {}
		for _, vid in pairs(vehs) do
			if not knownVehs[vid] then
				knownVehs[vid] = true
				if vehPrimed then
					local e = game.interface.getEntity(vid)
					if e and e.vehicles and e.vehicles[1] and e.vehicles[1].fileName then
						local models = {}
						for _, part in pairs(e.vehicles) do
							if part and part.fileName then models[#models + 1] = part.fileName end
						end
						-- The vehicle entity names its own depot, so ship that
						-- construction's POSITION (ids differ between
						-- instances). Falls back to the vehicle's own position.
						local dx, dy = depotPosOf(e)
						if not dx then
							pcall(function()
								if e.position then
									dx = e.position.x or e.position[1]
									dy = e.position.y or e.position[2]
								end
							end)
						end
						appendLine(CAPTURE_FILE, string.format(
							"VEH at=%.3f,%.3f models=%s", dx or 0, dy or 0,
							table.concat(models, "|")))
						log("captured vehicle: " .. table.concat(models, "|"))
					end
				end
			end
		end
		vehPrimed = true
	end)
	if not ok then log("vehicle capture error: " .. tostring(err)) end
end

-- ---------- purchases (from the native buyVehicle hook) ----------
-- A vehicle in a depot is not a world entity, so polling can never see a
-- purchase. The DLL hooks the buyVehicle command factory and appends
--   BUY depot=<entityId> mids=<modelId,...>
-- here; this side turns the depot id into a POSITION (ids are not comparable
-- across instances) and ships it.
local function pollBuys()
	if not BUY_FILE then return end
	local f = io.open(BUY_FILE, "rb")
	if not f then return end
	local size = f:seek("end")
	-- start at 0, not end-of-file: the DLL truncates this file at startup, so
	-- everything in it belongs to this session and must be consumed. Skipping
	-- to the end here would drop the first purchase, since the file is created
	-- by that very purchase.
	if buyOffset < 0 then buyOffset = 0 end
	if size < buyOffset then buyOffset = 0 end
	f:seek("set", buyOffset)
	local chunk = f:read("*a") or ""
	local lastNl = chunk:find("\n[^\n]*$")
	local complete = lastNl and chunk:sub(1, lastNl) or ""
	buyOffset = buyOffset + #complete
	f:close()
	for line in complete:gmatch("([^\n]+)\n") do
		local depotId = tonumber(line:match("depot=(%-?%d+)"))
		local mids = line:match("mids=([%d,]+)")
		if depotId and mids then
			if ticks < suppressBuyUntil then
				log("skipped echo of replayed purchase")
			else
				-- A depot is its OWN entity, not the construction housing it --
				-- it has no CONSTRUCTION component, which is why the transform
				-- lookup came back empty. It does expose a position.
				local dx, dy
				pcall(function()
					local de = game.interface.getEntity(depotId)
					if de and de.position then
						dx = de.position.x or de.position[1]
						dy = de.position.y or de.position[2]
					end
				end)
				if dx then
					appendLine(CAPTURE_FILE,
						string.format("VEH at=%.3f,%.3f mids=%s", dx, dy, mids))
					log("captured purchase: mids=" .. mids)
				else
					log("purchase at unknown depot " .. depotId .. " -- dropped")
				end
			end
		end
	end
end

-- ---------- edge (road/rail) capture ----------
--
-- This channel used to detect only NEW entity ids. That one decision is behind
-- four separate reported bugs, because three things never produced a new id:
--
--   demolishing an edge   -- the id disappears; nothing was ever shipped
--   upgrading an edge     -- electrifying keeps the same id, so catenary,
--                            bus lanes and tram track silently never crossed
--   building a junction   -- the game SPLITS the existing edge: the old id is
--                            removed and two new ones appear. The two new ones
--                            shipped, the removal did not, so the peer ended up
--                            with the original edge overlapping both halves --
--                            "a rail I built got cut in two in the other".
--
-- So the sweep now diffs the whole edge set: added / removed / changed.
-- edgeState[eid] = { kind, p0, p1, sig, prop } -- p0/p1 are kept as strings so
-- a removal can still be described after the edge is gone.
local edgeState = {}
local edgePrimed = false

-- coarse geometry signatures of edges we replayed from the peer, so pollEdges
-- doesn't capture them and ship them straight back (echo loop)
local remoteEdgeSigs = {}

-- order-independent, ~1m-tolerant signature of an edge's ground track. Coarse
-- on purpose: a replayed edge can snap its endpoints to existing nodes, so an
-- exact key would miss the echo it is meant to suppress.
-- kind is part of the key: without it a replayed ROAD permanently suppressed a
-- RAIL laid along the same alignment, and a segment of a chain silently never
-- got captured -- the peer then showed the track cut in two.
local function edgeSig(kind, x0, y0, x1, y1)
	local a = string.format("%d,%d", math.floor(x0 + 0.5), math.floor(y0 + 0.5))
	local b = string.format("%d,%d", math.floor(x1 + 0.5), math.floor(y1 + 0.5))
	if a > b then a, b = b, a end
	return tostring(kind) .. ":" .. a .. "|" .. b
end

-- Entries only need to live long enough for the local capture sweep to notice
-- the edge our replay just created (a second or two). Keeping them forever
-- made every past replay a permanent landmine for future builds.
-- 60 s, not 5. A station BUILD emits a burst of edges and the replay of a
-- burst takes far longer than five seconds, so the signature expired before the
-- local sweep noticed each new edge -- which then looked like a local build and
-- was shipped straight back. Measured: the joiner built NOTHING and still
-- captured 139 edges, with suppression firing only 7 times.
local EDGE_SIG_TTL = 600
local function markRemoteEdge(sig) remoteEdgeSigs[sig] = ticks end
local function isRemoteEdge(sig)
	local t = remoteEdgeSigs[sig]
	if not t then return false end
	if ticks - t > EDGE_SIG_TTL then remoteEdgeSigs[sig] = nil return false end
	return true
end

-- Same idea for the other two directions. Replaying a peer's removal makes the
-- edge vanish locally, and replaying an upgrade changes local properties; both
-- would otherwise be re-captured on the next sweep and bounced back.
local remoteEdgeDelSigs, remoteEdgeModSigs = {}, {}
local function markRemoteEdgeDel(sig) remoteEdgeDelSigs[sig] = ticks end
local function isRemoteEdgeDel(sig)
	local t = remoteEdgeDelSigs[sig]
	if not t then return false end
	if ticks - t > EDGE_SIG_TTL then remoteEdgeDelSigs[sig] = nil return false end
	return true
end
local function markRemoteEdgeMod(sig) remoteEdgeModSigs[sig] = ticks end
local function isRemoteEdgeMod(sig)
	local t = remoteEdgeModSigs[sig]
	if not t then return false end
	if ticks - t > EDGE_SIG_TTL then remoteEdgeModSigs[sig] = nil return false end
	return true
end

local function v3str(p)
	return string.format("%.3f,%.3f,%.3f", p.x or p[1] or 0, p.y or p[2] or 0, p.z or p[3] or 0)
end

-- Filters retired ids at the DOOR, so nothing downstream has to remember to.
--
-- readEdgeGeom/readEdgeProps/constructionOwnerOf all call straight into the
-- engine with these ids, and an engine call on a retired id is a native crash
-- that pcall cannot catch -- it takes the process down. The node maps are a
-- snapshot and go stale exactly when the network is being rewritten: a track
-- MERGE retires edge ids, and so does a split, a demolish, and every
-- construction replay. That is the whole reason this sweep runs.
local function collectEdges()
	local edges = {}
	pcall(function()
		local m = api.engine.system.streetSystem.getNode2StreetEdgeMap()
		for _, list in pairs(m) do
			for _, eid in ipairs(list) do
				if entityAlive(eid) then edges[eid] = "road" end
			end
		end
	end)
	pcall(function()
		local m = api.engine.system.streetSystem.getNode2TrackEdgeMap()
		for _, list in pairs(m) do
			for _, eid in ipairs(list) do
				if entityAlive(eid) then edges[eid] = "rail" end
			end
		end
	end)
	return edges
end

-- Carry ALL the edge-kind properties, not just the type.
--   BASE_EDGE_TRACK  = { trackType, catenary }
--   BASE_EDGE_STREET = { streetType, hasBus, tramTrackType }
-- Only the type was being sent once, so electrification, tram tracks and bus
-- lanes silently vanished on the peer -- a rail built electrified in A arrived
-- plain in B. Returned as one string so it doubles as the change signature.
local function readEdgeProps(eid, kind)
	local typeId = -1
	local catenary, hasBus, tramType = false, false, 0
	pcall(function()
		if kind == "road" then
			local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
			if sc then
				if sc.streetType then typeId = sc.streetType end
				if sc.hasBus then hasBus = true end
				if sc.tramTrackType then tramType = sc.tramTrackType end
			end
		else
			local tc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK)
			if tc then
				if tc.trackType then typeId = tc.trackType end
				if tc.catenary then catenary = true end
			end
		end
	end)
	local extra = (kind == "rail")
		and (" cat=" .. (catenary and 1 or 0))
		or  (" bus=" .. (hasBus and 1 or 0) .. " tram=" .. tramType)
	return "type=" .. typeId .. extra
end

local function readEdgeGeom(eid)
	-- The edge is filtered by collectEdges, but node0/node1 come from the edge
	-- component itself and are NOT: a merge can retire a node while the edge
	-- referencing it is still readable. getComponent on that node is a native
	-- crash, so check before dereferencing.
	if not entityAlive(eid) then return nil end
	local comp = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
	if not comp then return nil end
	local function npos(nid)
		if not entityAlive(nid) then return nil end
		local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
		return nc and nc.position
	end
	local p0, p1 = npos(comp.node0), npos(comp.node1)
	if not p0 or not p1 then return nil end
	return p0, p1, comp
end

-- Record an edge's current state without shipping it. Used for edges we
-- created ourselves by replaying the peer, so the next sweep sees no change.
local function noteEdgeState(eid, kind)
	if not kind then
		kind = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK)
			and "rail" or "road"
	end
	local p0, p1 = readEdgeGeom(eid)
	if not p0 or not p1 then return end
	edgeState[eid] = {
		kind = kind, p0 = v3str(p0), p1 = v3str(p1),
		sig = edgeSig(kind, p0.x or p0[1], p0.y or p0[2], p1.x or p1[1], p1.y or p1[2]),
		prop = readEdgeProps(eid, kind),
	}
end

-- Re-reading properties for every edge on the map each sweep is far too
-- expensive (the test save carries ~9,600). Additions and removals are free --
-- they fall out of the id set alone -- so only the mutation check is throttled,
-- walking a slice of the set per sweep and wrapping around. A full pass takes
-- roughly 15 s, which is the worst-case latency for an upgrade to replicate.
local MUT_SCAN_PER_SWEEP = 300
local mutCursor = nil

-- More removals than this in a single sweep is not a player with a bulldozer.
-- Dragging over a long stretch can legitimately exceed it -- that is the
-- accepted cost of not letting sim churn or a stale state table propagate a
-- mass delete to the peer. The suppression is logged, never silent.
local EDGE_DEL_BURST_MAX = 12

-- How long after replaying anything a side must stay quiet before it is allowed
-- to originate an edge removal. 30 s at 10 Hz -- comfortably longer than the
-- churn a construction replay produces.
local REPLAY_QUIET_TICKS = 300

-- ---------- line capture ----------
--
-- A line is a list of stops, each naming a STATION GROUP. Entity ids are not
-- comparable across instances, so a stop travels as the station group's
-- POSITION and is resolved back on the peer -- the same rule the edge and
-- construction channels already follow.
--
-- Replay is api.cmd.make.createLine(name, color, playerEntity, line), where
-- argument 2 is a Vec3f COLOUR. That is documented, not guessable: probing got
-- as far as "argument 2 must be userdata" and every natural candidate --
-- including CmdData::CreateLine, the type the error message itself names -- is
-- rejected.
local lineState = {}          -- [lineId] = stops signature
local linePrimed = false
local remoteLineSigs = {}
local LINE_SIG_TTL = 100      -- ticks; createLine round-trips slower than an edge

local function markRemoteLine(sig) remoteLineSigs[sig] = ticks end
local function isRemoteLine(sig)
	local t = remoteLineSigs[sig]
	if not t then return false end
	if ticks - t > LINE_SIG_TTL then remoteLineSigs[sig] = nil return false end
	return true
end

local function stationGroupPos(sg)
	local ok, e = pcall(game.interface.getEntity, sg)
	if ok and e and e.position then
		local p = e.position
		return p.x or p[1], p.y or p[2]
	end
	return nil
end

-- "x,y;x,y;..." over the line's stops, or nil if any stop cannot be located
local function lineStopsSig(lid)
	local lc = api.engine.getComponent(lid, api.type.ComponentType.LINE)
	if not lc or not lc.stops then return nil end
	local parts, bad = {}, false
	pcall(function()
		for i = 1, #lc.stops do
			local x, y = stationGroupPos(lc.stops[i].stationGroup)
			if not x then bad = true return end
			parts[#parts + 1] = string.format("%.1f,%.1f", x, y)
		end
	end)
	if bad or #parts == 0 then return nil end
	return table.concat(parts, ";"), lc
end

local function pollLines()
	local ok, err = pcall(function()
		local lines = api.engine.system.lineSystem.getLines() or {}
		for _, lid in pairs(lines) do
			if not lineState[lid] then
				local stops, lc = lineStopsSig(lid)
				if stops then
					lineState[lid] = stops
					-- The save already carries 26 lines; without priming, the
					-- first sweep after load would ship every one of them.
					if linePrimed then
						if isRemoteLine(stops) then
							log("skipped echo of replayed line")
						else
							local name = "Line"
							pcall(function() name = game.interface.getName(lid) or name end)
							-- name goes LAST: it may contain spaces
							appendLine(CAPTURE_FILE, string.format(
								"LINE wait=%d stops=%s name=%s",
								math.floor(lc.waitingTime or 180), stops, name))
							log("captured line: " .. tostring(name))
						end
					end
				end
			end
		end
		linePrimed = true
	end)
	if not ok then log("line capture error: " .. tostring(err)) end
end

local function pollEdges()
	local ok, err = pcall(function()
		local cur = collectEdges()

		-- ---- removed ----
		-- MUST be emitted before additions, not after. Splitting an edge (which
		-- is what building a junction does) produces one removal and two halves
		-- in the SAME sweep. If the halves are replayed first, each one lands
		-- mid-span on an edge the peer has not removed yet, and buildProposal
		-- rejects a mid-span node outright -- measured, see probe_midspan.txt.
		-- Delete first and the halves then attach to free ground.
		--
		-- Collected before emitting, because a large burst is never a player
		-- demolishing something. It means the sim churned town roads, or our
		-- state table went stale across a load. Removals are DESTRUCTIVE on the
		-- peer -- unlike a duplicate add, a wrong delete destroys their
		-- geometry -- so a burst is dropped rather than shipped. State is
		-- always updated regardless; only the emission is suppressed.
		local gone = {}
		-- Assigning nil to the current key during pairs() is the one mutation
		-- Lua guarantees is safe here.
		for eid, st in pairs(edgeState) do
			if not cur[eid] then
				edgeState[eid] = nil
				gone[#gone + 1] = st
			end
		end
		if edgePrimed and #gone > 0 then
			-- NEVER ship removals while we are busy replaying the peer.
			--
			-- Replaying a construction rebuilds its own edges, so the diff sees
			-- them vanish and reappear. Reporting that as a demolition sends an
			-- EDGEDEL back to the side that owns the geometry, and it dutifully
			-- deletes it. That destroyed real player track: the joiner built
			-- nothing, yet emitted EDGEDEL that the host executed.
			--
			-- An addition that turns out to be redundant is harmless; a deletion
			-- is not. So removals only travel from a side that has been quiet.
			if ticks - lastReplayTick < REPLAY_QUIET_TICKS then
				log(string.format("holding %d edge removal(s): replayed %d tick(s) ago",
					#gone, ticks - lastReplayTick))
			elseif #gone > EDGE_DEL_BURST_MAX then
				log(string.format("suppressed %d edge removals in one sweep (> %d): "
					.. "sim churn or stale state, not a player demolish", #gone, EDGE_DEL_BURST_MAX))
			else
				for _, st in ipairs(gone) do
					if isRemoteEdgeDel(st.sig) then
						log("skipped echo of replayed edge removal: " .. st.kind)
					elseif st.owned then
						-- Construction-owned, so we never shipped it in the first
						-- place (see the added branch). It disappeared because its
						-- construction did, and the peer's DEMOLISH/CONMOD removes
						-- the peer's copy. Shipping a delete for it would target
						-- whatever the peer has at those coordinates instead.
						log("skipped removal of construction-owned edge: " .. st.kind)
					else
						if EDGEDEL_ENABLED then
						appendLine(CAPTURE_FILE, "EDGEDEL kind=" .. st.kind
							.. " p0=" .. st.p0 .. " p1=" .. st.p1)
						log("captured edge removal: " .. st.kind)
						end
					end
				end
			end
		end

		-- ---- added ----
		for eid, kind in pairs(cur) do
			if not edgeState[eid] then
				local p0, p1, comp = readEdgeGeom(eid)
				if p0 and p1 then
					local prop = readEdgeProps(eid, kind)
					local sig = edgeSig(kind, p0.x or p0[1], p0.y or p0[2],
						p1.x or p1[1], p1.y or p1[2])
					local owner = constructionOwnerOf(eid)
					-- Owned is not enough -- it must be owned by a construction
					-- that is generating its geometry RIGHT NOW. See noteConEdges.
					local ownedNow = owner ~= nil and inConEdgeGrace(owner)
					edgeState[eid] = { kind = kind, p0 = v3str(p0), p1 = v3str(p1),
						sig = sig, prop = prop, owned = ownedNow }
					if edgePrimed then
						if isRemoteEdge(sig) then
							-- one we just replayed from the peer; capturing it
							-- would bounce it straight back
							log("skipped echo of replayed edge: " .. kind)
						elseif ownedNow then
							-- This edge belongs to a construction, so the peer's
							-- copy of that construction generates it too. Shipping
							-- it lays a SECOND set of rails over the platform --
							-- the reported "double placed track on top of each
							-- other when the station gets placed".
							--
							-- NOTE this is OWNERSHIP, not proximity. Skipping edges
							-- merely NEAR a construction was tried and was wrong: it
							-- dropped the road SPLIT edges created when a depot
							-- snaps onto an existing road, which the peer cannot
							-- regenerate (buildConstruction does no network
							-- integration), so the depot arrived unconnected. Split
							-- edges belong to the road, not the construction, so
							-- getConstructionEntity returns nil for them and they
							-- still ship.
							-- Coordinates included deliberately: when this
							-- suppression is wrong the symptom on the peer is
							-- missing geometry with no error anywhere, and the
							-- only way to tell "the station's own platform" from
							-- "the player's connecting track" after the fact is
							-- to see where the dropped edge actually was.
							log("skipped construction-owned edge: " .. kind
								.. " (con " .. tostring(owner) .. " makes it on the peer)"
								.. " p0=" .. v3str(p0) .. " p1=" .. v3str(p1))
						else
							local t0 = comp.tangent0 and v3str(comp.tangent0) or "0,0,0"
							local t1 = comp.tangent1 and v3str(comp.tangent1) or "0,0,0"
							appendLine(CAPTURE_FILE, "EDGE kind=" .. kind
								.. " p0=" .. v3str(p0) .. " p1=" .. v3str(p1)
								.. " t0=" .. t0 .. " t1=" .. t1
								.. " " .. prop)
							log("captured edge: " .. kind .. " " .. prop)
						end
					end
				end
			end
		end

		-- ---- changed (throttled slice) ----
		if edgePrimed then
			local k = mutCursor
			if k ~= nil and cur[k] == nil then k = nil end  -- cursor edge is gone
			local scanned = 0
			while scanned < MUT_SCAN_PER_SWEEP do
				local nk, nkind
				if k == nil then nk, nkind = next(cur) else nk, nkind = next(cur, k) end
				if nk == nil then k = nil break end   -- wrapped; resume next sweep
				k = nk
				scanned = scanned + 1
				local st = edgeState[nk]
				if st then
					local prop = readEdgeProps(nk, nkind)
					if prop ~= st.prop then
						st.prop = prop
						if isRemoteEdgeMod(st.sig) then
							log("skipped echo of replayed edge upgrade: " .. st.kind)
						else
							appendLine(CAPTURE_FILE, "EDGEMOD kind=" .. st.kind
								.. " p0=" .. st.p0 .. " p1=" .. st.p1 .. " " .. prop)
							log("captured edge upgrade: " .. st.kind .. " " .. prop)
						end
					end
				end
			end
			mutCursor = k
		end

		edgePrimed = true
	end)
	if not ok then log("edge capture error: " .. tostring(err)) end
end

-- ---------- edge replay ----------
local nextNewNode = -1
local committedNodes = {}   -- [posKey] = entityId, confirmed by a proposal callback
local reservedNodes = {}    -- [posKey] = tick reserved, proposal still in flight
local retryQueue = {}       -- edge lines waiting on reserved nodes
local cachedStreetTypeId = nil
-- a reservation whose proposal never reports back must not wedge that spot
-- forever; release it after this many ticks and let the edge try again.
-- MEASURED 2026-08-07: this update() runs at 10 Hz (GameSim::Step is 5 Hz and
-- the script updates twice per step), NOT the 60 Hz originally assumed -- so
-- 300 ticks was blocking a spot for 30 s, not 5.
local RESERVE_TIMEOUT = 50   -- 5 s at 10 Hz

local function getAnyStreetTypeId()
	if cachedStreetTypeId then return cachedStreetTypeId end
	pcall(function()
		local m = api.engine.system.streetSystem.getNode2StreetEdgeMap()
		for _, edges in pairs(m) do
			for _, eid in pairs(edges) do
				local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
				if sc and sc.streetType and sc.streetType >= 0 then
					cachedStreetTypeId = sc.streetType
					return
				end
			end
		end
	end)
	return cachedStreetTypeId or 0
end

-- Node identity is HORIZONTAL only. The peer recreates a node at the position
-- we shipped, but the engine is free to settle it at a different height
-- (embankments, terrain smoothing under track) -- on a 5% grade that easily
-- exceeds a 1.5 m tolerance. Keying on z made the second segment of a chain
-- fail to find the node the first one had just made, so it created a duplicate
-- at an occupied spot and the proposal was rejected: rails arrived
-- disconnected, only the non-touching ones surviving.
-- Two nodes sharing an x/y really are the same node here; stacked geometry
-- (a bridge over a track) does not share endpoints.
local function posKey(x, y, z)
	return string.format("%.1f/%.1f", x, y)
end

local function findNodeNear(x, y, z, eps)
	local best, bestD
	local maps = {}
	pcall(function() maps[1] = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	pcall(function() maps[2] = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	for _, m in pairs(maps) do
		for nid, _ in pairs(m) do
			local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
			if nc and nc.position then
				local p = nc.position
				-- horizontal only; see posKey above for why z is excluded
				local dx, dy = (p.x or p[1]) - x, (p.y or p[2]) - y
				local d = dx * dx + dy * dy
				if d < eps * eps and (not bestD or d < bestD) then
					best, bestD = nid, d
				end
			end
		end
	end
	return best
end

-- ---------- mid-span splitting on the REPLAY side ----------
--
-- buildProposal REJECTS an edge whose endpoint lands partway along an existing
-- edge (measured; see probe_midspan.txt). The interactive tool splits for you,
-- a raw proposal does not.
--
-- This matters most on the PEER. When a player snaps a road onto an existing
-- road, the host does not split anything -- it attaches to geometry it already
-- has, so it captures no removal. The peer receives an edge whose endpoint sits
-- mid-span on ITS copy, tries to plant a fresh node there, and is refused:
-- observed success=false on 7 of 8 road edges. That is the "road intersection
-- bug", and it was invisible for so long because the test harness drove
-- mptest.lua's actEdge -- which HAS splitting -- instead of this path.
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

local SPLIT_EPS = 3.0
local SPLIT_MIN_U = 0.08   -- nearer an end than this IS the endpoint, not a split

-- Find an existing edge of this kind that (x,y) lies on, and where along it.
-- Bounded work: a cheap reject before sampling, and only edges of the matching
-- map are considered. Everything is wrapped by the caller's pcall.
local function findEdgeContaining(kind, x, y)
	local best, bestD, bestU, seen = nil, nil, nil, {}
	local m
	if kind == "rail" then m = api.engine.system.streetSystem.getNode2TrackEdgeMap()
	else m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end
	for _, list in pairs(m or {}) do
		for _, eid in ipairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local comp, a, b, ta, tb = edgeGeomT(eid)
				if comp then
					local span = (b[1]-a[1])^2 + (b[2]-a[2])^2
					local d0 = (a[1]-x)^2 + (a[2]-y)^2
					local d1 = (b[1]-x)^2 + (b[2]-y)^2
					if d0 < span * 4 + 400 or d1 < span * 4 + 400 then
						for i = 1, 19 do
							local u = i / 20
							local q = hermitePos(a, ta, b, tb, u)
							local d = (q[1]-x)^2 + (q[2]-y)^2
							if d < SPLIT_EPS*SPLIT_EPS and (not bestD or d < bestD) then
								best, bestD, bestU = eid, d, u
							end
						end
					end
				end
			end
		end
	end
	if best and bestU > SPLIT_MIN_U and bestU < (1 - SPLIT_MIN_U) then
		return best, bestU
	end
	return nil
end

-- Copy an existing edge's kind-specific properties onto a new segment.
local function copyEdgeProps(dst, srcEid, kind)
	if kind == "road" then
		local sc = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE_STREET)
		dst.streetEdge = api.type.BaseEdgeStreet.new()
		dst.streetEdge.streetType = (sc and sc.streetType) or getAnyStreetTypeId()
		if sc then
			dst.streetEdge.hasBus = sc.hasBus and true or false
			dst.streetEdge.tramTrackType = sc.tramTrackType or 0
		end
	else
		local tc = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE_TRACK)
		dst.trackEdge = api.type.BaseEdgeTrack.new()
		dst.trackEdge.trackType = (tc and tc.trackType) or 0
		dst.trackEdge.catenary = (tc and tc.catenary) and true or false
		dst.streetEdge = api.type.BaseEdgeStreet.new()
		dst.streetEdge.streetType = getAnyStreetTypeId()
	end
end

local function replayEdge(line)
	local kind = line:match("kind=(%S+)")
	local function vec(tag)
		local x, y, z = line:match(tag .. "=([-%d.]+),([-%d.]+),([-%d.]+)")
		return tonumber(x), tonumber(y), tonumber(z)
	end
	local typeId = tonumber(line:match("type=(%-?%d+)"))
	if not kind or not typeId or typeId < 0 then
		log("edge replay skipped (bad header): " .. line:sub(1, 70))
		return
	end
	local p0x, p0y, p0z = vec("p0")
	local p1x, p1y, p1z = vec("p1")
	if not p0x or not p1x then
		log("edge replay skipped (bad coords): " .. line:sub(1, 70))
		return
	end

	-- Resolve an endpoint WITHOUT mutating state: returns an existing/committed
	-- node id, or a retry flag, or the posKey that still needs a node created.
	local function resolveNode(x, y, z)
		local pk = posKey(x, y, z)
		local c = committedNodes[pk]
		if c then return c, false, nil end
		local since = reservedNodes[pk]
		if since then
			if ticks - since < RESERVE_TIMEOUT then return nil, true, nil end
			reservedNodes[pk] = nil   -- proposal never reported back; unstick it
			log("node reservation at " .. pk .. " timed out, retrying")
		end
		local existing = findNodeNear(x, y, z, 1.5)
		if existing then return existing, false, nil end
		return nil, false, pk
	end

	-- Resolve BOTH endpoints before reserving either. The old code reserved
	-- node0 and only then discovered node1 was busy, bailing out with node0
	-- still marked pending and no proposal in flight -- so that spot stayed
	-- pending forever and every later edge touching it retried endlessly.
	-- Rail is drawn as chains sharing endpoints, so it wedged on segment 2.
	local id0, retry0, need0 = resolveNode(p0x, p0y, p0z)
	local id1, retry1, need1 = resolveNode(p1x, p1y, p1z)
	if retry0 or retry1 then
		retryQueue[#retryQueue + 1] = line
		return
	end

	local sp = api.type.SimpleProposal.new()
	local nodes = {}
	local reserved = {}
	local extraEdges = {}   -- split halves, added before the new edge
	local removeEdges = {}  -- the originals those halves replace
	local splitNote = nil
	local function createNode(x, y, z, pk)
		local id = nextNewNode
		nextNewNode = nextNewNode - 1
		local n = api.type.NodeAndEntity.new()
		n.entity = id
		n.comp.position = api.type.Vec3f.new(x, y, z)
		nodes[#nodes + 1] = n
		reservedNodes[pk] = ticks
		reserved[#reserved + 1] = pk
		return id
	end
	-- Before planting a brand-new node, check whether this endpoint actually
	-- lands ON an existing edge. If it does, the proposal must SPLIT that edge
	-- -- remove it and re-add both halves around the new node -- because
	-- buildProposal refuses a bare mid-span node. This is what makes a junction
	-- replicate; without it the peer rejects the edge outright.
	local function createNodeOrSplit(x, y, z, pk)
		local eid, u
		local ok = pcall(function() eid, u = findEdgeContaining(kind, x, y) end)
		if not ok or not eid then return createNode(x, y, z, pk) end

		local comp, a, b, ta, tb = edgeGeomT(eid)
		if not comp then return createNode(x, y, z, pk) end
		local pm = hermitePos(a, ta, b, tb, u)
		local tm = hermiteTangent(a, ta, b, tb, u)

		local mid = createNode(pm[1], pm[2], pm[3], pk)
		removeEdges[#removeEdges + 1] = eid

		-- sub-curve tangents scale by the length of the parameter interval, so
		-- a curved road keeps its shape through the split
		local function half(nA, nB, tA, tB, sc)
			local h = api.type.SegmentAndEntity.new()
			h.entity = -200000 - ticks - #extraEdges
			h.comp.node0 = nA
			h.comp.node1 = nB
			h.comp.tangent0 = api.type.Vec3f.new(tA[1]*sc, tA[2]*sc, tA[3]*sc)
			h.comp.tangent1 = api.type.Vec3f.new(tB[1]*sc, tB[2]*sc, tB[3]*sc)
			h.comp.type = 0
			h.comp.typeIndex = (kind == "rail") and -1 or 0
			h.type = (kind == "rail") and 1 or 0
			copyEdgeProps(h, eid, kind)
			extraEdges[#extraEdges + 1] = h
		end
		half(comp.node0, mid, ta, tm, u)
		half(mid, comp.node1, tm, tb, 1 - u)
		splitNote = string.format("split %d@%.2f", eid, u)
		return mid
	end

	if not id0 then id0 = createNodeOrSplit(p0x, p0y, p0z, need0) end
	if not id1 then
		-- degenerate zero-length edge: reuse the same node rather than
		-- reserving the same posKey twice
		if need1 == need0 then id1 = id0
		else id1 = createNodeOrSplit(p1x, p1y, p1z, need1) end
	end
	local function releaseReservations()
		for _, pk in ipairs(reserved) do reservedNodes[pk] = nil end
	end
	local n0, n1 = id0, id1
	for i, n in ipairs(nodes) do sp.streetProposal.nodesToAdd[i] = n end

	local t0x, t0y, t0z = vec("t0")
	local t1x, t1y, t1z = vec("t1")
	local e = api.type.SegmentAndEntity.new()
	e.entity = -100000 - ticks  -- unique-ish new edge id
	e.comp.node0 = n0
	e.comp.node1 = n1
	e.comp.tangent0 = api.type.Vec3f.new(t0x or 0, t0y or 0, t0z or 0)
	e.comp.tangent1 = api.type.Vec3f.new(t1x or 0, t1y or 0, t1z or 0)
	e.comp.type = 0
	e.comp.typeIndex = (kind == "rail") and -1 or 0
	-- THIS is what selects street vs track: e.type, not e.comp.type.
	-- Determined empirically (harness experiment, 2026-08-07):
	--   e.type=1, comp.type=0        -> real track edge, lands in the TRACK map
	--   e.type=0                     -> street edge
	--   comp.type=1 (either e.type)  -> proposal always fails
	-- Until now this was hard-coded to 0, so every replayed "rail" silently
	-- became a road.
	e.type = (kind == "rail") and 1 or 0
	-- optional properties; absent in older event lines, so default to the
	-- same values a freshly constructed component would have
	local catenary = (line:match(" cat=(%d)") == "1")
	local hasBus   = (line:match(" bus=(%d)") == "1")
	local tramType = tonumber(line:match(" tram=(%d+)")) or 0
	if kind == "road" then
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = typeId
		e.streetEdge.hasBus = hasBus
		e.streetEdge.tramTrackType = tramType
	else
		e.trackEdge = api.type.BaseEdgeTrack.new()
		e.trackEdge.trackType = typeId
		e.trackEdge.catenary = catenary
		-- A valid streetType is mandatory on EVERY edge or the proposal dies on
		-- ResTypeRep<StreetType>::Get(-1) ("Assertion `iindex > -1'"). It is
		-- only used for validation though -- the resulting track edge comes out
		-- with no street component at all.
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = getAnyStreetTypeId()
	end
	-- halves FIRST, then the new edge -- and the originals they replace go in
	-- the same proposal so the network is never momentarily inconsistent
	local slot = 0
	for _, h in ipairs(extraEdges) do
		slot = slot + 1
		sp.streetProposal.edgesToAdd[slot] = h
	end
	slot = slot + 1
	sp.streetProposal.edgesToAdd[slot] = e
	for i, rid in ipairs(removeEdges) do sp.streetProposal.edgesToRemove[i] = rid end
	if splitNote then log("edge replay (" .. kind .. "): " .. splitNote) end

	-- remember the geometry before sending: pollEdges must not treat the edge
	-- this creates as a local build and ship it back to the peer
	markRemoteEdge(edgeSig(kind, p0x, p0y, p1x, p1y))

	local rok, rerr = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, nil, false), function(res, success)
			-- A failure right next to a construction we just replayed is the
			-- expected case: our own buildConstruction already created that
			-- edge. Say so, rather than making it look like a real fault --
			-- but always ATTEMPT it, because the peer also sends genuine road
			-- SPLIT edges from the same spot and those must get built.
			if success then
				log("edge replay (" .. kind .. "): success=true")
			elseif nearRecentConstruction((p0x + p1x) * 0.5, (p0y + p1y) * 0.5) then
				log("edge replay (" .. kind .. "): already present (construction made it)")
			else
				log("edge replay (" .. kind .. "): success=false")
			end
			releaseReservations()
			-- record committed node ids so later edges can reference them
			if success and type(res) == "table" then
				local function scan(t)
					for _, v in pairs(t) do
						if type(v) == "number" then
							local nc = api.engine.getComponent(v, api.type.ComponentType.BASE_NODE)
							if nc and nc.position then
								local p = nc.position
								committedNodes[posKey(p.x or p[1], p.y or p[2], p.z or p[3])] = v
							end
							-- Record the replayed edge's full state so the next
							-- sweep sees it as unchanged rather than as a local
							-- build to ship back.
							local ec = api.engine.getComponent(v, api.type.ComponentType.BASE_EDGE)
							if ec then noteEdgeState(v) end
						end
					end
				end
				pcall(scan, res)
			end
		end)
	end)
	if not rok then
		log("edge replay error: " .. tostring(rerr))
		releaseReservations()   -- nothing in flight; don't wedge these spots
	end
end

-- Vehicle config construction, following the documented shape at
-- https://wiki.transportfever2.com/api/topics/examples.md.html
--
-- Two earlier attempts took the game down because the config was incomplete,
-- and neither failure is catchable from Lua:
--   1. loadConfig left empty -> Assertion `!loadConfig.empty()' -> SIGABRT
--   2. vehicleGroups left empty -> hang. Per the type reference, each entry is
--      a group size and THE SUM MUST EQUAL THE VEHICLE COUNT; an empty list
--      against a non-empty vehicles list is an inconsistent config.
-- Build it exactly as documented, and never send a partial one.
-- RE-ENABLED 2026-08-10, after the sim wedge was found and fixed.
--
-- The cause was NOT the command, the call site, or the payload size. sol2
-- returns a usertype's vector members BY VALUE, so `tvp.autoLoadConfig[c] = 1`
-- mutated a temporary and was discarded -- leaving a NULL vector that the
-- engine then iterated, hanging the sim thread with nothing for pcall to catch.
-- Found by dumping the raw bytes of a real UI purchase (native buyVehicle hook,
-- RVA 0x9dca00) against a script-built one: +0x60 of the unit struct held a
-- live vector in the game's command and all zeros in ours.
--
-- The config above now read-modify-writes every vector member. Verified:
-- buyVehicle callback success=true, sim kept ticking.
local VEH_REPLAY_ENABLED = true

local function replayVeh(line)
	if not VEH_REPLAY_ENABLED then
		log("veh replay disabled: " .. line:sub(1, 60))
		return
	end
	-- newer lines carry "at=x,y" before models; older ones do not
	local atX = tonumber(line:match("at=([-%d.]+),"))
	local atY = tonumber(line:match("at=[-%d.]+,([-%d.]+)"))
	-- mids= is the native-capture form (raw model ids); models= is the older
	-- file-name form. Accept both.
	local mstr = line:match("mids=([%d,]+)")
	local sep = ","
	if not mstr then mstr = line:match("models=(.+)$") sep = "|" end
	if not mstr then return end
	local models = {}
	for m in mstr:gmatch("[^" .. sep .. "]+") do models[#models + 1] = m end
	if #models == 0 then return end

	-- Ask the game for depots directly instead of guessing from construction
	-- file names. getDepots() covers road/rail/air/water uniformly, and a depot
	-- is its own entity with a position -- it has no CONSTRUCTION component, so
	-- the old filename scan could never have matched one.
	local depot, bestD
	pcall(function()
		for _, did in pairs(game.interface.getDepots() or {}) do
			local de = game.interface.getEntity(did)
			if de and de.position then
				local px = de.position.x or de.position[1]
				local py = de.position.y or de.position[2]
				local d = 0
				if atX and px then
					local ddx, ddy = px - atX, py - atY
					d = ddx * ddx + ddy * ddy
				end
				if not bestD or d < bestD then depot, bestD = did, d end
			end
		end
	end)
	if not depot then
		log("veh replay: no depot found, skipped " .. tostring(models[1]))
		return
	end
	log(string.format("veh replay: depot=%d dist=%.1f models=%s",
		depot, bestD and math.sqrt(bestD) or -1, mstr))
	-- buyVehicle's 4th argument is a TransportVehicleConfig USERDATA, not a
	-- table: passing {vehicles={{part={modelId=n}}}} fails with
	--   "stack index 4, expected userdata, received table".
	-- modelId lives on TransportVehiclePart.part, not on the part itself.
	-- DANGER: an incomplete config does not fail gracefully, it ABORTS the
	-- process. Sending a part with an empty loadConfig tripped
	--   Assertion `!loadConfig.empty()'                     (UpdateTransportModeBitset)
	--   Assertion `loadConfig.size() == tv.compartments.size()'  (MakeVehicleCargoInfo)
	-- and the game died with SIGABRT. These are C++ asserts, so pcall cannot
	-- catch them -- the config MUST be complete before we send, and if we
	-- cannot make it complete we must not send at all.
	local config, nUnits = nil, 0
	local built = pcall(function()
		config = api.type.TransportVehicleConfig.new()
		local now = 0
		pcall(function() now = (game.interface.getGameTime().time or 0) * 1000 end)

		for _, m in ipairs(models) do
			-- mids= carries raw rep ids (native capture); models= carries file
			-- names. Rep ids match across instances for an identical mod set --
			-- the same assumption already used for streetType/trackType.
			local mid = tonumber(m)
			if not mid then mid = api.res.modelRep.find(m) end
			if not mid then error("unresolvable model " .. tostring(m)) end

			-- loadConfig needs one entry per compartment of THIS model
			local nComp = nil
			pcall(function()
				local md = api.res.modelRep.get(mid)
				local tv = md and md.metadata and md.metadata.transportVehicle
				if tv and tv.compartments then
					local c = 0
					for _ in pairs(tv.compartments) do c = c + 1 end
					nComp = c
				end
			end)
			if not nComp or nComp < 1 then
				error("no compartment count for modelId " .. tostring(mid))
			end

			local part = api.type.VehiclePart.new()
			part.modelId = mid
			-- READ-MODIFY-WRITE every vector member. sol2 hands these back BY
			-- VALUE, so `x.vec[i] = v` mutates a temporary and is silently lost.
			-- That left autoLoadConfig as a NULL vector, the engine iterated it,
			-- and the sim thread wedged uncatchably -- diagnosed by dumping the
			-- bytes of a real UI purchase against ours via the native buyVehicle
			-- hook. Do not "simplify" these back to in-place indexing.
			local lc = part.loadConfig
			for c = 1, nComp do lc[c] = 0 end
			part.loadConfig = lc
			-- -1,-1,-1 is the "no custom colour" sentinel a manual purchase uses
			part.reversed = false
			part.color = api.type.Vec3f.new(-1, -1, -1)
			part.logo = ""

			local tvp = api.type.TransportVehiclePart.new()
			tvp.purchaseTime = now
			tvp.maintenanceState = 1.0
			tvp.targetMaintenanceState = 0
			local alc = tvp.autoLoadConfig
			for c = 1, nComp do alc[c] = 1 end
			tvp.autoLoadConfig = alc
			tvp.part = part

			nUnits = nUnits + 1
			config.vehicles[nUnits] = tvp
		end

		-- REQUIRED: group sizes must sum to the number of vehicles. Leaving
		-- this empty is what hung the sim last time.
		local grp = config.vehicleGroups
		grp[1] = nUnits
		config.vehicleGroups = grp
	end)
	if not built or nUnits ~= #models then
		log(string.format(
			"veh replay: REFUSING to send incomplete config (%d/%d units) for %s"
			.. " -- an incomplete one aborts the process, so skipping",
			nUnits, #models, tostring(mstr)))
		return
	end
	-- our own buyVehicle re-enters the native hook; don't ship it back
	suppressBuyUntil = ticks + 30
	local pid
	pcall(function() pid = api.engine.util.getPlayer() end)
	-- make + send both inside the pcall: make threw on the bad config type and,
	-- being outside, took the whole replay down without reaching any log line
	local rok, rerr = pcall(function()
		local cmd = api.cmd.make.buyVehicle(pid, depot, config)
		api.cmd.sendCommand(cmd, function(result, success)
			log("veh replay result: success=" .. tostring(success))
			if success and result then known[result] = "remote"; knownVehs[result] = true end
		end)
	end)
	log(string.format("veh replay sent: %d unit(s) ok=%s%s", nUnits, tostring(rok),
		rok and "" or (" err=" .. tostring(rerr))))
end

-- ---------- time sync ----------
-- Host-authoritative clock. The host ("a") broadcasts its speed and game time;
-- the client follows. Either side may *request* a speed change (so both players
-- can pause / fast-forward) -- the request goes to the host, which adopts it and
-- re-broadcasts, so there is exactly one writer and no tug-of-war.
--
-- Speed is the thing that actually matters: getGameSpeed() is a multiplier with
-- 0 == paused, and if the two sims run at different speeds their clocks diverge
-- without limit. Date drift is only a backstop, corrected in whole days because
-- setDate() has no finer granularity.
local TIME_AUTHORITY     = "a"
local TIME_SEND_EVERY    = 30   -- ticks between host broadcasts
-- setDate has whole-day granularity, so 1 is the smallest correctable drift.
-- Was 2, which left a 1-day gap sitting there uncorrected. Note this is a
-- band-aid: the two sims drift because commands are applied on arrival rather
-- than at an agreed tick, and only lockstep scheduling fixes that properly.
local DRIFT_CORRECT_DAYS = 1
local timeSyncEnabled    = true
local lastKnownSpeed     = nil  -- speed as of our last send/apply
local lastTimeSend       = 0
local lastTimePayload    = nil  -- suppress verbatim-identical heartbeats
local lastTimeForced     = 0
local pendingSpeed       = nil  -- speed we asked for, engine hasn't applied yet
local pendingSince       = 0
local PENDING_TIMEOUT    = 30   -- 3 s at 10 Hz; don't wedge if it never lands

local function isTimeAuthority() return INSTANCE == TIME_AUTHORITY end

local function gameSpeed()
	local s
	pcall(function() s = game.interface.getGameSpeed() end)
	return s
end

local function gameTimeSecs()
	local t
	pcall(function() t = game.interface.getGameTime().time end)
	return t
end

local function secondsPerDay()
	local m
	pcall(function() m = game.interface.getMillisPerDay() end)
	if not m or m == 0 then return nil end
	return m / 1000
end

local function applySpeed(s)
	local cur = gameSpeed()
	if cur == s then lastKnownSpeed = s pendingSpeed = nil return end
	local ok, err = pcall(function()
		api.cmd.sendCommand(api.cmd.make.setGameSpeed(s))
	end)
	-- setGameSpeed is ASYNC: for a tick or two getGameSpeed() still reports the
	-- OLD value. Without tracking that, the next poll saw cur ~= lastKnownSpeed,
	-- read it as the user changing speed locally, and asked the host to go back
	-- -- so pausing one instance paused the other, which then un-paused the
	-- first. Hold reporting until the engine actually settles on what we asked
	-- for.
	lastKnownSpeed = s
	pendingSpeed = s
	pendingSince = ticks
	log("time: speed " .. tostring(cur) .. " -> " .. tostring(s)
		.. " ok=" .. tostring(ok) .. (ok and "" or (" err=" .. tostring(err))))
end

local function pollTime()
	if not timeSyncEnabled or not INSTANCE then return end
	local cur = gameSpeed()
	if cur == nil then return end
	if lastKnownSpeed == nil then lastKnownSpeed = cur end

	-- a speed change we requested is still settling: anything we observe now is
	-- our own doing, not the user's, so do not report it
	if pendingSpeed ~= nil then
		if cur == pendingSpeed then
			pendingSpeed = nil
		elseif ticks - pendingSince < PENDING_TIMEOUT then
			return
		else
			log("time: speed change to " .. tostring(pendingSpeed) .. " never landed")
			pendingSpeed = nil
			lastKnownSpeed = cur
		end
	end

	local changed = (cur ~= lastKnownSpeed)

	if isTimeAuthority() then
		if changed or (ticks - lastTimeSend) >= TIME_SEND_EVERY then
			lastKnownSpeed = cur
			lastTimeSend = ticks
			local t = gameTimeSecs()
			local spd = secondsPerDay()
			if t and spd then
				local payload = string.format(
					"TIME speed=%s t=%.3f secday=%.3f", tostring(cur), t, spd)
				-- While paused, speed AND game time are both frozen, so the
				-- heartbeat repeats verbatim forever -- 15 of 19 lines in the
				-- first live test were identical. Send only when something
				-- actually changed, with a slow keepalive so a peer that joins
				-- during a pause still learns the clock.
				local stale = (ticks - lastTimeForced) >= TIME_SEND_EVERY * 10
				if payload ~= lastTimePayload or stale then
					appendLine(CAPTURE_FILE, payload)
					lastTimePayload = payload
					if stale then lastTimeForced = ticks end
				end
			end
		end
	elseif changed then
		-- client: ask the host to change speed rather than doing it locally
		lastKnownSpeed = cur
		appendLine(CAPTURE_FILE, "SPEEDREQ speed=" .. tostring(cur))
		log("time: asked host for speed " .. tostring(cur))
	end
end

local function replayTime(line)
	if not timeSyncEnabled then return end
	if isTimeAuthority() then return end   -- the host does not follow anyone
	local speed  = tonumber(line:match("speed=([%-%d.]+)"))
	local hostT  = tonumber(line:match(" t=([%-%d.]+)"))
	local secday = tonumber(line:match("secday=([%-%d.]+)"))
	if speed then applySpeed(speed) end

	local localT = gameTimeSecs()
	if not (hostT and localT and secday and secday > 0) then return end
	local driftDays = (hostT - localT) / secday
	if driftDays >= DRIFT_CORRECT_DAYS then
		local ok, err = pcall(function()
			local d = game.interface.getDateFromNowPlusOffsetDays(math.floor(driftDays))
			game.interface.setDate(table.unpack(d))
		end)
		log(string.format("time: %.2f days behind host -> corrected ok=%s%s",
			driftDays, tostring(ok), ok and "" or (" err=" .. tostring(err))))
	elseif driftDays <= -DRIFT_CORRECT_DAYS then
		-- setDate cannot safely run the clock backwards, so we only report this.
		-- Matching speed should stop it growing; if it persists it means the two
		-- sims are advancing at genuinely different rates and needs a real fix.
		if ticks % 600 == 0 then
			log(string.format("time: %.2f days AHEAD of host (not corrected)", -driftDays))
		end
	end
end

local function replaySpeedReq(line)
	if not timeSyncEnabled then return end
	if not isTimeAuthority() then return end   -- only the host acts on requests
	local speed = tonumber(line:match("speed=([%-%d.]+)"))
	if speed then
		log("time: host adopting requested speed " .. tostring(speed))
		applySpeed(speed)
	end
end

local function replayDemolish(line)
	local x, y, z = line:match("p=([-%d.]+),([-%d.]+),([-%d.]+)")
	x, y = tonumber(x), tonumber(y)
	if not x or not y then return end
	-- mark first: bulldozing makes the construction vanish here too, and the
	-- capture sweep would otherwise report that as a fresh local demolition
	remoteDemolished[demolishKey(x, y)] = true
	-- Only bulldoze something that is both close AND the same construction type.
	-- Position alone is not enough of a guarantee to destroy a player's build.
	local wantFile = line:match("file=(%S+)")
	local best, bestD
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false })
		for _, id in pairs(ents) do
			-- entityAlive first: getComponent on a retired id is a native crash,
			-- and these scans run right after a replay has been retiring things.
			local co = entityAlive(id)
				and api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION) or nil
			if co and co.transf then
				local dx, dy = co.transf[13] - x, co.transf[14] - y
				local d = dx * dx + dy * dy
				local fileOk = (not wantFile) or wantFile == "?" or co.fileName == wantFile
				if d < 100.0 and fileOk and (not bestD or d < bestD) then
					best, bestD = id, d
				end
			end
		end
	end)
	if not best then
		log(string.format("demolish replay: no %s within 10m of %.1f,%.1f -- ignoring",
			tostring(wantFile), x, y))
		return
	end
	conPos[best] = nil
	local ok, err = pcall(game.interface.bulldoze, best)
	log(string.format("demolish replay id=%d ok=%s%s", best, tostring(ok),
		ok and "" or (" err=" .. tostring(err))))
end

-- ---------- station / construction edit replay ----------
--
-- upgradeConstruction(id, fileName, params) is the same call the shipped
-- constructionupgrader.lua makes. Matched by position AND fileName, never id:
-- construction ids depend on build order and are not comparable across
-- instances, and rewriting the wrong building's params is destructive.
local function replayConMod(line)
	if not CONMOD_ENABLED then
		log("station edit replay: CONMOD disabled -- ignoring")
		return
	end
	local x, y = line:match("p=([-%d.]+),([-%d.]+)")
	x, y = tonumber(x), tonumber(y)
	local wantFile = line:match("file=(%S+)")
	local pstr = line:match("params=(.+)$")
	if not (x and y and wantFile and pstr) then return end

	local params = deserParams(pstr)
	if type(params) ~= "table" then
		log("station edit replay: unparseable params -- ignoring")
		return
	end
	-- Replaying with a used seed drives errorState critical, which is a fatal
	-- assert rather than a rejected proposal. Same rule as the build path.
	params.seed = nil

	-- Mark before acting. Three separate capture paths are about to see the
	-- effects of this call and all three must stay quiet:
	--   params changed   -> markRemoteConMod  (an id that survives the edit)
	--   old id vanished  -> markConRewrite    (upgradeConstruction REPLACES)
	--   new id appeared  -> markConRewrite
	-- Only the first was marked before, so the replacement looked like a
	-- demolish-plus-rebuild and was shipped back at the peer.
	-- The signature is taken from the params we are about to apply, so the echo
	-- guard recognises exactly this edit coming back and nothing else. Computed
	-- from `params` rather than by reading the entity afterwards, because
	-- upgradeConstruction retires the id and touching it is a native crash.
	markRemoteConMod(demolishKey(x, y), serStable(params))
	markConRewrite(x, y)

	local best, bestD
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false })
		for _, id in pairs(ents) do
			-- entityAlive first: getComponent on a retired id is a native crash,
			-- and these scans run right after a replay has been retiring things.
			local co = entityAlive(id)
				and api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION) or nil
			if co and co.fileName == wantFile and co.transf then
				local dx, dy = co.transf[13] - x, co.transf[14] - y
				local d = dx * dx + dy * dy
				if d < 100 and (not bestD or d < bestD) then best, bestD = id, d end
			end
		end
	end)
	if not best then
		log(string.format("station edit replay: no %s within 10m of %.1f,%.1f -- ignoring",
			wantFile, x, y))
		return
	end

	local ok, err = pcall(game.interface.upgradeConstruction, best, wantFile, params)
	log(string.format("station edit replay id=%d ok=%s%s", best, tostring(ok),
		ok and "" or (" err=" .. tostring(err))))

	-- DIAGNOSTIC: on failure, re-upgrade the construction with its OWN current
	-- params. That is a no-op edit, so it MUST succeed on any construction the
	-- engine is willing to upgrade at all.
	--
	-- It separates the only two explanations for "internal error", which need
	-- opposite fixes:
	--   self-upgrade ok    -> our wire params are bad for this construction
	--                         (serialization round trip, modules table, depth)
	--   self-upgrade fails -> the construction itself cannot be upgraded here,
	--                         regardless of params. Likely because WE created it
	--                         with buildConstruction, which does not do the
	--                         network integration the interactive tool performs.
	--                         Then the fix belongs in the BUILD replay, not here.
	-- Modular rail stations fail every time while depots succeed, so this is the
	-- fork worth measuring rather than reasoning about.
	if not ok and entityAlive(best) then
		local sok, serr = pcall(function()
			local e = game.interface.getEntity(best)
			if not (e and e.params) then return "no params" end
			local own = {}
			for kk, vv in pairs(e.params) do if kk ~= "seed" then own[kk] = vv end end
			local ok2, err2 = pcall(game.interface.upgradeConstruction, best, wantFile, own)
			return "ok=" .. tostring(ok2) .. (ok2 and "" or (" err=" .. tostring(err2)))
		end)
		log("  probe: self-upgrade with its OWN params -> " .. tostring(sok and serr or serr))
	end
	if ok then
		-- upgradeConstruction REPLACES: this call retires `best`. So do NOT
		-- touch it again -- not getEntity, not getComponent. The previous code
		-- called game.interface.getEntity(best) on the very next line to
		-- re-baseline its params, and that is the line that hard-crashed the
		-- joiner: a native crash in mpbridge.lua_update() logged immediately
		-- after "station edit replay id=281674 ok=true". pcall does not help;
		-- the process dies.
		--
		-- Unconditional, not guarded by entityAlive, because the guard depends
		-- on an API probe that has already been wrong once. Dropping a live id
		-- is harmless: the next sweep re-adopts it from getEntities, and the
		-- BUILD that would otherwise be emitted is suppressed by conRewrite,
		-- which is marked at this position above.
		conEditable[best] = nil
		conParams[best] = nil
		conPos[best] = nil
		known[best] = nil
	end
end

-- ---------- line replay ----------

local function findStationGroupNear(x, y)
	local best, bestD
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "STATION_GROUP", includeData = false })
		for _, sg in pairs(ents) do
			local sx, sy = stationGroupPos(sg)
			if sx then
				local dx, dy = sx - x, sy - y
				local d = dx * dx + dy * dy
				if d < 400 and (not bestD or d < bestD) then best, bestD = sg, d end
			end
		end
	end)
	return best
end

local function replayLineCreate(line)
	local wait = tonumber(line:match(" wait=(%d+)")) or 180
	local stopsStr = line:match(" stops=(%S+)")
	local name = line:match(" name=(.+)$") or "Line"
	if not stopsStr then return end

	-- mark before acting: the line we are about to create shows up in our own
	-- sweep and would otherwise be shipped straight back
	markRemoteLine(stopsStr)

	local sgs = {}
	for pair in stopsStr:gmatch("[^;]+") do
		local x, y = pair:match("([-%d.]+),([-%d.]+)")
		x, y = tonumber(x), tonumber(y)
		if not x then return end
		local sg = findStationGroupNear(x, y)
		if not sg then
			log(string.format("line replay: no station group within 20m of %.1f,%.1f -- ignoring line", x, y))
			return
		end
		sgs[#sgs + 1] = sg
	end
	if #sgs < 2 then
		log("line replay: needs at least 2 stops, got " .. #sgs)
		return
	end

	local lineObj = api.type.Line.new()
	local built, berr = pcall(function()
		lineObj.waitingTime = wait
		for i, sg in ipairs(sgs) do
			-- MEASURED off a real line; the docs do not describe Line.Stop.
			-- Note the type is api.type.Line.Stop, nested -- there is no
			-- api.type.LineStop.
			local s = api.type.Line.Stop.new()
			s.stationGroup = sg
			s.station = 0
			s.terminal = 0
			s.loadMode = 0
			s.minWaitingTime = 0
			s.maxWaitingTime = wait
			lineObj.stops[i] = s
		end
	end)
	if not built then
		log("line replay: could not build Line object: " .. tostring(berr))
		return
	end

	local rok, rerr = pcall(function()
		api.cmd.sendCommand(
			api.cmd.make.createLine(name, api.type.Vec3f.new(0.9, 0.2, 0.2),
				api.engine.util.getPlayer(), lineObj),
			function(res, success)
				log(string.format("line replay '%s' (%d stops): success=%s",
					tostring(name), #sgs, tostring(success)))
			end)
	end)
	if not rok then log("line replay error: " .. tostring(rerr)) end
end

-- ---------- edge removal replay ----------

local function nodeEdgeMap(kind)
	local m
	pcall(function()
		if kind == "rail" then
			m = api.engine.system.streetSystem.getNode2TrackEdgeMap()
		else
			m = api.engine.system.streetSystem.getNode2StreetEdgeMap()
		end
	end)
	return m or {}
end

-- Edge ids are NOT comparable across instances (they depend on build order), so
-- a removal has to be matched by geometry, the same way a demolish is matched
-- by position and file name. Both orientations are tried: node0/node1 order is
-- not guaranteed to agree between the two games.
local EDGE_MATCH_EPS2 = 2.0 * 2.0
local function findEdgeByGeom(kind, p0x, p0y, p1x, p1y)
	local best, bestD, seen = nil, nil, {}
	for _, list in pairs(nodeEdgeMap(kind)) do
		for _, eid in ipairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local a, b = readEdgeGeom(eid)
				if a and b then
					local ax, ay = a.x or a[1], a.y or a[2]
					local bx, by = b.x or b[1], b.y or b[2]
					local d1 = (ax - p0x)^2 + (ay - p0y)^2 + (bx - p1x)^2 + (by - p1y)^2
					local d2 = (ax - p1x)^2 + (ay - p1y)^2 + (bx - p0x)^2 + (by - p0y)^2
					local d = (d1 < d2) and d1 or d2
					if d < EDGE_MATCH_EPS2 * 2 and (not bestD or d < bestD) then
						best, bestD = eid, d
					end
				end
			end
		end
	end
	return best
end

local function replayEdgeDel(line)
	if not EDGEDEL_ENABLED then
		log("edge removal replay: EDGEDEL disabled -- ignoring (nothing deleted)")
		return
	end
	local kind = line:match("kind=(%S+)") or "road"
	local p0x, p0y = line:match("p0=([-%d.]+),([-%d.]+)")
	local p1x, p1y = line:match("p1=([-%d.]+),([-%d.]+)")
	p0x, p0y = tonumber(p0x), tonumber(p0y)
	p1x, p1y = tonumber(p1x), tonumber(p1y)
	if not (p0x and p0y and p1x and p1y) then return end

	-- Mark before acting. Removing the edge locally makes it vanish from our own
	-- sweep too, which would otherwise be reported as a fresh local demolition
	-- and sent straight back to the peer.
	markRemoteEdgeDel(edgeSig(kind, p0x, p0y, p1x, p1y))

	local eid = findEdgeByGeom(kind, p0x, p0y, p1x, p1y)
	if not eid then
		log(string.format("edge removal replay: no %s edge near %.1f,%.1f -> %.1f,%.1f -- ignoring",
			kind, p0x, p0y, p1x, p1y))
		return
	end

	local sp = api.type.SimpleProposal.new()
	sp.streetProposal.edgesToRemove[1] = eid

	-- A node with no remaining edges is invalid geometry -- the game's own
	-- bulldozer takes them out along with the edge, so do the same for any
	-- endpoint this edge was the last user of.
	local m = nodeEdgeMap(kind)
	local comp = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
	if comp then
		local nRem, done = 0, {}
		for _, nid in ipairs({ comp.node0, comp.node1 }) do
			if nid and not done[nid] then
				done[nid] = true
				local n = 0
				if m[nid] then for _ in pairs(m[nid]) do n = n + 1 end end
				if n <= 1 then
					nRem = nRem + 1
					sp.streetProposal.nodesToRemove[nRem] = nid
				end
			end
		end
	end

	local rok, rerr = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, nil, false),
			function(res, success)
				log("edge removal replay: success=" .. tostring(success))
				if success then edgeState[eid] = nil end
			end)
	end)
	if not rok then log("edge removal replay error: " .. tostring(rerr)) end
end

-- ---------- edge upgrade replay ----------
--
-- There is no "update" channel in a proposal: the only bound fields are
-- edgesToAdd / edgesToRemove / nodesToAdd / nodesToRemove (verified against the
-- symbol names in the shipped binary). An upgrade is therefore expressed as a
-- remove plus an add in ONE proposal, reusing the existing node ids so the
-- edge's connections to its neighbours survive.
local function replayEdgeMod(line)
	local kind = line:match("kind=(%S+)") or "road"
	local p0x, p0y = line:match("p0=([-%d.]+),([-%d.]+)")
	local p1x, p1y = line:match("p1=([-%d.]+),([-%d.]+)")
	p0x, p0y = tonumber(p0x), tonumber(p0y)
	p1x, p1y = tonumber(p1x), tonumber(p1y)
	if not (p0x and p0y and p1x and p1y) then return end

	markRemoteEdgeMod(edgeSig(kind, p0x, p0y, p1x, p1y))

	local eid = findEdgeByGeom(kind, p0x, p0y, p1x, p1y)
	if not eid then
		log(string.format("edge upgrade replay: no %s edge near %.1f,%.1f -> %.1f,%.1f -- ignoring",
			kind, p0x, p0y, p1x, p1y))
		return
	end
	local old = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
	if not old then return end

	local typeId = tonumber(line:match(" type=(-?%d+)")) or -1
	local catenary = (line:match(" cat=(%d)") == "1")
	local hasBus   = (line:match(" bus=(%d)") == "1")
	local tramType = tonumber(line:match(" tram=(%d+)")) or 0

	local function v3(p)
		if not p then return api.type.Vec3f.new(0, 0, 0) end
		return api.type.Vec3f.new(p.x or p[1] or 0, p.y or p[2] or 0, p.z or p[3] or 0)
	end

	local sp = api.type.SimpleProposal.new()
	sp.streetProposal.edgesToRemove[1] = eid

	local e = api.type.SegmentAndEntity.new()
	e.entity = -100000 - ticks
	e.comp.node0 = old.node0          -- existing node ids: keep the connections
	e.comp.node1 = old.node1
	e.comp.tangent0 = v3(old.tangent0)
	e.comp.tangent1 = v3(old.tangent1)
	e.comp.type = 0
	e.comp.typeIndex = (kind == "rail") and -1 or 0
	e.type = (kind == "rail") and 1 or 0   -- see the note in replayEdge
	if kind == "road" then
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = typeId
		e.streetEdge.hasBus = hasBus
		e.streetEdge.tramTrackType = tramType
	else
		e.trackEdge = api.type.BaseEdgeTrack.new()
		e.trackEdge.trackType = typeId
		e.trackEdge.catenary = catenary
		-- mandatory for validation even on a track edge; see replayEdge
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = getAnyStreetTypeId()
	end
	sp.streetProposal.edgesToAdd[1] = e

	local rok, rerr = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, nil, false),
			function(res, success)
				log("edge upgrade replay: success=" .. tostring(success))
				if success then edgeState[eid] = nil end
			end)
	end)
	if not rok then log("edge upgrade replay error: " .. tostring(rerr)) end
end

local function replayLine(line)
	lastReplayTick = ticks
	if line:sub(1, 4) == "VEH " then return replayVeh(line) end
	if line:sub(1, 9) == "DEMOLISH " then return replayDemolish(line) end
	-- EDGEDEL/EDGEMOD must be tested before EDGE: they share the prefix but not
	-- the trailing space, so "EDGE " does not match them -- still, keeping them
	-- adjacent makes the ordering requirement obvious to the next reader.
	if line:sub(1, 8) == "EDGEDEL " then return replayEdgeDel(line) end
	if line:sub(1, 8) == "EDGEMOD " then return replayEdgeMod(line) end
	if line:sub(1, 5) == "EDGE " then return replayEdge(line) end
	if line:sub(1, 5) == "TIME " then return replayTime(line) end
	if line:sub(1, 9) == "SPEEDREQ " then return replaySpeedReq(line) end
	if line:sub(1, 7) == "CONMOD " then return replayConMod(line) end
	if line:sub(1, 5) == "LINE " then return replayLineCreate(line) end
	local fileName, tstr, pstr = line:match("^BUILD file=(%S+) t=(%S+) params=(.+)$")
	if not fileName then
		fileName, tstr = line:match("^BUILD file=(%S+) t=(.+)$")
	end
	if not fileName then return end
	local t = {}
	for n in tstr:gmatch("[^,]+") do t[#t + 1] = tonumber(n) end
	if #t ~= 16 then return end
	local params = {}
	if pstr then
		do
			local v = deserParams(pstr)
			if v then params = v end
		end
	end
	params.seed = nil  -- replaying with a used seed -> critical errorState -> fatal assert
	-- the construction we are about to build will spawn its own access edges;
	-- mark the spot so pollEdges doesn't capture and re-ship them
	noteConstruction(t[13], t[14])
	local rok, newId = pcall(game.interface.buildConstruction, fileName, params, t)
	-- buildConstruction takes no player argument, so it produces an UNOWNED
	-- construction: it renders, but the UI will not let you click it and a
	-- station is not usable. Verified by component diff -- the builder's copy
	-- had PLAYER_OWNED, the replayed copy did not. setPlayer adds it.
	if rok and newId then
		local ok2, err2 = pcall(function()
			game.interface.setPlayer(newId, api.engine.util.getPlayer())
		end)
		if not ok2 then log("setPlayer failed on " .. tostring(newId) .. ": " .. tostring(err2)) end
	end
	log("replay " .. fileName .. ": ok=" .. tostring(rok) .. " id=" .. tostring(newId))
	if rok and newId then
		known[newId] = "remote"
		-- Mark WHERE this construction landed, so the edge sweep can recognise
		-- the track/road it is about to spawn as construction-made rather than
		-- a fresh local build. See nearRecentConstruction in pollEdges.
		--
		-- An earlier version of this SCANNED every edge in both node->edge maps
		-- and called getComponent on each id, right after the replay had mutated
		-- the world. That hard-CRASHED the joiner inside mpbridge.lua_update()
		-- -- a native crash, which pcall cannot catch -- and took the whole game
		-- script down with it. Never walk the edge maps here; note the position
		-- and let the sweep do an O(1) proximity test instead.
		noteConstruction(t[13], t[14])
	end
end

local function pollEvents()
	-- "rb", not "r": in text mode on Windows f:seek("end") reports the on-disk
	-- byte count while f:read strips the CR from every CRLF, so #complete came
	-- out one byte short per line and eventsOffset drifted backwards forever.
	local f = io.open(EVENTS_FILE, "rb")
	if not f then
		if ticks % 60 == 0 then log("pollEvents: cannot open " .. tostring(EVENTS_FILE)) end
		return
	end
	local size = f:seek("end")
	if eventsOffset < 0 or size < eventsOffset then
		eventsOffset = size  -- first poll (skip stale history) or bridge truncated
	end
	f:seek("set", eventsOffset)
	local chunk = f:read("*a") or ""
	local lastNl = chunk:find("\n[^\n]*$")
	local complete = lastNl and chunk:sub(1, lastNl) or ""
	local consumed = #complete
	-- log unconditionally: the previous version only logged when size > 0, so
	-- the "events file is empty / wrong file" case -- the one actually worth
	-- diagnosing -- produced no output at all.
	if ticks % 60 == 0 then
		log(string.format("pollEvents: size=%d offset=%d consumed=%d retry=%d file=%s",
			size, eventsOffset, consumed, #retryQueue, EVENTS_FILE))
	end
	eventsOffset = eventsOffset + consumed
	f:close()
	-- one malformed event must not abort the rest of the batch (and the offset
	-- is already advanced above, so a thrower can't wedge us in a replay loop)
	for line in complete:gmatch("([^\n]+)\n") do
		local rok, rerr = pcall(replayLine, (line:gsub("\r+$", "")))
		if not rok then
			log("replay error: " .. tostring(rerr) .. " | " .. line:sub(1, 70))
		end
	end
	-- drain edge retries (nodes committed since)
	if #retryQueue > 0 then
		local q = retryQueue
		retryQueue = {}
		for _, line in ipairs(q) do
			local rok, rerr = pcall(replayLine, line)
			if not rok then log("retry replay error: " .. tostring(rerr)) end
		end
	end
end

-- ---------- dev harness: run recon/m4/harness.lua on change ----------
local HARNESS_FILE = "C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/3710243057/recon/m4/harness.lua"
local harnessSrc = nil
local function runHarness()
	local f = io.open(HARNESS_FILE, "r")
	if not f then return end
	local src = f:read("*a")
	f:close()
	-- compare content, not length: the old check keyed on #src alone, so any
	-- edit that happened to preserve the file size never re-ran
	if src == harnessSrc then return end
	harnessSrc = src
	local chunk, cerr = load(src, "harness")
	if not chunk then log("harness load error: " .. tostring(cerr)) return end
	local ok, err = pcall(chunk)
	log("harness: ok=" .. tostring(ok) .. (ok and "" or (" err=" .. tostring(err))))
end

function data()
	return {
		init = function()
			log("mp bridge mod live")
			-- must match the bridge dll's "[m5] data dir" log line, else the
			-- two halves are reading and writing different files
			log("base dir: " .. BASE .. "  [" .. tostring(BASE_SOURCE) .. "]")
			-- Checked here too, not only in update(): if lockstep is in this
			-- Lua state its global is already set, so the very first log line
			-- of the session can say so.
			if channelConflict() then reportConflict() end
			-- NOTE: the entityExists probe deliberately does NOT live here.
			-- init() only runs for a new game; continuing a save calls load(),
			-- so anything set up here is absent in every session this project
			-- actually runs. It is probed lazily in entityAlive instead.
		end,

		update = function()
			ticks = ticks + 1
			-- keep re-reading identity, not only until the first success, so a
			-- mis-injected bridge shows up in the log instead of silently
			-- pointing us at the wrong files forever
			if ticks % 60 == 0 then detectInstance() end
			if not INSTANCE then return end
			-- Before ANY poll. Both capture and replay touch files lockstep
			-- also owns, so once the two are running together neither reading
			-- nor writing is safe -- see channelConflict.
			if ticks % 30 == 0 then channelConflict() end
			if conflict then reportConflict() return end
			-- update() is 10 Hz (measured). Capture polls do full-world
			-- getEntities scans, which are expensive on a large save, so they
			-- stay at 2 Hz. Replay is just a file read at a byte offset, so it
			-- runs at 5 Hz -- halving inbound latency costs nothing.
			-- Constructions are polled MORE OFTEN than the rest, deliberately.
			--
			-- Two reasons. Latency: update() only hits 10 Hz while the sim runs
			-- (measured ~4 Hz here), so a %5 gate meant station edits were noticed
			-- at well under 1 Hz -- visibly laggy. And ordering: a station edit
			-- must reach the wire BEFORE the platform edges it creates, or the
			-- peer builds bare track and then refuses the upgrade. Polling
			-- constructions at least as often as edges guarantees that, instead of
			-- relying on the two happening to land in the same sweep.
			--
			-- Affordable because the edit scan walks conEditable (a few hundred),
			-- not the ~5,350 constructions on the map -- raising it to a full
			-- per-sweep scan left the sim tick rate unchanged at +39-41 in 10 s.
			if ticks % 2 == 0 then
				pollConstructions()
			end
			if ticks % 5 == 0 then
				pollVehicles()
				pollEdges()
				pollLines()
				pollTime()
			end
			-- purchases come from the native hook via a file; cheap to check
			if ticks % 2 == 0 then pollBuys() end
			if ticks % 2 == 0 then
				pollEvents()
			end
			if ticks % 60 == 0 then runHarness() end

			-- Liveness beacon for the harness, and it must come from HERE --
			-- inside the mod's own update() -- because nothing else proves this
			-- script is still running. The native hook's "[simhook] ticks=" keeps
			-- counting even when the Lua game script is dead, so the harness
			-- watched a healthy-looking sim while replication was silently off.
			--
			-- eventsOffset rides along because a joiner can be alive and still
			-- deaf: if the host is shipping lines and this offset does not move,
			-- the fault is the events file, not the script.
			-- Every 100 ticks, not 300. update() is 10 Hz only while the sim is
			-- RUNNING; with the game paused -- which is how most of this project
			-- is tested -- it barely advances. A 120 s scenario run reached tick
			-- ~120 on the joiner, so a 300-tick beacon never fired once and the
			-- harness called a perfectly healthy instance dead.
			if ticks % 100 == 0 then
				-- conEditable size rides along because the whole point of that
				-- set is that it is small enough to diff every sweep. If it ever
				-- approaches CONMOD_SCAN_PER_SWEEP the scan is round-robining
				-- again and station edits will lag behind their own edges.
				local nEd = 0
				for _ in pairs(conEditable) do nEd = nEd + 1 end
				log("alive ticks=" .. ticks .. " eventsOffset=" .. tostring(eventsOffset)
					.. " editable=" .. nEd .. " deadSkips=" .. deadSkips)
				deadSkips = 0
				if nEd > CONMOD_SCAN_PER_SWEEP then
					log(string.format("!! editable=%d exceeds CONMOD_SCAN_PER_SWEEP=%d --"
						.. " the edit scan is round-robining again, so station edits will"
						.. " ship AFTER their own edges and the peer will build bare track",
						nEd, CONMOD_SCAN_PER_SWEEP))
				end
			end
		end,

		save = function() return {} end,
		load = function(s) end,
	}
end
