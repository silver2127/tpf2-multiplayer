"""The dashboard's section buttons are tabs: opening one closes the others.

Runs the real block of lockstep.lua (from `local TABS =` to the toggle row) against
stand-in section boxes, labels and chat input.
"""
from pathlib import Path
import unittest

from lupa.lua52 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua").read_text()

HARNESS = '''
local block = ...
local function box(name) return { name = name, visible = nil, setVisible = function(self, v) self.visible = v end } end
local function label() return { setText = function(self, t) self.text = t end } end
return function(tab, chatOpen)
    local CM = { dashTab = tab, closed = 0 }
    local D = { lobbyBox = box("lobby"), statsBox = box("stats"), chatBox = box("chat"), coBox = box("companies"),
                speedShown = true, chatOpen = chatOpen,
                tabLabels = { lobby = label(), stats = label(), chat = label(), companies = label(), speed = label() } }
    function CM.chatCloseInput() D.chatOpen = false; CM.closed = CM.closed + 1 end
    local run = assert(load("local CM, D = ...\\n" .. block .. "\\nreturn selectTab, applyTabs"))
    local selectTab, applyTabs = run(CM, D)
    applyTabs()
    return CM, D, selectTab
end
'''


class DashTabs(unittest.TestCase):
    def setUp(self):
        start = SOURCE.index("local TABS = ")
        end = SOURCE.index('local tog = api.gui.layout.BoxLayout.new("HORIZONTAL")', start)
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute("assert(load(...))", SOURCE)          # the whole script still compiles
        self.make = self.lua.execute(HARNESS, SOURCE[start:end])

    def shown(self, CM, D):
        boxes = {"lobby": D.lobbyBox, "stats": D.statsBox, "chat": D.chatBox, "companies": D.coBox}
        open_boxes = sorted(name for name, b in boxes.items() if b.visible)
        flags = {name: CM["dashShow" + key] for name, key in
                 (("lobby", "Lobby"), ("stats", "Stats"), ("chat", "Chat"), ("companies", "Companies"), ("speed", "Speed"))}
        return open_boxes, sorted(name for name, v in flags.items() if v)

    def test_default_is_chat_alone(self):
        CM, D, _ = self.make(None, False)
        self.assertEqual(CM.dashTab, "chat")
        self.assertEqual(self.shown(CM, D), (["chat"], ["chat"]))
        self.assertEqual(D.tabLabels.chat.text, "[ chat ]")
        self.assertEqual(D.tabLabels.stats.text, "  stats  ")

    def test_opening_a_tab_closes_the_others(self):
        CM, D, select = self.make(None, False)
        select("stats")
        self.assertEqual(self.shown(CM, D), (["stats"], ["stats"]))
        select("speed")                                   # the speed row has no box; its flag drives the tick
        self.assertEqual(self.shown(CM, D), ([], ["speed"]))
        self.assertIsNone(D.speedShown)                   # the tick re-applies the row
        self.assertEqual(D.tabLabels.speed.text, "[ speed ]")
        self.assertEqual(D.tabLabels.stats.text, "  stats  ")
        select("companies")
        self.assertEqual(self.shown(CM, D), (["companies"], ["companies"]))
        self.assertFalse(CM.dashShowSpeed)                # the speed flag is false, not nil (the tick reads ~= false)

    def test_clicking_the_open_tab_closes_it(self):
        CM, D, select = self.make("lobby", False)
        select("lobby")
        self.assertIs(CM.dashTab, False)
        self.assertEqual(self.shown(CM, D), ([], []))
        self.assertEqual(D.tabLabels.lobby.text, "  lobby  ")
        select("lobby")
        self.assertEqual(self.shown(CM, D), (["lobby"], ["lobby"]))

    def test_leaving_chat_closes_an_open_input(self):
        CM, D, select = self.make("chat", True)
        self.assertEqual(CM.closed, 0)
        select("stats")
        self.assertEqual(CM.closed, 1)
        self.assertFalse(D.chatOpen)

    def test_a_rebuild_keeps_the_open_tab(self):
        CM, D, _ = self.make("companies", False)          # CM.dashTab carries over a window rebuild
        self.assertEqual(self.shown(CM, D), (["companies"], ["companies"]))
        CM, D, _ = self.make(False, False)
        self.assertEqual(self.shown(CM, D), ([], []))


if __name__ == "__main__":
    unittest.main()
