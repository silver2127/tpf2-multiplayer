"""Offline check (Lua 5.2): the COMPANIES tab (mp/companies_gui.lua) against a fake
api.gui, fed with the dash lines a real registry writes (company_harness).

The tab is built once and refreshed; the checks click its buttons and read what
lands in the inject file -- the only thing the GUI state can do:
  - one row per company, yours first, with its colour class, who plays it, [locked]
  - selecting a row survives a refresh (the old ComboBox reset it)
  - SWITCH TO IT / a locked company's password / DELETE... with the company that
    takes over / NEW COMPANY with name, colour, paint and password / the settings:
    rename, colour, vehicle paint on/off, password set/remove, station access
  - the note shows the sim's last word; an older save's import note shows

    python tools/company_gui_test.py
"""
import os

import lupa.lua52 as lupa

from company_harness import check, fails, Machine, Session, TOWN

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
GUI = open(os.path.join(MP, "companies_gui.lua"), encoding="utf-8").read()
COMP = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
SHARED = open(os.path.join(MP, "shared_infra.lua"), encoding="utf-8").read()

# a session to draw: Ada (a) plays Ada's company, Bob (b) Bob's, plus a locked "Vault"
ROSTER = {"a": "Ada", "b": "Bob"}
A = Machine("a", "7656100000000001", "Ada", ROSTER, chip=1)
B = Machine("b", "7656100000000002", "Bob", ROSTER, chip=2)
for m in (A, B):
    m.world({100: {"balance": 1000}}, TOWN, 100)
    m.boot()
    m.join()
s = Session([A, B])
s.pump()
s.request(B, "CMNEW 7 1 Vault")
s.pump()
vault = B.cm().cmMyCompany
s.request(B, f"CMPW {vault} x")
s.pump()
s.request(B, "CMSWITCH 2")
s.pump()
DASH = A.T.dash()

FAKE_GUI = r'''
local GUI, COMP, SHARED, BASE = ...
local W = { all = {} }
local function widget(kind, text)
  local w = { kind = kind, text = text or "", visible = true, classes = {}, items = {} }
  W.all[#W.all + 1] = w
  function w:setText(t) self.text = t end
  function w:getText() return self.text end
  function w:setStyleClassList(c) self.classes = c end
  function w:setVisible(v) self.visible = v end
  function w:setMinimumSize() end
  function w:setMaximumSize() end
  function w:setLayout(l) self.layout = l end
  function w:addItem(x) self.items[#self.items + 1] = x end
  function w:removeItem(x) for i, y in ipairs(self.items) do if y == x then table.remove(self.items, i) end end end
  function w:onClick(fn) self.click = fn end
  function w:onIndexChanged(fn) self.changed = fn end
  function w:setSelected(i) self.selected = i end
  return w
end
api = { gui = {
  comp = {
    TextView = { new = function(t) return widget("TextView", t) end },
    Button = { new = function(tv) local b = widget("Button"); b.tv = tv; return b end },
    TextInputField = { new = function() return widget("Input", "") end },
    ComboBox = { new = function() return widget("ComboBox") end },
    Component = { new = function(name) local c = widget("Component"); c.name = name; return c end },
  },
  layout = { BoxLayout = { new = function() return widget("Layout") end } },
  util = { Size = { new = function() return {} end } },
} }
game = {}
local function esc(s) return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function unesc(s) return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = { INSTANCE = "a", BASE = BASE, IDENTITY_FILE = BASE .. "none" }
local CM = { ticks = 0, escName = esc, unescName = unesc }
assert(load(COMP, "@companies.lua"))()(CM, K, function() end)
assert(load(GUI, "@companies_gui.lua"))()(CM, K, function() end)
local T = { W = W, CM = CM }
local D, box = {}, widget("Layout")
T.D = D
CM.coGuiBuild(D, box)
local tick = 0
function T.refresh(dash)
  local kv = {}
  for line in dash:gmatch("[^\n]+") do
    local k, v = line:match("^(%w+)=(.*)$")
    if k == "co" then kv.coList = kv.coList or {}; kv.coList[#kv.coList + 1] = v elseif k then kv[k] = v end
  end
  tick = tick + 1
  CM.coGuiRefresh(D, kv, tick)
end
function T.button(label)
  for _, w in ipairs(W.all) do if w.kind == "Button" and w.tv and w.tv.text == label then return w end end
end
function T.click(label) local b = T.button(label); assert(b, "no button " .. label); b.click() end
local function clickTv(tv)
  for _, w in ipairs(W.all) do if w.kind == "Button" and w.tv == tv then w.click(); return true end end
  return false
end
-- the picker: a shade (1..5, then the greys 6..11) of the hue shown, or a hue (1..24)
function T.clickSwatch(prefix, i) return clickTv(D[prefix .. "ShadeTv"][i]) end
function T.clickHue(prefix, h) return clickTv(D[prefix .. "HueTv"][h]) end
function T.shadeValue(prefix, i)
  return CM.cmRgbValue(CM.cmClassRGB(D[prefix .. "ShadeCls"][i]))
end
-- how many places hold each widget: a layout's items, a component's layout, a button's label
function T.twice()
  local held, out = {}, {}
  local function hold(x) held[x] = (held[x] or 0) + 1 end
  for _, w in ipairs(W.all) do
    for _, x in ipairs(w.items) do if type(x) == "table" then hold(x) end end
    if w.layout then hold(w.layout) end
    if w.tv then hold(w.tv) end
  end
  for x, n in pairs(held) do if n > 1 then out[#out + 1] = x.kind .. ":" .. tostring(x.text) end end
  return table.concat(out, ", ")
end
function T.injected()
  local f = io.open(BASE .. "lockstep_inject_a.txt", "r")
  if not f then return "" end
  local t = f:read("*a"); f:close(); os.remove(BASE .. "lockstep_inject_a.txt")
  return t
end
function T.rows()
  local out = {}
  for i, r in ipairs(D.coRows) do
    if r.c.visible then out[#out + 1] = r.tv.text .. " | " .. r.info.text .. " | " .. tostring(r.sw.classes[1]) end
  end
  return table.concat(out, "\n")
end
return T
'''

L = lupa.LuaRuntime(unpack_returned_tuples=True)
T = L.execute(FAKE_GUI, GUI, COMP, SHARED, A.dir.replace("\\", "/") + "/")
T.refresh(DASH)
rows = T.rows()
print(rows)
lines = rows.split("\n")
check("one row per company", len(lines) == 3, str(len(lines)))
check("yours first, with its colour and 'you'", lines[0].startswith("Ada's company | you") and "mpCo" in lines[0], lines[0])
check("... and highlighted as selected", T.D.coRows[1].c.classes[1] == "mpCoSel", str(T.D.coRows[1].c.classes[1]))
check("Bob's row says who plays it", "Bob's company | Bob" in rows, rows)
check("a locked company says so", "Vault" in rows and "locked" in rows, rows)
check("your company's name is shown", "Ada's company" in T.D.coNameText.text, T.D.coNameText.text)
check("no widget is placed twice (a button's label is not also a row item)", T.twice() == "", T.twice())
check("nothing but the list is open at first", not T.D.coSelBox.visible and not T.D.coNewBox.visible and not T.D.coSetBox.visible and not T.D.coDelBox.visible)

# select Bob's row, refresh: the selection stays
bob_row = next(i for i in range(1, 4) if "Bob's company" in T.D.coRows[i].tv.text)
T.D.coRows[bob_row].btn.click()
T.refresh(DASH)
check("a selected row stays selected across a refresh", T.D.coSel == 2 and T.D.coRows[bob_row].c.classes[1] == "mpCoSel", str(T.D.coSel))
check("the selected company's panel shows", T.D.coSelBox.visible and T.D.coSelText.text == "BOB'S COMPANY", T.D.coSelText.text)
check("no password field for an open company", not T.D.coSelPwRow.visible)
T.click("SWITCH TO IT")
check("SWITCH TO IT asks for that company", T.injected() == "CMSWITCH 2\n")
check("one toggle for its vehicles at your stations", T.D.coAccessTv.text == "DENY" and "may stop" in T.D.coSelOpenText.text, T.D.coAccessTv.text)
T.click("DENY")
check("... which denies them", T.injected() == "CMOPEN 2 0\n")

# the locked company: its password goes with the switch
vault_row = next(i for i in range(1, 4) if "Vault" in T.D.coRows[i].tv.text)
T.D.coRows[vault_row].btn.click()
T.refresh(DASH)
check("a locked company asks for its password", T.D.coSelPwRow.visible)
T.D.coSelPwInput.text = "x"
T.click("SWITCH TO IT")
check("... which goes with the switch, and the field is cleared", T.injected() == f"CMSWITCH {vault} x\n" and T.D.coSelPwInput.text == "")

# delete it into Bob's company
T.click("DELETE...")
T.refresh(DASH)
check("DELETE... replaces the actions with the confirmation", T.D.coDelBox.visible and not T.D.coSelBox.visible and "Delete Vault?" in T.D.coDelText.text, T.D.coDelText.text)
combo = T.D.coDelCombo
check("the takeover choice lists the other companies, then Nobody", len(list(combo["items"].values())) == 3, str(list(combo["items"].values())))
combo.changed(1)       # the second entry
into = T.D.coDelInto
T.D.coSelPwInput.text = "x"
T.click("DELETE")
check("DELETE names the company that takes over", T.injected() == f"CMDEL {vault} {into} x\n" and into in (1, 2))

# delete with nobody taking over: it is last in the list, and needs a second click
T.D.coRows[vault_row].btn.click()
T.refresh(DASH)
T.click("DELETE...")
T.refresh(DASH)
items = list(T.D.coDelCombo["items"].values())
check("'Nobody' is offered, last (never preselected)", items[-1].startswith("Nobody") and not items[0].startswith("Nobody"), str(items))
T.D.coDelCombo.changed(len(items) - 1)
T.refresh(DASH)
check("choosing nobody turns the button into DELETE EVERYTHING, in red", T.D.coDelNowTv.text == "DELETE EVERYTHING" and T.D.coDelNow.classes[1] == "mpCoDanger",
      T.D.coDelNowTv.text)
T.D.coSelPwInput.text = "x"
T.D.coDelNow.click()
T.refresh(DASH)
check("the first click only asks again", T.injected() == "" and T.D.coDelNowTv.text == "DELETE EVERYTHING - SURE?" and "REALLY DELETE EVERYTHING" in T.D.coDelText.text,
      T.D.coDelText.text)
check("the warning is broken into lines that fit the window", "\n" in T.D.coDelText.text and max(len(l) for l in T.D.coDelText.text.split("\n")) <= 52, T.D.coDelText.text)
T.D.coDelNow.click()
check("the second click deletes with nobody taking over", T.injected() == f"CMDEL {vault} 0 x\n")

# a new company
T.click("+ NEW COMPANY")
T.refresh(DASH)
check("+ NEW COMPANY opens its form (and hides itself)", T.D.coNewBox.visible and not T.D.coSetBox.visible and not T.D.coNewToggle.visible)
T.D.coNewName.text = "Blue Line"
CMT = T.CM
check("the picker has 24 hues and 5 shades + 6 greys", len(list(T.D.coNewHueTv.values())) == 24 and len(list(T.D.coNewShadeTv.values())) == 11)
# the hue of another company's colour: a shade that looks like it is marked "-"
other_color = [int(l.split(":")[1]) for l in DASH.splitlines() if l.startswith("co=") and not l.startswith("co=1:")][0]
T.clickHue("coNew", CMT.coGuiHueOf(other_color))
T.refresh(DASH)
marks = [T.D.coNewShadeTv[i].text for i in range(1, 12)]
check("colours that look like another company's are marked", " - " in marks, str(marks))
check("the hue shown is marked", T.D.coNewHueTv[CMT.coGuiHueOf(other_color)].text == " > ")
free = [i for i in range(1, 12) if T.D.coNewShadeTv[i].text == "   "][0]
T.clickSwatch("coNew", free)
want = int(T.shadeValue("coNew", free))
T.D.coNewPaintBtn.click()          # (the settings have a button of the same name)
T.refresh(DASH)
check("the chosen colour is marked", T.D.coNewShadeTv[free].text == " X ", T.D.coNewShadeTv[free].text)
check("its exact code is shown", T.D.coNewHexText.text == "now " + CMT.cmColorHex(want), T.D.coNewHexText.text)
check("the swatch shows its own class", "mpCo" + str(int(T.D.coNewShadeCls[free])) == T.D.coNewShadeTv[free].classes[1], str(T.D.coNewShadeTv[free].classes[1]))
check("the paint toggle reads off", T.D.coNewPaintTv.text == "PAINT VEHICLES: OFF", T.D.coNewPaintTv.text)
T.D.coNewPw.text = "pw 2"
T.click("CREATE")
got = T.injected()
check("CREATE sends colour, paint, the escaped name and the password", got == f"CMNEW {want} 0 Blue%20Line pw 2\n", got)

# your company's settings, beside your company's name
T.click("SETTINGS")
T.refresh(DASH)
check("SETTINGS opens them", T.D.coSetBox.visible and not T.D.coNewBox.visible and T.D.coSetToggleTv.text == "CLOSE")
T.D.coRename.text = "Ada Rail"
T.click("RENAME")
check("RENAME sends the name", T.injected() == "CMNAME 1 Ada Rail\n")
T.clickSwatch("coSet", 3)
got = T.injected()
check("a colour click sends CMCOLOR with the paint as it is", got == f"CMCOLOR 1 {int(T.shadeValue('coSet', 3))} 1\n", got)
T.D.coSetHexInput.text = "#1E90FF"
T.click("USE")
got = T.injected()
check("an exact #RRGGBB sends exactly that colour", got == f"CMCOLOR 1 {16777216 + 0x1E90FF} 1\n", got)
T.D.coSetHexInput.text = "blue"
T.click("USE")
T.refresh(DASH)
check("... and something else sends nothing and says how", T.injected() == "" and "#1E90FF" in T.D.coNote.text, T.D.coNote.text)
T.click("PAINT VEHICLES: ON")
check("the paint toggle turns it off", T.injected() == "CMCOLOR 1 1 0\n")
check("no password button while nothing is typed and nothing is locked", not T.D.coPwBtn.visible)
T.D.coPwInput.text = "s3cret"
T.refresh(DASH)
check("... SET once something is typed", T.D.coPwBtn.visible and T.D.coPwBtnTv.text == "SET", T.D.coPwBtnTv.text)
T.click("SET")
check("SET sends the password", T.injected() == "CMPW 1 s3cret\n")
check("the station line and its one toggle", T.D.coOpenText.text == "Your stations: open to everyone" and T.D.coOpenBtnTv.text == "CLOSE TO ALL",
      T.D.coOpenText.text + " / " + T.D.coOpenBtnTv.text)
T.click("CLOSE TO ALL")
check("CLOSE TO ALL closes them", T.injected() == "CMOPEN * 0\n")
check("the note is the sim's last word", T.D.coNote.text.strip() != "")

# the note about an imported older save: shown until OK, and it fits the window
MIG = "This save is from an older version: companies are handed out once as players join. If you got the wrong one, switch to yours."
DASH_MIG = "\n".join(l for l in DASH.splitlines() if not l.startswith("comig=")) + "\ncomig=" + T.CM.escName(MIG) + "\n"
T.refresh(DASH_MIG)
check("an imported save's note is shown", T.D.coMigBox.visible and "older version" in T.D.coMigText.text)
check("... broken into lines that fit beside its OK", max(len(l) for l in T.D.coMigText.text.split("\n")) <= 46, T.D.coMigText.text)
T.click("OK")
T.refresh(DASH_MIG)
check("OK hides it", not T.D.coMigBox.visible)
T.refresh(DASH)
check("no note, no box", not T.D.coMigBox.visible)

print("FAILED: " + ", ".join(fails) if fails else "ALL PASS: the COMPANIES tab shows the registry and asks for the right things")
raise SystemExit(1 if fails else 0)
