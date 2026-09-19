-- Snowball Fences uses real constructions for both its cursor and its segments.
-- Run its planner with build/bulldoze virtualized; only captured clicks commit.
local M = {}
local engineScript, CM, K, log
local function active()
	return CM and (CM.peerSeen or (tonumber(CM.rosterPlayers) or 0) > 1)
end
local function outputFile(file)
	return file == "asset/snowball_fences_fence_on.con"
		or file == "asset/snowball_fences_fence_off.con"
		or file == "asset/snowball_fences_fence_auto.con"
end
local function finite(v)
	return type(v) == "number" and v == v and math.abs(v) < 1000000
end
-- Whitelist the planner's model-only output. No removals, terrain or graph data.
local function modelsOnly(params)
	assert(type(params) == "table" and type(params.result) == "table", "missing fence result")
	for key in pairs(params.result) do assert(key == "models", "unsupported fence geometry") end
	local models = params.result.models
	assert(type(models) == "table" and #models > 0 and #models <= 512, "invalid fence size")
	local out = {}
	for i, model in ipairs(models) do
		assert(type(model.id) == "string" and model.id:match("%.mdl$") and not model.id:find("..",1,true), "invalid fence model")
		assert(#model.id <= 256 and type(model.transf) == "table" and #model.transf == 16, "invalid fence transform")
		local t = {}; for j=1,16 do assert(finite(model.transf[j]), "invalid fence coordinate"); t[j]=model.transf[j] end
		out[i] = {id=model.id, transf=t}
	end
	return {result={models=out}, seed=0}
end
local function path()
	return K and K.BASE and K.INSTANCE and (K.BASE .. "fences_preview_" .. K.INSTANCE .. ".txt")
end
local function preview(p)
	local file = path(); if not file then return end
	local f = io.open(file, "w")
	if f then f:write(tostring(os.time()), "\n", p and CM.ser(p) or "{}"); f:close() end
end
local function plan(script, name, param)
	local build, demolish = game.interface.buildConstruction, game.interface.bulldoze
	local random, randomseed = math.random, math.randomseed
	local seed=1
	-- Decorative bushes pick random models/rotations. Planning is local and
	-- preview frequency must never consume the simulation's shared RNG.
	math.random=function(a,b)
		seed=(seed*16807)%2147483647
		local r=(seed-1)/2147483646
		if not a then return r end
		if not b then a,b=1,a end
		return a+math.floor(r*(b-a+1))
	end
	math.randomseed=function() seed=1 end
	local result
	game.interface.buildConstruction = function(file, params, t)
		assert(not result and outputFile(file), "unexpected Fences build")
		for i=1,16 do assert(t[i] == ((i==1 or i==6 or i==11 or i==16) and 1 or 0), "unexpected Fences transform") end
		local clean = modelsOnly(params)
		-- Move the construction origin onto its geometry, instead of putting
		-- every segment at world (0,0), which defeats selection/key lookup.
		local x,y,z=0,0,0
		for _,m in ipairs(clean.result.models) do x=x+m.transf[13]; y=y+m.transf[14]; z=z+m.transf[15] end
		local n=#clean.result.models
		-- Numeric wire fields have four decimal places. Anchor identically on
		-- the origin and peers before subtracting it from the model transforms.
		x,y,z=tonumber(string.format("%.4f",x/n)),tonumber(string.format("%.4f",y/n)),tonumber(string.format("%.4f",z/n))
		for _,m in ipairs(clean.result.models) do m.transf[13]=m.transf[13]-x; m.transf[14]=m.transf[14]-y; m.transf[15]=m.transf[15]-z end
		result={file=file,params=clean,x=x,y=y,z=z}
		-- No entity is created, so the mod never retains a preview entity id.
		return nil
	end
	game.interface.bulldoze = function() end
	local ok, err = pcall(script.handleEvent, "snowball_fences_callback.lua", "__fencesEvent__", name, param)
	game.interface.buildConstruction, game.interface.bulldoze = build, demolish
	math.random, math.randomseed = random, randomseed
	if not ok then error(err) end
	return result
end
function M.wrap(script)
	local out={}; for k,v in pairs(script) do out[k]=v end
	local previewAt
	for _,key in ipairs({"init","load","update"}) do
		local fn=script[key]
		out[key]=function(...) engineScript=script; if fn then return fn(...) end end
	end
	out.handleEvent=function(src,id,name,param)
		engineScript=script
		if not active() then return script.handleEvent(src,id,name,param) end
		if src ~= "snowball_fences_callback.lua" or id ~= "__fencesEvent__" then return script.handleEvent(src,id,name,param) end
		-- Cancelled clicks are consumed from CONXP, never from a second GUI
		-- notification. Read pending clicks before a preview/tool-finish event.
		if CM.pollInject then CM.pollInject() end
		if CM.actionsOff or CM.resyncHold then preview(nil); return end
		if name == "builder.proposalCreate" or name == "finish" then
			if name == "builder.proposalCreate" then
				local now=os.clock()
				if previewAt and now-previewAt<0.1 then return end
				previewAt=now
			else previewAt=nil end
			local ok,p=pcall(plan,script,name,param)
			preview(ok and p or nil)
			if not ok then log("Fences preview: "..tostring(p)) end
		end
	end
	return out
end
function M.bind(cm,k,logger)
	CM,K,log=cm,k,logger
	function CM.fencesCapture(file,t,pstr,hadRoadc)
		if not engineScript or hadRoadc ~= 0 or not file:match("^asset/snowball_fence_") then return false end
		if CM.actionsOff or CM.resyncHold then preview(nil); return true end
		local params=assert(CM.deserParams(pstr), "invalid Fences placement params")
		local send=game.interface.sendScriptEvent
		local event
		game.interface.sendScriptEvent=function(id,name,param)
			assert(id=="__fencesEvent__" and not event, "unexpected Fences event")
			event={name=name,param=param}
		end
		local ok,err=pcall(engineScript.guiHandleEvent,"constructionBuilder","builder.apply",{
			proposal={toAdd={{fileName=file,transf=t,params=params}}},result={}})
		game.interface.sendScriptEvent=send
		if not ok then error(err) end
		if not event then return false end
		local p=plan(engineScript,event.name,event.param)
		preview(nil)
		if p then
			CM.scheduleLocal("FENCE",{file=p.file,x=p.x,y=p.y,z=p.z,params=CM.ser(p.params)})
			log("Fences: scheduled synchronized segment")
		else log("Fences: endpoint/finish accepted without building a cursor object") end
		return true
	end
	function CM.fencesProposal(p)
		assert(outputFile(p.file) and finite(p.x) and finite(p.y) and finite(p.z), "invalid fence command")
		local sp=api.type.SimpleProposal.new()
		local ce=api.type.SimpleProposal.ConstructionEntity.new()
		ce.fileName=p.file; ce.params=modelsOnly(p.params)
		ce.transf=api.type.Mat4f.new(api.type.Vec4f.new(1,0,0,0),api.type.Vec4f.new(0,1,0,0),api.type.Vec4f.new(0,0,1,0),api.type.Vec4f.new(p.x,p.y,p.z,1))
		ce.playerEntity=api.engine.util.getPlayer(); ce.name="Fence"
		sp.constructionsToAdd[1]=ce
		return sp
	end
	function CM.execFence(c)
		local p={file=c.file,x=tonumber(c.x),y=tonumber(c.y),z=tonumber(c.z),params=CM.deserParams(c.params)}
		local sp=CM.fencesProposal(p)
		local ctx=api.type.Context.new()
		ctx.checkTerrainAlignment=false
		ctx.cleanupStreetGraph=false
		ctx.gatherBuildings=false
		ctx.gatherFields=false
		ctx.player=api.engine.util.getPlayer()
		if CM.cmRoadPlayer then CM.cmRoadPlayer(c,ctx) end
		sp.constructionsToAdd[1].playerEntity=ctx.player
		-- An additive proposal only: no clearing buildings or replacing an
		-- existing construction at the same coordinate, even on repeated clicks.
		api.cmd.sendCommand(api.cmd.make.buildProposal(sp,ctx,false),function(res,success)
			local id=res and res.resultEntities and res.resultEntities[1]
			if success and type(id)=="number" and id>=0 and api.engine.entityExists(id) then
				CM.registerFenceReplay(id,c.file,c.params)
			end
			log(string.format("EXEC FENCE seq=%s origin=%s success=%s",tostring(c.seq),tostring(c.origin),tostring(success)))
		end)
	end
	function CM.fencesPreviewRead()
		local file=path(); if not file then return end
		local f=io.open(file,"r"); if not f then return end
		local s=f:read(262145); f:close()
		if not s or #s>262144 then return end
		local stamp,body=s:match("^(%d+)\n(.*)$")
		local age=stamp and os.time()-tonumber(stamp)
		if not age or age<0 or age>2 or tonumber(stamp)<(CM.previewGuiStarted or 0) then return end
		local p=CM.deserParams(body)
		if p and p.file then p.fence=true; p.details=true; return p,body end
	end
end
return M
