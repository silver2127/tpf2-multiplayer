-- mp/companies.lua -- multi-company mode (opt-in; co-op is the default and is untouched)
--
-- Split out of lockstep.lua on 2026-09-08 (the single 11k-line chunk was at
-- 182 of Lua 5.1's 200 file-scope locals). Loaded from the game script as
--     local companies = require("mp.companies")(CM, K, log)
-- It is a FACTORY, not a plain module table: every load of the game script
-- gets fresh state (package.loaded would otherwise hand the next game the
-- previous game's file-scope locals), and the code below keeps the same
-- per-load semantics it had inside lockstep.lua. Anything defined later in
-- lockstep.lua is reached through CM (CM.ser, CM.deepcopy, ...); CM is the
-- shared state table, K the constants table, log the instance-tagged logger.
-- The body stays at column 0 on purpose: tools/luacheck.py's use-before-define
-- checks look at column-0 declarations.
return function(CM, K, log)
-- ---------- multi-company mode (opt-in; co-op is the default and is untouched) ----------
--
-- Two multiplayer models share this mod:
--   * "coop"      : one shared company, every action replicated (the original
--                   lockstep). This is the DEFAULT and nothing below runs.
--   * "companies" : each player runs their OWN company in the same map. On this
--                   machine the local player is company CM.cmMyCompany; every remote
--                   company is a dedicated engine player entity (game.interface.
--                   addPlayer -- the same mechanism the "Multiplayer Companies"
--                   mod uses). A replicated build lands owned by the local player,
--                   then we setPlayer it to the ORIGIN company. Because companies
--                   have separate state, this does not require a bit-identical
--                   world -- only that actions appear, attributed correctly.
--
-- Config file (written by the lobby/menu): line 1 = mode, line 2 = my company id,
-- line 3 = comma-separated roster of all company ids. Absent => coop.
-- ONE table for all companies-mode state/fns: Lua 5.1 allows only 200
-- top-level locals per chunk and this file sits right at it (a 19-local batch
-- crashed both instances at script load with "too many local variables").
-- The table itself is declared at the top of the file so the K.BASE discovery
-- can record into it; everything companies-mode starts here.
CM.CM_CFG_FILE = K.BASE .. "mp_company_cfg.txt"
-- The game buffers stdout until exit, so print()-only logging is invisible while
-- a live test runs. Companies-mode diagnostics go to their own file on disk.
function CM.cmLog(s)
	log(s)
	local f = io.open(K.BASE .. "mp_company_" .. tostring(K.INSTANCE or "?") .. ".log", "a")
	if f then f:write(s, "\n"); f:close() end
end
CM.cmMode       = "coop"
CM.cmMyCompany  = nil
CM.cmRoster     = nil        -- array of all company ids in the session
CM.cmCompanyPid = {}         -- [companyId] = engine player entity id
CM.cmExpectedCompany = {}    -- conKey -> origin company of a replay about to land there
CM.cmExpectedBal0    = {}    -- conKey -> our balance right before that replay was applied
CM.cmReady      = false
CM.cmCfgStamp   = nil

function CM.cmReadConfig()
	-- Once a company command (CMNEW/CMSWITCH/CMDEL) has run, the lobby's file
	-- is history: the lockstep commands own the roster and the origin map.
	if CM.cmLive then return end
	local f = io.open(CM.CM_CFG_FILE, "r")
	if not f then CM.cmMode = "coop"; CM.cmOriginCompany = CM.cmOriginCompany or {}; CM.cmApplySaved(); return end
	local mode = f:read("*l"); local mine = f:read("*l"); local roster = f:read("*l")
	local omap = f:read("*l")
	f:close()
	-- line 4 (optional): "a=1,b=2,..." -- the LOBBY-assigned company per origin
	-- instance. When present it is authoritative: a remote command's own
	-- company stamp is replaced by it, so a peer cannot act as a company the
	-- lobby did not give it (it can still only forge its origin letter, which
	-- the sealed transport ties to a lobby member).
	CM.cmOriginCompany = {}
	if omap then
		for o, cid in omap:gmatch("(%a+)%s*=%s*(%d+)") do CM.cmOriginCompany[o] = tonumber(cid) end
	end
	CM.cmMode = (mode and mode:gsub("%s", "")) or "coop"
	CM.cmMyCompany = mine and tonumber(mine)
	CM.cmRoster = nil
	if roster then
		CM.cmRoster = {}
		for id in roster:gmatch("%d+") do CM.cmRoster[#CM.cmRoster + 1] = tonumber(id) end
	end
	CM.cmApplySaved()
end

-- Lazily set up the player entities for companies mode. The local player is our
-- own company; each other company gets an addPlayer() entity, once.
function CM.cmEnsure()
	if CM.cmMode == "companies" and CM.cmReady then return end
	CM.cmReadConfig()
	if CM.cmMode ~= "companies" or not CM.cmMyCompany or not CM.cmRoster then CM.cmReady = false; return end
	pcall(function() CM.cmCompanyPid[CM.cmMyCompany] = api.engine.util.getPlayer() end)
	for _, cid in ipairs(CM.cmRoster) do
		if cid ~= CM.cmMyCompany and not CM.cmCompanyPid[cid] then
			local pid = nil
			pcall(function() pid = game.interface.addPlayer() end)
			if pid then
				CM.cmCompanyPid[cid] = pid
				pcall(function() game.interface.setMaximumLoan(pid, 100000000) end)
				CM.cmLog(string.format("CM: company %d -> AI player %s", cid, tostring(pid)))
			end
		end
	end
	if CM.cmCompanyPid[CM.cmMyCompany] then
		CM.cmReady = true
		CM.cmLog(string.format("CM: companies mode ready, me=co%d players=%d", CM.cmMyCompany, #CM.cmRoster))
	end
end

-- A replicated build landed owned by our local player; hand it to the origin
-- company and lock it so other companies cannot bulldoze it. Cost transfer
-- (refund local player, charge the origin company) is R2 -- TODO once R1
-- (ownership) is validated live. sendScriptEvent lets a co-resident
-- "Multiplayer Companies" mod track the logical ownership too (harmless if absent).
function CM.cmOwnerOf(eid)
	local ok, comp = pcall(function()
		return api.engine.getComponent(eid, api.type.ComponentType.PLAYER_OWNED)
	end)
	if ok and comp then return comp.player end
	return nil
end

-- R2: move money. A remote company's action was paid by OUR local wallet (the
-- build/buy ran as our human player); refund us and charge the origin company's
-- player. bookJournalEntry to a specific pid is the proven mechanism (the
-- Companies mod seeds inactive companies' AI players with it). Chunked because
-- TpF2 silently drops very large single journal amounts (mod-measured).
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
	-- getEntity(nil) is an ENGINE assert (scripting::ReadNonNegativeEntity):
	-- pcall catches the Lua side, but the game has already written an ~800 KB
	-- minidump on its assert handler -- the stall felt on every buy and depot in
	-- co-op (2026-09-02: 18 dumps for 5 buys + 2 depots, 2 per action). Guard.
	if type(pid) ~= "number" or pid < 0 then return nil end
	local bal = nil
	pcall(function() local e = game.interface.getEntity(pid); if e and e.balance then bal = e.balance end end)
	return bal
end

-- A replayed road/rail proposal was built as OUR player (buildContext), so our
-- wallet paid and our player owns the new edges. The proposal result says what
-- it cost (resultProposalData.costs -- measured present on ProposalData) and
-- which entities it made (resultEntities). Hand the entities to the origin
-- company and move the cost there. Constructions and vehicles settle by balance
-- delta elsewhere; edges use the proposal's own figure, which is exact and does
-- not care what else changed the balance meanwhile.
function CM.cmSettleBuild(c, res, success, what)
	if not success or not c or not c.company then return end
	CM.cmEnsure()
	if CM.cmMode ~= "companies" then return end
	local cid = tonumber(c.company)
	if not cid then return end
	local cost = nil
	pcall(function() cost = res and res.resultProposalData and res.resultProposalData.costs end)
	cost = tonumber(cost)
	local n, owned = 0, 0
	if cid ~= CM.cmMyCompany then
		-- resultEntities is empty for a street/track proposal (measured: a road
		-- build returns costs=1000 but entities=0), so the new edge is found by
		-- its endpoints instead -- the same locator the stop channel uses. Single
		-- edges (ROAD/RAIL) carry their coords on the command; a polyline (ROADP)
		-- is multi-segment and only its cost is settled, ownership left as-is.
		pcall(function()
			if (what == "ROAD" or what == "RAIL") and c.x0 and c.x1 then
				local eid = CM.findEdgeByEnds(what == "RAIL", c.x0, c.y0, c.x1, c.y1, 2.0)
				if eid then
					n = 1
					local po = nil
					pcall(function() po = api.engine.getComponent(eid, api.type.ComponentType.PLAYER_OWNED) end)
					if po then owned = 1; CM.cmReassignEntity(eid, cid, what) end
				end
			else
				local ents = res and res.resultEntities
				if not ents then return end
				for i = 1, #ents do
					local eid = ents[i]
					if eid and eid > 0 then
						n = n + 1
						local po = nil
						pcall(function() po = api.engine.getComponent(eid, api.type.ComponentType.PLAYER_OWNED) end)
						if po then owned = owned + 1; CM.cmReassignEntity(eid, cid, what) end
					end
				end
			end
		end)
	end
	CM.cmLog(string.format("CM: settle %s seq=%s origin=%s co%d cost=%s entities=%d owned=%d",
		tostring(what), tostring(c.seq), tostring(c.origin), cid, tostring(cost), n, owned))
	if cid ~= CM.cmMyCompany and cost and cost > 0 then CM.cmTransferCost(cid, cost, what) end
end

-- Transfer `cost` from the origin company to us (refund local, charge origin).
function CM.cmTransferCost(cid, cost, what)
	CM.cmLog(string.format("CM: cost settle %s: cid=%s cost=%s me=%s", tostring(what), tostring(cid), tostring(cost), tostring(CM.cmMyCompany)))
	if CM.cmMode ~= "companies" or not cid or cid == CM.cmMyCompany then return end
	if not cost or cost <= 0 then CM.cmLog("CM: cost settle: nothing to move (delta " .. tostring(cost) .. ")"); return end
	local mePid, theirPid = CM.cmCompanyPid[CM.cmMyCompany], CM.cmCompanyPid[cid]
	if not mePid or not theirPid then CM.cmLog("CM: cost transfer: missing pid (me=" .. tostring(mePid) .. " co" .. tostring(cid) .. "=" .. tostring(theirPid) .. ")"); return end
	local ok1, e1 = CM.cmBookJournal(mePid, cost)       -- refund us
	local ok2, e2 = CM.cmBookJournal(theirPid, -cost)   -- charge them
	CM.cmLog(string.format("CM: cost %s: moved %d from co%d(pid %s) to me(pid %s) | refund ok=%s %s | charge ok=%s %s",
		tostring(what), cost, cid, tostring(theirPid), tostring(mePid), tostring(ok1), tostring(e1 or ""), tostring(ok2), tostring(e2 or "")))
end

-- Generic reassign for any entity type (vehicles, lines): setPlayer only.
-- setBulldozeable is CONSTRUCTION-only (asserts otherwise) so it is skipped here.
function CM.cmReassignEntity(eid, cid, kind)
	CM.cmEnsure()
	if CM.cmMode ~= "companies" or not eid or not cid or cid == CM.cmMyCompany then return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	if before == pid then
		CM.cmLog(string.format("CM: %s eid=%s already owned by co%d pid=%s -- no setPlayer needed", tostring(kind), tostring(eid), cid, tostring(pid)))
		return
	end
	local ok, err = pcall(function() game.interface.setPlayer(eid, pid) end)
	local after = CM.cmOwnerOf(eid)
	CM.cmLog(string.format("CM: reassigned %s eid=%s -> co%d pid=%s | owner before=%s after=%s | setPlayer ok=%s err=%s",
		tostring(kind), tostring(eid), cid, tostring(pid), tostring(before), tostring(after), tostring(ok), tostring(err)))
end

-- ---------- in-game company management (2026-09-09) ----------
-- Three lockstep commands, applied at their stamp on every peer, so every
-- machine changes its company map on the same step:
--   CMNEW    cid=N [sw=1]   company N joins the roster (an AI player entity on
--                           every machine that is not playing it); sw=1 = the
--                           origin switches to it in the same command
--   CMSWITCH cid=N          the origin now plays as company N. Nothing changes
--                           hands: the origin's machine only re-labels its
--                           human player as N (CM.cmLocalSwitch), everyone else
--                           only updates origin -> company.
--   CMDEL    cid=N          company N is dissolved: its assets and money merge
--                           into the origin's company. Refused while anyone
--                           plays it. The AI entity stays behind, empty.
-- The GUI cannot call these (it is a separate Lua state): it appends
-- "CMSWITCH 3" style lines to the inject file and inject.lua schedules them.
CM.CM_OWNED_TYPES = { "CONSTRUCTION", "VEHICLE", "LINE", "BASE_EDGE", "BASE_NODE", "STATION_GROUP", "STATION", "SIGNAL" }
function CM.cmOwnedEntities(pid)
	local out, seen = {}, {}
	for _, kind in ipairs(CM.CM_OWNED_TYPES) do
		pcall(function()
			local t = game.interface.getEntities({ radius = 999999 }, { type = kind, includeData = false }) or {}
			for _, eid in pairs(t) do
				if type(eid) == "number" and not seen[eid] then
					seen[eid] = true
					if CM.cmOwnerOf(eid) == pid then out[#out + 1] = eid end
				end
			end
		end)
	end
	return out
end
function CM.cmMoveAssets(fromPid, toPid, why)
	if not fromPid or not toPid or fromPid == toPid then return 0 end
	local ents = CM.cmOwnedEntities(fromPid)
	local human = nil; pcall(function() human = api.engine.util.getPlayer() end)
	local n = 0
	for _, eid in ipairs(ents) do
		local ok = pcall(function() game.interface.setPlayer(eid, toPid) end)
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
-- every roster company we are not playing needs its AI player entity; cmEnsure
-- stops looking once the session is "ready", so a company created in-game
-- goes through here
function CM.cmEnsurePlayers()
	for _, cid in ipairs(CM.cmRoster or {}) do
		if cid ~= CM.cmMyCompany and not CM.cmCompanyPid[cid] then
			local pid = nil
			pcall(function() pid = game.interface.addPlayer() end)
			if pid then
				CM.cmCompanyPid[cid] = pid
				pcall(function() game.interface.setMaximumLoan(pid, 100000000) end)
				CM.cmLog(string.format("CM: company %d -> AI player %s (in-game)", cid, tostring(pid)))
			end
		end
	end
end
-- A company's wallet is its balance AND its loan; both live on the player
-- entity. Type-0 journal entries move the loan (and the balance with it),
-- type-6 entries move balance only (measured 2026-09-01).
function CM.cmWallet(pid)
	local b, l = nil, nil
	if type(pid) ~= "number" or pid < 0 then return nil end
	pcall(function() local e = game.interface.getEntity(pid); if e then b = e.balance or 0; l = e.loan or 0 end end)
	return b, l
end
-- set pid's wallet to (bal, loan) from (b0, l0)
function CM.cmSetWallet(pid, b0, l0, bal, loan)
	local dl = loan - l0
	if dl ~= 0 then CM.cmBookJournal(pid, dl, K.JOURNAL_LOAN or 0) end        -- moves loan and balance by dl
	local db = bal - (b0 + dl)
	if db ~= 0 then CM.cmBookJournal(pid, db, K.JOURNAL_TRANSFER or 6) end   -- balance only
end
function CM.cmSwapWallets(p1, p2)
	local b1, l1 = CM.cmWallet(p1); local b2, l2 = CM.cmWallet(p2)
	if not b1 or not b2 then return false end
	CM.cmSetWallet(p1, b1, l1, b2, l2)
	CM.cmSetWallet(p2, b2, l2, b1, l1)
	return true, b1, l1, b2, l2
end
function CM.cmNote(s) CM.cmLastNote = s; log("company: " .. s) end
-- The company's colour, 0..1 RGB: the same six fixed colours and golden-angle
-- hue walk the lobby chips use (menu_hook.cpp coColor), so a vehicle's paint
-- matches the chip its owner shows in the roster.
CM.CM_COLORS = { {220,80,80}, {80,140,230}, {90,190,110}, {230,180,60}, {180,100,220}, {80,200,200} }
function CM.cmCompanyColor(cid)
	cid = tonumber(cid) or 1
	local c = CM.CM_COLORS[cid]
	if c then return c[1] / 255, c[2] / 255, c[3] / 255 end
	local h = ((cid - 7) * 137.508) % 360
	local sat, val = 0.62, 0.85
	local C = val * sat
	local X = C * (1 - math.abs((h / 60) % 2 - 1))
	local m = val - C
	local r, g, b
	if h < 60 then r, g, b = C, X, 0 elseif h < 120 then r, g, b = X, C, 0 elseif h < 180 then r, g, b = 0, C, X
	elseif h < 240 then r, g, b = 0, X, C elseif h < 300 then r, g, b = X, 0, C else r, g, b = C, 0, X end
	return r + m, g + m, b + m
end
-- A vehicle we just bought gets our company's colour on every instance: one
-- replicated VCOLOR against the vehicle's shared key, a few units after the
-- buy so the key has bound on every peer (it binds when the vehicle appears).
function CM.cmColorNewVehicle(key)
	if CM.cmMode ~= "companies" or not CM.cmMyCompany then return end
	local r, g, b = CM.cmCompanyColor(CM.cmMyCompany)
	CM.scheduleLocal("VCOLOR", { kind = "veh", key = key, r = r, g = g, b = b, delay = 3 })
	log(string.format("company: vehicle %s painted in company %d's colour (%.2f,%.2f,%.2f)", tostring(key), CM.cmMyCompany, r, g, b))
end
-- Company passwords. The wire and every peer only ever see a salted hash
-- (same function everywhere, so the accept/refuse decision is identical on
-- every machine); the clear text stays in the inject file on the typist's
-- disk. A hash never parses as a number (the "h" prefix), so decodeCmd
-- leaves it a string. Empty password = open company.
CM.cmPw = {}   -- cid -> hash, or nil when open
function CM.cmHashPw(cid, pw)
	pw = tostring(pw or "")
	if pw == "" or pw == "-" then return nil end
	local sIn = "co" .. tostring(cid) .. ":" .. pw
	local M1, A1, M2, A2 = 2147483647, 48271, 2147483629, 40692
	local h1, h2 = 2166136261 % M1, 2166136261 % M2
	for i = 1, #sIn do local b = sIn:byte(i); h1 = (h1 * A1 + b) % M1; h2 = (h2 * A2 + b) % M2 end
	return string.format("h%010d%010d", h1, h2)
end
-- may `c` (op from origin o) act on company cid? true, or false + reason
function CM.cmPwOk(c, cid)
	local need = CM.cmPw[cid]
	if not need then return true end
	local given = c.pw
	if given == nil or given == "" or given == "-" then return false, "company " .. cid .. " needs its password" end
	if tostring(given) ~= need then return false, "wrong password for company " .. cid end
	return true
end
function CM.cmRosterHas(cid)
	for _, c in ipairs(CM.cmRoster or {}) do if c == cid then return true end end
	return false
end
function CM.cmNextId()
	if CM.cmOriginCompany == nil then CM.cmReadConfig() end
	local m = 1   -- coop's single shared company is company 1
	for _, c in ipairs(CM.cmRoster or {}) do if c > m then m = c end end
	if CM.cmMyCompany and CM.cmMyCompany > m then m = CM.cmMyCompany end
	return m + 1
end
-- who plays company cid: our own letter, or any origin the map points at it
function CM.cmPeerLive(o)
	local pr = CM.peers and CM.peers[o]
	return pr and pr.at and (CM.ticks - pr.at) <= (K.PEER_STALE_TICKS or 60)
end
function CM.cmPlayersOf(cid)
	local out = {}
	if CM.cmMyCompany == cid then out[#out + 1] = K.INSTANCE end
	-- only origins we actually hear from: the lobby file can carry letters of
	-- players who left, and a session that shrank must not keep companies locked
	for o, c in pairs(CM.cmOriginCompany or {}) do if c == cid and o ~= K.INSTANCE and CM.cmPeerLive(o) then out[#out + 1] = o end end
	table.sort(out)
	return out
end
-- coop has one shared company; the first company command turns the session
-- into companies mode with everyone on company 1 (what the lobby file says)
function CM.cmGoLive()
	if CM.cmLive then return end
	if CM.cmOriginCompany == nil then CM.cmReadConfig() end
	if CM.cmMode ~= "companies" then
		CM.cmMode = "companies"
		CM.cmMyCompany = CM.cmMyCompany or 1
		CM.cmRoster = { CM.cmMyCompany }
		CM.cmOriginCompany = CM.cmOriginCompany or {}
		for o in pairs(CM.peers or {}) do CM.cmOriginCompany[o] = CM.cmOriginCompany[o] or CM.cmMyCompany end
		pcall(function() CM.cmCompanyPid[CM.cmMyCompany] = api.engine.util.getPlayer() end)
		CM.cmReady = CM.cmCompanyPid[CM.cmMyCompany] ~= nil
		CM.cmLog("CM: session moved from coop to companies mode by an in-game company command")
	end
	CM.cmLive = true
end
-- The player moves to company cid on THIS machine, and NOTHING is handed over
-- (2026-09-10: "it's not supposed to swap anything, just move the player to a
-- different company"). No entity changes owner and no money moves. Only the
-- company -> player-entity map changes: our human player now stands for cid, so
-- what we do from here on is cid's, and the company we left takes cid's former
-- AI entity, which the other players' actions for it are attributed to.
-- What this replaces was the Companies mod's hotseat swap: setPlayer on every
-- entity the human owned, then a wallet swap. The engine asserts on setPlayer for
-- a vehicle on a line and for track and road edges and nodes (interface.cpp:2340,
-- a minidump apiece, pcall does not help), so one click on "new company" fired 98
-- of them and froze the host.
function CM.cmLocalSwitch(cid)
	local old = CM.cmMyCompany
	if cid == old then return true end
	local human, ai = CM.cmCompanyPid[old], CM.cmCompanyPid[cid]
	if not human or not ai then log("company: switch: missing player entity (me=" .. tostring(human) .. " target=" .. tostring(ai) .. ")"); return false end
	CM.cmCompanyPid[old] = ai; CM.cmCompanyPid[cid] = human
	CM.cmMyCompany = cid
	CM.cmNote(string.format("now playing company %d (was %d; nothing moved between the companies)", cid, old))
	return true
end

-- ---------- the company state travels IN THE SAVE (2026-09-09) ----------
-- Roster, passwords, origin -> company and company -> player entity are not
-- world state the engine saves; without this a resumed save (a relay's
-- /resume, or anyone loading a shared save later) would come up with fresh,
-- empty AI entities and every company's assets stranded. The game script's
-- save() hook stores it; load() stashes it and cmReadConfig applies it over
-- the lobby file: the entity ids are valid on every machine that loads this
-- save (the entities are in it), and the human player of the save IS the
-- company that saved it, so each machine then hotseat-swaps to its own
-- company (its old one if the saved map knows its letter, else the lobby's).
function CM.cmSaveState()
	if CM.cmMode ~= "companies" or not CM.cmMyCompany then return { v = 1, mode = "coop" } end
	local st = { v = 1, mode = "companies", mine = CM.cmMyCompany, roster = {}, origin = {}, pw = {}, pid = {} }
	for i, cid in ipairs(CM.cmRoster or {}) do st.roster[i] = cid end
	for o, cid in pairs(CM.cmOriginCompany or {}) do st.origin[o] = cid end
	st.origin[K.INSTANCE] = CM.cmMyCompany
	for cid, h in pairs(CM.cmPw or {}) do st.pw[tostring(cid)] = h end
	for cid, pid in pairs(CM.cmCompanyPid or {}) do st.pid[tostring(cid)] = pid end
	return st
end
function CM.cmLoadState(st)
	if type(st) == "table" and st.mode == "companies" then CM.cmSaved = st end
end
function CM.cmApplySaved()
	local sv = CM.cmSaved
	if not sv then return end
	CM.cmSaved = nil
	local human = nil; pcall(function() human = api.engine.util.getPlayer() end)
	local savedHuman = sv.pid and sv.pid[tostring(sv.mine)]
	if not human or savedHuman ~= human then
		log(string.format("company: saved state ignored -- this save's player entity is %s, the state was written for %s", tostring(human), tostring(savedHuman)))
		return
	end
	CM.cmMode = "companies"
	CM.cmRoster = {}
	for i, cid in ipairs(sv.roster or {}) do CM.cmRoster[i] = tonumber(cid) end
	CM.cmPw = {}
	for k, h in pairs(sv.pw or {}) do CM.cmPw[tonumber(k)] = h end
	CM.cmCompanyPid = {}
	for k, pid in pairs(sv.pid or {}) do CM.cmCompanyPid[tonumber(k)] = pid end
	local want = CM.cmMyCompany   -- the lobby's chip for us (may be nil in coop)
	if sv.origin and sv.origin[K.INSTANCE] then want = tonumber(sv.origin[K.INSTANCE]) end
	if not want or not CM.cmRosterHas(want) then want = tonumber(sv.mine) end
	for o, cid in pairs(sv.origin or {}) do if o ~= K.INSTANCE then CM.cmOriginCompany[o] = tonumber(cid) end end
	CM.cmMyCompany = tonumber(sv.mine)     -- the human entity is the saver's company right now
	CM.cmLive = true
	CM.cmReady = true
	log(string.format("company: state restored from the save: %d companies, saver was co%d, we take co%d", #CM.cmRoster, tonumber(sv.mine), want))
	if want ~= CM.cmMyCompany then CM.cmLocalSwitch(want) end
end

function CM.execCompanyCmd(c)
	local cid = c.cid and math.floor(tonumber(c.cid) + 0.5) or nil
	local o = c.origin
	if not cid or cid < 1 then log("company: bad cid in " .. tostring(c.op)); return end
	CM.cmGoLive()
	CM.cmEnsure(); CM.cmEnsurePlayers()
	if c.op == "CMNEW" then
		if CM.cmRosterHas(cid) then log("company: CMNEW " .. cid .. " already exists")
		else
			CM.cmRoster[#CM.cmRoster + 1] = cid
			table.sort(CM.cmRoster)
			local h = c.pw; if h == nil or h == "" or h == "-" or h == 0 then h = nil end
			CM.cmPw[cid] = h and tostring(h) or nil
			CM.cmEnsurePlayers()   -- creates the AI entity for it here (we are not playing it yet)
			CM.cmNote(string.format("%s created company %d%s (roster now %d)", tostring(o), cid, CM.cmPw[cid] and " [password]" or "", #CM.cmRoster))
		end
		if tonumber(c.sw or 0) == 1 then CM.execCompanyCmd({ op = "CMSWITCH", cid = cid, origin = o, pw = c.pw }) end
	elseif c.op == "CMSWITCH" then
		if not CM.cmRosterHas(cid) then log("company: CMSWITCH to unknown company " .. cid); return end
		local okPw, why = CM.cmPwOk(c, cid)
		if not okPw then CM.cmNote(string.format("%s cannot join company %d: %s", tostring(o), cid, why)); return end
		if o == K.INSTANCE then
			CM.cmLocalSwitch(cid)
		else
			CM.cmOriginCompany[o] = cid
			CM.cmNote(string.format("%s now plays company %d", tostring(o), cid))
		end
	elseif c.op == "CMDEL" then
		if not CM.cmRosterHas(cid) then CM.cmNote("cannot dissolve " .. cid .. ": no such company"); return end
		local players = CM.cmPlayersOf(cid)
		if #players > 0 then CM.cmNote(string.format("cannot dissolve %d: played by %s", cid, table.concat(players, ","))); return end
		local okPw, why = CM.cmPwOk(c, cid)
		if not okPw then CM.cmNote(string.format("%s cannot dissolve company %d: %s", tostring(o), cid, why)); return end
		local intoCid = (o == K.INSTANCE) and CM.cmMyCompany or CM.cmOriginCompany[o]
		local fromPid, toPid = CM.cmCompanyPid[cid], intoCid and CM.cmCompanyPid[intoCid]
		if not fromPid or not toPid then log("company: CMDEL " .. cid .. ": missing player entity"); return end
		local n = CM.cmMoveAssets(fromPid, toPid, "dissolve " .. cid)
		local bf, lf = CM.cmWallet(fromPid); local bt, lt = CM.cmWallet(toPid)
		if bf and bt then
			CM.cmSetWallet(toPid, bt, lt, bt + bf, lt + lf)
			CM.cmSetWallet(fromPid, bf, lf, 0, 0)
		end
		for i = #CM.cmRoster, 1, -1 do if CM.cmRoster[i] == cid then table.remove(CM.cmRoster, i) end end
		CM.cmCompanyPid[cid] = nil
		CM.cmPw[cid] = nil
		CM.cmNote(string.format("%s dissolved company %d into %d (%d entities, balance %s, loan %s)", tostring(o), cid, intoCid, n, tostring(bf), tostring(lf)))
	elseif c.op == "CMPW" then
		-- set / clear a company's password: only someone playing it may
		if not CM.cmRosterHas(cid) then CM.cmNote("cannot set a password: no company " .. cid); return end
		local mineCid = (o == K.INSTANCE) and CM.cmMyCompany or CM.cmOriginCompany[o]
		if mineCid ~= cid then CM.cmNote(string.format("%s cannot set company %d's password (plays %s)", tostring(o), cid, tostring(mineCid))); return end
		local h = c.pw; if h == nil or h == "" or h == "-" or h == 0 then h = nil end
		CM.cmPw[cid] = h and tostring(h) or nil
		CM.cmNote(string.format("%s %s company %d's password", tostring(o), h and "set" or "cleared", cid))
	end
end

function CM.cmReassignConstruction(eid, cid)
	-- Initialise BEFORE the mode gate. The first thing that happens on a peer can
	-- be a replayed build arriving before any local action has read the config --
	-- without this it evaluated mode=coop/me=nil and silently skipped (verified in
	-- the 2026-08-29 trace: "reassign requested ... mode=coop me=nil").
	CM.cmEnsure()
	CM.cmLog(string.format("CM: reassign requested eid=%s cid=%s mode=%s me=%s",
		tostring(eid), tostring(cid), tostring(CM.cmMode), tostring(CM.cmMyCompany)))
	if CM.cmMode ~= "companies" or not eid or not cid then return end
	if cid == CM.cmMyCompany then CM.cmLog("CM: own company, skip"); return end
	local pid = CM.cmCompanyPid[cid]
	if not pid then CM.cmEnsure(); pid = CM.cmCompanyPid[cid] end
	if not pid then CM.cmLog("CM: no player for company " .. tostring(cid)); return end
	local before = CM.cmOwnerOf(eid)
	-- setBulldozeable CRASHES the game (ParcelSystem assertion -- pcall does NOT
	-- catch C++ asserts) on entities that can't take the Bulldozeable component.
	-- The "Multiplayer Companies" mod's rule: only call it on entities that have a
	-- CONSTRUCTION component. Same guard here.
	local hasCon = false
	pcall(function() hasCon = api.engine.getComponent(eid, api.type.ComponentType.CONSTRUCTION) ~= nil end)
	-- Do NOT swallow errors silently: the whole point of this probe is to learn
	-- whether setPlayer works on a replicated construction. Record each result.
	local okSP, errSP = pcall(function() game.interface.setPlayer(eid, pid) end)
	local okBZ, errBZ = true, "skipped (no CONSTRUCTION component)"
	if hasCon then okBZ, errBZ = pcall(function() game.interface.setBulldozeable(eid, false) end) end
	-- (sendScriptEvent is a GUI-side API and is nil on the engine side -- the
	-- optional "tell the Companies mod" hook is dropped; ownership + lock are ours.)
	local okEV, errEV = true, nil
	local after = CM.cmOwnerOf(eid)
	CM.cmLog(string.format("CM: reassigned eid=%s -> co%d pid=%s | owner before=%s after=%s | setPlayer ok=%s err=%s | bulldozeable ok=%s err=%s | event ok=%s err=%s",
		tostring(eid), cid, tostring(pid), tostring(before), tostring(after),
		tostring(okSP), tostring(errSP), tostring(okBZ), tostring(errBZ), tostring(okEV), tostring(errEV)))
end

return {}
end
