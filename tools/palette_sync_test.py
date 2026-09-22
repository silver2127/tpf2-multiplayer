"""The company colour palette is defined in FIVE places and they must match exactly:
  - native/linux/src/panel_linux.cpp CoColor       (native Linux lobby chips)
  - native/src/menu_hook.cpp        coColor        (lobby chips)
  - native/src/slice_hook.cpp       IconCompanyColor (icon / station-label / window tints)
  - mod/.../scripts/mp/companies.lua CM.cmCompanyColor (vehicle paint)
  - mod/.../config/style_sheet/mp_lockstep.lua companyColor (!mpCoN / !mpWinCoN classes)

A drift between them means a company shows one colour on its icon and another on its
chip/paint. This parses the fixed palette out of each and asserts they are identical,
and that the golden-angle overflow starts at the same company id (cid - N-1) in all five.

    python tools/palette_sync_test.py
"""
from pathlib import Path
import re
from slice_source import slice_source   # the source with native/src/slice/*.inl inlined

repo = Path(__file__).resolve().parents[1]


def triples(text, pat):
    return [tuple(int(x) for x in m) for m in re.findall(pat, text)]


menu = (repo / "native/src/menu_hook.cpp").read_text(encoding="utf-8")
slice_ = slice_source(repo)
comp = (repo / "mod/mp_lockstep_1/res/scripts/mp/companies.lua").read_text(encoding="utf-8")
ss = (repo / "mod/mp_lockstep_1/res/config/style_sheet/mp_lockstep.lua").read_text(encoding="utf-8")

# menu_hook: RGB(r,g,b) inside `first[N] = { ... }`
menu_arr = re.search(r"static const COLORREF first\[\d+\]\s*=\s*\{(.*?)\};", menu, re.S)[1]
menu_pal = triples(menu_arr, r"RGB\((\d+),\s*(\d+),\s*(\d+)\)")

# slice_hook: {r,g,b} inside `first[N][3] = { ... }`
slice_arr = re.search(r"static const int first\[\d+\]\[3\]\s*=\s*\{(.*?)\};", slice_, re.S)[1]
slice_pal = triples(slice_arr, r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")

# companies.lua: CM.CM_COLORS = { {r,g,b}, ... }
comp_arr = re.search(r"CM\.CM_COLORS\s*=\s*\{(.*?)\}\s*$", comp, re.M)[1]
comp_pal = triples(comp_arr, r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")

# style sheet: local FIRST = { { r, g, b }, ... }
ss_arr = re.search(r"local FIRST\s*=\s*\{(.*?)\}\s*$", ss, re.M)[1]
ss_pal = triples(ss_arr, r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")

linux = (repo / "native/linux/src/panel_linux.cpp").read_text(encoding="utf-8")
linux_arr = re.search(r"static const Rgb first\[\d+\]\s*=\s*\{(.*?)\};", linux, re.S)[1]
linux_pal = triples(linux_arr, r"rgb\((\d+),\s*(\d+),\s*(\d+)\)")

fails = []
pals = {"panel_linux": linux_pal, "menu_hook": menu_pal, "slice_hook": slice_pal, "companies.lua": comp_pal, "style_sheet": ss_pal}
for name, p in pals.items():
    print(f"{name}: {len(p)} colours")
    if len(p) != 20:
        fails.append(f"{name}: expected 20 colours, parsed {len(p)}")

ref = menu_pal
for name, p in pals.items():
    if p != ref:
        fails.append(f"{name} palette differs from menu_hook:\n  {name}={p}\n  menu ={ref}")

# the golden-angle overflow offset must be the same in all five, and = len+1
n = len(ref)
off = n + 1
for name, text, pat in (
    ("panel_linux", linux, rf"\(cid - {off}\) \* 137\.508"),
    ("menu_hook", menu, rf"\(cid - {off}\) \* 137\.508"),
    ("slice_hook", slice_, rf"\(cid - {off}\) \* 137\.508"),
    ("companies.lua", comp, rf"\(cid - {off}\) \* 137\.508"),
    ("style_sheet", ss, rf"\(cid - {off}\) \* 137\.508"),
):
    if not re.search(pat, text):
        fails.append(f"{name}: golden-angle overflow is not (cid - {off}) -- palette size {n} and offset disagree")

# Native fixed-colour bounds must agree with the palette length too.
for name, source in (("panel_linux", linux), ("menu_hook", menu), ("slice_hook", slice_)):
    if not re.search(rf"cid >= 1 && cid <= {n}\)", source):
        fails.append(f"{name}: fixed-colour bound differs from palette size {n}")

if fails:
    print("FAILED:")
    for f in fails:
        print("  " + f)
    raise SystemExit(1)
print(f"palette sync: ok -- {n} identical colours across all five, overflow at cid {off}")
