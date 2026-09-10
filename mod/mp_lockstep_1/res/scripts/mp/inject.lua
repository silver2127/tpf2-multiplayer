-- mp/inject.lua -- inject reader (pollInject)
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.inject")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
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

function CM.pollInject()
	if not K.INJECT_FILE then return end
	local data, newOff = CM.readFrom(K.INJECT_FILE, CM.injectOffset)
	CM.injectOffset = newOff
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
			-- Diagnostics (EVAL, HEAL) always run; a CAPTURE is dropped
			-- when nobody is playing with us, because the engine already built it.
			-- Inside the per-line pcall on purpose: a `return` here skips THIS
			-- line. Outside it, the first dropped capture abandoned every line
			-- after it in the same read -- a diagnostic queued behind a build
			-- never ran (review, 2026-08-31).
			-- The slice says, per capture, whether it cancelled the local build.
			if o == "ARMED" then CM.lastArmed = tonumber(w[2]) or 0; return end
			-- The street's bus lane and tram track, on their own line just ahead of
			-- the ROADE (ROADE is positional and length-checked, so it cannot be
			-- widened). Consumed by the next ROADE exactly as ARMED is.
			if o == "STREETP" then
				CM.lastStreetBus = tonumber(w[2]) or 0
				CM.lastStreetTram = tonumber(w[3]) or 0
				log(string.format("STREETP: hasBus=%s tramTrackType=%s",
					tostring(CM.lastStreetBus), tostring(CM.lastStreetTram)))
				return
			end
			-- A capture whose local build was CANCELLED must always be replayed,
			-- peer or no peer -- dropping it deletes the player's own work.
			if not CM.peerSeen and (CM.lastArmed or 0) == 0
			   and o ~= "EVAL" and o ~= "HEAL" and o ~= "CMNEW" and o ~= "CMSWITCH" and o ~= "CMDEL" and o ~= "CMPW" then
				CM.soloDrop(line)
				return
			end

			-- ROADN n x0 y0 x1 y1 ...   (written by slice_hook from a captured
			-- player build; carries every tessellated node)
			if o == "CMNEW" or o == "CMSWITCH" or o == "CMDEL" or o == "CMPW" then
				-- the in-game company row (GUI state) asked for a company command:
				--   CMNEW [password]   CMSWITCH cid [password]   CMDEL cid [password]   CMPW cid [password]
				-- the clear text stays here; only its salted hash goes on the wire
				local cid, pwAt = tonumber(w[2]), 3
				if o == "CMNEW" then cid = CM.cmNextId(); pwAt = 2 end
				local pw = table.concat(w, " ", pwAt)
				if cid then
					CM.scheduleLocal(o, { cid = cid, sw = (o == "CMNEW") and 1 or nil, pw = CM.cmHashPw(cid, pw) or "-" })
					log("company: requested " .. o .. " " .. cid .. (pw ~= "" and " [with password]" or ""))
				end
			elseif o == "HEAL" then
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
						pcall(function() hitEid = CM.findEdgeContaining(isTrack, xyz[1], xyz[2]) end)
						if not hitEid then pcall(function() hitEid = CM.findEdgeContaining(not isTrack, xyz[1], xyz[2]) end) end
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
						-- carry the bus lane / tram track the slice just decoded, so an
						-- upgrade that ADDS either one actually reaches the peers (and the
						-- originator, whose own upgrade was cancelled)
						if CM.lastStreetBus then sargs.bus = CM.lastStreetBus end
						if CM.lastStreetTram then sargs.tram = CM.lastStreetTram end
						CM.lastStreetBus, CM.lastStreetTram = nil, nil
						-- Decide HERE, once, and put the decisions on the wire.
						-- This runs the real replay in plan-only mode: same
						-- resolution, same splits, nothing built. Both instances
						-- then execute the originator's answer at the stamp instead
						-- of each re-deriving one against its own world.
						local planT0 = os.clock()
						local okPlan, xv, xh = pcall(function()
							return CM.execPolyline({ pts = sargs.pts, links = sargs.links,
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
							log(string.format("ROADP plan: %d vertex decision(s), %d crossing decision(s) shipped (%d ms at the click)",
								entries(xv), entries(xh), math.floor((os.clock() - planT0) * 1000 + 0.5)))
						else
							log("ROADP plan pass failed (" .. tostring(xv) .. ") -- peers will derive their own")
						end
						-- Not cancelled here (no live session at the time): the engine
						-- built it natively, so this instance must not replay it.
						if (CM.lastArmed or 1) == 0 then sargs.skipOrigin = 1 end
						CM.scheduleLocal("ROADP", sargs)
					else
						log("inject: ROADE produced no usable edges: " .. line:sub(1, 70))
					end
				else
					log("inject: bad ROADE line: " .. line:sub(1, 70))
				end
			elseif o == "CONUP" and #w >= 4 then
				-- A CANCELLED construction upgrade: the old
				-- entity id and the new CE's file/params, walked off the proposal.
				-- The entity still stands (the upgrade was cancelled), so resolve
				-- it to its position here; strict=1 makes execConU run on this
				-- instance too, and every instance upgrades at the stamp.
				local oldId = tonumber(w[2])
				local cfile = w[3]
				local tstr = tostring(w[4] or ""):match("^t=(.*)$")
				local pstr = line:match("params=(.*)$") or "{}"
				local ct = {}
				for tok in tostring(tstr or ""):gmatch("[^,]+") do ct[#ct + 1] = tonumber(tok) end
				local x, y
				pcall(function()
					if oldId and api.engine.entityExists(oldId) then
						local co = api.engine.getComponent(oldId, api.type.ComponentType.CONSTRUCTION)
						if co and co.transf then x, y = co.transf[13], co.transf[14] end
					end
				end)
				if not x and #ct == 16 then x, y = ct[13], ct[14] end
				if cfile and x and y then
					-- Ship the DIFF against the entity as it stands NOW (see CM.conDiff):
					-- the proposal was built from this same entity, so the difference
					-- is exactly this click. Falls back to the full set if the entity
					-- cannot be read (then rapid clicks may overwrite each other).
					local diffStr, nset, ndel
					pcall(function()
						local e = oldId and game.interface.getEntity(oldId)
						local p1 = CM.deserParams(pstr)
						if e and e.params and p1 then
							local d = CM.conDiff(e.params, p1)
							nset, ndel = 0, 0
							for _ in pairs(d.mset) do nset = nset + 1 end
							for _ in pairs(d.mdel) do ndel = ndel + 1 end
							diffStr = CM.ser(d)
						end
					end)
					if diffStr then
						CM.scheduleLocal("CONU", { file = cfile, x = x, y = y, params = diffStr, diff = 1, strict = 1 })
						log(string.format("CONUP: cancelled upgrade of %d (%s at %.1f,%.1f): diff %d module(s) set, %d removed -- every instance applies it at the stamp",
							oldId or -1, cfile, x, y, nset, ndel))
					else
						CM.scheduleLocal("CONU", { file = cfile, x = x, y = y, params = pstr, strict = 1 })
						log(string.format("CONUP: cancelled upgrade of %d (%s at %.1f,%.1f) -- entity unreadable, shipping the FULL set (rapid clicks may overwrite)", oldId or -1, cfile, x, y))
					end
				else
					log("inject: bad CONUP line: " .. line:sub(1, 70))
				end
			elseif o == "STOPX" and #w >= 10 then
			-- A CANCELLED stop / signal / waypoint placement:
			-- decoded off the tool's PROPOSAL by the slice (StashStopFromProposal) and
			-- written only once the cancel landed, ARMED 1 ahead of it. The native
			-- build never happened, so the edge id is still ours and the object stands
			-- nowhere yet. Ships the SAME STOPADD the poll would have -- minus
			-- skipOrigin: every instance, this one included, replays it through
			-- nativeStopProposal at the stamp.
			--
			-- Side: `left` is the ENGINE's byte straight off the record, shipped as
			-- eleft with our tangent at the object (tx,ty); a peer flips it only when
			-- ITS matched edge runs the other way. That is exact for an on-centreline
			-- object too. The poll path could not do this for a track object -- the
			-- engine byte is not readable off a built signal -- and its geometric
			-- fallback built signals facing the wrong way (2026-09-08).
			local eid, kind, mid = tonumber(w[2]), tonumber(w[3]), tonumber(w[4])
			local x, y = tonumber(w[5]), tonumber(w[6])
			local engLeft, oneWay = tonumber(w[8]) == 1, tonumber(w[9]) == 1
			local name = line:match("name=(.*)$") or ""
			if (CM.lastArmed or 0) ~= 1 then
				log("STOPX: not armed -- the native build stands, the poll captures it")
			elseif eid and kind and mid and x and y then
				local ok2, why = pcall(function()
					local comp, a, b, ta, tb = CM.edgeGeomT(eid)
					if not comp then error("edge " .. tostring(eid) .. " is not here") end
					local u = CM.uOnEdgeFine(eid, x, y) or CM.uOnEdge(eid, x, y) or 0.5
					local q = CM.hermitePos(a, ta, b, tb, u)
					local t = CM.hermiteTangent(a, ta, b, tb, u)
					local tl = math.sqrt(t[1] * t[1] + t[2] * t[2])
					if tl < 1e-6 then error("degenerate tangent on edge " .. tostring(eid)) end
					local geoLeft = (t[1] * (y - q[2]) - t[2] * (x - q[1])) > 0
					local model = api.res.modelRep.getName(mid)
					if not model or model == "" then error("model " .. tostring(mid) .. " has no name") end
					local isTrack = false
					pcall(function() isTrack = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_TRACK) ~= nil end)
					local stname = ""
					pcall(function()
						local sc = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE_STREET)
						if sc then stname = api.res.streetTypeRep.getName(sc.streetType) or "" end
					end)
					-- side as the poll encodes it: STOP_LEFT=0 / STOP_RIGHT=1 / track object=2
					local wside = kind == 2 and 2 or (engLeft and 0 or 1)
					local fields = {
						ax = a[1], ay = a[2], bx = b[1], by = b[2],
						u = u, left = geoLeft and 1 or 0, side = wside, conv = (engLeft == geoLeft) and 1 or 0,
						eleft = engLeft and 1 or 0, tx = t[1] / tl, ty = t[2] / tl,
						x = x, y = y, kind = kind == 2 and 2 or 1, track = isTrack and 1 or 0,
						oneWay = oneWay and 1 or 0, stname = CM.escName(stname),
						model = CM.escName(model), name = CM.escName(name), cancelled = 1 }
					CM.scheduleLocal("STOPADD", fields)
					log(string.format("STOPX: cancelled %s '%s' on edge %d u=%.3f engine-left=%s geo-left=%s side=%d%s -> STOPADD (strict, every instance replays)",
						model, name, eid, u, tostring(engLeft), tostring(geoLeft), wside, oneWay and " one-way" or ""))
				end)
				if not ok2 then
					-- the native build is already gone: the placement is lost on EVERY
					-- instance alike (no divergence) -- say so, the player re-places it
					log("STOPX: " .. tostring(why) .. " -- DROPPED, the cancelled placement is lost everywhere; place it again")
				end
			else
				log("inject: bad STOPX line: " .. line:sub(1, 70))
			end

			elseif o == "CONXP" and #w >= 3 then
				-- The construction HALF of a CANCELLED placement: its params walked
				-- off the PROPOSAL by the
				-- slice (StashConxpFromProposal) and written only once the cancel
				-- landed. There is no entity to poll -- the native build never
				-- happened -- so this takes the seat the entity poll would have
				-- filled in pendingCons and pairs with its ROADC like any capture.
				-- name is derived (ce.name must be non-empty: it names and owns the
				-- child depot entity); cost/bal are nil (every instance pays the same
				-- scripted cost at the stamp, so the COOP snap is skipped); survivors
				-- is the PRE-build set (all three run the identical scripted build, so
				-- the survivor-diff is a no-op). cancelled=1 makes the originator's
				-- execConX build like a peer instead of bulldoze-and-rebuild.
				local cfile = w[2]
				local tstr = tostring(w[3] or ""):match("^t=(.*)$")
				local pstr = line:match("params=(.*)$") or "{}"
				local ct = {}
				for tok in tostring(tstr or ""):gmatch("[^,]+") do ct[#ct + 1] = tonumber(tok) end
				if cfile and #ct == 16 then
					-- Name is generated at BUILD time (execConX -> CM.depotName): the
					-- engine auto-names a native placement "<town> Road depot", which
					-- a script buildProposal does not, so we reproduce it from the
					-- nearest town + a duplicate "#N" suffix, deterministic on the
					-- synced world. c.name here is only the fallback if that fails.
					local base = cfile:match("([^/]+)%.con$") or "construction"
					CM.pendingCons[#CM.pendingCons + 1] = { at = CM.gameTime() or 0, file = cfile, t = tstr, params = pstr,
						name = CM.escName(base), x = ct[13], y = ct[14], id = nil,
						survivors = CM.gatherSurvivors(ct[13], ct[14], nil), cancelled = 1 }
					log(string.format("CONXP: cancelled placement %s at (%.1f,%.1f) params=%s -- parked for pairing",
						cfile, ct[13], ct[14], pstr:sub(1, 100)))
				else
					log("inject: bad CONXP line: " .. line:sub(1, 70))
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
					CM.pendingRoadc[#CM.pendingRoadc + 1] = { at = CM.gameTime() or 0, posOf = posOf,
						adds = adds, rms = rms, spos = spos, etype = etype, stype = stype,
						ttype = ttype, cat = cat, bal0 = CM.balPrevConPoll }
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
						local k = CM.vehKeyFor(depot)
						if k then
							log(string.format("VREPL: %s, %d part(s): %s", k, #parts, enc[1]:sub(1, 60)))
							-- armed=1: the slice cancelled it and the
							-- originator replays at the stamp too; 0: it ran natively.
							CM.scheduleLocal("VREPL", { veh = k,
							                         parts = table.concat(enc, ";"),
							                         groups = table.concat(groups, "/"),
							                         armed = CM.lastArmed or 0 })
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
							for _, rec in pairs(CM.consByKey) do
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
							local bargs = { x = dx, y = dy, file = dfile or "?",
							                parts = table.concat(enc, ";"),
							                groups = table.concat(groups, "/"),
							                skipOrigin = 1 }
							-- STRICT: the slice cancelled the buy, so no vehicle
							-- will ever appear in the depot to wait for. Parking
							-- it would stall 1.5 s and then ship anyway without a
							-- purchaseTime. Ship at once and let THIS instance
							-- replay at the stamp like every peer -- which is the
							-- whole point: the entity is then created on the same
							-- sim-step everywhere.
							-- carry the slice's ARMED verdict ON the command, so the
							-- replay guard reads per-command truth
							bargs.armed = tonumber(CM.lastArmed or 0)
							if K.STRICT_OPS.VBUY and tonumber(CM.lastArmed or 0) == 1 then
								-- NO expectVehicle here. Under strict the originator
								-- REPLAYS its own buy, and execVBuy binds the key from
								-- the command's own res.resultEntity -- the exact
								-- vehicle that purchase produced. Registering a second,
								-- HINTLESS pending key here made two entries compete for
								-- one buy: the hintless one grabbed whichever fresh
								-- vehicle it found first, so with several buys and line
								-- assignments in quick succession the keys bound to the
								-- wrong vehicles and setLine landed on the peers but not
								-- on the originator (measured 2026-09-03, right after
								-- strict buys went in).
								CM.scheduleLocal("VBUY", bargs)
								log("VBUY: STRICT -- cancelled locally, shipped at once; every instance creates it at the stamp (key binds on replay)")
							else
								-- shipped by CM.shipParkedBuys once the vehicle exists (purchaseTime)
								CM.parkedBuys[#CM.parkedBuys + 1] = {
									args = bargs, depot = dparent or depot,
									since = CM.gameTime() or 0 }
							end
						end
					end
				else
					log("inject: bad " .. tostring(o) .. " line: " .. line:sub(1, 70))
				end

			elseif o == "VSELL" and #w >= 2 then
				local n = tonumber(w[2]) or 0
				local ids = {}
				for i = 1, n do local id = tonumber(w[2 + i]); if id then ids[#ids + 1] = id end end
				-- Same key-binding race as VLINE: a sell right after a batch buy
				-- finds the keys unbound and shipped NOTHING ("none shippable").
				-- Defer and retry. STRICT (ARMED 1): the sale was
				-- cancelled, the vehicles still stand here, and the originator
				-- replays at the stamp like everyone else. ARMED 0: the host sold
				-- natively already; vehKeyOf survives until forgetVehicle, so the
				-- key still resolves after the vehicle is gone locally.
				CM.deferVehCap({ kind = "VSELL", ids = ids, armed = CM.lastArmed or 0, since = CM.gameTime() or 0 })

			elseif (o == "VNAME" and #w >= 3) or (o == "VCOLOR" and #w >= 5) then
				-- The slice ships a LOCAL entity id. Work out what kind of thing it
				-- is here, while we can still ask the engine, and put the shared key
				-- on the wire instead: a vehicle key, a line key, or a position for
				-- a construction. Anything else (a town building, an industry) is
				-- not ours to rename.
				local id = tonumber(w[2])
				local kind, key
				if id then
					key = CM.vehKeyOf[id] and CM.vehKeyFor(id) or nil
					if key then kind = "veh" end
					if not key then
						key = CM.lineKeyOf[id] and CM.lineKeyFor(id) or nil
						if key then kind = "line" end
					end
					if not key then
						local co
						pcall(function() co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION) end)
						if co and co.transf then
							local ck = CM.conKey(co.transf[13], co.transf[14])
							if CM.consByKey[ck] then key, kind = ck, "con" end
						end
					end
					-- a vehicle or line that came out of the save: primed, not registered.
					-- Lines first -- forgetVehicle does not clear primedVeh, so a stale
					-- vehicle id could otherwise shadow a live line (review, 2026-08-31).
					if not key and CM.primedLines[id] then
						key = CM.lineKeyFor(id); if key then kind = "line" end
					end
					if not key and CM.primedVeh[id] then
						key = CM.vehKeyFor(id); if key then kind = "veh" end
					end
				end
				if key then
					if o == "VNAME" then
						log(string.format("VNAME: %s %s = %s", kind, key, tostring(w[3])))
						CM.scheduleLocal("VNAME", { kind = kind, key = key, name = w[3], skipOrigin = 1 })
					else
						log(string.format("VCOLOR: %s %s = %s,%s,%s", kind, key, w[3], w[4], w[5]))
						CM.scheduleLocal("VCOLOR", { kind = kind, key = key,
							r = tonumber(w[3]), g = tonumber(w[4]), b = tonumber(w[5]), skipOrigin = 1 })
					end
				else
					log(string.format("%s: entity %s is not a tracked vehicle, line or construction -- not shipped",
						o, tostring(w[2])))
				end

			elseif o == "VREV" and #w >= 2 then
				local id = tonumber(w[2])
				local k = id and CM.vehKeyFor(id)
				if k then
					log("VREV: " .. k)
					CM.scheduleLocal("VREV", { key = k, armed = CM.lastArmed or 0 })
				end

			elseif o == "VDEPOT" and #w >= 3 then
				local id, sell = tonumber(w[2]), tonumber(w[3]) or 0
				local k = id and CM.vehKeyFor(id)
				if k then
					local armed = CM.lastArmed or 0
					log(string.format("VDEPOT: %s sell=%d%s", k, sell, armed == 1 and " (strict)" or ""))
					-- armed=1: the slice cancelled it and the originator
					-- replays at the stamp too; 0: it ran natively, peers only.
					CM.scheduleLocal("VDEPOT", { key = k, sell = sell, armed = armed })
				end

			elseif o == "VLINE" and #w >= 4 then
				local id, line, stop = tonumber(w[2]), tonumber(w[3]), tonumber(w[4]) or 0
				-- A batch buy binds its vehicle keys over the next few ticks
				-- (pollVehKeys), so a SetLine captured immediately after the buy
				-- often finds vehKeyFor == nil. Dropping it silently un-assigned
				-- every such vehicle: the slice had already CANCELLED the host's
				-- SetLine (STRICT, no-callback), so the vehicle was left off the
				-- line on EVERY instance, the host included -- "only every other
				-- vehicle got assigned" (2026-09-02). Retry the capture instead:
				-- hold the raw id and re-resolve for a few seconds, then ship.
				CM.deferVehCap({ kind = "VLINE", id = id, line = line, stop = stop,
				                 armed = CM.lastArmed or 0, since = CM.gameTime() or 0 })

			elseif o == "LCREATE" then
				CM.pendingLineCreates[#CM.pendingLineCreates + 1] = { since = CM.gameTime() or 0 }

			elseif o == "LUPDATE" and #w >= 2 then
				local lid = tonumber(w[2])
				-- The UI fires UpdateLine right after CreateLine; this line is
				-- parsed BEFORE the tick's pollLineKeys would key the new line.
				-- Key it now so the first stops are not dropped.
				if lid and not CM.lineKeyOf[lid] and not CM.primedLines[lid] then CM.pollLineKeys() end
				local lk = lid and CM.lineKeyFor(lid)
				if lk then
					-- Two shapes. DECODED: the NEW stop list
					-- came off the command itself -- the cancel means the entity
					-- still holds the OLD one -- so build the stops string from it
					-- exactly as lineSnapshot would, station groups resolved to
					-- positions here while they still mean something. Name and
					-- colour are not part of an UpdateLine; read them from the entity.
					-- EVENT-ONLY (2 words, the decode failed): the update ran natively; read the
					-- whole line back as before and ship it to the peers only.
					local nstops = tonumber(w[4])
					if #w >= 4 and nstops and #w >= 4 + nstops * 7 then
						local wait = tonumber(w[3]) or 180
						local stops, alts, bad = {}, {}, nil
						local pos = 5   -- sequential: each stop carries a variable alternatives tail
						for i = 1, nstops do
							local sg, st, term = tonumber(w[pos]), tonumber(w[pos + 1]) or 0, tonumber(w[pos + 2]) or 0
							local lm, mn, mx = tonumber(w[pos + 3]) or 0, tonumber(w[pos + 4]) or 0, tonumber(w[pos + 5]) or 180
							local na = tonumber(w[pos + 6]) or 0
							pos = pos + 7
							local al = {}
							for a = 1, na do
								al[#al + 1] = string.format("%d:%d", tonumber(w[pos]) or 0, tonumber(w[pos + 1]) or 0)
								pos = pos + 2
							end
							-- NOT `sg and stationGroupPos(sg)`: `and` truncates a call to its
							-- first return, so y was always nil and every decoded LUPDATE
							-- died in string.format -- "cannot assign the new station to a
							-- line" (2026-09-08).
							local x, y
							if sg then x, y = CM.stationGroupPos(sg) end
							if not x then bad = string.format("stop %d: entity %s is not a station group", i, tostring(sg)); break end
							local sx, sy = CM.stationPosInGroup(sg, st)
							stops[#stops + 1] = string.format("%.2f,%.2f,%d,%d,%d,%d,%d", x, y, st, term, lm, mn, mx)
								.. (sx and string.format(",%.1f,%.1f", sx, sy) or "")
							alts[#alts + 1] = table.concat(al, "/")
						end
						local armed = CM.lastArmed or 0
						if bad then
							-- The DLL's +0x00 stationGroup slot is INFERRED; this is
							-- where a wrong guess shows. The cancel already happened
							-- (armed=1), so say so plainly: the edit is lost, redo it.
							log(string.format("LUPDATE: decoded line %s REJECTED (%s) -- %s", lk, bad,
								armed == 1 and "the edit was cancelled and is LOST; redo it, and report this line" or "not replicated"))
						else
							local snap = CM.lineSnapshot(lid) or {}
							local newStops, newAlts = table.concat(stops, ";"), table.concat(alts, ";")
							-- a second quick edit: the editor built it from the list before the
							-- first one landed -- put this click's change onto that one instead
							local pend = CM.linePending and CM.linePending(lk)
							if pend and snap.stops and pend.stops ~= snap.stops then
								local okM, mS, mA, adds, dels, sets = pcall(CM.mergeLineEdit, snap.stops, snap.alts, newStops, newAlts, pend.stops, pend.alts)
								if okM and mS then
									log(string.format("LUPDATE: %s edited again before %s landed -- the click's change (+%d -%d ~%d) goes onto it: %d stop(s)",
										lk, pend.seq and ("seq " .. tostring(pend.seq)) or "the last edit", adds, dels, sets, CM.lineCount(mS)))
									newStops, newAlts = mS, mA
								else
									log("LUPDATE: merge failed (" .. tostring(mS) .. ") -- shipping the click as captured")
								end
							end
							log(string.format("LUPDATE: %s decoded, %d stop(s), wait %d%s", lk, #stops, wait,
								armed == 1 and " (strict)" or ""))
							CM.scheduleLocal("LUPDATE", { key = lk, name = snap.name or "", color = snap.color or "0.9,0.2,0.2",
							                           wait = wait, stops = newStops,
							                           alts = newAlts, armed = armed })
							if CM.noteLineSent then CM.noteLineSent(lk, newStops, newAlts) end
						end
					else
						local snap = CM.lineSnapshot(lid)
						if snap then
							log(string.format("LUPDATE: %s '%s'", lk, CM.unescName(snap.name)))
							CM.scheduleLocal("LUPDATE", { key = lk, name = snap.name, color = snap.color, wait = snap.wait,
							                           stops = snap.stops, alts = snap.alts, armed = 0 })
						else
							log("LUPDATE: line " .. tostring(lid) .. " could not be read back -- not replicated")
						end
					end
				end

			elseif o == "LDELETE" and #w >= 2 then
				local lid = tonumber(w[2])
				if lid and not CM.lineKeyOf[lid] and not CM.primedLines[lid] then CM.pollLineKeys() end
				local lk = lid and CM.lineKeyFor(lid)
				if lk then
					local armed = CM.lastArmed or 0
					log(string.format("LDELETE: %s%s", lk, armed == 1 and " (strict)" or ""))
					CM.scheduleLocal("LDELETE", { key = lk, armed = armed })
					-- Under strict the line still exists here and our own replay
					-- must resolve its key; the replay callback forgets it on success.
					if armed ~= 1 then CM.forgetLine(lid) end
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
					coords[#coords + 1] = string.format("%.4f", CM.groundAt(x, y))
				end
				if coords and #coords == n * 3 and n >= 2 then
					CM.scheduleLocal("ROADN", { pts = table.concat(coords, ","),
					                         etype = etype, stype = stype, ttype = ttype })
				else
					log("inject: bad ROADN line: " .. line:sub(1, 60))
				end

			-- ROAD/RAIL x0 y0 x1 y1
			elseif (o == "ROAD" or o == "RAIL") and #w >= 5 then
				local x0, y0 = tonumber(w[2]), tonumber(w[3])
				local x1, y1 = tonumber(w[4]), tonumber(w[5])
				if x0 and y0 and x1 and y1 then
					local a = { x0 = x0, y0 = y0, z0 = CM.groundAt(x0, y0),
					            x1 = x1, y1 = y1, z1 = CM.groundAt(x1, y1), stype = 16 }
					if o == "RAIL" then
						a.ttype = tonumber(w[6]) or 1
						a.cat   = tonumber(w[7]) or 0
					end
					CM.scheduleLocal(o, a)
				end

			-- CON <file> x y
			elseif o == "CON" and #w >= 4 then
				local x, y = tonumber(w[3]), tonumber(w[4])
				if x and y then
					CM.scheduleLocal("CON", { file = w[2], x = x, y = y, z = CM.groundAt(x, y) })
				end

			-- EDEMO <re> <rn> [<node0> <node1> <kind>]*re [<id> <x> <y> <z>]*rn
			-- The slice ships node IDS because they are only ever resolved HERE,
			-- on the instance that did the bulldozing. Endpoint nodes survive an
			-- edge-only demolish, so they are still readable on this tick even
			-- though the bulldoze has already applied; nodes the bulldozer also
			-- removed are gone, and travel with their positions instead.
			elseif o == "EDEMO" and #w >= 3 then
				local re, rn = tonumber(w[2]), tonumber(w[3])
				if re and rn and #w >= 3 + re * 3 + rn * 4 then
					local gone = {}
					local base = 3 + re * 3
					for i = 0, rn - 1 do
						local id = tonumber(w[base + i * 4 + 1])
						local x  = tonumber(w[base + i * 4 + 2])
						local y  = tonumber(w[base + i * 4 + 3])
						local z  = tonumber(w[base + i * 4 + 4])
						if id and x and y then gone[id] = { x, y, z or 0 } end
					end
					-- A node the bulldozer removed is checked against `gone`
					-- FIRST and never reaches the engine. The entityExists guard
					-- covers the rest: calling a component getter on a dead id
					-- writes an ~800 KB minidump per call, and pcall does NOT
					-- stop it (nil-entity-api-call-writes-a-minidump).
					local function posOf(nid)
						if gone[nid] then return gone[nid] end
						local alive = false
						pcall(function() alive = api.engine.entityExists(nid) end)
						if not alive then return nil end
						local ok, pos = pcall(CM.nodePosXYZ, nid)
						if ok then return pos end
						return nil
					end
					local recs, lost = {}, 0
					for i = 0, re - 1 do
						local n0   = tonumber(w[3 + i * 3 + 1])
						local n1   = tonumber(w[3 + i * 3 + 2])
						local kind = tonumber(w[3 + i * 3 + 3]) or 0
						local p0 = n0 and posOf(n0)
						local p1 = n1 and posOf(n1)
						if p0 and p1 then
							recs[#recs + 1] = string.format("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d",
								p0[1], p0[2], p0[3], p1[1], p1[2], p1[3], (kind == 1) and 1 or 0)
						else
							-- Never silent: an endpoint we cannot place is a road
							-- the peers will keep, which is a divergence.
							lost = lost + 1
							log(string.format("EDEMO capture: edge %d dropped, endpoint unresolved (n0=%s p0=%s n1=%s p1=%s)",
								i, tostring(n0), tostring(p0 and "ok"), tostring(n1), tostring(p1 and "ok")))
						end
					end
					if #recs > 0 then
						CM.scheduleLocal("EDEMO", { params = table.concat(recs, ";") })
						log(string.format("EDEMO captured: %d edge(s) shipped, %d dropped", #recs, lost))
					else
						log(string.format("EDEMO captured: nothing shipped (%d dropped)", lost))
					end
				end

			-- CDEMO <n> <id>... -- a construction demolish the slice CANCELLED.
			-- The ids are local; resolve each to its file
			-- and position NOW, while the construction is still standing (that
			-- is what the cancel bought us), and ship that. strict=1 makes
			-- execDemolish run here too and match exactly, not nearest-in-30m.
			-- An id that is not a construction means the toRemove offset lied:
			-- nothing is shipped for it and the construction stays put on every
			-- instance (the bulldoze was cancelled), so nothing diverges.
			elseif o == "CDEMO" and #w >= 3 then
				local cnt = tonumber(w[2]) or 0
				for i = 1, cnt do
					local id = tonumber(w[2 + i])
					local co
					if id and id > 0 then
						pcall(function()
							if api.engine.entityExists(id) then
								co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
							end
						end)
					end
					if co and co.transf then
						local x, y = co.transf[13], co.transf[14]
						local file = tostring(co.fileName or "")
						CM.scheduleLocal("DEMOLISH", { x = x, y = y, z = CM.groundAt(x, y), file = file, strict = 1 })
						log(string.format("CDEMO: id %d -> DEMOLISH %.1f,%.1f %s (strict, replays here at the stamp)",
							id, x, y, file))
					else
						log(string.format("CDEMO: id %s is not a standing construction -- NOT shipped; the "
							.. "bulldoze was cancelled, so it still stands on every instance (the slice read a wrong id: please report it)",
							tostring(id)))
					end
				end

			-- DEMOLISH x y
			elseif o == "STOPXDEL" and #w >= 3 then
			-- A CANCELLED stop / signal bulldoze: the
			-- slice named the removed edge object off the bulldozer's proposal
			-- and cancelled the native removal, ARMED 1 ahead of this line. The
			-- object still stands here, so its position is read off it, and the
			-- STOPDEL goes out WITHOUT skipOrigin: every instance, this one
			-- included, removes it through nativeStopProposal at the stamp. The
			-- poll path removed it here at click time and on the peers two steps
			-- later; passengers walking to it re-planned on different steps and
			-- the people count diverged from there (2026-09-08).
			local eo, eid = tonumber(w[2]), tonumber(w[3])
			if (CM.lastArmed or 0) ~= 1 then
				log("STOPXDEL: not armed -- the native bulldoze ran, the stop poll ships it")
			elseif eo and eo > 0 then
				local x, y
				pcall(function()
					local mil = api.engine.getComponent(eo, api.type.ComponentType.MODEL_INSTANCE_LIST)
					local fi = mil and mil.fatInstances and mil.fatInstances[1]
					if fi then x, y = fi.transf[13], fi.transf[14] end
				end)
				if x and y then
					CM.scheduleLocal("STOPDEL", { x = x, y = y, cancelled = 1 })
					log(string.format("STOPXDEL: cancelled bulldoze of edge object %d at %.1f,%.1f (edge %s) -> STOPDEL (strict, every instance replays)",
						eo, x, y, tostring(eid)))
				else
					log(string.format("STOPXDEL: edge object %d has no model instance here -- DROPPED, the cancelled bulldoze is lost everywhere; bulldoze it again", eo))
				end
			else
				log("inject: bad STOPXDEL line: " .. line:sub(1, 70))
			end

			elseif o == "DEMOLISH" and #w >= 3 then
				local x, y = tonumber(w[2]), tonumber(w[3])
				if x and y then
					CM.scheduleLocal("DEMOLISH", { x = x, y = y, z = CM.groundAt(x, y) })
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
end
