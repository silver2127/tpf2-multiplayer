-- Cosmetic construction proposals: bounded data codec, never Lua source or IDs.
return function(CM, K)
local function finite(n, limit)
	return type(n) == "number" and n == n and math.abs(n) <= limit
end
local function fileOK(s)
	return type(s) == "string" and #s <= 180 and s:match("^[%w_./%-]+%.con$") and
		s:sub(1,1) ~= "/" and not s:find("..",1,true)
end
local function transform(t)
	if type(t) ~= "table" or #t ~= 16 then return nil end
	for i=1,16 do if not finite(t[i], i >= 13 and i <= 15 and 1000000 or 100) then return nil end end
	if math.abs(t[4])+math.abs(t[8])+math.abs(t[12])+math.abs(t[16]-1) > 0.001 then return nil end
	local det = t[1]*(t[6]*t[11]-t[7]*t[10])-t[5]*(t[2]*t[11]-t[3]*t[10])+t[9]*(t[2]*t[7]-t[3]*t[6])
	return math.abs(det) > 0.000001 and math.abs(det) <= 1000
end
-- Length-prefixed typed values. Decode does not call load/eval. Limits apply
-- during traversal, before allocating a wire body (including cyclic input).
local function encode(value)
	local seen, count, bytes = {}, 0, 0
	local function put(s) bytes=bytes+#s; assert(bytes<=K.PREVIEW_MAX_BYTES); return s end
	local function visit(v, depth)
		count=count+1; assert(count<=512 and depth<=8)
		local typ=type(v)
		if typ=="number" then assert(finite(v, 2^53-1)); return put("n"..string.format("%.17g",v)..":") end
		if typ=="boolean" then return put(v and "t" or "f") end
		if typ=="string" then
			assert(#v<=512)
			local hex=v:gsub(".",function(c) return string.format("%02x",c:byte()) end)
			return put("s"..#hex..":"..hex)
		end
		assert((typ=="table" or typ=="userdata") and not seen[v]); seen[v]=true
		local keys={}
		for k in pairs(v) do assert(type(k)=="string" or type(k)=="number"); keys[#keys+1]=k; assert(#keys<=128) end
		table.sort(keys,function(a,b) if type(a)~=type(b) then return type(a)<type(b) end; return a<b end)
		local parts={put("m"..#keys..":")}
		for _,k in ipairs(keys) do parts[#parts+1]=visit(k,depth+1); parts[#parts+1]=visit(v[k],depth+1) end
		seen[v]=nil; return table.concat(parts)
	end
	local ok, result=pcall(visit,value,0)
	return ok and result or nil
end
local function decode(s)
	if type(s)~="string" or #s>K.PREVIEW_MAX_BYTES then return nil end
	local at,count=1,0
	local function visit(depth)
		count=count+1; assert(count<=512 and depth<=8)
		local tag=s:sub(at,at); at=at+1
		if tag=="t" then return true elseif tag=="f" then return false end
		local finish=s:find(":",at,true); assert(finish and finish-at<=32)
		local raw=s:sub(at,finish-1); local n=tonumber(raw); at=finish+1
		assert(finite(n,2^53-1))
		if tag=="n" then return n end
		assert(n>=0 and n==math.floor(n))
		if tag=="s" then
			assert(n<=1024 and n%2==0 and at+n-1<=#s)
			local hex=s:sub(at,at+n-1); at=at+n; assert(not hex:find("[^%da-f]"))
			return (hex:gsub("..",function(h) return string.char(tonumber(h,16)) end))
		end
		assert(tag=="m" and n<=128)
		local t={}
		for _=1,n do
			local k=visit(depth+1); assert(type(k)=="number" or type(k)=="string"); assert(t[k]==nil)
			t[k]=visit(depth+1)
		end
		return t
	end
	local ok,v=pcall(visit,0)
	if ok and at==#s+1 then return v end
end
local function marker(t)
	-- A small oriented placement marker, NOT an asserted building footprint.
	local points, curves={},{}
	for _,p in ipairs({{-5,-5},{5,-5},{5,5},{-5,5}}) do
		points[#points+1]={t[13]+p[1]*t[1]+p[2]*t[5],t[14]+p[1]*t[2]+p[2]*t[6]}
	end
	for i,a in ipairs(points) do local b=points[i%4+1]; curves[i]={a[1],a[2],b[1],b[2],b[1]-a[1],b[2]-a[2],b[1]-a[1],b[2]-a[2]} end
	return curves
end
function CM.previewExtractConstruction(param)
	local proposal=param and param.proposal
	if not proposal or not proposal.toAdd or #proposal.toAdd~=1 then return nil end
	-- Edits to existing modular stations require a separate replacement path.
	if proposal.toRemove and #proposal.toRemove>0 then return nil end
	local c=proposal.toAdd[1]
	if not fileOK(c.fileName) then return nil end
	local t={}; for i=1,16 do t[i]=c.transf[i] end
	if not transform(t) then return nil end
	local params=encode(c.params)
	if not params then return nil end
	return {kind="construction",file=c.fileName,transf=t,params=decode(params),curves=marker(t),details=true}
end
function CM.previewEncodeConstruction(p)
	if not fileOK(p.file) or not transform(p.transf) or type(p.params)~="table" then return nil end
	local params=encode(p.params); if not params then return nil end
	local t={}; for i=1,16 do t[i]=string.format("%.9g",p.transf[i]) end
	local status=p.invalid==true and "1" or (p.invalid==false and "0" or "u")
	local body="5|"..status.."|"..p.file.."|"..table.concat(t,",").."|"..params
	if #body<=K.PREVIEW_MAX_BYTES then return body end
end
function CM.previewDecodeConstruction(body)
	local status,file,raw,encoded=body:match("^5|([01u])|([^|]+)|([^|]+)|([^|]+)$")
	if not fileOK(file) then return nil end
	local t={}; for n in (raw..","):gmatch("(.-),") do local v=tonumber(n); if not v then return nil end; t[#t+1]=v end
	if not transform(t) then return nil end
	local params=decode(encoded); if type(params)~="table" then return nil end
	local p={kind="construction",file=file,transf=t,params=params,curves=marker(t),details=true}
	if status~="u" then p.invalid=status=="1" end
	return p
end
function CM.previewNativeConstruction(p)
	if not fileOK(p.file) or not transform(p.transf) or type(p.params)~="table" then return nil end
	local rep=api.res.constructionRep
	local index=rep.find(p.file)
	if not finite(index,1000000) or index<0 or rep.getName(index)~=p.file then return nil end
	local sp=api.type.SimpleProposal.new()
	local c=api.type.SimpleProposal.ConstructionEntity.new()
	c.fileName=p.file; c.params=decode(encode(p.params) or "")
	if not c.params then return nil end
	local t=p.transf
	c.transf=api.type.Mat4f.new(api.type.Vec4f.new(t[1],t[2],t[3],t[4]),api.type.Vec4f.new(t[5],t[6],t[7],t[8]),
		api.type.Vec4f.new(t[9],t[10],t[11],t[12]),api.type.Vec4f.new(t[13],t[14],t[15],t[16]))
	c.playerEntity=api.engine.util.getPlayer(); c.name="Multiplayer preview"
	sp.constructionsToAdd[1]=c
	return sp
end
end
