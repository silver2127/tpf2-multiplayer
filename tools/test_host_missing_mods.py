"""The host refuses to share a save its OWN game could not load: a mod the
save needs that is nowhere on this PC. The status line and the chat name the
missing mods; a save whose mods are all here (or whose list is unknown) goes
ahead as before.

    python tools/test_host_missing_mods.py
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby                                                 # noqa: E402


class FakeIo:
    def __init__(self):
        self.events = []

    def emit(self, ev):
        self.events.append(ev)


class HostMissingMods(unittest.TestCase):
    def setUp(self):
        lobby._mod_refusal_notes.clear()
        self.io = FakeIo()
        self.lines = []
        self.log = self.lines.append
        self.have = {("*1954591986", 1): r"C:\ws\1954591986", ("_urbangames_deluxe_pack", 1): r"C:\g\dlcs\x"}
        self.lookup = lambda m, v: self.have.get((m, v))

    def test_all_here_starts(self):
        mods = [("*1954591986", 1), ("_urbangames_deluxe_pack", 1)]
        self.assertTrue(lobby._host_mods_check(r"C:\s\world.sav", mods, self.io, self.log, self.lookup))
        self.assertEqual(self.io.events, [])

    def test_unknown_list_starts(self):
        self.assertTrue(lobby._host_mods_check(r"C:\s\world.sav", None, self.io, self.log, self.lookup))
        self.assertTrue(lobby._host_mods_check(r"C:\s\world.sav", [], self.io, self.log, self.lookup))
        self.assertEqual(self.io.events, [])

    def test_missing_refuses_and_names_them(self):
        mods = [("*1954591986", 1), ("*2916150031", 1), ("*1911374498", 1)]
        self.assertEqual(lobby._host_missing_mods(mods, self.lookup), [("*2916150031", 1), ("*1911374498", 1)])
        self.assertFalse(lobby._host_mods_check(r"C:\s\world.sav", mods, self.io, self.log, self.lookup))
        kinds = [e["type"] for e in self.io.events]
        self.assertEqual(kinds, ["status", "chat"])
        status, chat = self.io.events
        self.assertIn("'world' needs 2 mod(s) not installed on this PC", status["detail"])
        self.assertIn("*2916150031_1", chat["text"])
        self.assertIn("*1911374498_1", chat["text"])
        self.assertIn("START GAME again", chat["text"])
        self.assertTrue(any("NOT sharing" in l and "*2916150031_1" in l for l in self.lines))

    def test_chat_once_a_minute_status_every_time(self):
        mods = [("*2916150031", 1)]
        for _ in range(3):
            self.assertFalse(lobby._host_mods_check(r"C:\s\world.sav", mods, self.io, self.log, self.lookup))
        self.assertEqual([e["type"] for e in self.io.events], ["status", "chat", "status", "status"])

    def test_long_list_is_cut(self):
        mods = [(f"*{2000000000 + i}", 1) for i in range(12)]
        self.assertFalse(lobby._host_mods_check(r"C:\s\big.sav", mods, self.io, self.log, self.lookup))
        chat = self.io.events[1]["text"]
        self.assertIn("(4 more)", chat)
        self.assertIn("*2000000007_1", chat)
        self.assertNotIn("*2000000008_1", chat)
        # the log has every name
        self.assertTrue(any("*2000000011_1" in l for l in self.lines))


if __name__ == "__main__":
    unittest.main(verbosity=1)
