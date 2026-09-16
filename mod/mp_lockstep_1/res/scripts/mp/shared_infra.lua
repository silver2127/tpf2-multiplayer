-- Infrastructure keeps its engine owner. A line handover transfers only that
-- line and its fleet, never the stations/tracks that the service happens to use.
-- Requires the guarded native entity-only setPlayer extension, not a Workshop mod.
return function(CM, K)
    function CM.cmRoadPlayer(c, ctx)
        CM.cmEnsure()
        if CM.cmMode ~= "companies" then return false end
        local cid = tonumber(c.company)
        local pid = cid and CM.cmCompanyPid[cid]
        if not pid or not ctx then error("Missing company build context") end
        ctx.player = pid
        if ctx.player ~= pid then error("Company build context rejected its player") end
        return true
    end

    function CM.cmMayModify(eid)
        CM.cmEnsure()
        if CM.cmMode ~= "companies" then return true end
        if not eid or not api.engine.entityExists(eid) then return false end
        if CM.cmForeignOwner(eid) then
            CM.cmNote("Shared infrastructure: only its owner can change it")
            return false
        end
        return true
    end

    function CM.cmMayReplaceEdges(pairsToRemove, isTrack)
        CM.cmEnsure()
        if CM.cmMode ~= "companies" or #pairsToRemove == 0 then return true end
        local ss = api.engine.system.streetSystem
        -- A road proposal can also refresh a rail bridge above it. Removal
        -- records therefore need both networks, not just the primary type.
        local maps = {ss.getNode2TrackEdgeMap(), ss.getNode2StreetEdgeMap()}
        for _, pair in ipairs(pairsToRemove) do
            local found = false
            for _, map in ipairs(maps) do
              for _, eid in pairs(map[pair[1]] or {}) do
                local edge = api.engine.getComponent(eid, api.type.ComponentType.BASE_EDGE)
                if edge and ((edge.node0 == pair[1] and edge.node1 == pair[2]) or
                        (edge.node1 == pair[1] and edge.node0 == pair[2])) then
                    found = true
                    if not CM.cmMayModify(eid) then return false end
                end
              end
            end
            if not found then return false end
        end
        return true
    end

    local ready = false
    local function capable()
        if ready then return true end
        -- The game and DLL can have separate CRT environment snapshots. Read
        -- the capability from the same current bridge identity as lockstep.
        local f = io.open(K.IDENTITY_FILE, "r")
        if not f then return false end
        local text = f:read("*a"); f:close()
        ready = text and text:match("\nentity_owner_v1=1\r?\n") ~= nil
        return ready
    end
    CM.cmOwnerCapability = capable

    function CM.cmSetEntityOwner(eid, pid)
        if not capable() then error("Shared infrastructure requires the matching native DLL") end
        if type(pid) ~= "number" or pid < 0 or pid >= 268435456 or pid ~= math.floor(pid) then
            error("Invalid company player ID")
        end
        -- Native relay decodes this before reaching the engine owner setter.
        game.interface.setPlayer(eid, 1610612736 + pid)
    end

    function CM.cmSetConstructionPlayer(eid, pid)
        if not capable() then error("Shared infrastructure requires the matching native DLL") end
        local co = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION)
        if co and co.depots and #co.depots == 0 then
            return game.interface.setPlayer(eid, pid)
        end
        -- Construction handover also claims vehicles in its depots. This is
        -- wrong for a shared airport: a visiting company's aircraft stays theirs.
        -- Snapshot before the engine call, restore before returning to simulation.
        local owners = {}
        local function take(v)
            local owner = CM.cmOwnerOf(v)
            if owner ~= nil then owners[v] = owner end
        end
        for _, v in pairs(game.interface.getEntities({radius = 999999},
                {type = "VEHICLE", includeData = false}) or {}) do take(v) end
        local parked = api.engine.system.transportVehicleSystem.getVehiclesWithState(
            api.type.enum.TransportVehicleState.IN_DEPOT)
        for i = 1, #parked do take(parked[i]) end
        game.interface.setPlayer(eid, pid)
        for v, owner in pairs(owners) do
            if CM.cmOwnerOf(v) ~= owner then CM.cmSetEntityOwner(v, owner) end
        end
    end

    function CM.cmSetPlayer(eid, pid)
        if CM.cmMode ~= "companies" then return game.interface.setPlayer(eid, pid) end
        local line = api.engine.getComponent(eid, api.type.ComponentType.LINE)
        if not line then
            if api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) then
                return CM.cmSetConstructionPlayer(eid, pid)
            end
            return game.interface.setPlayer(eid, pid)
        end
        -- Enumerate everything before mutating anything. Parked vehicles are
        -- absent from the spatial query; neither query failure may be ignored.
        local fleet, seen = {}, {}
        local function take(v)
            if not seen[v] then
                seen[v] = true
                local tv = api.engine.getComponent(v, api.type.ComponentType.TRANSPORT_VEHICLE)
                if tv and tv.line == eid then fleet[#fleet + 1] = v end
            end
        end
        for _, v in pairs(game.interface.getEntities({radius = 999999},
                {type = "VEHICLE", includeData = false}) or {}) do take(v) end
        local parked = api.engine.system.transportVehicleSystem.getVehiclesWithState(
            api.type.enum.TransportVehicleState.IN_DEPOT)
        for i = 1, #parked do take(parked[i]) end
        table.sort(fleet)
        CM.cmSetEntityOwner(eid, pid)
        for _, v in ipairs(fleet) do CM.cmSetEntityOwner(v, pid) end
    end
end
