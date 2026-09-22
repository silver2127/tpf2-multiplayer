local CM, K
for l = 2, 14 do
  local d = debug.getinfo(l, "f")
  if d then
    for i = 1, 250 do
      local k, v = debug.getupvalue(d.func, i)
      if not k then break end
      if k == "CM" and not CM then CM = v elseif k == "K" and not K then K = v end
    end
  end
end
if not CM or not K then return "CM/K not found" end
if CM.hjOrigStamp then CM.hashStampOf = CM.hjOrigStamp end
local orig = CM.hashStampOf
CM.hjOrigStamp = orig
CM.hjLastUnit = nil
local function j(t) if type(t) ~= "table" then return tostring(t) end local r = {} for i = 1, #t do r[#r + 1] = tostring(t[i]) end return table.concat(r, ",") end
local function dump(now)
  local t = game.interface.getEntities({ radius = 999999 }, { type = "SIM_PERSON", includeData = false }) or {}
  local rows, order = {}, {}
  for i = 1, #t do
    local id = t[i]
    order[#order + 1] = tostring(id)
    local e = game.interface.getEntity(id)
    if e then
      rows[#rows + 1] = string.format("%d|%s|d=%s|t=%s|lm=%s|lnr=%s|mm=%s|sp=%.6f|tt=%s|rw=%s|rl=%s", id, tostring(e.name), j(e.destinations), tostring(e.targetOrAtEntity), tostring(e.lastMoveMode), tostring(e.lastNonResType), j(e.moveModes), e.speed or -1, j(e.travelTimes), j(e.landUse2ReachableWalkDrive), j(e.landUse2ReachableLines))
    end
  end
  table.sort(rows)
  local f = io.open(K.BASE .. string.format("hjprobe_%s_%.1f.txt", K.INSTANCE, now), "w")
  f:write("ORDER " .. table.concat(order, ",") .. "\n")
  f:write(table.concat(rows, "\n"))
  f:write("\n")
  f:close()
  -- vehicles and sim cargo: every field, sorted keys, two levels deep
  local function ser(v, depth)
    if type(v) == "number" then return string.format("%.4f", v) end
    if type(v) ~= "table" then return tostring(v) end
    if depth <= 0 then return "{}" end
    local ks = {}
    for k in pairs(v) do ks[#ks + 1] = k end
    table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(ks) do parts[#parts + 1] = tostring(k) .. "=" .. ser(v[k], depth - 1) end
    return "{" .. table.concat(parts, ";") .. "}"
  end
  local vrows, vorder = {}, {}
  for _, kind in ipairs({ "VEHICLE", "SIM_CARGO" }) do
    local ids = game.interface.getEntities({ radius = 999999 }, { type = kind, includeData = false }) or {}
    for i = 1, #ids do
      vorder[#vorder + 1] = tostring(ids[i])
      local e = game.interface.getEntity(ids[i])
      if e then vrows[#vrows + 1] = string.format("%d|%s|%s", ids[i], kind, ser(e, 3)) end
    end
  end
  table.sort(vrows)
  local g = io.open(K.BASE .. string.format("hjveh_%s_%.1f.txt", K.INSTANCE, now), "w")
  g:write("ORDER " .. table.concat(vorder, ",") .. "\n")
  g:write(table.concat(vrows, "\n"))
  g:write("\n")
  g:close()
end
CM.hashStampOf = function(now)
  local u = math.floor(now)
  local every = CM.hjEvery or 5
  if now - u < 0.1 and u ~= CM.hjLastUnit and u % every == 0 then
    CM.hjLastUnit = u
    local ok, err = pcall(dump, now)
    if not ok then CM.hjErr = tostring(err) end
  end
  return orig(now)
end
return "probe installed on " .. tostring(K.INSTANCE)
