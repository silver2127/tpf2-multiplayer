-- mp/lines.lua -- lines: cross-peer identity, create/update/delete
--
-- Split out of lockstep.lua on 2026-09-08. Loaded from the game script as
--     require("mp.lines")(CM, K, log)
-- A FACTORY so each load of the game script gets fresh file-scope state.
-- Symbols shared between modules live in CM (CM.<name>); K is the constants
-- table, log the instance-tagged logger. Body kept at column 0 on purpose:
-- tools/luacheck.py's use-before-define checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- lines: cross-peer identity + Create / Update / Delete ----------
--
-- Same shape as vehicles: a created line gets the key origin:seq, each peer
-- records which LOCAL line entity appeared for it (lineSystem.getLines() minus
-- the known set), save lines are s:<id>. Content is READ BACK from the entity
-- (name, colour, waitingTime, stops) and stops travel as station-group
-- POSITIONS; the peer finds its own station group within 20 m.
local lineIdOf
CM.lineKeyOf, lineIdOf = {}, {}
local knownLines
knownLines, CM.primedLines = {}, {}
local pendingLineKeys = {}          -- { key=, since= }   (peer: waiting for its replayed line)
CM.pendingLineCreates = {}       -- { since= }         (originator: waiting to read the new line)
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
	CM.lineKeyOf[lid] = key
	lineIdOf[key] = lid
	knownLines[lid] = true
	log(string.format("line: %s <-> local line %d", key, lid))
end

function CM.lineKeyFor(lid)
	if CM.lineKeyOf[lid] then return CM.lineKeyOf[lid] end
	if CM.primedLines[lid] then return "s:" .. tostring(lid) end
	log(string.format("line: local line %s has no cross-peer key -- not shipped", tostring(lid)))
	return nil
end

function CM.lineIdFor(key)
	if lineIdOf[key] then return lineIdOf[key] end
	local s = tostring(key):match("^s:(%-?%d+)$")
	local id = s and tonumber(s) or nil
	if id and CM.primedLines[id] then return id end
	return nil
end

function CM.forgetLine(lid)
	local key = CM.lineKeyOf[lid]
	if key then lineIdOf[key] = nil end
	CM.lineKeyOf[lid] = nil
	-- ids get reused: a deleted line's id must not stay 'known', or the next
	-- line to reuse it is invisible to pairing and never replicates.
	knownLines[lid] = nil
	CM.primedLines[lid] = nil
end

function CM.stationGroupPos(sg)
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
			local sx, sy = CM.stationGroupPos(sg)
			if sx then
				local d = (sx - x) ^ 2 + (sy - y) ^ 2
				if d < 400 and (not bestD or d < bestD) then best, bestD = sg, d end
			end
		end
	end)
	return best
end

-- World position of a station entity (a roadside stop or a construction's
-- station): the interface first, the model transform as the fallback.
function CM.stationPos(st)
	local okE, e = pcall(game.interface.getEntity, st)
	if okE and e and e.position then return e.position[1] or e.position.x, e.position[2] or e.position.y end
	local okM, x, y = pcall(function()
		local mil = api.engine.getComponent(st, api.type.ComponentType.MODEL_INSTANCE_LIST)
		local fi = mil.fatInstances[1]
		return fi.transf[13], fi.transf[14]
	end)
	if okM and x then return x, y end
	return nil
end
function CM.stationPosInGroup(sg, idx)
	local x, y
	pcall(function()
		local gc = api.engine.getComponent(sg, api.type.ComponentType.STATION_GROUP)
		local st = gc and gc.stations and gc.stations[(tonumber(idx) or 0) + 1]
		if st then x, y = CM.stationPos(st) end
	end)
	return x, y
end

-- Two stop signatures name the same stops when every group (and station, when
-- shipped) is within 2 m and the other fields match. A string compare is too
-- strict: positions come from each instance's own geometry.
function CM.stopsSigEqual(a, b)
	if a == b then return true end
	local ok, same = pcall(function()
		local function parse(s)
			local recs = {}
			for rec in tostring(s or ""):gmatch("[^;]+") do
				local f = {}
				for v in rec:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
				recs[#recs + 1] = f
			end
			return recs
		end
		local ra, rb = parse(a), parse(b)
		if #ra ~= #rb then return false end
		for i = 1, #ra do
			local p, q = ra[i], rb[i]
			if (p[1] - q[1]) ^ 2 + (p[2] - q[2]) ^ 2 > 4 then return false end
			for k = 4, 7 do if p[k] ~= q[k] then return false end end
			if p[8] and q[8] then
				if (p[8] - q[8]) ^ 2 + (p[9] - q[9]) ^ 2 > 4 then return false end
			elseif p[3] ~= q[3] then return false end
		end
		return true
	end)
	return ok and same or false
end

-- name / colour / wait / stops of a line, ready for the wire; nil if a stop
-- cannot be located (it would be unreplayable anyway). Each stop carries the
-- station GROUP's position, the station index inside it, and (fields 8-9) the
-- station's own position: the index is edge-frame data -- a merged pair lists
-- [right-side, left-side] relative to the edge's node order, which differs per
-- instance -- so the peer resolves the station by where it stands.
function CM.lineSnapshot(lid)
	local snap
	pcall(function()
		local lc = api.engine.getComponent(lid, api.type.ComponentType.LINE)
		if not lc or not lc.stops then return end
		local stops, alts = {}, {}
		for i = 1, #lc.stops do
			local s = lc.stops[i]
			local x, y = CM.stationGroupPos(s.stationGroup)
			if not x then return end
			local sx, sy = CM.stationPosInGroup(s.stationGroup, s.station)
			stops[#stops + 1] = string.format("%.2f,%.2f,%d,%d,%d,%d,%d", x, y,
				tonumber(s.station) or 0, tonumber(s.terminal) or 0, tonumber(s.loadMode) or 0,
				tonumber(s.minWaitingTime) or 0, tonumber(s.maxWaitingTime) or 180)
				.. (sx and string.format(",%.1f,%.1f", sx, sy) or "")
			-- alternative platforms (the line editor's multi-terminal choice)
			local al = {}
			pcall(function()
				local at = s.alternativeTerminals
				if at then for a = 1, #at do al[#al + 1] = string.format("%d:%d", tonumber(at[a].station) or 0, tonumber(at[a].terminal) or 0) end end
			end)
			alts[#alts + 1] = table.concat(al, "/")
		end
		local name = ""
		pcall(function() name = game.interface.getName(lid) or "" end)
		local r, g, b = 0.9, 0.2, 0.2
		pcall(function()
			local cc = api.engine.getComponent(lid, api.type.ComponentType.COLOR)
			if cc and cc.color then r, g, b = cc.color.x or cc.color[1], cc.color.y or cc.color[2], cc.color.z or cc.color[3] end
		end)
		snap = { name = CM.escName(name), color = string.format("%.3f,%.3f,%.3f", r, g, b),
		         wait = tonumber(lc.waitingTime) or 180, stops = table.concat(stops, ";"),
		         alts = table.concat(alts, ";") }
	end)
	return snap
end

-- ---------- rapid line edits merge onto the update still waiting ----------
-- A line update carries the WHOLE new stop list, built by the line editor from
-- the line as the engine holds it. Cancelled and replayed, it lands only at its
-- stamp, so a second click inside that window was built from the OLD list and
-- replaced the stop the first click added (line 14, 2026-09-10: three quick
-- stations applied as 1, 1 and 2 stops on every instance). At capture the click
-- is diffed against the engine's list and that change is replayed onto the
-- newest update still waiting for the same line.
local function lineCount(str)
	if not str or str == "" then return 0 end
	local n = 0
	for _ in (str .. ";"):gmatch("([^;]*);") do n = n + 1 end
	return n
end
local function lineSplit(str, n)
	local t = {}
	if n == 0 then return t end
	for part in ((str or "") .. ";"):gmatch("([^;]*);") do t[#t + 1] = part end
	while #t < n do t[#t + 1] = "" end
	while #t > n do t[#t] = nil end
	return t
end
local function stopKey(entry)
	local a, b, c, d = tostring(entry):match("^([^,]*),([^,]*),([^,]*),([^,]*)")
	return table.concat({ a or "", b or "", c or "", d or "" }, ",")
end
CM.lineCount = lineCount

-- base = the engine's list, click = what the editor built from it, pending = the
-- list still on its way. Returns the merged stops, alts and the counts of stops
-- added, removed and re-set. Stop identity is position + station + terminal.
function CM.mergeLineEdit(baseS, baseA, clickS, clickA, pendS, pendA)
	local nb, nc, np = lineCount(baseS), lineCount(clickS), lineCount(pendS)
	local B, C, P = lineSplit(baseS, nb), lineSplit(clickS, nc), lineSplit(pendS, np)
	local BA, CA, PA = lineSplit(baseA, nb), lineSplit(clickA, nc), lineSplit(pendA, np)
	local L = {}
	for i = nb + 1, 1, -1 do
		L[i] = {}
		for j = nc + 1, 1, -1 do
			if i > nb or j > nc then L[i][j] = 0
			elseif stopKey(B[i]) == stopKey(C[j]) then L[i][j] = L[i + 1][j + 1] + 1
			else L[i][j] = math.max(L[i + 1][j], L[i][j + 1]) end
		end
	end
	local function find(key)
		for k = 1, #P do if stopKey(P[k]) == key then return k end end
		return nil
	end
	local i, j, adds, dels, sets = 1, 1, 0, 0, 0
	while i <= nb or j <= nc do
		if i <= nb and j <= nc and stopKey(B[i]) == stopKey(C[j]) then
			if B[i] ~= C[j] or BA[i] ~= CA[j] then
				local k = find(stopKey(B[i]))
				if k then P[k], PA[k] = C[j], CA[j]; sets = sets + 1 end
			end
			i, j = i + 1, j + 1
		elseif j <= nc and (i > nb or L[i][j + 1] >= L[i + 1][j]) then
			local k = (i <= nb) and find(stopKey(B[i])) or nil
			if k then table.insert(P, k, C[j]); table.insert(PA, k, CA[j])
			else P[#P + 1] = C[j]; PA[#PA + 1] = CA[j] end
			adds = adds + 1
			j = j + 1
		else
			local k = find(stopKey(B[i]))
			if k then table.remove(P, k); table.remove(PA, k) end
			dels = dels + 1
			i = i + 1
		end
	end
	return table.concat(P, ";"), table.concat(PA, ";"), adds, dels, sets
end

-- The newest update for `key` that may not show on the entity yet: one still
-- queued (any origin), or one of ours captured in the last few game units.
function CM.linePending(key)
	local best
	for _, c in ipairs(CM.queue or {}) do
		if c.op == "LUPDATE" and c.key == key and c.stops then
			local at, bat = tonumber(c.at) or 0, best and (tonumber(best.at) or 0) or nil
			if not best or at > bat or (at == bat and (tonumber(c.seq) or 0) > (tonumber(best.seq) or 0)) then best = c end
		end
	end
	if best then return best end
	local sent = CM.lineSent and CM.lineSent[key]
	if sent and ((CM.gameTime() or 0) - (sent.t or 0)) < 3 then return sent end
	return nil
end
function CM.noteLineSent(key, stops, alts)
	CM.lineSent = CM.lineSent or {}
	CM.lineSent[key] = { stops = stops, alts = alts, t = CM.gameTime() or 0 }
end

function CM.primeLineKeys()
	if linesPrimed then return end
	linesPrimed = true
	local n = 0
	for _, lid in ipairs(allLines()) do CM.primedLines[lid] = true; knownLines[lid] = true; n = n + 1 end
	log(string.format("line: primed %d save line(s) as known / s:<id>", n))
end

function CM.pollLineKeys()
	if #pendingLineKeys == 0 and #CM.pendingLineCreates == 0 then return end
	local now = CM.gameTime()
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
			local snap = CM.lineSnapshot(fresh[fi])
			if snap and (not p.sig or CM.stopsSigEqual(snap.stops, p.sig)) then hit = fi; break end
		end
		if hit then
			registerLineKey(p.key, fresh[hit])
			if p.company then CM.cmReassignEntity(fresh[hit], p.company, "line") end   -- companies mode
			table.remove(fresh, hit)
			table.remove(pendingLineKeys, pk)
		end
	end
	-- ORIGINATOR: any remaining fresh line is a local UI creation -> ship it
	while #CM.pendingLineCreates > 0 and #fresh > 0 do
		local lid = table.remove(fresh, 1)
		table.remove(CM.pendingLineCreates, 1)
		local snap = CM.lineSnapshot(lid)
		if snap then
			CM.scheduleLocal("LCREATE", { name = snap.name, color = snap.color, wait = snap.wait,
			                           stops = snap.stops, skipOrigin = 1 })
			registerLineKey(K.INSTANCE .. ":" .. tostring(CM.seqNo), lid)
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
	for i = #CM.pendingLineCreates, 1, -1 do
		if now - CM.pendingLineCreates[i].since > 6 then table.remove(CM.pendingLineCreates, i); log("line: LCREATE never produced a line -- dropped") end
	end
	for i = #pendingLineKeys, 1, -1 do
		if now - pendingLineKeys[i].since > 6 then log("line: key " .. pendingLineKeys[i].key .. " never produced a line -- dropped"); table.remove(pendingLineKeys, i) end
	end
end

local function buildLineObject(c)
	local lineObj = api.type.Line.new()
	lineObj.waitingTime = tonumber(c.wait) or 180
	local n = 0
	local altList = nil
	if c.alts and c.alts ~= "" then
		altList = {}
		-- keep empty entries: "a;;b" must stay aligned with the stops
		for rec in (tostring(c.alts) .. ";"):gmatch("([^;]*);") do altList[#altList + 1] = rec end
	end
	for rec in tostring(c.stops or ""):gmatch("[^;]+") do
		local f = {}
		for v in rec:gmatch("[^,]+") do f[#f + 1] = tonumber(v) end
		if #f < 7 then error("bad stop record " .. rec) end
		local sg = findStationGroupNear(f[1], f[2])
		if not sg then error(string.format("no station group within 20 m of %.1f,%.1f", f[1], f[2])) end
		local idx = f[3]
		if f[8] and f[9] then
			-- resolve the station inside the group by position (see lineSnapshot)
			local okG, gc = pcall(api.engine.getComponent, sg, api.type.ComponentType.STATION_GROUP)
			if okG and gc and gc.stations then
				local best, bestD
				for k = 1, #gc.stations do
					local px, py = CM.stationPos(gc.stations[k])
					if px then
						local d = (px - f[8]) ^ 2 + (py - f[9]) ^ 2
						if d < 100 and (not bestD or d < bestD) then best, bestD = k - 1, d end
					end
				end
				if best and best ~= idx then
					log(string.format("line: station index %d -> %d in group %d (resolved by position %.1f,%.1f)", idx, best, sg, f[8], f[9]))
				end
				if best then idx = best end
			end
		end
		local s = api.type.Line.Stop.new()
		s.stationGroup = sg
		s.station = idx
		s.terminal = f[4]
		s.loadMode = f[5]
		s.minWaitingTime = f[6]
		s.maxWaitingTime = f[7]
		n = n + 1
		-- alternative platforms, aligned by stop index in c.alts ("st:term/st:term;;...")
		local altRec = altList and altList[n]
		if altRec and altRec ~= "" then
			local okA, errA = pcall(function()
				local at = s.alternativeTerminals
				local k = 0
				for st, term in altRec:gmatch("(%d+):(%d+)") do
					local t = api.type.StationTerminal.new()
					t.station = tonumber(st); t.terminal = tonumber(term)
					k = k + 1; at[k] = t
				end
				s.alternativeTerminals = at
			end)
			if not okA then log(string.format("line: stop %d alternative terminals not applied: %s", n, tostring(errA))) end
		end
		lineObj.stops[n] = s
	end
	return lineObj, n
end

-- A line op replayed on a peer can share a batch with the LCREATE that makes
-- its line, and createLine materializes its entity (and binds its key) only on
-- a LATER sim step -- so lineIdFor is nil for a few steps. Dropping the op there
-- (the original behaviour) left the peer's line without the stops the host added:
-- a vehicle later assigned to that 0-stop line crashed the path solver
-- (move_path_util_common) and the assignment was simply lost -- the "set line
-- in a batch never arrived" case (2026-09-08). Retry on the SAME deterministic
-- step cadence the VLINE paths use (advanced from the AGREED stamp, never local
-- game-time) so every peer retries on identical steps, then give up loudly.
local function retryLineDep(c)
	c.tries = (tonumber(c.tries) or 0) + 1
	if c.tries <= 30 then
		c.notBeforeStep = (c.notBeforeStep or CM.stepOf(c.at)) + K.VLINE_RETRY_STEPS
		CM.retryQueue = CM.retryQueue or {}
		CM.retryQueue[#CM.retryQueue + 1] = c
		if c.tries == 1 or c.tries % 10 == 0 then
			log(string.format("%s seq=%s: line key %s not bound yet -- retry %d (step %d)",
				tostring(c.op), tostring(c.seq), tostring(c.key), c.tries, c.notBeforeStep))
		end
	else
		log(string.format("%s seq=%s: line key %s never bound after %d tries -- dropped (DIVERGENCE)",
			tostring(c.op), tostring(c.seq), tostring(c.key), c.tries))
	end
end

function CM.execLine(c)
	-- LCREATE is never cancelled (the editor's UpdateLine(-1) on a cancelled
	-- create is a fatal assert), so the originator always skips it. LUPDATE /
	-- LDELETE replay here too when the slice cancelled them (armed=1).
	if c.origin == K.INSTANCE and (not K.STRICT_OPS[c.op] or tonumber(c.armed or 1) == 0) then
		log(string.format("%s seq=%s: originator already applied locally, skipping", c.op, tostring(c.seq)))
		return
	end
	if c.origin == K.INSTANCE then
		log(string.format("%s seq=%s: STRICT -- originator replaying at stamp (local was cancelled)", c.op, tostring(c.seq)))
	end
	local ok, err = pcall(function()
		if c.op == "LCREATE" then
			local lineObj, n = buildLineObject(c)
			local r, g, b = tostring(c.color or ""):match("^([^,]+),([^,]+),([^,]+)$")
			local color = api.type.Vec3f.new(tonumber(r) or 0.9, tonumber(g) or 0.2, tonumber(b) or 0.2)
			local name = CM.unescName(c.name)
			local key = tostring(c.origin) .. ":" .. tostring(c.seq)
			api.cmd.sendCommand(api.cmd.make.createLine(name, color, api.engine.util.getPlayer(), lineObj),
				function(res, success)
					log(string.format("EXEC LCREATE seq=%s origin=%s at=%s '%s' stops=%d success=%s",
						tostring(c.seq), tostring(c.origin), tostring(c.at), name, n, tostring(success)))
					if success then pendingLineKeys[#pendingLineKeys + 1] = { key = key, sig = c.stops, since = CM.gameTime() or 0, company = c.company and tonumber(c.company) or nil } end
				end)
		elseif c.op == "LUPDATE" then
			local lid = CM.lineIdFor(c.key)
			if not lid then retryLineDep(c); return end
			local lineObj, n = buildLineObject(c)
			api.cmd.sendCommand(api.cmd.make.updateLine(lid, lineObj), function(res, success)
				log(string.format("EXEC LUPDATE seq=%s origin=%s at=%s %s stops=%d success=%s",
					tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.key), n, tostring(success)))
			end)
		elseif c.op == "LDELETE" then
			local lid = CM.lineIdFor(c.key)
			if not lid then retryLineDep(c); return end
			api.cmd.sendCommand(api.cmd.make.deleteLine(lid), function(res, success)
				log(string.format("EXEC LDELETE seq=%s origin=%s at=%s %s success=%s",
					tostring(c.seq), tostring(c.origin), tostring(c.at), tostring(c.key), tostring(success)))
				if success then CM.forgetLine(lid) end
			end)
		end
	end)
	if not ok then log(string.format("exec%s error: %s", tostring(c.op), tostring(err))) end
end
end
