-- Diagnostic only: loaded through existing EVAL, never installed as mod Lua.
local targets = @TARGETS@
local probeId = @PROBE_ID@
local prefix = @PREFIX@
local gi = game.interface
local original = gi.getGameTime
local initial = original()
assert(initial and initial.time and initial.time < targets[1],
  'PEOPLE_PROBE first target already passed; no wrapper installed')
if debug and debug.getupvalue then
  local names = {}
  for i = 1, 30 do
    local name = debug.getupvalue(original, i)
    if not name then break end
    names[name] = true
  end
  assert(not (names.original and names.snapshot and (names.target or names.targets)),
    'PEOPLE_PROBE another diagnostic is armed')
end
local ct = api.type.ComponentType
local stateTypes = {}
local stateNames = {}
local candidates = {
  'SIM_ENTITY_AT_BUILDING', 'SIM_ENTITY_AT_STOCK',
  'SIM_ENTITY_AT_TERMINAL', 'SIM_ENTITY_AT_VEHICLE',
  'SIM_ENTITY_IDLE', 'SIM_ENTITY_MOVING',
  'SIM_PERSON_AT_TERMINAL', 'SIM_PERSON_AT_VEHICLE'
}
-- ComponentType is a lookup proxy: pairs() exposes no enum names in build35924.
for _, name in ipairs(candidates) do
  local ok, value = pcall(function() return ct[name] end)
  if ok and value ~= nil then
    stateTypes[name] = value
    stateNames[#stateNames + 1] = name
  end
end
assert(#stateNames > 0, 'PEOPLE_PROBE no supported state component types')
local function quote(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
    if c == '\\' then return '\\\\' elseif c == '"' then return '\\"' end
    return string.format('\\u%04x', string.byte(c))
  end) .. '"'
end
local function json(v, depth)
  local kind = type(v)
  if kind == 'number' then
    if v ~= v or v == math.huge or v == -math.huge then return 'null' end
    return string.format('%.17g', v)
  elseif kind == 'boolean' then return tostring(v)
  elseif kind == 'nil' then return 'null'
  elseif kind == 'table' then
    assert(depth <= 8, 'PEOPLE_PROBE unexpected payload nesting')
    local parts = {}
    for key, value in pairs(v) do
      parts[#parts + 1] = quote(tostring(key)) .. ':' .. json(value, depth + 1)
    end
    table.sort(parts)
    return '{' .. table.concat(parts, ',') .. '}'
  else return quote(tostring(v)) end
end
local function snapshot(target, now)
  local ids = {}
  api.engine.forEachEntityWithComponent(function(id) ids[#ids + 1] = id end, ct.SIM_PERSON)
  table.sort(ids)
  local spatial = {}
  local spatialCount = 0
  for _, id in pairs(gi.getEntities({radius = 999999},
      {type = 'SIM_PERSON', includeData = false}) or {}) do
    if not spatial[id] then spatialCount = spatialCount + 1 end
    spatial[id] = true
  end
  local capacityOrder = {}
  api.engine.forEachEntityWithComponent(function(id)
    capacityOrder[#capacityOrder + 1] = id
  end, ct.PERSON_CAPACITY)
  local rows, stateCounts, stateSpatialCounts = {}, {}, {}
  for _, name in ipairs(stateNames) do stateCounts[name] = 0; stateSpatialCounts[name] = 0 end
  for _, id in ipairs(ids) do
    local person = api.engine.getComponent(id, ct.SIM_PERSON)
    local states = {}
    for _, name in ipairs(stateNames) do
      if api.engine.getComponent(id, stateTypes[name]) ~= nil then
        states[name] = true
        stateCounts[name] = stateCounts[name] + 1
        if spatial[id] then stateSpatialCounts[name] = stateSpatialCounts[name] + 1 end
      end
    end
    rows[#rows + 1] = {kind = 'person', id = id, spatial = spatial[id] or false,
      entity = gi.getEntity(id), lastDestinationUpdate = person and person.lastDestinationUpdate,
      states = states}
  end
  local after = original()
  local meta = {kind = 'meta', request = probeId, target = target, time = now,
    timeAfter = after and after.time, globalCount = api.engine.system.simPersonSystem.getCount(),
    entityCount = #ids, spatialCount = spatialCount, stateNames = stateNames,
    stateCounts = stateCounts, stateSpatialCounts = stateSpatialCounts,
    personCapacityOrder = capacityOrder}
  local path = prefix .. '_' .. tostring(target) .. '.jsonl'
  local existing = io.open(path, 'r')
  if existing then existing:close(); error('PEOPLE_PROBE output already exists: ' .. path) end
  local encoded = {json(meta, 0)}
  for _, row in ipairs(rows) do encoded[#encoded + 1] = json(row, 0) end
  local f = assert(io.open(path, 'w'))
  local ok, err = f:write(table.concat(encoded, string.char(10)), string.char(10))
  local closed, closeErr = f:close()
  assert(ok and closed, err or closeErr)
  print('PEOPLE_PROBE snapshot request=' .. probeId .. ' target=' .. target ..
    ' time=' .. tostring(now) .. ' timeAfter=' .. tostring(meta.timeAfter) ..
    ' global=' .. meta.globalCount .. ' spatial=' .. spatialCount .. ' file=' .. path)
end
local unpackValues = table.unpack or unpack
local function packValues(...) return {n = select('#', ...), ...} end
local nextTarget = 1
local finished = false
local lastTime = initial.time
local wrapper
local function finish()
  finished = true
  if gi.getGameTime == wrapper then gi.getGameTime = original end
end
wrapper = function(...)
  local values = packValues(original(...))
  local now = values[1]
  if not finished and now and now.time then
    if now.time < lastTime then
      finish()
      print('PEOPLE_PROBE abandoned request=' .. probeId .. ': world clock moved backwards')
    end
    lastTime = now.time
  end
  if not finished and now and now.time and now.time >= targets[nextTarget] then
    -- Restore before any diagnostic API calls. Never change the returned time.
    if gi.getGameTime ~= wrapper then
      finish()
      print('PEOPLE_PROBE abandoned: another clock wrapper replaced this diagnostic')
      return unpackValues(values, 1, values.n)
    end
    gi.getGameTime = original
    local ok, err = pcall(snapshot, targets[nextTarget], now.time)
    nextTarget = nextTarget + 1
    if not ok or nextTarget > #targets then
      finish()
      if not ok then print('PEOPLE_PROBE error request=' .. probeId .. ' ' .. tostring(err)) end
    elseif gi.getGameTime == original then gi.getGameTime = wrapper
    else finish(); print('PEOPLE_PROBE abandoned: clock function changed during snapshot') end
  end
  return unpackValues(values, 1, values.n)
end
gi.getGameTime = wrapper
return 'PEOPLE_PROBE armed request=' .. probeId .. ' from=' .. tostring(initial.time) ..
  ' targets=' .. table.concat(targets, ',') .. '; preserves clock returns and self-restores'
