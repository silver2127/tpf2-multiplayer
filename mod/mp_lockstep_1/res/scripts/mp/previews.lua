-- Shared road/rail previews. Cosmetic only: no command submission, entity writes,
-- save state or lockstep stamps. Copy geometry and resource names; never send
-- proposal/entity references. Native 3D rendering falls back to ground outlines.
return function(CM, K, log)
K.PREVIEW_MAX_EDGES = 24
K.PREVIEW_MAX_BYTES = 4096
K.PREVIEW_MAX_PEERS = 16
K.PREVIEW_INTERVAL_S = 0.2
K.PREVIEW_KEEPALIVE_S = 1
K.PREVIEW_STALE_S = 4

local function number(v)
	v = tonumber(v)
	if v and v == v and math.abs(v) <= 1000000 then return v end
end

local function xy(v)
	if v == nil then return nil end
	local x, y
	pcall(function() x, y = number(v[1]), number(v[2]) end)
	if not x then pcall(function() x, y = number(v.x), number(v.y) end) end
	if x and y then return { x, y } end
end

-- The base game's mission/proposalutil.lua uses this event layout:
-- param.proposal.proposal.{addedNodes,addedSegments,new2oldSegments}.
function CM.previewExtract(id, param)
	if id == "constructionBuilder" then return CM.previewExtractConstruction(param) end
	local kind = id == "streetBuilder" and "road" or (id == "trackBuilder" and "rail")
	if not kind or not param or not param.proposal then return nil end
	local sp = param.proposal.proposal
	if not sp or not sp.addedSegments or not sp.addedNodes then return nil end
	if #sp.addedSegments > 256 or #sp.addedNodes > 512 then return nil end
	local nodes, curves = {}, {}
	for _, n in ipairs(sp.addedNodes) do
		if type(n.entity) == "number" and n.comp then nodes[n.entity] = xy(n.comp.position) end
	end
	local function position(id2)
		if type(id2) ~= "number" or id2 ~= math.floor(id2) then return nil end
		if nodes[id2] then return nodes[id2] end
		if id2 < 0 or not api.engine.entityExists(id2) then return nil end
		local node = api.engine.getComponent(id2, api.type.ComponentType.BASE_NODE)
		local p = node and xy(node.position)
		nodes[id2] = p
		return p
	end
	for _, edge in ipairs(sp.addedSegments) do
		-- Existing companion replacements at crossings are not the planned route.
		if edge.type == (kind == "road" and 0 or 1) and
			(not sp.new2oldSegments or sp.new2oldSegments[edge.entity] == nil) then
			local e = edge.comp
			if not e then return nil end
			local a, b = position(e.node0), position(e.node1)
			local ta, tb = xy(e.tangent0), xy(e.tangent1)
			if not a or not b or not ta or not tb then return nil end
			curves[#curves + 1] = { a[1], a[2], b[1], b[2], ta[1], ta[2], tb[1], tb[2] }
			if #curves > K.PREVIEW_MAX_EDGES then return nil end
		end
	end
	if #curves == 0 then return nil end
	return { kind = kind, curves = curves }
end

function CM.previewEncode(p)
	if not p then return "off" end
	if p.kind == "construction" then return CM.previewEncodeConstruction(p) end
	if p.kind ~= "road" and p.kind ~= "rail" then return nil end
	if #p.curves < 1 or #p.curves > K.PREVIEW_MAX_EDGES then return nil end
	local rows = {}
	for i, c in ipairs(p.curves) do
		if #c ~= 8 then return nil end
		local values = {}
		for j = 1, 8 do
			if not number(c[j]) then return nil end
			values[j] = string.format("%.2f", c[j])
		end
		rows[i] = table.concat(values, ",")
	end
	local result = p.kind .. " " .. table.concat(rows, ";")
	if #result <= K.PREVIEW_MAX_BYTES then
		return (p.details and CM.previewEncode3d(result, p.details, p.invalid)) or result
	end
end

function CM.previewDecode(body)
	if type(body) ~= "string" or #body > K.PREVIEW_MAX_BYTES then return nil end
	if body:sub(1, 2) == "5|" then return CM.previewDecodeConstruction(body) end
	if body:sub(1, 2) == "3|" or body:sub(1, 2) == "4|" then return CM.previewDecode3d(body) end
	if body == "off" then return { off = true } end
	local kind, data = body:match("^(%a+) ([%d%.,;%-]+)$")
	if kind ~= "road" and kind ~= "rail" then return nil end
	if not data or data:find(";;", 1, true) or data:sub(1, 1) == ";" or data:sub(-1) == ";" then return nil end
	local curves = {}
	for row in data:gmatch("[^;]+") do
		local c = {}
		if row:find(",,", 1, true) or row:sub(1, 1) == "," or row:sub(-1) == "," then return nil end
		for v in row:gmatch("[^,]+") do
			local n = number(v)
			if not n then return nil end
			c[#c + 1] = n
		end
		if #c ~= 8 then return nil end
		curves[#curves + 1] = c
		if #curves > K.PREVIEW_MAX_EDGES then return nil end
	end
	if #curves == 0 then return nil end
	return { kind = kind, curves = curves }
end

-- A thin ribbon around the actual Hermite curve. setZone draws on the ground:
-- bridges/tunnels show their horizontal route, not their height or 3D model.
function CM.previewPolygon(c, width)
	local length = math.sqrt((c[3] - c[1]) ^ 2 + (c[4] - c[2]) ^ 2)
	local samples = math.max(8, math.min(48, math.ceil(length / 15)))
	local left, right = {}, {}
	for i = 0, samples do
		local t = i / samples
		local h0, h1 = 2*t^3 - 3*t^2 + 1, -2*t^3 + 3*t^2
		local h2, h3 = t^3 - 2*t^2 + t, t^3 - t^2
		local x = h0*c[1] + h1*c[3] + h2*c[5] + h3*c[7]
		local y = h0*c[2] + h1*c[4] + h2*c[6] + h3*c[8]
		local d0, d1, d2, d3 = 6*t*t-6*t, -6*t*t+6*t, 3*t*t-4*t+1, 3*t*t-2*t
		local dx = d0*c[1] + d1*c[3] + d2*c[5] + d3*c[7]
		local dy = d0*c[2] + d1*c[4] + d2*c[6] + d3*c[8]
		local norm = math.sqrt(dx*dx + dy*dy)
		if norm < 0.001 then dx, dy = c[3]-c[1], c[4]-c[2]; norm = length end
		if norm < 0.001 then return nil end
		local nx, ny = -dy / norm * width, dx / norm * width
		left[#left+1], right[#right+1] = {x+nx, y+ny}, {x-nx, y-ny}
	end
	for i = #right, 1, -1 do left[#left+1] = right[i] end
	return left
end

local function path(me, incoming)
	return K.BASE .. "tpf2mp_preview_" .. (incoming and "in_" or "out_") .. me .. ".txt"
end

local function write(path2, body)
	local f = io.open(path2, "w")
	if not f then return false end
	local written = f:write(body .. "\nend\n")
	local closed = f:close()
	return written ~= nil and closed ~= nil
end

local function read(path2)
	local f = io.open(path2, "r")
	if not f then return nil end
	local body = f:read((K.PREVIEW_MAX_BYTES + 100) * K.PREVIEW_MAX_PEERS + 100); f:close()
	if body and body:sub(-5) == "\nend\n" then return body:sub(1, -6) end
end

-- Poll changes at 5 Hz, but publish unchanged snapshots only as heartbeats.
-- Cache only successful writes so a transient I/O failure is retried next poll.
local snapshots = {}
local function publish(path2, body, now, clk)
	local old = snapshots[path2]
	if old and old.body == body and clk-old.at < K.PREVIEW_KEEPALIVE_S then return end
	if write(path2, now .. "\n" .. body) then snapshots[path2] = {body=body, at=clk} end
end
local validatedBody, validatedOK
local encodedLocal, encodedBody = nil, "off"

-- GUI and engine states communicate through bounded, complete snapshots, as
-- cursors.lua does. Heartbeats expire if either state stops updating.
function CM.previewRecv(line)
	if #line > K.PREVIEW_MAX_BYTES + 100 then return end
	local o, generation, seq, body = line:match("^LSPREVIEW o=([a-z]+) g=(%d+) s=(%d+) (.+)$")
	if not o or #o > 2 or o == K.INSTANCE then return end
	generation, seq = tonumber(generation), tonumber(seq)
	if generation > 1e12 or seq > 1e12 then return end
	CM.previewPeers = CM.previewPeers or {}
	local old = CM.previewPeers[o]
	if old and (generation < old.generation or (generation == old.generation and seq <= old.seq)) then return end
	-- The retained body was fully validated; a heartbeat changes only its age.
	if (not old or old.body ~= body) and not CM.previewDecode(body) then return end
	if not old then
		local count = 0
		for peer, p2 in pairs(CM.previewPeers) do
			if os.time()-p2.at > 8*K.PREVIEW_STALE_S then CM.previewPeers[peer] = nil
			else count = count+1 end
		end
		if count >= K.PREVIEW_MAX_PEERS then return end
	end
	CM.previewPeers[o] = { generation = generation, seq = seq, body = body, at = os.time() }
end

function CM.previewTick()
	if not K.INSTANCE or not K.BASE then return end
	local now, clk = os.time(), os.clock()
	if CM.previewScriptMe ~= K.INSTANCE then
		snapshots = {}
		CM.previewPeers = CM.previewScriptMe and {} or (CM.previewPeers or {})
		CM.previewScriptMe, CM.previewSent = K.INSTANCE, nil
		CM.previewGeneration, CM.previewSeq = now, 0
		CM.previewScriptStarted = now
	end
	if CM.previewScriptAt and clk - CM.previewScriptAt < K.PREVIEW_INTERVAL_S then return end
	CM.previewScriptAt = clk
	local raw = read(path(K.INSTANCE, false))
	local wall, body
	if raw then wall, body = raw:match("^(%d+)\n(.+)$"); wall = tonumber(wall) end
	if not wall or wall < CM.previewScriptStarted or now-wall > K.PREVIEW_STALE_S or wall > now+1 then body = "off" end
	if body ~= validatedBody then
		validatedBody, validatedOK = body, CM.previewDecode(body) ~= nil
	end
	if not validatedOK then body = "off" end
	if body ~= CM.previewSent or clk - (CM.previewSentAt or 0) >= K.PREVIEW_KEEPALIVE_S then
		CM.previewSeq = math.max(CM.previewSeq + 1, math.floor(clk * 1000))
		CM.broadcast(string.format("LSPREVIEW o=%s g=%d s=%d %s", K.INSTANCE, CM.previewGeneration, CM.previewSeq, body))
		CM.previewSent, CM.previewSentAt = body, clk
	end
	local out = {}
	for o, p in pairs(CM.previewPeers) do
		if now - p.at <= K.PREVIEW_STALE_S then
			local r, g, b = CM.cursorColor(o)
			out[#out+1] = string.format("%s %.3f %.3f %.3f %s", o, r, g, b, p.body)
		end
	end
	-- Stable order makes the content comparison independent of table iteration.
	table.sort(out)
	publish(path(K.INSTANCE, true), table.concat(out, "\n"), now, clk)
end

function CM.previewGuiEvent(id, name, param)
	if name == "builder.proposalCreate" then
		local ok, p = pcall(CM.previewExtract, id, param)
		CM.previewLocal = ok and p or nil
		if CM.previewLocal then
			local ok3, details = pcall(CM.previewExtract3d, id, param)
			if p.kind ~= "construction" and ok3 and details and #details == #p.curves then p.details = details end
			local okState, invalid = pcall(CM.previewExtractInvalid, param)
			if okState then p.invalid = invalid end
		end
		CM.previewLocalTool = CM.previewLocal and id or nil
		CM.previewEventAt = os.clock()
		if not ok and not CM.previewCaptureError then
			CM.previewCaptureError = true
			log("preview capture: " .. tostring(p))
		end
	elseif name == "builder.apply" then
		CM.previewLocal, CM.previewLocalTool = nil, nil
	elseif name == "tabWidget.currentChanged" and id == "menu.construction" then
		CM.previewLocal, CM.previewLocalTool = nil, nil
	end
	-- Never return a proposal callback result: this observer must not alter
	-- the original builder's validity, warnings, costs or confirmation.
end

-- Build 35924 proposalCreate carries data.errorState. The native renderer's
-- red flag is set from nonempty error messages, not from the warnings list.
-- Missing/unreadable state stays unknown; never turn it into a valid preview.
function CM.previewExtractInvalid(param)
	local state = param and param.data and param.data.errorState
	if not state or state.messages == nil then return nil end
	local kind = type(state.messages)
	if kind ~= "table" and kind ~= "userdata" then return nil end
	return #state.messages > 0
end

-- Measured on build 35924: cancelling sends no builder event. The unnamed
-- BuildControlComp stays alive, but its CancelButton/CostsLabel/ErrorLabel
-- become invisible. Inspect only the renderer's small action layer, never
-- the full UI/HUD tree. No callbacks or input handlers are replaced.
function CM.previewControlsVisible()
	local renderer = api.gui.util.getGameUI():getMainRendererComponent()
	local layers = renderer:getLayout()
	if not layers then return false end
	local count = 0
	local function visible(item, depth)
		count = count + 1
		if not item or depth > 8 or count > 96 then return false end
		item = api.gui.util.downcast(item)
		local name, shown, layout, n
		pcall(function() name = item:getName(); shown = item:isVisible(); layout = item:getLayout() end)
		if shown == false then return false end
		if name == "BuildControlComp::CancelButton" or name == "BuildControlComp::CostsLabel" or name == "BuildControlComp::ErrorLabel" then return shown == true end
		if layout then return visible(layout, depth+1) end
		pcall(function() n = item:getNumItems() end)
		if n then
			for i=0, math.min(n, 32)-1 do
				if visible(item:getItem(i), depth+1) then return true end
			end
		end
		return false
	end
	for i=0, math.min(layers:getNumItems(), 8)-1 do
		local layer = api.gui.util.downcast(layers:getItem(i))
		if layer:getName() == "RendererComponent::Layer1" then return visible(layer, 0) end
	end
	return false
end

-- Versioned extension: original XY curves plus heights and resource FILE NAMES.
-- Resource indices are resolved independently on each client, never transmitted.
local function resourceName(s)
	return type(s) == "string" and #s > 0 and #s <= 180 and
		not s:find("..", 1, true) and not s:find("[%c|,;\\:]") and s:sub(1, 1) ~= "/"
end
local function token(s)
	return (s:gsub("([^%w_./%-])", function(c) return string.format("%%%02X", c:byte()) end))
end
local function untoken(s)
	local decoded = s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)
	if resourceName(decoded) and token(decoded) == s then return decoded end
end
local function fields(row, delimiter)
	local out = {}
	for value in (row .. delimiter):gmatch("(.-)" .. delimiter) do out[#out+1] = value end
	return out
end
local function integer(v, hi)
	v = number(v)
	if v and v >= 0 and v <= hi and v == math.floor(v) then return v end
end
function CM.previewEncode3d(body, details, invalid)
	local rows = {}
	for i, d in ipairs(details) do
		if not resourceName(d.file) or not integer(d.terrain, 2) or
			not integer(d.bus, 1) or not integer(d.tram, 2) or not integer(d.cat, 1) or
			(d.terrain > 0 and not resourceName(d.structure)) then return nil end
		local row = {}
		for j=1,4 do if not number(d[j]) then return nil end; row[j] = string.format("%.3f", d[j]) end
		row[5], row[6] = tostring(d.terrain), token(d.file)
		row[7], row[8], row[9] = tostring(d.bus), tostring(d.tram), tostring(d.cat)
		row[10] = d.terrain == 0 and "-" or token(d.structure)
		rows[i] = table.concat(row, ",")
	end
	local prefix = type(invalid) == "boolean" and (invalid and "4|1|" or "4|0|") or "3|"
	local result = prefix .. body .. "|" .. table.concat(rows, ";")
	if #result <= K.PREVIEW_MAX_BYTES then return result end
end
function CM.previewDecode3d(body)
	local status
	if body:sub(1,2) == "4|" then
		local rest
		status, rest = body:match("^4|([01])|(.+)$")
		if not rest then return nil end
		body = "3|" .. rest
	end
	local xyBody, extra = body:match("^3|([^|]+)|([^|]+)$")
	if not xyBody then return nil end
	local p = CM.previewDecode(xyBody)
	if not p or p.off then return nil end
	if status then p.invalid = status == "1" end
	local rows = fields(extra, ";")
	if #rows ~= #p.curves then return nil end
	p.details = {}
	for i, row in ipairs(rows) do
		local f, d = fields(row, ","), {}
		if #f ~= 10 then return nil end
		for j=1,4 do d[j] = number(f[j]); if not d[j] then return nil end end
		d.terrain, d.file = integer(f[5], 2), untoken(f[6])
		d.bus, d.tram, d.cat = integer(f[7], 1), integer(f[8], 2), integer(f[9], 1)
		if not d.terrain or not d.file or not d.bus or not d.tram or not d.cat then return nil end
		if d.terrain == 0 then if f[10] ~= "-" then return nil end
		else d.structure = untoken(f[10]); if not d.structure then return nil end end
		p.details[i] = d
	end
	return p
end
local function xyz(v)
	if not v then return nil end
	local x,y,z
	pcall(function() x,y,z = number(v.x),number(v.y),number(v.z) end)
	if not x then pcall(function() x,y,z = number(v[1]),number(v[2]),number(v[3]) end) end
	if x and y and z then return {x,y,z} end
end
function CM.previewExtract3d(id, param)
	local rail = id == "trackBuilder"
	local sp, nodes, details = param.proposal.proposal, {}, {}
	for _, n in ipairs(sp.addedNodes) do nodes[n.entity] = xyz(n.comp.position) end
	local function pos(n)
		if nodes[n] then return nodes[n] end
		if type(n) ~= "number" or n < 0 or n ~= math.floor(n) or not api.engine.entityExists(n) then return nil end
		local comp = api.engine.getComponent(n, api.type.ComponentType.BASE_NODE)
		nodes[n] = comp and xyz(comp.position)
		return nodes[n]
	end
	local rep = rail and api.res.trackTypeRep or api.res.streetTypeRep
	for _, edge in ipairs(sp.addedSegments) do
		if edge.type == (rail and 1 or 0) and (not sp.new2oldSegments or sp.new2oldSegments[edge.entity] == nil) then
			local e = edge.comp
			local a,b,ta,tb = pos(e.node0),pos(e.node1),xyz(e.tangent0),xyz(e.tangent1)
			if not a or not b or not ta or not tb then return nil end
			local resource = rail and edge.trackEdge.trackType or edge.streetEdge.streetType
			if not integer(resource, 1000000) then return nil end
			local d = {a[3],b[3],ta[3],tb[3], terrain=e.type, file=rep.getName(resource),
				bus=not rail and edge.streetEdge.hasBus and 1 or 0,
				tram=not rail and edge.streetEdge.tramTrackType or 0,
				cat=rail and edge.trackEdge.catenary and 1 or 0}
			if e.type ~= 0 then
				if not integer(e.typeIndex, 1000000) then return nil end
				local structureRep = e.type == 1 and api.res.bridgeTypeRep or api.res.tunnelTypeRep
				d.structure = structureRep.getName(e.typeIndex)
			end
			details[#details+1] = d
		end
	end
	return details
end

local function nativePath(name) return K.BASE .. "tpf2mp_preview_native_" .. name .. ".txt" end
local function nativeSession()
	local ready = read(nativePath("ready"))
	if not ready or not ready:match("^%d+$") or #ready > 20 then return nil end
	if ready ~= CM.previewNativeSession then
		CM.previewNativeSession, CM.previewNativeDrawn = ready, {}
	end
	return ready
end
local function findResource(rep, name)
	if not rep or not resourceName(name) then return nil end
	local id = rep.find(name)
	if not integer(id, 1000000) then return nil end
	if rep.getName(id) == name then return id end
end
function CM.previewNativeProposal(p)
	if p.fence then return CM.fencesProposal(p) end
	if p.kind == "construction" then return CM.previewNativeConstruction(p) end
	local sp = api.type.SimpleProposal.new()
	local nodeMap, nextNode = {}, 0
	-- Resources repeat across segments. Resolve once per proposal; do not keep
	-- indices across scene/resource reloads or remember a missing resource.
	local resources = {}
	local function resource(rep2, name)
		if not rep2 or not resourceName(name) then return nil end
		local byName = resources[rep2]
		if not byName then byName = {}; resources[rep2] = byName end
		if byName[name] == nil then byName[name] = findResource(rep2, name) end
		return byName[name]
	end
	local function node(x,y,z)
		local key = string.format("%.3f,%.3f,%.3f",x,y,z)
		if nodeMap[key] then return nodeMap[key] end
		nextNode = nextNode+1
		local n = api.type.NodeAndEntity.new()
		n.entity = -nextNode
		n.comp.position = api.type.Vec3f.new(x,y,z)
		sp.streetProposal.nodesToAdd[nextNode] = n
		nodeMap[key] = n.entity
		return n.entity
	end
	local rail = p.kind == "rail"
	local rep = rail and api.res.trackTypeRep or api.res.streetTypeRep
	local dummyStreet = rail and resource(api.res.streetTypeRep, "standard/town_small_new.lua")
	if rail and not dummyStreet then return nil end
	for i,c in ipairs(p.curves) do
		local d = p.details[i]
		local length2 = (c[3]-c[1])^2+(c[4]-c[2])^2+(d[2]-d[1])^2
		if length2 < 0.0001 or length2 > 100000000 or
			c[5]^2+c[6]^2+d[3]^2 < 0.0001 or c[7]^2+c[8]^2+d[4]^2 < 0.0001 then return nil end
		local resourceId = resource(rep, d.file)
		if not resourceId then return nil end
		local structure = -1
		if d.terrain > 0 then
			structure = resource(d.terrain == 1 and api.res.bridgeTypeRep or api.res.tunnelTypeRep, d.structure)
			if not structure then return nil end
		end
		local e = api.type.SegmentAndEntity.new()
		e.entity = -100-i
		e.comp.node0, e.comp.node1 = node(c[1],c[2],d[1]), node(c[3],c[4],d[2])
		e.comp.tangent0 = api.type.Vec3f.new(c[5],c[6],d[3])
		e.comp.tangent1 = api.type.Vec3f.new(c[7],c[8],d[4])
		e.comp.type, e.comp.typeIndex, e.type = d.terrain, structure, rail and 1 or 0
		e.streetEdge = api.type.BaseEdgeStreet.new()
		e.streetEdge.streetType = rail and dummyStreet or resourceId
		if rail then
			e.trackEdge = api.type.BaseEdgeTrack.new()
			e.trackEdge.trackType, e.trackEdge.catenary = resourceId, d.cat == 1
		else e.streetEdge.hasBus, e.streetEdge.tramTrackType = d.bus == 1, d.tram end
		sp.streetProposal.edgesToAdd[i] = e
	end
	return sp
end
local function nativeSend(o, mode, p, sessionId)
	if sessionId == nil then sessionId = nativeSession() end
	if not sessionId then return false end
	local ok, result = pcall(function()
		local sp = p and CM.previewNativeProposal(p) or api.type.SimpleProposal.new()
		if not sp then return false end
		CM.previewNativeNonce = (CM.previewNativeNonce or 0)+1
		local nonce = string.format("%.0f", CM.previewNativeNonce)
		if not write(nativePath("request"), sessionId .. " " .. nonce .. " " .. o .. " " .. mode) then return false end
		-- Conversion only. NEVER pass this command to api.cmd.sendCommand.
		api.cmd.make.buildProposal(sp, nil, false)
		write(nativePath("request"), "")
		return read(nativePath("ack")) == sessionId .. " " .. nonce .. " ok"
	end)
	if not ok then
		write(nativePath("request"), "")
		if not CM.previewNativeError then log("3D preview: " .. tostring(result)); CM.previewNativeError = true end
	end
	return ok and result == true
end
function CM.previewNativeUpdate(o, p, sig, sessionId)
	if not p.details then return false end
	if sessionId == nil then sessionId = nativeSession() end
	if not sessionId then return false end
	local old, now = CM.previewNativeDrawn[o], os.clock()
	if old and old.sig == sig then
		if now-old.at < K.PREVIEW_KEEPALIVE_S then return true end
		if nativeSend(o, "keep", nil, sessionId) then old.at = now; return true end
	end
	local mode = p.invalid == true and "drawbad" or (p.invalid == false and "drawok" or "draw")
	if nativeSend(o, mode, p, sessionId) then CM.previewNativeDrawn[o] = {sig=sig,at=now}; return true end
	return false
end
function CM.previewNativeFinish(wanted, sessionId)
	for o in pairs(CM.previewNativeDrawn or {}) do
		if not wanted[o] then nativeSend(o, "clear", nil, sessionId); CM.previewNativeDrawn[o] = nil end
	end
end

function CM.previewGuiTick()
	if not K.INSTANCE or not K.BASE then return end
	local now, clk = os.time(), os.clock()
	if CM.previewGuiAt and clk - CM.previewGuiAt < K.PREVIEW_INTERVAL_S then return end
	CM.previewGuiAt = clk
	if CM.previewGuiMe ~= K.INSTANCE then
		snapshots = {}
		CM.previewGuiDecoded = nil
		CM.previewGuiMe, CM.previewLocal = K.INSTANCE, nil
		CM.previewGuiStarted = now
	end
	if CM.previewLocal and clk - (CM.previewEventAt or 0) > 0.3 then
		local ok, active = pcall(CM.previewControlsVisible)
		if (ok and not active) or (not ok and clk - CM.previewEventAt > K.PREVIEW_STALE_S) then
			CM.previewLocal, CM.previewLocalTool = nil, nil
		end
	end
	-- Events replace the copied proposal table; never retain native event userdata.
	if CM.previewLocal ~= encodedLocal then
		encodedLocal = CM.previewLocal
		encodedBody = CM.previewEncode(encodedLocal) or "off"
	end
	publish(path(K.INSTANCE, false), encodedBody, now, clk)
	local incoming = read(path(K.INSTANCE, true))
	local wall = incoming and tonumber(incoming:match("^(%d+)\n"))
	local wanted, nativeWanted = {}, {}
	local decoded, previous = {}, CM.previewGuiDecoded or {}
	local sessionId
	local function sessionOnce()
		if sessionId == nil then sessionId = nativeSession() or false end
		return sessionId
	end
	if wall and wall >= CM.previewGuiStarted and now-wall <= K.PREVIEW_STALE_S and wall <= now+1 then
		for line in incoming:gmatch("[^\n]+") do
			local o, r, g, b, body = line:match("^([a-z]+) ([%d%.]+) ([%d%.]+) ([%d%.]+) (.+)$")
			if o and #o <= 2 and o ~= K.INSTANCE then
				local old = previous[o]
				local p = old and old.body == body and old.p or CM.previewDecode(body)
				if p then decoded[o] = {body=body, p=p} end
				if p and not p.off then
					local native = p.details and CM.previewNativeUpdate(o, p, line, sessionOnce())
					if native then nativeWanted[o] = true end
					for i, c in ipairs(native and {} or p.curves) do
						local key = "mppreview_" .. o .. "_" .. i
						wanted[key] = { curve = c, width = p.kind == "road" and 3 or 1.5, color = {tonumber(r), tonumber(g), tonumber(b), 0.8}, sig = line }
					end
				end
			end
		end
	end
	CM.previewGuiDecoded = decoded
	-- The Fences planner publishes cosmetic model geometry locally. It must
	-- never create the mod's temporary world constructions for a preview.
	if CM.fencesPreviewRead then
		local ok,p,sig=pcall(CM.fencesPreviewRead)
		local visible,controls=pcall(CM.previewControlsVisible)
		if ok and p and visible and controls then
			if CM.previewNativeUpdate(K.INSTANCE,p,sig,sessionOnce()) then
				nativeWanted[K.INSTANCE]=true
			else
				local models=p.params and p.params.result and p.params.result.models
				local a=models and models[1] and models[1].transf
				local b=models and models[#models] and models[#models].transf
				if a and b then
					local x,y,u,v=p.x+a[13],p.y+a[14],p.x+b[13],p.y+b[14]
					wanted.mpfences={curve={x,y,u,v,u-x,v-y,u-x,v-y},width=0.5,color={0.3,0.7,1,0.8},sig=sig}
				end
			end
		end
	end
	if next(CM.previewNativeDrawn or {}) then CM.previewNativeFinish(nativeWanted, sessionOnce()) end
	CM.previewDrawn = CM.previewDrawn or {}
	for key in pairs(CM.previewDrawn) do
		if not wanted[key] then game.interface.setZone(key, nil); CM.previewDrawn[key] = nil end
	end
	for key, p in pairs(wanted) do
		if CM.previewDrawn[key] ~= p.sig then
			local polygon = CM.previewPolygon(p.curve, p.width)
			if polygon then
				game.interface.setZone(key, { polygon = polygon, draw = true, drawColor = p.color })
				CM.previewDrawn[key] = p.sig
			elseif CM.previewDrawn[key] then
				game.interface.setZone(key, nil)
				CM.previewDrawn[key] = nil
			end
		end
	end
end
require("mp/preview_constructions")(CM, K)
end
