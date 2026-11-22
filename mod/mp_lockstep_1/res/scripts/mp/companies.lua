-- mp/companies.lua -- companies: one registry every machine agrees on (rewrite 2026-09-27)
--
-- Loaded from the game script as
--     local companies = require("mp.companies")(CM, K, log)
-- It is a FACTORY, not a plain module table: every load of the game script
-- gets fresh state (package.loaded would otherwise hand the next game the
-- previous game's file-scope locals). Everything lives on CM (Lua 5.1 allows
-- only 200 locals per chunk and lockstep.lua sits near it). The body stays at
-- column 0 on purpose: tools/luacheck.py's use-before-define checks look at
-- column-0 declarations.
--
-- WHY A REWRITE. The first companies code grew one bug report at a time and
-- decided things from state that differs between machines: lobby letters kept
-- in the save (a host change handed every player the other's company, the
-- 2026-09-27 report), company ids picked by the requester before the stamp (two
-- players creating at once shared one company), a command's company decided when
-- it ARRIVED rather than at its stamp, dissolve refused or applied by local
-- heartbeat timing, and the menu's password field meaning three things. The
-- rules now:
--
--   * ONE REGISTRY (CM.co), changed only by company commands at their stamp, in
--     stamp order, on every machine alike. It knows every company (name, colour,
--     vehicle paint, password, station access, founder), which PLAYER belongs to
--     which company, and which lobby letter each player has in THIS session.
--   * PLAYERS ARE KEYS, NOT LETTERS. A player is "s:<SteamID64>" (the game's own
--     tpf2_steam.txt), else "n:<lobby name>", else "l:<letter>". Each game says who
--     it is in its own CMJOIN once it is live; the registry maps the key to a
--     company and the session's letter to the key. Letters never reach the save.
--   * A COMMAND'S COMPANY IS DECIDED AT ITS STAMP (CM.cmAttribute, from execute in
--     lockstep.lua): the company its origin's player belongs to at that point.
--   * THE ENGINE SIDE IS LOCAL and unchanged in spirit: each company is an engine
--     player entity; the local human entity is whichever company this machine
--     plays, the others are addPlayer() entities, and a switch swaps assets and
--     wallets between the two (the hotseat, CM.cmLocalSwitch). Entity ids differ
--     per machine (CM.cmCompanyPid); what each COMPANY owns does not.
--
-- Company commands (all applied by CM.execCompanyCmd at the stamp):
--   CMJOIN   key name want lm      a game joins the session as player `key`
--   CMLEAVE  who                   letter `who` left the lobby
--   CMNEW    name color paint pw   a new company (id chosen at the stamp); the origin switches to it
--   CMSWITCH cid pw pw1            the origin plays company cid
--   CMDEL    cid into pw pw1       company cid is deleted; assets, money and loan go to `into`
--   CMNAME   cid name              rename (members only)
--   CMCOLOR  cid color paint       colour / vehicle paint (members only)
--   CMPW     cid pw                set / clear the password (members only)
--   CMOPEN   cid who on            who may stop at cid's stations (members only)
return function(CM, K, log)
require("mp.shared_infra")(CM, K)

CM.CM_CFG_FILE = K.BASE .. "mp_company_cfg.txt"
CM.CM_MAX = 200                    -- company ids 1..200; also the palette size (style sheet !mpCo1..200)
CM.CM_PICK_COLORS = 24             -- a new company's default colours (the first 24 palette entries)
CM.CM_JOIN_HOLD_TICKS = 1800       -- a game whose own CMJOIN never lands releases its held actions after ~30 s
CM.CM_LEAVE_GRACE = 10             -- seconds a letter must be gone from the roster before CMLEAVE

-- The game buffers stdout until exit, so print()-only logging is invisible while
-- a live test runs. Companies diagnostics also go to their own file.
function CM.cmLog(s)
	log(s)
	local f = io.open(K.BASE .. "mp_company_" .. tostring(K.INSTANCE or "?") .. ".log", "a")
	if f then f:write(s, "\n"); f:close() end
end
function CM.cmNote(s) CM.cmLastNote = s; log("company: " .. s) end

-- ---------- state ----------
-- The registry. Replicated: changed only at a command's stamp.
function CM.cmEmptyRegistry()
	return { v = 2, mode = "coop", list = {}, members = {}, names = {}, origin = {}, nextId = 1, founded = {} }
end
CM.co = CM.cmEmptyRegistry()
-- Local representation (per machine, valid in this machine's world).
CM.cmMode       = "coop"     -- mirror of CM.co.mode, read by every consumer
CM.cmMyCompany  = nil        -- the company this machine's HUMAN entity plays right now
CM.cmCompanyPid = {}         -- [cid] = engine player entity on this machine
CM.cmExpectedCompany = {}    -- conKey -> origin company of a construction replay about to land here
CM.cmExpectedBal0    = {}    -- conKey -> our balance right before that replay was applied
CM.cmOriginCompany   = {}    -- derived: letter -> company, for the cursors
CM.cmRoster          = {}    -- derived: sorted company ids
CM.cmBooted     = false      -- the save / lobby file has been read for this load
CM.cmLive       = false      -- (kept for lockstep.lua) same as cmBooted
CM.cmJoinAsked  = false      -- our CMJOIN is scheduled
CM.cmJoined     = false      -- our CMJOIN has been applied here
CM.cmSaved      = nil        -- the save's record, waiting for cmBoot
CM.cmCarried    = nil        -- a record this machine could not apply: saved back unchanged
CM.cmSwitchWanted = nil      -- the hotseat this machine still has to do (waits for a world that answers)
CM.cmSwitchTries  = 0
CM.cmMigrated   = nil        -- a note about an imported older save, for the menu

-- ---------- small helpers ----------
function CM.cmCo(cid) cid = tonumber(cid); return cid and CM.co.list[cid] or nil end
function CM.cmIds()
	local ids = {}
	for cid in pairs(CM.co.list) do ids[#ids + 1] = cid end
	table.sort(ids)
	return ids
end
function CM.cmRosterHas(cid) return CM.cmCo(cid) ~= nil end
-- the company a player (key) belongs to
function CM.cmCompanyOfKey(key) local cid = key and CM.co.members[key]; return CM.cmCo(cid) and cid or nil end
-- the company an origin letter plays in this session
function CM.cmCompanyOfOrigin(o) return CM.cmCompanyOfKey(o and CM.co.origin[o]) end
-- the letters playing a company in this session, sorted (deterministic: registry only)
function CM.cmPlayersOf(cid)
	local out = {}
	for o, key in pairs(CM.co.origin) do if CM.co.members[key] == cid then out[#out + 1] = o end end
	table.sort(out)
	return out
end
-- the player names in a company (members, present or not), sorted
function CM.cmMemberNames(cid)
	local out = {}
	for key, c in pairs(CM.co.members) do if c == cid then out[#out + 1] = CM.co.names[key] or key end end
	table.sort(out)
	return out
end
-- derived views some consumers read
function CM.cmRefreshDerived()
	CM.cmMode = CM.co.mode == "companies" and "companies" or "coop"
	CM.cmRoster = CM.cmIds()
	local om = {}
	for o in pairs(CM.co.origin) do om[o] = CM.cmCompanyOfOrigin(o) end
	CM.cmOriginCompany = om
end
function CM.cmPidAlive(pid)
	if type(pid) ~= "number" or pid <= 0 then return false end
	local alive = false
	pcall(function() alive = api.engine.entityExists(pid) end)
	return alive
end
function CM.cmHuman()
	local h = nil
	pcall(function() h = api.engine.util.getPlayer() end)
	return h
end
function CM.cmCompanyOfPid(pid)
	if pid == nil then return nil end
	for cid, p in pairs(CM.cmCompanyPid or {}) do if p == pid then return cid end end
	return nil
end
function CM.cmOwnerOf(eid)
	local ok, comp = pcall(function()
		return api.engine.getComponent(eid, api.type.ComponentType.PLAYER_OWNED)
	end)
	if ok and comp then return comp.player end
	return nil
end

-- ---------- players: who am I, and the lobby's roster ----------
if type(CM.playerNames) ~= "table" then CM.playerNames = {} end
-- mp_players.txt: the menu DLL writes "a=alice" per line from the lobby roster.
-- Display only (and the check that a saved letter still is the same player);
-- company decisions never read it.
function CM.readPlayerNames()
	local f = io.open(K.BASE .. "mp_players.txt", "r")
	if not f then return end
	local t = {}
	for line in f:lines() do
		local l, n = line:match("^(%a+)=(.+)$")
		if l and n then t[l] = n:gsub("[%c]", "") end
	end
	f:close()
	CM.playerNames = t
end
function CM.playerNameOf(letter)
	return CM.playerNames[letter] or letter
end
-- This game's player: its SteamID64 from the bridge's tpf2_steam.txt ("id=..."),
-- else its lobby name, else its letter. Read fresh until a Steam id is found.
function CM.cmMyKey()
	if CM.cmMyKeyCached then return CM.cmMyKeyCached end
	local id = nil
	pcall(function()
		local f = io.open(K.BASE .. "tpf2_steam.txt", "r")
		if f then
			local text = f:read("*a") or ""; f:close()
			id = text:match("id=(%d%d%d%d%d%d+)")
		end
	end)
	if id then CM.cmMyKeyCached = "s:" .. id; return CM.cmMyKeyCached end
	pcall(CM.readPlayerNames)
	local n = K.INSTANCE and CM.playerNames[K.INSTANCE]
	if n and n ~= "" then return "n:" .. n end
	return "l:" .. tostring(K.INSTANCE or "?")
end
function CM.cmMyName()
	pcall(CM.readPlayerNames)
	local n = K.INSTANCE and CM.playerNames[K.INSTANCE]
	if n and n ~= "" then return n end
	local name = nil
	pcall(function()
		local f = io.open(K.BASE .. "tpf2_names.txt", "r")
		if f then local t = f:read("*a") or ""; f:close(); name = t:match("player=([^\r\n]+)") end
	end)
	return name or tostring(K.INSTANCE or "?")
end
-- Players still loading in: the menu DLL writes mp_loading.txt ("letter=name=stage"
-- per player receiving the save, loading or catching up; empty once everyone is
-- in). A company change is refused on the REQUESTING game while it is not empty
-- (before anything ships), so no machine ever decides on it.
function CM.cmLoadingPlayers()
	local out = {}
	local f = io.open(K.BASE .. "mp_loading.txt", "r")
	if not f then return out end
	for line in f:lines() do
		local l, n, stage = line:match("^(%a+)=([^=]*)=?(.*)$")
		if l and l ~= K.INSTANCE then out[#out + 1] = { letter = l, name = (n ~= "" and n or l):gsub("[%c]", ""), stage = (stage or ""):gsub("[%c]", "") } end
	end
	f:close()
	return out
end
function CM.cmLoadingNote(who)
	local names = {}
	for _, p in ipairs(who) do names[#names + 1] = p.name end
	return string.format("company changes wait until %s %s loaded in", table.concat(names, ", "), #names == 1 and "has" or "have")
end

-- ---------- names and colours ----------
function CM.cmOrdinal(n)
	n = tonumber(n) or 1
	if n <= 1 then return "" end
	local last, tens = n % 10, n % 100
	local suffix = (tens >= 11 and tens <= 13) and "th" or (last == 1 and "st" or last == 2 and "nd" or last == 3 and "rd" or "th")
	return tostring(n) .. suffix .. " "
end
function CM.cmDisplayName(cid, given, founderName, ordinal)
	if given and given ~= "" then return given end
	if founderName and founderName ~= "" then return founderName .. "'s " .. CM.cmOrdinal(ordinal) .. "company" end
	return "Company " .. tostring(cid)
end
-- a company's name: the one given, else "<founder>'s [Nth ]company", else "Company N"
function CM.cmNameOf(cid)
	local co = CM.cmCo(cid)
	if not co then return "Company " .. tostring(cid) end
	local fname = co.founder and CM.co.names[co.founder] or co.fname
	return CM.cmDisplayName(cid, co.name, fname, co.ord)
end
-- COLOURS (free choice, 2026-09-27). A company's colour is one number:
--   1..200               a palette index (older saves, and the defaults)
--   CM_RGB + 0xRRGGBB    any colour the player picked
-- The paint, the vehicle icons (mp_company_perms.txt, 5th field) and the Big Maps
-- minimap draw the exact colour. Station icons and foreign windows are styled by
-- CLASS (!mpCoN / !mpWinCoN, res/config/style_sheet/mp_lockstep.lua), and a class
-- cannot be made while the game runs: they draw the nearest of CM_CLASSES fixed
-- colours -- the palette (1..200) and the picker's grid (201..CM_CLASSES).
-- The palette: 20 distinct colours (Trubetskoy), then a golden-angle hue walk.
-- The same table is in the style sheet, the slice's icon tint and the Big Maps
-- minimap (tools/palette_sync_test.py keeps them in step).
CM.CM_COLORS = { {230,25,75}, {0,130,200}, {60,180,75}, {245,130,48}, {145,30,180}, {70,240,240}, {240,50,230}, {255,225,25}, {0,128,128}, {170,110,40}, {210,245,60}, {128,0,0}, {0,0,128}, {128,128,0}, {250,190,212}, {220,190,255}, {170,255,195}, {255,215,180}, {128,128,128}, {255,250,200} }
CM.CM_RGB = 16777216               -- 0x1000000: a colour at or above it is CM_RGB + 0xRRGGBB
-- the picker's grid: 24 hues x 5 shades, then 6 greys (the style sheet builds the
-- same classes from the same numbers; tools/company_color_test.py compares them)
CM.CM_GRID_HUES = 24
CM.CM_GRID_SHADES = { { 0.30, 1.00 }, { 0.60, 1.00 }, { 0.90, 0.95 }, { 0.95, 0.72 }, { 0.95, 0.48 } }
CM.CM_GRID_GREYS = { 240, 190, 140, 95, 55, 20 }
CM.CM_CLASSES = CM.CM_MAX + CM.CM_GRID_HUES * #CM.CM_GRID_SHADES + #CM.CM_GRID_GREYS
-- two colours closer than this (redmean distance, 0..~765) are one colour to the
-- eye: a company may not pick one that close to another company's. The 20 palette
-- colours are at least 68 apart; neighbouring pastel hues of the grid ~27..38.
CM.CM_COLOR_NEAR = 40
function CM.cmPaletteColor(idx)
	idx = tonumber(idx) or 1
	local c = CM.CM_COLORS[idx]
	if c then return c[1] / 255, c[2] / 255, c[3] / 255 end
	local h = ((idx - 21) * 137.508) % 360
	local sat, val = 0.62, 0.85
	local C = val * sat
	local X = C * (1 - math.abs((h / 60) % 2 - 1))
	local m = val - C
	local r, g, b
	if h < 60 then r, g, b = C, X, 0 elseif h < 120 then r, g, b = X, C, 0 elseif h < 180 then r, g, b = 0, C, X
	elseif h < 240 then r, g, b = 0, X, C elseif h < 300 then r, g, b = X, 0, C else r, g, b = C, 0, X end
	return r + m, g + m, b + m
end
local function byte255(x) return math.floor(x * 255 + 0.5) end
-- HSV (h in degrees, s and v 0..1) -> 0..255 integers
function CM.cmHsv(h, s, v)
	h = h % 360
	local C = v * s
	local X = C * (1 - math.abs((h / 60) % 2 - 1))
	local m = v - C
	local r, g, b
	if h < 60 then r, g, b = C, X, 0 elseif h < 120 then r, g, b = X, C, 0 elseif h < 180 then r, g, b = 0, C, X
	elseif h < 240 then r, g, b = 0, X, C elseif h < 300 then r, g, b = X, 0, C else r, g, b = C, 0, X end
	return byte255(r + m), byte255(g + m), byte255(b + m)
end
-- the class of hue column `hue` (1..24) and shade row `shade` (1..5); the greys: hue 0, shade 1..6
function CM.cmGridClass(hue, shade)
	if hue == 0 then return CM.CM_MAX + CM.CM_GRID_HUES * #CM.CM_GRID_SHADES + shade end
	return CM.CM_MAX + (hue - 1) * #CM.CM_GRID_SHADES + shade
end
-- a class's colour, 0..255 integers: the palette, then the grid
function CM.cmClassRGB(i)
	i = math.floor(tonumber(i) or 1)
	if i <= CM.CM_MAX then
		local r, g, b = CM.cmPaletteColor(math.max(1, i))
		return byte255(r), byte255(g), byte255(b)
	end
	local k = i - CM.CM_MAX - 1
	local ns = #CM.CM_GRID_SHADES
	if k < CM.CM_GRID_HUES * ns then
		local sh = CM.CM_GRID_SHADES[k % ns + 1]
		return CM.cmHsv(math.floor(k / ns) * (360 / CM.CM_GRID_HUES), sh[1], sh[2])
	end
	local grey = CM.CM_GRID_GREYS[k - CM.CM_GRID_HUES * ns + 1] or 128
	return grey, grey, grey
end
function CM.cmRgbValue(r, g, b)
	local function c(x) x = math.floor(tonumber(x) or 0); return x < 0 and 0 or (x > 255 and 255 or x) end
	return CM.CM_RGB + c(r) * 65536 + c(g) * 256 + c(b)
end
-- a stored colour -> 0..255 integers
function CM.cmColorRGB(v)
	v = tonumber(v) or 1
	if v >= CM.CM_RGB then
		local x = math.floor(v - CM.CM_RGB)
		return math.floor(x / 65536) % 256, math.floor(x / 256) % 256, x % 256
	end
	return CM.cmClassRGB(v)
end
-- "#RRGGBB" <-> a stored colour
function CM.cmColorHex(v) return string.format("#%02X%02X%02X", CM.cmColorRGB(v)) end
function CM.cmHexColor(s)
	local h = tostring(s or ""):match("^%s*#?(%x%x%x%x%x%x)%s*$")
	if not h then return nil end
	return CM.cmRgbValue(tonumber(h:sub(1, 2), 16), tonumber(h:sub(3, 4), 16), tonumber(h:sub(5, 6), 16))
end
-- how different two colours look (redmean), 0 = the same
function CM.cmColorDistance(v1, v2)
	local r1, g1, b1 = CM.cmColorRGB(v1)
	local r2, g2, b2 = CM.cmColorRGB(v2)
	local rm = (r1 + r2) / 2
	local dr, dg, db = r1 - r2, g1 - g2, b1 - b2
	return math.sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db)
end
-- the class that draws a stored colour: a palette index is its own class, an
-- exact colour the nearest one (cached: the same few colours are asked every tick)
CM.cmClassCache = {}
function CM.cmColorClass(v)
	v = tonumber(v) or 1
	if v < CM.CM_RGB then return CM.cmClampColor(v) or 1 end
	local hit = CM.cmClassCache[v]
	if hit then return hit end
	local best, bestD = 1, math.huge
	for i = 1, CM.CM_CLASSES do
		local d = CM.cmColorDistance(v, i)
		if d < bestD then best, bestD = i, d end
	end
	CM.cmClassCache[v] = best
	return best
end
-- a valid stored colour, else nil
function CM.cmClampColor(v)
	v = math.floor(tonumber(v) or 0)
	if v >= CM.CM_RGB and v <= CM.CM_RGB + 16777215 then return v end
	if v < 1 or v > CM.CM_MAX then return nil end
	return v
end
-- the colour a company draws with (its id's colour until it picks one)
function CM.cmColorOf(cid)
	local co = CM.cmCo(cid)
	return (co and CM.cmClampColor(co.color)) or CM.cmClampColor(cid) or 1
end
-- its colour as 0..1 floats (the paint, the cursors)
function CM.cmCompanyColor(cid)
	local r, g, b = CM.cmColorRGB(CM.cmColorOf(cid))
	return r / 255, g / 255, b / 255
end
-- the company (other than `except`) whose colour looks like colour v
function CM.cmColorHolder(v, except)
	for cid, co in pairs(CM.co.list) do
		if cid ~= except and co.color and CM.cmColorDistance(co.color, v) < CM.CM_COLOR_NEAR then return cid end
	end
	return nil
end
-- the first default colour no company's colour looks like (else a grid colour, else the id's own)
function CM.cmFreeColor(cid)
	for i = 1, CM.CM_PICK_COLORS do if not CM.cmColorHolder(i, cid) then return i end end
	for i = CM.CM_MAX + 1, CM.CM_CLASSES do if not CM.cmColorHolder(i, cid) then return CM.cmRgbValue(CM.cmClassRGB(i)) end end
	return CM.cmClampColor(cid) or 1
end

-- ---------- passwords ----------
-- The wire and every peer only ever see a salted hash; the clear text stays in
-- the inject file on the typist's disk. v2 hashes are salted with a constant (a
-- new company's id is not known when its password is typed); v1 hashes (saves
-- from before 2026-09-27) were salted with the company id and still verify.
function CM.cmHashRaw(sIn)
	local M1, A1, M2, A2 = 2147483647, 48271, 2147483629, 40692
	local h1, h2 = 2166136261 % M1, 2166136261 % M2
	for i = 1, #sIn do local b = sIn:byte(i); h1 = (h1 * A1 + b) % M1; h2 = (h2 * A2 + b) % M2 end
	return string.format("h%010d%010d", h1, h2)
end
function CM.cmHashPw(pw)
	pw = tostring(pw or "")
	if pw == "" or pw == "-" then return nil end
	return CM.cmHashRaw("tpf2mp-company:" .. pw)
end
function CM.cmHashPwV1(cid, pw)
	pw = tostring(pw or "")
	if pw == "" or pw == "-" then return nil end
	return CM.cmHashRaw("co" .. tostring(cid) .. ":" .. pw)
end
-- may command c (pw = v2 hash, pw1 = v1 hash) open company cid? true, or false + why
function CM.cmPwOk(c, cid)
	local co = CM.cmCo(cid)
	if not co or not co.pw then return true end
	local given = (co.pwv == 1) and c.pw1 or c.pw
	if given == nil or given == "" or given == "-" or given == 0 then return false, CM.cmNameOf(cid) .. " needs its password" end
	if tostring(given) ~= co.pw then return false, "wrong password for " .. CM.cmNameOf(cid) end
	return true
end

-- ---------- station permissions (2026-09-16) ----------
-- Which companies' vehicles may stop at a company's stations: co.open is nil
-- (everyone, the default), {} (nobody) or a set of company ids. The originator's
-- line editor asks the slice (mp_company_perms.txt); every instance re-checks the
-- LCREATE / LUPDATE it applies (lines.lua) against the registry.
function CM.cmStationOpen(ownerCid, userCid)
	ownerCid, userCid = tonumber(ownerCid), tonumber(userCid)
	if not ownerCid or not userCid or ownerCid == userCid then return true end
	local co = CM.cmCo(ownerCid)
	local set = co and co.open
	if set == nil then return true end
	return set[userCid] == true
end
function CM.cmOpenText(cid)
	local co = CM.cmCo(cid)
	local set = co and co.open
	if set == nil then return "everyone" end
	local names = {}
	for _, other in ipairs(CM.cmIds()) do
		if other ~= tonumber(cid) and set[other] then names[#names + 1] = CM.cmNameOf(other) end
	end
	if #names == 0 then return "nobody" end
	return table.concat(names, ", ")
end
-- the wire / dash / file form of one company's permission: "*", "-" or "1,3"
function CM.cmOpenCode(cid)
	local co = CM.cmCo(cid)
	local set = co and co.open
	if set == nil then return "*" end
	local ids = {}
	for other in pairs(set) do ids[#ids + 1] = other end
	table.sort(ids)
	if #ids == 0 then return "-" end
	local t = {}
	for i, v in ipairs(ids) do t[i] = tostring(v) end
	return table.concat(t, ",")
end
function CM.cmOpenFromCode(code)
	code = tostring(code or "*")
	if code == "*" then return nil end
	local set = {}
	if code ~= "-" then for v in code:gmatch("%d+") do set[tonumber(v)] = true end end
	return set
end
-- mp_company_perms.txt, for the slice: the player entity of every company, the
-- style class its station icons and windows draw (4th field) and its exact colour
-- (5th, RRGGBB hex: the vehicle icons), and what each company's stations are open
-- to. Rewritten only when its text changes.
function CM.cmWritePerms()
	if CM.cmMode ~= "companies" then return end
	local lines = {}
	local cids = {}
	for cid in pairs(CM.cmCompanyPid or {}) do if CM.cmCo(cid) then cids[#cids + 1] = cid end end
	table.sort(cids)
	for _, cid in ipairs(cids) do lines[#lines + 1] = string.format("pid %s %d %d %s", tostring(CM.cmCompanyPid[cid]), cid, CM.cmColorClass(CM.cmColorOf(cid)), CM.cmColorHex(CM.cmColorOf(cid)):sub(2)) end
	-- this game's own player entity: the Linux window wash is for other companies only
	local human = CM.cmHuman()
	if human then lines[#lines + 1] = "me " .. tostring(human) end
	for _, cid in ipairs(CM.cmIds()) do lines[#lines + 1] = string.format("open %d %s", cid, CM.cmOpenCode(cid)) end
	local text = table.concat(lines, string.char(10)) .. string.char(10)
	if text == CM.cmPermsWritten then return end
	local f = io.open(K.BASE .. "mp_company_perms.txt", "w")
	if f then f:write(text); f:close(); CM.cmPermsWritten = text end
end
-- Every instance checks a line's stops against the permissions before it
-- applies the create / update (lines.lua). Returns true, or false and why.
function CM.cmLineStopsPermitted(userCid, stationGroups)
	userCid = tonumber(userCid)
	if CM.cmMode ~= "companies" or not userCid then return true end
	for _, sg in ipairs(stationGroups or {}) do
		local owner = CM.cmOwnerOf(sg)
		if owner == nil then
			pcall(function()
				local gc = api.engine.getComponent(sg, api.type.ComponentType.STATION_GROUP)
				if gc and gc.stations and gc.stations[1] then owner = CM.cmOwnerOf(gc.stations[1]) end
			end)
		end
		local ownerCid = CM.cmCompanyOfPid(owner)
		if ownerCid and not CM.cmStationOpen(ownerCid, userCid) then
			return false, string.format("%s's stations are not open to %s", CM.cmNameOf(ownerCid), CM.cmNameOf(userCid))
		end
	end
	return true
end
-- mp_company_map.txt: this instance's company -> player entity -> name -> colour,
-- for other mods (the Big Maps minimap). Entity ids are this instance's own.
--   me=<my company id>
--   <cid>=<pid>=<name, percent-escaped>=<style class>=<RRGGBB>
function CM.cmWriteCompanyMap()
	if CM.cmMode ~= "companies" then
		if CM.cmMapWritten ~= "" then
			CM.cmMapWritten = ""
			local f = io.open(K.BASE .. "mp_company_map.txt", "w")
			if f then f:close() end
		end
		return
	end
	local lines = { "me=" .. tostring(CM.cmMyCompany or 1) }
	for _, cid in ipairs(CM.cmIds()) do
		local pid = CM.cmCompanyPid[cid]
		if pid then lines[#lines + 1] = cid .. "=" .. tostring(pid) .. "=" .. CM.escName(CM.cmNameOf(cid)) .. "=" .. CM.cmColorClass(CM.cmColorOf(cid)) .. "=" .. CM.cmColorHex(CM.cmColorOf(cid)):sub(2) end
	end
	local text = table.concat(lines, "\n") .. "\n"
	if text == CM.cmMapWritten then return end
	local f = io.open(K.BASE .. "mp_company_map.txt", "w")
	if not f then return end
	f:write(text); f:close()
	CM.cmMapWritten = text
end

-- ---------- ownership questions (originator-side guards) ----------
-- Is this player-owned entity another company's? true, its company, its owner pid.
-- Asked on the ORIGINATOR before an edit ships (inject.lua); a refusal there leaves
-- the entity untouched on every instance.
function CM.cmForeignOwner(eid)
	CM.cmEnsure()
	if CM.cmMode ~= "companies" or not eid then return false end
	local owner = CM.cmOwnerOf(eid)
	if not owner then return false end
	local mine = CM.cmCompanyPid[CM.cmMyCompany] or CM.cmHuman()
	if owner == mine then return false end
	return true, CM.cmCompanyOfPid(owner), owner
end
-- the object holding one side of an edge, if another company owns it: its id and company
function CM.cmStopSideForeign(eid, side)
	if side == 2 or not CM.objectsOnEdge then return nil end
	local objs = CM.objectsOnEdge(eid)
	for _, ob in ipairs(objs or {}) do
		if ob[2] == side then
			local foreign, cid = CM.cmForeignOwner(ob[1])
			if foreign then return ob[1], cid end
		end
	end
	return nil
end

-- ---------- money ----------
-- A remote company's action paid from OUR wallet (built as our human) is moved
-- back: refund us, charge the company. bookJournalEntry to a specific pid is the
-- proven mechanism; chunked because TpF2 silently drops very large single amounts.
CM.CM_JOURNAL_CHUNK = 10000000
function CM.cmBookJournal(pid, amount, jtype)
	if not pid or not amount or amount == 0 then return true end
	jtype = jtype or K.JOURNAL_TRANSFER
	amount = math.floor(amount + 0.5)
	local sign = amount >= 0 and 1 or -1
	local remaining = math.abs(amount)
	local okAll, errAll = true, nil
	local chunks = 0
	while remaining > 0 and chunks < 1000 do
		local chunk = math.min(remaining, CM.CM_JOURNAL_CHUNK)
		local ok, err = pcall(function()
			local cat = api.type.JournalEntryCategory.new(); cat.type = jtype
			local entry = api.type.JournalEntry.new()
			entry.amount = sign * chunk; entry.category = cat; entry.time = -1
			api.cmd.sendCommand(api.cmd.make.bookJournalEntry(pid, entry, api.type.Vec3f.new(0, 0, 0)))
		end)
		if not ok then okAll, errAll = false, err end
		remaining = remaining - chunk; chunks = chunks + 1
	end
	return okAll, errAll
end
function CM.cmBalance(pid)
	-- getEntity(nil) is an ENGINE assert (an ~800 KB minidump each); guard.
	if type(pid) ~= "number" or pid < 0 then return nil end
	local bal = nil
	pcall(function() local e = game.interface.getEntity(pid); if e and e.balance then bal = e.balance end end)
	return bal
end
-- A company's wallet is its balance AND its loan. Type-0 journal entries move the
-- loan (and the balance with it), type-6 entries the balance only (measured 2026-09-01).
function CM.cmWallet(pid)
	local b, l = nil, nil
	if type(pid) ~= "number" or pid < 0 then return nil end
	pcall(function() local e = game.interface.getEntity(pid); if e then b = e.balance or 0; l = e.loan or 0 end end)
	return b, l
end
-- set pid's wallet to (bal, loan) from (b0, l0). ORDER MATTERS (2026-09-10): a loan
-- entry that REPAYS needs the cash on hand, or the engine refuses it and the
-- player keeps the old loan AND gets the balance. Borrowing first; a repayment
-- gets the cash it needs moved in before it.
function CM.cmSetWallet(pid, b0, l0, bal, loan)
	local dl = loan - l0
	if dl < 0 then
		local need = -dl - b0
		if need > 0 then
			need = need + 1
			CM.cmBookJournal(pid, need, K.JOURNAL_TRANSFER or 6)
		else
			need = 0
		end
		CM.cmBookJournal(pid, dl, K.JOURNAL_LOAN or 0)
		local db = bal - (b0 + need + dl)
		if db ~= 0 then CM.cmBookJournal(pid, db, K.JOURNAL_TRANSFER or 6) end
	else
		if dl > 0 then CM.cmBookJournal(pid, dl, K.JOURNAL_LOAN or 0) end
		local db = bal - (b0 + dl)
		if db ~= 0 then CM.cmBookJournal(pid, db, K.JOURNAL_TRANSFER or 6) end
	end
end
function CM.cmSwapWallets(p1, p2)
	local b1, l1 = CM.cmWallet(p1); local b2, l2 = CM.cmWallet(p2)
	if not b1 or not b2 then return false end
	CM.cmSetWallet(p1, b1, l1, b2, l2)
	CM.cmSetWallet(p2, b2, l2, b1, l1)
	return true, b1, l1, b2, l2
end
-- Transfer `cost` from company cid to us (refund local, charge the company).
function CM.cmTransferCost(cid, cost, what)
	if CM.cmMode ~= "companies" or not cid or cid == CM.cmMyCompany then return end
	if not cost or cost <= 0 then return end
	local mePid, theirPid = CM.cmCompanyPid[CM.cmMyCompany], CM.cmCompanyPid[cid]
	if not mePid or not theirPid then CM.cmLog("CM: cost transfer: missing pid (me=" .. tostring(mePid) .. " co" .. tostring(cid) .. "=" .. tostring(theirPid) .. ")"); return end
	local ok1, e1 = CM.cmBookJournal(mePid, cost)
	local ok2, e2 = CM.cmBookJournal(theirPid, -cost)
	CM.cmLog(string.format("CM: cost %s: moved %d from co%d(pid %s) to me(pid %s) | refund ok=%s %s | charge ok=%s %s",
		tostring(what), cost, cid, tostring(theirPid), tostring(mePid), tostring(ok1), tostring(e1 or ""), tostring(ok2), tostring(e2 or "")))
end

-- ---------- ownership handover ----------
-- Generic reassign for any entity type (vehicles, lines): setPlayer only.
function CM.cmReassignEntity(eid, cid, kind)
	CM.cmEnsure()
	if CM.cmMode ~= "companies" or not eid or not cid or cid == CM.cmMyCompany then return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	if before == pid then return end
	local ok, err = pcall(function() CM.cmSetPlayer(eid, pid) end)
	CM.cmLog(string.format("CM: reassigned %s eid=%s -> co%d pid=%s | owner before=%s after=%s | ok=%s err=%s",
		tostring(kind), tostring(eid), cid, tostring(pid), tostring(before), tostring(CM.cmOwnerOf(eid)), tostring(ok), tostring(err)))
end
-- A replicated construction landed owned by our player: hand it to its company and
-- lock it against other companies' bulldozers. setBulldozeable only on entities
-- with a CONSTRUCTION component (anything else is an uncatchable engine assert).
function CM.cmReassignConstruction(eid, cid)
	CM.cmEnsure()
	if CM.cmMode ~= "companies" or not eid or not cid or cid == CM.cmMyCompany then return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	local hasCon = false
	pcall(function() hasCon = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) ~= nil end)
	local okSP, errSP = pcall(function() CM.cmSetConstructionPlayer(eid, pid) end)
	local okBZ, errBZ = true, nil
	if hasCon then okBZ, errBZ = pcall(function() game.interface.setBulldozeable(eid, false) end) end
	CM.cmLog(string.format("CM: reassigned construction eid=%s -> co%d pid=%s | owner before=%s after=%s | setPlayer ok=%s %s | bulldozeable ok=%s %s",
		tostring(eid), cid, tostring(pid), tostring(before), tostring(CM.cmOwnerOf(eid)), tostring(okSP), tostring(errSP or ""), tostring(okBZ), tostring(errBZ or "")))
end
-- A replayed road/rail proposal built as OUR player: hand the entities to the
-- company and move the proposal's own cost figure.
function CM.cmSettleBuild(c, res, success, what)
	if not success or not c or not c.company then return end
	CM.cmEnsure()
	if CM.cmMode ~= "companies" then return end
	local cid = tonumber(c.company)
	if not cid or cid == CM.cmMyCompany then return end
	local cost = nil
	pcall(function() cost = res and res.resultProposalData and res.resultProposalData.costs end)
	cost = tonumber(cost)
	pcall(function()
		if (what == "ROAD" or what == "RAIL") and c.x0 and c.x1 then
			-- resultEntities is empty for a street/track proposal: find the edge by its ends
			local eid = CM.findEdgeByEnds(what == "RAIL", c.x0, c.y0, c.x1, c.y1, 2.0)
			if eid and CM.cmOwnerOf(eid) then CM.cmReassignEntity(eid, cid, what) end
		else
			local ents = res and res.resultEntities
			for i = 1, #(ents or {}) do
				local eid = ents[i]
				if eid and eid > 0 and CM.cmOwnerOf(eid) then CM.cmReassignEntity(eid, cid, what) end
			end
		end
	end)
	if cost and cost > 0 then CM.cmTransferCost(cid, cost, what) end
end
CM.CM_OWNED_TYPES = { "CONSTRUCTION", "VEHICLE", "LINE", "BASE_EDGE", "BASE_NODE", "STATION_GROUP", "STATION", "SIGNAL" }
function CM.cmOwnedEntities(pid)
	local out, seen = {}, {}
	local function take(eid)
		if type(eid) == "number" and eid > 0 and not seen[eid] then
			seen[eid] = true
			if CM.cmOwnerOf(eid) == pid then out[#out + 1] = eid end
		end
	end
	for _, kind in ipairs(CM.CM_OWNED_TYPES) do
		pcall(function()
			local t = game.interface.getEntities({ radius = 999999 }, { type = kind, includeData = false }) or {}
			for _, eid in pairs(t) do take(eid) end
		end)
	end
	-- LINES have no position: the line system is their enumerator
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do take(ls[i]) end
	end)
	-- PARKED vehicles are not world entities: only the vehicle system sees them
	pcall(function()
		local v = api.engine.system.transportVehicleSystem.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
		for i = 1, #v do take(v[i]) end
	end)
	return out
end
function CM.cmHandOver(list, to)
	for _, eid in ipairs(list) do
		local ok, err = pcall(CM.cmSetPlayer, eid, to)
		if not ok then CM.cmLog("CM: asset handover failed: " .. tostring(err)) end
	end
end
-- everything fromPid owns goes to toPid; constructions stay bulldozeable only for the human
function CM.cmMoveAssets(fromPid, toPid, why)
	if not fromPid or not toPid or fromPid == toPid then return 0 end
	local ents = CM.cmOwnedEntities(fromPid)
	local human = CM.cmHuman()
	local n = 0
	for _, eid in ipairs(ents) do
		local ok = pcall(function() CM.cmSetPlayer(eid, toPid) end)
		if ok then
			n = n + 1
			local hasCon = false
			pcall(function() hasCon = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) ~= nil end)
			if hasCon then pcall(function() game.interface.setBulldozeable(eid, toPid == human) end) end
		end
	end
	CM.cmLog(string.format("CM: %s: moved %d/%d entities pid %s -> %s", tostring(why), n, #ents, tostring(fromPid), tostring(toPid)))
	return n
end
-- A LINE BELONGS WITH ITS VEHICLES: a line whose vehicles all belong to one
-- company, and which another company owns, is re-owned to the vehicles' company.
function CM.cmRepairLineOwners(why)
	if CM.cmMode ~= "companies" then return end
	local ids = {}
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do ids[#ids + 1] = ls[i] end
	end)
	local vehOfLine = {}
	local function scan(list)
		for i = 1, #list do
			local v = list[i]
			pcall(function()
				local tv = api.engine.getComponent(v, api.type.ComponentType.TRANSPORT_VEHICLE)
				if tv and tv.line and tv.line > 0 then
					vehOfLine[tv.line] = vehOfLine[tv.line] or {}
					table.insert(vehOfLine[tv.line], CM.cmOwnerOf(v))
				end
			end)
		end
	end
	pcall(function()
		local t = game.interface.getEntities({ radius = 999999 }, { type = "VEHICLE", includeData = false }) or {}
		local list = {}
		for _, e in pairs(t) do list[#list + 1] = e end
		scan(list)
	end)
	pcall(function()
		local v = api.engine.system.transportVehicleSystem.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
		local list = {}
		for i = 1, #v do list[#list + 1] = v[i] end
		scan(list)
	end)
	local known = {}
	for cid, pid in pairs(CM.cmCompanyPid or {}) do known[pid] = cid end
	local fixed = 0
	for _, lid in ipairs(ids) do
		local owners = vehOfLine[lid]
		if owners and #owners > 0 then
			local want = owners[1]
			for _, o in ipairs(owners) do if o ~= want then want = nil; break end end
			local have = CM.cmOwnerOf(lid)
			if want and known[want] and have ~= want then
				local ok = pcall(function() CM.cmSetPlayer(lid, want) end)
				fixed = fixed + 1
				CM.cmLog(string.format("CM: line %d owned by pid %s but its %d vehicle(s) belong to co%d -- re-owned (%s) ok=%s",
					lid, tostring(have), #owners, known[want], tostring(why), tostring(ok)))
			end
		end
	end
	if fixed > 0 then CM.cmNote(string.format("%d line(s) re-owned to their vehicles' company (%s)", fixed, why)) end
end
-- once a switch has settled, lines follow their vehicles
function CM.cmVehRecheck()
	if CM.cmRepairAt and (CM.ticks or 0) >= CM.cmRepairAt then
		CM.cmRepairAt = nil
		pcall(CM.cmRepairLineOwners, "after the switch")
	end
end

-- ---------- engine player entities ----------
-- Every company this machine does not play needs its own engine player entity.
-- Company 1 of a world that has no company record yet is the world's own player
-- (it holds the starting money on every machine alike); every other new company
-- is an addPlayer() entity, which starts empty.
function CM.cmEnsurePids()
	local human = CM.cmHuman()
	for _, cid in ipairs(CM.cmIds()) do
		if not CM.cmCompanyPid[cid] then
			local bound = false
			for _, p in pairs(CM.cmCompanyPid) do if p == human then bound = true end end
			CM.cmWorldBound = CM.cmWorldBound or {}
			if cid == 1 and CM.cmReserved1 then
				-- this machine switched into another company before company 1 existed:
				-- the entity that took over the world player's assets and money is company 1
				CM.cmCompanyPid[cid] = CM.cmReserved1
				CM.cmWorldBound[cid] = true
				CM.cmLog(string.format("CM: company 1 -> player %s (the world's player's assets, moved there by an earlier switch)", tostring(CM.cmReserved1)))
				CM.cmReserved1 = nil
			elseif cid == 1 and human and not bound then
				CM.cmCompanyPid[cid] = human
				CM.cmMyCompany = cid
				CM.cmWorldBound[cid] = true
				CM.cmLog(string.format("CM: company 1 -> the world's player %s", tostring(human)))
			else
				local pid = nil
				pcall(function() pid = game.interface.addPlayer() end)
				if pid then
					CM.cmCompanyPid[cid] = pid
					pcall(function() game.interface.setMaximumLoan(pid, 100000000) end)
					CM.cmLog(string.format("CM: company %d -> player %s", cid, tostring(pid)))
				end
			end
		end
	end
end
-- The game's finances / company windows show the player ENTITY's name. Keep
-- every company's entity named after the company (make.setName from Lua is not
-- shipped by the slice, so nothing echoes).
function CM.cmApplyNames()
	if CM.cmMode ~= "companies" then return end
	for _, cid in ipairs(CM.cmIds()) do
		local pid = CM.cmCompanyPid[cid]
		if pid then
			local want = CM.cmNameOf(cid)
			local have = nil
			pcall(function()
				local nc = api.engine.getComponent(pid, api.type.ComponentType.NAME)
				if nc and type(nc.name) == "string" then have = nc.name end
			end)
			if have ~= want then
				pcall(function() api.cmd.sendCommand(api.cmd.make.setName(pid, want)) end)
			end
		end
	end
end
-- Does this world answer entity queries yet? A world a tick or two out of its
-- load does not, and a switch then moved "0 + 0 entities" (2026-09-22).
function CM.cmWorldAnswers()
	local n = 0
	pcall(function()
		local t = game.interface.getEntities({ radius = 999999 }, { type = "CONSTRUCTION", includeData = false }) or {}
		for _ in pairs(t) do n = n + 1; break end
	end)
	if n > 0 then return true end
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines() or {}
		if #ls > 0 then n = 1 end
	end)
	return n > 0
end
-- The hotseat on THIS machine: everything the human entity owns goes to company
-- cid's entity, cid's assets and wallet come to the human, and the map swaps.
-- Local representation only: what each company owns in the world does not change.
function CM.cmLocalSwitch(cid)
	local old = CM.cmMyCompany
	if cid == old then return true end
	local human, other = CM.cmHuman(), CM.cmCompanyPid[cid]
	if not human or not other then CM.cmNote("switch: company " .. tostring(cid) .. " has no player entity here"); return false end
	local mine = CM.cmOwnedEntities(human)
	local theirs = CM.cmOwnedEntities(other)
	CM.cmHandOver(mine, other)
	CM.cmHandOver(theirs, human)
	for _, eid in ipairs(mine) do pcall(function() if api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) then game.interface.setBulldozeable(eid, false) end end) end
	for _, eid in ipairs(theirs) do pcall(function() if api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) then game.interface.setBulldozeable(eid, true) end end) end
	local okW, bh, lh, bo, lo = CM.cmSwapWallets(human, other)
	-- the loan poll must not ship the swapped loan as the player's own
	if okW then CM.loanExpect = lo; CM.loanExpectSince = CM.ticks end
	if old then CM.cmCompanyPid[old] = other
	else
		-- the world's player belonged to no company yet: what it had (the starting
		-- money) is company 1's on every machine, and now sits on `other`
		CM.cmReserved1 = other
		if CM.cmCo(1) and not CM.cmCompanyPid[1] then CM.cmEnsurePids() end
	end
	CM.cmCompanyPid[cid] = human
	CM.cmMyCompany = cid
	CM.cmRepairAt = (CM.ticks or 0) + 25
	CM.cmNote(string.format("you now play %s (%d + %d entities moved; wallet %s/%s <-> %s/%s%s)", CM.cmNameOf(cid), #mine, #theirs,
		tostring(bh), tostring(lh), tostring(bo), tostring(lo), okW and "" or " -- wallet swap FAILED"))
	CM.cmApplyNames()
	pcall(CM.cmWritePerms)
	return true
end
CM.CM_SWITCH_WAIT_TICKS = 3600      -- about a minute: then switch anyway and say so
function CM.cmLoadSwitchTick()
	local want = CM.cmSwitchWanted
	if not want then return end
	if want == CM.cmMyCompany or not CM.cmCo(want) then CM.cmSwitchWanted = nil; return end
	CM.cmSwitchTries = (CM.cmSwitchTries or 0) + 1
	if not CM.cmWorldAnswers() then
		if CM.cmSwitchTries == 1 or CM.cmSwitchTries % 60 == 0 then
			CM.cmLog(string.format("CM: the switch to co%d waits for a world that answers entity queries (try %d)", want, CM.cmSwitchTries))
		end
		if CM.cmSwitchTries < CM.CM_SWITCH_WAIT_TICKS then return end
		CM.cmLog(string.format("CM: the world answered nothing for %d tries -- switching to co%d anyway", CM.cmSwitchTries, want))
	end
	CM.cmSwitchWanted = nil
	CM.cmLocalSwitch(want)
end
-- this machine should play cid: now, or as soon as the world answers
function CM.cmWantSwitch(cid)
	if cid == CM.cmMyCompany then CM.cmSwitchWanted = nil; return end
	CM.cmSwitchWanted = cid
	CM.cmSwitchTries = 0
	CM.cmLoadSwitchTick()
end

-- ---------- the save ----------
-- The registry travels in the save, with this machine's entity map (valid in the
-- world the save holds) and the company its human entity plays (`mine`).
function CM.cmSaveState()
	if not CM.cmBooted then
		-- not applied in this session (single player, a machine whose player entity
		-- did not match): keep the record this world was loaded with, word for word
		if CM.cmCarried then return CM.cmCarried end
		return nil
	end
	local co = CM.co
	local st = { v = 2, mode = co.mode, mine = CM.cmMyCompany, nextId = co.nextId,
	             list = {}, members = {}, names = {}, origin = {}, pid = {}, founded = {},
	             originName = {} }
	for cid, c in pairs(co.list) do
		st.list[tostring(cid)] = { name = c.name, color = c.color, paint = c.paint and 1 or 0, pw = c.pw, pwv = c.pwv,
		                           founder = c.founder, fname = c.fname, ord = c.ord,
		                           open = (c.open ~= nil) and CM.cmOpenCode(cid) or nil }
	end
	for k, cid in pairs(co.members) do st.members[k] = cid end
	for k, n in pairs(co.names) do st.names[k] = n end
	for o, k in pairs(co.origin) do st.origin[o] = k; st.originName[o] = CM.playerNames[o] end
	for k, n in pairs(co.founded) do st.founded[k] = n end
	for cid, pid in pairs(CM.cmCompanyPid) do st.pid[tostring(cid)] = pid end
	if co.legacy then st.legacy = co.legacy end
	if co.start then st.start = { l = co.start.l } end
	st.reserved1 = CM.cmReserved1
	return st
end
function CM.cmLoadState(st)
	if type(st) ~= "table" then return end
	if st.mode == "companies" or tonumber(st.v) == 2 then
		CM.cmSaved = st; CM.cmCarried = st
		-- a record that arrives after something already booted a fresh registry
		-- (before any company command ran here) replaces it
		if CM.cmBooted and not CM.cmAnyExec and not CM.cmJoinAsked then
			CM.cmBooted, CM.cmLive, CM.cmBootRefused = false, false, nil
			CM.co, CM.cmCompanyPid, CM.cmMyCompany = CM.cmEmptyRegistry(), {}, nil
		end
	end
end
-- v1 (before 2026-09-27) -> v2. v1 kept `origin` as lobby letter -> company, and
-- letters belong to one lobby: whose company was whose is only known for the
-- saver (`mine`, the company its human entity played). The companies come across
-- as they were (their id's colour, vehicle paint on, passwords as v1 hashes);
-- the players are claimed as they join (CM.cmClaimLegacy).
function CM.cmMigrate(sv)
	if tonumber(sv.v) == 2 then return sv end
	local st = { v = 2, mode = "companies", mine = tonumber(sv.mine), list = {}, members = {}, names = {}, origin = {},
	             pid = {}, founded = {}, originName = {} }
	local maxId = 0
	for _, cid in ipairs(sv.roster or {}) do
		cid = tonumber(cid)
		if cid then
			maxId = math.max(maxId, cid)
			local f = type(sv.founded) == "table" and sv.founded[tostring(cid)] or nil
			local fname = type(f) == "table" and type(f.nm) == "string" and f.nm ~= "" and f.nm or nil
			st.list[tostring(cid)] = { name = type(sv.names) == "table" and sv.names[tostring(cid)] or nil,
				color = CM.cmClampColor(cid), paint = 1,
				pw = type(sv.pw) == "table" and sv.pw[tostring(cid)] or nil, pwv = 1,
				fname = fname, ord = type(f) == "table" and tonumber(f.n) or nil,
				open = type(sv.open) == "table" and sv.open[tostring(cid)] or nil }
		end
	end
	st.nextId = maxId + 1
	for k, pid in pairs(sv.pid or {}) do st.pid[tostring(k)] = pid end
	local letters = {}
	for o, cid in pairs(sv.origin or {}) do letters[o] = tonumber(cid) end
	st.legacy = { letters = letters, mine = tonumber(sv.mine) }
	-- the names a save of the 2026-09-27 hotfix kept for its letters
	if type(sv.who) == "table" then
		for o, n in pairs(sv.who) do
			local cid = letters[o]
			if cid and type(n) == "string" and n ~= "" then st.members["n:" .. n] = cid; st.names["n:" .. n] = n end
		end
	end
	return st
end
-- Apply the save's record, once per load, before this game joins. Every machine
-- loading the same save and the same lobby roster ends up with the same registry.
function CM.cmApplySaved(sv)
	local human = CM.cmHuman()
	sv = CM.cmMigrate(sv)
	local savedHuman = sv.pid and sv.mine and sv.pid[tostring(sv.mine)]
	if savedHuman and human and savedHuman ~= human then
		CM.cmLog(string.format("CM: saved companies ignored -- this world's player entity is %s, the record was written for %s (kept for the next save)", tostring(human), tostring(savedHuman)))
		return false
	end
	local co = CM.cmEmptyRegistry()
	co.mode = sv.mode == "companies" and "companies" or "coop"
	for k, c in pairs(sv.list or {}) do
		local cid = tonumber(k)
		if cid and type(c) == "table" then
			co.list[cid] = { name = type(c.name) == "string" and c.name ~= "" and c.name or nil,
				color = CM.cmClampColor(c.color) or CM.cmClampColor(cid) or 1,
				paint = tonumber(c.paint) ~= 0, pw = type(c.pw) == "string" and c.pw ~= "" and c.pw or nil,
				pwv = tonumber(c.pwv) == 1 and 1 or 2,
				founder = type(c.founder) == "string" and c.founder or nil,
				fname = type(c.fname) == "string" and c.fname or nil, ord = tonumber(c.ord),
				open = c.open ~= nil and CM.cmOpenFromCode(c.open) or nil }
		end
	end
	for k, cid in pairs(sv.members or {}) do if co.list[tonumber(cid)] then co.members[k] = tonumber(cid) end end
	for k, n in pairs(sv.names or {}) do if type(n) == "string" then co.names[k] = n end end
	for k, n in pairs(sv.founded or {}) do co.founded[k] = tonumber(n) or 0 end
	-- this session's letters: kept only where the lobby still puts the same player
	-- on that letter (a hot join into the session that saved); a new session starts
	-- empty and every game says who it is in its CMJOIN
	pcall(CM.readPlayerNames)
	for o, key in pairs(sv.origin or {}) do
		local was = type(sv.originName) == "table" and sv.originName[o] or nil
		if was and CM.playerNames[o] == was then co.origin[o] = key end
	end
	local maxId = 0
	for cid in pairs(co.list) do maxId = math.max(maxId, cid) end
	co.nextId = math.max(tonumber(sv.nextId) or 1, maxId + 1)
	if type(sv.legacy) == "table" then co.legacy = sv.legacy end
	if type(sv.start) == "table" and tonumber(sv.start.l) then co.start = { l = tonumber(sv.start.l) } end
	CM.co = co
	CM.cmCompanyPid = {}
	for k, pid in pairs(sv.pid or {}) do
		local cid = tonumber(k)
		if cid and co.list[cid] and CM.cmPidAlive(pid) then CM.cmCompanyPid[cid] = pid end
	end
	CM.cmMyCompany = tonumber(sv.mine)
	if CM.cmMyCompany and not co.list[CM.cmMyCompany] then CM.cmMyCompany = nil end
	CM.cmReserved1 = CM.cmPidAlive(sv.reserved1) and sv.reserved1 or nil
	if co.legacy then
		CM.cmMigrated = "This save is from an older version: companies are handed out once as players join. If you got the wrong one, switch to yours."
	end
	CM.cmLog(string.format("CM: companies restored from the save (v%s): %d companies, mode %s, this world's player plays co%s%s",
		tostring(sv.v), #CM.cmIds(), co.mode, tostring(CM.cmMyCompany), co.legacy and " -- imported from an older save" or ""))
	return true
end
-- Once per load: the save's record, else a fresh registry. The lobby's file
-- (mp_company_cfg.txt) only says what THIS game asks for in its CMJOIN.
function CM.cmBoot()
	if CM.cmBooted then return end
	CM.cmBooted = true
	CM.cmLive = true
	local sv = CM.cmSaved
	CM.cmSaved = nil
	if sv then
		if CM.cmApplySaved(sv) then CM.cmCarried = nil
		else CM.cmBooted = false; CM.cmLive = false; CM.cmBootRefused = true end
	end
	CM.cmRefreshDerived()
	CM.cmEnsurePids()
end
-- the lobby file: line 1 mode, line 2 my company chip
function CM.cmLobbyAsk()
	local f = io.open(CM.CM_CFG_FILE, "r")
	if not f then return "coop", 1 end
	local mode = f:read("*l"); local mine = f:read("*l")
	f:close()
	mode = mode and mode:gsub("%s", "") or "coop"
	return mode == "companies" and "companies" or "coop", tonumber(mine) or 1
end

-- Everything that touches companies calls this first. Cheap once booted.
function CM.cmEnsure()
	if CM.cmBootRefused then return end
	if not CM.cmBooted then CM.cmBoot() end
end

-- ---------- joining ----------
-- Our CMJOIN goes out once this game is live in the session (not while it loads
-- or catches up: a command stamped from behind lands LATE and out of order). It
-- is sent again if the registry ever forgets our letter (a spurious CMLEAVE).
-- A dedicated server's game is nobody's: it never joins and never holds anything.
function CM.cmNoPlayer()
	return CM.dedicatedPauseEmpty ~= nil and CM.dedicatedPauseEmpty() == true
end
function CM.cmJoinTick(live)
	if CM.cmBootRefused or not CM.cmBooted or not K.INSTANCE or CM.cmNoPlayer() then return end
	-- joined: our letter is in the registry (under our key, or "<key>|<name>" when
	-- another game on this Steam account came first -- not the plain key then)
	if CM.cmJoinAsked and CM.co.origin[K.INSTANCE] ~= nil then return end
	if CM.cmJoinAsked and (CM.ticks or 0) - (CM.cmJoinAskedAt or 0) < 600 then return end
	if not live then return end
	local lm, want = CM.cmLobbyAsk()
	local key, name = CM.cmMyKey(), CM.cmMyName()
	CM.cmJoinAsked, CM.cmJoinAskedAt = true, CM.ticks or 0
	CM.scheduleLocal("CMJOIN", { key = CM.escName(key), name = CM.escName(name), want = want, lm = lm })
	CM.cmLog(string.format("CM: joining as %s (%s), lobby asks for %s company %d", key, name, lm, want))
end
-- Our own actions wait until our CMJOIN has landed and our switch is done: before
-- that nothing knows which company they are for. Only with a peer (a solo world
-- has no one to agree with) and never for longer than CM_JOIN_HOLD_TICKS.
function CM.cmHoldActions()
	if not CM.peerSeen or CM.cmBootRefused or CM.cmNoPlayer() then return false end
	if CM.cmJoined and not CM.cmSwitchWanted then return false end
	-- the clock runs from our CMJOIN going out, not while this game is still
	-- loading or catching up (its actions are off then anyway)
	if not CM.cmJoinAsked then return true end
	CM.cmHoldSince = CM.cmHoldSince or (CM.ticks or 0)
	if (CM.ticks or 0) - CM.cmHoldSince > CM.CM_JOIN_HOLD_TICKS then
		if not CM.cmHoldGaveUp then CM.cmHoldGaveUp = true; CM.cmLog("CM: our join has not landed in 30 s -- actions go out anyway") end
		return false
	end
	return true
end
-- Letters that left the lobby: any game that notices sends CMLEAVE (idempotent).
function CM.cmLeaveTick()
	if not CM.cmBooted or not CM.peerSeen then return end
	pcall(CM.readPlayerNames)
	if next(CM.playerNames) == nil then return end
	local now = os.time()
	CM.cmGoneSince = CM.cmGoneSince or {}
	for o in pairs(CM.co.origin) do
		if o ~= K.INSTANCE and not CM.playerNames[o] then
			CM.cmGoneSince[o] = CM.cmGoneSince[o] or now
			if now - CM.cmGoneSince[o] >= CM.CM_LEAVE_GRACE and not (CM.cmLeaveSent or {})[o] then
				CM.cmLeaveSent = CM.cmLeaveSent or {}
				CM.cmLeaveSent[o] = true
				CM.scheduleLocal("CMLEAVE", { who = o })
			end
		else
			CM.cmGoneSince[o] = nil
		end
	end
end
-- The company a command belongs to is decided AT ITS STAMP: its origin's company
-- in the registry as it stands then (the originator's own stamp is only the
-- fallback for an origin that has not joined). Called by execute in lockstep.lua.
function CM.cmAttribute(c)
	if c.cmAttributed then return end
	c.cmAttributed = true
	if CM.cmMode ~= "companies" or not c.origin then return end
	local cid = CM.cmCompanyOfOrigin(c.origin)
	if cid then
		if c.company and tonumber(c.company) ~= cid then
			CM.cmLog(string.format("CM: %s seq=%s from %s stamped co%s, the registry says co%d", tostring(c.op), tostring(c.seq), tostring(c.origin), tostring(c.company), cid))
		end
		c.company = cid
	end
end

-- ---------- company commands, at their stamp ----------
-- STARTING CAPITAL (2026-09-27). Company 1 of a new world is the world's player and
-- has the game's starting money; every other company was an empty addPlayer()
-- entity ("take a loan to fund it"). Now the world player's LOAN is remembered at
-- the stamp the first company is created -- before any switch, so every machine
-- reads the same untouched entity -- and every later company starts with a loan of
-- that size (balance and loan alike, a type-0 journal entry), the way a new game
-- starts. A world whose player had no loan then gives new companies nothing.
function CM.cmNoteStart()
	local co = CM.co
	if co.start or next(co.list) ~= nil then return end
	local _, l = CM.cmWallet(CM.cmHuman())
	l = math.floor((tonumber(l) or 0) + 0.5)
	co.start = { l = l }
	CM.cmLog(string.format("CM: starting capital for new companies: a loan of %d (the world player's)", l))
end
function CM.cmGrantStart(cid)
	local st = CM.co.start
	local pid = CM.cmCompanyPid[cid]
	if not st or not pid or (st.l or 0) <= 0 or (CM.cmWorldBound or {})[cid] then return end
	CM.cmBookJournal(pid, st.l, K.JOURNAL_LOAN or 0)
	CM.cmLog(string.format("CM: co%d starts with a loan of %d", cid, st.l))
end
function CM.cmCreate(cid, founderKey, fields)
	fields = fields or {}
	local co = CM.co
	CM.cmNoteStart()
	local ord = nil
	if founderKey then co.founded[founderKey] = (co.founded[founderKey] or 0) + 1; ord = co.founded[founderKey] end
	-- one colour per company: a colour another company uses gives way to a free one
	local color = CM.cmClampColor(fields.color)
	if color and CM.cmColorHolder(color, cid) then color = nil end
	co.list[cid] = { name = fields.name, color = color or CM.cmFreeColor(cid),
	                 paint = fields.paint ~= false, pw = fields.pw, pwv = 2,
	                 founder = founderKey, fname = founderKey and co.names[founderKey] or nil, ord = ord }
	co.nextId = math.max(co.nextId or 1, cid + 1)
	CM.cmRefreshDerived()
	CM.cmEnsurePids()
	CM.cmGrantStart(cid)
	return cid
end
-- a new company's id: counting up (a deleted company's id is not handed out again
-- at once), then from the bottom once the count passes CM_MAX -- the slice's
-- station gate and the style sheet stop at 200
function CM.cmNewId()
	local id = math.max(CM.co.nextId or 1, 1)
	while id <= CM.CM_MAX and CM.co.list[id] do id = id + 1 end
	if id <= CM.CM_MAX then return id end
	for i = 1, CM.CM_MAX do if not CM.co.list[i] then return i end end
	return nil
end
-- An older save's players, claimed as they join (v1 kept letters, not players):
-- the lobby host (a) takes the company the save's own player played (a machine
-- saves its own hotseat, and the host loads its own save); anyone else first the
-- company their letter had, unless that is the host's, then the lowest company
-- nobody has claimed.
function CM.cmClaimLegacy(o)
	local lg = CM.co.legacy
	if not lg then return nil end
	local claimed = {}
	for _, cid in pairs(CM.co.members) do claimed[cid] = true end
	local function free(cid) return cid and CM.co.list[cid] and not claimed[cid] end
	if o == "a" and free(lg.mine) then return lg.mine end
	local reserved = lg.mine
	local mine = lg.letters and lg.letters[o]
	if free(mine) and mine ~= reserved then return mine end
	for _, cid in ipairs(CM.cmIds()) do if free(cid) and cid ~= reserved then return cid end end
	return nil
end
function CM.cmExecJoin(c, o)
	local co = CM.co
	local key = CM.unescName(tostring(c.key or ""))
	if key == "" then key = "l:" .. tostring(o) end
	local name = CM.unescName(tostring(c.name or ""))
	if name == "" then name = tostring(o) end
	-- ONE STEAM ACCOUNT, TWO GAMES (2026-09-27, a local test: the second game runs
	-- in a sandbox on the same account). Such a game is "<steam key>|<name>", decided
	-- the same whichever joins first: its own entry if it has one; else the plain key
	-- when that key's recorded name is its own, or when no other letter of this
	-- session holds the key (a player who changed their lobby name).
	if key:sub(1, 2) == "s:" then
		local dual = key .. "|" .. name
		local heldElsewhere = false
		for o2, k2 in pairs(co.origin) do if k2 == key and o2 ~= o then heldElsewhere = true end end
		if co.members[dual] or (heldElsewhere and co.names[key] ~= name) then key = dual end
	end
	-- a player known by name only so far is the same player once it has a Steam id
	local byName = "n:" .. name
	if key ~= byName and co.members[byName] and not co.members[key] then
		co.members[key] = co.members[byName]; co.members[byName] = nil
		for _, c2 in pairs(co.list) do if c2.founder == byName then c2.founder = key end end
	end
	-- and a game whose Steam id could not be read this time (tpf2_steam.txt empty)
	-- is the Steam player of that name, when exactly one is known
	if key == byName and not co.members[key] then
		local found = nil
		for k in pairs(co.members) do
			if k:sub(1, 2) == "s:" and co.names[k] == name then
				if found then found = false; break end
				found = k
			end
		end
		if found then key = found end
	end
	co.names[key] = name
	-- a letter points at one player; a player at its latest letter
	for o2, k2 in pairs(co.origin) do if k2 == key and o2 ~= o then co.origin[o2] = nil end end
	co.origin[o] = key
	if tostring(c.lm) == "companies" and co.mode ~= "companies" then
		co.mode = "companies"
		CM.cmNote("the session plays separate companies")
	end
	local cid = CM.cmCompanyOfKey(key)
	local how = "back in their company"
	if not cid and co.legacy then
		cid = CM.cmClaimLegacy(o)
		if cid then
			how = "claimed from the older save"
			-- an older save kept founders by letter: the claimant names an unnamed company now
			local it = co.list[cid]
			if not it.founder and not it.fname then it.founder = key; it.ord = 1 end
		end
	end
	if not cid then
		local want = tonumber(c.want) or 1
		if co.mode ~= "companies" then
			cid = 1
			if not co.list[1] then CM.cmCreate(1, key); how = "founded the shared company" else how = "joined the shared company" end
		else
			-- the lobby's chip: the company of another player in the session means
			-- "share it" (unless it is locked); anything else is a company of their own
			local sharedWith = nil
			for o2, k2 in pairs(co.origin) do if o2 ~= o and co.members[k2] == want then sharedWith = k2 end end
			if sharedWith and co.list[want] and not co.list[want].pw then
				cid = want; how = "shares " .. CM.cmNameOf(want)
			else
				cid = (not co.list[want] and CM.cmClampColor(want)) and want or CM.cmNewId()
				if cid then
					CM.cmCreate(cid, key)
					how = "founded " .. CM.cmNameOf(cid)
				else
					cid = CM.cmIds()[1]
					how = "joined " .. CM.cmNameOf(cid) .. " (no room for another company)"
				end
			end
		end
	end
	co.members[key] = cid
	-- THE OLDER SAVE IS HANDED OUT (2026-09-27: its note never went away, and the
	-- legacy record rode along in every later save). Once every company has a
	-- player the claims are done: the record goes, on every machine alike (members
	-- are replicated), and with it the note.
	if co.legacy then
		local held = {}
		for _, c2 in pairs(co.members) do held[c2] = true end
		local open = false
		for _, c2 in ipairs(CM.cmIds()) do if not held[c2] then open = true end end
		if not open then
			co.legacy = nil
			CM.cmMigrated = nil
			CM.cmLog("CM: every company of the older save has its player -- the import is done")
		end
	end
	CM.cmRefreshDerived()
	CM.cmEnsurePids()
	CM.cmNote(string.format("%s joined: %s", name, how))
	if o == K.INSTANCE then
		CM.cmJoined = true
		if CM.cmMode == "companies" then CM.cmWantSwitch(cid) end
	end
end
-- A company's headquarters: its constructions whose file is a headquarters.
function CM.cmHQsOf(pid)
	local out = {}
	if not pid then return out end
	pcall(function()
		for _, id in pairs(game.interface.getEntities({ radius = 999999 }, { type = "CONSTRUCTION", includeData = false }) or {}) do
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and tostring(co.fileName or ""):find("headquarter", 1, true) and CM.cmOwnerOf(id) == pid then out[#out + 1] = id end
		end
	end)
	table.sort(out)
	return out
end
-- Bulldoze fromPid's headquarters. The refund lands on whoever the engine credits:
-- when that is our own player (not the company taking over), it is moved on to
-- `toPid`, so every machine ends with the same wallets.
function CM.cmRemoveHQs(fromPid, toPid, why)
	for _, id in ipairs(CM.cmHQsOf(fromPid)) do
		local me = CM.cmCompanyPid[CM.cmMyCompany]
		local b0 = CM.cmBalance(me)
		-- every machine removes it at this stamp: our removal tracker must not ship it as a DEMOLISH
		pcall(function()
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.transf and CM.expectedDemolish and CM.conKey then CM.expectedDemolish[CM.conKey(co.transf[13], co.transf[14])] = true end
		end)
		pcall(function() game.interface.setBulldozeable(id, true) end)
		local ok = pcall(game.interface.bulldoze, id)
		local b1 = CM.cmBalance(me)
		if ok and me ~= toPid and me ~= fromPid and b0 and b1 and b1 ~= b0 then
			CM.cmBookJournal(me, b0 - b1)
			CM.cmBookJournal(toPid, b1 - b0)
		end
		CM.cmLog(string.format("CM: %s: headquarters %d removed (ok=%s)", tostring(why), id, tostring(ok)))
	end
end
-- DELETE WITH NOBODY TAKING OVER (2026-09-27, user): the company's things go.
-- Every machine does the same at the stamp, in id order: vehicles sold, lines
-- deleted, buildings bulldozed (marked as expected, so the removal tracker ships
-- nothing), its road and track pieces removed in one proposal (with the nodes
-- that would be left bare), and its money and loan set to nothing. What the game
-- will not remove (a station another company's line still serves) goes to
-- `heirPid`, the deleting player's company. Returns what went and what was kept.
function CM.cmWipe(fromPid, heirPid, why)
	local sold, lines, built, edges, kept = 0, 0, 0, 0, 0
	local function owned(kind)
		local out = {}
		pcall(function()
			for _, id in pairs(game.interface.getEntities({ radius = 999999 }, { type = kind, includeData = false }) or {}) do
				if CM.cmOwnerOf(id) == fromPid then out[#out + 1] = id end
			end
		end)
		if kind == "VEHICLE" then
			pcall(function()
				local parked = api.engine.system.transportVehicleSystem.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
				local seen = {}
				for _, id in ipairs(out) do seen[id] = true end
				for i = 1, #parked do if not seen[parked[i]] and CM.cmOwnerOf(parked[i]) == fromPid then out[#out + 1] = parked[i] end end
			end)
		end
		table.sort(out)
		return out
	end
	-- a sell / delete / removal the game refuses leaves the thing with the heir, not
	-- with a company that no longer exists (the commands answer later: asynchronous)
	local function toHeir(id, what)
		if heirPid then pcall(function() CM.cmSetPlayer(id, heirPid) end) end
		CM.cmLog(string.format("CM: %s: %s %d could not be removed -- kept by the deleting company", tostring(why), what, id))
	end
	for _, v in ipairs(owned("VEHICLE")) do
		local ok = pcall(function() api.cmd.sendCommand(api.cmd.make.sellVehicle(v), function(_, okc) if not okc then toHeir(v, "vehicle") end end) end)
		if ok then sold = sold + 1 else toHeir(v, "vehicle") end
	end
	local lineIds = {}
	pcall(function()
		local ls = api.engine.system.lineSystem.getLines()
		for i = 1, #ls do if CM.cmOwnerOf(ls[i]) == fromPid then lineIds[#lineIds + 1] = ls[i] end end
	end)
	table.sort(lineIds)
	for _, l in ipairs(lineIds) do
		local ok = pcall(function() api.cmd.sendCommand(api.cmd.make.deleteLine(l), function(_, okc) if not okc then toHeir(l, "line") end end) end)
		if ok then lines = lines + 1 else toHeir(l, "line") end
	end
	for _, id in ipairs(owned("CONSTRUCTION")) do
		pcall(function()
			local co = api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)
			if co and co.transf and CM.expectedDemolish and CM.conKey then CM.expectedDemolish[CM.conKey(co.transf[13], co.transf[14])] = true end
		end)
		pcall(function() game.interface.setBulldozeable(id, true) end)
		if pcall(game.interface.bulldoze, id) then built = built + 1
		else toHeir(id, "building"); kept = kept + 1 end
	end
	-- the road and track pieces, and any node that has no other edge left
	local rm = owned("BASE_EDGE")
	if #rm > 0 then
		pcall(function()
			local rmSet, nodeSet, nodes = {}, {}, {}
			for _, e in ipairs(rm) do
				rmSet[e] = true
				local be = api.engine.getComponent(e, api.type.ComponentType.BASE_EDGE)
				for _, n in ipairs({ be and be.node0, be and be.node1 }) do
					if n and not nodeSet[n] then nodeSet[n] = true; nodes[#nodes + 1] = n end
				end
			end
			table.sort(nodes)
			local ss = api.engine.system.streetSystem
			local maps = { ss.getNode2StreetEdgeMap(), ss.getNode2TrackEdgeMap() }
			local orphans = {}
			for _, n in ipairs(nodes) do
				local total, gone = 0, 0
				for _, m in ipairs(maps) do
					for _, e in pairs((m and m[n]) or {}) do total = total + 1; if rmSet[e] then gone = gone + 1 end end
				end
				if total > 0 and total == gone then orphans[#orphans + 1] = n end
			end
			local sp = api.type.SimpleProposal.new()
			for i, e in ipairs(rm) do sp.streetProposal.edgesToRemove[i] = e end
			for i, n in ipairs(orphans) do sp.streetProposal.nodesToRemove[i] = n end
			local ctx = api.type.Context.new()
			ctx.cleanupStreetGraph = true
			api.cmd.sendCommand(api.cmd.make.buildProposal(sp, ctx, true), function(_, ok2)
				CM.cmLog(string.format("CM: %s: %d road/track piece(s) removed ok=%s", tostring(why), #rm, tostring(ok2)))
				-- refused as a whole (a piece a building still needs): the pieces stay, with the heir
				if not ok2 then for _, e in ipairs(rm) do if api.engine.entityExists(e) then toHeir(e, "road/track piece") end end end
			end)
			CM.edemoCache = nil
			edges = #rm
		end)
	end
	-- the money and the loan
	local b, l = CM.cmWallet(fromPid)
	if b then CM.cmSetWallet(fromPid, b, l, 0, 0) end
	CM.cmLog(string.format("CM: %s: %d vehicle(s) sold, %d line(s), %d building(s), %d road/track piece(s) removed, %d kept", tostring(why), sold, lines, built, edges, kept))
	return sold, lines, built, edges, kept
end
-- Delete company cid: everything it owns, its money and its loan go to `into`,
-- or -- into = 0 -- to nobody: it is removed (CM.cmWipe).
function CM.cmExecDelete(c, o, cid)
	local into = tonumber(c.into)
	local co = CM.co
	local mineCid = CM.cmCompanyOfOrigin(o)
	local nobody = into == 0
	if not nobody and (not into or into == cid or not co.list[into]) then return CM.cmNote("delete: choose another company to take over " .. CM.cmNameOf(cid)) end
	local players = CM.cmPlayersOf(cid)
	if #players > 0 then
		local who = {}
		for _, l in ipairs(players) do who[#who + 1] = co.names[co.origin[l]] or l end
		return CM.cmNote(string.format("%s cannot be deleted: %s %s playing it", CM.cmNameOf(cid), table.concat(who, ", "), #who == 1 and "is" or "are"))
	end
	local okPw, why = CM.cmPwOk(c, cid)
	if not okPw then return CM.cmNote("delete refused: " .. why) end
	if not nobody and into ~= mineCid and co.list[into].pw then return CM.cmNote("delete: " .. CM.cmNameOf(into) .. " is locked -- hand the company to your own or an open one") end
	if nobody then
		local fromName = CM.cmNameOf(cid)
		local fromPid = CM.cmCompanyPid[cid]
		local sold, lines, built, edges, kept = 0, 0, 0, 0, 0
		if fromPid then sold, lines, built, edges, kept = CM.cmWipe(fromPid, mineCid and CM.cmCompanyPid[mineCid], "delete " .. cid .. " (nobody takes over)") end
		-- its players are nobody's now: they get a company again when they next join
		for k, m in pairs(co.members) do if m == cid then co.members[k] = nil end end
		co.list[cid] = nil
		for _, other in pairs(co.list) do if other.open then other.open[cid] = nil end end
		CM.cmCompanyPid[cid] = nil
		CM.cmRefreshDerived()
		CM.cmApplyNames()
		pcall(CM.cmWritePerms)
		return CM.cmNote(string.format("%s deleted %s and everything it had: %d vehicle(s) sold, %d line(s), %d building(s), %d road/track piece(s) removed, money and loan dropped%s",
			tostring(co.names[co.origin[o]] or o), fromName, sold, lines, built, edges, kept > 0 and string.format(" (%d thing(s) the game would not remove went to your company)", kept) or ""))
	end
	local fromName, intoName = CM.cmNameOf(cid), CM.cmNameOf(into)
	local fromPid, toPid = CM.cmCompanyPid[cid], CM.cmCompanyPid[into]
	local n = 0
	if fromPid and toPid then
		-- one headquarters per company: the deleted company's goes when the company
		-- taking over has its own, else it takes this one over
		if #CM.cmHQsOf(toPid) > 0 then CM.cmRemoveHQs(fromPid, toPid, "delete " .. cid) end
		n = CM.cmMoveAssets(fromPid, toPid, "delete " .. cid)
		local bf, lf = CM.cmWallet(fromPid); local bt, lt = CM.cmWallet(toPid)
		if bf and bt then
			CM.cmSetWallet(toPid, bt, lt, bt + bf, lt + lf)
			CM.cmSetWallet(fromPid, bf, lf, 0, 0)
		end
	else
		CM.cmLog(string.format("CM: delete co%d into co%d: missing player entity (%s / %s) -- registry only", cid, into, tostring(fromPid), tostring(toPid)))
	end
	for k, m in pairs(co.members) do if m == cid then co.members[k] = into end end
	co.list[cid] = nil
	for _, other in pairs(co.list) do if other.open then other.open[cid] = nil end end
	CM.cmCompanyPid[cid] = nil
	CM.cmRefreshDerived()
	CM.cmApplyNames()
	pcall(CM.cmWritePerms)
	CM.cmNote(string.format("%s deleted %s: %d entities, its money and its loan went to %s", tostring(co.names[co.origin[o]] or o), fromName, n, intoName))
end
-- Repaint every vehicle company cid owns here in its colour (paint turned on, or
-- the colour changed while it is on). Each machine paints its own copies at the
-- stamp; the slice would capture the paint as a player's click, so each is expected.
function CM.cmRepaint(cid)
	local pid = CM.cmCompanyPid[cid]
	if not pid then return end
	local r, g, b = CM.cmCompanyColor(cid)
	local n = 0
	local function paint(v)
		if CM.cmOwnerOf(v) ~= pid then return end
		n = n + 1
		pcall(function()
			if CM.expectColorEcho then CM.expectColorEcho(v, r, g, b) end
			api.cmd.sendCommand(api.cmd.make.setColor(v, api.type.Vec3f.new(r, g, b)))
		end)
	end
	pcall(function()
		for _, v in pairs(game.interface.getEntities({ radius = 999999 }, { type = "VEHICLE", includeData = false }) or {}) do paint(v) end
	end)
	pcall(function()
		local parked = api.engine.system.transportVehicleSystem.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)
		for i = 1, #parked do paint(parked[i]) end
	end)
	CM.cmLog(string.format("CM: repainted %d vehicle(s) of co%d", n, cid))
end
function CM.execCompanyCmd(c)
	CM.cmEnsure()
	if CM.cmBootRefused then return end
	CM.cmAnyExec = true
	local o = c.origin
	local co = CM.co
	if c.op == "CMJOIN" then return CM.cmExecJoin(c, o) end
	if c.op == "CMLEAVE" then
		local who = tostring(c.who or "")
		if co.origin[who] then
			local name = co.names[co.origin[who]] or who
			co.origin[who] = nil
			CM.cmRefreshDerived()
			CM.cmNote(name .. " left the session")
		end
		return
	end
	local key = co.origin[o]
	local mineCid = CM.cmCompanyOfOrigin(o)
	local who = key and co.names[key] or tostring(o)
	if not key then return CM.cmNote(string.format("%s %s ignored: that player has not joined", tostring(o), tostring(c.op))) end
	local cid = c.cid and math.floor(tonumber(c.cid) or 0) or nil
	if c.op == "CMNEW" then
		if #CM.cmIds() >= CM.CM_MAX then return CM.cmNote("no more companies: " .. CM.CM_MAX .. " is the limit") end
		local was = co.mode
		if co.mode ~= "companies" then
			-- the first company of a co-op session: everyone so far stays in the shared one
			co.mode = "companies"
			if not co.list[1] then CM.cmCreate(1, key) end
		end
		local h = c.pw; if h == nil or h == "" or h == "-" or h == 0 then h = nil end
		local name = CM.unescName(tostring(c.name or "")):gsub("[%c]", ""):gsub("^%s+", ""):gsub("%s+$", "")
		local newId = CM.cmNewId()
		CM.cmCreate(newId, key, { name = name ~= "" and name or nil, color = c.color, paint = tonumber(c.paint) ~= 0, pw = h and tostring(h) or nil })
		co.members[key] = newId
		CM.cmRefreshDerived()
		CM.cmNote(string.format("%s founded %s%s%s", who, CM.cmNameOf(newId), h and " [password]" or "", was ~= "companies" and " -- the session now plays separate companies" or ""))
		if o == K.INSTANCE then CM.cmWantSwitch(newId) end
		return
	end
	if not cid or not co.list[cid] then return CM.cmNote(string.format("%s: no company %s", tostring(c.op), tostring(c.cid))) end
	if c.op == "CMSWITCH" then
		if mineCid == cid then return end
		local okPw, why = CM.cmPwOk(c, cid)
		if not okPw then return CM.cmNote(string.format("%s cannot join %s: %s", who, CM.cmNameOf(cid), why)) end
		co.members[key] = cid
		CM.cmRefreshDerived()
		CM.cmNote(string.format("%s now plays %s", who, CM.cmNameOf(cid)))
		if o == K.INSTANCE then CM.cmWantSwitch(cid) end
		return
	end
	if c.op == "CMDEL" then return CM.cmExecDelete(c, o, cid) end
	-- the rest are a company's own settings: only someone playing it may change them
	if mineCid ~= cid then return CM.cmNote(string.format("%s cannot change %s (plays %s)", who, CM.cmNameOf(cid), mineCid and CM.cmNameOf(mineCid) or "nothing")) end
	local it = co.list[cid]
	if c.op == "CMNAME" then
		local name = CM.unescName(tostring(c.name or "")):gsub("[%c]", ""):gsub("^%s+", ""):gsub("%s+$", "")
		it.name = (name ~= "") and name or nil
		CM.cmNote(string.format("%s renamed the company: %s", who, CM.cmNameOf(cid)))
		CM.cmApplyNames()
	elseif c.op == "CMCOLOR" then
		local color = CM.cmClampColor(c.color) or it.color
		-- a new colour must not look like another company's (keeping the old one while
		-- the paint toggles is always fine, even an old save's close pair)
		local holder = color ~= it.color and CM.cmColorHolder(color, cid)
		if holder then return CM.cmNote(string.format("that colour looks too much like %s's", CM.cmNameOf(holder))) end
		local paint = (c.paint == nil) and it.paint or (tonumber(c.paint) ~= 0)
		local repaint = paint and (not it.paint or color ~= it.color)
		it.color, it.paint = color, paint
		CM.cmNote(string.format("%s: colour %s, vehicles %s", CM.cmNameOf(cid), CM.cmColorHex(color), paint and "painted in it" or "keep their own colours"))
		if repaint then CM.cmRepaint(cid) end
	elseif c.op == "CMPW" then
		local h = c.pw; if h == nil or h == "" or h == "-" or h == 0 then h = nil end
		it.pw = h and tostring(h) or nil
		it.pwv = 2
		CM.cmNote(string.format("%s %s the password of %s", who, h and "set" or "cleared", CM.cmNameOf(cid)))
	elseif c.op == "CMOPEN" then
		local whom, on = tostring(c.who or "*"), tonumber(c.on or 1) == 1
		if whom == "*" then
			it.open = (not on) and {} or nil
		else
			local other = tonumber(whom)
			if not other or not co.list[other] or other == cid then return CM.cmNote("station access: no company " .. whom) end
			local set = it.open
			if set == nil then
				set = {}
				for _, r in ipairs(CM.cmIds()) do if r ~= cid then set[r] = true end end
			end
			set[other] = on and true or nil
			local all = true
			for _, r in ipairs(CM.cmIds()) do if r ~= cid and not set[r] then all = false end end
			it.open = (not all) and set or nil
		end
		CM.cmNote(string.format("%s's stations are now open to %s", CM.cmNameOf(cid), CM.cmOpenText(cid)))
	end
	pcall(CM.cmWritePerms)
end

-- ---------- the originator's side ----------
-- A vehicle we just bought gets our company's colour on every instance, when the
-- company paints its vehicles: one replicated VCOLOR against the vehicle's shared
-- key, a few units after the buy so the key has bound everywhere.
function CM.cmColorNewVehicle(key)
	if CM.cmMode ~= "companies" or not CM.cmMyCompany then return end
	local it = CM.cmCo(CM.cmMyCompany)
	if not it or not it.paint then return end
	local r, g, b = CM.cmCompanyColor(CM.cmMyCompany)
	CM.scheduleLocal("VCOLOR", { kind = "veh", key = key, r = r, g = g, b = b, delay = 3 })
end
-- Can this game send company commands at all? The hotseat needs the native
-- entity-only owner setter; a game without it refuses before anything ships.
function CM.cmCanRequest()
	if CM.cmOwnerCapability and not CM.cmOwnerCapability() then
		CM.cmNote("company changes need the matching shared-infrastructure DLL")
		return false
	end
	if not CM.cmJoined and CM.peerSeen then CM.cmNote("company changes wait until you have joined the session"); return false end
	return true
end
-- The menu's request (an inject line, inject.lua) -> a command. Passwords are
-- hashed here; the clear text never leaves this game.
--   CMNEW color paint name pw...   CMSWITCH cid pw...   CMDEL cid into pw...
--   CMPW cid pw...                 CMNAME cid name...   CMCOLOR cid color paint
--   CMOPEN who on
function CM.cmRequest(words)
	local o = words[1]
	CM.cmEnsure()
	if not CM.cmCanRequest() then return end
	if o == "CMNEW" or o == "CMSWITCH" or o == "CMDEL" then
		local loading = CM.cmLoadingPlayers()
		if #loading > 0 then CM.cmNote(CM.cmLoadingNote(loading)); return end
	end
	if o == "CMNEW" then
		local pw = table.concat(words, " ", 5)
		CM.scheduleLocal("CMNEW", { color = tonumber(words[2]) or 0, paint = tonumber(words[3]) == 0 and 0 or 1,
			name = words[4] and words[4] ~= "-" and words[4] or "", pw = CM.cmHashPw(pw) or "-" })
	elseif o == "CMSWITCH" then
		local cid = tonumber(words[2]); if not cid then return end
		local pw = table.concat(words, " ", 3)
		CM.scheduleLocal("CMSWITCH", { cid = cid, pw = CM.cmHashPw(pw) or "-", pw1 = CM.cmHashPwV1(cid, pw) or "-" })
	elseif o == "CMDEL" then
		local cid, into = tonumber(words[2]), tonumber(words[3]); if not cid or not into then return end
		local pw = table.concat(words, " ", 4)
		CM.scheduleLocal("CMDEL", { cid = cid, into = into, pw = CM.cmHashPw(pw) or "-", pw1 = CM.cmHashPwV1(cid, pw) or "-" })
	elseif o == "CMPW" then
		local cid = tonumber(words[2]); if not cid then return end
		CM.scheduleLocal("CMPW", { cid = cid, pw = CM.cmHashPw(table.concat(words, " ", 3)) or "-" })
	elseif o == "CMNAME" then
		local cid = tonumber(words[2]); if not cid then return end
		CM.scheduleLocal("CMNAME", { cid = cid, name = CM.escName(table.concat(words, " ", 3)) })
	elseif o == "CMCOLOR" then
		local cid = tonumber(words[2]); if not cid then return end
		CM.scheduleLocal("CMCOLOR", { cid = cid, color = tonumber(words[3]) or 0, paint = tonumber(words[4]) == 0 and 0 or 1 })
	elseif o == "CMOPEN" then
		local whom, on = tostring(words[2] or "*"), tonumber(words[3]) == 1 and 1 or 0
		if CM.cmMyCompany and (whom == "*" or tonumber(whom)) then
			CM.scheduleLocal("CMOPEN", { cid = CM.cmMyCompany, who = whom, on = on })
		end
	else
		return
	end
	log("company: requested " .. o)
end

-- ---------- the menu's view (dash file lines, lockstep.lua) ----------
-- comode=, comine=, cojoined=, comig=, and one co= line per company:
--   co=<cid>:<colour>:<paint 0|1>:<locked 0|1>:<name>:<members>:<playing letters>:<open code>
-- name and members percent-escaped (members comma-joined before escaping).
function CM.cmDashLines()
	local out = {}
	out[#out + 1] = "comode=" .. CM.cmMode
	out[#out + 1] = "comine=" .. tostring(CM.cmMyCompany or 0)
	out[#out + 1] = "cojoined=" .. ((CM.cmJoined or not CM.peerSeen) and "1" or "0")
	out[#out + 1] = "comig=" .. CM.escName(CM.cmMigrated or "")
	out[#out + 1] = "conote=" .. tostring(CM.cmLastNote or "")
	for _, cid in ipairs(CM.cmIds()) do
		local it = CM.co.list[cid]
		out[#out + 1] = string.format("co=%d:%d:%d:%d:%s:%s:%s:%s", cid, CM.cmColorOf(cid), it.paint and 1 or 0, it.pw and 1 or 0,
			CM.escName(CM.cmNameOf(cid)), CM.escName(table.concat(CM.cmMemberNames(cid), ", ")),
			table.concat(CM.cmPlayersOf(cid), ","), CM.cmOpenCode(cid))
	end
	return out
end

return {}
end
