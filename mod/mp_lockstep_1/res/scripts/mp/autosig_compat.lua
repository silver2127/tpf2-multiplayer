-- AutoSig2's GUI build callback cannot follow a cancelled native placement.
-- Keep its route/spacing algorithm, but collect its proposal without applying it.
local M = {}
local engineScript
local function copy(t)
	local out = {}; for k, v in pairs(t or {}) do out[k] = v end; return out
end

function M.wrap(script)
	local out = copy(script)
	-- Game scripts in the engine share require's cache; GUI has a separate VM.
	for _, key in ipairs({"init", "load", "update", "handleEvent"}) do
		local fn = script[key]
		out[key] = function(...)
			engineScript = script
			if fn then return fn(...) end
		end
	end
	if script.guiHandleEvent then
		out.guiHandleEvent = function(id, name, param)
			if id == "streetTerminalBuilder" and name == "builder.apply" then
				local p = param and param.proposal and param.proposal.proposal
				local obj = p and p.edgeObjectsToAdd and p.edgeObjectsToAdd[1]
				-- The cancelled tool has no resulting entity. Never pass a nil id
				-- to getComponent (the original callback does so unconditionally).
				if not obj or not obj.resultEntity or obj.resultEntity <= 0 then return end
				if not api.engine.getComponent(obj.resultEntity, api.type.ComponentType.MODEL_INSTANCE_LIST) then return end
			end
			return script.guiHandleEvent(id, name, param)
		end
	end
	return out
end

function M.bind(CM, K, log)
	function CM.autoSigCapture(fields)
		if not engineScript or fields.track ~= 1 or fields.kind ~= 2 then return end
		local state = engineScript.save and engineScript.save()
		if state and state.use then
			local d = tonumber(state.distance)
			if d and d >= 10 and d <= 2000 then
				fields.autosig = d
				fields.autosigMode = state.remove and "remove" or (state.replace and "replace" or "add")
				fields.autosigBackward = state.backward and 1 or 0
			end
		end
	end

	-- Read the engine's signal positions, as AutoSig2 does. Model transforms are
	-- offset from the track and cannot pair a replacement with its old signal.
	function CM.autoSigSignalPositions(eid)
		assert(eid and eid > 0, "invalid AutoSig edge")
		local network = assert(api.engine.getComponent(eid, api.type.ComponentType.TRANSPORT_NETWORK))
		local result, length = {}, 0
		for i, section in ipairs(network.edges) do
			local start = length
			length = length + section.geometry.length
			for _, reverse in ipairs({false, true}) do
				local signal = api.engine.system.signalSystem.getSignal(api.type.EdgeId.new(eid, i - 1), reverse)
				local id = type(signal) == "number" and signal or signal.entity
				if id and id > 0 then result[id] = {pos = reverse and start or length, left = not reverse} end
			end
		end
		assert(length > 0, "empty AutoSig edge")
		for _, info in pairs(result) do info.u = info.pos / length end
		return result
	end

	function CM.autoSigAfterSeed(c, nodes, objects, left)
		if c.origin ~= K.INSTANCE or not c.autosig or not engineScript then return end
		local script = engineScript
		local before = copy(script.save())
		local requested = copy(before)
		local mode = c.autosigMode or "add"
		assert(mode == "add" or mode == "replace" or mode == "remove", "invalid AutoSig mode")
		requested.use, requested.replace, requested.remove = true, mode == "replace", mode == "remove"
		requested.backward = tonumber(c.autosigBackward) == 1
		requested.distance = tonumber(c.autosig)
		assert(requested.distance and requested.distance >= 10 and requested.distance <= 2000, "invalid AutoSig spacing")
		local make, send, get = api.cmd.make.buildProposal, api.cmd.sendCommand, api.engine.getComponent
		local pending, token = {}, {}
		-- The Workshop planner appends to comp.objects. Give it private vectors,
		-- so planning cannot modify the world or another script's component view.
		api.engine.getComponent = function(id, kind)
			assert(id and id > 0, "invalid entity in AutoSig planner")
			local comp = get(id, kind)
			if comp and kind == api.type.ComponentType.BASE_EDGE then
				local edge = {node0=comp.node0, node1=comp.node1, tangent0=comp.tangent0,
					tangent1=comp.tangent1, type=comp.type, typeIndex=comp.typeIndex, objects={}}
				for _, obj in ipairs(comp.objects) do edge.objects[#edge.objects+1] = {obj[1],obj[2]} end
				return edge
			end
			return comp
		end
		api.cmd.make.buildProposal = function(proposal)
			pending[#pending + 1] = proposal
			return token
		end
		api.cmd.sendCommand = function(cmd)
			assert(cmd == token, "unexpected command from AutoSig planner")
			-- Do not run the success/cost callback: nothing has been built yet.
		end
		local ok, err = pcall(function()
			script.load(requested)
			script.handleEvent("mp", "__autosig2__", "build", {
				nodes = nodes, edgeObjects = objects, left = left,
				oneWay = tonumber(c.oneWay) == 1, model = CM.unescName(c.model),
			})
		end)
		api.cmd.make.buildProposal, api.cmd.sendCommand = make, send
		api.engine.getComponent = get
		script.load(before)
		if not ok then error(err) end
		-- Validate/translate the entire batch before scheduling anything. Local
		-- edge and player ids stay here; ordinary STOPADD carries geometry only.
		local commands, seenTargets = {}, {}
		for _, proposal in ipairs(pending) do
			local sp = proposal.streetProposal
			assert(mode ~= "add" or not sp.edgeObjectsToRemove or #sp.edgeObjectsToRemove == 0, "AutoSig add attempted removal")
			assert(mode ~= "remove" or #sp.edgeObjectsToAdd == 0, "AutoSig remove attempted addition")
			local edges, sourceEdges, targetEdges, targets, additions = {}, {}, {}, {}, {}
			for i, edge in ipairs(sp.edgesToAdd) do
				local eid = assert(sp.edgesToRemove[i], "AutoSig source edge missing")
				assert(eid > 0 and edge.entity < 0, "invalid AutoSig edge mapping")
				local source = assert(get(eid, api.type.ComponentType.BASE_EDGE))
				assert(source.node0 == edge.comp.node0 and source.node1 == edge.comp.node1, "AutoSig changed track endpoints")
				edges[edge.entity], sourceEdges[edge.entity] = edge, eid
				for _, obj in ipairs(source.objects) do targetEdges[obj[1]] = eid end
			end
			for _, id in ipairs(sp.edgeObjectsToRemove or {}) do
				assert(not seenTargets[id], "duplicate AutoSig removal")
				seenTargets[id] = true
				local eid = assert(targetEdges[id], "AutoSig removal outside planned edges")
				local target = assert(CM.autoSigDescribeTarget(id, eid, c), "AutoSig target is not an editable rail signal")
				local info = assert(CM.autoSigSignalPositions(eid)[id], "AutoSig target position missing")
				targets[#targets+1] = {fields=target, eid=eid, u=info.u, left=info.left}
			end
			for _, obj in ipairs(sp.edgeObjectsToAdd) do
				local edge = assert(edges[obj.edgeEntity], "AutoSig edge missing")
				local be = edge.comp
				assert(be.node0 and be.node0 >= 0 and be.node1 and be.node1 >= 0, "AutoSig endpoint missing")
				local a = assert(api.engine.getComponent(be.node0, api.type.ComponentType.BASE_NODE)).position
				local b = assert(api.engine.getComponent(be.node1, api.type.ComponentType.BASE_NODE)).position
				local pa, pb = {a.x,a.y,a.z}, {b.x,b.y,b.z}
				local ta, tb = {be.tangent0.x,be.tangent0.y,be.tangent0.z}, {be.tangent1.x,be.tangent1.y,be.tangent1.z}
				local u = tonumber(obj.param)
				assert(u and u >= 0 and u <= 1, "invalid AutoSig position")
				local p = CM.hermitePos(pa,ta,pb,tb,u)
				local t = CM.hermiteTangent(pa,ta,pb,tb,u)
				local len = math.sqrt(t[1]^2+t[2]^2)
				assert(len > 0, "degenerate AutoSig tangent")
				local fields = {ax=a.x,ay=a.y,bx=b.x,by=b.y,x=p[1],y=p[2],
					tx=t[1]/len,ty=t[2]/len,eleft=obj.left and 1 or 0,oneWay=obj.oneWay and 1 or 0,
					kind=2,track=1,side=2,model=CM.escName(obj.model),name="",company=c.company,
					autosigFollow=1}
				if mode == "replace" then
					local matched
					for _, target in ipairs(targets) do
						if target.eid == sourceEdges[obj.edgeEntity] and math.abs(target.u-u) < 0.000001
							and target.left == obj.left then
							assert(not matched, "ambiguous AutoSig replacement")
							matched = target
						end
					end
					assert(matched and not matched.replacement, "AutoSig replacement has no unique target")
					matched.replacement = fields
				else additions[#additions+1] = {op="STOPADD", fields=fields} end
			end
			for _, target in ipairs(targets) do
				local fields = target.replacement or {x=target.fields.rx,y=target.fields.ry,company=c.company,autosigFollow=1}
				for key, value in pairs(target.fields) do fields[key] = value end
				commands[#commands+1] = {op=target.replacement and "STOPREP" or "STOPDEL",fields=fields}
			end
			for _, add in ipairs(additions) do commands[#commands+1] = add end
		end
		for _, command in ipairs(commands) do CM.scheduleLocal(command.op, command.fields) end
		log(string.format("AutoSig %s: scheduled %d synchronized signal action(s)", mode, #commands))
	end
end
return M
