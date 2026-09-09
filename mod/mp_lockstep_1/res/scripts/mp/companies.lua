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
	local f = io.open(CM.CM_CFG_FILE, "r")
	if not f then CM.cmMode = "coop"; CM.cmOriginCompany = CM.cmOriginCompany or {}; return end
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
