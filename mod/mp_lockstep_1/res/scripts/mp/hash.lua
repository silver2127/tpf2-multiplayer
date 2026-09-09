-- mp/hash.lua -- exact hashing, game time, world hash (desync detector), vehicle drift metric
--
-- Split out of lockstep.lua on 2026-09-08 (the single 11k-line chunk was at
-- 182 of Lua 5.1's 200 file-scope locals). Loaded from the game script as
--     local hash = require("mp.hash")(CM, K, log)
-- It is a FACTORY, not a plain module table: every load of the game script
-- gets fresh state (package.loaded would otherwise hand the next game the
-- previous game's file-scope locals), and the code below keeps the same
-- per-load semantics it had inside lockstep.lua. Anything defined later in
-- lockstep.lua is reached through CM (CM.ser, CM.deepcopy, ...); CM is the
-- shared state table, K the constants table, log the instance-tagged logger.
-- The body stays at column 0 on purpose: tools/luacheck.py's use-before-define
-- checks look at column-0 declarations.
return function(CM, K, log)
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

-- ---------- vehicle drift metric ----------
-- The p lane of the hash answers "are the vehicles in the same places" with a
-- yes/no. This measures the DISTANCE. At every hash stamp each peer ships its
-- vehicle positions (LSVPOS, chunked to stay under a bridge frame); the
-- receiver pairs each of its own vehicles with the nearest peer vehicle and
-- keeps the mean and max of those distances per stamp. Ids differ between
-- instances, so nearest-neighbour is the pairing: while the worlds are close
-- it is exact, and once they are far apart the number is large either way.
-- Only samples taken at the SAME sim time are compared (a train moves ~5 m per
-- 0.2-unit step); others are counted as skipped. A running history says
-- whether the drift GROWS (the sims are diverging) or stays flat (a constant
-- offset, e.g. one late apply) -- that trend is the point of this metric.
K.VPOS_PER_PART = 30
-- Vehicle drift is now a DESYNC, not just a metric -- but thresholded, so the
-- benign sub-tick jitter between honest instances (~1-3 m) never cries wolf.
-- A depot-triggered native-vs-replay drift blows past this and keeps growing.
-- cfg vpos_desync_m overrides it.
K.VPOS_DESYNC_M = 10
K.VPOS_KEEP = 24
CM.vposMine = {}      -- stamp -> { s = simtime, pts = {{x,y},...} }
CM.vposPeer = {}      -- letter -> stamp -> { s=, n=, m=, got=, pts= }
CM.vposHist = {}      -- letter -> { {t=, mean=, max=}, ... }
CM.vposSkipped = 0
CM.vposDone = {}      -- letter..":"..stamp -> true

local function vposPrune(tbl, keep)
	local ks = {}
	for k in pairs(tbl) do ks[#ks + 1] = k end
	if #ks <= keep then return end
	table.sort(ks)
	for i = 1, #ks - keep do tbl[ks[i]] = nil end
end

function CM.vposCompare(stamp, o)
	local key = o .. ":" .. tostring(stamp)
	if CM.vposDone[key] then return end
	local mine, theirs = CM.vposMine[stamp], CM.vposPeer[o] and CM.vposPeer[o][stamp]
	if not mine or not theirs or theirs.got < theirs.m then return end
	CM.vposDone[key] = true
	if math.abs((mine.s or -1) - (theirs.s or -2)) > 0.05 then
		CM.vposSkipped = CM.vposSkipped + 1
		log(string.format("VPOS t=%d vs %s: sampled at different sim times (%.1f vs %.1f) -- skipped (%d so far)",
			stamp, o, mine.s or -1, theirs.s or -1, CM.vposSkipped))
		return
	end
	local a, b = mine.pts, theirs.pts
	if #a == 0 or #b == 0 then
		log(string.format("VPOS t=%d vs %s: n=%d/%d -- nothing to pair", stamp, o, #a, #b))
		return
	end
	local sum, mx, over1, over10 = 0, 0, 0, 0
	for i = 1, #a do
		local ax, ay = a[i][1], a[i][2]
		local best = nil
		for j = 1, #b do
			local dx, dy = b[j][1] - ax, b[j][2] - ay
			local d = dx * dx + dy * dy
			if not best or d < best then best = d end
		end
		local d = math.sqrt(best)
		sum = sum + d
		if d > mx then mx = d end
		if d > 1 then over1 = over1 + 1 end
		if d > 10 then over10 = over10 + 1 end
	end
	local mean = sum / #a
	local h = CM.vposHist[o] or {}
	CM.vposHist[o] = h
	h[#h + 1] = { t = stamp, mean = mean, max = mx }
	while #h > 60 do table.remove(h, 1) end
	local trend = ""
	if #h >= 10 then
		local n = #h
		local half = math.floor(n / 2)
		local s1, s2 = 0, 0
		for i = 1, half do s1 = s1 + h[i].mean end
		for i = half + 1, n do s2 = s2 + h[i].mean end
		local m1, m2 = s1 / half, s2 / (n - half)
		local word = "flat"
		if m2 > m1 * 1.25 + 0.2 then word = "GROWING" elseif m2 < m1 * 0.8 - 0.2 then word = "shrinking" end
		trend = string.format(" | trend over %d samples: %.2f -> %.2f m (%s)", n, m1, m2, word)
	end
	CM.vposLast = CM.vposLast or {}
	CM.vposLast[o] = { mean = mean, max = mx, t = stamp, n = #a }
	log(string.format("VPOS t=%d vs %s @%.1f: n=%d/%d mean=%.2f m max=%.2f m over1m=%d over10m=%d%s",
		stamp, o, mine.s, #a, #b, mean, mx, over1, over10, trend))
	-- VEHICLE DRIFT AS A DESYNC. Vehicle positions are not in the verdict
	-- hash (they round-jitter), so this is the only place the drift is
	-- caught. Beyond the tolerance it counts as a real desync: bump the
	-- counter, mark the dashboard, and log it loud, exactly like a hash
	-- mismatch. One count per stamp per peer (vposDone guards re-entry).
	local lim = CM.cfgNum and CM.cfgNum("vpos_desync_m", K.VPOS_DESYNC_M) or K.VPOS_DESYNC_M
	if mx > lim then
		CM.desyncs = CM.desyncs + 1
		CM.dashVerdict = string.format("DESYNC vpos %.0fm vs %s", mx, o)
		log(string.format("!! DESYNC (vehicle drift) t=%d vs %s: max=%.2f m mean=%.2f m (> %d m) -- total %d",
			stamp, o, mx, mean, lim, CM.desyncs))
	end
end


function CM.vposRecv(line)
	local stamp = tonumber(line:match(" t=(%-?%d+)"))
	local st = tonumber(line:match(" s=([%-%d%.]+)"))
	local o = line:match(" o=(%a)")
	local i, m, n = tonumber(line:match(" i=(%d+)")), tonumber(line:match(" m=(%d+)")), tonumber(line:match(" n=(%d+)"))
	local d = line:match(" d=(%S*)")
	if not (stamp and st and o and i and m) then return end
	CM.vposPeer[o] = CM.vposPeer[o] or {}
	local rec = CM.vposPeer[o][stamp]
	if not rec then
		rec = { s = st, n = n or 0, m = m, got = 0, pts = {}, seen = {} }
		CM.vposPeer[o][stamp] = rec
		vposPrune(CM.vposPeer[o], K.VPOS_KEEP)
	end
	if rec.seen[i] then return end
	rec.seen[i] = true
	rec.got = rec.got + 1
	if d and d ~= "-" then
		for x, y in d:gmatch("([%-%d%.]+),([%-%d%.]+)") do
			rec.pts[#rec.pts + 1] = { tonumber(x) or 0, tonumber(y) or 0 }
		end
	end
	CM.vposCompare(stamp, o)
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
		local raw = {}
		for vid, e in pairs(t) do
			nv = nv + 1
			local p = type(e) == "table" and e.position or nil
			if p then
				-- sorted below, so this says nothing about WHICH vehicle is
				-- where -- ids differ between instances and always will
				vpos[#vpos + 1] = string.format("%.0f,%.0f,%.0f",
					p[1] or p.x or 0, p[2] or p.y or 0, p[3] or p.z or 0)
				-- quantised exactly as it ships (0.1 m), so a peer's copy of an
				-- identical world compares at 0.00 and not at the rounding floor
				-- (measured 0.04-0.05 m before this)
				raw[#raw + 1] = { math.floor((p[1] or p.x or 0) * 10 + 0.5) / 10, math.floor((p[2] or p.y or 0) * 10 + 0.5) / 10 }
			end
		end
		-- the raw positions feed the drift METRIC (CM.vposShip / CM.vposCompare):
		-- the hash says equal-or-not, the metric says by how many metres
		CM.lastVposRaw, CM.lastVposT = raw, now
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
				if co and co.transf and CM.isPlayerConstruction(cid, fn) then
					local e = game.interface.getEntity(cid)
					local p = (e and e.params) and CM.deepcopy(e.params) or {}
					p.seed = nil
					cons[#cons + 1] = string.format("%s@%.1f,%.1f:%s", fn,
						co.transf[13], co.transf[14], hashStr(CM.ser(p) or ""))
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
	-- Offline diff for the rail-station height desync (cfg dump_egeo=1, default
	-- off): write the EXACT sorted strings the e-lane hashes to a per-instance
	-- side file, rolling latest. After placing one station, egeo_a.txt vs
	-- egeo_b.txt shows which edges differ and whether only the z field moved
	-- (native-vs-replay height path) or x,y too (topology/weld). The hashed
	-- arrays are untouched.
	if CM.cfgFlag("dump_egeo", false) then
		pcall(function()
			local f = io.open("egeo_" .. tostring(K.INSTANCE or "x") .. ".txt", "w")
			if f then
				f:write(string.format("stamp=%.1f nedges=%d\n", now or -1, #egeo))
				for i = 1, #egeo do f:write(egeo[i] .. "\n") end
				f:close()
			end
		end)
	end

	if #edges == 0 and not warnedNoEdges then
		warnedNoEdges = true
		log("WARNING: edge enumeration found NOTHING -- the desync detector is BLIND TO ROADS AND TRACK")
	elseif edgeStrategy and not warnedNoEdges then
		warnedNoEdges = true
		log("edge enumeration via " .. edgeStrategy .. ": " .. #edges .. " edges")
		-- Same save on every instance, so every instance derives the same
		-- cadence. Logged so it can be compared across the rig: if these
		-- ever differ, the stamp grids are disjoint and the detector is blind.
		CM.hashEvery = CM.hashEveryFor(#edges)
		log(string.format("hash cadence: every %d game units (%d edges)", CM.hashEvery, #edges))
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
	-- m/l: balance and loan of the shared company (co-op). Money is world state
	-- and drifts silently -- a delivery that happened on one side and not yet the
	-- other -- so it rides in the detail with its VALUE, which makes the compare
	-- log say by how much and at which stamp. Detail-only: not in the verdict.
	-- In companies mode each instance's own player is a different company, so
	-- the lane is blank there rather than a permanent false difference.
	local mBal, mLoan = "-", "-"
	if CM.cmMode ~= "companies" then
		pcall(function()
			local e = game.interface.getEntity(api.engine.util.getPlayer())
			if e then mBal = string.format("%d", e.balance or 0); mLoan = string.format("%d", e.loan or 0) end
		end)
	end
	CM.dashMoney, CM.dashLoan = mBal, mLoan
	-- PEOPLE COUNT LANE (n:). The people sim is deterministic in lockstep, so
	-- two instances at the same stamp must agree on how many sim persons
	-- exist. They did not: a depot placed over town buildings left one
	-- instance 6 people short of the other (847 vs 853) -- exactly the number
	-- of buildings the placement demolished -- and the bus driving past those
	-- buildings then drifted. A probe lane, not a verdict lane, until we know
	-- whether the count ever legitimately jitters. Ids only (no includeData):
	-- ~850 people is cheap to count and we never need their payload here.
	local np = 0
	pcall(function()
		local tp = game.interface.getEntities({ radius = 999999 }, { type = "SIM_PERSON", includeData = false }) or {}
		for _ in pairs(tp) do np = np + 1 end
	end)
	local detail = string.format("v%d,c%d:%s,e%d:%s,z:%s,p%d@%.1f:%s,m:%s,l:%s,t:%d,n:%d",
		nv, #cons, hc, #egeo, he, hashStr(table.concat(egeoZ, "|")),
		#vpos, now or -1, hashStr(table.concat(vpos, "|")), mBal, mLoan, nt, np)
	return verdict, detail
end

return { gameTime = gameTime, worldHash = worldHash, vposPrune = vposPrune }
end
