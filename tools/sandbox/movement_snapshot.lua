-- Diagnostic only: prepare/review before existing EVAL; never installed mod Lua.
local targets = @TARGETS@
local probeId = @PROBE_ID@
local prefix = @PREFIX@
local gi = game.interface
local original = gi.getGameTime
local initial = original()
assert(initial and initial.time and initial.time < targets[1],
  'MOVEMENT_PROBE first target already passed; no wrapper installed')
if debug and debug.getupvalue then
  local names = {}
  for i = 1, 30 do
    local name = debug.getupvalue(original, i)
    if not name then break end
    names[name] = true
  end
  assert(not (names.original and names.snapshot and (names.target or names.targets)),
    'MOVEMENT_PROBE another diagnostic is armed')
end
local ct = api.type.ComponentType
local components = {}
for _, name in ipairs({'SIM_PERSON', 'SIM_ENTITY_AT_BUILDING', 'SIM_ENTITY_MOVING', 'MOVE_PATH', 'MODEL_PERSON', 'MODEL_INSTANCE_LIST'}) do
  local ok, value = pcall(function() return ct[name] end)
  assert(ok and value ~= nil, 'MOVEMENT_PROBE unsupported component: ' .. name)
  components[name] = value
end
local function quote(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
    if c == '\\' then return '\\\\' elseif c == '"' then return '\\"' end
    return string.format('\\u%04x', string.byte(c))
  end) .. '"'
end
local function json(v, depth)
  local kind = type(v)
  if kind == 'number' then
    assert(v == v and v ~= math.huge and v ~= -math.huge, 'MOVEMENT_PROBE nonfinite JSON value')
    return string.format('%.17g', v)
  elseif kind == 'boolean' then return tostring(v)
  elseif kind == 'nil' then return 'null'
  elseif kind == 'string' then return quote(v)
  elseif kind == 'table' then
    assert(depth <= 16, 'MOVEMENT_PROBE unexpected payload nesting')
    local parts = {}
    for key, value in pairs(v) do
      assert(type(key) == 'string' or type(key) == 'number', 'MOVEMENT_PROBE unsupported key')
      parts[#parts + 1] = quote(tostring(key)) .. ':' .. json(value, depth + 1)
    end
    table.sort(parts)
    return '{' .. table.concat(parts, ',') .. '}'
  end
  error('MOVEMENT_PROBE unsupported JSON type: ' .. kind)
end
local function snapshot(target, now)
  local errors = {simulation = 0, geometry = 0, samples = {}}
  local function failure(group, where, message)
    errors[group] = errors[group] + 1
    if #errors.samples < 24 then
      errors.samples[#errors.samples + 1] = {group = group, where = where, message = tostring(message)}
    end
  end
  local function field(value, key, group, where, optional)
    local ok, result = pcall(function() return value[key] end)
    if not ok or (result == nil and not optional) then
      failure(group, where .. '.' .. tostring(key), ok and 'missing required field' or result)
      return nil
    end
    return result
  end
  local function scalar(value, group, where)
    local kind = type(value)
    if kind == 'number' then
      if value == value and value ~= math.huge and value ~= -math.huge then return value end
    elseif kind == 'boolean' or kind == 'string' then return value end
    failure(group, where, 'expected finite scalar, got ' .. kind)
    return nil
  end
  local function fields(value, names, group, where)
    local result = {}
    for _, name in ipairs(names) do
      local item = field(value, name, group, where)
      if item ~= nil then result[name] = scalar(item, group, where .. '.' .. name) end
    end
    return result
  end
  local function sequence(value, group, where, encode, limit)
    local ok, size = pcall(function() return #value end)
    if not ok or type(size) ~= 'number' or size < 0 or size > limit or size ~= math.floor(size) then
      failure(group, where, 'invalid or oversized sequence length')
      return nil
    end
    local result = {}
    for i = 1, size do
      local item = field(value, i, group, where)
      if item ~= nil then result[i] = encode(item, where .. '[' .. i .. ']') end
    end
    return {count = size, values = result}
  end
  local function pathPos(value, where)
    return fields(value, {'edgeIndex', 'pos01', 'pos'}, 'simulation', where)
  end
  local function dyn(value, where)
    local result = fields(value, {'speed', 'brakeDecel', 'accel',
      'timeUntilAccel', 'timeStanding', 'timeToIgnore', 'approachingStation'}, 'simulation', where)
    -- Live build35924 exposes the previous state through MovePath.dyn0;
    -- speed0/pathPos0 inside DynState are nil and are not required fields.
    for _, name in ipairs({'pathPos'}) do
      local item = field(value, name, 'simulation', where)
      if item ~= nil then result[name] = pathPos(item, where .. '.' .. name) end
    end
    return result
  end
  local function path(value, where)
    local result = fields(value, {'endOffset', 'terminalDecisionOffset'}, 'simulation', where)
    local edges = field(value, 'edges', 'simulation', where)
    if edges ~= nil then
      result.edges = sequence(edges, 'simulation', where .. '.edges', function(edge, at)
        local row = fields(edge, {'dir'}, 'simulation', at)
        local id = field(edge, 'edgeId', 'simulation', at)
        if id ~= nil then row.edgeId = fields(id, {'entity', 'index'}, 'simulation', at .. '.edgeId') end
        return row
      end, 16384)
    end
    return result
  end
  local function movePath(value, where)
    local result = fields(value, {'endParam', 'endPos', 'state', 'blocked',
      'allowEarlyArrival', 'reverse'}, 'simulation', where)
    local route = field(value, 'path', 'simulation', where)
    local current = field(value, 'dyn', 'simulation', where)
    local previous = field(value, 'dyn0', 'simulation', where, true)
    if route ~= nil then result.path = path(route, where .. '.path') end
    if current ~= nil then result.dyn = dyn(current, where .. '.dyn') end
    result.dyn0 = {present = previous ~= nil}
    if previous ~= nil then result.dyn0.value = dyn(previous, where .. '.dyn0') end
    return result
  end
  local function vector3(value, where)
    return fields(value, {'x', 'y', 'z'}, 'simulation', where)
  end
  local function extPath(value, where)
    local edges = field(value, 'edges', 'simulation', where)
    if edges == nil then return {} end
    return {edges = sequence(edges, 'simulation', where .. '.edges', function(edge, at)
      local range = field(edge, 'range', 'simulation', at, true)
      local link = field(edge, 'link', 'simulation', at, true)
      local result = {range = {present = range ~= nil}, link = {present = link ~= nil}}
      if (range == nil) == (link == nil) then
        failure('simulation', at, 'expected exactly one ExtPathEdge range/link variant')
      end
      if range ~= nil then
        local id = field(range, 'edgeId', 'simulation', at .. '.range')
        local params = field(range, 'params', 'simulation', at .. '.range')
        result.range.value = {}
        if id ~= nil then result.range.value.edgeId = fields(id, {'entity', 'index'}, 'simulation', at .. '.range.edgeId') end
        if params ~= nil then
          result.range.value.params = sequence(params, 'simulation', at .. '.range.params', function(item, itemAt)
            return scalar(item, 'simulation', itemAt)
          end, 2)
          if result.range.value.params and result.range.value.params.count ~= 2 then
            failure('simulation', at .. '.range.params', 'expected exactly two range parameters')
          end
        end
      end
      if link ~= nil then
        local points = field(link, 'points', 'simulation', at .. '.link')
        if points ~= nil then
          result.link.value = {points = sequence(points, 'simulation', at .. '.link.points', vector3, 2)}
          if result.link.value.points and result.link.value.points.count ~= 2 then
            failure('simulation', at .. '.link.points', 'expected exactly two link points')
          end
        end
      end
      return result
    end, 16384)}
  end
  local function walker(value, where)
    local result = fields(value, {'stopped', 'topSpeed', 'speed', 'brakeDecel',
      'offset', 'movementType', 'dist', 'stateTime'}, 'simulation', where)
    local route = field(value, 'path', 'simulation', where)
    local progress = field(value, 'pathPos', 'simulation', where)
    local position = field(value, 'position', 'simulation', where)
    local previous = field(value, 'dist0', 'simulation', where, true)
    if route ~= nil then result.path = extPath(route, where .. '.path') end
    if progress ~= nil then result.pathPos = pathPos(progress, where .. '.pathPos') end
    if position ~= nil then result.position = vector3(position, where .. '.position') end
    result.dist0 = {present = previous ~= nil}
    if previous ~= nil then result.dist0.value = scalar(previous, 'simulation', where .. '.dist0') end
    return result
  end
  local function get(id, name, group)
    local ok, value = pcall(api.engine.getComponent, id, components[name])
    if not ok then failure(group, tostring(id) .. '.' .. name, value); return nil end
    return value
  end
  local ids = {}
  api.engine.forEachEntityWithComponent(function(id) ids[#ids + 1] = id end, components.SIM_PERSON)
  assert(#ids <= 20000, 'MOVEMENT_PROBE person limit exceeded')
  table.sort(ids)
  local rows, movingCount, pathCount, walkerCount, transformCount = {}, 0, 0, 0, 0
  for _, id in ipairs(ids) do
    local at = tostring(id)
    local person = get(id, 'SIM_PERSON', 'simulation')
    local entityOk, entity = pcall(gi.getEntity, id)
    if not entityOk or type(entity) ~= 'table' then
      failure('simulation', at .. '.entity', entityOk and 'expected entity table' or entity)
      entity = {}
    end
    local building = get(id, 'SIM_ENTITY_AT_BUILDING', 'simulation')
    local moving = get(id, 'SIM_ENTITY_MOVING', 'simulation')
    local route = get(id, 'MOVE_PATH', 'simulation')
    local walk = get(id, 'MODEL_PERSON', 'simulation')
    local row = {kind = 'person', id = id, entity = entity, moving = moving ~= nil,
      simMoving = {present = moving ~= nil}, movePath = {present = route ~= nil},
      walker = {present = walk ~= nil}, atBuilding = {present = building ~= nil},
      worldTransforms = {present = false}}
    if building ~= nil then
      row.atBuilding.value = fields(building, {'waitingTime'}, 'simulation', at .. '.SIM_ENTITY_AT_BUILDING')
    end
    local updated = field(person, 'lastDestinationUpdate', 'simulation', at .. '.SIM_PERSON')
    if updated ~= nil then
      row.lastDestinationUpdate = scalar(updated, 'simulation', at .. '.lastDestinationUpdate')
    end
    if moving ~= nil then
      movingCount = movingCount + 1
      row.simMoving.value = fields(moving, {'line', 'lineStop0', 'lineStop1', 'partialPath'}, 'simulation', at .. '.SIM_ENTITY_MOVING')
      if route == nil and walk == nil then failure('simulation', at, 'moving person has neither MOVE_PATH nor MODEL_PERSON') end
    end
    if route ~= nil then pathCount = pathCount + 1; row.movePath.value = movePath(route, at .. '.MOVE_PATH') end
    if walk ~= nil then walkerCount = walkerCount + 1; row.walker.value = walker(walk, at .. '.MODEL_PERSON') end
    -- Model transforms are supplementary. Compare them separately from the
    -- authoritative simulation route/progress, since they can expose rendering
    -- or interpolation differences that do not change simulation decisions.
    if moving ~= nil then
      local models = get(id, 'MODEL_INSTANCE_LIST', 'geometry')
      if models == nil then failure('geometry', at, 'moving person has no MODEL_INSTANCE_LIST')
      else
        local instances = field(models, 'fatInstances', 'geometry', at .. '.MODEL_INSTANCE_LIST')
        if instances ~= nil then
          row.worldTransforms.present = true
          row.worldTransforms.instances = sequence(instances, 'geometry', at .. '.fatInstances', function(instance, where)
            local matrix = field(instance, 'transf', 'geometry', where)
            local values = {}
            if matrix ~= nil then
              for i = 1, 16 do
                local item = field(matrix, i, 'geometry', where .. '.transf')
                if item ~= nil then values[i] = scalar(item, 'geometry', where .. '.transf[' .. i .. ']') end
              end
            end
            return {transf = values}
          end, 32)
          if row.worldTransforms.instances and row.worldTransforms.instances.count > 0 then
            transformCount = transformCount + 1
          else failure('geometry', at, 'moving person has no fat instance transforms') end
        end
      end
    end
    rows[#rows + 1] = row
  end
  local countOk, globalCount = pcall(function() return api.engine.system.simPersonSystem.getCount() end)
  if not countOk or type(globalCount) ~= 'number' or globalCount < 0 or globalCount ~= math.floor(globalCount)
      or globalCount == math.huge then
    failure('simulation', 'globalCount', countOk and 'expected finite person count' or globalCount)
    globalCount = nil
  end
  local after = original()
  local meta = {kind = 'meta', schema = 4, request = probeId, target = target, time = now,
    timeAfter = after and after.time, entityCount = #ids, movingCount = movingCount,
    globalCount = globalCount,
    movePathCount = pathCount, walkerCount = walkerCount, transformCount = transformCount,
    simulationCaptureComplete = errors.simulation == 0,
    geometryCaptureComplete = errors.geometry == 0, errors = errors}
  local name = prefix .. '_' .. tostring(target) .. '.jsonl'
  local existing = io.open(name, 'r')
  if existing then existing:close(); error('MOVEMENT_PROBE output already exists: ' .. name) end
  local encoded = {json(meta, 0)}
  for _, row in ipairs(rows) do encoded[#encoded + 1] = json(row, 0) end
  local f = assert(io.open(name, 'w'))
  local ok, err = f:write(table.concat(encoded, string.char(10)), string.char(10))
  local closed, closeErr = f:close()
  assert(ok and closed, err or closeErr)
  print('MOVEMENT_PROBE snapshot request=' .. probeId .. ' target=' .. target ..
    ' time=' .. tostring(now) .. ' timeAfter=' .. tostring(meta.timeAfter) ..
    ' moving=' .. movingCount .. ' paths=' .. pathCount .. ' walkers=' .. walkerCount .. ' transforms=' .. transformCount ..
    ' errors=' .. errors.simulation .. '/' .. errors.geometry .. ' file=' .. name)
end
local unpackValues = table.unpack or unpack
local function packValues(...) return {n = select('#', ...), ...} end
local nextTarget, finished, lastTime = 1, false, initial.time
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
      finish(); print('MOVEMENT_PROBE abandoned request=' .. probeId .. ': world clock moved backwards')
    end
    lastTime = now.time
  end
  if not finished and now and now.time and now.time >= targets[nextTarget] then
    if gi.getGameTime ~= wrapper then
      finish(); print('MOVEMENT_PROBE abandoned: another clock wrapper replaced this diagnostic')
      return unpackValues(values, 1, values.n)
    end
    gi.getGameTime = original
    local ok, err = pcall(snapshot, targets[nextTarget], now.time)
    nextTarget = nextTarget + 1
    if not ok or nextTarget > #targets then
      finish()
      if not ok then print('MOVEMENT_PROBE error request=' .. probeId .. ' ' .. tostring(err)) end
    elseif gi.getGameTime == original then gi.getGameTime = wrapper
    else finish(); print('MOVEMENT_PROBE abandoned: clock function changed during snapshot') end
  end
  return unpackValues(values, 1, values.n)
end
gi.getGameTime = wrapper
return 'MOVEMENT_PROBE armed request=' .. probeId .. ' from=' .. tostring(initial.time) ..
  ' targets=' .. table.concat(targets, ',') .. '; preserves clock returns and self-restores'
