-- mp/stops.lua -- roadside stops (edge objects) and native-shape stop replay
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.stops")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
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
CM.knownStop   = {}      -- edge-object entity -> {x, y, model, kind, oneWay, side} while it exists
CM.stopPrimed  = false
CM.expectStop  = {}      -- { {x, y, t}, ... }: a replay is about to create one near here
CM.expectStopDel = {}    -- same, about to remove one

-- Announcements are matched by DISTANCE (2 m), not by a rounded-coordinate key:
-- the replay projects the shipped position onto this instance's own edge and
-- lands a little off, and a missed match ships a phantom STOPADD back. Entries
-- expire after 8 game units so a failed replay cannot swallow a later real one.
function CM.expectAdd(list, x, y) list[#list + 1] = { x, y, t = CM.gameTime() or 0 } end
function CM.expectFind(list, x, y)
	local now = CM.gameTime() or 0
	for i = #list, 1, -1 do
		if now - (list[i].t or now) > 8 then table.remove(list, i) end
	end
	local best, bestD
	for i = 1, #list do
		local e = list[i]
		local d = (e[1] - x) ^ 2 + (e[2] - y) ^ 2
		if d < 4 and (not bestD or d < bestD) then best, bestD = i, d end
	end
	return best
end
function CM.expectTake(list, x, y)
	local i = CM.expectFind(list, x, y)
	if i then table.remove(list, i); return true end
	return false
end
function CM.expectDrop(list, x, y) CM.expectTake(list, x, y) end

-- Optional switches from tpf2_slice.cfg in the game folder (the game-script
-- CWD; the same file the slice reads): key=value lines, 1/0.
function CM.cfgFlag(key, default)
	-- Re-read every ~5 s (2026-09-09): the PID gains and the pacing switches
	-- are meant to be tuned while the game runs, like the DLL's own cfg.
	if CM.cfgCache == nil or ((CM.ticks or 0) - (CM.cfgCacheAt or 0)) > 27 then
		CM.cfgCacheAt = CM.ticks or 0
		CM.cfgCache = {}
		pcall(function()
			-- game folder first (CWD), then the data dir -- the DLLs' order
			local f = io.open("tpf2_slice.cfg", "r") or (K.BASE and io.open(K.BASE .. "tpf2_slice.cfg", "r"))
			if not f then return end
			for line in f:lines() do
				local k, v = tostring(line):match("^%s*([%w_]+)%s*=%s*(%S+)")
				if k then CM.cfgCache[k] = v end
			end
			f:close()
		end)
	end
	local v = CM.cfgCache[key]
	if v == nil then return default end
	return v == "1" or v == "true" or v == "yes"
end
-- stops_native=0: the old edge rebuild (every neighbour re-created as a new
-- entity; its guards refuse anything under a line or a second object).
-- STRICT BUY (cfg strict_buy, default off). Set HERE rather than at the
-- K.STRICT_OPS literal above, because CM.cfgFlag is not defined yet at that
-- point -- calling it there is the nil-at-load crash from 2cefc5a.
--
-- execVBuy already carries both branches; this is the switch that selects the
-- strict one. The slice only actually cancels when it can fire the depot
-- window's completion callback, and it ships ARMED=0 when it could not, so a
-- buy that ran natively is still skipped rather than applied twice.
K.STRICT_OPS.VBUY = CM.cfgFlag("strict_buy", true)   -- default ON since 2026-09-08 (the callback fire is fixed; see slice strict_buy)
-- exec_delay is read where CM.cfgNum is defined (it is not yet, here).
K.STOPS_NATIVE = CM.cfgFlag("stops_native", true)
-- stops_del_on_line=0: refuse to remove a stop a line uses. Natively the apply
-- rewrites the lines and the station group before the entity dies, exactly as
-- when the player bulldozes it; measured on the rig before this defaulted on.
K.STOPS_DEL_ON_LINE = CM.cfgFlag("stops_del_on_line", true)
K.STOP_EDGE_EPS = 14.0   -- stop model to road centreline, widest town road with margin

-- Everything the wire needs about one edge object, from this instance's world.
-- SIDE is what the edge lists the object as -- {entity, EdgeObjectType} with
-- STOP_LEFT=0 / STOP_RIGHT=1 / SIGNAL=2 (sol enum registration, decompiled
-- 2026-09-02). It is the same bool as the record's `left` in another encoding,
-- NOT passenger/cargo: the earlier "STATION.cargo decides" reading was a
-- single-sample coincidence (a left truck stop and a right bus stop). The
-- engine keys the terminal lane, the CreateLanes slot (one object per value
-- per edge) and replacement on this value.
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
		local comp, a, b, ta, tb = CM.edgeGeomT(eid)
		d.ax, d.ay, d.bx, d.by = a[1], a[2], b[1], b[2]
		local u = CM.uOnEdge(eid, d.x, d.y) or 0.5
		d.u = u
		-- which side of the direction of travel node0 -> node1 the model sits
		local q = CM.hermitePos(a, ta, b, tb, u)
		local t = CM.hermiteTangent(a, ta, b, tb, u)
		local cross = t[1] * (d.y - q[2]) - t[2] * (d.x - q[1])
		d.left = cross > 0
		pcall(function()
			local objs = comp.objects
			for i = 1, #objs do
				local o = objs[i]
				if tonumber(o[1]) == eo then d.side = tonumber(o[2]); break end
			end
		end)
		if d.side == 0 or d.side == 1 then
			-- engine-left (STOP_LEFT) against the geometric cross sign: the
			-- engine's convention, read off this very object. It travels with
			-- the stop, so a peer never has to guess it.
			d.conv = ((d.side == 0) == d.left)
			if CM.leftConv ~= d.conv then
				CM.leftConvFlips = (CM.leftConvFlips or 0) + 1
				log(string.format("stops: left convention: engine-left %s geometric-left (stop %d side=%d cross>0=%s%s)",
					d.conv and "IS" or "is NOT", eo, d.side, tostring(d.left),
					CM.leftConvFlips > 1 and string.format(" -- CHANGED, %d flip(s): side is not a pure function of geometry?", CM.leftConvFlips - 1) or ""))
				CM.leftConv = d.conv
			end
		end
		pcall(function()
			local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
			if sc then d.stname = api.res.streetTypeRep.getName(sc.streetType) end
		end)
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

-- Own loan, every 15 ticks. The first reading is the baseline (a loaded save's
-- loan is known, not new); any later change is the player at the finances
-- window and ships as LOAN. A change WE applied from a peer's LOAN is held out
-- by execLoan (loanExpect) so it is re-baselined, not echoed.
function CM.pollLoan()
	local ok, err = pcall(function()
		local e = game.interface.getEntity(api.engine.util.getPlayer())
		local v = e and tonumber(e.loan)
		if not v then return end
		if CM.loanExpect then
			-- A replayed loan is still settling (possibly over several chunked
			-- journal entries). Never ship while it is in flight.
			if v == CM.loanExpect then
				CM.lastLoan = v
				CM.loanExpect = nil
				return
			end
			if (CM.ticks - (CM.loanExpectSince or CM.ticks)) > K.LOAN_SETTLE_TICKS then
				-- It never reached the value (the engine clamps at the loan cap,
				-- so a peer with less headroom legitimately cannot get there).
				-- Re-baseline to whatever we ended up with WITHOUT shipping it:
				-- shipping here is exactly what started the echo.
				log(string.format("loan: expected %d but settled at %d after %d ticks -- re-baselining, not shipping (DIVERGENCE)",
					CM.loanExpect, v, K.LOAN_SETTLE_TICKS))
				CM.lastLoan = v
				CM.loanExpect = nil
			end
			return
		end
		if CM.lastLoan == nil then CM.lastLoan = v; return end
		if v ~= CM.lastLoan then
			log(string.format("loan: %d -> %d (player) -> LOAN", CM.lastLoan, v))
			CM.lastLoan = v
			CM.scheduleLocal("LOAN", { v = v })
		end
	end)
	if not ok then log("loan poll error: " .. tostring(err)) end
end

function CM.pollStops()
	local ok, err = pcall(function()
		local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
		if not CM.stopPrimed then
			-- what the save already had is known, not new
			for eo, eid in pairs(m) do
				if isPlayerStop(eo) then
					local d = describeStop(eo, eid)
					CM.knownStop[eo] = d and { d.x, d.y, model = d.model, kind = d.kind, oneWay = d.oneWay, side = d.side } or { 0, 0 }
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
				local rec = { d.x, d.y, model = d.model, kind = d.kind, oneWay = d.oneWay, side = d.side }
				if rebound then
					gone[rebound] = nil
					CM.knownStop[rebound] = nil
					CM.knownStop[eo] = rec
				else
					CM.knownStop[eo] = rec
					if CM.expectTake(CM.expectStop, d.x, d.y) then
						-- our own replay landing
					else
						-- REPLACE: dropping a compatible stop on the same side of an
						-- edge that already has one MOVES it -- natively ONE proposal
						-- (old in edgeObjectsToRemove, new added, every line through
						-- it re-pointed). Seen here as a gone + a fresh on one edge,
						-- same side. Ship it as one op so the peer removes and adds
						-- in one proposal too.
						local rep, repPos = nil, nil
						if d.side == 0 or d.side == 1 then
							for geo, pos in pairs(gone) do
								if pos.side == d.side and not CM.expectFind(CM.expectStopDel, pos[1], pos[2]) then
									local _, dist = CM.uOnEdge(eid, pos[1], pos[2])
									if dist and dist < 3.0 then rep, repPos = geo, pos; break end
								end
							end
						end
						local convW = -1
						if d.conv ~= nil then convW = d.conv and 1 or 0
						elseif CM.leftConv ~= nil then convW = CM.leftConv and 1 or 0 end
						local fields = {
							ax = d.ax, ay = d.ay, bx = d.bx, by = d.by,
							u = d.u, left = d.left and 1 or 0, side = d.side or -1, conv = convW,
							x = d.x, y = d.y, kind = d.kind, track = d.track and 1 or 0,
							oneWay = d.oneWay and 1 or 0, stname = CM.escName(d.stname or ""),
							model = CM.escName(d.model), name = CM.escName(d.name),
							skipOrigin = 1 }
						if rep then
							gone[rep] = nil
							CM.knownStop[rep] = nil
							fields.rx, fields.ry = repPos[1], repPos[2]
							CM.scheduleLocal("STOPREP", fields)
							log(string.format("stops: %s '%s' replaced the stop at %.1f,%.1f -> now at %.1f,%.1f side=%s conv=%d -> STOPREP",
								d.model, d.name, repPos[1], repPos[2], d.x, d.y, tostring(d.side), convW))
							-- The engine re-pointed every line through that station to
							-- the new one (old2newEdgeObjects, which a Lua proposal cannot
							-- carry); the peers' lines lose the stop instead. Send them
							-- each such line as it is now -- queued behind the STOPREP.
							for _, lid in ipairs(CM.linesUsingStation(eo)) do
								local lk = CM.lineKeyFor(lid)
								local snap = lk and CM.lineSnapshot(lid)
								if snap then
									CM.scheduleLocal("LUPDATE", { key = lk, name = snap.name, color = snap.color, wait = snap.wait,
									                           stops = snap.stops, skipOrigin = 1 })
									log(string.format("stops: line %s re-shipped after the replace -> LUPDATE", lk))
								end
							end
						else
							CM.scheduleLocal("STOPADD", fields)
							log(string.format("stops: captured %s '%s' kind=%d side=%s %s at %.1f,%.1f u=%.3f left=%s conv=%d -> STOPADD",
								d.model, d.name, d.kind, tostring(d.side), d.track and "track" or "street", d.x, d.y, d.u, tostring(d.left), convW))
						end
					end
				end
			end
		end
		-- genuinely gone (nothing reappeared at the same spot)
		for eo, pos in pairs(gone) do
			CM.knownStop[eo] = nil
			if CM.expectTake(CM.expectStopDel, pos[1], pos[2]) then
				-- our own replay
			else
				CM.scheduleLocal("STOPDEL", { x = pos[1], y = pos[2], skipOrigin = 1 })
				log(string.format("stops: roadside stop at %.1f,%.1f removed -> STOPDEL", pos[1], pos[2]))
			end
		end
	end)
	if not ok then log("stops poll error: " .. tostring(err)) end
end

-- LEGACY (stops_native=0): rebuild a street edge with a given set of stops on
-- it, every one of them RE-CREATED through edgeObjectsToAdd as entity -k. That
-- is the lossy part: the native tool carries the untouched neighbours under
-- their real ids (see CM.nativeStopProposal). Kept, with its guards, as the
-- fallback. Edge objects have their own negative numbering: the k-th entry of
-- edgeObjectsToAdd is entity -k, and the edge lists it as {-k, side}. Listing
-- the object under any other id, or leaving it off the edge, asserts inside
-- CalcNodeIndex, which walks that very list looking for the object.
--
-- stops: list of { u=, left=, model=, name= } relative to node0 -> node1.
-- Every station entity some LINE stops at (via its station group), plus the
-- groups' positions for a proximity fallback. Rebuilding an edge REPLACES its
-- edge objects with new entities, so a stop a line references would be left
-- as a dead entity in that line -- an uncatchable engine assert the next time
-- the line or one of its vehicles is touched (both peers, 2026-09-02: a merged
-- station's old stop deleted from under a bus line).
local function stationsOnLines()
	local ents, groups = {}, {}
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do
			local lc = api.engine.getComponent(ls[i], api.type.ComponentType.LINE)
			if lc and lc.stops then
				for j = 1, #lc.stops do
					local sg = lc.stops[j].stationGroup
					if sg and sg ~= -1 and not groups[sg] then
						groups[sg] = true
						pcall(function()
							local gc = api.engine.getComponent(sg, api.type.ComponentType.STATION_GROUP)
							if gc and gc.stations then for k = 1, #gc.stations do ents[gc.stations[k]] = true end end
						end)
					end
				end
			end
		end
	end)
	return ents, groups
end

local function rebuildEdgeWithStops(eid, stops, why, onDone)
	local comp, a, b, ta, tb = CM.edgeGeomT(eid)
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
	-- Never rebuild under a line. See stationsOnLines: the objects come back as
	-- NEW entities and the line keeps the old id -> fatal. Refuse (DIVERGENCE):
	-- a stop that stays or is missing shows in the c-lane; a crash shows nothing.
	do
		local stopsOnLine, groups = stationsOnLines()
		for eo, e2 in pairs(m) do
			if e2 == eid then
				local used = stopsOnLine[eo] or false
				if not used and next(groups) then
					-- fallback by proximity: a group centred within 5 m of this stop
					local d = describeStop(eo, eid)
					if d then
						for sg in pairs(groups) do
							local gx, gy = CM.stationGroupPos(sg)
							if gx and (gx - d.x) ^ 2 + (gy - d.y) ^ 2 < 25 then used = true; break end
						end
					end
				end
				if used then
					return false, string.format("edge %d carries stop %d that a LINE uses -- rebuilding would leave the line on a dead entity (fatal engine assert), refused (DIVERGENCE)", eid, eo)
				end
			end
		end
	end
	-- TWO OBJECTS ON ONE EDGE IS A FATAL ENGINE ASSERT. StreetGeometry::CreateLanes
	-- requires the interleaved edgeObjects list to end in a -1 sentinel and rejects
	-- a second object with edgeObjects[0].second == -1 -- an UNCATCHABLE C++ assert
	-- that killed both peers on a STOPDEL rebuild (2026-09-02): the peer's road graph
	-- differs from the originator's (town growth, wrong-side placement), so two of
	-- the originator's stops mapped onto one peer edge, and rebuilding it with both
	-- crashed the game. A missing/extra stop is a visible c-lane divergence; a dead
	-- game is not. Refuse to build more than one object on an edge here too, the same
	-- rule execStopAdd already enforces before a build.
	if #stops > 1 then
		return false, string.format("edge %d would carry %d objects -- a second is a fatal engine assert, refused (DIVERGENCE)", eid, #stops)
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
	CM.copyEdgeProps(e, eid, isTrack, nil)
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
		-- st.left is the GEOMETRIC side computed on THIS instance's edge (execStopAdd
		-- projects the shipped world position onto the peer's own edge). A blanket
		-- inversion here was wrong -- it flipped exactly the edges the projection
		-- already had right (inconsistent side, 2026-09-02). Pass it straight; the
		-- verify-and-flip below corrects whatever the engine's convention actually is.
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
			CM.expectAdd(CM.expectStop, st.x, st.y)
			CM.expectAdd(CM.expectStopDel, st.x, st.y)
		end
	end
	local cmd = api.cmd.make.buildProposal(sp, CM.buildContext(), true)
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
				if st.x then CM.expectDrop(CM.expectStop, st.x, st.y); CM.expectDrop(CM.expectStopDel, st.x, st.y) end
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

function CM.execStopAddLegacy(c)
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
		local comp, a, b, ta, tb = CM.edgeGeomT(eid)
		-- u and left are measured along THIS instance's node0->node1, which need
		-- not match the originator's: applying the shipped u/left (even with the
		-- endpoint-flip heuristic) put a stop 12 m along the edge on the other
		-- side (2026-09-02, 'Am Sportplatz' 1240 -> 1252). Project the shipped
		-- world position onto our edge and take the side from the cross product,
		-- exactly as describeStop does at capture. Positions in, positions out.
		local u = CM.uOnEdge(eid, c.x, c.y)
		local left = tonumber(c.left) == 1
		if u then
			local q = CM.hermitePos(a, ta, b, tb, u)
			local tg = CM.hermiteTangent(a, ta, b, tb, u)
			local cross = tg[1] * (c.y - q[2]) - tg[2] * (c.x - q[1])
			left = cross > 0
		else
			u = tonumber(c.u) or 0.5
			local da = (a[1] - c.ax) ^ 2 + (a[2] - c.ay) ^ 2
			local db = (a[1] - c.bx) ^ 2 + (a[2] - c.by) ^ 2
			if db < da then u = 1 - u; left = not left end
			log(string.format("STOPADD seq=%s: projection failed, fell back to shipped u/left", tostring(c.seq)))
		end
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
		stops[#stops + 1] = { u = u, left = left, model = CM.unescName(c.model), name = CM.unescName(c.name), x = c.x, y = c.y,
			kind = tonumber(c.kind) or 1, oneWay = tonumber(c.oneWay) == 1 }
		local ok2, why = rebuildEdgeWithStops(eid, stops, string.format("STOPADD seq=%s origin=%s '%s'",
			tostring(c.seq), tostring(c.origin), CM.unescName(c.name)))
		if not ok2 then log(string.format("STOPADD seq=%s: %s -- skipped", tostring(c.seq), tostring(why))) end
	end)
	if not ok then log("exec STOPADD error: " .. tostring(err)) end
end

function CM.execStopDelLegacy(c)
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
		CM.expectAdd(CM.expectStopDel, c.x, c.y)
		local ok2, why = rebuildEdgeWithStops(eid, keep, string.format("STOPDEL seq=%s origin=%s", tostring(c.seq), tostring(c.origin)),
			function(success) if not success then CM.expectDrop(CM.expectStopDel, c.x, c.y) end end)
		if not ok2 then
			CM.expectDrop(CM.expectStopDel, c.x, c.y)
			log(string.format("STOPDEL seq=%s: %s -- skipped", tostring(c.seq), tostring(why)))
		end
	end)
	if not ok then log("exec STOPDEL error: " .. tostring(err)) end
end

-- ====================== NATIVE-SHAPE STOP REPLAY ======================
--
-- What the street tool and the bulldozer actually submit (decompiled
-- 2026-09-02: street_util::MakeEdgeObjectProposal 0x21ef7d0 and
-- MakeRemoveEdgeObjectsProposal 0x21f0c60, confirmed by the game's own
-- res/scripts/mission/proposalutil.lua): the edge IS removed and re-added as
-- entity -1 -- but its object list is carried over VERBATIM, every untouched
-- object under its REAL id. street_util::UpdateEngine treats id >= 0 in an
-- added segment's objects as an update (re-parented, entity kept, station group
-- and lines untouched) and only -k as edgeObjectsToAdd[k]. A removed object
-- goes in edgeObjectsToRemove, and Apply rewrites every line and the station
-- group BEFORE that entity dies. The old rebuild re-added the neighbours as
-- fresh -k entities and never listed the removed one: THAT left lines on dead
-- ids (fatal assert on both peers) -- not the rebuild itself.
--
-- The objects pair's second value is the SIDE (STOP_LEFT=0, STOP_RIGHT=1,
-- SIGNAL=2), the same bool as the record's `left`; StreetGeometry::CreateLanes
-- keeps one slot per value per edge -- two stops on opposite sides are legal,
-- two on one side are the fatal assert. Both are derived HERE from this
-- instance's own geometry (they must agree with each other and with our node
-- order); the wire only says which convention the engine used on the host.
-- A station MERGE (a stop joining a nearby station group) is not in the
-- proposal at all: Apply decides it by distance, so the same placement gets
-- the same merge on every peer for free.

-- All edge objects on an edge as the edge lists them: { {id, side}, ... }, or
-- nil (with the count) when the list could not be read -- never a partial
-- list, which would drop the missing objects with the edge.
function CM.objectsOnEdge(eid)
	local list, n = {}, 0
	pcall(function()
		local be = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
		local objs = be and be.objects
		if not objs then return end
		n = #objs
		for i = 1, n do
			local o = objs[i]
			local id, side = tonumber(o[1]), tonumber(o[2])
			if id then list[#list + 1] = { id, side or 1 } end
		end
	end)
	if #list ~= n then return nil, n end
	return list, n
end

-- Is this edge frozen into a construction (an apron)? A plain street proposal
-- cannot remove it -- the engine answers critical=true with no message.
function CM.frozenOwnerOf(eid)
	local owner
	pcall(function()
		local comp, a = CM.edgeGeomT(eid)
		if not comp then return end
		for _, cid in pairs(game.interface.getEntities({ pos = { a[1], a[2] }, radius = 80 },
				{ type = "CONSTRUCTION", includeData = false }) or {}) do
			local cc = api.engine.getComponent(cid, api.type.ComponentType.CONSTRUCTION)
			if cc and cc.frozenEdges then
				for _, fe in pairs(cc.frozenEdges) do if fe == eid then owner = cid end end
			end
		end
	end)
	return owner
end

-- A copy of an edge as the tool makes it: placeholder entity, same nodes,
-- tangents, type and street/track props. The caller sets comp.objects.
function CM.nativeEdgeCopy(eid, isTrack, ent)
	local comp, a, b, ta, tb = CM.edgeGeomT(eid)
	if not comp then return nil end
	local e = api.type.SegmentAndEntity.new()
	e.entity = ent or -1
	e.comp.node0 = comp.node0
	e.comp.node1 = comp.node1
	e.comp.tangent0 = api.type.Vec3f.new(ta[1], ta[2], ta[3])
	e.comp.tangent1 = api.type.Vec3f.new(tb[1], tb[2], tb[3])
	e.comp.type = comp.type or 0
	e.comp.typeIndex = comp.typeIndex or -1
	e.type = isTrack and 1 or 0
	CM.copyEdgeProps(e, eid, isTrack, nil)
	return e
end

-- Every line that stops at this station (through its station group).
function CM.linesUsingStation(st)
	local out = {}
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do
			local lc = api.engine.getComponent(ls[i], api.type.ComponentType.LINE)
			local hit = false
			if lc and lc.stops then
				for j = 1, #lc.stops do
					local sg = lc.stops[j].stationGroup
					if sg and sg ~= -1 then
						pcall(function()
							local gc = api.engine.getComponent(sg, api.type.ComponentType.STATION_GROUP)
							if gc and gc.stations then
								for k = 1, #gc.stations do if gc.stations[k] == st then hit = true end end
							end
						end)
					end
				end
			end
			if hit then out[#out + 1] = ls[i] end
		end
	end)
	return out
end

-- The nearest player stop / signal within eps of a point: entity, its edge.
function CM.findStopNear(x, y, eps)
	local m = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
	local best, bestD
	for eo, eid in pairs(m) do
		if isPlayerStop(eo) then
			local d = describeStop(eo, eid)
			if d then
				local dd = (d.x - x) ^ 2 + (d.y - y) ^ 2
				if dd < eps * eps and (not bestD or dd < bestD) then best, bestD = eo, dd end
			end
		end
	end
	return best, best and m[best] or nil
end

-- Which side of an edge a point is on, in THIS instance's frame: the
-- geometric left (cross sign along node0 -> node1) and the engine's `left`
-- byte. conv is the convention measured on the originator from the very
-- object being shipped (true: engine-left is geometric-left); nil falls back
-- to the decompile's reading of the stop tool (engine-left = cross < 0).
function CM.sideOnEdge(eid, u, x, y, conv)
	local comp, a, b, ta, tb = CM.edgeGeomT(eid)
	local q = CM.hermitePos(a, ta, b, tb, u)
	local tg = CM.hermiteTangent(a, ta, b, tb, u)
	local cross = tg[1] * (y - q[2]) - tg[2] * (x - q[1])
	local geoL = cross > 0
	-- plain if/else: `conv and geoL or not geoL` is WRONG when geoL is false
	-- (Lua's and/or idiom falls through a false middle) -- it put every
	-- right-side stop on the left and echoed it back to the host (2026-09-02)
	local engL
	if conv == nil then engL = not geoL
	elseif conv then engL = geoL
	else engL = not geoL end
	return geoL, engL
end

-- Submit ONE native-shape proposal. add = { eid, u, left, side, model, name,
-- oneWay, x, y } or nil; remove = { eo, eid, x, y } or nil (either or both).
-- Returns true when a command went out (onDone(success) follows), else
-- false, why.
function CM.nativeStopProposal(add, remove, why, onDone)
	local edges = {}                      -- the add's edge is entity -1 (edgeEntity = -1)
	if add then edges[#edges + 1] = add.eid end
	if remove and remove.eid ~= (add and add.eid) then edges[#edges + 1] = remove.eid end
	local sp = api.type.SimpleProposal.new()
	local survivors = {}
	for k, eid in ipairs(edges) do
		local owner = CM.frozenOwnerOf(eid)
		if owner then return false, string.format("edge %d is frozen into construction %d -- refused (DIVERGENCE)", eid, owner) end
		local isTrack = false
		pcall(function() isTrack = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
		local e = CM.nativeEdgeCopy(eid, isTrack, -k)
		if not e then return false, string.format("edge %d gone", eid) end
		local objs, n = CM.objectsOnEdge(eid)
		if not objs then return false, string.format("edge %d: could not read its %d object(s) -- refused rather than drop them", eid, n or -1) end
		local list = {}
		for _, o in ipairs(objs) do
			if not (remove and o[1] == remove.eo) then
				list[#list + 1] = { o[1], o[2] }
				survivors[#survivors + 1] = o[1]
			end
		end
		if add and eid == add.eid then list[#list + 1] = { -1, add.side } end
		e.comp.objects = list
		sp.streetProposal.edgesToAdd[k] = e
		sp.streetProposal.edgesToRemove[k] = eid
	end
	if remove then sp.streetProposal.edgeObjectsToRemove[1] = remove.eo end
	if add then
		local eo = api.type.SimpleStreetProposal.EdgeObject.new()
		eo.edgeEntity = -1
		eo.param = add.u
		eo.left = add.left and true or false
		eo.oneWay = add.oneWay and true or false
		eo.model = add.model
		eo.playerEntity = api.engine.util.getPlayer()
		eo.name = add.name or ""
		sp.streetProposal.edgeObjectsToAdd[1] = eo
	end
	-- only the NEW and the REMOVED object are events for the poller; the
	-- survivors keep their ids and it never sees them change
	if add then CM.expectAdd(CM.expectStop, add.x, add.y) end
	if remove then CM.expectAdd(CM.expectStopDel, remove.x, remove.y) end
	local cmd = api.cmd.make.buildProposal(sp, CM.buildContext(), true)
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
			if add then CM.expectDrop(CM.expectStop, add.x, add.y) end
			if remove then CM.expectDrop(CM.expectStopDel, remove.x, remove.y) end
		end
		-- the proof: every survivor still maps to an edge under the SAME id
		local kept, lost = 0, {}
		pcall(function()
			local m2 = api.engine.system.streetSystem.getEdgeObject2EdgeMap() or {}
			for _, id in ipairs(survivors) do
				if m2[id] then kept = kept + 1 else lost[#lost + 1] = tostring(id) end
			end
		end)
		log(string.format("EXEC %s: %d edge(s)%s%s success=%s%s; survivors kept %d/%d%s", why, #edges,
			add and " +add" or "", remove and (" -rm " .. tostring(remove.eo)) or "", tostring(success), msg,
			kept, #survivors, #lost > 0 and (" LOST [" .. table.concat(lost, ",") .. "]") or ""))
		if onDone then onDone(success) end
	end)
	return true
end

-- STOPADD and STOPREP (c.rx/c.ry = the stop the originator's placement
-- replaced). Returns true when a proposal went out.
function CM.execStopAdd(c)
	if not K.STOPS_NATIVE then CM.execStopAddLegacy(c); return false end
	local sent = false
	local ok, err = pcall(function()
		local tag = string.format("%s seq=%s", c.rx and "STOPREP" or "STOPADD", tostring(c.seq))
		local wantTrack = tonumber(c.track) == 1
		-- Which edge: the originator's own edge if we have it (both endpoints
		-- within 2 m), else the nearest centreline to the stop's WORLD position
		-- (the peer's town road may be nodded differently). The shipped position
		-- is the SHELTER model, which stands at the kerb -- 6-8 m off the
		-- centreline of a medium town road -- so the search must be wide: the
		-- 5 m split tolerance found nothing for five stops in a row (2026-09-02).
		local eid, u
		if c.ax and c.bx then eid = CM.findEdgeByEnds(wantTrack, c.ax, c.ay, c.bx, c.by, 2.0) end
		if eid then
			u = CM.uOnEdgeFine(eid, c.x, c.y)
			if not u then eid = nil end
		end
		if not eid then eid, u = CM.findEdgeContaining(wantTrack, c.x, c.y, nil, K.STOP_EDGE_EPS) end
		if not eid then
			log(string.format("%s: no %s edge within %.0f m of %.1f,%.1f on this instance -- skipped (DIVERGENCE)",
				tag, wantTrack and "track" or "street", K.STOP_EDGE_EPS, c.x, c.y))
			return
		end
		local comp, a, b = CM.edgeGeomT(eid)
		local sameEnds = false
		if c.ax then
			local function near(pt, x, y) return (pt[1] - x) ^ 2 + (pt[2] - y) ^ 2 < 4 end
			sameEnds = (near(a, c.ax, c.ay) and near(b, c.bx, c.by)) or (near(a, c.bx, c.by) and near(b, c.ax, c.ay))
		end
		-- side and left, in OUR frame, with the convention the host measured
		local hostSide = tonumber(c.side)
		local conv
		if tonumber(c.conv) == 1 then conv = true
		elseif tonumber(c.conv) == 0 then conv = false
		elseif hostSide == 0 or hostSide == 1 then conv = ((hostSide == 0) == (tonumber(c.left) == 1))
		else conv = CM.leftConv end
		local geoL, engL = CM.sideOnEdge(eid, u, c.x, c.y, conv)
		-- STRICT (STOPX): the engine's own left byte rides the wire with the
		-- originator's unit tangent at the object. Flip it only when OUR matched
		-- edge runs the other way (tangent dot < 0). No geometry guess: exact for
		-- a track object, whose engine `left` is not its model's geometric side,
		-- and for an on-centreline waypoint, where the cross sign is noise.
		local flipped = nil
		if c.eleft ~= nil and c.tx ~= nil and c.ty ~= nil then
			local _, a2, b2, ta2, tb2 = CM.edgeGeomT(eid)
			local tg = CM.hermiteTangent(a2, ta2, b2, tb2, u)
			flipped = (tg[1] * tonumber(c.tx) + tg[2] * tonumber(c.ty)) < 0
			engL = (tonumber(c.eleft) == 1)
			if flipped then engL = not engL end
		end
		local side = (hostSide == 2 or tonumber(c.kind) == 2) and 2 or (engL and 0 or 1)
		local objs, n = CM.objectsOnEdge(eid)
		if not objs then
			log(string.format("%s: edge %d: could not read its %d object(s) -- skipped (DIVERGENCE)", tag, eid, n or -1))
			return
		end
		-- REPLACE: the object the originator's tool removed with this placement
		local rm = nil
		if c.rx then
			local old, oldEid = CM.findStopNear(tonumber(c.rx), tonumber(c.ry), 2.0)
			if old then
				rm = { eo = old, eid = oldEid, x = tonumber(c.rx), y = tonumber(c.ry) }
				local lines = CM.linesUsingStation(old)
				if #lines > 0 then
					if not K.STOPS_DEL_ON_LINE then
						log(string.format("%s: replaced stop %d is used by %d line(s) and stops_del_on_line=0 -- refused (DIVERGENCE)", tag, old, #lines))
						return
					end
					log(string.format("%s: replaced stop %d is used by %d line(s) -- the engine rewrites them, the LUPDATE behind this restores the stop", tag, old, #lines))
				end
			else
				log(string.format("%s: the replaced stop near %.1f,%.1f is not here -- placing the new one only", tag, tonumber(c.rx), tonumber(c.ry)))
			end
		end
		-- guards, ignoring the object being replaced: duplicate, same side
		-- (one object per side per edge -- the CreateLanes rule), and a road of
		-- another type already carrying something (its lane layout is unknown)
		local stname = ""
		pcall(function()
			local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
			if sc then stname = api.res.streetTypeRep.getName(sc.streetType) or "" end
		end)
		local hostSt = CM.unescName(c.stname or "")
		local wantModel = CM.unescName(c.model)
		for _, o in ipairs(objs) do
			if not (rm and o[1] == rm.eo) then
				local d = describeStop(o[1], eid)
				local samePlace = d and (d.x - c.x) ^ 2 + (d.y - c.y) ^ 2 < 1.0
				if samePlace and d.model == wantModel then
					log(string.format("%s: a stop already stands at %.1f,%.1f -- nothing to do", tag, c.x, c.y))
					return
				end
				-- "One object per side per edge" is the STREET CreateLanes rule
				-- (one bus/tram shelter per road side). It does NOT hold for TRACK
				-- signals/waypoints (side 2): a track edge carries MANY signals
				-- (block signalling), so the originator's native tool places a
				-- second signal on an edge that already has one, while this guard
				-- refused it on the peers -- dropping the signal (A-native vs peer,
				-- 2026-09-08). Skip the guard for side 2; the co-location (<1 m)
				-- check above still stops true duplicates, and nativeStopProposal +
				-- the engine are the final arbiter (A already proved it accepts it).
				if (samePlace or o[2] == side) and side ~= 2 and not rm then
					-- ONE-CLICK REPLACE (2026-09-08). The strict STOPX carries no
					-- removal position (the old poll-based STOPREP did), and this
					-- guard read every click on an occupied side as a pure add and
					-- refused it -- so a bus stop over a truck stop (or the reverse)
					-- never worked under strict. The engine allows one object per
					-- side per edge, so natively that click can only be a replace:
					-- remove the object that holds the side in the same proposal.
					rm = { eo = o[1], eid = eid, x = d and d.x or c.x, y = d and d.y or c.y }
					local lines = CM.linesUsingStation(o[1])
					if #lines > 0 then
						if not K.STOPS_DEL_ON_LINE then
							log(string.format("%s: replaced stop %d is used by %d line(s) and stops_del_on_line=0 -- refused (DIVERGENCE)", tag, o[1], #lines))
							return
						end
						log(string.format("%s: replacing stop %d (%s) used by %d line(s) -- the engine rewrites them, the LUPDATE behind this restores the stop",
							tag, o[1], d and d.model or "?", #lines))
					else
						log(string.format("%s: replacing stop %d (%s) on side %d with %s", tag, o[1], d and d.model or "?", side, wantModel))
					end
				elseif o[2] == side and side ~= 2 then
					log(string.format("%s: edge %d already carries object %d on side %d -- one per side per edge, skipped (DIVERGENCE)",
						tag, eid, o[1], side))
					return
				end
				if hostSt ~= "" and stname ~= "" and stname ~= hostSt then
					log(string.format("%s: edge %d is '%s' here, '%s' on the originator, and already carries an object -- lane layout unknown, skipped (DIVERGENCE)",
						tag, eid, stname, hostSt))
					return
				end
			end
		end
		log(string.format("%s: edge %d (%s) u=%.3f geoLeft=%s %s -> left=%s side=%d, %d object(s) on it%s",
			tag, eid, sameEnds and "same ends" or "drifted ends", u, tostring(geoL),
			flipped ~= nil and string.format("engine-left=%s edge %s", tostring(tonumber(c.eleft) == 1), flipped and "REVERSED here" or "same way")
				or ("conv=" .. tostring(conv)),
			tostring(engL), side, #objs, rm and (", replacing " .. tostring(rm.eo)) or ""))
		local okB, why = CM.nativeStopProposal(
			{ eid = eid, u = u, left = engL, side = side, model = CM.unescName(c.model), name = CM.unescName(c.name),
			  oneWay = tonumber(c.oneWay) == 1, x = c.x, y = c.y },
			rm, string.format("%s origin=%s '%s'", tag, tostring(c.origin), CM.unescName(c.name)),
			function() CM.conxBusy = false end)
		if okB then sent = true else log(string.format("%s: %s -- skipped", tag, tostring(why))) end
	end)
	if not ok then log("exec STOPADD error: " .. tostring(err)) end
	return sent
end

function CM.execStopDel(c)
	if not K.STOPS_NATIVE then CM.execStopDelLegacy(c); return false end
	local sent = false
	local ok, err = pcall(function()
		local tag = string.format("STOPDEL seq=%s", tostring(c.seq))
		local best, eid = CM.findStopNear(c.x, c.y, 2.0)
		if not best then
			log(string.format("%s: no roadside stop within 2 m of %.1f,%.1f -- skipped", tag, c.x, c.y))
			return
		end
		local lines = CM.linesUsingStation(best)
		if #lines > 0 then
			if not K.STOPS_DEL_ON_LINE then
				log(string.format("%s: stop %d is used by %d line(s) and stops_del_on_line=0 -- refused (DIVERGENCE)", tag, best, #lines))
				return
			end
			log(string.format("%s: stop %d is used by %d line(s) -- removed natively, the engine rewrites the lines first", tag, best, #lines))
		end
		local okB, why = CM.nativeStopProposal(nil, { eo = best, eid = eid, x = c.x, y = c.y },
			string.format("%s origin=%s", tag, tostring(c.origin)), function() CM.conxBusy = false end)
		if okB then sent = true else log(string.format("%s: %s -- skipped", tag, tostring(why))) end
	end)
	if not ok then log("exec STOPDEL error: " .. tostring(err)) end
	return sent
end

-- Stop replays run ONE AT A TIME through the construction queue, and each
-- re-reads the edge and its objects when it actually runs. Two stops on one
-- edge in the same stamp used to build from the same pre-apply world: the
-- second listed an edge the first had already replaced and, by positive id, an
-- object the first had removed -- a dead reference handed to the engine.
function CM.stopRun(c)
	CM.conxBusy, CM.conxBusyAt = true, CM.gameTime() or 0
	local sent = false
	local ok, err = pcall(function()
		if c.op == "STOPDEL" then sent = CM.execStopDel(c) else sent = CM.execStopAdd(c) end
	end)
	if not ok then log(string.format("%s seq=%s: error %s", tostring(c.op), tostring(c.seq), tostring(err))) end
	if not sent then CM.conxBusy = false end
end

function CM.stopEnqueue(c)
	if tonumber(c.skipOrigin or 0) == 1 and c.origin == K.INSTANCE then return end
	local nowB = CM.gameTime() or 0
	if CM.conxBusy and CM.conxBusyAt and (nowB - CM.conxBusyAt) > 3.0 then
		log(string.format("%s: previous replay never reported back (%.1f units) -- releasing the queue", tostring(c.op), nowB - CM.conxBusyAt))
		CM.conxBusy = false
	end
	if CM.conxBusy or #CM.conxQueue > 0 then
		CM.conxQueue[#CM.conxQueue + 1] = { c = c, notBefore = nowB }
		log(string.format("%s seq=%s: a replay is in flight -- queued (%d waiting)", tostring(c.op), tostring(c.seq), #CM.conxQueue))
		return
	end
	CM.stopRun(c)
end

-- The queue pump's dispatcher: constructions, stops and the line ops that
-- were held behind them.
function CM.runQueued(c)
	if c.op == "STOPADD" or c.op == "STOPDEL" or c.op == "STOPREP" then CM.stopRun(c)
	elseif c.op == "LCREATE" or c.op == "LUPDATE" or c.op == "LDELETE" then CM.execLine(c)
	else CM.execConX(c) end
end
end
