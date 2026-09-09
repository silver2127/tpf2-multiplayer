-- MP test harness: scripted in-game actions for automated replication tests.
--
-- Deliberately a SEPARATE game script from mpbridge.lua and fully pcall-walled:
-- a broken test must never be able to take down the replication path it exists
-- to test. It shares nothing with the bridge except the on-disk convention for
-- instance identity.
--
-- Actions are performed through the same engine commands a player's clicks go
-- through (buildConstruction / buildProposal), so the capture path is exercised
-- for real rather than being simulated by writing capture lines directly.
--
-- Flow:
--   driver writes  <BASE>mp_test_scenario_<inst>.txt
--   this script runs it step by step and appends to
--                  <BASE>mp_test_results_<inst>.txt
--   driver reads both instances' results and decides pass/fail.

-- ---------- runtime data directory ----------
-- The SAME contract as mpbridge.lua, lockstep.lua and bridge/src/datadir.h,
-- resolved in the same order, because all of them have to land on one directory
-- or they read and write past each other:
--   1. $TPF2MP_DATADIR             (the dev harness pins the old workshop dir)
--   2. $LOCALAPPDATA/tpf2mp/data/  (shipping layout)
--   3. the workshop literal        (the dev rig before the data dir existed)
-- The FIRST candidate holding tpf2_instance.txt wins.
--
-- This used to be candidate 3 alone, hardcoded -- the same defect mpbridge.lua
-- was fixed for. It is worse here than it looks: that workshop directory still
-- CONTAINS a tpf2_instance.txt, left over from 2026-08-29, so detectInstance
-- succeeded and bound this harness to whatever letter that stale file happened
-- to name, then read scenarios from and wrote results to a directory no live
-- instance has touched in days. The runner reported "actor captured nothing"
-- and looked like a replication failure.
-- The game's Lua may lack 'os' entirely, hence the pcall around every getenv.
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
	local function add(p) if p then cands[#cands + 1] = dir(p) end end
	add(env("TPF2MP_DATADIR"))
	local lad = env("LOCALAPPDATA")
	add(lad and (lad .. "/tpf2mp/data"))
	add("C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/3710243057/recon/m4/out/")
	for _, p in ipairs(cands) do
		local f = io.open(p .. "tpf2_instance.txt", "r")
		if f then f:close() return p end
	end
	return cands[#cands]
end)()
local IDENTITY_FILE = BASE .. "tpf2_instance.txt"

-- ---------- refuse to run alongside mp_lockstep_1 ----------
-- This harness drives actions through api.cmd from a game script. mp_bridge
-- replicates by POLLING the world, so it sees those actions and ships them --
-- which is why scenarios test mp_bridge properly.
--
-- mp_lockstep does not work that way. It replicates the PLAYER's native command,
-- captured by the slice DLL on the CALLER'S RETURN ADDRESS, and a call from Lua
-- has the wrong caller: slice_hook.cpp logs "BuildProposal from caller_rva=...
-- (not the road path) -- ignored" and drops it. So with lockstep loaded, a
-- scenario builds in THIS world and no other -- it manufactures a desync in a
-- session that had none, and the harness then reports it as a product failure.
--
-- Detected through lockstep's global LS, which it assigns at top level, so it
-- exists from the moment that mod loads (game scripts share one Lua state).
local lockstepPresent = false
local function checkLockstep()
	if lockstepPresent then return true end
	if type(LS) == "table" and LS.findNodeNear ~= nil then lockstepPresent = true end
	return lockstepPresent
end

local INSTANCE, SCENARIO_FILE, RESULT_FILE
local ticks = 0
local waitUntil = 0
local steps = nil        -- parsed scenario, nil = nothing loaded
local stepIdx = 0
local lastScenario = nil -- content signature of the scenario we are running

local function log(s) print("[mptest-" .. tostring(INSTANCE) .. "] " .. s) end

local function appendResult(s)
	local f = io.open(RESULT_FILE, "a")
	if not f then return end
	f:write(s .. "\n")
	f:close()
end

-- tick, not os.date: os may be trimmed in the game's Lua sandbox, and report()
-- runs outside a pcall, so a nil there would throw straight into update().
-- Ticks also correlate better with the bridge log than wall clock does.
local function report(step, status, detail)
	local line = string.format("STEP %d %s tick=%d | %s", step, status,
		ticks, detail or "")
	appendResult(line)
	log(line)
end

local function detectInstance()
	local f = io.open(IDENTITY_FILE, "r")
	if not f then return false end
	local s = f:read("*l")
	f:close()
	if not s or #s == 0 then return false end
	local inst = s:gsub("%s", "")
	if inst == INSTANCE then return true end
	INSTANCE = inst
	SCENARIO_FILE = BASE .. "mp_test_scenario_" .. INSTANCE .. ".txt"
	RESULT_FILE = BASE .. "mp_test_results_" .. INSTANCE .. ".txt"
	log("test harness bound to instance " .. INSTANCE)
	return true
end

-- ---------- world helpers ----------

local function groundAt(x, y)
	local z = 0
	pcall(function() z = game.interface.getHeight({ x, y }) or 0 end)
	return z
end

-- column-major 4x4 with identity rotation
local function transfAt(x, y, z)
	return { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 }
end

local function anyStreetType()
	local t
	pcall(function()
		local m = api.engine.system.streetSystem.getNode2StreetEdgeMap()
		for _, edges in pairs(m) do
			for _, eid in pairs(edges) do
				local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
				if sc and sc.streetType and sc.streetType >= 0 then t = sc.streetType return end
			end
		end
	end)
	return t or 0
end

local function anyTrackType()
	local t
	pcall(function()
		local m = api.engine.system.streetSystem.getNode2TrackEdgeMap()
		for _, edges in pairs(m) do
			for _, eid in pairs(edges) do
				local tc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK)
				if tc and tc.trackType and tc.trackType >= 0 then t = tc.trackType return end
			end
		end
	end)
	return t or 0
end

local function counts()
	local nc, ne, nv = 0, 0, 0
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false })
		for _ in pairs(ents) do nc = nc + 1 end
	end)
	pcall(function()
		local seen = {}
		for _, mapFn in ipairs({ "getNode2StreetEdgeMap", "getNode2TrackEdgeMap" }) do
			local m = api.engine.system.streetSystem[mapFn]()
			for _, list in pairs(m) do
				for _, eid in ipairs(list) do seen[eid] = true end
			end
		end
		for _ in pairs(seen) do ne = ne + 1 end
	end)
	pcall(function()
		local vs = api.engine.system.transportVehicleSystem.getVehicles() or {}
		for _ in pairs(vs) do nv = nv + 1 end
	end)
	return nc, ne, nv
end

-- ---------- actions ----------

local function actCon(fileName, x, y, params)
	local z = groundAt(x, y)
	local ok, id = pcall(game.interface.buildConstruction, fileName,
		params or {}, transfAt(x, y, z))
	return ok and id ~= nil, string.format("%s at %.1f,%.1f,%.1f -> ok=%s id=%s",
		fileName, x, y, z, tostring(ok), tostring(id))
end

-- known-good params, lifted from a real captured build in tpf2_capture_b.txt
local function actDepot(x, y)
	return actCon("depot/road_depot_era_a.con", x, y,
		{ year = 1850, paramX = 0, paramY = 0 })
end

-- How close an existing node has to be to count as "the same spot". Horizontal
-- only, matching posKey() in mpbridge.lua: two nodes sharing an x/y really are
-- the same node, while stacked geometry (a bridge over a track) does not share
-- endpoints.
local NODE_REUSE_EPS = 2.0

local function nodeMapFor(kind)
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

-- Find an existing node to hang this endpoint off. Restricted to the map
-- matching `kind`: a track edge cannot attach to a street-only node, so
-- searching both maps would hand back a node the proposal then rejects.
local function findNodeNear(kind, x, y)
	local best, bestD, bestPos
	for nid, _ in pairs(nodeMapFor(kind)) do
		local ok, nc = pcall(api.engine.getComponent, nid, api.type.ComponentType.BASE_NODE)
		if ok and nc and nc.position then
			local p = nc.position
			local px, py, pz = p.x or p[1], p.y or p[2], p.z or p[3]
			local dx, dy = px - x, py - y
			local d = dx * dx + dy * dy
			if d < NODE_REUSE_EPS * NODE_REUSE_EPS and (not bestD or d < bestD) then
				best, bestD, bestPos = nid, d, { px, py, pz }
			end
		end
	end
	return best, bestPos
end

-- ---------- mid-span splitting ----------
--
-- MEASURED (probe_midspan.txt, 2026-08-07): buildProposal REJECTS an edge whose
-- endpoint lands in the middle of an existing edge -- edges 9646 before and
-- 9646 after, callback success=false. The interactive track tool splits the
-- edge for you; a raw proposal does not. So a junction has to be built
-- explicitly: remove the old edge, add the two halves either side of a new
-- node, and hang the new edge off that node -- all in ONE proposal, so the
-- network is never momentarily broken.

-- TpF2 edges are cubic Hermite curves: a position and a tangent at each end.
-- Splitting one means evaluating the curve, not lerping the endpoints, or a
-- curved edge visibly changes shape when a junction is added to it.
local function hermitePos(p0, t0, p1, t1, u)
	local u2, u3 = u * u, u * u * u
	local h00, h10 = 2*u3 - 3*u2 + 1, u3 - 2*u2 + u
	local h01, h11 = -2*u3 + 3*u2, u3 - u2
	local r = {}
	for i = 1, 3 do
		r[i] = h00*p0[i] + h10*t0[i] + h01*p1[i] + h11*t1[i]
	end
	return r
end

local function hermiteTangent(p0, t0, p1, t1, u)
	local u2 = u * u
	local g00, g10 = 6*u2 - 6*u, 3*u2 - 4*u + 1
	local g01, g11 = -6*u2 + 6*u, 3*u2 - 2*u
	local r = {}
	for i = 1, 3 do
		r[i] = g00*p0[i] + g10*t0[i] + g01*p1[i] + g11*t1[i]
	end
	return r
end

local function vec3(v)
	if not v then return { 0, 0, 0 } end
	return { v.x or v[1] or 0, v.y or v[2] or 0, v.z or v[3] or 0 }
end

-- position + tangents of an edge, as plain tables
local function edgeGeom(eid)
	local comp = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
	if not comp then return nil end
	local function np(nid)
		local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
		if not nc or not nc.position then return nil end
		return vec3(nc.position)
	end
	local p0, p1 = np(comp.node0), np(comp.node1)
	if not p0 or not p1 then return nil end
	return comp, p0, p1, vec3(comp.tangent0), vec3(comp.tangent1)
end

local SPLIT_EPS = 3.0
-- A hit this close to either end is the endpoint, not a mid-span split;
-- splitting there would leave a degenerate stub edge.
local SPLIT_MIN_U = 0.08

-- Find an existing edge that (x,y) lies on, and where along it.
local function findEdgeContaining(kind, x, y)
	local best, bestD, bestU, seen = nil, nil, nil, {}
	for _, list in pairs(nodeMapFor(kind)) do
		for _, eid in ipairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local comp, p0, p1, t0, t1 = edgeGeom(eid)
				if comp then
					-- cheap reject before sampling: if the point is far from both
					-- ends relative to the span, it cannot be on this edge
					local span = (p1[1]-p0[1])^2 + (p1[2]-p0[2])^2
					local d0 = (p0[1]-x)^2 + (p0[2]-y)^2
					local d1 = (p1[1]-x)^2 + (p1[2]-y)^2
					if d0 < span * 4 + 400 or d1 < span * 4 + 400 then
						for i = 1, 19 do
							local u = i / 20
							local q = hermitePos(p0, t0, p1, t1, u)
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
		dst.streetEdge.streetType = (sc and sc.streetType) or anyStreetType()
		if sc then
			dst.streetEdge.hasBus = sc.hasBus and true or false
			dst.streetEdge.tramTrackType = sc.tramTrackType or 0
		end
	else
		local tc = api.engine.getComponent(srcEid, api.type.ComponentType.BASE_EDGE_TRACK)
		dst.trackEdge = api.type.BaseEdgeTrack.new()
		dst.trackEdge.trackType = (tc and tc.trackType) or anyTrackType()
		dst.trackEdge.catenary = (tc and tc.catenary) and true or false
		-- a valid streetType is mandatory on every edge for validation
		dst.streetEdge = api.type.BaseEdgeStreet.new()
		dst.streetEdge.streetType = anyStreetType()
	end
end

-- Build one edge, REUSING an endpoint node that already exists rather than
-- creating a second node in the same spot, and SPLITTING an existing edge when
-- an endpoint lands mid-span. Without node reuse every segment touching an
-- earlier one was rejected outright -- which is why chained rail and any branch
-- off an existing line never built, and why runs looked like "some rail builds
-- are being completely missed".
local function actEdge(kind, x0, y0, x1, y1)
	local typeId = (kind == "rail") and anyTrackType() or anyStreetType()

	-- Resolve both endpoints before building the proposal. An endpoint that
	-- already carries a node is referenced by its real entity id and must NOT
	-- also appear in nodesToAdd; only genuinely new spots get a placeholder.
	local id0, pos0 = findNodeNear(kind, x0, y0)
	local id1, pos1 = findNodeNear(kind, x1, y1)
	local z0 = pos0 and pos0[3] or groundAt(x0, y0)
	local z1 = pos1 and pos1[3] or groundAt(x1, y1)
	-- snap to the reused node's exact position so the tangent matches the edge
	if pos0 then x0, y0 = pos0[1], pos0[2] end
	if pos1 then x1, y1 = pos1[1], pos1[2] end

	if id0 and id1 and id0 == id1 then
		return false, string.format("%s %.1f,%.1f -> %.1f,%.1f skipped: both ends are node %d",
			kind, x0, y0, x1, y1, id0)
	end

	local sp = api.type.SimpleProposal.new()
	local nAdd, eAdd, eRem = 0, 0, 0
	local splits = {}
	local function addNode(x, y, z)
		nAdd = nAdd + 1
		local n = api.type.NodeAndEntity.new()
		n.entity = -nAdd
		n.comp.position = api.type.Vec3f.new(x, y, z)
		sp.streetProposal.nodesToAdd[nAdd] = n
		return -nAdd
	end
	local function nextEdgeSlot()
		eAdd = eAdd + 1
		return eAdd
	end

	-- If an endpoint has no node but sits on an existing edge, split that edge
	-- here: remove it and re-add both halves around a new node, which then
	-- becomes this endpoint. The halves inherit the original's properties and
	-- Hermite tangents scaled by the split parameter, so the curve is unchanged.
	local function resolveOrSplit(x, y, z)
		local eid, u = findEdgeContaining(kind, x, y)
		if not eid then return addNode(x, y, z) end

		local comp, p0, p1, t0, t1 = edgeGeom(eid)
		if not comp then return addNode(x, y, z) end
		local pm = hermitePos(p0, t0, p1, t1, u)
		local tm = hermiteTangent(p0, t0, p1, t1, u)

		local mid = addNode(pm[1], pm[2], pm[3])
		eRem = eRem + 1
		sp.streetProposal.edgesToRemove[eRem] = eid

		-- sub-curve tangents scale by the length of the parameter interval
		local function half(nA, nB, tA, tB, s)
			local slot = nextEdgeSlot()
			local e = api.type.SegmentAndEntity.new()
			e.entity = -200000 - ticks - slot
			e.comp.node0 = nA
			e.comp.node1 = nB
			e.comp.tangent0 = api.type.Vec3f.new(tA[1]*s, tA[2]*s, tA[3]*s)
			e.comp.tangent1 = api.type.Vec3f.new(tB[1]*s, tB[2]*s, tB[3]*s)
			e.comp.type = 0
			e.comp.typeIndex = (kind == "rail") and -1 or 0
			e.type = (kind == "rail") and 1 or 0
			copyEdgeProps(e, eid, kind)
			sp.streetProposal.edgesToAdd[slot] = e
		end
		half(comp.node0, mid, t0, tm, u)
		half(mid, comp.node1, tm, t1, 1 - u)

		splits[#splits + 1] = string.format("split:%d@%.2f", eid, u)
		return mid, pm
	end

	local ref0, ref1 = id0, id1
	if not ref0 then
		local r, pm = resolveOrSplit(x0, y0, z0)
		ref0 = r
		if pm then x0, y0, z0 = pm[1], pm[2], pm[3] end
	end
	if not ref1 then
		local r, pm = resolveOrSplit(x1, y1, z1)
		ref1 = r
		if pm then x1, y1, z1 = pm[1], pm[2], pm[3] end
	end

	local dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
	local e = api.type.SegmentAndEntity.new()
	e.entity = -100000 - ticks
	e.comp.node0 = ref0
	e.comp.node1 = ref1
	e.comp.tangent0 = api.type.Vec3f.new(dx, dy, dz)
	e.comp.tangent1 = api.type.Vec3f.new(dx, dy, dz)
	e.comp.type = 0
	e.comp.typeIndex = (kind == "rail") and -1 or 0
	-- e.type selects street(0) vs track(1); comp.type must stay 0. See the
	-- matching note in mpbridge.lua -- this was 0 for rail, which is why the
	-- harness kept producing roads.
	e.type = (kind == "rail") and 1 or 0
	if kind == "road" then
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = typeId
	else
		e.trackEdge = api.type.BaseEdgeTrack.new()
		e.trackEdge.trackType = typeId
		-- mandatory for validation (ResTypeRep<StreetType>::Get(-1) asserts
		-- without it) but discarded on the resulting track edge
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = anyStreetType()
	end
	-- must come AFTER any split halves, which have already claimed slots
	sp.streetProposal.edgesToAdd[nextEdgeSlot()] = e

	local sent = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, nil, false),
			function(res, success)
				appendResult(string.format("  %s edge callback success=%s",
					kind, tostring(success)))
			end)
	end)
	return sent, string.format("%s %.1f,%.1f -> %.1f,%.1f type=%d nodes=%s/%s%s sent=%s",
		kind, x0, y0, x1, y1, typeId,
		id0 and ("reuse:" .. tostring(id0)) or "new",
		id1 and ("reuse:" .. tostring(id1)) or "new",
		(#splits > 0) and (" " .. table.concat(splits, ",")) or "",
		tostring(sent))
end

-- Remove an edge by geometry, so a scenario can exercise the removal channel.
-- Matching is by endpoint position in either orientation: node0/node1 order is
-- an internal detail and the scenario should not have to know it.
local EDGE_MATCH_EPS2 = 2.0 * 2.0
local function actDelEdge(kind, x0, y0, x1, y1)
	local target, seen = nil, {}
	for _, list in pairs(nodeMapFor(kind)) do
		for _, eid in ipairs(list) do
			if not seen[eid] then
				seen[eid] = true
				local comp = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
				if comp then
					local function np(nid)
						local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
						return nc and nc.position
					end
					local a, b = np(comp.node0), np(comp.node1)
					if a and b then
						local ax, ay = a.x or a[1], a.y or a[2]
						local bx, by = b.x or b[1], b.y or b[2]
						local d1 = (ax - x0)^2 + (ay - y0)^2 + (bx - x1)^2 + (by - y1)^2
						local d2 = (ax - x1)^2 + (ay - y1)^2 + (bx - x0)^2 + (by - y0)^2
						local d = (d1 < d2) and d1 or d2
						if d < EDGE_MATCH_EPS2 * 2 then target = eid break end
					end
				end
			end
		end
		if target then break end
	end
	if not target then
		return false, string.format("deledge %s %.1f,%.1f -> %.1f,%.1f: no matching edge",
			kind, x0, y0, x1, y1)
	end

	local sp = api.type.SimpleProposal.new()
	sp.streetProposal.edgesToRemove[1] = target
	-- take any endpoint this was the last edge of; a node with no edges is
	-- invalid geometry
	local comp = api.engine.getComponent(target, api.type.ComponentType.BASE_EDGE)
	local m = nodeMapFor(kind)
	if comp then
		local nRem, done = 0, {}
		for _, nid in ipairs({ comp.node0, comp.node1 }) do
			if nid and not done[nid] then
				done[nid] = true
				local cnt = 0
				if m[nid] then for _ in pairs(m[nid]) do cnt = cnt + 1 end end
				if cnt <= 1 then
					nRem = nRem + 1
					sp.streetProposal.nodesToRemove[nRem] = nid
				end
			end
		end
	end

	local sent = pcall(function()
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, nil, false),
			function(res, success)
				appendResult(string.format("  %s deledge callback success=%s",
					kind, tostring(success)))
			end)
	end)
	return sent, string.format("deledge %s id=%d at %.1f,%.1f -> %.1f,%.1f sent=%s",
		kind, target, x0, y0, x1, y1, tostring(sent))
end

-- Edit an existing construction in place, the way the station UI does: rewrite
-- its params and call upgradeConstruction. This does NOT create or destroy an
-- entity, which is exactly why edits were invisible to a capture that only
-- asked "is this id new?" -- so it is the test the CONMOD channel needs.
-- SELFUPGRADE <fileNameSubstring>
--
-- Re-applies a construction's OWN params to itself. That is a no-op edit, so it
-- must succeed on anything the engine will upgrade at all -- which makes it a
-- clean yes/no test of "can this construction be upgraded here", independent of
-- anything our replication does to params in transit.
--
-- The question it exists to answer: modular rail station edits replay as
-- "ok=false err=internal error" on the joiner every time, while depots succeed.
-- Run this on BOTH sides. The joiner's station was created by our BUILD replay
-- (buildConstruction), the host's by the interactive tool, and buildConstruction
-- skips the network integration the tool performs. If the host self-upgrades and
-- the joiner does not, the bug is in how the joiner CREATES the station and no
-- amount of work on the CONMOD params will fix it.
local function actSelfUpgrade(want)
	if not want or want == "" then return false, "selfupgrade needs a filename substring" end
	local found, foundFile
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false })
		for _, id in pairs(ents) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.fileName and co.fileName:find(want, 1, true) then
				found, foundFile = id, co.fileName
				break
			end
		end
	end)
	if not found then return false, "selfupgrade: no construction matching '" .. want .. "'" end

	local e = game.interface.getEntity(found)
	if not e or not e.params then
		return false, "selfupgrade: construction " .. found .. " has no params"
	end
	local own = {}
	for k, v in pairs(e.params) do if k ~= "seed" then own[k] = v end end
	local nMod = 0
	if type(e.params.modules) == "table" then
		for _ in pairs(e.params.modules) do nMod = nMod + 1 end
	end
	local ok, err = pcall(game.interface.upgradeConstruction, found, foundFile, own)
	return ok, string.format("selfupgrade %s id=%d modules=%d -> ok=%s%s",
		foundFile, found, nMod, tostring(ok), ok and "" or (" err=" .. tostring(err)))
end

local function actConEdit(x, y, key, value)
	if not (x and y and key) then return false, "conedit needs x y key value" end
	local num = tonumber(value)

	local best, bestD, bestFile
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "CONSTRUCTION", includeData = false })
		for _, id in pairs(ents) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.transf then
				local dx, dy = co.transf[13] - x, co.transf[14] - y
				local d = dx * dx + dy * dy
				if d < 900 and (not bestD or d < bestD) then
					best, bestD, bestFile = id, d, co.fileName
				end
			end
		end
	end)
	if not best then
		return false, string.format("conedit: nothing within 30m of %.1f,%.1f", x, y)
	end

	local e = game.interface.getEntity(best)
	if not e or not e.params then
		return false, "conedit: construction " .. best .. " has no params"
	end
	local params = e.params
	local before = tostring(params[key])
	params[key] = (num ~= nil) and num or value
	-- reusing a seed drives errorState critical, which is a fatal assert
	params.seed = nil

	local ok, err = pcall(game.interface.upgradeConstruction, best, bestFile, params)
	return ok, string.format("conedit id=%d %s %s: %s -> %s ok=%s%s",
		best, tostring(bestFile), key, before, tostring(params[key]), tostring(ok),
		ok and "" or (" err=" .. tostring(err)))
end

-- ---------- lines ----------

-- Collect station groups, nearest-first from a point, so a line can be built
-- between two real stops rather than arbitrary ids.
local function stationGroups()
	local sgs = {}
	pcall(function()
		local ents = game.interface.getEntities({ radius = 999999 },
			{ type = "STATION_GROUP", includeData = false })
		for _, id in pairs(ents) do sgs[#sgs + 1] = id end
	end)
	return sgs
end

-- MEASURED SHAPE (probe fields, 2026-08-07), from a real line in the save:
--   LINE       { stops = vector, waitingTime = 180, vehicleInfo = userdata }
--   Line.Stop  { stationGroup, station, terminal, loadMode, stopConfig,
--                minWaitingTime, maxWaitingTime, waypoints }
local function buildLineObject(sgA, sgB)
	local line = api.type.Line.new()
	local ok, err = pcall(function()
		line.waitingTime = 180
		local i = 0
		for _, sg in ipairs({ sgA, sgB }) do
			i = i + 1
			local s = api.type.Line.Stop.new()
			s.stationGroup = sg
			s.station = 0
			s.terminal = 0
			s.loadMode = 0
			s.minWaitingTime = 0
			s.maxWaitingTime = 180
			line.stops[i] = s
		end
	end)
	if not ok then return nil, tostring(err) end
	return line
end
--
-- SIGNATURE (official API docs, api.cmd module):
--   api.cmd.make.createLine(name, color, playerEntity, line)
--     name         string
--     color        api.type.Vec3f   -- RGB
--     playerEntity Entity
--     line         api.type.Line
--
-- Worth recording how much the guessing cost: the black-box probe got as far as
-- "argument 1 is a string" and "argument 2 must be userdata", then stalled --
-- passing a CmdData::CreateLine there was rejected as "does not properly
-- reflect the desired type" because what it actually wanted was a Vec3f COLOUR.
-- No amount of further probing would have suggested a colour. Check the docs.
local function actCreateLine(name)
	local sgs = stationGroups()
	if #sgs < 2 then
		return false, "createline: need 2 station groups, found " .. #sgs
	end

	-- report which ones, so a failure can be traced to the stops it used
	local function sgName(sg)
		local e = game.interface.getEntity(sg)
		return (e and e.name) or ("#" .. tostring(sg))
	end
	local a, b = sgs[1], sgs[2]

	local line, err = buildLineObject(a, b)
	if not line then return false, "createline: bad Line object: " .. tostring(err) end

	local lineName = name or "MP Test Line"
	local color = api.type.Vec3f.new(0.9, 0.2, 0.2)
	local player = api.engine.util.getPlayer()

	local created
	local sent, serr = pcall(function()
		api.cmd.sendCommand(
			api.cmd.make.createLine(lineName, color, player, line),
			function(res, success)
				created = success
				appendResult(string.format("  createLine callback success=%s", tostring(success)))
			end)
	end)
	return sent, string.format("createLine '%s' %s <-> %s player=%s sent=%s%s",
		lineName, sgName(a), sgName(b), tostring(player), tostring(sent),
		sent and "" or (" err=" .. tostring(serr)))
end

-- ---------- cross-state buy request ----------
--
-- The engine state writes the request; the GUI state performs the dispatch.
-- They are separate Lua states running the same file, so nothing in memory is
-- shared and the handoff has to go through a file.
local guiTicks = 0
local guiLastDepot = nil    -- depot of the last GUI-state purchase

local function buyReqFile()
	return BASE .. "tpf2_buyreq_" .. tostring(INSTANCE) .. ".txt"
end

local function writeBuyRequest(depot, models, comps)
	local f = io.open(buyReqFile(), "w")
	if not f then return false end
	f:write(string.format("depot=%d models=%s comps=%s\n",
		depot, table.concat(models, ","), table.concat(comps, ",")))
	f:close()
	return true
end

-- DIAGNOSTIC: does a DIFFERENT vehicle command also wedge the sim, or is it
-- buyVehicle specifically?
--
-- Dispatching from the GUI state proved the caller does not deadlock
-- (sendCommand returns ok=true) yet the sim still dies processing the command.
-- That points at the command/config rather than the call site. setLine takes
-- only entity ids -- no config to get wrong -- so if setLine survives, the
-- fault is in the TransportVehicleConfig; if setLine wedges too, the whole
-- vehicle command family is unusable from a script and the answer is native.
--
-- Reassigning an existing vehicle between two existing lines is reversible and
-- harmless on a test save.
local function setLineProbeFile()
	return BASE .. "tpf2_setlinereq_" .. tostring(INSTANCE) .. ".txt"
end

local function drainSetLineProbe()
	local path = setLineProbeFile()
	local f = io.open(path, "r")
	if not f then return end
	-- Require CONTENT, not just existence. os.remove may be unavailable in the
	-- game's Lua sandbox, in which case the fallback below blanks the file --
	-- and a blank file still opens, so an existence-only check re-fired this
	-- probe on every GUI tick and spammed setLine commands until the file was
	-- deleted from outside.
	local body = f:read("*l")
	f:close()
	if not body or body == "" then return end
	if not pcall(function() os.remove(path) end) then
		local w = io.open(path, "w"); if w then w:close() end
	end

	local veh, curLine, other
	pcall(function()
		local lines = {}
		for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
			lines[#lines + 1] = lid
		end
		table.sort(lines)
		for _, lid in ipairs(lines) do
			for _, vid in pairs(api.engine.system.transportVehicleSystem.getLineVehicles(lid) or {}) do
				if not veh then veh, curLine = vid, lid end
			end
		end
		for _, lid in ipairs(lines) do
			if lid ~= curLine then other = other or lid end
		end
	end)
	if not (veh and other) then
		appendResult("  [GUI STATE] setLine probe: no vehicle/second line to use")
		return
	end
	appendResult(string.format(
		"  [GUI STATE] setLine probe: moving vehicle %d from line %s to line %d",
		veh, tostring(curLine), other))
	local sent, serr = pcall(function()
		api.cmd.sendCommand(api.cmd.make.setLine(veh, other, 0), function(res, success)
			appendResult("  [GUI STATE] setLine callback success=" .. tostring(success))
		end)
	end)
	appendResult("  [GUI STATE] setLine sendCommand ok=" .. tostring(sent)
		.. (sent and "" or (" err=" .. tostring(serr))))
end

local function assignReqFile()
	return BASE .. "tpf2_assignreq_" .. tostring(INSTANCE) .. ".txt"
end

-- Runs in the GUI state, where guiLastDepot lives.
local function drainAssignRequest()
	local path = assignReqFile()
	local f = io.open(path, "r")
	if not f then return end
	local body = f:read("*l")
	f:close()
	if not pcall(function() os.remove(path) end) then
		local w = io.open(path, "w"); if w then w:close() end
	end
	if not body or body == "" then return end

	if not guiLastDepot then
		appendResult("  [GUI STATE] assign: nothing bought in this state yet")
		return
	end
	-- A vehicle sitting in a depot is not a world VEHICLE entity, which is why
	-- getEntities never found one. transportVehicleSystem.getDepotVehicles is
	-- the accessor that does. Highest id == most recently bought.
	local veh
	pcall(function()
		local best
		for _, vid in pairs(api.engine.system.transportVehicleSystem
		                    .getDepotVehicles(guiLastDepot) or {}) do
			if not best or vid > best then best = vid end
		end
		veh = best
	end)
	if not veh then
		appendResult("  [GUI STATE] assign: depot " .. tostring(guiLastDepot)
			.. " reports no vehicles")
		return
	end
	local lines = {}
	pcall(function()
		for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
			lines[#lines + 1] = lid
		end
	end)
	if #lines == 0 then
		appendResult("  [GUI STATE] assign: no lines exist")
		return
	end
	table.sort(lines)
	local lid = lines[#lines]        -- highest id == most recently created
	appendResult(string.format("  [GUI STATE] assign: vehicle %s (from depot %s) -> line %d",
		tostring(veh), tostring(guiLastDepot), lid))
	local sent, serr = pcall(function()
		api.cmd.sendCommand(api.cmd.make.setLine(veh, lid, 0),
			function(res, success)
				appendResult("  [GUI STATE] assign setLine callback success=" .. tostring(success))
			end)
	end)
	appendResult("  [GUI STATE] assign sendCommand ok=" .. tostring(sent)
		.. (sent and "" or (" err=" .. tostring(serr))))
end

-- Runs in the GUI state.
local function drainBuyRequest()
	local path = buyReqFile()
	local f = io.open(path, "r")
	if not f then return end
	local line = f:read("*l")
	f:close()
	-- Consume BEFORE acting, so a wedge cannot re-trigger the same request on
	-- every gui tick. `os` may be trimmed in the game's Lua sandbox (see the
	-- note at the top of this file), so blanking the file is the fallback.
	if not pcall(function() os.remove(path) end) then
		local w = io.open(path, "w"); if w then w:close() end
	end
	if not line then return end

	local depot = tonumber(line:match("depot=(%d+)"))
	local mstr = line:match("models=([%d,]+)")
	local cstr = line:match("comps=([%d,]+)")
	if not (depot and mstr) then return end

	local models, comps = {}, {}
	for m in mstr:gmatch("[^,]+") do models[#models + 1] = tonumber(m) end
	for c in (cstr or ""):gmatch("[^,]+") do comps[#comps + 1] = tonumber(c) end

	appendResult(string.format("  [GUI STATE] dispatching buyVehicle: depot=%d models=%s",
		depot, mstr))

	local config = api.type.TransportVehicleConfig.new()
	local nUnits = 0
	local built = pcall(function()
		for i, mid in ipairs(models) do
			local nc = (comps[i] and comps[i] > 0) and comps[i] or 1
			local part = api.type.VehiclePart.new()
			part.modelId = mid
			-- Set EVERY field the reference config has. reversed/color/logo were
			-- left at whatever .new() produces, and an uninitialised Vec3f colour
			-- is the last structural difference from a vehicle the game built
			-- itself (which shows reversed=false, color=<userdata>, logo="").
			part.reversed = false
			-- -1,-1,-1 is the "no custom colour" sentinel. A manual UI purchase
			-- passes bf800000 in all three lanes; 1,1,1 was my invention.
			part.color = api.type.Vec3f.new(-1, -1, -1)
			part.logo = ""
			-- 0, NOT -1. The docs describe -1 as "choose automatically", but a
			-- config dumped from a vehicle the GAME built uses loadConfig={0};
			-- -1 never appears. Taking the doc wording at face value here was a
			-- regression -- mpbridge.lua already had 0.
			local lc = part.loadConfig
			for c = 1, nc do lc[c] = 0 end
			part.loadConfig = lc
			local tvp = api.type.TransportVehiclePart.new()
			tvp.part = part
			-- MILLISECONDS. A game-built vehicle showed purchaseTime=34699000
			-- against a current getGameTime().time of 55234 -- it cannot have
			-- been bought in the future, so the field is time*1000.
			tvp.purchaseTime = game.interface.getGameTime().time * 1000
			tvp.maintenanceState = 1.0
			tvp.targetMaintenanceState = 0
			-- READ-MODIFY-WRITE, do not index the member in place.
			-- A byte dump of the command the GAME dispatches shows a third
			-- std::vector at +0x60 of the unit struct holding one element,
			-- while ours was entirely NULL there -- so `tvp.autoLoadConfig[c] = 1`
			-- was mutating a temporary copy sol2 handed back and throwing it
			-- away. A null vector the engine then iterates is exactly the sort
			-- of thing that wedges the sim thread uncatchably.
			local alc = tvp.autoLoadConfig
			for c = 1, nc do alc[c] = 1 end
			tvp.autoLoadConfig = alc
			nUnits = nUnits + 1
			config.vehicles[nUnits] = tvp
		end
		config.vehicleGroups[1] = nUnits
	end)
	if not built or nUnits ~= #models then
		appendResult("  [GUI STATE] refused: incomplete config")
		return
	end

	local sent, serr = pcall(function()
		api.cmd.sendCommand(
			api.cmd.make.buyVehicle(api.engine.util.getPlayer(), depot, config),
			function(res, success)
				appendResult(string.format("  [GUI STATE] buyVehicle callback success=%s",
					tostring(success)))
				-- NOTE: res is a CmdData::BuyVehicle -- the command data, not the
				-- new entity. It carries no result id (every candidate field came
				-- back nil), and setLine rejected it outright: "expected number,
				-- received sol.CmdData::BuyVehicle". The vehicle is found by
				-- asking the DEPOT instead, via getDepotVehicles.
				if success then guiLastDepot = depot end
			end)
	end)
	appendResult(string.format("  [GUI STATE] sendCommand returned ok=%s%s -- "
		.. "reaching this line at all means the GUI state did NOT deadlock",
		tostring(sent), sent and "" or (" err=" .. tostring(serr))))
end

-- ---------- vehicles ----------
--
-- buyVehicle(playerEntity, depotEntity, config) is the call that has twice
-- taken the process down, and neither failure was catchable from Lua:
--   * an empty loadConfig            -> SIGABRT
--   * vehicleGroups not summing to
--     the vehicle count              -> hang
-- So the model is NOT guessed. It is copied from a vehicle already running in
-- this save, which guarantees a real modelId and the right compartment count,
-- and the config is refused unless every piece is present. Documented field
-- list (api.type docs):
--   TransportVehicleConfig { vehicles = {TransportVehiclePart,...},
--                            vehicleGroups = {int,...} }  -- groups must sum
--   TransportVehiclePart   { part, purchaseTime, maintenanceState,
--                            targetMaintenanceState, autoLoadConfig }
--   VehiclePart            { modelId, reversed, loadConfig, color, logo }
--                            loadConfig -1 == choose automatically
-- Set from the buyVehicle callback so ASSIGN can act on the vehicle we just
-- bought. A freshly bought vehicle sits in its depot doing nothing until it is
-- given a line, so buy-then-assign is the whole "get a vehicle going" chain.
local lastBought = nil

local function findSampleVehicle()
	local veh
	pcall(function()
		for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
			for _, vid in pairs(api.engine.system.transportVehicleSystem.getLineVehicles(lid) or {}) do
				veh = veh or vid
			end
			if veh then return end
		end
	end)
	return veh
end

-- `units` defaults to 1. A single unit is the smallest thing that proves the
-- channel, and the 26-unit consist that wedged the sim is exactly what happens
-- when this is left to copy whatever the sample vehicle happened to be.
local function actBuyVehicle(units)
	local want = tonumber(units) or 1
	if want < 1 then want = 1 end
	local veh = findSampleVehicle()
	if not veh then return false, "buyveh: no existing vehicle to copy a model from" end

	local vc = api.engine.getComponent(veh, api.type.ComponentType.TRANSPORT_VEHICLE)
	if not vc then return false, "buyveh: sample vehicle has no TRANSPORT_VEHICLE" end
	local depot = vc.depot
	if not depot or depot < 0 then
		return false, "buyveh: sample vehicle reports no depot (" .. tostring(depot) .. ")"
	end

	-- read the sample's models and compartment counts
	local models, comps = {}, {}
	local read = pcall(function()
		local src = vc.transportVehicleConfig
		for i = 1, #src.vehicles do
			local part = src.vehicles[i].part
			models[#models + 1] = part.modelId
			local n = 0
			pcall(function() n = #part.loadConfig end)
			comps[#comps + 1] = n
		end
	end)
	if not read or #models == 0 then
		return false, "buyveh: could not read the sample vehicle's config"
	end

	-- log what we are about to do BEFORE dispatching: appendResult flushes on
	-- every call, so this line survives even if the command kills the process
	appendResult(string.format("  buyveh: sample vehicle %d reports %d unit(s), depot %d, comps=%s",
		veh, #models, depot, table.concat(comps, ",")))

	-- HARD CAP, added after a measured wedge. Copying a whole 26-part consist
	-- hung instance A's SIM thread outright at tick 452 -- and note the process
	-- kept answering the UI and reported Responding=True the whole time, so it
	-- looked healthy while its world had stopped. Never dispatch a config this
	-- test cannot justify; one unit is all that is needed to prove the channel.
	local MAX_UNITS = 4
	local keep = math.min(want, MAX_UNITS, #models)
	if #models > keep then
		appendResult(string.format(
			"  buyveh: trimming %d unit(s) to %d -- a large consist is not a test payload",
			#models, keep))
		while #models > keep do table.remove(models) end
		while #comps > keep do table.remove(comps) end
	end

	local config = api.type.TransportVehicleConfig.new()
	local nUnits = 0
	local built = pcall(function()
		for i, mid in ipairs(models) do
			local part = api.type.VehiclePart.new()
			part.modelId = mid
			-- Set EVERY field the reference config has. reversed/color/logo were
			-- left at whatever .new() produces, and an uninitialised Vec3f colour
			-- is the last structural difference from a vehicle the game built
			-- itself (which shows reversed=false, color=<userdata>, logo="").
			part.reversed = false
			-- -1,-1,-1 is the "no custom colour" sentinel. A manual UI purchase
			-- passes bf800000 in all three lanes; 1,1,1 was my invention.
			part.color = api.type.Vec3f.new(-1, -1, -1)
			part.logo = ""
			-- never leave loadConfig empty; -1 is documented as "automatic"
			local nc = (comps[i] and comps[i] > 0) and comps[i] or 1
			-- 0, NOT -1. The docs describe -1 as "choose automatically", but a
			-- config dumped from a vehicle the GAME built uses loadConfig={0};
			-- -1 never appears. Taking the doc wording at face value here was a
			-- regression -- mpbridge.lua already had 0.
			local lc = part.loadConfig
			for c = 1, nc do lc[c] = 0 end
			part.loadConfig = lc

			local tvp = api.type.TransportVehiclePart.new()
			tvp.part = part
			-- MILLISECONDS. A game-built vehicle showed purchaseTime=34699000
			-- against a current getGameTime().time of 55234 -- it cannot have
			-- been bought in the future, so the field is time*1000.
			tvp.purchaseTime = game.interface.getGameTime().time * 1000
			tvp.maintenanceState = 1.0
			tvp.targetMaintenanceState = 0
			-- READ-MODIFY-WRITE, do not index the member in place.
			-- A byte dump of the command the GAME dispatches shows a third
			-- std::vector at +0x60 of the unit struct holding one element,
			-- while ours was entirely NULL there -- so `tvp.autoLoadConfig[c] = 1`
			-- was mutating a temporary copy sol2 handed back and throwing it
			-- away. A null vector the engine then iterates is exactly the sort
			-- of thing that wedges the sim thread uncatchably.
			local alc = tvp.autoLoadConfig
			for c = 1, nc do alc[c] = 1 end
			tvp.autoLoadConfig = alc

			nUnits = nUnits + 1
			config.vehicles[nUnits] = tvp
		end
		-- REQUIRED: the group sizes must sum to the vehicle count
		config.vehicleGroups[1] = nUnits
	end)
	if not built or nUnits ~= #models then
		return false, string.format(
			"buyveh: REFUSING to send an incomplete config (%d/%d units) -- "
			.. "an incomplete one aborts the process", nUnits, #models)
	end

	-- DO NOT dispatch from here. Sending buyVehicle from the engine-state tick
	-- wedges the sim -- measured twice, with 26 units and with 1. Hand the
	-- request to the GUI state instead and let guiUpdate dispatch it; if that
	-- also wedges, the deadlock theory is wrong and the config is suspect after
	-- all. Either way this step returns immediately and the scenario continues,
	-- so we get a result line rather than silence.
	local ok = writeBuyRequest(depot, models, comps)
	return ok, string.format(
		"buyveh QUEUED for the GUI state: depot=%d units=%d models=%s written=%s",
		depot, nUnits, table.concat(models, ","), tostring(ok))
end

-- ---------- API introspection ----------
--
-- Guessing at sol2 signatures has crashed this game twice (an empty loadConfig
-- SIGABRTs, a missing vehicleGroups hangs) and neither is catchable from Lua.
-- So: ask the bindings what they actually expose, in-game, and write it to the
-- results file. Slower than guessing, but it cannot take the process down.

local function dumpKeys(label, t)
	local ok, err = pcall(function()
		local names, n = {}, 0
		for k, v in pairs(t) do
			n = n + 1
			names[#names + 1] = tostring(k) .. ":" .. type(v)
		end
		table.sort(names)
		-- keep lines short enough to survive the 1024-byte chunked transport
		local line, out = "", {}
		for _, s in ipairs(names) do
			if #line + #s > 400 then out[#out + 1] = line; line = "" end
			line = line .. s .. " "
		end
		out[#out + 1] = line
		appendResult(string.format("  %s (%d entries):", label, n))
		for _, l in ipairs(out) do appendResult("      " .. l) end
	end)
	if not ok then
		appendResult(string.format("  %s: NOT ENUMERABLE (%s)", label, tostring(err)))
	end
end

-- A sol2 usertype usually refuses pairs() on the instance; the metatable is
-- where the bound members live, so try both.
local function dumpType(label, path)
	local ok, ctor = pcall(function()
		local t = api.type
		for part in path:gmatch("[^%.]+") do t = t[part] end
		return t
	end)
	if not ok or not ctor then
		appendResult("  api.type." .. path .. ": MISSING")
		return
	end
	appendResult("  api.type." .. path .. ": " .. type(ctor))
	local made, obj = pcall(function() return ctor.new() end)
	if made and obj then
		dumpKeys(label .. ".instance", obj)
		local mt = getmetatable(obj)
		if mt then dumpKeys(label .. ".metatable", mt) end
	else
		appendResult("  " .. label .. ": .new() failed (" .. tostring(obj) .. ")")
	end
end

-- sol2 usertypes refuse pairs(), so members cannot be listed -- but they CAN be
-- read by name. Probe a candidate list against a real instance and report what
-- actually resolves. A real object from the running world beats a blank .new(),
-- because it also shows plausible values and container lengths.
local function tryFields(label, obj, names)
	local found = {}
	for _, n in ipairs(names) do
		local ok, v = pcall(function() return obj[n] end)
		if ok and v ~= nil then
			local d = type(v)
			if d == "number" or d == "boolean" or d == "string" then
				d = tostring(v)
			elseif d == "userdata" or d == "table" then
				local cnt
				pcall(function() cnt = #v end)
				d = d .. (cnt and ("[#" .. cnt .. "]") or "")
			end
			found[#found + 1] = n .. "=" .. d
		end
	end
	if #found == 0 then
		appendResult("  " .. label .. ": none of the candidates resolved")
		return
	end
	local line, out = "", {}
	for _, s in ipairs(found) do
		if #line + #s > 400 then out[#out + 1] = line; line = "" end
		line = line .. s .. "  "
	end
	out[#out + 1] = line
	appendResult("  " .. label .. ":")
	for _, l in ipairs(out) do appendResult("      " .. l) end
end

local LINE_FIELDS = { "stops", "waitingTime", "vehicleInfo", "name", "waypoints" }
local STOP_FIELDS = { "stationGroup", "station", "terminal", "loadMode", "stopConfig",
	"minWaitingTime", "maxWaitingTime", "waypoints", "alighting", "boarding",
	"param", "attachedTo" }
local VEH_FIELDS = { "line", "stopIndex", "state", "depot", "name", "config",
	"transportVehicleConfig", "carrier", "sellPrice", "userStopped" }

local function actProbeFields()
	local lines = {}
	pcall(function()
		for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
			lines[#lines + 1] = lid
		end
	end)
	appendResult("  lines in world: " .. #lines)
	if #lines == 0 then return true, "probe fields (no lines)" end

	local lid = lines[1]
	local lc = api.engine.getComponent(lid, api.type.ComponentType.LINE)
	if not lc then
		appendResult("  line " .. lid .. " has no LINE component")
		return true, "probe fields"
	end
	tryFields("LINE " .. lid, lc, LINE_FIELDS)

	-- walk into the first stop: this is the shape createLine has to be handed
	pcall(function()
		local stops = lc.stops
		local n = #stops
		appendResult("  stop count: " .. n)
		if n > 0 then
			tryFields("stops[1]", stops[1], STOP_FIELDS)
			pcall(function()
				local sc = stops[1].stopConfig
				if sc then tryFields("stops[1].stopConfig", sc, STOP_FIELDS) end
			end)
		end
	end)

	-- and a vehicle actually assigned to that line
	pcall(function()
		local vs = api.engine.system.transportVehicleSystem.getLineVehicles(lid) or {}
		local shown = 0
		for _, vid in pairs(vs) do
			if shown < 1 then
				shown = shown + 1
				local vc = api.engine.getComponent(vid, api.type.ComponentType.TRANSPORT_VEHICLE)
				appendResult("  vehicle " .. vid .. " on line " .. lid .. ":")
				if vc then tryFields("    TRANSPORT_VEHICLE", vc, VEH_FIELDS) end
			end
		end
		if shown == 0 then appendResult("  no vehicles on line " .. lid) end
	end)
	return true, "probe fields"
end

-- Find createLine's signature by CONSTRUCTING the command and never sending it.
-- An arity mismatch raises a catchable Lua error; actually dispatching a
-- malformed command is what has hard-crashed this game before.
local function actProbeCreateLine()
	local sgs = stationGroups()
	appendResult("  station groups: " .. #sgs)
	if #sgs < 2 then return true, "probe createline: need 2 station groups" end

	local line, err = buildLineObject(sgs[1], sgs[2])
	if not line then
		appendResult("  could not build Line object: " .. tostring(err))
		return true, "probe createline"
	end
	local n = -1
	pcall(function() n = #line.stops end)
	appendResult("  Line object built, stops=" .. n)

	local player = api.engine.util.getPlayer()
	appendResult("  player entity = " .. tostring(player))

	-- FIRST ROUND SAID: "stack index 2, expected string", and the type it
	-- reported was that of the FIRST argument passed (number for player,
	-- sol-userdata for line). So sol counts the first user argument as stack
	-- index 2, and that argument must be a STRING -- the line name.
	local NAME = "MP Test Line"
	local attempts = {
		{ 'createLine(name, line)',          function() return api.cmd.make.createLine(NAME, line) end },
		{ 'createLine(name, player, line)',  function() return api.cmd.make.createLine(NAME, player, line) end },
		{ 'createLine(name, line, player)',  function() return api.cmd.make.createLine(NAME, line, player) end },
		{ 'createLine(name)',                function() return api.cmd.make.createLine(NAME) end },
		{ 'createLine(name, line, true)',    function() return api.cmd.make.createLine(NAME, line, true) end },
	}
	for _, a in ipairs(attempts) do
		local ok2, res = pcall(a[2])
		if ok2 then
			appendResult(string.format("  %-30s ACCEPTED -> %s", a[1], type(res)))
		else
			-- keep the whole message: the type name in it is the actual signal
			local msg = tostring(res):gsub("^.-mptest%.lua\"?%]?:%d+:%s*", "")
			appendResult(string.format("  %-30s err=%s", a[1], msg:sub(1, 200)))
		end
	end
	return true, "probe createline"
end

-- Try a list of candidate calls, reporting which the bindings accept. Each
-- costs one line of output and no risk: command FACTORIES only build a command
-- object, they do not dispatch it.
local function tryCalls(label, attempts)
	appendResult("  " .. label .. ":")
	for _, a in ipairs(attempts) do
		local ok, res = pcall(a[2])
		if ok then
			appendResult(string.format("      %-34s ACCEPTED -> %s", a[1], type(res)))
		else
			local msg = tostring(res):gsub("^.-mptest%.lua\"?%]?:%d+:%s*", "")
			appendResult(string.format("      %-34s err=%s", a[1], msg:sub(1, 160)))
		end
	end
end

-- Everything still unknown about the vehicle/line pipeline, in ONE run: each
-- full launch-and-load cycle costs about ten minutes, so discovering these one
-- at a time is the expensive way to do it.
-- api.cmd.make.createLine came back as a TABLE, not a function, and calling it
-- complains that stack index 3 must be a userdata from
--   std::variant<CmdData::SetGameSpeed, ..., CmdData::CreateLine, ...>
-- So the factory is (name, <CmdData payload>) and the payload is its own type.
-- Find where that type lives.
local function actProbeCmdShape()
	local mk = api.cmd.make.createLine
	appendResult("  api.cmd.make.createLine is a " .. type(mk))
	dumpKeys("createLine table", mk)
	local mt = getmetatable(mk)
	if mt then dumpKeys("createLine metatable", mt) end

	-- where might CmdData live?
	for _, path in ipairs({ "CmdData", "CreateLine", "cmd", "Cmd" }) do
		local ok, v = pcall(function() return api.type[path] end)
		appendResult(string.format("  api.type.%-12s = %s", path,
			(ok and v ~= nil) and type(v) or "nil"))
		if ok and type(v) == "table" then dumpKeys("api.type." .. path, v) end
	end
	local ok2, v2 = pcall(function() return api.cmd.CmdData end)
	appendResult("  api.cmd.CmdData = " .. ((ok2 and v2 ~= nil) and type(v2) or "nil"))
	if ok2 and type(v2) == "table" then dumpKeys("api.cmd.CmdData", v2) end
	dumpKeys("api.cmd", api.cmd)
	return true, "probe cmdshape"
end

-- api.type.CreateLine turned out to exist and to have .new() -- that is the
-- CmdData payload createLine wants as its second argument. Find its members.
local function actProbeCreateLine2()
	local ok, obj = pcall(function() return api.type.CreateLine.new() end)
	if not ok or not obj then
		appendResult("  api.type.CreateLine.new() failed: " .. tostring(obj))
		return true, "probe createline2"
	end
	appendResult("  CreateLine.new() -> " .. type(obj))
	tryFields("CreateLine", obj, {
		"line", "stops", "name", "waitingTime", "vehicleInfo",
		"player", "playerEntity", "entity", "id", "lineEntity",
	})

	-- Now try the real call with the payload, still WITHOUT dispatching.
	local sgs = stationGroups()
	local line = (#sgs >= 2) and buildLineObject(sgs[1], sgs[2]) or nil
	tryCalls("createLine with CreateLine payload", {
		{ "createLine(name, CreateLine)", function()
			return api.cmd.make.createLine("MP Test Line", api.type.CreateLine.new()) end },
		{ "createLine(name, cl{line=line})", function()
			local cl = api.type.CreateLine.new()
			if line then cl.line = line end
			return api.cmd.make.createLine("MP Test Line", cl) end },
	})
	return true, "probe createline2"
end

-- Dump a REAL vehicle's TransportVehicleConfig, field by field.
--
-- I had assumed a known-good config needed a fresh manual purchase with the
-- native hook logging it. Not so: this save is full of vehicles the game itself
-- built, and their configs are exactly the reference we need. This is all
-- reads -- nothing is dispatched -- so it cannot wedge the sim, unlike every
-- other approach to this problem so far.
--
-- The point is to diff these values against what actBuyVehicle constructs and
-- find the field the engine rejects.
local VPART_FIELDS = { "modelId", "reversed", "color", "logo" }
local TVPART_FIELDS = { "purchaseTime", "maintenanceState", "targetMaintenanceState" }

local function actProbeVehCfg()
	local veh = findSampleVehicle()
	if not veh then return false, "vehcfg: no vehicle to inspect" end
	local vc = api.engine.getComponent(veh, api.type.ComponentType.TRANSPORT_VEHICLE)
	if not vc then return false, "vehcfg: no TRANSPORT_VEHICLE component" end

	appendResult("  reference vehicle " .. veh .. " (built by the game itself)")
	local cfg = vc.transportVehicleConfig
	if not cfg then return false, "vehcfg: no transportVehicleConfig" end

	-- vehicleGroups: the field the docs single out as mandatory
	pcall(function()
		local g = {}
		for i = 1, #cfg.vehicleGroups do g[#g + 1] = tostring(cfg.vehicleGroups[i]) end
		appendResult(string.format("  vehicleGroups = {%s}   (#vehicles = %d)",
			table.concat(g, ","), #cfg.vehicles))
	end)

	-- first two units only; 26 of them would bury the interesting part
	for i = 1, math.min(2, (function() local n = 0 pcall(function() n = #cfg.vehicles end) return n end)()) do
		pcall(function()
			local tvp = cfg.vehicles[i]
			appendResult("  --- vehicles[" .. i .. "] (TransportVehiclePart) ---")
			tryFields("      tvp", tvp, TVPART_FIELDS)
			pcall(function()
				local a = {}
				for k = 1, #tvp.autoLoadConfig do a[#a + 1] = tostring(tvp.autoLoadConfig[k]) end
				appendResult("      autoLoadConfig = {" .. table.concat(a, ",") .. "}")
			end)
			local part = tvp.part
			tryFields("      part", part, VPART_FIELDS)
			pcall(function()
				local l = {}
				for k = 1, #part.loadConfig do l[#l + 1] = tostring(part.loadConfig[k]) end
				appendResult("      loadConfig = {" .. table.concat(l, ",") .. "}")
			end)
		end)
	end

	-- and what WE build, for the same model, so the diff is right here
	appendResult("  --- what actBuyVehicle constructs for comparison ---")
	pcall(function()
		local t = game.interface.getGameTime()
		appendResult(string.format("      our purchaseTime would be %s (getGameTime().time)",
			tostring(t and t.time)))
		appendResult("      our maintenanceState=1.0 targetMaintenanceState=0")
		appendResult("      our loadConfig={-1}  autoLoadConfig={1}")
		appendResult("      our reversed/color/logo: NOT SET")
	end)
	return true, "probe vehcfg"
end

-- Is this instance broke? Replication charges BOTH sides -- every construction
-- replayed from the peer is paid for locally too -- so a joiner that has
-- mirrored hundreds of builds can be deep in debt and simply unable to build,
-- which looks exactly like "replication is broken".
local function actProbeMoney()
	local player = api.engine.util.getPlayer()
	appendResult("  player entity = " .. tostring(player))
	local e = game.interface.getEntity(player)
	if e then
		tryFields("player entity", e, { "name", "money", "balance", "id", "type" })
	end
	for _, ct in ipairs({ "ACCOUNT", "PLAYER", "COMPANY" }) do
		local ok, c = pcall(function()
			return api.engine.getComponent(player, api.type.ComponentType[ct])
		end)
		if ok and c then
			tryFields("component " .. ct, c, { "money", "balance", "amount", "loan", "value" })
		end
	end
	pcall(function()
		local j = game.interface.getPlayerJournal(player)
		if j then
			local n = 0
			for _ in pairs(j) do n = n + 1 end
			appendResult("  journal entries: " .. n)
		end
	end)
	local nc, ne, nv = counts()
	appendResult(string.format("  world: constructions=%d edges=%d", nc, ne))
	return true, "probe money"
end

local function actProbeApi2()
	local sgs = stationGroups()
	appendResult("  station groups: " .. #sgs)

	-- Station groups have to be addressed by POSITION on the wire (entity ids
	-- are not comparable across instances), so find out how to get one.
	if #sgs > 0 then
		local sg = sgs[1]
		local e = game.interface.getEntity(sg)
		if e then
			tryFields("stationGroup entity " .. sg, e,
				{ "position", "name", "stations", "town", "id", "type" })
		else
			appendResult("  getEntity(stationGroup) returned nil")
		end
		local c = api.engine.getComponent(sg, api.type.ComponentType.STATION_GROUP)
		if c then tryFields("STATION_GROUP component", c, { "stations", "name" }) end
	end

	-- A depot and any vehicle, to work out the buy/assign calls
	local depot, veh, lineId
	pcall(function()
		for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
			lineId = lineId or lid
			for _, vid in pairs(api.engine.system.transportVehicleSystem.getLineVehicles(lid) or {}) do
				veh = veh or vid
			end
		end
	end)
	pcall(function()
		local vc = veh and api.engine.getComponent(veh, api.type.ComponentType.TRANSPORT_VEHICLE)
		if vc then depot = vc.depot end
	end)
	appendResult(string.format("  sample line=%s vehicle=%s depot=%s",
		tostring(lineId), tostring(veh), tostring(depot)))

	if veh and lineId then
		tryCalls("setLine", {
			{ "setLine(veh, line)",        function() return api.cmd.make.setLine(veh, lineId) end },
			{ "setLine(veh, line, 0)",     function() return api.cmd.make.setLine(veh, lineId, 0) end },
			{ "setLine(veh, line, 0, 0)",  function() return api.cmd.make.setLine(veh, lineId, 0, 0) end },
		})
		tryCalls("sendToDepot", {
			{ "sendToDepot(veh)",          function() return api.cmd.make.sendToDepot(veh) end },
			{ "sendToDepot(veh, false)",   function() return api.cmd.make.sendToDepot(veh, false) end },
		})
	end

	if depot then
		-- buyVehicle is the one that has crashed the game; only the FACTORY is
		-- exercised here, never sendCommand.
		local player = api.engine.util.getPlayer()
		tryCalls("buyVehicle", {
			{ "buyVehicle(player, depot, cfg)",
			  function() return api.cmd.make.buyVehicle(player, depot,
				api.type.TransportVehicleConfig.new()) end },
			{ "buyVehicle(depot, cfg)",
			  function() return api.cmd.make.buyVehicle(depot,
				api.type.TransportVehicleConfig.new()) end },
		})
	end
	return true, "probe api2"
end

local function actProbe(what)
    what = (what or "all"):lower()

    if what == "fields" then return actProbeFields() end
    if what == "createline" then return actProbeCreateLine() end
    if what == "api2" then return actProbeApi2() end
    if what == "money" then return actProbeMoney() end
    if what == "vehcfg" then return actProbeVehCfg() end
    if what == "cmdshape" then return actProbeCmdShape() end
    if what == "createline2" then return actProbeCreateLine2() end

    if what == "all" or what == "cmd" then
        dumpKeys("api.cmd.make", api.cmd.make)
    end
    if what == "all" or what == "line" then
        dumpType("Line", "Line")
        dumpType("LineStop", "LineStop")
    end
    if what == "all" or what == "veh" then
        dumpType("TransportVehicleConfig", "TransportVehicleConfig")
        dumpType("VehiclePart", "VehiclePart")
    end
    if what == "all" or what == "world" then
        -- a real instance of anything is worth more than a constructed blank
        local shown = 0
        pcall(function()
            for _, lid in pairs(api.engine.system.lineSystem.getLines() or {}) do
                if shown < 2 then
                    shown = shown + 1
                    local l = api.engine.getComponent(lid, api.type.ComponentType.LINE)
                    appendResult("  existing line " .. tostring(lid) .. ":")
                    if l then dumpKeys("    LINE component", l) end
                end
            end
        end)
        if shown == 0 then appendResult("  no existing lines in this save") end
        -- what station groups exist to build a line between
        pcall(function()
            local sgs = game.interface.getEntities({ radius = 999999 },
                { type = "STATION_GROUP", includeData = false })
            local n = 0
            for _ in pairs(sgs) do n = n + 1 end
            appendResult("  station groups on map: " .. n)
        end)
    end
    return true, "probe " .. what
end

-- ---------- scenario ----------

local function parseScenario(src)
	local out = {}
	for line in src:gmatch("[^\r\n]+") do
		line = line:match("^%s*(.-)%s*$")
		if #line > 0 and line:sub(1, 1) ~= "#" then
			local words = {}
			for w in line:gmatch("%S+") do words[#words + 1] = w end
			out[#out + 1] = words
		end
	end
	return out
end

local function loadScenario()
	local f = io.open(SCENARIO_FILE, "r")
	if not f then return end
	local src = f:read("*a")
	f:close()
	if not src or #src == 0 then return end
	local sig = #src .. ":" .. (src:sub(1, 40))
	if sig == lastScenario then return end
	lastScenario = sig
	steps = parseScenario(src)
	stepIdx = 0
	waitUntil = 0
	appendResult(string.format("=== scenario start (%d steps) at %s ===",
		#steps, ticks))
	log("loaded scenario with " .. #steps .. " step(s)")
end

local function runStep(w)
	local op = w[1]:upper()
	local function n(i) return tonumber(w[i]) end

	if op == "WAIT" then
		waitUntil = ticks + (n(2) or 60)
		return true, "wait " .. (n(2) or 60) .. " ticks"

	elseif op == "LOG" then
		return true, table.concat(w, " ", 2)

	elseif op == "SNAPSHOT" then
		local nc, ne, nv = counts()
		return true, string.format("%s constructions=%d edges=%d vehicles=%d",
			w[2] or "snapshot", nc, ne, nv)

	elseif op == "DEPOT" then
		return actDepot(n(2), n(3))

	elseif op == "CON" then
		return actCon(w[4], n(2), n(3), { year = 1850, paramX = 0, paramY = 0 })

	elseif op == "ROAD" then
		return actEdge("road", n(2), n(3), n(4), n(5))

	elseif op == "RAIL" then
		return actEdge("rail", n(2), n(3), n(4), n(5))

	elseif op == "PROBE" then
		return actProbe(w[2])

	elseif op == "LINE" then
		return actCreateLine(w[2] and table.concat(w, " ", 2) or nil)

	elseif op == "SETLINEPROBE" then
		local f = io.open(setLineProbeFile(), "w")
		if f then f:write("go") f:close() end
		return f ~= nil, "setLine probe armed for the GUI state"

	elseif op == "BUYVEH" then
		return actBuyVehicle(n(2))

	elseif op == "ASSIGN" then
		local f = io.open(assignReqFile(), "w")
		if f then f:write("go") f:close() end
		return f ~= nil, "assign armed for the GUI state"

	elseif op == "SELFUPGRADE" then
		return actSelfUpgrade(w[2])

	elseif op == "CONEDIT" then
		return actConEdit(n(2), n(3), w[4], w[5])

	elseif op == "DELROAD" then
		return actDelEdge("road", n(2), n(3), n(4), n(5))

	elseif op == "DELRAIL" then
		return actDelEdge("rail", n(2), n(3), n(4), n(5))

	elseif op == "END" then
		steps = nil
		appendResult("=== scenario end ===")
		return true, "end"
	end
	return false, "unknown op " .. tostring(op)
end

function data()
	return {
		init = function()
			log("test harness live (idle until a scenario file appears)")
		end,

		update = function()
			ticks = ticks + 1
			-- Latching, and BEFORE anything else: with lockstep loaded, running a
			-- scenario is worse than running nothing -- it changes one world only.
			-- Loud and repeated, because a one-shot line scrolls away long before
			-- anyone looks and the symptom (a "desync" the harness caused) gives
			-- no hint of the cause.
			if checkLockstep() then
				if ticks % 200 == 0 then
					log("!! DISABLED -- mp_lockstep_1 is loaded in this Lua state.")
					log("!! Scenario actions are issued from Lua; lockstep captures the PLAYER's"
						.. " native command by caller address and deliberately ignores the Lua path,"
						.. " so a scenario would build in THIS world and no other -- a desync this"
						.. " harness invented. Use tools\\soak.ps1 for lockstep instead.")
				end
				steps = nil
				return
			end
			if ticks % 60 == 0 then pcall(detectInstance) end
			if not INSTANCE then return end
			if ticks % 30 == 0 then pcall(loadScenario) end
			if not steps then return end
			if ticks < waitUntil then return end
			-- one step per tick keeps ordering obvious in the results file
			if stepIdx >= #steps then
				steps = nil
				appendResult("=== scenario complete ===")
				log("scenario complete")
				return
			end
			stepIdx = stepIdx + 1
			local w = steps[stepIdx]
			-- pcall yields (ok, <runStep's two return values>)
			local ok, success, detail = pcall(runStep, w)
			if not ok then
				report(stepIdx, "ERROR", tostring(success) .. " | " .. table.concat(w, " "))
			else
				report(stepIdx, success and "OK" or "FAIL", detail)
			end
		end,

		-- EXPERIMENT: dispatch buyVehicle from the GUI STATE instead of the
		-- engine state.
		--
		-- Sending it from update() wedges the sim thread, measured twice at the
		-- identical point with both a 26-unit and a 1-unit config, so it is not
		-- the payload. The remaining theory is that the command cannot be
		-- dispatched from inside the engine-state script tick without
		-- deadlocking. guiUpdate runs in the GUI state, which the api.cmd docs
		-- explicitly say also carries api.cmd -- so if the theory holds, the
		-- same call succeeds from here.
		--
		-- The two states do NOT share upvalues: this is a different Lua state
		-- running the same file, so `pendingBuy` set in update() would be
		-- invisible here. The request therefore travels through a file, the
		-- same way everything else in this project crosses a boundary.
		guiUpdate = function()
			guiTicks = guiTicks + 1
			if guiTicks % 10 ~= 0 then return end
			-- Same gate as update(). The buy/assign drains dispatch api.cmd from
			-- the GUI state, which lockstep ignores just the same.
			if checkLockstep() then return end
			pcall(detectInstance)
			if not INSTANCE then return end
			pcall(drainBuyRequest)
			pcall(drainAssignRequest)
			pcall(drainSetLineProbe)
		end,

		save = function() return {} end,
		load = function(s) end,
	}
end
