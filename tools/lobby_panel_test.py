"""Exercise the Python-to-Lua lobby snapshot, including hotjoin roster changes."""
from pathlib import Path
import sys
import tempfile
import unittest

from lupa.lua52 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "netpunch"))
from lobby import LobbyIO


class LobbyPanel(unittest.TestCase):
    def test_open_controls_without_existing_lobby(self):
        lua = LuaRuntime(unpack_returned_tuples=True)
        source = (ROOT / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua").read_text()
        # Compile the entire script, then exercise its actual button callback.
        lua.execute("assert(load(...))", source)
        start = source.index('lobbyL:addItem(toggleBtn("  host / manage lobby  "')
        end = source.index("end))", start) + len("end))")
        lua.execute('''
            lobbyL = {addItem = function(_, button) openLobby = button end}
            function toggleBtn(_, callback) return callback end
            D = {lobbyText = {setText = function(_, text) failure = text end}}
        ''')
        with tempfile.TemporaryDirectory() as folder:
            lua.globals().K = lua.table(BASE=folder.replace("\\", "/") + "/")
            lua.execute(source[start:end])
            lua.globals().openLobby()
            self.assertEqual((Path(folder) / "tpf2_lobby_open.txt").read_text(), "open\n")
            self.assertIsNone(lua.globals().failure)
            lua.globals().K.BASE = folder.replace("\\", "/") + "/missing/"
            lua.globals().openLobby()
            self.assertIn("Could not open lobby controls", lua.globals().failure)

    def test_roster_pagination_and_disconnect(self):
        lua = LuaRuntime(unpack_returned_tuples=True)
        cm = lua.table()
        lua.execute((ROOT / "mod/mp_lockstep_1/res/scripts/mp/stats.lua").read_text())(cm, lua.table(), lambda *_: None)
        with tempfile.TemporaryDirectory() as folder:
            io = LobbyIO(folder)
            players = ["Host"] + ["Player %d" % i for i in range(1, 18)]
            io.write_state(state="connected", lobby="Test world", host="Host", you="Player 17",
                           players=players, companies={"Host": 1, "Player 17": 4}, code="PRIVATE_CODE")
            body = (Path(folder) / "lobby_panel.txt").read_text()
            self.assertNotIn("PRIVATE_CODE", body)
            text, page, pages = cm.lobbyPanelPage(body, 1)
            self.assertIn("Host (host) - company 1", text)
            self.assertEqual((page, pages), (1, 3))
            text, page, pages = cm.lobbyPanelPage(body, 3)
            self.assertIn("Player 17 (you) - company 4", text)
            # Leaving players must remove rows and clamp a previously selected page.
            io.write_state(players=["Host"])
            text, page, pages = cm.lobbyPanelPage((Path(folder) / "lobby_panel.txt").read_text(), 3)
            self.assertEqual((page, pages), (1, 1))
            self.assertNotIn("Player 17", text)
            io.write_state(state="disconnected")
            text, _, _ = cm.lobbyPanelPage((Path(folder) / "lobby_panel.txt").read_text(), 1)
            self.assertIn("Disconnected", text)
            self.assertIn("0 player(s)", text)
            self.assertEqual("", cm.lobbyPanelPage("", 1)[0])


if __name__ == "__main__":
    unittest.main()
