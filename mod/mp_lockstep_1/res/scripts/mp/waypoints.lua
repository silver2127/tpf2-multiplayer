-- Waypoints belong to the leg AFTER a station stop. Keep them in that stop's
-- wire record so queued line edits move/delete them with the correct leg.
return function(CM)
    local function describe(id, index)
        if not api.engine.entityExists(id) then error("Missing waypoint " .. tostring(id)) end
        local mil = api.engine.getComponent(id, api.type.ComponentType.MODEL_INSTANCE_LIST)
        local fi = mil and mil.fatInstances and mil.fatInstances[1]
        if not fi then error("Waypoint has no model: " .. id) end
        return string.format("%.3f:%.3f:%.3f:%s:%d", fi.transf[13], fi.transf[14], fi.transf[15],
            CM.escName(api.res.modelRep.getName(fi.modelId)), index)
    end
    function CM.lineWaypointSuffix(waypoints)
        local out = {}
        for i = 1, #(waypoints or {}) do
            local w = waypoints[i]
            out[#out + 1] = describe(w.entity, w.index)
        end
        return #out > 0 and ("~" .. table.concat(out, "!")) or ""
    end
    function CM.lineCaptureWaypoints(line, stops)
        local tail = line:match(" wp=([^%s]+)")
        if not tail then return end
        local byStop = {}
        for row in tail:gmatch("[^,]+") do
            local s, id, index = row:match("^(%d+):(%d+):(%d+)$")
            s, id, index = tonumber(s), tonumber(id), tonumber(index)
            if not s or not stops[s] then error("Invalid captured waypoint leg") end
            byStop[s] = byStop[s] or {}
            byStop[s][#byStop[s] + 1] = describe(id, index)
        end
        for s, list in pairs(byStop) do stops[s] = stops[s] .. "~" .. table.concat(list, "!") end
    end
    function CM.lineReadWaypoints(record)
        local out = {}
        local tail = record:match("~(.*)$")
        if not tail then return out end
        for row in tail:gmatch("[^!]+") do
            local x,y,z,model,index = row:match("^([^:]+):([^:]+):([^:]+):([^:]+):(%d+)$")
            x,y,z,index = tonumber(x),tonumber(y),tonumber(z),tonumber(index)
            if not x or not y or not z or not index then error("Invalid waypoint record") end
            local id = CM.findStopNear(x,y,2.0)
            if not id then error("Waypoint not found near " .. x .. "," .. y) end
            local mil = api.engine.getComponent(id,api.type.ComponentType.MODEL_INSTANCE_LIST)
            local fi = mil and mil.fatInstances and mil.fatInstances[1]
            local sl = api.engine.getComponent(id,api.type.ComponentType.SIGNAL_LIST)
            if not fi or math.abs(fi.transf[15]-z)>1 or
                    api.res.modelRep.getName(fi.modelId)~=CM.unescName(model) or
                    not sl or not sl.signals or index>=#sl.signals then
                error("Waypoint model, height or signal index differs")
            end
            local w = api.type.SignalId.new()
            w.entity, w.index = id, index
            out[#out+1] = w
        end
        return out
    end
end
