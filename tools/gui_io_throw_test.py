"""The in-game panel survives an io.open that throws (a Windows user name with accents).

The game's io.open converts its path from UTF-8 to wide and THROWS on bytes that are not
UTF-8 (native/src/datadir.h); the error value is the half-built file handle, printed as
"file (0000000000000000)". os.getenv hands back ANSI bytes, so every path built from
LOCALAPPDATA does that for an accented user name. On 0.7.1.4 one such open killed the
panel's whole refresh, every time: no window, no chat, no companies tab (2026-11-14).

Runs the real blocks of lockstep.lua (CM.openFile, CM.netDir/CM.chatSend, readDash) in
lupa.lua52 with an io.open that behaves like the game's:
  - CM.openFile returns nil instead of raising, and logs the path once
  - CM.netDir finds the lobby folder through the ASCII data folder the DLLs published
  - readDash of a peer whose file is absent returns nil instead of raising
  - an ASCII profile resolves exactly as before

    python tools/gui_io_throw_test.py
"""
from pathlib import Path
import tempfile
import unittest

from lupa.lua52 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua").read_text(encoding="utf-8")


def block(start, end):
    a = SOURCE.index(start)
    return SOURCE[a:SOURCE.index(end, a)]


a = SOURCE.index("function CM.openFile(path, mode)")
OPEN_FILE = SOURCE[a:SOURCE.index("\nend\n", a) + 5]
NET_DIR = block("function CM.netDir()", "\n\t\t\t\t-- Last n chat lines")
READ_DASH = block("local function readDash(inst)", "\n\t\t\t\t-- Which instances are in the session")

# An io.open like the game's: a path that is not valid UTF-8 throws, a valid one opens.
GAME_IO = r'''
realOpen = io.open
local fh = io.tmpfile()
local function validUtf8(s)
    local i, n = 1, #s
    while i <= n do
        local c = s:byte(i)
        local len = c < 0x80 and 1 or (c >= 0xC2 and c <= 0xDF) and 2 or (c >= 0xE0 and c <= 0xEF) and 3
            or (c >= 0xF0 and c <= 0xF4) and 4 or nil
        if not len then return false end
        for j = i + 1, i + len - 1 do
            local b = s:byte(j)
            if not b or b < 0x80 or b > 0xBF then return false end
        end
        i = i + len
    end
    return true
end
io.open = function(p, m)
    if not validUtf8(p) then error(fh) end      -- the half-built handle, as the game throws it
    if p:sub(1, 9) == "netpunch/" then return nil, "no lobby in the game folder here" end
    return realOpen(p, m)
end
out = {}
print = function(...) local t = {} for i = 1, select("#", ...) do t[#t + 1] = tostring(select(i, ...)) end out[#out + 1] = table.concat(t, " ") end
'''


class GuiIoThrow(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute("assert(load(...))", SOURCE)          # the whole script still compiles
        self.lua.execute("CM = {}; K = {}; env = {}; os.getenv = function(k) return env[k] end")
        self.lua.execute(GAME_IO)
        self.lua.execute(OPEN_FILE)

    def tearDown(self):
        self.tmp.cleanup()

    def g(self):
        return self.lua.globals()

    def accented(self):
        """LOCALAPPDATA as ANSI bytes of an accented name; K.BASE the ASCII short path the proxy publishes."""
        short = self.root / "Users/JOS~1/AppData/Local/tpf2mp"
        (short / "data").mkdir(parents=True)
        self.lua.execute("env.LOCALAPPDATA = ...", (self.root.as_posix() + "/Users/Jos").encode() + b"\xe9/AppData/Local")
        self.g().K.BASE = (short / "data").as_posix() + "/"
        return short

    def test_open_file_returns_nil_and_logs_once(self):
        bad = b"C:/Users/Jos\xe9/x.txt"                     # ANSI bytes, as os.getenv returns them
        f, why = self.lua.eval("function(p) return CM.openFile(p, 'r') end")(bad)
        self.assertIsNone(f)
        self.assertIn("io.open threw", why)
        self.lua.eval("function(p) return CM.openFile(p, 'r') end")(bad)
        log = list(self.g().out.values())
        self.assertEqual(len(log), 1, log)
        self.assertIn("C:/Users/Jos?/x.txt", log[0])
        self.assertIn("file (", log[0])

    def test_open_file_passes_a_good_open_through(self):
        p = self.root / "ok.txt"
        p.write_text("x")
        body = self.lua.eval("function(p) local f = CM.openFile(p); local b = f:read('*a'); f:close(); return b end")(p.as_posix())
        self.assertEqual(body, "x")
        missing, why = self.lua.eval("function(p) return CM.openFile(p, 'r') end")((self.root / "no.txt").as_posix())
        self.assertIsNone(missing)
        self.assertNotIn("threw", why)

    def test_accented_profile_finds_the_lobby_through_the_short_path(self):
        short = self.accented()
        self.lua.execute(NET_DIR)
        cm = self.g().CM
        self.assertIsNone(cm.netDir())                     # no lobby yet: nil, not an error
        (short / "netpunch").mkdir()
        (short / "netpunch/lobby_out.jsonl").write_text("")
        self.assertEqual(cm.netDir(), (short / "netpunch").as_posix())
        self.assertTrue(cm.chatSend("hola"))
        self.assertIn("hola", (short / "netpunch/lobby_in.jsonl").read_text())

    def test_accented_profile_old_resolver_would_have_thrown(self):
        self.accented()
        with self.assertRaises(Exception):
            self.lua.execute('local f = io.open(env.LOCALAPPDATA .. "/tpf2mp/netpunch/lobby_out.jsonl", "r")')

    def test_ascii_profile_resolves_as_before(self):
        la = self.root / "Local"
        (la / "tpf2mp/netpunch").mkdir(parents=True)
        (la / "tpf2mp/netpunch/lobby_out.jsonl").write_text("")
        g = self.g()
        g.env["LOCALAPPDATA"] = la.as_posix()
        g.K.BASE = (la / "tpf2mp/data").as_posix() + "/"
        self.lua.execute(NET_DIR)
        self.assertEqual(g.CM.netDir(), la.as_posix() + "/tpf2mp/netpunch")

    def test_read_dash_of_an_absent_peer_does_not_raise(self):
        short = self.accented()
        (short / "data/lockstep_dash_a.txt").write_text("wall=5\npeers=-\n")
        self.lua.execute('CM.baseCandidates = { K.BASE, env.LOCALAPPDATA .. "/tpf2mp/data/" }')
        readDash = self.lua.execute(READ_DASH + "\nreturn readDash")
        own, _ = readDash("a")
        self.assertEqual(own["wall"], "5")
        self.assertIsNone(readDash("b"))                   # was: "file (0000000000000000)", the refresh gone


if __name__ == "__main__":
    unittest.main()
