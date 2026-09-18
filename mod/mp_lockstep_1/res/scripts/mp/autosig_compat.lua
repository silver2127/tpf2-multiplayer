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
		if state and state.use and not state.replace and not state.remove then
			local d = tonumber(state.distance)
			if d and d >= 10 and d <= 2000 then fields.autosig = d end
		end
	end

	function CM.autoSigAfterSeed(c, nodes, objects, left)
		if c.origin ~= K.INSTANCE or not c.autosig or not engineScript then return end
		local script = engineScript
		local before = copy(script.save())
		local requested = copy(before)
		requested.use, requested.replace, requested.remove = true, false, false
		requested.distance = tonumber(c.autosig)
		assert(requested.distance and requested.distance >= 10 and requested.distance <= 2000, "invalid AutoSig spacing")
		local make, send = api.cmd.make.buildProposal, api.cmd.sendCommand
		local pending, token = {}, {}
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
		script.load(before)
		if not ok then error(err) end
		-- Validate/translate the entire batch before scheduling anything. Local
		-- edge and player ids stay here; ordinary STOPADD carries geometry only.
		local commands = {}
		for _, proposal in ipairs(pending) do
			local sp = proposal.streetProposal
			assert(not sp.edgeObjectsToRemove or #sp.edgeObjectsToRemove == 0, "AutoSig planner attempted removal")
			local edges = {}
			for _, edge in ipairs(sp.edgesToAdd) do edges[edge.entity] = edge end
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
				commands[#commands+1] = {ax=a.x,ay=a.y,bx=b.x,by=b.y,x=p[1],y=p[2],
					tx=t[1]/len,ty=t[2]/len,eleft=obj.left and 1 or 0,oneWay=obj.oneWay and 1 or 0,
					kind=2,track=1,side=2,model=CM.escName(obj.model),name="",company=c.company,
					autosigFollow=1}
			end
		end
		for _, fields in ipairs(commands) do CM.scheduleLocal("STOPADD", fields) end
		log(string.format("AutoSig: scheduled %d synchronized signal(s)", #commands))
	end
end
return M
