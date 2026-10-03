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
local function button() return { setStyleClassList = function(self, t) self.classes = t end } end
return function(tab, chatOpen)
    local CM = { dashTab = tab, closed = 0 }
    local D = { lobbyBox = box("lobby"), statsBox = box("stats"), chatBox = box("chat"), coBox = box("companies"),
                speedShown = true, chatOpen = chatOpen,
                tabButtons = { lobby = button(), stats = button(), chat = button(), companies = button(), speed = button() },
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
        self.assertEqual(D.tabLabels.chat.text, "CHAT")
        self.assertEqual(D.tabLabels.stats.text, "STATUS")
        self.assertEqual(list(D.tabButtons.chat.classes.values()), ["mpDashTab", "mpDashSelected"])
        self.assertEqual(list(D.tabButtons.stats.classes.values()), ["mpDashTab"])

    def test_opening_a_tab_closes_the_others(self):
        CM, D, select = self.make(None, False)
        select("stats")
        self.assertEqual(self.shown(CM, D), (["stats"], ["stats"]))
        select("speed")                                   # the speed row has no box; its flag drives the tick
        self.assertEqual(self.shown(CM, D), ([], ["speed"]))
        self.assertIsNone(D.speedShown)                   # the tick re-applies the row
        self.assertEqual(D.tabLabels.speed.text, "SPEED")
        self.assertEqual(list(D.tabButtons.speed.classes.values()), ["mpDashTab", "mpDashSelected"])
        self.assertEqual(list(D.tabButtons.chat.classes.values()), ["mpDashTab"])
        self.assertEqual(D.tabLabels.stats.text, "STATUS")
        select("companies")
        self.assertEqual(self.shown(CM, D), (["companies"], ["companies"]))
        self.assertFalse(CM.dashShowSpeed)                # the speed flag is false, not nil (the tick reads ~= false)

    def test_clicking_the_open_tab_closes_it(self):
        CM, D, select = self.make("lobby", False)
        select("lobby")
        self.assertIs(CM.dashTab, False)
        self.assertEqual(self.shown(CM, D), ([], []))
        self.assertEqual(D.tabLabels.lobby.text, "SESSION")
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

    def test_collapse_releases_chat_focus_and_preserves_tab(self):
        start = SOURCE.index("function D.setCollapsed(collapsed)")
        end = SOURCE.index('D.win = api.gui.comp.Window.new', start)
        self.lua.execute('''
            CM = { dashTab = "chat", closed = 0 }
            local function view()
                return { setVisible = function(self, v) self.visible = v end }
            end
            D = { body = view(), compact = view(), chatOpen = true,
                win = { setSize = function() error("Do not override automatic window sizing") end } }
            api = { gui = { util = { Size = { new = function(w, h) return {w, h} end } } } }
            function CM.chatCloseInput() CM.closed = CM.closed + 1; D.chatOpen = false end
        ''')
        self.lua.execute(SOURCE[start:end])
        cm, d = self.lua.globals().CM, self.lua.globals().D
        d.setCollapsed(True)
        self.assertTrue(cm.dashCollapsed)
        self.assertFalse(d.body.visible)
        self.assertTrue(d.compact.visible)
        self.assertEqual(cm.closed, 1)
        self.assertEqual(cm.dashTab, "chat")
        d.setCollapsed(True)
        self.assertEqual(cm.closed, 1)
        d.setCollapsed(False)
        self.assertTrue(d.body.visible)
        self.assertFalse(d.compact.visible)
        self.assertFalse(d.chatOpen)
        self.assertEqual(cm.dashTab, "chat")


if __name__ == "__main__":
    unittest.main()
