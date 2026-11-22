"""Offline check (Lua 5.2): free company colours (2026-09-27).

A company's colour is exact where it is drawn directly (the paint, the vehicle
icons, the minimap) and the nearest style class where the game styles by class
(station icons, foreign windows, the COMPANIES tab swatches). The classes are
built twice -- mp/companies.lua (CM.cmClassRGB, what the tab and the perms file
name) and the style sheet (!mpCoN, what the game draws) -- from the same numbers.
This runs both and asserts every class has the same colour in each, that the
picker's grid is where CM.cmGridClass says, and that the perms / map lines carry
the class and the exact RRGGBB the slice and the minimap read.

    python tools/company_color_test.py
"""
import os
import sys
from lupa import LuaRuntime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(ROOT, "mod", "mp_lockstep_1", "res", "scripts", "mp")
SHEET = os.path.join(ROOT, "mod", "mp_lockstep_1", "res", "config", "style_sheet", "mp_lockstep.lua")

fails = []
def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(name)

lua = LuaRuntime(unpack_returned_tuples=True)
COMP = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
SHARED = open(os.path.join(MP, "shared_infra.lua"), encoding="utf-8").read()
CM = lua.execute(r'''
local COMP, SHARED = ...
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
api = { gui = {} }
game = {}
local CM = { ticks = 0, escName = function(s) return s end, unescName = function(s) return s end }
assert(load(COMP, "@companies.lua"))()(CM, { INSTANCE = "a", BASE = "", IDENTITY_FILE = "none" }, function() end)
return CM
''', COMP, SHARED)

# the style sheet, with the game's stylesheetutil stubbed: every rule it adds
rules = lua.execute(r'''
local SHEET = ...
package.loaded["stylesheetutil"] = {
  makeAdder = function(result) return function(sel, props) result[#result + 1] = { sel = sel, props = props } end end,
  makeColor = function(r, g, b, a) return { r / 255, g / 255, b / 255, (a or 255) / 255 } end,
}
local env = setmetatable({}, { __index = _G })
local chunk = assert(load(SHEET, "@mp_lockstep.lua", "t", env))
chunk()
local out = {}
for _, r in ipairs(env.data()) do
  local n = r.sel:match("^!mpCo(%d+)$")
  if n then local c = r.props.backgroundColor; out[tonumber(n)] = { c[1], c[2], c[3] } end
end
return out
''', open(SHEET, encoding="utf-8").read())

n = int(CM.CM_CLASSES)
check("326 classes: the palette's 200, 24 hues x 5 shades, 6 greys", n == 326, str(n))
sheet = {int(k): tuple(v.values()) for k, v in rules.items()}
check("the style sheet defines every class", sorted(sheet) == list(range(1, n + 1)), f"{len(sheet)} classes")
diff = []
for i in range(1, n + 1):
    r, g, b = CM.cmClassRGB(i)
    s = sheet.get(i)
    if not s or tuple(round(x * 255) for x in s) != (r, g, b):
        diff.append(f"{i}: lua {(r, g, b)} sheet {s}")
check("every class has the same colour in companies.lua and the style sheet", not diff, "; ".join(diff[:5]))

# the grid: the hue columns and shade rows the picker shows
check("grid class of hue 1 shade 1 is 201, the last grey 326",
      CM.cmGridClass(1, 1) == 201 and CM.cmGridClass(24, 5) == 320 and CM.cmGridClass(0, 1) == 321 and CM.cmGridClass(0, 6) == 326)
r, g, b = CM.cmClassRGB(CM.cmGridClass(1, 3))
check("hue 1 is red", r > 200 and g < 40 and b < 40, str((r, g, b)))
r, g, b = CM.cmClassRGB(CM.cmGridClass(9, 3))
check("hue 9 (120 degrees) is green", g > 200 and r < 40 and b < 40, str((r, g, b)))
check("the greys are grey", len({CM.cmClassRGB(CM.cmGridClass(0, k)) for k in range(1, 7)}) == 6 and
      all(len(set(CM.cmClassRGB(CM.cmGridClass(0, k)))) == 1 for k in range(1, 7)))

# the palette's 20 distinct colours pass the similarity rule against each other
near = [(i, j) for i in range(1, 21) for j in range(i + 1, 21) if CM.cmColorDistance(i, j) < CM.CM_COLOR_NEAR]
check("no two of the 20 distinct colours count as alike", not near, str(near))

# a stored colour -> class, exact and hex
RGB = int(CM.CM_RGB)
v = RGB + 0x336699
check("an exact colour reads back", tuple(CM.cmColorRGB(v)) == (0x33, 0x66, 0x99) and CM.cmColorHex(v) == "#336699")
check("a palette index still reads as its colour", tuple(CM.cmColorRGB(1)) == (230, 25, 75) and CM.cmColorClass(1) == 1)
check("out-of-range colours are refused", CM.cmClampColor(0) is None and CM.cmClampColor(201) is None and CM.cmClampColor(RGB + 0x1000000) is None)

if fails:
    print(f"{len(fails)} FAILED: {fails}")
    sys.exit(1)
print("ALL PASS: free colours draw alike in the registry and the style sheet")
