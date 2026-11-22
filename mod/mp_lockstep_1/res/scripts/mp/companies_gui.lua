-- mp/companies_gui.lua -- the COMPANIES tab of the Multiplayer window (GUI Lua state)
--
-- Rewritten with the registry (companies.lua, 2026-09-27). The GUI state cannot
-- reach the lockstep queue: it READS the sim's dash file (lockstep_dash_<letter>.txt,
-- the co=/comine=/... lines of CM.cmDashLines) and WRITES requests into the inject
-- file, which inject.lua hands to CM.cmRequest. Nothing here decides anything; the
-- sim refuses what it must and says why in conote=.
--
-- Layout, top to bottom (few buttons: one per decision, toggles instead of pairs,
-- the main action of a section in the primary style, the rest plain):
--   YOUR COMPANY    [colour] name ........................ SETTINGS
--                   (a note after importing an older save)
--   COMPANIES       one row per company: [colour] name (the row's button selects
--                   it; the selected row is highlighted) and, dimmed, who plays it
--                   + NEW COMPANY
--   <selected>      another company: its password when locked, SWITCH TO IT,
--                   DELETE..., and one toggle for its vehicles at your stations
--   delete          which company takes over everything; DELETE / CANCEL
--   new company     name, colour, vehicle paint toggle, password; CREATE / CANCEL
--   settings        name + RENAME, colour, vehicle paint toggle, password with
--                   ONE button (set, or remove when the field is empty), and one
--                   toggle opening / closing your stations to everyone
--   the last note from the sim, dimmed
-- A row pool (CM.CO_GUI_ROWS) is built once and shown / hidden: rebuilding
-- widgets on every refresh lost clicks, and a rebuilt ComboBox reset the
-- selection (the old tab jumped to the first company on any label change).
-- Style classes (res/config/style_sheet/mp_lockstep.lua): mpCoHead (section
-- heading), mpCoDim (secondary text), mpCoSel (the selected row), mpDashPrimary
-- (the section's main button), mpCoN / mpCoPick (colours).
return function(CM, K, log)
CM.CO_GUI_ROWS = 16

-- one request line into this game's inject file
function CM.coGuiSend(line)
	local f = io.open(K.BASE .. "lockstep_inject_" .. (K.INSTANCE or "a") .. ".txt", "a")
	if f then f:write(line .. string.char(10)); f:close(); return true end
	return false
end
-- the dash's company lines -> { list = {...}, byId = {}, mine, mode, joined, migrated, note }
function CM.coGuiParse(kv)
	local out = { list = {}, byId = {} }
	if not kv then return out end
	out.mine = tonumber(kv.comine)
	if out.mine == 0 then out.mine = nil end
	out.mode = kv.comode or "coop"
	out.joined = kv.cojoined ~= "0"
	out.migrated = CM.unescName(kv.comig or "")
	out.note = kv.conote or ""
	for _, v in ipairs(kv.coList or {}) do
		local cid, color, paint, locked, name, members, playing, open = v:match("^(%d+):(%d+):(%d):(%d):([^:]*):([^:]*):([^:]*):(.*)$")
		if cid then
			local it = { cid = tonumber(cid), color = tonumber(color), paint = paint == "1", locked = locked == "1",
			             name = CM.unescName(name), members = CM.unescName(members), open = open, playing = {} }
			for l in playing:gmatch("%a+") do it.playing[#it.playing + 1] = l end
			out.list[#out.list + 1] = it
			out.byId[it.cid] = it
		end
	end
	table.sort(out.list, function(p, q)
		if (p.cid == out.mine) ~= (q.cid == out.mine) then return p.cid == out.mine end
		if p.name:lower() == q.name:lower() then return p.cid < q.cid end
		return p.name:lower() < q.name:lower()
	end)
	return out
end
-- may company `other` stop at the stations of a company whose open code is `code`?
function CM.coGuiOpenFor(code, other)
	code = tostring(code or "*")
	if code == "*" then return true end
	for v in code:gmatch("%d+") do if tonumber(v) == other then return true end end
	return false
end
function CM.coGuiOpenText(code, st)
	code = tostring(code or "*")
	if code == "*" then return "everyone" end
	if code == "-" then return "nobody" end
	local ns = {}
	for v in code:gmatch("%d+") do local it = st.byId[tonumber(v)]; ns[#ns + 1] = it and it.name or ("Company " .. v) end
	return #ns > 0 and table.concat(ns, ", ") or "nobody"
end

-- ---------- widgets ----------
local function gui() return api.gui end
function CM.coGuiText(s, cls)
	local tv = gui().comp.TextView.new(s or "")
	if cls then pcall(function() tv:setStyleClassList({ cls }) end) end
	return tv
end
-- a button and its label (the label is needed to change the text later); cls
-- "mpDashPrimary" marks a section's main action
function CM.coGuiButtonTv(label, fn, cls)
	local tv = gui().comp.TextView.new((label:gsub("^%s+", ""):gsub("%s+$", "")):upper())
	local b = gui().comp.Button.new(tv, true)
	if cls then pcall(function() b:setStyleClassList({ cls }) end) end
	b:onClick(function() local ok, err = pcall(fn); if not ok then print("[ls-gui] companies: " .. tostring(err)) end end)
	return b, tv
end
-- a button alone: ONE return value, so it can sit inside a table constructor (a second
-- value, the label, would land in the row as a widget of its own)
function CM.coGuiButton(label, fn, cls)
	local b = CM.coGuiButtonTv(label, fn, cls)
	return b
end
function CM.coGuiInput(minW)
	local mk = gui().comp.TextInputField
	local ok, inp = pcall(function() return mk.new() end)
	if not ok then inp = mk.new("") end
	pcall(function() inp:setMinimumSize(gui().util.Size.new(minW or 180, 26)) end)
	pcall(function() inp:setMaximumSize(gui().util.Size.new(320, 26)) end)
	return inp
end
function CM.coGuiGet(inp)
	local t = ""
	pcall(function() t = inp and inp:getText() or "" end)
	return ((t or ""):gsub("[%c]", ""):gsub("^%s+", ""):gsub("%s+$", ""))
end
function CM.coGuiClear(inp)
	if not inp then return end
	if not pcall(function() inp:setText("", false) end) then pcall(function() inp:setText("") end) end
end
function CM.coGuiBox(items, name, orient)
	local l = gui().layout.BoxLayout.new(orient or "HORIZONTAL")
	for _, it in ipairs(items) do l:addItem(it) end
	local c = gui().comp.Component.new(name or "mpCompanyRow")
	c:setLayout(l)
	return c
end
function CM.coGuiShow(w, on) if w then pcall(function() w:setVisible(on and true or false, false) end) end end
-- A long text broken into lines at spaces: a TextView does not wrap, and the
-- delete warning ran past the window's right edge (2026-09-27). 52 characters is
-- what the chat log holds in the same window (CM.chatWrap).
function CM.coGuiWrap(s, width)
	width = width or 52
	local out = {}
	local NL = string.char(10)
	for para in (tostring(s or "") .. NL):gmatch("(.-)" .. NL) do
		local line = ""
		for word in para:gmatch("%S+") do
			if line ~= "" and #line + 1 + #word > width then out[#out + 1] = line; line = word
			else line = (line == "" and word or line .. " " .. word) end
		end
		out[#out + 1] = line
	end
	return table.concat(out, NL)
end
function CM.coGuiSetText(D, key, w, s)
	D.coShown = D.coShown or {}
	if w and D.coShown[key] ~= s then D.coShown[key] = s; pcall(function() w:setText(s) end) end
end
function CM.coGuiSetClass(D, key, w, cls)
	D.coShown = D.coShown or {}
	local sig = type(cls) == "table" and table.concat(cls, " ") or tostring(cls)
	if w and D.coShown[key] ~= sig then
		D.coShown[key] = sig
		pcall(function() w:setStyleClassList(type(cls) == "table" and cls or { cls }) end)
	end
end
function CM.coGuiSwatch() return gui().comp.TextView.new("     ") end
-- one colour button: a label painted by class (the mark on it is set later)
local function swatchButton(onClick)
	local tv = gui().comp.TextView.new("   ")
	local b = gui().comp.Button.new(tv, true)
	b:onClick(function() local ok, err = pcall(onClick); if not ok then print("[ls-gui] companies: " .. tostring(err)) end end)
	return b, tv
end
-- the hue column (1..24) a colour sits nearest to, for opening the picker on it
function CM.coGuiHueOf(v)
	local r, g, b = CM.cmColorRGB(v)
	local mx, mn = math.max(r, g, b), math.min(r, g, b)
	if mx - mn < 20 then return 1 end
	local h
	if mx == r then h = ((g - b) / (mx - mn)) % 6 elseif mx == g then h = (b - r) / (mx - mn) + 2 else h = (r - g) / (mx - mn) + 4 end
	return math.floor(h * 60 / (360 / CM.CM_GRID_HUES) + 0.5) % CM.CM_GRID_HUES + 1
end
-- THE COLOUR PICKER (free choice, 2026-09-27; the user: "lass uns die farbauswahl
-- frei waehlbar machen"). Three parts, onPick(colour) with a stored colour value:
--   hues     24 buttons in two rows: choose a hue (nothing is sent)
--   shades   that hue in 5 shades, then 6 greys: a click picks the colour
--   exact    "#RRGGBB" typed in, USE picks exactly that colour
-- Every swatch is a style class the sheet defines (the grid classes 201..), so it
-- shows its true colour. D[prefix .. "Hue"] is the hue shown.
function CM.coGuiPicker(D, prefix, onPick)
	local rows = {}
	local per = CM.CM_GRID_HUES / 2
	D[prefix .. "HueTv"], D[prefix .. "ShadeTv"], D[prefix .. "ShadeCls"] = {}, {}, {}
	for r = 0, 1 do
		local items = {}
		for h = r * per + 1, (r + 1) * per do
			local b, tv = swatchButton(function() D[prefix .. "Hue"] = h end)
			pcall(function() tv:setStyleClassList({ "mpCo" .. CM.cmGridClass(h, 3) }) end)
			D[prefix .. "HueTv"][h] = tv
			items[#items + 1] = b
		end
		rows[#rows + 1] = CM.coGuiBox(items, prefix .. "Hues" .. r)
	end
	local items = {}
	local n = #CM.CM_GRID_SHADES + #CM.CM_GRID_GREYS
	for i = 1, n do
		local b, tv = swatchButton(function()
			local cls = D[prefix .. "ShadeCls"][i]
			if cls then onPick(CM.cmRgbValue(CM.cmClassRGB(cls))) end
		end)
		D[prefix .. "ShadeTv"][i] = tv
		items[#items + 1] = b
	end
	rows[#rows + 1] = CM.coGuiBox(items, prefix .. "Shades")
	D[prefix .. "HexText"] = CM.coGuiText("", "mpCoDim")
	D[prefix .. "HexInput"] = CM.coGuiInput(90)
	rows[#rows + 1] = CM.coGuiBox({ D[prefix .. "HexText"], CM.coGuiText("exact #RRGGBB"), D[prefix .. "HexInput"], CM.coGuiButton("Use", function()
		local typed = CM.coGuiGet(D[prefix .. "HexInput"])
		local v = CM.cmHexColor(typed)
		if not v then D.coHint = "type a colour like #1E90FF"; return end
		CM.coGuiClear(D[prefix .. "HexInput"])
		D[prefix .. "Hue"] = CM.coGuiHueOf(v)
		onPick(v)
	end) }, prefix .. "Exact")
	return rows
end
-- The picker's state each refresh: the shade row follows the hue shown, the chosen
-- colour is marked X, a colour that looks like another company's "-" (the sim
-- refuses those), and the exact line shows the chosen colour's code.
function CM.coGuiMarkColor(D, prefix, chosen, st, self)
	local others = {}
	for _, it in ipairs(st and st.list or {}) do if it.cid ~= self and it.color then others[#others + 1] = it.color end end
	local function taken(v)
		for _, o in ipairs(others) do if CM.cmColorDistance(o, v) < CM.CM_COLOR_NEAR then return true end end
		return false
	end
	local function same(v) return chosen and CM.cmColorDistance(chosen, v) < 1 end
	local function mark(key, tv, cls, m)
		CM.coGuiSetText(D, key .. "T", tv, m)
		-- !mpCoN paints text like the background: a mark is white (style sheet !mpCoPick)
		CM.coGuiSetClass(D, key .. "K", tv, m ~= "   " and { "mpCo" .. cls, "mpCoPick" } or { "mpCo" .. cls })
	end
	if not D[prefix .. "Hue"] then D[prefix .. "Hue"] = chosen and CM.coGuiHueOf(chosen) or 1 end
	local hue = D[prefix .. "Hue"]
	for h, tv in pairs(D[prefix .. "HueTv"] or {}) do mark(prefix .. "H" .. h, tv, CM.cmGridClass(h, 3), h == hue and " > " or "   ") end
	local ns = #CM.CM_GRID_SHADES
	for i, tv in pairs(D[prefix .. "ShadeTv"] or {}) do
		local cls = i <= ns and CM.cmGridClass(hue, i) or CM.cmGridClass(0, i - ns)
		D[prefix .. "ShadeCls"][i] = cls
		local v = CM.cmRgbValue(CM.cmClassRGB(cls))
		mark(prefix .. "S" .. i, tv, cls, (same(v) and " X ") or (taken(v) and " - ") or "   ")
	end
	CM.coGuiSetText(D, prefix .. "Hex", D[prefix .. "HexText"], chosen and ("now " .. CM.cmColorHex(chosen)) or "no colour chosen")
end
-- who plays a company, for its row: "you", player names, else its members, else nobody
function CM.coGuiWho(it, st)
	local who = {}
	for _, l in ipairs(it.playing) do who[#who + 1] = CM.playerNameOf(l) end
	local s
	if #who > 0 then s = table.concat(who, ", ")
	elseif it.members ~= "" then s = it.members .. " (away)"
	else s = "nobody" end
	if it.cid == st.mine then s = "you" .. (#who > 1 and (" + " .. (#who - 1)) or "") end
	if it.locked then s = s .. "  -  locked" end
	return s
end

-- ---------- build (once per window) ----------
function CM.coGuiBuild(D, box)
	local V = gui().layout.BoxLayout.new("VERTICAL")
	-- your company, with the way into its settings beside it
	V:addItem(CM.coGuiText("YOUR COMPANY", "mpCoHead"))
	D.coSwMine = CM.coGuiSwatch()
	D.coNameText = CM.coGuiText("")
	D.coSetToggle, D.coSetToggleTv = CM.coGuiButtonTv("Settings", function() D.coSetOpen = not D.coSetOpen; D.coNewOpen = false end)
	V:addItem(CM.coGuiBox({ D.coSwMine, D.coNameText, D.coSetToggle }, "mpCompanyMine"))
	-- the note about an imported older save, until OK (or until every company has its player)
	D.coMigText = CM.coGuiText("", "mpDashAlert")
	D.coMigBox = CM.coGuiBox({ D.coMigText, CM.coGuiButton("OK", function() D.coMigSeen = D.coMigShown end) }, "mpCompanyMigrated")
	V:addItem(D.coMigBox)
	-- your company's settings (hidden until SETTINGS)
	D.coRename = CM.coGuiInput(220)
	D.coPaintBtn, D.coPaintTv = CM.coGuiButtonTv("Paint vehicles: on", function()
		local me = D.coState and D.coMine and D.coState.byId[D.coMine]
		if me then CM.coGuiSend(string.format("CMCOLOR %d %d %d", D.coMine, me.color, me.paint and 0 or 1)) end
	end)
	D.coPwInput = CM.coGuiInput(180)
	D.coPwBtn, D.coPwBtnTv = CM.coGuiButtonTv("Set password", function()
		if not D.coMine then return end
		local pw = CM.coGuiGet(D.coPwInput)
		CM.coGuiSend("CMPW " .. D.coMine .. (pw ~= "" and (" " .. pw) or ""))
		CM.coGuiClear(D.coPwInput)
	end)
	D.coOpenText = CM.coGuiText("")
	D.coOpenBtn, D.coOpenBtnTv = CM.coGuiButtonTv("Close to all", function()
		local me = D.coState and D.coMine and D.coState.byId[D.coMine]
		if me then CM.coGuiSend("CMOPEN * " .. (me.open == "*" and "0" or "1")) end
	end)
	local sl = { CM.coGuiBox({ CM.coGuiText("Name"), D.coRename, CM.coGuiButton("Rename", function()
		if not D.coMine then return end
		CM.coGuiSend("CMNAME " .. D.coMine .. " " .. CM.coGuiGet(D.coRename))
		CM.coGuiClear(D.coRename)
	end) }, "mpCompanyRename") }
	sl[#sl + 1] = CM.coGuiText("Colour", "mpCoDim")
	for _, r in ipairs(CM.coGuiPicker(D, "coSet", function(v)
		local me = D.coState and D.coMine and D.coState.byId[D.coMine]
		if me then CM.coGuiSend(string.format("CMCOLOR %d %d %d", D.coMine, v, me.paint and 1 or 0)) end
	end)) do sl[#sl + 1] = r end
	sl[#sl + 1] = D.coPaintBtn
	sl[#sl + 1] = CM.coGuiBox({ CM.coGuiText("Password"), D.coPwInput, D.coPwBtn }, "mpCompanyPwRow")
	sl[#sl + 1] = CM.coGuiBox({ D.coOpenText, D.coOpenBtn }, "mpCompanyOpenAll")
	D.coSetBox = CM.coGuiBox(sl, "mpCompanySettings", "VERTICAL")
	V:addItem(D.coSetBox)
	-- the companies
	V:addItem(CM.coGuiText("COMPANIES", "mpCoHead"))
	D.coRows = {}
	for i = 1, CM.CO_GUI_ROWS do
		local row = {}
		row.sw = CM.coGuiSwatch()
		row.btn, row.tv = CM.coGuiButtonTv("-", function() if row.cid then D.coSel = row.cid; D.coDelOpen = false; D.coHint = nil end end)
		row.info = CM.coGuiText("", "mpCoDim")
		row.c = CM.coGuiBox({ row.sw, row.btn, row.info }, "mpCompanyListRow" .. i)
		V:addItem(row.c)
		D.coRows[i] = row
	end
	D.coMoreText = CM.coGuiText("", "mpCoDim")
	V:addItem(D.coMoreText)
	D.coNewToggle = CM.coGuiButton("+ New company", function() D.coNewOpen = not D.coNewOpen; D.coSetOpen = false end)
	V:addItem(D.coNewToggle)
	-- new company (hidden until + NEW COMPANY)
	D.coNewName = CM.coGuiInput(220)
	D.coNewPaint = true
	D.coNewPaintBtn, D.coNewPaintTv = CM.coGuiButtonTv("Paint vehicles: on", function() D.coNewPaint = not D.coNewPaint end)
	D.coNewPw = CM.coGuiInput(180)
	local nl = { CM.coGuiText("NEW COMPANY", "mpCoHead"), CM.coGuiBox({ CM.coGuiText("Name"), D.coNewName }, "mpCompanyNewName") }
	nl[#nl + 1] = CM.coGuiText("Colour", "mpCoDim")
	for _, r in ipairs(CM.coGuiPicker(D, "coNew", function(v) D.coNewColor = v end)) do nl[#nl + 1] = r end
	nl[#nl + 1] = D.coNewPaintBtn
	nl[#nl + 1] = CM.coGuiBox({ CM.coGuiText("Password (optional)"), D.coNewPw }, "mpCompanyNewPw")
	nl[#nl + 1] = CM.coGuiBox({ CM.coGuiButton("Create", function()
		local name = CM.coGuiGet(D.coNewName)
		local pw = CM.coGuiGet(D.coNewPw)
		CM.coGuiSend(string.format("CMNEW %d %d %s%s", D.coNewColor or 0, D.coNewPaint and 1 or 0,
			name ~= "" and CM.escName(name) or "-", pw ~= "" and (" " .. pw) or ""))
		CM.coGuiClear(D.coNewName); CM.coGuiClear(D.coNewPw)
		D.coNewOpen = false; D.coNewColor = nil; D.coNewHue = nil
		D.coHint = "creating " .. (name ~= "" and name or "a company") .. "..."
	end, "mpDashPrimary"), CM.coGuiButton("Cancel", function() D.coNewOpen = false end) }, "mpCompanyNewActions")
	D.coNewBox = CM.coGuiBox(nl, "mpCompanyNew", "VERTICAL")
	V:addItem(D.coNewBox)
	-- the selected company (another one than yours)
	D.coSelText = CM.coGuiText("", "mpCoHead")
	D.coSelPwInput = CM.coGuiInput(180)
	D.coSelPwRow = CM.coGuiBox({ CM.coGuiText("Its password"), D.coSelPwInput }, "mpCompanySelPw")
	D.coSwitchBtn = CM.coGuiButton("Switch to it", function()
		if not D.coSel or D.coSel == D.coMine then return end
		local pw = CM.coGuiGet(D.coSelPwInput)
		CM.coGuiSend("CMSWITCH " .. D.coSel .. (pw ~= "" and (" " .. pw) or ""))
		CM.coGuiClear(D.coSelPwInput)
		D.coHint = "switching..."
	end, "mpDashPrimary")
	D.coDelBtn = CM.coGuiButton("Delete...", function()
		if not D.coSel or D.coSel == D.coMine then return end
		D.coDelOpen = true; D.coDelInto = D.coMine
	end)
	D.coSelActions = CM.coGuiBox({ D.coSwitchBtn, D.coDelBtn }, "mpCompanySelActions")
	D.coSelOpenText = CM.coGuiText("", "mpCoDim")
	D.coAccessBtn, D.coAccessTv = CM.coGuiButtonTv("Deny", function()
		local me = D.coState and D.coMine and D.coState.byId[D.coMine]
		if D.coSel and D.coSel ~= D.coMine and me then
			CM.coGuiSend("CMOPEN " .. D.coSel .. " " .. (CM.coGuiOpenFor(me.open, D.coSel) and "0" or "1"))
		end
	end)
	D.coSelOpenRow = CM.coGuiBox({ D.coSelOpenText, D.coAccessBtn }, "mpCompanySelOpen")
	D.coSelBox = CM.coGuiBox({ D.coSelText, D.coSelPwRow, D.coSelActions, D.coSelOpenRow }, "mpCompanySelected", "VERTICAL")
	V:addItem(D.coSelBox)
	-- delete: who takes over
	D.coDelText = CM.coGuiText("")
	D.coDelPickL = gui().layout.BoxLayout.new("HORIZONTAL")
	D.coDelPick = gui().comp.Component.new("mpCompanyDelPick")
	D.coDelPick:setLayout(D.coDelPickL)
	D.coDelNow, D.coDelNowTv = CM.coGuiButtonTv("Delete", function()
		if not D.coSel or not D.coDelInto or D.coDelInto == D.coSel then return end
		-- nobody taking over removes everything: that needs a second, deliberate click
		if D.coDelInto == 0 and not D.coDelArmed then D.coDelArmed = true; return end
		D.coDelArmed = false
		local pw = CM.coGuiGet(D.coSelPwInput)
		CM.coGuiSend("CMDEL " .. D.coSel .. " " .. D.coDelInto .. (pw ~= "" and (" " .. pw) or ""))
		CM.coGuiClear(D.coSelPwInput)
		D.coDelOpen = false
		D.coHint = "deleting..."
	end, "mpDashPrimary")
	D.coDelBox = CM.coGuiBox({ D.coDelText,
		CM.coGuiBox({ CM.coGuiText("Everything goes to"), D.coDelPick }, "mpCompanyDelInto"),
		CM.coGuiBox({ D.coDelNow, CM.coGuiButton("Cancel", function() D.coDelOpen = false; D.coDelArmed = false end) }, "mpCompanyDelActions") },
		"mpCompanyDelete", "VERTICAL")
	V:addItem(D.coDelBox)
	-- the note
	D.coNote = CM.coGuiText("", "mpCoDim")
	V:addItem(D.coNote)
	D.coBox = gui().comp.Component.new("mpCompanies")
	D.coBox:setLayout(V)
	box:addItem(D.coBox)
end

-- the delete dialog's "goes to" choice: every other company; rebuilt only when that set changes
function CM.coGuiDelPick(D, st)
	local items = {}
	for _, it in ipairs(st.list) do if it.cid ~= D.coSel then items[#items + 1] = it end end
	items[#items + 1] = { cid = 0, name = "Nobody: everything is removed", nobody = true }
	local sig = {}
	for _, it in ipairs(items) do sig[#sig + 1] = it.cid .. "=" .. it.name end
	sig = table.concat(sig, "|") .. "|sel=" .. tostring(D.coSel)
	if sig == D.coDelSig then return end
	D.coDelSig, D.coDelItems = sig, items
	local cb = gui().comp.ComboBox.new()
	for _, it in ipairs(items) do cb:addItem(it.name .. ((not it.nobody and it.cid == D.coMine) and "  (yours)" or "") .. (it.locked and "  (locked)" or "")) end
	D.coDelBuilding = true
	cb:onIndexChanged(function(i)
		if D.coDelBuilding then return end
		local it = D.coDelItems and D.coDelItems[(tonumber(i) or -1) + 1]
		if it then D.coDelInto = it.cid; D.coDelArmed = false end
	end)
	if D.coDelCombo then
		if not pcall(function() D.coDelPickL:removeItem(D.coDelCombo) end) then CM.coGuiShow(D.coDelCombo, false) end
	end
	D.coDelPickL:addItem(cb)
	D.coDelCombo = cb
	local at = nil
	for i, it in ipairs(items) do if it.cid == D.coDelInto then at = i - 1 end end
	if not at and #items > 0 then at = 0; D.coDelInto = items[1].cid end
	if at then pcall(function() cb:setSelected(at, false) end) end
	D.coDelBuilding = false
end

-- ---------- refresh (every GUI tick the tab is open) ----------
function CM.coGuiRefresh(D, kv, guiTick)
	if not D.coBox or not kv then return end
	if (guiTick % 30) == 0 or not D.coNamesRead then D.coNamesRead = true; pcall(CM.readPlayerNames) end
	local st = CM.coGuiParse(kv)
	D.coState, D.coMine = st, st.mine
	local me = st.mine and st.byId[st.mine]
	-- your company
	CM.coGuiSetClass(D, "swMine", D.coSwMine, "mpCo" .. CM.cmColorClass(me and me.color or 1))
	local mineText = me and me.name or (st.joined and "-" or "joining the session...")
	if st.mode ~= "companies" and me then mineText = me.name .. "  (shared by everyone)" end
	CM.coGuiSetText(D, "mineName", D.coNameText, " " .. mineText .. "   ")
	CM.coGuiShow(D.coSetToggle, me ~= nil)
	CM.coGuiSetText(D, "setToggle", D.coSetToggleTv, D.coSetOpen and "CLOSE" or "SETTINGS")
	D.coMigShown = st.migrated
	CM.coGuiSetText(D, "mig", D.coMigText, CM.coGuiWrap(st.migrated or "", 46))
	CM.coGuiShow(D.coMigBox, st.migrated ~= "" and st.migrated ~= D.coMigSeen)
	-- the selection survives every refresh; a deleted company falls back to yours
	if not D.coSel or not st.byId[D.coSel] then D.coSel = st.mine end
	-- the rows
	for i, row in ipairs(D.coRows or {}) do
		local it = st.list[i]
		row.cid = it and it.cid or nil
		CM.coGuiShow(row.c, it ~= nil)
		if it then
			CM.coGuiSetClass(D, "rowSw" .. i, row.sw, "mpCo" .. CM.cmColorClass(it.color))
			CM.coGuiSetClass(D, "rowSel" .. i, row.c, it.cid == D.coSel and "mpCoSel" or "mpCoRow")
			CM.coGuiSetText(D, "rowTv" .. i, row.tv, it.name)
			CM.coGuiSetText(D, "rowInfo" .. i, row.info, CM.coGuiWho(it, st))
		end
	end
	local more = #st.list - #(D.coRows or {})
	CM.coGuiSetText(D, "more", D.coMoreText, more > 0 and ("+" .. more .. " more") or "")
	CM.coGuiShow(D.coMoreText, more > 0)
	-- the selected company, when it is another one
	local sel = D.coSel and st.byId[D.coSel]
	local other = sel ~= nil and sel.cid ~= st.mine
	CM.coGuiShow(D.coSelBox, other and not D.coDelOpen)
	if other then
		CM.coGuiSetText(D, "selText", D.coSelText, sel.name:upper())
		CM.coGuiShow(D.coSelPwRow, sel.locked)
		local showOpen = st.mode == "companies" and me ~= nil
		CM.coGuiShow(D.coSelOpenRow, showOpen)
		if showOpen then
			local allowed = CM.coGuiOpenFor(me.open, sel.cid)
			CM.coGuiSetText(D, "selOpen", D.coSelOpenText, "Its vehicles may " .. (allowed and "" or "not ") .. "stop at your stations")
			CM.coGuiSetText(D, "access", D.coAccessTv, allowed and "DENY" or "ALLOW")
		end
	end
	-- delete (replaces the selected company's actions while open)
	local delOpen = D.coDelOpen and other
	CM.coGuiShow(D.coDelBox, delOpen)
	if delOpen then
		CM.coGuiDelPick(D, st)
		local text
		if D.coDelInto == 0 and D.coDelArmed then
			text = "REALLY DELETE EVERYTHING? Every vehicle, line, building, road and track of " .. sel.name
				.. " is removed, its money and loan are gone. This cannot be undone. Click DELETE EVERYTHING again to do it."
		elseif D.coDelInto == 0 then
			text = "Delete " .. sel.name .. " and remove everything it has: vehicles are sold, lines, buildings, roads and tracks removed, money and loan dropped."
		else
			text = "Delete " .. sel.name .. "? Its vehicles, lines, stations, money and loan go to the company chosen below."
		end
		CM.coGuiSetText(D, "delText", D.coDelText, CM.coGuiWrap(text .. (#sel.playing > 0 and " Someone is playing it: the game will refuse." or "")))
		CM.coGuiSetText(D, "delNow", D.coDelNowTv, D.coDelInto == 0 and (D.coDelArmed and "DELETE EVERYTHING - SURE?" or "DELETE EVERYTHING") or "DELETE")
		CM.coGuiSetClass(D, "delNowCls", D.coDelNow, D.coDelInto == 0 and "mpCoDanger" or "mpDashPrimary")
	end
	-- new company / settings
	CM.coGuiShow(D.coNewBox, D.coNewOpen)
	CM.coGuiShow(D.coNewToggle, not D.coNewOpen)
	CM.coGuiShow(D.coSetBox, D.coSetOpen and me ~= nil)
	if D.coNewOpen then
		CM.coGuiMarkColor(D, "coNew", D.coNewColor, st, nil)
		CM.coGuiSetText(D, "newPaint", D.coNewPaintTv, D.coNewPaint and "PAINT VEHICLES: ON" or "PAINT VEHICLES: OFF")
	end
	if D.coSetOpen and me then
		CM.coGuiMarkColor(D, "coSet", me.color, st, me.cid)
		CM.coGuiSetText(D, "paint", D.coPaintTv, me.paint and "PAINT VEHICLES: ON" or "PAINT VEHICLES: OFF")
		-- one password button: set what is typed, or remove the lock when the field is empty
		local typed = CM.coGuiGet(D.coPwInput) ~= ""
		CM.coGuiShow(D.coPwBtn, typed or me.locked)
		CM.coGuiSetText(D, "pwBtn", D.coPwBtnTv, typed and (me.locked and "CHANGE" or "SET") or "REMOVE")
		CM.coGuiSetText(D, "openText", D.coOpenText, "Your stations: open to " .. CM.coGuiOpenText(me.open, st))
		CM.coGuiSetText(D, "openBtn", D.coOpenBtnTv, me.open == "*" and "CLOSE TO ALL" or "OPEN TO ALL")
	end
	-- the note: a local hint until the sim says something new; the loading wait wins
	if st.note ~= "" and st.note ~= D.coNoteSeen then D.coNoteSeen = st.note; D.coHint = nil end
	if (guiTick % 30) == 0 and CM.cmLoadingPlayers then
		local loading = CM.cmLoadingPlayers()
		D.coLoadingNote = (#loading > 0) and CM.cmLoadingNote(loading) or nil
	end
	CM.coGuiSetText(D, "note", D.coNote, CM.coGuiWrap(D.coHint or D.coLoadingNote or st.note or ""))
end

return {}
end
