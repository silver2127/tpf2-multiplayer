-- mp/cons.lua -- constructions: hybrid replication, station edits, edge demolish, capture polls
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.cons")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
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
function CM.ser(v, depth)
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
			local inner = CM.ser(v[k], depth + 1)
			if inner then parts[#parts + 1] = key .. "=" .. inner end
		end
		return "{" .. table.concat(parts, ",") .. "}"
	end
	return nil   -- functions/userdata: omit rather than substitute a wrong type
end

-- Construction-params DIFF, for strict module edits (CONUP).
--
-- The module builder builds each proposal from the ENTITY as it stands at
-- click time. Under strict the entity does not have the PREVIOUS click's
-- module yet (it lands at the stamp, ~0.6 s later), so the next click's full
-- param set lacks it -- and when both replay, the second overwrites the first
-- away: "only every other platform gets filled" (2026-09-08; the log showed
-- every fast pair of CONUPs carrying the SAME module count). A click's true
-- intent is one slot changed. Ship THAT: the difference between the entity's
-- params at capture and the proposal's, applied onto whatever the entity
-- holds at the stamp. Diffs compose; snapshots do not.
--   top  = top-level keys set/changed (non-module)   topdel = removed
--   mset = modules slot -> record set/changed        mdel   = removed slots
-- Records compare by their canonical serialisation (ser sorts keys).
function CM.conDiff(p0, p1)
	local d = { top = {}, topdel = {}, mset = {}, mdel = {} }
	p0 = p0 or {}; p1 = p1 or {}
	for k, v in pairs(p1) do
		if k ~= "modules" and k ~= "seed" and CM.ser(p0[k]) ~= CM.ser(v) then d.top[k] = v end
	end
	for k in pairs(p0) do
		if k ~= "modules" and k ~= "seed" and p1[k] == nil then d.topdel[k] = true end
	end
	local m0, m1 = p0.modules or {}, p1.modules or {}
	for slot, rec in pairs(m1) do
		if CM.ser(m0[slot]) ~= CM.ser(rec) then d.mset[slot] = rec end
	end
	for slot in pairs(m0) do
		if m1[slot] == nil then d.mdel[slot] = true end
	end
	return d
end

function CM.conApplyDiff(cur, d)
	cur = cur or {}
	for k, v in pairs(d.top or {}) do cur[k] = v end
	for k in pairs(d.topdel or {}) do cur[k] = nil end
	cur.modules = cur.modules or {}
	for slot, rec in pairs(d.mset or {}) do cur.modules[slot] = rec end
	for slot in pairs(d.mdel or {}) do cur.modules[slot] = nil end
	return cur
end

function CM.deserParams(pstr)
	if not pstr or pstr == "" then return nil end
	local chunk = load("return " .. pstr, "params")
	if not chunk then return nil end
	local ok, v = pcall(chunk)
	if ok and type(v) == "table" then return v end
	return nil
end

local knownCons    = {}      -- construction ids already seen (or primed)
local ownershipPending = {}  -- buildable con id -> polls waited for PLAYER_OWNED
CM.consPrimed   = false   -- first poll only records what exists
-- Names travel percent-escaped: the wire is whitespace-tokenised and a
-- construction name ("Neckargemünd Train depot") has spaces.
function CM.escName(s)
	return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end
function CM.unescName(s)
	return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

CM.expectedCons = {}      -- posKey -> true: our own replay is about to land here
-- Pairing buffers for CONX: the hook's ROADC street payload arrives within a
-- tick of the placement; the construction itself is noticed by a poll up to
-- K.CON_POLL_EVERY ticks later. Whichever comes first waits for the other.
CM.pendingRoadc = {}      -- { at, posOf, adds, rms, spos, etype, stype, ttype, cat }
CM.pendingCons  = {}      -- { at, file, t, params, x, y }

-- Horizontal only, same reasoning as node matching: the engine settles z.
function CM.conKey(x, y) return string.format("%.1f/%.1f", x, y) end

local function execConP(c)
	if c.origin == K.INSTANCE then
		log(string.format("CONP seq=%d: originator already built it locally, skipping", c.seq))
		return
	end
	local ok, err = pcall(function()
		local t = {}
		for tok in tostring(c.t or ""):gmatch("[^,]+") do t[#t + 1] = tonumber(tok) end
		if #t ~= 16 then log("CONP: bad transf, " .. #t .. " numbers"); return end
		local params = CM.deserParams(c.params) or {}
		-- A used seed drives errorState critical, which is a fatal assert rather
		-- than a rejected build. Strip it; the engine assigns a fresh one.
		params.seed = nil
		local key = CM.conKey(t[13], t[14])
		CM.expectedCons[key] = true
		if c.company and CM.cmMode == "companies" then CM.cmExpectedCompany[key] = tonumber(c.company); CM.cmEnsure(); CM.cmExpectedBal0[key] = CM.cmBalance(CM.cmCompanyPid[CM.cmMyCompany])
			CM.cmLog(string.format("CM: CONP bal0 snapshot key=%s mePid=%s bal0=%s", key, tostring(CM.cmCompanyPid[CM.cmMyCompany]), tostring(CM.cmExpectedBal0[key]))) end
		local built, newId = pcall(game.interface.buildConstruction, c.file, params, t)
		if built and newId then
			-- buildConstruction takes no player, so the result is unowned and the
			-- UI will not let you click it. setPlayer fixes that (mp_bridge).
			pcall(function() game.interface.setPlayer(newId, api.engine.util.getPlayer()) end)
		else
			CM.expectedCons[key] = nil
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
CM.consByKey    = {}   -- conKey -> { id=, file=, params=<ser string> }
local expectedEdit = {}   -- conKey -> true: our own replayed edit is about to replace the entity
CM.primeQueue   = {}   -- ids from the first poll, classified a few per tick

local function findConNear(file, x, y, maxDist)
	local best, bestD
	for key, rec in pairs(CM.consByKey) do
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

function CM.execConU(c)
	-- Poll-detected (legacy): the originator already applied it natively.
	-- STRICT (strict=1, from a CONUP the slice cancelled): nothing changed here
	-- yet, so the originator upgrades at the stamp like everyone else.
	if c.origin == K.INSTANCE and tonumber(c.strict or 0) ~= 1 then
		log(string.format("CONU seq=%d: originator already applied it, skipping", c.seq))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("CONU seq=%d: STRICT -- originator replaying at stamp (local was cancelled)", c.seq))
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
		local params
		if tonumber(c.diff or 0) == 1 then
			-- a strict module edit: one click's change, applied onto whatever
			-- this entity holds now (so earlier clicks already landed survive)
			local cur = {}
			pcall(function() local e = game.interface.getEntity(rec.id); if e and e.params then cur = e.params end end)
			params = CM.conApplyDiff(cur, CM.deserParams(c.params) or {})
		else
			params = CM.deserParams(c.params) or {}
		end
		params.seed = nil
		local key = CM.conKey(x, y)
		expectedEdit[key] = true
		local uok, uerr = pcall(game.interface.upgradeConstruction, rec.id, c.file, params)
		log(string.format("EXEC CONU seq=%s origin=%s at=%s file=%s target=%d ok=%s%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.file), rec.id,
			tostring(uok), uok and "" or (" err=" .. tostring(uerr))))
		if uok then
			rec.params = (tonumber(c.diff or 0) == 1) and CM.ser(params) or c.params
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
function CM.execCon(c)
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
CM.expectedDemolish = {}

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
-- Unowned constructions within r of (x,y): the town-building count the strict
-- path logs at every step so the next over-demolish names its step.
function CM.townCountNear(x, y, r)
	local n = 0
	pcall(function()
		for _, id in pairs(game.interface.getEntities({ pos = { x, y }, radius = r or 200 },
			{ type = "CONSTRUCTION", includeData = false }) or {}) do
			local po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED)
			if po == nil then n = n + 1 end
		end
	end)
	return n
end

function CM.healNodeAt(x, y, why, origT)
	local healed = false
	pcall(function()
		local nid = CM.findNodeNear(false, x, y, 1.5)
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
			local comp, a, b, ta, tb = CM.edgeGeomT(eid)
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
		local t0 = { tng[1][1] * s1, tng[1][2] * s1, tng[1][3] * s1 }
		local t1 = { tng[2][1] * s2, tng[2][2] * s2, tng[2][3] * s2 }
		-- EXACT GEOMETRY when the caller knows it. Reconstructing the merged edge
		-- by rescaling the stubs' tangents assumes the split left them untouched,
		-- but a native placement re-shapes them (terrain align + graph cleanup),
		-- so the merged road is NOT the original road: it sits a little off, and
		-- the rebuild's own graph cleanup then re-touches it along its length and
		-- the buildings lining it get cleared -- collateral the peers, whose road
		-- was never split, never see (measured: A 58 buildings vs peers 60,
		-- 2026-09-08). The strict replay SHIPS the removed edge with its original
		-- tangents, so the originator can put back exactly the road that was
		-- there. origT = {t0x,t0y,t0z, t1x,t1y,t1z} oriented from the shipped
		-- edge's node0 to node1; our merged edge runs far[1] -> far[2], so orient
		-- by the chord: if the chord agrees with t0 the ends line up, otherwise
		-- the shipped edge ran the other way and both tangents flip and swap.
		if origT and #origT >= 6 then
			local cx, cy = far[2][2][1] - far[1][2][1], far[2][2][2] - far[1][2][2]
			if cx * origT[1] + cy * origT[2] >= 0 then
				t0 = { origT[1], origT[2], origT[3] }
				t1 = { origT[4], origT[5], origT[6] }
			else
				t0 = { -origT[4], -origT[5], -origT[6] }
				t1 = { -origT[1], -origT[2], -origT[3] }
			end
		end
		local sp = api.type.SimpleProposal.new()
		local e = api.type.SegmentAndEntity.new()
		e.entity = -1
		e.comp.node0 = far[1][1]
		e.comp.node1 = far[2][1]
		e.comp.tangent0 = api.type.Vec3f.new(t0[1], t0[2], t0[3])
		e.comp.tangent1 = api.type.Vec3f.new(t1[1], t1[2], t1[3])
		e.comp.type = 0
		e.comp.typeIndex = -1
		e.type = 0
		CM.copyEdgeProps(e, ids[1], false, nil)
		sp.streetProposal.edgesToAdd[1] = e
		sp.streetProposal.edgesToRemove[1] = ids[1]
		sp.streetProposal.edgesToRemove[2] = ids[2]
		-- The node has to go with them. Dropping only the two edges leaves the
		-- engine to reconcile an orphan node and it refuses the whole proposal
		-- with critical=true and 'Internal error (see console for details)'
		-- (measured on the live peer, 2026-08-30).
		sp.streetProposal.nodesToRemove[1] = nid
		-- NEVER ignoreErrors here. A heal rebuilds the two stubs as ONE merged
		-- edge, and that reconstituted Hermite is not bit-identical to the
		-- original road -- it swings a little through whatever lines it. With
		-- ignoreErrors=true the engine resolved that collision by BULLDOZING the
		-- buildings: the strict pre-rebuild heal on the originator removed a
		-- column of five town buildings directly north of the healed node that
		-- both peers (whose road was never split, so never healed) kept -- a
		-- construction-lane DESYNC (59 vs 65 buildings, measured 2026-09-08).
		-- A road merge must never demolish anything. If the merge would
		-- collide, the engine refuses it; the split node and stubs then simply
		-- stay, which every consumer already tolerates (findEdgeContaining
		-- position-resolves against a stub just as well as a through-road).
		-- gatherBuildings=FALSE is the whole point. A merge with a nil context
		-- uses the engine default, which GATHERS (demolishes) town buildings in
		-- the merged edge's footprint -- 7 of them on the strict originator,
		-- buildings the peers (who never split, never heal) kept, so A ended 6
		-- short and the town-building lane desynced (2026-09-08). The peers'
		-- own depot build sets exactly this flag (conxContext, gatherBuildings
		-- =false) and clears obstacles ONLY through the shared survivor-diff.
		-- The heal must do the same: merge the road, demolish NOTHING, and let
		-- A's survivor-diff -- the identical code and the same shipped survivor
		-- list as the peers -- converge A to the exact same building set.
		-- checkTerrainAlignment=false too: this is putting back a road that was
		-- already there, not shaping a new one.
		local hctx = nil
		pcall(function()
			local c = api.type.Context:new()
			c.checkTerrainAlignment = false
			c.cleanupStreetGraph    = true
			c.gatherBuildings       = false
			c.gatherFields          = true
			c.player                = api.engine.util.getPlayer()
			hctx = c
		end)
		local cmd = api.cmd.make.buildProposal(sp, hctx, false)
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
			log(string.format("HEAL(%s): node %d at %.1f,%.1f rejoined: %s%s | town buildings within 200 m now: %d",
				why, nid, x, y, tostring(ok2), extra, CM.townCountNear(x, y, 200)))
		end)
	end)
	return healed
end

-- Every node a replay of ours cut into a road, with the tick it was cut at.
CM.splitWatch = {}
function CM.watchSplit(x, y)
	CM.splitWatch[string.format("%.1f/%.1f", x, y)] = { x, y, CM.ticks }
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
		if dx * dx + dy * dy < 60 * 60 then CM.splitWatch[k] = { site[1], site[2], CM.ticks } end
	end
end

function CM.sweepSplits()
	for k, w in pairs(CM.splitWatch) do
		if CM.ticks - w[3] > CM.SPLIT_SETTLE then
			local nid = CM.findNodeNear(false, w[1], w[2], 1.5)
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


function CM.execDemolish(c)
	-- Poll-detected (legacy) demolish: the originator already bulldozed its own
	-- construction (the removal detector fired BECAUSE it was gone locally), so
	-- only the peers replay. STRICT (strict=1, from a CDEMO the slice cancelled):
	-- nothing has been removed anywhere yet, so the originator replays too and
	-- everyone bulldozes on the same sim-step.
	local strict = tonumber(c.strict or 0) == 1
	if c.origin == K.INSTANCE and not strict then
		log(string.format("DEMOLISH seq=%s: originator already bulldozed locally, skipping", tostring(c.seq)))
		return
	end
	local ok, err = pcall(function()
		local best, bestD
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		-- Strict matches EXACTLY (same file, within 2 m): if the cancel ever
		-- fails and the native bulldoze runs, the nearest-in-30m rule would take
		-- out a neighbour instead. Legacy keeps the 30 m rule -- its position
		-- comes from a key rounded to 0.1 m and it has no file to check.
		local limit = strict and 4 or 900
		for _, id in pairs(ents) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.transf then
				local dx, dy = co.transf[13] - c.x, co.transf[14] - c.y
				local d = dx * dx + dy * dy
				local fileOk = (not strict) or (c.file == nil or c.file == "" or tostring(co.fileName or "") == tostring(c.file))
				if d < limit and fileOk and (not bestD or d < bestD or (d == bestD and id < best)) then
					best, bestD = id, d
				end
			end
		end
		if not best then
			log(string.format("EXEC DEMOLISH seq=%s: nothing %s of %.1f,%.1f%s",
				tostring(c.seq), strict and "matching within 2m" or "within 30m", c.x, c.y,
				strict and " (already gone here -- no-op)" or ""))
			return
		end
		-- Mark the spot so our own removal-detector recognises this as a replay
		-- (our construction is about to vanish) and does not echo it back.
		CM.expectedDemolish[CM.conKey(c.x, c.y)] = true
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
		-- Refund attribution. A poll-detected demolish is OPTIMISTIC: the
		-- originator bulldozes natively at click, the peer replays here ~a few
		-- game-units later, so the balance moves at different sim-times and any
		-- refund difference is a lasting coop money gap (measured 2026-09-02: two
		-- depot demolishes opened a ~175k split). A STRICT one (strict_condemo)
		-- lands this same bulldoze on every instance at the stamp, originator
		-- included, so the refund is simultaneous. The measurement stays for
		-- both, so the log shows which case this was.
		local bal0 = CM.cmBalance(api.engine.util.getPlayer())
		local dok, derr = pcall(game.interface.bulldoze, best)
		local bal1 = CM.cmBalance(api.engine.util.getPlayer())
		if bal0 and bal1 then
			log(string.format("DEMOLISH seq=%s: refund on this peer = %+d (%d -> %d)", tostring(c.seq), bal1 - bal0, bal0, bal1))
		end
		CM.rearmSplitsNear(c.x, c.y)
		log(string.format("EXEC DEMOLISH seq=%s origin=%s at=%s id=%d success=%s",
			tostring(c.seq), tostring(c.origin), tostring(c.at), best, tostring(dok)))
		CM.cmLog(string.format("CM: DEMOLISH seq=%s id=%d owner=%s unlock=%s bulldoze ok=%s err=%s",
			tostring(c.seq), best, tostring(owner), unlocked, tostring(dok), tostring(derr)))
	end)
	if not ok then log("exec DEMOLISH error: " .. tostring(err)) end
end

-- ---------- EDEMO: a bulldozed road/rail EDGE ----------
--
-- The gap this closes: every replicated op ADDED or edited. The only removals
-- that crossed were constructions, vehicles, lines and stops -- never a road.
-- The slice saw the bulldozer ("BULLDOZE re=1 ... edge-demolish shape") and
-- only wrote it to the log, so demolishing a road was a silent LOCAL edit. The
-- originator's road vanished, every peer kept theirs, and the e and z lanes
-- diverged on the spot. It reads as a lag artifact -- "I demolished during lag
-- and the crossing did not form" -- but lag only widens the window in which the
-- player does it. The demolish never replicated at any speed (2026-09-03).
--
-- Nothing here names an edge by ID. c.params carries the removed edges as
-- ENDPOINT POSITIONS, "x0,y0,z0,x1,y1,z1;..." -- the peer finds the node
-- nearest each end and takes the edge BETWEEN them. Positions are identical
-- across instances (same save, same replayed proposals) while entity ids are
-- not, and a bulldoze aimed by a divergent id would destroy the wrong road with
-- nothing to rebuild it from. See [[replicate-positions-not-entity-ids]].
-- "x0,y0,z0,x1,y1,z1,kind;..." -> { {x0,y0,z0,x1,y1,z1,kind}, ... }
local function edemoPairs(params)
	local out = {}
	for rec in tostring(params or ""):gmatch("[^;]+") do
		local v = {}
		for f in rec:gmatch("[^,]+") do v[#v + 1] = tonumber(f) end
		if #v >= 7 then out[#out + 1] = v end
	end
	return out
end

-- Nearest node to each endpoint, searching ONLY the map of that endpoint's own
-- KIND. A road node and a rail node can sit at the same spot and are different
-- nodes; matching across kinds is how findNodeNear once welded track to street,
-- and here it would bulldoze the road beside a level crossing instead of the
-- rail the player actually removed.
--
-- One pass per map covering ALL endpoints, rather than a scan per endpoint: a
-- dragged bulldoze removes many edges at once and a scan each would be tens of
-- thousands of component reads. It runs on a player action, never on a tick.
local function edemoMatchNodes(want)
	local byKind = {}
	pcall(function() byKind[0] = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	pcall(function() byKind[1] = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	local best, bestD = {}, {}
	for kind = 0, 1 do
		local m = byKind[kind]
		if m then
			for nid in pairs(m) do
				-- fetched at most once per node, and only if some endpoint of
				-- this kind is still looking
				local pos = nil
				for i, w in ipairs(want) do
					if w[4] == kind then
						if pos == nil then pos = CM.nodePosXYZ(nid) or false end
						if pos then
							local dx, dy = pos[1] - w[1], pos[2] - w[2]
							local d = dx * dx + dy * dy
							-- ties broken by the lower id so every peer agrees
							if d <= K.EDEMO_TOL_SQ and (not bestD[i] or d < bestD[i]
									or (d == bestD[i] and nid < best[i])) then
								best[i], bestD[i] = nid, d
							end
						end
					end
				end
			end
		end
	end
	return best, byKind
end

function CM.execEdgeDemolish(c)
	if not CM.cfgFlag("road_demolish", false) then
		log(string.format("EDEMO seq=%s: road_demolish not enabled on this instance -- IGNORED (this peer keeps the road and will diverge)",
			tostring(c.seq)))
		return
	end
	-- NO originator skip. The slice CANCELS the player's bulldoze at
	-- CommandList::Add and this replays it at the stamp, so all three instances
	-- remove the road at the same game-time -- the same cancel-and-replay
	-- architecture as roads, upgrades, vehicles and lines. Compare execDemolish
	-- above, which does skip: a CONSTRUCTION demolish is found by a poll AFTER
	-- it has already happened locally, so there is nothing left to replay here.
	--
	-- If the cancel could not be honoured (the slice cannot fire the bulldozer's
	-- completion callback and lets it run rather than wedge the tool) the road
	-- is already gone on this instance. That is not an error: nothing matches
	-- and it logs "nothing to remove". Replay is idempotent by construction.
	local ok, err = pcall(function()
		local prs = edemoPairs(c.params)
		if #prs == 0 then
			log(string.format("EDEMO seq=%s: no endpoint pairs in params", tostring(c.seq)))
			return
		end
		-- flatten both ends of every edge into one match list
		-- want[i] = {x, y, z, kind}; two entries per removed edge
		local want = {}
		for _, v in ipairs(prs) do
			local kind = (v[7] == 1) and 1 or 0
			want[#want + 1] = { v[1], v[2], v[3], kind }
			want[#want + 1] = { v[4], v[5], v[6], kind }
		end
		local nodes, byKind = edemoMatchNodes(want)

		local rmSet, rmList, unmatched = {}, {}, 0
		for i = 1, #prs do
			local a, b = nodes[i * 2 - 1], nodes[i * 2]
			if not (a and b) then
				unmatched = unmatched + 1
				log(string.format("EDEMO seq=%s: edge %d unmatched (endpoint node missing: a=%s b=%s)",
					tostring(c.seq), i, tostring(a), tostring(b)))
			else
				-- the edge between them = the intersection of their edge
				-- lists, taken in THIS edge's own map only
				local found
				local m = byKind[(prs[i][7] == 1) and 1 or 0]
				local ea, eb = m and m[a], m and m[b]
				if ea and eb then
					local inA = {}
					for _, e in pairs(ea) do inA[e] = true end
					for _, e in pairs(eb) do
						if inA[e] and (not found or e < found) then found = e end
					end
				end
				if found then
					if not rmSet[found] then
						rmSet[found] = true
						rmList[#rmList + 1] = found
					end
				else
					unmatched = unmatched + 1
					log(string.format("EDEMO seq=%s: edge %d: nodes %d/%d matched but no edge between them",
						tostring(c.seq), i, a, b))
				end
			end
		end
		-- SAFETY NET (2026-09-08): removing an edge that still carries an edge
		-- object (a stop or a signal a line references) is a fatal engine
		-- assert -- one EDEMO of that kind took all three instances down with a
		-- minidump each. The slice no longer ships that shape (it is an edge
		-- replace, not a demolish), but a stale or foreign command must not be
		-- able to crash a peer: drop such edges here and say so.
		do
			local kept = {}
			for _, eid in ipairs(rmList) do
				local nobj = 0
				pcall(function()
					local be = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
					if be and be.objects then nobj = #be.objects end
				end)
				if nobj > 0 then
					rmSet[eid] = nil
					log(string.format("EDEMO seq=%s: edge %d carries %d edge object(s) -- REFUSED (fatal engine assert; DIVERGENCE if the originator removed it)",
						tostring(c.seq), eid, nobj))
				else
					kept[#kept + 1] = eid
				end
			end
			rmList = kept
		end
		if #rmList == 0 then
			log(string.format("EDEMO seq=%s: nothing to remove (%d unmatched)", tostring(c.seq), unmatched))
			return
		end

		-- A node left with no edges at all makes the engine refuse the whole
		-- proposal as critical ("Internal error"), so orphans go WITH the edges.
		-- Derived here rather than shipped: what is orphaned depends on this
		-- world's edge lists, and the originator's answer need not be ours.
		local orphans, orphSet = {}, {}
		for i = 1, #want do
			local nid = nodes[i]
			if nid and not orphSet[nid] then
				local total, killed = 0, 0
				for k = 0, 1 do
					local m = byKind[k]
					for _, e in pairs((m and m[nid]) or {}) do
						total = total + 1
						if rmSet[e] then killed = killed + 1 end
					end
				end
				if total > 0 and killed == total then
					orphSet[nid] = true
					orphans[#orphans + 1] = nid
				end
			end
		end

		local sp = api.type.SimpleProposal.new()
		for i, eid in ipairs(rmList) do sp.streetProposal.edgesToRemove[i] = eid end
		for i, nid in ipairs(orphans) do sp.streetProposal.nodesToRemove[i] = nid end
		local cmd = api.cmd.make.buildProposal(sp, nil, true)
		if not cmd then
			log(string.format("EDEMO seq=%s: buildProposal returned nil", tostring(c.seq)))
			return
		end
		local nEdges, nOrph = #rmList, #orphans
		api.cmd.sendCommand(cmd, function(res, ok2)
			local extra = ""
			if not ok2 then
				pcall(function()
					local es = res.resultProposalData and res.resultProposalData.errorState
					if es then
						extra = string.format(" critical=%s msg=%s", tostring(es.critical),
							tostring(es.messages and es.messages[1]))
					end
				end)
			end
			log(string.format("EXEC EDEMO seq=%s origin=%s at=%s: removed %d edge(s) + %d orphan node(s) success=%s%s",
				tostring(c.seq), tostring(c.origin), tostring(c.at), nEdges, nOrph, tostring(ok2), extra))
		end)
	end)
	if not ok then log("exec EDEMO error: " .. tostring(err)) end
end

-- Deep-copy a params table so a per-sample mutation cannot leak into the next.
function CM.deepcopy(t)
	if type(t) ~= "table" then return t end
	local r = {}
	for k, v in pairs(t) do r[k] = CM.deepcopy(v) end
	return r
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
function CM.isPlayerConstruction(id, fileName)
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
	local prev = CM.consByKey[key]
	CM.consByKey[key] = { id = id, file = fn, params = pstr }
	return prev
end

local function shipEdit(fn, key, pstr)
	local kx, ky = key:match("^([-%d.]+)/([-%d.]+)$")
	CM.scheduleLocal("CONU", { file = fn, x = tonumber(kx), y = tonumber(ky), params = pstr })
	log(string.format("con: edit captured %s at %s params=%dB", fn, key, #pstr))
end

-- Town constructions + asset groups still standing within 120 m of (cx,cy),
-- as "x:y;x:y;..." (nil if none). Shipped with a CONX so the peer removes
-- exactly the in-footprint town buildings that the originator's world no longer
-- has -- no pad, no bbox, no corridor width to guess. Used by BOTH CONX emit
-- paths (queueConCapture and findConstructionForRoadc: the latter never carried
-- survivors, which is why every station shipped `survivors=0`, 2026-08-29).
function CM.gatherSurvivors(cx, cy, selfId)
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
	-- balance before this construction existed (previous poll), so the strict
	-- path can refund exactly what the native copy cost
	if CM.balPrevConPoll then CM.conBal0[key] = CM.balPrevConPoll end
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
	local survivors = CM.gatherSurvivors(transf[13], transf[14], id)
	CM.pendingCons[#CM.pendingCons + 1] = { at = CM.gameTime() or 0, file = fn, key = key,
	                                  t = table.concat(t, ","), params = pstr,
	                                  x = transf[13], y = transf[14], name = CM.escName(name),
	                                  bal0 = CM.balPrevConPoll,  -- balance before this build; ships as the true cost
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
	local conxCost = nil
	local base0 = cn.bal0 or rc.bal0   -- pre-build balance (rc covers the rescue path)
	-- Cancelled placement: nobody has paid yet -- every instance pays the same
	-- scripted cost at the stamp, so no cost/bal ships and no snap happens.
	if cn.cancelled then base0 = nil end
	if base0 then
		local bnow = CM.cmBalance(api.engine.util.getPlayer())
		if bnow then conxCost = base0 - bnow end
	end
	local conxBal = (not cn.cancelled) and CM.cmBalance(api.engine.util.getPlayer()) or nil  -- absolute post-build balance (canonical coop wallet)
	log(string.format("con: CONX cost=%s (bal0=%s bal=%s) for %s", tostring(conxCost), tostring(base0), tostring(conxBal), tostring(cn.file)))
	CM.scheduleLocal("CONX", { file = cn.file, t = cn.t, params = cn.params, name = cn.name, survivors = cn.survivors, cost = conxCost, bal = conxBal, cancelled = cn.cancelled,
	                        snodes = table.concat(sn, ";"), sedges = table.concat(se, ";"),
	                        srm = table.concat(sr, ";"), spos = table.concat(spz, ";"),
	                        etype = rc.etype, stype = rc.stype, ttype = rc.ttype, cat = rc.cat })
	-- Arm OUR split site too. The orphan-split heal (CM.sweepSplits) only knew
	-- the splits a REPLAY made, so after a demolish the peer healed the split
	-- and the originator never did: A kept the node and two halves, the peer
	-- merged them (e1082 vs e1081, 2026-09-02), and the next depot attached to
	-- that node on A had nothing to attach to on the peers -- fatal. The split
	-- node is the added node that lies on a removed edge's segment.
	pcall(function()
		for _, r in ipairs(rc.rms) do
			-- a removed-edge record is { node0, node1, tangents[6] }; the endpoint
			-- POSITIONS are the existing nodes' entries in rc.spos
			local pa, pb = rc.spos[r[1]], rc.spos[r[2]]
			if not (pa and pb) then return end
			local ax, ay, bx, by = pa[1], pa[2], pb[1], pb[2]
			local vx, vy = bx - ax, by - ay
			local L2 = vx * vx + vy * vy
			if L2 > 1 then
				for _, q in pairs(rc.posOf) do
					local t = ((q[1] - ax) * vx + (q[2] - ay) * vy) / L2
					if t > 0.02 and t < 0.98 then
						local px, py = ax + t * vx, ay + t * vy
						if (q[1] - px) ^ 2 + (q[2] - py) ^ 2 < 2.25 then CM.watchSplit(q[1], q[2]) end
					end
				end
			end
		end
	end)
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
			if co and co.fileName and co.transf and CM.isPlayerConstruction(id, tostring(co.fileName)) then
				local key = CM.conKey(co.transf[13], co.transf[14])
				-- not one we already track (avoid re-shipping an existing station)
				if not CM.consByKey[key] then
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
	local pstr = (e and e.params) and CM.ser(e.params) or "{}"
	local nm = ""
	pcall(function()
		local nc = api.engine.getComponent(bestId, api.type.ComponentType.NAME)
		if nc and nc.name then nm = tostring(nc.name) end
	end)
	-- register so the demolish/edit trackers see it, and so we do not re-ship it
	local key = CM.conKey(best.transf[13], best.transf[14])
	knownCons[bestId] = true
	noteCon(bestId, tostring(best.fileName), key, pstr)
	return { file = tostring(best.fileName), t = table.concat(t, ","), params = pstr,
	         name = CM.escName(nm), x = best.transf[13], y = best.transf[14], id = bestId,
	         survivors = CM.gatherSurvivors(best.transf[13], best.transf[14], bestId) }
end

function CM.flushConPairs()
	local now = CM.gameTime()
	if not now then return end
	for ci = #CM.pendingCons, 1, -1 do
		local cn = CM.pendingCons[ci]
		local best, bestD
		for ri, rc in ipairs(CM.pendingRoadc) do
			local d
			for _, p in pairs(rc.posOf) do
				local dd = (p[1] - cn.x) ^ 2 + (p[2] - cn.y) ^ 2
				if not d or dd < d then d = dd end
			end
			if d and d < 150 * 150 and (not bestD or d < bestD) then best, bestD = ri, d end
		end
		if best then
			local rc = table.remove(CM.pendingRoadc, best)
			table.remove(CM.pendingCons, ci)
			shipConxPair(cn, rc)
		elseif now - cn.at > 1.0 then
			table.remove(CM.pendingCons, ci)
			-- cancelled=1 rides along: a free-standing placement the slice cancelled
			-- must be built by the scripted proposal on the originator too, not
			-- bulldozed and rebuilt (the flag used to be dropped here)
			CM.scheduleLocal("CONP", { file = cn.file, t = cn.t, params = cn.params, name = cn.name, cancelled = cn.cancelled })
			log(string.format("con: captured %s (free-standing%s) -> CONP", cn.file, cn.cancelled == 1 and ", cancelled" or ""))
		end
	end
	for ri = #CM.pendingRoadc, 1, -1 do
		local rc = CM.pendingRoadc[ri]
		-- Active rescue: a ROADC that has waited a beat without a pollNewConstructions
		-- capture (id reuse) -- look for its construction directly by position.
		if now - rc.at > 1.0 and now - rc.at <= 15 then
			local cn = findConstructionForRoadc(rc)
			if cn then
				table.remove(CM.pendingRoadc, ri)
				log(string.format("con: ROADC rescued its construction %s by position (id-reuse case)", cn.file))
				shipConxPair(cn, rc)
			end
		elseif now - rc.at > 15 then
			table.remove(CM.pendingRoadc, ri)
			log("ROADC: no construction paired within 15 units -- street payload dropped")
		end
	end
end

-- Balance as of the previous construction poll. A player's native build lands
-- between two polls, so this is the balance BEFORE it -- the only way to learn
-- what the native copy cost, which the strict reconciliation needs (the scripted
-- rebuild costs a DIFFERENT amount: it grades the terrain differently, measured
-- 16054 more on one depot).
CM.balPrevConPoll = nil
CM.conBal0 = {}          -- conKey -> balance before that construction was built
function CM.pollNewConstructions()
	local balAtEntry = nil
	pcall(function() local e = game.interface.getEntity(api.engine.util.getPlayer()); if e then balAtEntry = tonumber(e.balance) end end)
	local ok, err = pcall(function()
		local list = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false }) or {}
		if not CM.consPrimed then
			-- Existing constructions get classified a few per tick, so a station
			-- that was in the save can still have its EDITS replicated, without
			-- thousands of component reads in one frame.
			for _, id in pairs(list) do knownCons[id] = true; CM.primeQueue[#CM.primeQueue + 1] = id end
			CM.consPrimed = true
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
						tostring(CM.isPlayerConstruction(id, fn0)), tostring(waitOwnership)))
				end
				if not waitOwnership then
				ownershipPending[id] = nil
				knownCons[id] = true
				local alive = false
				pcall(function() alive = api.engine.entityExists(id) end)
				if alive then
					local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
					if co and co.fileName and co.transf and CM.isPlayerConstruction(id, tostring(co.fileName)) then
						local fn = tostring(co.fileName)
						local key = CM.conKey(co.transf[13], co.transf[14])
						local e = game.interface.getEntity(id)
						local pstr = (e and e.params) and CM.ser(e.params) or "{}"
						if CM.expectedCons[key] then
							CM.expectedCons[key] = nil
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
						elseif CM.consByKey[key] and CM.consByKey[key].file == fn then
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
	if balAtEntry then CM.balPrevConPoll = balAtEntry end
end

-- Classify a slice of the primed ids each tick.
function CM.primeConstructions()
	if #CM.primeQueue == 0 then return end
	local ok, err = pcall(function()
		local n = 0
		while #CM.primeQueue > 0 and n < K.PRIME_PER_TICK do
			local id = table.remove(CM.primeQueue)
			n = n + 1
			local alive = false
			pcall(function() alive = api.engine.entityExists(id) end)
			if alive then
				local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
				if co and co.fileName and co.transf and CM.isPlayerConstruction(id, tostring(co.fileName)) then
					local e = game.interface.getEntity(id)
					noteCon(id, tostring(co.fileName), CM.conKey(co.transf[13], co.transf[14]),
						(e and e.params) and CM.ser(e.params) or "{}")
				end
			end
		end
		if #CM.primeQueue == 0 then
			local c = 0; for _ in pairs(CM.consByKey) do c = c + 1 end
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
				if CM.isPlayerConstruction(id, fn) then found = id end
			end
		end
	end)
	return found
end

function CM.pollConstructionRemovals()
	local ok, err = pcall(function()
		for key, rec in pairs(CM.consByKey) do
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
						if expectedEdit[key] or CM.expectedCons[key] then
							-- upgrade/replay in flight -- but a STALE flag here
							-- silently suppresses a real demolish forever
							-- (suspected one-off 2026-08-29: bulldoze logged by
							-- the hook, mod said nothing). Tripwire it.
							log(string.format("con: %s is gone but edit/replay flags block the demolish (edit=%s cons=%s) -- will re-check",
								key, tostring(expectedEdit[key] ~= nil), tostring(CM.expectedCons[key] ~= nil)))
						elseif CM.expectedDemolish[key] then
							CM.expectedDemolish[key] = nil
							CM.consByKey[key] = nil
							log(string.format("con: %s bulldozed by replay -- not echoed", key))
						else
							CM.consByKey[key] = nil
							if x and y then
								CM.rearmSplitsNear(x, y)
								CM.scheduleLocal("DEMOLISH", { x = x, y = y })
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

function CM.scanConstructionEdits()
	local ok, err = pcall(function()
		for key, rec in pairs(CM.consByKey) do
			local alive = false
			pcall(function() alive = api.engine.entityExists(rec.id) end)
			if alive then
				local e = game.interface.getEntity(rec.id)
				local pstr = (e and e.params) and CM.ser(e.params) or "{}"
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
end
