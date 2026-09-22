-- tpf2_bigmap minimap: written into res/config/game_script by the Big Maps
-- plugin (tpf2_bigmap.dll) at game start, and removed again when the minimap
-- is switched off or the plugin is uninstalled. Do not edit this copy; the
-- source is tpf2-bigmap/mod/minimap/bigmap_minimap.lua.
--
-- GUI only: there is no update(), save() or load(), so this cannot change the
-- simulation, a savegame or multiplayer determinism.
--
-- Drawing. The terrain, roads, tracks and stations are not drawn here.
-- imageView:setImage() with a "bigmap:minimap?..." token is intercepted by the
-- plugin, which renders that world rectangle natively and uploads it as the
-- image's texture. The road and rail network travels once, as text lines after
-- the token's header, and is cached natively under an id; toggling a layer or a
-- company re-sends only the header. Nothing grows a widget or a line primitive
-- per edge, so a 512x512-tile network costs pixels, not UI objects. This script
-- owns the window, the legend, the town and industry markers, the camera
-- marker and click-to-move.
--
-- Companies. Tracks and stations are coloured by their engine owner
-- (PLAYER_OWNED). In the multiplayer mod's companies mode each company is an
-- engine player; the lobby's mp_company_cfg.txt gives this machine's company
-- and the roster, and colours are the lobby chip colours (mp/companies.lua
-- CM.cmCompanyColor), so a company looks the same here as in the lobby. Any
-- other game with two or more owners gets distinct colours too; a lone
-- player's network keeps the classic track colour.
--
-- Units. setMinimumSize() takes UI units, which the game multiplies by the UI
-- scale, while AbsoluteLayout positions, getContentRect() and getMousePos()
-- are screen pixels (stock selectortooltip.lua mixes the last three). Things
-- are placed in screen pixels, and sizes divided by a UI scale measured from
-- the map container's content rect. How AbsoluteLayout reads rect.y is not
-- documented (the Workshop minimap treated it as the vertical centre); it is
-- measured once per session and the map rebuilt if the guess was wrong.

local TEXTURE_LONG_SIDE = 1024   -- texture pixels along the map's long side
local RELIEF = 4                 -- hillshade exaggeration (the plugin accepts 0..64)
local VIEW_FRACTION = 0.55       -- picture's long side, as a fraction of the screen's short side
local VIEW_MIN, VIEW_MAX = 300, 1400   -- screen pixels
local MARKER_SIZE = 12           -- UI units
local LEGEND_ICON = 16           -- UI units
local MAX_INDUSTRY_MARKERS = 4000
local MAX_OWNERS = 52            -- the plugin's palette holds 53 entries
local CAMERA_SEGMENTS = 32
local INSTALL_FRAMES = 600       -- frames to wait for the main toolbar
local MEASURE_FRAMES = 30        -- frames to wait for the container's first layout
local GATHER_SECONDS = 0.006     -- network gathering budget per frame
local BUTTON_ICON = "ui/button/medium/terrain@2x.tga"
local PLACEHOLDER = "ui/icons/main-menu/map_town.tga"   -- the plugin replaces it; must stay 31 chars
-- res/textures/ui/ui.zip ships these two only as @2x: the plain names are no file.
local TOWN_ICON = "ui/icons/main-menu/map_town@2x.tga"
local INDUSTRY_ICON = "ui/icons/main-menu/map_industry@2x.tga"

-- What each stock industry makes (res/construction/construction.zip,
-- industry/*.con stockListConfig.rule.output). An industry only reports a
-- cargo in itemsProduced once it has produced some, which on a fresh map is
-- almost none of them.
local STOCK_OUTPUT = {
    chemical_plant = "PLASTIC", coal_mine = "COAL", construction_material = "CONSTRUCTION_MATERIALS",
    farm = "GRAIN", food_processing_plant = "FOOD", forest = "LOGS", fuel_refinery = "FUEL",
    goods_factory = "GOODS", iron_ore_mine = "IRON_ORE", machines_factory = "MACHINES", oil_refinery = "OIL",
    oil_well = "CRUDE", quarry = "STONE", saw_mill = "PLANKS", steel_mill = "STEEL", tools_factory = "TOOLS",
}
local LAYER_ROADS, LAYER_TRACKS, LAYER_STATIONS = 1, 2, 4
local EDGE_URBAN, EDGE_COUNTRY, EDGE_HIGHWAY, EDGE_TRACK = 0, 1, 2, 3
local STATION_RAIL, STATION_ROAD, STATION_WATER, STATION_AIR = 0, 1, 2, 3

-- The lobby chip colours (mp/companies.lua CM.CM_COLORS / CM.cmCompanyColor).
local COMPANY_COLORS = { { 220, 80, 80 }, { 80, 140, 230 }, { 90, 190, 110 }, { 230, 180, 60 }, { 180, 100, 220 }, { 80, 200, 200 } }

local tr = _ or function(s) return s end

local function clock()
    local ok, t = pcall(function() return os.clock() end)
    if ok and type(t) == "number" then
        return t
    end
    return nil
end

local state = {
    installed = false,
    installTries = 0,
    button = nil,
    window = nil,
    holder = nil,       -- BoxLayout the map container lives in
    status = nil,       -- TextView next to Refresh
    companyBox = nil,   -- legend: BoxLayout of company rows
    companyRows = {},
    industryBox = nil,  -- legend: BoxLayout of industry type rows
    industryRows = {},
    mapComp = nil,      -- Component wrapping the AbsoluteLayout
    layout = nil,       -- AbsoluteLayout: picture, camera marker, industry and town markers
    map = nil,          -- { x0, y0, x1, y1 (m), w, h (texture px), vw, vh (screen px), sw, sh }
    image = nil,
    camera = nil,
    step = nil,         -- onStep connection
    phase = nil,        -- "measure" -> "calibrate" -> "live" (or "rebuild")
    waitFrames = 0,
    scale = 1,          -- screen pixels per UI unit
    guess = 1,
    probe = nil,        -- UI-unit width the container was given before measuring
    yCentre = true,     -- AbsoluteLayout rect.y is the item's vertical centre (else its top)
    yKnown = false,
    camOffset = nil,    -- picture origin in the camera view's coordinates
    towns = {},
    industryTypes = {},     -- fileName -> { name, icon, count, shown, markers }
    industryTypeOrder = {},
    lastCamera = nil,
    generation = 0,
    netId = 0,          -- id of the network the plugin holds for this world (0 = none yet)
    netSerial = 0,
    gather = nil,
    companies = {},     -- palette rows: { index, pid, label, r, g, b }
    hiddenOwners = {},  -- palette index -> true
    jobs = {},
    show = { towns = true, industries = true, roads = true, tracks = true, stations = true, camera = true },
    clicksLogged = 0,
}

local function log(...)
    print("[bigmap minimap]", ...)
end

-- UI changes never run inside an event callback: they are queued and run from
-- guiUpdate.
local function later(job)
    state.jobs[#state.jobs + 1] = job
end

local function round(v)
    return math.floor(v + 0.5)
end

local function setStatus(text)
    if state.status then
        pcall(function()
            state.status:setText(text)
        end)
    end
end

-- ---- geometry ---------------------------------------------------------------

-- The map is centred on the origin. Binary search, so it needs about 22 calls
-- per axis at any size (a linear scan to a fixed limit broke on large maps).
local function probeEdge(axis)
    local v = api.type.Vec2f.new(0, 0)
    local lo, hi = 0, 4194304
    while hi - lo > 1 do
        local mid = math.floor((lo + hi) / 2)
        if axis == "x" then
            v.x = mid
            v.y = 0
        else
            v.x = 0
            v.y = mid
        end
        if api.engine.terrain.isValidCoordinate(v) then
            lo = mid
        else
            hi = mid
        end
    end
    return lo
end

local function screenSize()
    local ok, rect = pcall(game.gui.getContentRect, "mainView")
    if ok and type(rect) == "table" and rect[3] and rect[4] and rect[3] > 0 and rect[4] > 0 then
        return rect[3], rect[4]
    end
    return 1920, 1080
end

-- Only a starting point: the measured value replaces it before anything is placed.
local function uiScaleGuess(screenH)
    local ok, cfg = pcall(api.util.getAppConfig)
    if ok and cfg and not cfg.uiAutoScaling and type(cfg.uiScaling) == "number" and cfg.uiScaling > 0 then
        return cfg.uiScaling
    end
    return math.max(1, math.floor(screenH / 1080 * 4 + 0.5) / 4)
end

local function computeMap()
    local ex, ey = probeEdge("x"), probeEdge("y")
    if ex < 256 or ey < 256 then
        return nil
    end
    local sw, sh = screenSize()
    local long = math.max(VIEW_MIN, math.min(VIEW_MAX, math.floor(math.min(sw, sh) * VIEW_FRACTION)))
    local m = { x0 = -ex, y0 = -ey, x1 = ex, y1 = ey, sw = sw, sh = sh }
    if ex >= ey then
        m.w = TEXTURE_LONG_SIDE
        m.h = math.max(16, math.floor(TEXTURE_LONG_SIDE * ey / ex + 0.5))
        m.vw = long
        m.vh = math.max(24, math.floor(long * ey / ex + 0.5))
    else
        m.h = TEXTURE_LONG_SIDE
        m.w = math.max(16, math.floor(TEXTURE_LONG_SIDE * ex / ey + 0.5))
        m.vh = long
        m.vw = math.max(24, math.floor(long * ex / ey + 0.5))
    end
    return m
end

-- Picture pixels (screen pixels from the picture's top-left). North (maximum
-- world y) is at the top, matching the texture: the plugin writes the south
-- edge into row 0 and the image draws row 0 at the bottom.
local function toView(x, y)
    local m = state.map
    return (x - m.x0) / (m.x1 - m.x0) * m.vw, (m.y1 - y) / (m.y1 - m.y0) * m.vh
end

local function toWorld(px, py)
    local m = state.map
    return m.x0 + px / m.vw * (m.x1 - m.x0), m.y1 - py / m.vh * (m.y1 - m.y0)
end

local function contentRect(comp)
    if not comp then
        return nil
    end
    local ok, r = pcall(function()
        return comp:getContentRect()
    end)
    if ok and r and type(r.w) == "number" and type(r.h) == "number" and r.w > 0 and r.h > 0 then
        return r
    end
    return nil
end

-- Screen pixels to UI units; returns the UI-unit size actually set.
local function fixSize(comp, w, h)
    local uw = math.max(1, math.floor(w / state.scale + 0.5))
    local uh = math.max(1, math.floor(h / state.scale + 0.5))
    local size = api.gui.util.Size.new(uw, uh)
    comp:setMinimumSize(size)
    comp:setMaximumSize(size)
    return uw, uh
end

-- Adds an item whose top-left corner is (x, top) in screen pixels.
local function addAt(comp, x, top, w, h)
    local r = api.gui.util.Rect.new()
    r.x = math.floor(x + 0.5)
    if state.yCentre then
        r.y = math.floor(top + h / 2 + 0.5)
    else
        r.y = math.floor(top + 0.5)
    end
    r.w = math.floor(w + 0.5)
    r.h = math.floor(h + 0.5)
    state.layout:addItem(comp, r)
end

-- ---- camera ---------------------------------------------------------------

local function cameraController()
    return api.gui.util.getGameUI():getMainRendererComponent():getCameraController()
end

local function moveCamera(x, y)
    local ok, err = pcall(function()
        local cc = cameraController()
        local data = cc:getCameraData()
        data.x = x
        data.y = y
        cc:setCameraData(data)
    end)
    if ok then
        return
    end
    local ok2, err2 = pcall(function()
        local cam = game.gui.getCamera()
        cam[1] = x
        cam[2] = y
        game.gui.setCamera(cam)
    end)
    if not ok2 then
        log("could not move the camera: " .. tostring(err) .. " / " .. tostring(err2))
    end
end

local function focusEntity(entity)
    local ok, err = pcall(function()
        cameraController():focus(entity, false)
    end)
    if not ok then
        log("could not focus entity " .. tostring(entity) .. ": " .. tostring(err))
    end
end

local function drawCamera()
    if not state.camera or not state.map then
        return
    end
    local ok, d = pcall(function()
        return cameraController():getCameraData()
    end)
    if not ok or not d then
        return
    end
    local last = state.lastCamera
    if last and last[1] == d.x and last[2] == d.y and last[3] == d.z then
        return
    end
    state.lastCamera = { d.x, d.y, d.z }
    local m = state.map
    local cx, cy = toView(d.x, d.y)
    if state.camOffset then
        cx = cx + state.camOffset[1]
        cy = cy + state.camOffset[2]
    end
    local perMetre = m.vw / (m.x1 - m.x0)
    local radius = math.max(4, math.min(m.vw / 4, math.abs(d.z or 0) * perMetre))
    state.camera:clear()
    local from, to = api.type.Vec2f.new(0, 0), api.type.Vec2f.new(0, 0)
    for i = 0, CAMERA_SEGMENTS - 1 do
        local a0 = 2 * math.pi * i / CAMERA_SEGMENTS
        local a1 = 2 * math.pi * (i + 1) / CAMERA_SEGMENTS
        from.x = cx + radius * math.cos(a0)
        from.y = cy + radius * math.sin(a0)
        to.x = cx + radius * math.cos(a1)
        to.y = cy + radius * math.sin(a1)
        state.camera:addLine(from, to)
    end
end

-- ---- companies --------------------------------------------------------------

local function companyColor(cid)
    cid = tonumber(cid) or 1
    local c = COMPANY_COLORS[cid]
    if c then
        return c[1] / 255, c[2] / 255, c[3] / 255
    end
    local h = ((cid - 7) * 137.508) % 360
    local sat, val = 0.62, 0.85
    local C = val * sat
    local X = C * (1 - math.abs((h / 60) % 2 - 1))
    local m = val - C
    local r, g, b
    if h < 60 then
        r, g, b = C, X, 0
    elseif h < 120 then
        r, g, b = X, C, 0
    elseif h < 180 then
        r, g, b = 0, C, X
    elseif h < 240 then
        r, g, b = 0, X, C
    elseif h < 300 then
        r, g, b = X, 0, C
    else
        r, g, b = C, 0, X
    end
    return r + m, g + m, b + m
end

-- The multiplayer mod's lobby config, found the way its lockstep.lua looks
-- for its data folder. Absent or unreadable means not in companies mode.
local function readCompanyConfig()
    local cfg = { mode = "coop" }
    local function env(name)
        local ok, v = pcall(function() return os.getenv(name) end)
        if ok and type(v) == "string" and #v > 0 then
            return v
        end
        return nil
    end
    local function dir(p)
        p = p:gsub("\\", "/")
        if p:sub(-1) ~= "/" then
            p = p .. "/"
        end
        return p
    end
    local candidates = {}
    local pinned = env("TPF2MP_DATADIR")
    if pinned then
        candidates[#candidates + 1] = dir(pinned)
    end
    local localData = env("LOCALAPPDATA")
    if localData then
        candidates[#candidates + 1] = dir(localData .. "/tpf2mp/data")
    end
    local function unescape(s)
        return (tostring(s):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
    end
    for i = 1, #candidates do
        local okOpen, f = pcall(io.open, candidates[i] .. "mp_company_cfg.txt", "r")
        if okOpen and f then
            local lines = {}
            pcall(function()
                for k = 1, 3 do
                    lines[k] = f:read("*l")
                end
            end)
            pcall(function() f:close() end)
            if lines[1] then
                cfg.mode = (lines[1]:gsub("%s", ""))
            end
            cfg.mine = lines[2] and tonumber((lines[2]:gsub("%s", "")))
            if lines[3] then
                cfg.roster = {}
                for id in lines[3]:gmatch("%d+") do
                    cfg.roster[#cfg.roster + 1] = tonumber(id)
                end
            end
            -- The multiplayer mod's own map (mp_company_map.txt, since 0.5.8):
            -- "me=<cid>", then "<cid>=<player entity>=<name>" per company. It is
            -- the truth after switches, loads and in-game companies, where the
            -- creation-order guess below is not.
            local okMap, m = pcall(io.open, candidates[i] .. "mp_company_map.txt", "r")
            if okMap and m then
                local companyOf, names, me = {}, {}, nil
                pcall(function()
                    for line in m:lines() do
                        local cid, pid, name = line:match("^(%d+)=(%d+)=(.*)$")
                        if cid then
                            companyOf[tonumber(pid)] = tonumber(cid)
                            names[tonumber(cid)] = unescape(name)
                        else
                            me = tonumber(line:match("^me=(%d+)")) or me
                        end
                    end
                end)
                pcall(function() m:close() end)
                if next(companyOf) then
                    cfg.map, cfg.names = companyOf, names
                    cfg.mode = "companies"
                    if me then cfg.mine = me end
                end
            end
            return cfg
        end
    end
    return cfg
end

-- Every engine player, ascending. Remote companies are created with
-- addPlayer() in roster order, so their ids ascend in roster order too;
-- counting every player (not only owners seen) keeps a company that has built
-- nothing yet from shifting the others' colours.
local function allPlayers(owners)
    local seen, list = {}, {}
    pcall(function()
        api.engine.forEachEntityWithComponent(function(e)
            if not seen[e] then
                seen[e] = true
                list[#list + 1] = e
            end
        end, api.type.ComponentType.PLAYER)
    end)
    for i = 1, #owners do
        if not seen[owners[i]] then
            seen[owners[i]] = true
            list[#list + 1] = owners[i]
        end
    end
    table.sort(list)
    return list
end

-- Palette rows for the owners seen, in palette-index order, and whether to
-- colour by owner at all.
local function describeOwners(owners)
    local cfg = readCompanyConfig()
    local mine
    pcall(function() mine = api.engine.util.getPlayer() end)
    local companyOf = {}
    local companiesMode = cfg.mode == "companies" and cfg.mine and (cfg.roster or cfg.map)
    if cfg.map then
        for pid, cid in pairs(cfg.map) do
            companyOf[pid] = cid
        end
    elseif companiesMode then
        if mine then
            companyOf[mine] = cfg.mine
        end
        local others = {}
        for i = 1, #cfg.roster do
            if cfg.roster[i] ~= cfg.mine then
                others[#others + 1] = cfg.roster[i]
            end
        end
        local k = 0
        local players = allPlayers(owners)
        for i = 1, #players do
            if players[i] ~= mine then
                k = k + 1
                companyOf[players[i]] = others[k]
            end
        end
    end
    -- Without a roster: rank the local player first, then ascending ids.
    local ranked = {}
    for i = 1, #owners do
        ranked[i] = owners[i]
    end
    table.sort(ranked, function(a, b)
        if a == mine then
            return b ~= mine
        end
        if b == mine then
            return false
        end
        return a < b
    end)
    local rank = {}
    for i = 1, #ranked do
        rank[ranked[i]] = i
    end
    local rows = {}
    for i = 1, #owners do
        local pid = owners[i]
        local cid = companyOf[pid]
        local r, g, b = companyColor(cid or rank[pid])
        local label
        if cid then
            label = (cfg.names and cfg.names[cid] and cfg.names[cid] ~= "") and cfg.names[cid] or string.format(tr("Company %d"), cid)
            if pid == mine then
                label = label .. " " .. tr("(you)")
            end
        elseif pid == mine then
            label = tr("Your company")
        else
            label = string.format(tr("Player %s"), tostring(pid))
        end
        rows[#rows + 1] = { index = i - 1, pid = pid, label = label, r = round(r * 255), g = round(g * 255), b = round(b * 255) }
    end
    return rows, (companiesMode and true or false) or #owners >= 2
end

-- ---- network gathering --------------------------------------------------------

local function newGather()
    local ids = {}
    api.engine.forEachEntityWithComponent(function(e)
        ids[#ids + 1] = e
    end, api.type.ComponentType.BASE_EDGE)
    return {
        phase = "edges", edges = ids, i = 1, stationIds = nil, si = 1,
        lines = {}, nodes = {}, streetClass = {}, owners = {}, ownerList = {}, stationCons = {},
        roads = 0, tracks = 0, stations = 0, started = clock(),
    }
end

local function ownerIndex(g, id)
    local po
    pcall(function() po = api.engine.getComponent(id, api.type.ComponentType.PLAYER_OWNED) end)
    local pid = po and po.player
    if not pid then
        return -1
    end
    local idx = g.owners[pid]
    if not idx then
        if #g.ownerList >= MAX_OWNERS then
            return -1
        end
        idx = #g.ownerList
        g.owners[pid] = idx
        g.ownerList[#g.ownerList + 1] = pid
    end
    return idx
end

local function nodeXY(g, node)
    local p = g.nodes[node]
    if not p then
        local n = api.engine.getComponent(node, api.type.ComponentType.BASE_NODE)
        p = { n.position.x, n.position.y }
        g.nodes[node] = p
    end
    return p[1], p[2]
end

-- Stock street types carry categories urban / country / highway / one-way;
-- one-way types are told apart by name.
local function streetClass(g, streetType)
    local c = g.streetClass[streetType]
    if c then
        return c
    end
    c = EDGE_URBAN
    pcall(function()
        local rep = api.res.streetTypeRep.get(streetType)
        local first = rep and rep.categories and rep.categories[1]
        local name = api.res.streetTypeRep.getName(streetType) or ""
        if first == "highway" then
            c = EDGE_HIGHWAY
        elseif first == "country" or (first == "one-way" and string.find(name, "country", 1, true)) then
            c = EDGE_COUNTRY
        end
    end)
    g.streetClass[streetType] = c
    return c
end

local function gatherEdge(g, id)
    local e = api.engine.getComponent(id, api.type.ComponentType.BASE_EDGE)
    if not e then
        return
    end
    local cls, colour
    if api.engine.getComponent(id, api.type.ComponentType.BASE_EDGE_TRACK) then
        cls = EDGE_TRACK
        colour = ownerIndex(g, id)
        g.tracks = g.tracks + 1
    else
        local street = api.engine.getComponent(id, api.type.ComponentType.BASE_EDGE_STREET)
        if not street then
            return
        end
        cls = streetClass(g, street.streetType)
        colour = -1
        g.roads = g.roads + 1
    end
    local x0, y0 = nodeXY(g, e.node0)
    local x1, y1 = nodeXY(g, e.node1)
    g.lines[#g.lines + 1] = string.format("E %d %d %d %d %d %d %d %d %d %d", cls, colour,
        round(x0), round(y0), round(e.tangent0.x), round(e.tangent0.y),
        round(x1), round(y1), round(e.tangent1.x), round(e.tangent1.y))
end

local function stationClass(fileName)
    fileName = fileName or ""
    if string.find(fileName, "station/rail", 1, true) then
        return STATION_RAIL
    elseif string.find(fileName, "station/air", 1, true) or string.find(fileName, "airport", 1, true) then
        return STATION_AIR
    elseif string.find(fileName, "station/water", 1, true) or string.find(fileName, "harbor", 1, true)
        or string.find(fileName, "port", 1, true) then
        return STATION_WATER
    end
    return STATION_ROAD
end

local function gatherStation(g, id)
    local con = api.engine.system.streetConnectorSystem.getConstructionEntityForStation(id)
    local cls, x, y, dx, dy, half, colour
    if con and con ~= -1 then
        if g.stationCons[con] then
            return
        end
        g.stationCons[con] = true
        local c = api.engine.getComponent(con, api.type.ComponentType.CONSTRUCTION)
        if not c then
            return
        end
        cls = stationClass(c.fileName)
        local p = c.transf:cols(3)
        local d = cls == STATION_AIR and c.transf:cols(0) or c.transf:cols(1)
        x, y, dx, dy = p.x, p.y, d.x, d.y
        half = 40
        pcall(function()
            local bv = api.engine.getComponent(con, api.type.ComponentType.BOUNDING_VOLUME)
            if bv and bv.bbox then
                half = math.max(bv.bbox.max.x - bv.bbox.min.x, bv.bbox.max.y - bv.bbox.min.y) / 2
            end
        end)
        colour = ownerIndex(g, con)
    else
        -- A stop on a street is an edge object carrying STATION and PLAYER_OWNED.
        local edge = g.edgeObjects and g.edgeObjects[id]
        local e = edge and api.engine.getComponent(edge, api.type.ComponentType.BASE_EDGE)
        if not e then
            return
        end
        local x0, y0 = nodeXY(g, e.node0)
        local x1, y1 = nodeXY(g, e.node1)
        cls, x, y, dx, dy, half = STATION_ROAD, (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0, 15
        colour = ownerIndex(g, id)
    end
    local len = math.sqrt(dx * dx + dy * dy)
    if len <= 0 then
        dx, dy, len = 1, 0, 1
    end
    g.lines[#g.lines + 1] = string.format("S %d %d %d %d %d %d %d", cls, colour, round(x), round(y),
        round(dx / len * 1000), round(dy / len * 1000), math.min(5000, round(half)))
    g.stations = g.stations + 1
end

local renderTerrain
local rebuildCompanyLegend

local function finishGather(g)
    local rows, coloured = describeOwners(g.ownerList)
    local head = {}
    if coloured then
        for i = 1, #rows do
            head[#head + 1] = string.format("P %d %d %d %d", rows[i].index, rows[i].r, rows[i].g, rows[i].b)
        end
    end
    state.companies = coloured and rows or {}
    state.netSerial = state.netSerial + 1
    -- Unique across world loads (the script's state restarts with each world),
    -- and kept inside 32 bits.
    local base = 0
    pcall(function() base = math.floor(os.time()) % 1000000 end)
    state.netId = base * 1000 + state.netSerial % 1000 + 1
    local payload = table.concat(head, "\n")
    if #g.lines > 0 then
        payload = payload .. (#head > 0 and "\n" or "") .. table.concat(g.lines, "\n")
    end
    g.phase = "done"
    renderTerrain(payload)
    rebuildCompanyLegend()
    setStatus("")
    local t = clock()
    log(string.format("network %.0f: %d road edges, %d track edges, %d stations, %d owners%s%s",
        state.netId, g.roads, g.tracks, g.stations, #g.ownerList, coloured and ", coloured by owner" or "",
        (t and g.started) and string.format(" in %.2f s", t - g.started) or ""))
end

local function gatherTick()
    local g = state.gather
    if not g or g.phase == "done" or not state.image then
        return
    end
    local t0 = clock()
    local n = 0
    while true do
        if g.phase == "edges" then
            if g.i > #g.edges then
                g.phase = "stations"
                g.stationIds = {}
                api.engine.forEachEntityWithComponent(function(e)
                    g.stationIds[#g.stationIds + 1] = e
                end, api.type.ComponentType.STATION)
                pcall(function() g.edgeObjects = api.engine.system.streetSystem.getEdgeObject2EdgeMap() end)
            else
                local id = g.edges[g.i]
                g.i = g.i + 1
                pcall(gatherEdge, g, id)
            end
        elseif g.phase == "stations" then
            if g.si > #g.stationIds then
                g.edgeObjects = nil
                finishGather(g)
                return
            end
            local id = g.stationIds[g.si]
            g.si = g.si + 1
            pcall(gatherStation, g, id)
        else
            return
        end
        n = n + 1
        if n % 64 == 0 then
            local t = clock()
            if not t or not t0 then
                if n >= 3000 then
                    break
                end
            elseif t - t0 >= GATHER_SECONDS then
                break
            end
        end
    end
    local total = #g.edges + (g.stationIds and #g.stationIds or 0)
    local done = (g.i - 1) + (g.si - 1)
    setStatus(string.format(tr("Loading network %d%%"), math.floor(100 * done / math.max(1, total))))
end

-- ---- towns and industries ---------------------------------------------------

local function collectTowns()
    local ids = {}
    api.engine.forEachEntityWithComponent(function(entity)
        ids[#ids + 1] = entity
    end, api.type.ComponentType.TOWN)
    local towns = {}
    for i = 1, #ids do
        local ok, town = pcall(game.interface.getEntity, ids[i])
        if ok and type(town) == "table" and town.position then
            towns[#towns + 1] = { entity = ids[i], x = town.position[1], y = town.position[2], name = town.name }
        end
    end
    return towns
end

local function firstCargo(items)
    if type(items) ~= "table" then
        return nil
    end
    local names = {}
    for k in pairs(items) do
        if type(k) == "string" and k:sub(1, 1) ~= "_" then
            names[#names + 1] = k
        end
    end
    table.sort(names)
    return names[1]
end

local function cargoIcon(cargo)
    local icon
    pcall(function()
        local idx = api.res.cargoTypeRep.find(cargo)
        local rep = idx and idx >= 0 and api.res.cargoTypeRep.get(idx)
        if rep and type(rep.icon) == "string" and rep.icon ~= "" then
            icon = rep.icon
        end
    end)
    return icon
end

-- "industry/usa/steel_mill.con" -> "steel_mill"
local function industryBaseName(fileName)
    return tostring(fileName):match("([^/\\]+)%.con$") or tostring(fileName)
end

-- Every mod's root, from package.path: the game adds "<mod>/res/scripts/?.lua"
-- for each active mod (and the game's own res/scripts, whose constructions are
-- zipped and so never found loose).
local function modRoots()
    local roots, seen = {}, {}
    pcall(function()
        for entry in string.gmatch(tostring(package.path), "[^;]+") do
            local root = entry:gsub("\\", "/"):match("^(.-)/?res/scripts/%?%.lua$")
            if root and not seen[root] then
                seen[root] = true
                roots[#roots + 1] = root
            end
        end
    end)
    return roots
end

-- A mod industry's output, read from its own construction file: the first
-- cargo id in the first "output = { ... }" table, which is how the stock
-- industryutil stockListConfig and the mods built on it spell the rule.
local function conFileOutput(fileName)
    local roots = modRoots()
    for i = #roots, 1, -1 do   -- later mods override earlier ones
        local path = (roots[i] ~= "" and (roots[i] .. "/") or "") .. "res/construction/" .. fileName
        local ok, text = pcall(function()
            local f = io.open(path, "r")
            if not f then
                return nil
            end
            local s = f:read("*a")
            f:close()
            return s
        end)
        if ok and type(text) == "string" then
            for body in text:gmatch("output%s*=%s*(%b{})") do
                local cargo = body:match("(%u[%u%d_]*)%s*=")
                if cargo then
                    return cargo, path
                end
            end
            return nil, path
        end
    end
    return nil
end

-- One entry per industry construction type; its icon is set by resolveIndustryIcons.
local function industryType(fileName)
    local t = state.industryTypes[fileName]
    if not t then
        local base = industryBaseName(fileName)
        local pretty = base:gsub("_", " "):gsub("^%l", string.upper)
        t = { file = fileName, name = pretty, icon = INDUSTRY_ICON, source = nil, count = 0, shown = true, markers = {} }
        pcall(function()
            local rep = api.res.constructionRep.get(api.res.constructionRep.find(fileName))
            if rep and rep.description and rep.description.name and rep.description.name ~= "" then
                t.name = tr(rep.description.name)
            end
        end)
        state.industryTypes[fileName] = t
        state.industryTypeOrder[#state.industryTypeOrder + 1] = t
    end
    return t
end

-- Every type's icon before any marker is placed, so all markers of a type share
-- it. The cargo comes from, in order: what any industry of the type has produced
-- (authoritative), the output rule in a mod's own construction file (a mod
-- industry, or a mod's replacement of a stock one), the stock table (the stock
-- files are zipped), what one has taken in, and else the generic icon. Only a
-- produced cargo is final; production is asked again on the next build.
local function resolveIndustryIcons(industries)
    local sims = {}
    for i = 1, #industries do
        local t = industryType(industries[i].file)
        if t.source ~= "produced" and industries[i].sim then
            sims[t] = sims[t] or {}
            table.insert(sims[t], industries[i].sim)
        end
    end
    local counts = { produced = 0, file = 0, stock = 0, consumed = 0, generic = 0 }
    local generic = {}
    for i = 1, #state.industryTypeOrder do
        local t = state.industryTypeOrder[i]
        local list = sims[t]
        if list then
            local produced, consumed
            for k = 1, #list do
                local ok, ent = pcall(game.interface.getEntity, list[k])
                if ok and type(ent) == "table" then
                    produced = firstCargo(ent.itemsProduced)
                    consumed = consumed or firstCargo(ent.itemsConsumed)
                    if produced then
                        break
                    end
                end
            end
            if t.fileOutput == nil then
                t.fileOutput = conFileOutput(t.file) or false   -- read a construction file once per session
            end
            local cargo, source = produced, "produced"
            if not cargo and t.fileOutput then
                cargo, source = t.fileOutput, "file"
            end
            if not cargo and STOCK_OUTPUT[industryBaseName(t.file)] then
                cargo, source = STOCK_OUTPUT[industryBaseName(t.file)], "stock"
            end
            if not cargo and consumed then
                cargo, source = consumed, "consumed"
            end
            local icon = cargo and cargoIcon(cargo)
            if not icon then
                source = "generic"
            end
            t.icon, t.source, t.cargo = icon or INDUSTRY_ICON, source, icon and cargo or nil
        end
        if t.source then
            counts[t.source] = counts[t.source] + 1
            if t.source == "generic" then
                generic[#generic + 1] = t.file
            end
        end
    end
    log(string.format("industry icons: %d from production, %d from mod files, %d from the stock list, %d from inputs, "
        .. "%d generic%s", counts.produced, counts.file, counts.stock, counts.consumed, counts.generic,
        #generic > 0 and (" (" .. table.concat(generic, ", ") .. ")") or ""))
end

local function collectIndustries()
    local ids = {}
    api.engine.forEachEntityWithComponent(function(entity)
        ids[#ids + 1] = entity
    end, api.type.ComponentType.SIM_BUILDING)
    local seen, list = {}, {}
    for i = 1, #ids do
        if #list >= MAX_INDUSTRY_MARKERS then
            log("industry markers capped at " .. MAX_INDUSTRY_MARKERS)
            break
        end
        local ok, con = pcall(api.engine.system.streetConnectorSystem.getConstructionEntityForSimBuilding, ids[i])
        if ok and con and con ~= -1 and not seen[con] then
            seen[con] = true
            local ok2, info = pcall(function()
                local c = api.engine.getComponent(con, api.type.ComponentType.CONSTRUCTION)
                if not c or not c.fileName or not string.find(c.fileName, "industry/", 1, true) then
                    return nil
                end
                local p = c.transf:cols(3)
                local nameComp = api.engine.getComponent(con, api.type.ComponentType.NAME)
                return { entity = con, sim = ids[i], x = p.x, y = p.y, file = c.fileName,
                         name = nameComp and nameComp.name or c.fileName }
            end)
            if ok2 and info then
                list[#list + 1] = info
            end
        end
    end
    return list
end

local function addMarker(icon, item)
    local px, py = toView(item.x, item.y)
    local img = api.gui.comp.ImageView.new(icon)
    local size = api.gui.util.Size.new(MARKER_SIZE, MARKER_SIZE)
    img:setMinimumSize(size)
    img:setMaximumSize(size)
    if item.name and item.name ~= "" then
        img:setTooltip(item.name)
    end
    local entity = item.entity
    img:insertMouseListener(function(e)
        if e.button == 0 and e.type == 2 then
            later(function()
                focusEntity(entity)
            end)
            return true
        end
        return false
    end)
    local s = MARKER_SIZE * state.scale
    addAt(img, px - s / 2, py - s / 2, s, s)
    return img
end

local function applyVisibility()
    for i = 1, #state.towns do
        state.towns[i]:setVisible(state.show.towns, false)
    end
    for i = 1, #state.industryTypeOrder do
        local t = state.industryTypeOrder[i]
        for k = 1, #t.markers do
            t.markers[k]:setVisible(state.show.industries and t.shown, false)
        end
    end
    if state.camera then
        state.camera:setVisible(state.show.camera, false)
    end
end

-- ---- legend -------------------------------------------------------------------

local function clearRows(box, rows)
    for i = #rows, 1, -1 do
        local row = rows[i]
        pcall(function()
            box:removeItem(row)
            row:destroy()
        end)
        rows[i] = nil
    end
end

local function legendRow(parts)
    local layout = api.gui.layout.BoxLayout.new("HORIZONTAL")
    for i = 1, #parts do
        layout:addItem(parts[i])
    end
    local row = api.gui.comp.Component.new("")
    row:setLayout(layout)
    return row
end

-- A colour chip: one thick line in a small line view.
local function swatch(r, g, b)
    local view = api.gui.comp.LineRenderView.new()
    local w, h = 18, 10
    local size = api.gui.util.Size.new(w, h)
    view:setMinimumSize(size)
    view:setMaximumSize(size)
    view:setColor(api.type.Vec4f.new(r / 255, g / 255, b / 255, 1))
    local sh = h * state.scale
    view:setWidth(sh)
    view:addLine(api.type.Vec2f.new(0, sh / 2), api.type.Vec2f.new(w * state.scale, sh / 2))
    return view
end

rebuildCompanyLegend = function()
    if not state.companyBox then
        return
    end
    clearRows(state.companyBox, state.companyRows)
    for i = 1, #state.companies do
        local c = state.companies[i]
        local box = api.gui.comp.CheckBox.new("")
        box:setSelected(not state.hiddenOwners[c.index], false)
        local index = c.index
        box:onToggle(function(on)
            state.hiddenOwners[index] = not on or nil
            later(function()
                renderTerrain()
            end)
        end)
        local row = legendRow({ box, swatch(c.r, c.g, c.b), api.gui.comp.TextView.new(c.label) })
        state.companyBox:addItem(row)
        state.companyRows[#state.companyRows + 1] = row
    end
end

local function rebuildIndustryLegend()
    if not state.industryBox then
        return
    end
    clearRows(state.industryBox, state.industryRows)
    table.sort(state.industryTypeOrder, function(a, b) return a.name < b.name end)
    for i = 1, #state.industryTypeOrder do
        local t = state.industryTypeOrder[i]
        local box = api.gui.comp.CheckBox.new("")
        box:setSelected(t.shown, false)
        box:onToggle(function(on)
            t.shown = on
            later(applyVisibility)
        end)
        local icon = api.gui.comp.ImageView.new(t.icon)
        local size = api.gui.util.Size.new(LEGEND_ICON, LEGEND_ICON)
        icon:setMinimumSize(size)
        icon:setMaximumSize(size)
        local row = legendRow({ box, icon, api.gui.comp.TextView.new(string.format("%s (%d)", t.name, t.count)) })
        state.industryBox:addItem(row)
        state.industryRows[#state.industryRows + 1] = row
    end
end

-- ---- map ------------------------------------------------------------------

-- The picture: terrain always; roads, tracks and stations from the plugin's
-- cached network, if it is this world's. `payload` (network text) replaces
-- that cache.
renderTerrain = function(payload)
    local m = state.map
    if not m or not state.image then
        return
    end
    state.generation = state.generation + 1
    local layers = 0
    if state.show.roads then
        layers = layers + LAYER_ROADS
    end
    if state.show.tracks then
        layers = layers + LAYER_TRACKS
    end
    if state.show.stations then
        layers = layers + LAYER_STATIONS
    end
    local hide = 0
    for index, hidden in pairs(state.hiddenOwners) do
        if hidden then
            hide = hide + 2 ^ index
        end
    end
    -- %.0f, not %d, for the id and the mask: Lua 5.2's %d goes through a C long,
    -- which is 32-bit on Windows, and rejects anything larger.
    local token = string.format("bigmap:minimap?x0=%d&y0=%d&x1=%d&y1=%d&w=%d&h=%d&relief=%d&g=%d&layers=%d&net=%.0f&hide=%.0f",
        m.x0, m.y0, m.x1, m.y1, m.w, m.h, RELIEF, state.generation, layers, state.netId, hide)
    if payload then
        token = token .. "\n" .. payload
    end
    state.image:setImage(token, false)
end

-- A click on the picture moves the camera there. The position comes from
-- getMousePos() and the picture's content rect, which share screen pixels;
-- the first clicks are logged with the event's own position for comparison.
local function onMapMouse(e)
    if not (e.button == 0 and e.type == 2) or not state.map or state.phase ~= "live" then
        return false
    end
    local m = state.map
    local px, py, source
    local ir = contentRect(state.image)
    local okMouse, mouse = pcall(game.gui.getMousePos)
    if ir and okMouse and type(mouse) == "table" and mouse[1] and mouse[2] then
        px = (mouse[1] - ir.x) * m.vw / ir.w
        py = (mouse[2] - ir.y) * m.vh / ir.h
        source = "mouse"
    else
        local p = e.pos or e.position
        if p then
            px, py, source = p.x, p.y, "event"
        end
    end
    if state.clicksLogged < 3 then
        state.clicksLogged = state.clicksLogged + 1
        local p = e.pos or e.position
        log(string.format("map click: picture (%s, %s) from %s; event pos (%s, %s); picture rect %s",
            tostring(px), tostring(py), tostring(source), tostring(p and p.x), tostring(p and p.y),
            ir and string.format("%d,%d %dx%d", ir.x, ir.y, ir.w, ir.h) or "?"))
    end
    if not (px and py) or px < 0 or py < 0 or px > m.vw or py > m.vh then
        return false
    end
    local x, y = toWorld(px, py)
    later(function()
        moveCamera(x, y)
    end)
    return true
end

local function clearMap()
    if state.step then
        pcall(function()
            state.step:disconnect()
        end)
        state.step = nil
    end
    if state.mapComp then
        local comp = state.mapComp
        state.mapComp = nil
        pcall(function()
            state.holder:removeItem(comp)
            comp:destroy()
        end)
    end
    state.layout, state.image, state.camera, state.lastCamera, state.camOffset = nil, nil, nil, nil, nil
    state.towns = {}
    for i = 1, #state.industryTypeOrder do
        state.industryTypeOrder[i].markers = {}
        state.industryTypeOrder[i].count = 0
    end
    state.phase = nil
    state.gather = nil
end

-- Everything that depends on the measured UI scale.
local function populate()
    local m = state.map
    state.image = api.gui.comp.ImageView.new(PLACEHOLDER)
    fixSize(state.image, m.vw, m.vh)
    state.image:insertMouseListener(onMapMouse)
    addAt(state.image, 0, 0, m.vw, m.vh)
    renderTerrain()     -- terrain now; this world's previous network, if any, until the new one is in

    -- Above the picture, below the markers, and clickable like the picture.
    state.camera = api.gui.comp.LineRenderView.new()
    state.camera:setColor(api.type.Vec4f.new(1, 1, 1, 1))
    state.camera:setWidth(2)
    state.camera:insertMouseListener(onMapMouse)
    addAt(state.camera, 0, 0, m.vw, m.vh)

    local industries = collectIndustries()
    resolveIndustryIcons(industries)
    for i = 1, #industries do
        local t = industryType(industries[i].file)
        t.count = t.count + 1
        t.markers[#t.markers + 1] = addMarker(t.icon, industries[i])
    end
    local towns = collectTowns()
    for i = 1, #towns do
        state.towns[#state.towns + 1] = addMarker(TOWN_ICON, towns[i])
    end
    -- Types with no industry left on this map drop out of the legend.
    local kept = {}
    for i = 1, #state.industryTypeOrder do
        if state.industryTypeOrder[i].count > 0 then
            kept[#kept + 1] = state.industryTypeOrder[i]
        else
            state.industryTypes[state.industryTypeOrder[i].file] = nil
        end
    end
    state.industryTypeOrder = kept
    rebuildIndustryLegend()
    applyVisibility()
    log(string.format("map %dx%d m in a %dx%d px picture, texture %dx%d, %d towns, %d industries of %d types",
        m.x1 - m.x0, m.y1 - m.y0, m.vw, m.vh, m.w, m.h, #towns, #industries, #state.industryTypeOrder))
end

-- Only once the layout is confirmed: a calibration rebuild would discard it.
local function startGather()
    state.gather = newGather()
    setStatus(tr("Loading network"))
end

local buildMap

local function logLayout()
    local m = state.map
    log(string.format("layout: UI scale %.3f (guessed %.2f), AbsoluteLayout y is the %s, picture %dx%d px",
        state.scale, state.guess, state.yCentre and "vertical centre" or "top edge", m.vw, m.vh))
end

local function onMapStep()
    local m = state.map
    if not m or not state.mapComp then
        return
    end
    if state.phase == "measure" then
        local cr = contentRect(state.mapComp)
        if cr then
            state.scale = cr.w / state.probe
        else
            state.waitFrames = state.waitFrames + 1
            if state.waitFrames < MEASURE_FRAMES then
                return
            end
            state.scale = state.guess
            log("layout: the map container never reported a size; using the UI scale guess")
        end
        if not (state.scale > 0.2 and state.scale < 10) then
            state.scale = state.guess
        end
        fixSize(state.mapComp, m.vw, m.vh)
        populate()
        if state.yKnown then
            state.phase = "live"
            logLayout()
            startGather()
        else
            state.phase = "calibrate"
        end
        return
    end
    if state.phase == "calibrate" then
        local cr, ir = contentRect(state.mapComp), contentRect(state.image)
        if not (cr and ir) then
            return
        end
        local dx, dy = ir.x - cr.x, ir.y - cr.y
        local tol = math.max(3, m.vh * 0.02)
        state.yKnown = true
        if math.abs(dy) > tol and math.abs(math.abs(dy) - m.vh / 2) <= tol then
            state.yCentre = not state.yCentre
            state.phase = "rebuild"
            log(string.format("layout: picture landed %d px off vertically, so AbsoluteLayout y is the %s; rebuilding",
                dy, state.yCentre and "vertical centre" or "top edge"))
            later(buildMap)
            return
        end
        if math.abs(dx) > tol or math.abs(dy) > tol then
            log(string.format("layout: unexpected picture offset (%d, %d) px", dx, dy))
        end
        logLayout()
        state.phase = "live"
        startGather()
    end
    if state.phase == "live" then
        if not state.camOffset then
            local ir, vr = contentRect(state.image), contentRect(state.camera)
            if ir and vr then
                state.camOffset = { ir.x - vr.x, ir.y - vr.y }
                state.lastCamera = nil
            end
        end
        drawCamera()
    end
end

buildMap = function()
    clearMap()
    local m = computeMap()
    if not m then
        log("could not work out the map extent")
        return
    end
    state.map = m
    state.guess = uiScaleGuess(m.sh)
    state.scale = state.guess
    state.layout = api.gui.layout.AbsoluteLayout.new()
    local comp = api.gui.comp.Component.new("")
    comp:setLayout(state.layout)
    state.probe = fixSize(comp, m.vw, m.vh)
    state.holder:addItem(comp)
    state.mapComp = comp
    state.phase = "measure"
    state.waitFrames = 0
    state.step = comp:onStep(function()
        local ok, err = pcall(onMapStep)
        if not ok then
            log("map step: " .. tostring(err))
        end
    end)
end

-- ---- window and toolbar -----------------------------------------------------

local function checkbox(label, selected, onToggle)
    local box = api.gui.comp.CheckBox.new(tr(label))
    box:setSelected(selected, false)
    box:onToggle(onToggle)
    return box
end

local function buildWindow()
    local columns = api.gui.layout.BoxLayout.new("HORIZONTAL")

    local mapColumn = api.gui.layout.BoxLayout.new("VERTICAL")
    local bar = api.gui.layout.BoxLayout.new("HORIZONTAL")
    local refresh = api.gui.comp.Button.new(api.gui.comp.TextView.new(tr("Refresh")), true)
    refresh:onClick(function()
        later(buildMap)
    end)
    bar:addItem(refresh)
    state.status = api.gui.comp.TextView.new("")
    bar:addItem(state.status)
    local barComp = api.gui.comp.Component.new("")
    barComp:setLayout(bar)
    mapColumn:addItem(barComp)
    state.holder = api.gui.layout.BoxLayout.new("VERTICAL")
    local holderComp = api.gui.comp.Component.new("")
    holderComp:setLayout(state.holder)
    mapColumn:addItem(holderComp)
    local mapColumnComp = api.gui.comp.Component.new("")
    mapColumnComp:setLayout(mapColumn)
    columns:addItem(mapColumnComp)

    local legend = api.gui.layout.BoxLayout.new("VERTICAL")
    legend:addItem(api.gui.comp.TextView.new(tr("Show")))
    local function markerToggle(label, key)
        legend:addItem(checkbox(label, true, function(on)
            state.show[key] = on
            later(applyVisibility)
        end))
    end
    local function pictureToggle(label, key)
        legend:addItem(checkbox(label, true, function(on)
            state.show[key] = on
            later(function()
                renderTerrain()
            end)
        end))
    end
    markerToggle("Towns", "towns")
    markerToggle("Industries", "industries")
    pictureToggle("Roads", "roads")
    pictureToggle("Tracks", "tracks")
    pictureToggle("Stations", "stations")
    markerToggle("Camera", "camera")
    legend:addItem(api.gui.comp.TextView.new(tr("Companies")))
    state.companyBox = api.gui.layout.BoxLayout.new("VERTICAL")
    local companyComp = api.gui.comp.Component.new("")
    companyComp:setLayout(state.companyBox)
    legend:addItem(companyComp)
    legend:addItem(api.gui.comp.TextView.new(tr("Industry types")))
    state.industryBox = api.gui.layout.BoxLayout.new("VERTICAL")
    local industryComp = api.gui.comp.Component.new("")
    industryComp:setLayout(state.industryBox)
    legend:addItem(industryComp)
    local legendComp = api.gui.comp.Component.new("")
    legendComp:setLayout(legend)
    columns:addItem(legendComp)

    local window = api.gui.comp.Window.new(tr("Minimap"), columns)
    window:addHideOnCloseHandler()
    window:onClose(function()
        if state.button then
            state.button:setSelected(false, false)
        end
    end)
    window:setVisible(false, false)
    return window
end

local function openWindow()
    local sw, sh = screenSize()
    state.window:setPosition(math.floor(sw * 0.15), math.floor(sh * 0.08))
    state.window:setVisible(true, false)
    if not state.mapComp then
        buildMap()
    end
end

local function install()
    local main = api.gui.util.getById("mainButtonsLayout")
    if not main then
        return false
    end
    local layout = main:getItem(0)
    if not layout then
        return false
    end
    local icon = api.gui.comp.ImageView.new(BUTTON_ICON)
    icon:setMinimumSize(api.gui.util.Size.new(48, 48))
    icon:setMaximumSize(api.gui.util.Size.new(60, 60))
    local button = api.gui.comp.ToggleButton.new(icon)
    button:setTooltip(tr("Minimap (Big Maps)"))
    button:setMinimumSize(api.gui.util.Size.new(48, 48))
    layout:insertItem(button, 0)
    state.button = button
    state.window = buildWindow()
    button:onToggle(function(on)
        if on then
            later(openWindow)
        else
            later(function()
                state.window:close()
            end)
        end
    end)
    log("installed")
    return true
end

-- Retried each frame until the toolbar exists; an error stops the retries so a
-- half-built button is never added twice.
local function tryInstall()
    if state.installed or state.installTries >= INSTALL_FRAMES then
        return
    end
    state.installTries = state.installTries + 1
    local ok, result = pcall(install)
    if ok then
        state.installed = result
    else
        state.installTries = INSTALL_FRAMES
        log("could not add the minimap button: " .. tostring(result))
    end
end

local function runJobs()
    local jobs = state.jobs
    if #jobs == 0 then
        return
    end
    state.jobs = {}
    for i = 1, #jobs do
        local ok, err = pcall(jobs[i])
        if not ok then
            log("error: " .. tostring(err))
        end
    end
end

function data()
    return {
        guiInit = tryInstall,
        guiUpdate = function()
            tryInstall()
            runJobs()
            local ok, err = pcall(gatherTick)
            if not ok then
                log("network: " .. tostring(err))
                state.gather = nil
                setStatus("")
            end
        end,
    }
end
