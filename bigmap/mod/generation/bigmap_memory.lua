-- Terrain buffer reuse for the Lua terrain pipelines (Transport Fever 2 build 35924).
--
-- The native toolkit allocates one full-resolution float map per distinct
-- buffer name and keeps it until the pipeline ends, so the generation peak is
-- the number of distinct names. This pass renames temporary buffers so that
-- values whose lifetimes do not overlap share one name. It relies only on op
-- semantics verified in the native code (docs/generation-op-semantics.md):
--
--   full     the op writes every output element and the result never depends
--            on the prior output contents. Only such a write starts a new
--            value; every other write keeps mutating the current value.
--   inplace  the output may share storage with an input value that dies at
--            this op (pure elementwise ops, or inputs fully consumed before
--            the output is written).
--
-- Guards: an unknown layer or op schema leaves the whole pipeline untouched.
-- Only "__t_<n>" temporaries are renamed; every other name, and every name
-- referenced outside the name fields of the layers, keeps its own buffer.
-- A value first read before any full write keeps a buffer of its own (the
-- native allocation is fresh there). Op order, params, seeds and layer count
-- never change, and every intra-op alias of the stock pipeline is preserved.
local M = {}

local INPUT_KEYS = {"input", "input1", "input2"}
local NAME_KEYS = {input = true, input1 = true, input2 = true, output = true}

local ONE = {"input"}
local TWO = {"input1", "input2"}
local NONE = {}

-- layer.type .. ":" .. params.type -> semantics (see the doc for RVAs)
local OPS = {
    ["FEATURE:CONSTANT"] = {inputs = NONE, full = true},
    ["FEATURE:DATA"] = {inputs = NONE, full = true},
    ["FEATURE:NOISE"] = {inputs = NONE, full = true},
    ["FEATURE:RIDGED_NOISE"] = {inputs = NONE, full = true},
    ["FEATURE:GRADIENT_NOISE"] = {inputs = NONE, full = true},
    ["FEATURE:WHITE_NOISE"] = {inputs = NONE, full = true},
    ["FEATURE:DITHERING"] = {inputs = NONE, full = true},
    ["FEATURE:RIDGE"] = {inputs = NONE, full = true},
    ["FEATURE:POINTS"] = {inputs = NONE, full = false},
    ["FEATURE:RIVER"] = {inputs = NONE, full = false},
    ["OP:MAP"] = {inputs = ONE, full = true, inplace = true},
    ["OP:HERP"] = {inputs = ONE, full = true, inplace = true},
    ["OP:PWLERP"] = {inputs = ONE, full = true, inplace = true},
    ["OP:PWCONST"] = {inputs = ONE, full = true, inplace = true},
    ["OP:WHITE_NOISE"] = {inputs = ONE, full = true, inplace = true},
    ["OP:DISTANCE"] = {inputs = ONE, full = true, inplace = true},
    ["OP:MESA"] = {inputs = ONE, full = true, inplace = true},
    ["OP:GAUSS"] = {inputs = ONE, full = true},
    ["OP:GRADIENT"] = {inputs = ONE, full = true},
    ["OP:LAPLACE"] = {inputs = ONE, full = true},
    ["OP:AXPY"] = {inputs = ONE, full = false},
    ["MIX:MUL"] = {inputs = TWO, full = true, inplace = true},
    ["MIX:ADD"] = {inputs = TWO, full = true, inplace = true},
    ["MIX:COMP"] = {inputs = TWO, full = true, inplace = true},
    ["MIX:PERCOLATION"] = {inputs = TWO, full = true, inplace = true},
    ["MIX:MAD"] = {inputs = TWO, full = false},
    ["MIX:MASK"] = {inputs = TWO, full = false},
}

local function temporary(name)
    return type(name) == "string" and name:match("^__t_%d+$") ~= nil
end

local function specFor(layer)
    if type(layer) ~= "table" or type(layer.params) ~= "table" then return nil end
    local p = layer.params
    if type(layer.type) ~= "string" or type(p.type) ~= "string" then return nil end
    local spec = OPS[layer.type .. ":" .. p.type]
    if not spec then return nil end
    for key in pairs(NAME_KEYS) do
        if p[key] ~= nil and type(p[key]) ~= "string" then return nil end
    end
    if type(p.output) ~= "string" then return nil end
    local expected = {}
    for _, key in ipairs(spec.inputs) do
        if type(p[key]) ~= "string" then return nil end
        expected[key] = true
    end
    for _, key in ipairs(INPUT_KEYS) do
        if p[key] ~= nil and not expected[key] then return nil end
    end
    return spec
end

function M.Optimize(result)
    if type(result) ~= "table" or type(result.layers) ~= "table" then return result end
    local layers = result.layers
    local n = #layers
    local pinned = {}
    local function pin(value)
        if type(value) == "string" then pinned[value] = true
        elseif type(value) == "table" then
            for _, v in pairs(value) do pin(v) end
        end
    end
    -- Pin every name referenced outside the operation name fields: final
    -- height/forest/asset maps, mappings, colours and any future metadata.
    for k, v in pairs(result) do if k ~= "layers" then pin(v) end end
    for k, v in pairs(layers) do
        if type(k) ~= "number" or k < 1 or k > n or k % 1 ~= 0 then pin(v) end
    end
    local specs = {}
    for i = 1, n do
        local layer = layers[i]
        local spec = specFor(layer)
        if not spec then return result end -- unknown schema: leave it all alone
        specs[i] = spec
        for k, v in pairs(layer) do if k ~= "params" then pin(v) end end
        for k, v in pairs(layer.params) do if not NAME_KEYS[k] then pin(v) end end
    end
    if n == 0 then return result end

    -- Pass 1: split names into values at verified full writes.
    local before, allNames = 0, {}
    local current, values = {}, {}
    local reads, writes = {}, {}
    local function fixed(name) return pinned[name] or not temporary(name) end
    local function newValue(name, at, fresh)
        local v = {name = name, first = at, last = at, fresh = fresh, fixed = fixed(name)}
        values[#values + 1] = v
        return v
    end
    for i = 1, n do
        local p, spec = layers[i].params, specs[i]
        local r = {}
        for _, key in ipairs(spec.inputs) do
            local name = p[key]
            if not allNames[name] then allNames[name] = true; before = before + 1 end
            local v = current[name]
            if not v then v = newValue(name, i, true); current[name] = v end
            v.last = i
            r[key] = v
        end
        local name = p.output
        if not allNames[name] then allNames[name] = true; before = before + 1 end
        local v
        if spec.full then
            v = newValue(name, i, false)
            current[name] = v
        else
            v = current[name]
            if not v then v = newValue(name, i, true); current[name] = v end
            v.last = i
        end
        reads[i], writes[i] = r, v
    end
    for _, v in pairs(current) do
        if pinned[v.name] then v.last = n + 1 end
    end

    -- Pass 2: assign temporary values to physical buffers in start order.
    local slots, slotOf = {}, {}
    local function place(v, slot)
        slot.value = v
        slotOf[v] = slot
    end
    local function openSlot(v)
        local slot = {}
        slots[#slots + 1] = slot
        place(v, slot)
    end
    for i = 1, n do
        local p, spec, r, w = layers[i].params, specs[i], reads[i], writes[i]
        for _, key in ipairs(spec.inputs) do
            local v = r[key]
            if v.first == i and v.fresh and not v.fixed and not slotOf[v] then openSlot(v) end
        end
        if w.first == i and not w.fixed and not slotOf[w] then
            local chosen
            if not w.fresh then
                for _, key in ipairs(spec.inputs) do
                    -- The stock op already writes over this input: keep the alias.
                    if p[key] == p.output and slotOf[r[key]] then chosen = slotOf[r[key]] end
                end
                if not chosen and spec.inplace then
                    for _, key in ipairs(spec.inputs) do
                        local u = r[key]
                        if not chosen and not u.fixed and u.last == i and slotOf[u]
                            and slotOf[u].value == u then
                            chosen = slotOf[u]
                        end
                    end
                end
                if not chosen then
                    for _, slot in ipairs(slots) do
                        if slot.value.last < i then chosen = slot; break end
                    end
                end
            end
            if chosen then place(w, chosen) else openSlot(w) end
        end
    end

    -- Name the buffers: reuse each slot's first temporary name when possible.
    local used, maxIndex = {}, 0
    for name in pairs(allNames) do
        local index = temporary(name) and tonumber(name:sub(5)) or nil
        if index and index > maxIndex then maxIndex = index end
    end
    local function reserved(name) return used[name] or pinned[name] or (allNames[name] and fixed(name)) end
    for _, v in ipairs(values) do
        local slot = slotOf[v]
        if slot and not slot.name and not reserved(v.name) then
            slot.name = v.name
            used[v.name] = true
        end
    end
    for _, slot in ipairs(slots) do
        if not slot.name then
            repeat
                maxIndex = maxIndex + 1
                slot.name = "__t_" .. maxIndex
            until not reserved(slot.name) and not allNames[slot.name]
            used[slot.name] = true
        end
    end
    local function physical(v) return v.fixed and v.name or slotOf[v].name end

    -- Self-check: replay the renamed accesses and require that every read
    -- finds the value the stock pipeline would see there.
    local holder, after, names = {}, 0, {}
    for i = 1, n do
        local spec, r, w = specs[i], reads[i], writes[i]
        for _, key in ipairs(spec.inputs) do
            local v = r[key]
            local name = physical(v)
            if holder[name] == nil then
                if not v.fresh or v.first ~= i then return result end
                holder[name] = v
            elseif holder[name] ~= v then
                return result
            end
        end
        local name = physical(w)
        if spec.full then
            holder[name] = w
        elseif holder[name] == nil then
            if not w.fresh or w.first ~= i then return result end
            holder[name] = w
        elseif holder[name] ~= w then
            return result
        end
        for _, key in ipairs(spec.inputs) do
            local nm = physical(r[key])
            if not names[nm] then names[nm] = true; after = after + 1 end
        end
        if not names[name] then names[name] = true; after = after + 1 end
    end
    for _, v in pairs(current) do
        if pinned[v.name] and holder[v.name] ~= v then return result end
    end
    if after >= before then return result end

    for i = 1, n do
        local p, spec = layers[i].params, specs[i]
        for _, key in ipairs(spec.inputs) do p[key] = physical(reads[i][key]) end
        p.output = physical(writes[i])
    end
    print(string.format("[tpf2_bigmap] terrain memory: %d -> %d named buffers", before, after))
    return result
end

return M
