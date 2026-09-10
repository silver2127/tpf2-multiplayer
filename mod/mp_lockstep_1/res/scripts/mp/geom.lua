-- mp/geom.lua -- edge geometry: hermite, node/edge lookup, mid-span splitting (ported from mp_bridge)
--
-- Split out of lockstep.lua on 2026-09-08 (the single 11k-line chunk was at
-- 182 of Lua 5.1's 200 file-scope locals). Loaded from the game script as
--     local geom = require("mp.geom")(CM, K, log)
-- It is a FACTORY, not a plain module table: every load of the game script
-- gets fresh state (package.loaded would otherwise hand the next game the
-- previous game's file-scope locals), and the code below keeps the same
-- per-load semantics it had inside lockstep.lua. Anything defined later in
-- lockstep.lua is reached through CM (CM.ser, CM.deepcopy, ...); CM is the
-- shared state table, K the constants table, log the instance-tagged logger.
-- The body stays at column 0 on purpose: tools/luacheck.py's use-before-define
-- checks look at column-0 declarations.
return function(CM, K, log)
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
-- ...but that 4.5 m is a ROAD's half-width, and applying it to rail-against-rail
-- is far too generous: standard parallel track spacing is about the same 5 m, so
-- a new track laid alongside an existing one has its endpoint inside the
-- tolerance of its NEIGHBOUR and gets welded to it -- "building parallel track
-- sometimes joins the ends". A track has no width to speak of; its centreline is
-- the thing, so the match has to be tight enough to fit between two tracks.
-- REASONED from the constant and the game's track spacing, not yet measured
-- against a capture: findEdgeContaining logs the distance of every TRACK match
-- so the next occurrence either confirms this or names the real number.
K.SPLIT_EPS_TRACK = 2.0
K.SPLIT_MIN_U = 0.08   -- nearer an end than this IS the endpoint, not a split
K.SPLIT_MIN_DIST = 0.3  -- metres from an end node: closer than this IS the node

-- ---------- one map read per track build (2026-09-10) ----------
-- findNodeNear and findEdgeContaining walked the whole node->edge map, reading
-- every node's position and every edge's geometry, on EVERY call, and one track
-- build calls them a dozen times: 0.4 to 2.6 s of planning at the click on a
-- large map, on the sim thread, and the same again when the build applies.
-- Nothing changes the world inside one execPolyline call (its commands apply
-- later), so while a geometry scope is open each network is read once and those
-- lists answer every lookup. Map order and the distance tests are unchanged.
CM.geomDepth = 0
local geomCache = nil
function CM.geomScopeBegin()
	CM.geomDepth = (CM.geomDepth or 0) + 1
	if CM.geomDepth == 1 then geomCache = { maps = {}, nodes = {}, edges = {} } end
end
function CM.geomScopeEnd()
	CM.geomDepth = math.max(0, (CM.geomDepth or 1) - 1)
	if CM.geomDepth == 0 then geomCache = nil end
end
local function netMap(isTrack)
	local key = isTrack and "t" or "s"
	if geomCache and geomCache.maps[key] ~= nil then return geomCache.maps[key] or nil end
	local m
	if isTrack then
		pcall(function() m = api.engine.system.streetSystem.getNode2TrackEdgeMap() end)
	else
		pcall(function() m = api.engine.system.streetSystem.getNode2StreetEdgeMap() end)
	end
	if geomCache then geomCache.maps[key] = m or false end
	return m
end
CM.netMap = netMap
local function netNodes(isTrack)
	local key = isTrack and "t" or "s"
	if geomCache and geomCache.nodes[key] then return geomCache.nodes[key] end
	local list = {}
	-- HOLD THE MAP IN A LOCAL for the whole loop. The map is a C++ container
	-- owned by its Lua userdata; iterating a bare pairs(netMap(...)) leaves
	-- nothing referencing it, a GC step inside the loop (edgeGeomT and every
	-- getComponent allocate) frees it, and the next step walks freed tree
	-- nodes: a native access violation, no pcall can catch it (track build
	-- and depot replay crashes, 2026-09-10 16:25 and 16:27).
	local m = netMap(isTrack)
	for nid, _ in pairs(m or {}) do
		local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
		if nc and nc.position then
			local pp = nc.position
			list[#list + 1] = { nid, pp.x or pp[1], pp.y or pp[2] }
		end
	end
	if geomCache then geomCache.nodes[key] = list end
	return list
end
local function netEdges(isTrack)
	local key = isTrack and "t" or "s"
	if geomCache and geomCache.edges[key] then return geomCache.edges[key] end
	local list, seen = {}, {}
	local m = netMap(isTrack)   -- held for the loop: see netNodes
	for _, lst in pairs(m or {}) do
		for _, eid in pairs(lst) do
			if not seen[eid] then
				seen[eid] = true
				local comp, a, b, ta, tb = edgeGeomT(eid)
				list[#list + 1] = { eid, comp, a, b, ta, tb }
			end
		end
	end
	if geomCache then geomCache.edges[key] = list end
	return list
end
CM.netEdges = netEdges

local function edgeMaps()
	local maps = {}
	if geomCache then
		local sm, tm = netMap(false), netMap(true)
		if sm then maps[#maps + 1] = sm end
		if tm then maps[#maps + 1] = tm end
		return maps
	end
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
	if geomCache then
		for _, n in ipairs(netNodes(isTrack)) do
			local dx, dy = n[2] - x, n[3] - y
			local d = dx * dx + dy * dy
			if d < eps * eps and (not bestD or d < bestD) then best, bestD = n[1], d end
		end
		return best
	end
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

-- Is this node a STRAIGHT-THROUGH, i.e. may a rail be routed through it to
-- fuse a level crossing?
--
-- The engine's own invariant, from the assert that freezes the game:
--   Crossing.cpp:232  Angle(m_ctxs[0].curve[2], m_ctxs[2].curve[2]) > PI - ANGLE_TOL
--   Crossing.cpp:233  Angle(m_ctxs[1].curve[2], m_ctxs[3].curve[2]) > PI - ANGLE_TOL
-- A four-arm crossing must be two straight lines passing through each other:
-- arm 0 opposite arm 2, arm 1 opposite arm 3. Route a rail through a node where
-- the ROAD turns a corner and the road's own pair is not anti-parallel, and the
-- assert fires inside StreetGeometry -- which is a native crash, not something
-- pcall or ignoreErrors can catch (host freeze building a track crossover,
-- 2026-09-03).
--
-- A node created by SPLITTING an edge is always straight-through: both halves
-- carry the parent's tangent at the cut. A node that already existed may be a
-- junction or a corner, and those are the ones that must be refused.
--
-- Returns ok, angleDegrees, edgeCount.
K.XING_STRAIGHT_TOL_DEG = 10.0   -- how far from 180 deg still counts as straight
function CM.nodeIsStraightThrough(nid)
	if not nid then return false, nil, 0 end
	local alive = false
	pcall(function() alive = api.engine.entityExists(nid) end)
	if not alive then return false, nil, 0 end
	local ids, seen = {}, {}
	for _, m in ipairs(edgeMaps()) do
		for _, eid in pairs((m and m[nid]) or {}) do
			if not seen[eid] then seen[eid] = true; ids[#ids + 1] = eid end
		end
	end
	-- 1 edge = a dead end, 3+ = a junction. Neither is a line passing through.
	if #ids ~= 2 then return false, nil, #ids end
	-- Direction of each arm LEAVING the node: +tangent0 at node0, -tangent1 at
	-- node1 (the Hermite derivative points along increasing u).
	local dirs = {}
	for _, eid in ipairs(ids) do
		local comp, ea, eb, t0, t1 = edgeGeomT(eid)
		if not comp then return false, nil, #ids end
		local d
		if comp.node0 == nid then d = { t0[1], t0[2], t0[3] }
		elseif comp.node1 == nid then d = { -t1[1], -t1[2], -t1[3] }
		else return false, nil, #ids end
		local n = math.sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3])
		if n < 1e-6 then return false, nil, #ids end
		dirs[#dirs + 1] = { d[1] / n, d[2] / n, d[3] / n }
		local _ = ea, eb
	end
	local dot = dirs[1][1] * dirs[2][1] + dirs[1][2] * dirs[2][2] + dirs[1][3] * dirs[2][3]
	if dot < -1 then dot = -1 elseif dot > 1 then dot = 1 end
	local deg = math.deg(math.acos(dot))
	return deg >= (180 - K.XING_STRAIGHT_TOL_DEG), deg, #ids
end

local function findEdgeContaining(isTrack, x, y, skipNode, eps)
	local tol = eps or (isTrack and K.SPLIT_EPS_TRACK or K.SPLIT_EPS)
	-- Sampling is by DISTANCE, not by a fixed 19 points: a 77 m town road
	-- sampled every 7.7 m misses a point that is on it. And the end
	-- exclusion is by distance (K.SPLIT_MIN_DIST), not by fraction: the UI
	-- happily splits 1.0 m from an end node (measured: a depot snapped at
	-- u=0.013 of a 76.9 m edge), and 8% of a long edge is many metres.
	local best, bestD, bestU, bestGeom = nil, nil, nil, nil
	local function consider(eid, comp, a, b, ta, tb)
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
					if d < tol * tol and (not bestD or d < bestD) then
						best, bestD, bestU = eid, d, u
						bestGeom = { a, ta, b, tb, steps }
					end
				end
			end
		end
	end
	if geomCache then
		for _, e in ipairs(netEdges(isTrack)) do consider(e[1], e[2], e[3], e[4], e[5], e[6]) end
	else
		local seen = {}
		local m = netMap(isTrack)   -- held for the loop: see netNodes
		for _, list in pairs(m or {}) do
			for _, eid in pairs(list) do
				if not seen[eid] then
					seen[eid] = true
					consider(eid, edgeGeomT(eid))
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
	-- Distance of the match, logged for TRACKS because welding a new track to the
	-- one running parallel to it is the failure mode this tolerance controls.
	if isTrack then
		CM.cmLog(string.format("SPLIT: track edge %d matched at u=%.3f, %.2f m from the point (tol %.2f)",
			best, u, math.sqrt((q[1]-x)^2 + (q[2]-y)^2), tol))
	end
	return best, u
end

-- Bus lane and tram track for a street edge we are about to BUILD.
--
-- These live in BASE_EDGE_STREET next to streetType, but the slice's edge
-- decode skips them for a street (it forces trackType and never reads them),
-- so they are NOT on the wire. Every street rebuild hardcoded
-- hasBus=false / tramTrackType=0, which DESTROYED them: a road that had a bus
-- lane or a tram track lost both the moment anything replayed it -- and since a
-- street upgrade is cancelled and replayed from these values, that included the
-- ORIGINATOR, so adding a tram way or a bus lane looked like it did nothing at
-- all (2026-09-03).
--
-- Prefer the wire when it carries them (c.bus / c.tram, for when the capture
-- side learns to ship them); otherwise inherit from the edge this one replaces,
-- located by its endpoints. With nothing to inherit it falls back to a plain
-- street, which is the old behaviour.
function CM.streetProps(c, x0, y0, x1, y1)
	local bus, tram = false, 0
	local wb, wt = (c and c.bus), (c and c.tram)
	if wb ~= nil then bus = tonumber(wb) == 1 end
	if wt ~= nil then tram = tonumber(wt) or 0 end
	if wb == nil or wt == nil then
		pcall(function()
			local old = CM.findEdgeByEnds(false, x0, y0, x1, y1, 2.0)
			if not old then return end
			local sc = api.engine.getComponent(old, api.type.ComponentType.BASE_EDGE_STREET)
			if not sc then return end
			-- Log what the road being replaced ALREADY had, beside what the wire
			-- asked for. With before / asked / applied all in the log, which byte
			-- is tramTrackType stops being guesswork.
			log(string.format("STREETP before: edge %d hasBus=%s tramTrackType=%s  (wire bus=%s tram=%s)",
				old, tostring(sc.hasBus), tostring(sc.tramTrackType), tostring(wb), tostring(wt)))
			if wb == nil then bus = sc.hasBus and true or false end
			if wt == nil then tram = tonumber(sc.tramTrackType) or 0 end
		end)
	end
	return bus, tram
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

return { hermitePos = hermitePos, hermiteTangent = hermiteTangent, edgeGeomT = edgeGeomT, findNodeNear = findNodeNear, findEdgeContaining = findEdgeContaining, copyEdgeProps = copyEdgeProps }
end
