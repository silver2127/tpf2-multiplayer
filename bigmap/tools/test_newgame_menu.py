"""Offline test for the New Game density levels (tpf2_bigmap.dll).

Four halves; none needs the game running.

1. THE PATCHER, through the DLL's BigmapTestSyncBaseMod export -- the exact code
   the plugin runs at start -- on a scratch copy of the stock base_mod.lua:
   patch, a second start, restore, an earlier foreign patch, a game update,
   a file whose anchors changed or repeat, CRLF, and a patched file with no
   backup to restore from.
2. THE UNINSTALL RESTORE, rundll32 on the plugin in a game-shaped folder, the
   way installer\\Package.wxs runs it.
3. THE TOWNS STUB, the machine code the plugin redirects the Towns switch to,
   built by BigmapTestBuildTownStub with a plain `ret` as its way back and called
   as float(int): 3 stays 0.5, the added levels give Medium x scale, anything
   else stays 1.0.
4. THE PATCHED LUA, under lupa's Lua 5.2 (the game's version) with the game
   globals stubbed: the Towns, Number of industries and Industry density target
   lists end with the added levels, and runFn scales the start and target
   industry density for every level, stock ones unchanged.

Needs `pip install lupa` and out\\tpf2_bigmap.dll (build.bat). The stock text
comes from --stock, else <game>\\res\\config\\base_mod.lua.bigmapbak, else the
game's base_mod.lua when it carries no patch (TPF2_GAME_DIR overrides the Steam
path). Exits non-zero on any failure.
"""
import argparse
import ctypes
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAME = Path(os.environ.get("TPF2_GAME_DIR",
                           r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2"))

# BASEMOD_* in src/bigmap.cpp
ALREADY, PATCHED, STOCK, RESTORED = 0, 1, 2, 3
ERR_READ, ERR_FOREIGN, ERR_ANCHOR, ERR_WRITE = -1, -2, -3, -4

# DENSITY_LEVELS in src/bigmap.cpp: scales relative to Medium (towns 0.3, industries 0.6)
LEVELS = [
    ("Reduced (x0.50)", 0.50),
    ("Sparse (x0.30)", 0.30),
    ("Megalomaniac count at 56 km (x0.18)", 0.18),
    ("Minimal (x0.10)", 0.10),
    ("Megalomaniac count at 112 km (x0.046)", 0.046),
    ("Megalomaniac count at 160 km (x0.022)", 0.022),
]
LABELS = [label for label, _ in LEVELS]
SCALES = [scale for _, scale in LEVELS]
STOCK4 = ["Low", "Medium", "High", "Very high"]

failures = []


def check(name, cond, detail=""):
    print(("  ok    " if cond else "  FAIL  ") + name + (f"   [{detail}]" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def find_stock(arg):
    cands = [Path(arg)] if arg else [GAME / "res" / "config" / "base_mod.lua.bigmapbak",
                                     GAME / "res" / "config" / "base_mod.lua"]
    for c in cands:
        if c.is_file() and b"bigmap" not in c.read_bytes():
            print(f"stock text: {c}")
            return c.read_bytes()
    sys.exit("no clean stock base_mod.lua found; pass --stock <path>")


def only_inserts(before, after):
    """True when `after` is `before` with text added and none removed or changed."""
    ops = difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes()
    return all(tag in ("equal", "insert") for tag, *_ in ops)


def test_patcher(sync, stock):
    print("patcher")
    with tempfile.TemporaryDirectory() as td:
        f = Path(td) / "base_mod.lua"
        bak = Path(td) / "base_mod.lua.bigmapbak"
        why = ctypes.create_string_buffer(512)

        def run(want):
            rc = sync(str(f), want, why, len(why))
            return rc, why.value.decode("utf-8", "replace")

        f.write_bytes(stock)
        rc, w = run(1)
        patched = f.read_bytes()
        check("stock -> patched", rc == PATCHED, f"rc={rc} {w}")
        check("backup holds the stock text", bak.is_file() and bak.read_bytes() == stock)
        check("patch only adds text", only_inserts(stock.decode(), patched.decode()))
        check("every level is added to all three lists",
              all(patched.count(f'_("{label}")'.encode()) == 3 for label in LABELS))

        rc, w = run(1)
        check("second start writes nothing", rc == ALREADY and f.read_bytes() == patched, f"rc={rc} {w}")

        rc, w = run(0)
        check("levels off -> stock restored", rc == RESTORED and f.read_bytes() == stock, f"rc={rc} {w}")
        rc, w = run(0)
        check("restoring stock twice is a no-op", rc == STOCK and f.read_bytes() == stock, f"rc={rc} {w}")

        foreign = stock.replace(b'local osutil = require "osutil"\n',
                                b'local osutil = require "osutil"\n-- [bigmap density test] by hand\n', 1)
        f.write_bytes(foreign)
        bak.write_bytes(stock)
        rc, w = run(1)
        check("an earlier hand-made patch is replaced by ours",
              rc == PATCHED and f.read_bytes() == patched and bak.read_bytes() == stock, f"rc={rc} {w}")

        extra = b"\n-- a line a game update added\n"
        f.write_bytes(stock + extra)
        rc, w = run(1)
        check("a game update is re-patched, backup refreshed",
              rc == PATCHED and f.read_bytes() == patched + extra and bak.read_bytes() == stock + extra,
              f"rc={rc} {w}")

        f.write_bytes(patched)
        bak.unlink()
        rc, w = run(1)
        check("patched file with no backup is refused, untouched",
              rc == ERR_FOREIGN and f.read_bytes() == patched, f"rc={rc} {w}")
        rc, w = run(0)
        check("...and cannot be restored either", rc == ERR_FOREIGN and f.read_bytes() == patched, f"rc={rc} {w}")

        moved = stock.replace(b"local industryFreq = { .4, .6, .8, 1.0 }", b"local industryFreq = {.4, .6, .8, 1.0}")
        check("(the mutation changed the file)", moved != stock)
        f.write_bytes(moved)
        rc, w = run(1)
        check("anchor gone: refused, nothing written",
              rc == ERR_ANCHOR and "industryFreq" in w and f.read_bytes() == moved and not bak.exists(),
              f"rc={rc} {w}")

        dup = stock.replace(b'local osutil = require "osutil"\n',
                            b'local osutil = require "osutil"\nlocal osutil = require "osutil"\n', 1)
        f.write_bytes(dup)
        rc, w = run(1)
        check("anchor repeated: refused", rc == ERR_ANCHOR and "more than once" in w and f.read_bytes() == dup,
              f"rc={rc} {w}")

        crlf = stock.replace(b"\n", b"\r\n")
        f.write_bytes(crlf)
        rc, w = run(1)
        check("CRLF file: refused with the reason", rc == ERR_ANCHOR and "CRLF" in w, f"rc={rc} {w}")

        f.unlink()
        rc, w = run(1)
        check("missing file: read error", rc == ERR_READ, f"rc={rc} {w}")

        check("no temp files left behind", not list(Path(td).glob("*.bigmaptmp")))
    return patched


def test_uninstall_restore(dll_path, stock, patched):
    print("uninstall restore (rundll32, as installer\\Package.wxs runs it)")
    rundll = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "rundll32.exe"
    with tempfile.TemporaryDirectory() as td:
        game = Path(td) / "Transport Fever 2"
        (game / "plugins").mkdir(parents=True)
        (game / "res" / "config").mkdir(parents=True)
        dll = game / "plugins" / "tpf2_bigmap.dll"
        shutil.copy2(dll_path, dll)
        f = game / "res" / "config" / "base_mod.lua"
        bak = f.with_name("base_mod.lua.bigmapbak")
        cmd = f'"{rundll}" "{dll}",BigmapRestoreStockBaseMod'

        f.write_bytes(patched)
        bak.write_bytes(stock)
        r = subprocess.run(cmd, timeout=60)
        check("a patched file gets the stock text back", f.read_bytes() == stock, f"exit={r.returncode}")
        r = subprocess.run(cmd, timeout=60)
        check("a stock file is left alone", f.read_bytes() == stock and bak.read_bytes() == stock,
              f"exit={r.returncode}")
        f.write_bytes(patched)
        bak.unlink()
        r = subprocess.run(cmd, timeout=60)
        check("no backup: the file is left as it is", f.read_bytes() == patched, f"exit={r.returncode}")


def test_town_stub(dll):
    print("Towns switch stub (the machine code the plugin jumps to)")
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    ret = k32.VirtualAlloc(None, 16, 0x3000, 0x40)      # MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    ctypes.memmove(ret, b"\xC3", 1)                     # the stub's "way back": ret to the caller
    build = dll.BigmapTestBuildTownStub
    build.restype = ctypes.c_void_p
    build.argtypes = [ctypes.c_void_p]
    stub = build(ret)
    check("stub built", bool(stub))
    if not stub:
        return
    fn = ctypes.CFUNCTYPE(ctypes.c_float, ctypes.c_int)(stub)
    want = {3: 0.5}
    for k, s in enumerate(SCALES):
        want[4 + k] = 0.3 * s
    for idx in (-1, 0, 1, 2, 4 + len(SCALES), 11, 1000):
        want[idx] = 1.0
    for idx, w in sorted(want.items()):
        got = fn(idx)
        check(f"Towns index {idx} -> {got:.4f}", abs(got - w) < 1e-6, f"want {w}")


def test_row_shapes(dll):
    print("ratio shapes for the added size rows (DeriveShape)")
    fn = dll.BigmapTestDeriveShape
    fn.restype = None
    fn.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]

    def shape(side, fmt, cap=512):
        x, y = ctypes.c_int(), ctypes.c_int()
        fn(side, fmt, cap, ctypes.byref(x), ctypes.byref(y))
        return x.value, y.value

    got = [shape(128, f) for f in range(5)]
    check("32 km row (128): 1:1 .. 1:5", got == [(128, 128), (90, 180), (74, 222), (64, 256), (58, 290)], str(got))
    got = [shape(224, f) for f in range(5)]
    check("56 km row (224): 1:1 .. 1:5", got == [(224, 224), (158, 316), (130, 390), (112, 448), (100, 500)], str(got))
    got = [shape(512, f) for f in range(5)]
    check("128 km row (512): narrow ratios capped at 512, ratio kept",
          got == [(512, 512), (256, 512), (170, 510), (128, 512), (102, 510)], str(got))
    bad = []
    for side in (128, 160, 192, 224, 256, 320, 384, 448, 512):
        for f in range(5):
            k = f + 1
            x, y = shape(side, f)
            capped = y > 512 or 2 * (int((side / k ** 0.5) / 2 + 0.5)) * k > 512
            area_ok = capped or abs(x * y - side * side) <= 0.12 * side * side
            if y != k * x or x % 2 or y > 512 or x < 2 or not area_ok:
                bad.append((side, f, x, y))
    check("every row and ratio: exact 1:k, even, within 512, about the square's area", not bad, str(bad))
    got = shape(96, 1), shape(96, 3)
    check("close to the game's own Megalomaniac shapes (66x132, 48x192)",
          abs(got[0][0] - 66) <= 2 and got[1] == (48, 192), str(got))


LUA_STUBS = r"""
local function noop() end
UNSTUBBED = {}
setmetatable(_G, { __index = function(_, k) UNSTUBBED[#UNSTUBBED + 1] = tostring(k); return noop end })
package.preload["filefilterutil"] = function()
    local yes = function() return true end
    return { package = { base = yes, mod = function() return false end },
             util = { combineOr = function() return yes end, combineAnd = function() return yes end } }
end
package.preload["metadatautil"] = function() return setmetatable({}, { __index = function() return noop end }) end
package.preload["osutil"] = function() return { getConsoleGen = function() return 3 end } end
function _(s) return s end
function pGetText(ctx, s) return s end
function addFileFilter() end
function addModifier() end

local function auto()
    return setmetatable({}, { __index = function(t, k) local v = auto(); rawset(t, k, v); return v end })
end
function freshGame()
    game = { config = auto() }
    local c = game.config
    c.locations.town = { maxNumberPerArea = 0.2 }
    c.locations.industry = { maxNumberPerArea = 0.8, targetMaxNumberPerArea = 0.8 }
    c.costs = { terrainLower = 1.0, terrainRaise = 1.0 }
end
"""

STOCK_PARAMS = {
    "economy.industryDevelopment.closureProbability": 0,
    "locations.industry.maxNumberPerArea": 1,
    "locations.industry.targetMaxNumberPerArea": 2,
    "economy.townDevelopment.cargoNeedsPerTown": 1,
    "advancedOptions.maximumLoanScale": 2,
    "advancedOptions.loanInterestScale": 2,
    "advancedOptions.infrastructurePurchaseCostScale": 2,
    "advancedOptions.publicTransportDestinationsSensitivityScale": 4,
    "advancedOptions.privateTransportDestinationsSensitivityScale": 4,
    "advancedOptions.cargoSupplySensitivityScale": 4,
    "advancedOptions.emissionSensitivityScale": 4,
    "advancedOptions.trafficSpeedSensitivityScale": 4,
    "advancedOptions.stationOverflowSensitivityScale": 4,
}


def test_lua(patched):
    print("patched base_mod.lua under Lua 5.2")
    import lupa.lua52 as lupa52
    L = lupa52.LuaRuntime(unpack_returned_tuples=True)
    L.execute(LUA_STUBS)
    L.execute(patched.decode("utf-8"))
    g = L.globals()
    d = g.data()
    params = d["info"]["params"]
    by_key = {params[i]["key"]: params[i] for i in range(1, len(params) + 1)}

    def values(key):
        v = by_key[key]["values"]
        return [v[i] for i in range(1, len(v) + 1)]

    towns = values("locations.towns.frequency")
    inds = values("locations.industry.maxNumberPerArea")
    tgt = values("locations.industry.targetMaxNumberPerArea")
    check("Towns: the 4 stock levels, then ours", towns == STOCK4 + LABELS, str(towns))
    check("Number of industries: the 4 stock levels, then ours", inds == STOCK4 + LABELS, str(inds))
    check("Industry density target: Disabled + the 4 stock levels, then ours",
          tgt == ["Disabled"] + STOCK4 + LABELS, str(tgt))
    check("no params added", len(by_key) == len(set(by_key)) and not any("bigmap" in k for k in by_key))
    check("defaults unchanged (Medium)",
          by_key["locations.towns.frequency"]["defaultIndex"] == 1
          and by_key["locations.industry.maxNumberPerArea"]["defaultIndex"] == 1)

    run_fn = d["runFn"]
    settings = L.table_from({"climate": "temperate", "vehicles": "europe", "nameList": "europe",
                             "environment": "temperate", "difficulty": "easy"})
    freq = [0.4, 0.6, 0.8, 1.0] + [0.6 * s for s in SCALES]

    def generate(start, target):
        g.freshGame()
        p = {**STOCK_PARAMS, "locations.industry.maxNumberPerArea": start,
             "locations.industry.targetMaxNumberPerArea": target}
        run_fn(settings, L.table_from({"": L.table_from(p)}))
        loc = g.game["config"]["locations"]
        dev = g.game["config"]["economy"]["industryDevelopment"]
        return (loc["town"]["maxNumberPerArea"], loc["industry"]["maxNumberPerArea"],
                loc["industry"]["targetMaxNumberPerArea"], dev["spawnIndustries"])

    def near(a, b):
        return abs(a - b) < 1e-9

    for idx, name in enumerate(STOCK4 + LABELS):
        t, i, tg, spawn = generate(idx, idx + 1)       # the page stores target = start + 1
        want = 0.8 * freq[idx]
        check(f"industries {idx} {name!r}: start {i:.4f} target {tg:.4f}",
              near(i, want) and near(tg, want) and spawn is True and near(t, 0.2))
    t, i, tg, spawn = generate(6, 0)
    check("an added level with the target Disabled: target follows the start density",
          near(i, 0.8 * freq[6]) and near(tg, 0.8 * freq[6]) and spawn is False)
    unstubbed = sorted(set(g.UNSTUBBED.values()))
    if unstubbed:
        print("  note  globals the stubs did not define (answered with a no-op):", ", ".join(unstubbed))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", help="a clean stock base_mod.lua")
    ap.add_argument("--dll", default=str(REPO / "out" / "tpf2_bigmap.dll"))
    a = ap.parse_args()
    stock = find_stock(a.stock)
    dll = ctypes.WinDLL(a.dll)
    sync = dll.BigmapTestSyncBaseMod
    sync.argtypes = [ctypes.c_wchar_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    sync.restype = ctypes.c_int
    patched = test_patcher(sync, stock)
    test_uninstall_restore(a.dll, stock, patched)
    test_town_stub(dll)
    test_row_shapes(dll)
    test_lua(patched)
    print("\nRESULT:", "ALL PASS" if not failures else f"{len(failures)} FAILURE(S)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
