-- mp/roads.lua -- road/track replay: command order, proposal context, execEdge, execPolyline
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.roads")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- command execution ----------
-- Deterministic order is mandatory. Two commands due at the same stamp must be
-- applied in the same sequence on every peer, or the worlds diverge even though
-- both "executed the same commands".
function CM.cmdKey(c) return c.at .. "|" .. c.origin .. "|" .. c.seq end
function CM.cmdLess(x, y)
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
function CM.buildContext()
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

-- The Context a NATIVE construction placement runs under (measured 2026-08-28:
-- checkTerrainAlignment=1, cleanupStreetGraph=1). The replay used nil, and the
-- open item since then was a one-edge / heights divergence around every
-- replayed depot: the engine re-graded and cleaned the street graph on the
-- originator and not on the peer (today: A e1081 vs peers e1082, z differs, on
-- the very first depot). With the player set the peer is also charged the way
-- the originator was, which is the construction half of the coop money gap.
-- Gated, with a nil fallback if make refuses, so the worst case is today's
-- behaviour.
K.CONX_UI_CONTEXT = true
-- STRICT CONSTRUCTION REPLAY (execConX): the host keeps its native
-- station placement, but its interactive builder grades the terrain 0.1 m
-- higher than the scripted buildProposal the peers replay (measured
-- 2026-09-02: host z=4.5, peers z=4.4, z-only). Extracting params to cancel
-- the native build in the DLL is impossible (the module map is consumed
-- into geometry before the command exists). So DELETE-AND-REPLAY instead:
-- the originator builds native (params readable), then bulldozes its own
-- copy and rebuilds through the SAME scripted path as the peers, so all
-- three grade identically. Money (native charge + bulldoze refund +
-- recharge) is not reconciled yet -- this prototype proves the GEOMETRY
-- converges (dump_egeo) before that is solved.
function CM.conxContext()
	if not K.CONX_UI_CONTEXT then return nil end
	local ok, ctx = pcall(function()
		local c = api.type.Context:new()
		c.checkTerrainAlignment = true
		c.cleanupStreetGraph    = true
		c.gatherBuildings       = false
		c.gatherFields          = true
		c.player                = api.engine.util.getPlayer()
		return c
	end)
	if ok and ctx then return ctx end
	log("CONX: could not build the UI context -- using nil")
	return nil
end

function CM.execEdge(c)
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
			e.streetEdge.hasBus, e.streetEdge.tramTrackType =
				CM.streetProps(c, c.x0, c.y0, c.x1, c.y1)
		end
		sp.streetProposal.edgesToAdd[1] = e

		-- ignoreErrors=TRUE, as the construction and stop paths already pass.
		-- The native road tool demolishes exactly what its footprint collides
		-- with; false made the engine REFUSE the whole build on 'Collision'
		-- instead, so a road upgrade that the player's own tool would have
		-- completed failed on every instance -- and since the upgrade is
		-- cancelled and replayed from here, it failed on the originator too
		-- (measured 2026-09-03: five upgrades in a row, critical=false
		-- 'Collision', in an area the player had been demolishing).
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, CM.buildContext(), true),
			function(res, success)
				log(string.format("EXEC %s seq=%s origin=%s at=%s success=%s",
					c.op, tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(success)))
				pcall(CM.cmSettleBuild, c, res, success, c.op)   -- companies: owner + cost
			end)
	end)
	if not ok then log("exec error: " .. tostring(err)) end
end

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
				local comp, p, q = CM.edgeGeomT(eid)
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
	local comp, a, b, ta, tb = CM.edgeGeomT(eid)
	if not comp then return nil end
	local bestU, bestD
	for i = 1, 399 do
		local u = i / 400
		local q = CM.hermitePos(a, ta, b, tb, u)
		local d = (q[1]-x)^2 + (q[2]-y)^2
		if not bestD or d < bestD then bestU, bestD = u, d end
	end
	return bestU, bestD and math.sqrt(bestD) or nil
end

-- The same, refined below the 1/400 grid (ternary search on distance).
function CM.uOnEdgeFine(eid, x, y)
	local comp, a, b, ta, tb = CM.edgeGeomT(eid)
	if not comp then return nil end
	local u0 = CM.uOnEdge(eid, x, y)
	if not u0 then return nil end
	local lo, hi = math.max(0, u0 - 1 / 400), math.min(1, u0 + 1 / 400)
	for _ = 1, 12 do
		local u1, u2 = lo + (hi - lo) / 3, hi - (hi - lo) / 3
		local q1, q2 = CM.hermitePos(a, ta, b, tb, u1), CM.hermitePos(a, ta, b, tb, u2)
		if (q1[1] - x) ^ 2 + (q1[2] - y) ^ 2 < (q2[1] - x) ^ 2 + (q2[2] - y) ^ 2 then hi = u2 else lo = u1 end
	end
	return (lo + hi) / 2
end

function CM.execPolyline(c, planOnly)
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
		-- The plan-only pre-pass passes seq="plan" (a sentinel string); it builds
	-- nothing, so any base is fine, but c.seq * 1000 threw "arithmetic on a
	-- string" and aborted the WHOLE plan pass -- the host then shipped no
	-- split/crossing decisions and every peer derived its own, which diverged
	-- on a tram-track street upgrade (2026-09-02). Coerce to a number.
	local seqN = tonumber(c.seq) or 0
	local base = -(1000000 + CM.originIdx(c.origin) * 10000000 + seqN * 1000)
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
			local nA = CM.findNodeNear(isTrack, r[1], r[2], 1.5)
			local nB = CM.findNodeNear(isTrack, r[3], r[4], 1.5)
			local eid
			if nA and nB and nA ~= nB then
				local mm
				if isTrack then
					pcall(function() mm = CM.netMap(true) end)
				else
					pcall(function() mm = CM.netMap(false) end)
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
		local splitParentEnds = {}   -- end node -> the split parent it belongs to (see crossingsFor)
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
			local comp, a, b, ta, tb = CM.edgeGeomT(eid)
			if not comp then return nil end
			-- DEGENERATE SPLIT GUARD (proposal dump 2026-08-29): a vertex 0.5 m from
			-- the crossed edge's END node fell outside findNodeNear's 1.5 m, then
			-- findEdgeContaining returned u~1, and the second half was zero-length:
			-- a self-loop edge (n0==n1 by position) -> "Construction not possible".
			-- Splitting that close to an end is meaningless: use the end node.
			do
				local q = CM.hermitePos(a, ta, b, tb, u)
				local dA = math.sqrt((q[1]-a[1])^2 + (q[2]-a[2])^2)
				local dB = math.sqrt((q[1]-b[1])^2 + (q[2]-b[2])^2)
				if dA < XING_END_SNAP then CM.cmLog(string.format("XING: split of %d at u=%.2f is %.1f m from node0 -> snapping to node %d", eid, u, dA, comp.node0)); return comp.node0 end
				if dB < XING_END_SNAP then CM.cmLog(string.format("XING: split of %d at u=%.2f is %.1f m from node1 -> snapping to node %d", eid, u, dB, comp.node1)); return comp.node1 end
			end
			local crossedIsTrack = false
			pcall(function() crossedIsTrack = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
			local pm = CM.hermitePos(a, ta, b, tb, u)
			local tm = CM.hermiteTangent(a, ta, b, tb, u)
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
				CM.copyEdgeProps(h, eid, crossedIsTrack, nil)   -- the CROSSED edge's own props
				addEdges[#addEdges + 1] = h
			end
			half(comp.node0, mid, ta, tm, u)
			half(mid, comp.node1, tm, tb, 1 - u)
			splitRoads[eid] = mid
			splitParentEnds[comp.node0] = eid
			splitParentEnds[comp.node1] = eid
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
					local n = CM.findNodeNear(isTrack, told[1], told[2], 1.5)
						or CM.findNodeNear(not isTrack, told[1], told[2], 1.5)
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

			local existing = CM.findNodeNear(isTrack, x, y, 1.5)
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
				local rnode = CM.findNodeNear(false, x, y, 4.0)
				if rnode then
					log(string.format("ROADP: level crossing -- rail vertex %d shares road node %d", i, rnode))
					CM.cmLog(string.format("XING: vertex %d snapped to road node %d (%.1f,%.1f)", i, rnode, x, y))
					-- SHARING A NODE MEANS SHARING ITS HEIGHT, and the road's height
					-- is not the rail's. Measured 2026-08-31: the shipped vertex sat
					-- at z=27.17 and the road node 0.8 m away at z=29.20, so the rail
					-- had to climb 3.07 m over 31.6 m (9.7%) into it instead of
					-- 1.04 m (3.3%) -- 'Too much slope', and the player's build
					-- vanished on their own screen. The originator's engine moves the
					-- road to meet the rail at a crossing, but moving an existing node
					-- inside a replayed proposal is not safe (below); differences of
					-- 0.25 m or less are left alone.
					local rp = CM.nodePosXYZ(rnode)
					if rp and math.abs(rp[3] - z) > 0.25 then
						-- MOVING AN EXISTING NODE IN A REPLAYED PROPOSAL ASSERTS THE
						-- ENGINE (construction_util_engine.cpp:58 AddToEngine, live
						-- 2026-09-09, a 2.05 m move) and the game never recovers.
						-- The rail vertex takes the road's height instead; if the
						-- slope is then too steep the engine refuses the build the
						-- same way on every instance, and the player re-lays it.
						CM.cmLog(string.format("XING: road node %d is %.2f m off the rail's height -- NOT moving it; the rail meets it at %.2f", rnode, z - rp[3], rp[3]))
						z = rp[3]
					end
					planV[#planV + 1] = string.format("%d,N,%.2f,%.2f,%.2f", i, x, y, z)
					resolved[i] = rnode; return rnode
				end
				local reid, ru
				pcall(function() reid, ru = CM.findEdgeContaining(false, x, y) end)
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
			local okFind = pcall(function() eid, u = CM.findEdgeContaining(isTrack, x, y) end)
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
			local Mout = CM.findNodeNear(isTrack, wx, wy, 1.5)
			local eid, u
			pcall(function() eid, u = CM.findEdgeContaining(isTrack, wx, wy, Mout) end)
			if not eid then
				log(string.format("WELD: no street edge under %.1f,%.1f -- skipped", wx, wy))
			else
				local comp, a, b, ta, tb = CM.edgeGeomT(eid)
				if comp then
					local tm = CM.hermiteTangent(a, ta, b, tb, u)
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
						CM.copyEdgeProps(h, eid, isTrack, stype)
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
							local mm = CM.netMap(isTrack)
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
						local Min = CM.findNodeNear(isTrack, ap[1], ap[2], 1.5)
						            or newNodeAt(ap[1], ap[2], ap[3])
						local e2 = newEdge()
						e2.comp.node0 = Min
						e2.comp.node1 = X
						e2.comp.tangent0 = api.type.Vec3f.new(ap[4], ap[5], ap[6])
						e2.comp.tangent1 = api.type.Vec3f.new(ap[7], ap[8], ap[9])
						e2.comp.type = 0
						e2.comp.typeIndex = -1   -- native edges (road AND rail) carry typeIndex=-1; 0 broke the crossing tests
						e2.type = isTrack and 1 or 0
						CM.copyEdgeProps(e2, eid, isTrack, stype)
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
		-- How close the rail must pass to an EXISTING node before we will route
		-- it through that node. CROSS_BAND below is deliberately wide (a road's
		-- half-width plus margin) so a rail crossing a road's centreline is
		-- still found; this is the much tighter test for "and it actually meets
		-- this node". Sampling is ~0.5 m, so sub-metre is as tight as the
		-- measurement supports.
		K.XING_NODE_TOUCH = 0.75
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
		-- Node positions are read ONCE per proposal: the per-segment scan below
		-- used to call getComponent on every map node (10k) for every segment (a
		-- 23-segment rail = 230k component reads, seconds of stall at the click).
		local xingNodeCache = nil   -- NOT xingNodes: that is the crossing-node list the post-build probe walks
		local function xingNodeList()
			if xingNodeCache then return xingNodeCache end
			xingNodeCache = {}
			for _, getter in ipairs({ api.engine.system.streetSystem.getNode2StreetEdgeMap, api.engine.system.streetSystem.getNode2TrackEdgeMap }) do
				local m
				pcall(function() m = getter() end)
				if m then
					for nid, edges in pairs(m) do
						local nc = api.engine.getComponent(nid, api.type.ComponentType.BASE_NODE)
						local pnode = nc and nc.position
						if pnode then
							local ids = {}
							for _, eid in pairs(edges) do if eid > 0 then ids[#ids + 1] = eid end end
							xingNodeCache[#xingNodeCache + 1] = { pnode.x or pnode[1], pnode.y or pnode[2], ids }
						end
					end
				end
			end
			return xingNodeCache
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
			for si = 0, rs do rail[si] = CM.hermitePos(a, T0, b, T1, si / rs) end
			-- candidate street edges: any whose node lies within reach of the segment's bbox
			local minx, maxx = math.min(x0, x1) - 60, math.max(x0, x1) + 60
			local miny, maxy = math.min(y0, y1) - 60, math.max(y0, y1) + 60
			-- candidates from BOTH maps: streets (level crossing) and tracks (a rail
			-- crossing another rail is a plain junction -- same split, no crossing
			-- component). Placeholder ids (<0) are never in these maps.
			local cand, considered = {}, 0
			for _, nd in ipairs(xingNodeList()) do
				local px, py = nd[1], nd[2]
				if px >= minx and px <= maxx and py >= miny and py <= maxy then
					for _, eid in ipairs(nd[3]) do cand[eid] = true end
				end
			end
			for eid in pairs(cand) do
				considered = considered + 1
				local comp, ra, rb, rta, rtb = CM.edgeGeomT(eid)
				if comp then
					local rlen = math.max(math.sqrt((rb[1]-ra[1])^2 + (rb[2]-ra[2])^2), 1)
					local ss = math.min(600, math.max(10, math.floor(rlen / 0.5)))
					local bestD, bestRu, bestU = nil, nil, nil
					for sj = 1, ss - 1 do
						local ru = sj / ss
						local q = CM.hermitePos(ra, rta, rb, rtb, ru)
						for si = 0, rs do
							local r = rail[si]
							local d = (q[1]-r[1])^2 + (q[2]-r[2])^2
							if not bestD or d < bestD then bestD, bestRu, bestU = d, ru, si / rs end
						end
					end
					local dist = bestD and math.sqrt(bestD) or 1e9
					if dist > CROSS_BAND then -- too far
					elseif splitRoads[eid] or comp.node0 == n0 or comp.node1 == n0 or comp.node0 == n1 or comp.node1 == n1 then
						-- ADJACENCY, not a crossing. A branch never crosses the edge it
						-- branches FROM: a track joining a bridge 6 m before the bridge's
						-- end node diverged at 13 deg, sat inside the band, and was routed
						-- THROUGH that end node -- duplicating the split's second half ->
						-- 'Construction not possible' (proposal dump 2026-08-29). An edge
						-- this proposal split (the parent) or one sharing an endpoint with
						-- the segment only ever TOUCHES it.
					else
						local q = CM.hermitePos(ra, rta, rb, rtb, bestRu)
						local dA = math.sqrt((q[1]-ra[1])^2 + (q[2]-ra[2])^2)
						local dB = math.sqrt((q[1]-rb[1])^2 + (q[2]-rb[2])^2)
						local r = rail[math.floor(bestU * rs + 0.5)] or rail[0]
						local dEnd = math.min(math.sqrt((r[1]-x0)^2 + (r[2]-y0)^2), math.sqrt((r[1]-x1)^2 + (r[2]-y1)^2))
						if dEnd < CROSS_END_MIN then -- at a rail end: the endpoint case, handled by resolve
						elseif dA < CROSS_END_MIN or dB < CROSS_END_MIN then
							-- The rail crosses the road AT ONE OF ITS NODES (the road is
							-- already split there, e.g. a junction or a prior crossing).
							-- Nothing to split -- but the rail MUST be routed THROUGH that
							-- node so the engine fuses a level crossing. Building the rail
							-- edge over the node without sharing it = no crossing (seen
							-- live on A and B: closest 0.00 m at road u=0.01/0.99).
							local nid = (dA < dB) and comp.node0 or comp.node1
							-- TWO conditions before routing the rail through an
							-- EXISTING node, both learned from a host freeze.
							--
							-- (a) The rail must actually TOUCH the node. This
							-- branch was built for a measured "closest 0.00 m at
							-- road u=0.01/0.99"; it also fired at 1.06 m and at
							-- 4.04 m, which are near-misses beside a junction,
							-- not crossings. Routing the rail through a node it
							-- passes four metres from drags it off its own path.
							--
							-- (b) The node must be a straight-through. The engine
							-- asserts that a crossing's opposite arms are
							-- anti-parallel, so a rail routed through a road
							-- CORNER or junction crashes StreetGeometry outright.
							local straight = CM.nodeIsStraightThrough(nid)
							if splitParentEnds[nid] then
								-- (c) NEVER an end node of an edge this proposal split
								-- (2026-09-09). A stretched crossover leaves the track
								-- at a shallow angle, so its diagonal runs beside the
								-- parent's remaining half and passes inside the touch
								-- tolerance of the parent's END node (0.64 m measured).
								-- Routing through it duplicates the half the split
								-- already added (mid -> end), the duplicate is dropped
								-- and the engine refuses the proposal: every long
								-- crossover failed, short ones passed by luck of the
								-- distance. The rail branches OFF that edge; it cannot
								-- also cross it at its end.
							elseif dist > K.XING_NODE_TOUCH then
								-- a near-miss beside the node, not a crossing
							elseif not straight then
								-- a corner or junction: routing a rail through it trips the engine's crossing assert
							else
								hits[#hits + 1] = { node = nid, u = bestU }
							end
						else hits[#hits + 1] = { eid = eid, ru = bestRu, u = bestU } end   -- a crossing mid-edge: the road is split there
					end
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
							local n = CM.findNodeNear(false, told[1], told[2], 1.5)
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
						local ta = CM.hermiteTangent({ x0, y0, z0 }, T0, { x1, y1, z1 }, T1, ua)
						local tb2 = CM.hermiteTangent({ x0, y0, z0 }, T0, { x1, y1, z1 }, T1, ub)
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
					e.streetEdge.hasBus, e.streetEdge.tramTrackType =
						CM.streetProps(c, x0, y0, x1, y1)
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

		-- ignoreErrors=TRUE, as the construction and stop paths already pass.
		-- The native road tool demolishes exactly what its footprint collides
		-- with; false made the engine REFUSE the whole build on 'Collision'
		-- instead, so a road upgrade that the player's own tool would have
		-- completed failed on every instance -- and since the upgrade is
		-- cancelled and replayed from here, it failed on the originator too
		-- (measured 2026-09-03: five upgrades in a row, critical=false
		-- 'Collision', in an area the player had been demolishing).
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp, CM.buildContext(), true),
			function(res, success)
				pcall(CM.cmSettleBuild, c, res, success, "ROADP")   -- companies: owner + cost
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
				-- READ BACK the street properties the engine actually applied, beside
				-- what we asked for. The wire carries two candidate bytes for the tram
				-- variant (+0x51 and +0x54) and neither is confirmed; comparing the
				-- request against the applied component is what settles which one is
				-- tramTrackType, without guessing and building the wrong track.
				if success and not isTrack then
					pcall(function()
						local pts2 = {}
						for tok in tostring(c.pts or ""):gmatch("[^,]+") do pts2[#pts2 + 1] = tonumber(tok) end
						if #pts2 >= 6 then
							local eid = CM.findEdgeByEnds(false, pts2[1], pts2[2], pts2[4], pts2[5], 3.0)
							if eid then
								local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
								if sc then
									log(string.format("STREETP applied: edge %d streetType=%s hasBus=%s tramTrackType=%s  (asked bus=%s tram=%s)",
										eid, tostring(sc.streetType), tostring(sc.hasBus), tostring(sc.tramTrackType),
										tostring(c.bus), tostring(c.tram)))
								end
							end
						end
					end)
				end
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

-- One geometry scope per track build: the dozen lookups inside share one read
-- of the map (geom.lua). Nested calls share the outer scope.
do
	local execPolylineImpl = CM.execPolyline
	CM.execPolyline = function(c, planOnly)
		if CM.geomScopeBegin then CM.geomScopeBegin() end
		-- Exactly the two values execPolylineImpl returns (the encoded vertex and
		-- crossing plans). No table + unpack: the game's Lua has no table.maxn, and
		-- that one call failed EVERY build and ended the game on every instance
		-- at the stamp (2026-09-10).
		local ok, xv, xh = pcall(execPolylineImpl, c, planOnly)
		if CM.geomScopeEnd then CM.geomScopeEnd() end
		if not ok then error(xv, 0) end
		return xv, xh
	end
end
end
