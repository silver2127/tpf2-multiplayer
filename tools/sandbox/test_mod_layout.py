#!/usr/bin/env python3
"""Exercise mod discovery and migration without touching any installed game."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


SOURCE = Path(__file__).with_name("tpf2mp-lab")
SPEC = importlib.util.spec_from_loader("tpf2mp_lab", importlib.machinery.SourceFileLoader("tpf2mp_lab", str(SOURCE)))
lab = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lab)
LEGACY = "urbangames_legacy_vehicle_pack_1"


class ModLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tpf2mp-mod-layout-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "lab"
        self.game = self.base / "Steam/game"
        self.names = ("a_stock_1", "stock with spaces_1", LEGACY)
        for name in self.names:
            source = self.game / "mods" / name
            source.mkdir(parents=True)
            (source / "mod.lua").write_text("return " + repr(name) + "\n")
        for name in ("native", "proton"):
            actor = self.root / name
            (actor / "game/mods/mp_lockstep_1").mkdir(parents=True)
            (actor / "game/mods/mp_lockstep_1/mod.lua").write_text("unchanged multiplayer Lua\n")
            for mod in self.names:
                (actor / "game/mods" / mod).symlink_to(actor / "shared-game/mods" / mod)
        self.config = {"root": str(self.root), "game": str(self.game)}

    def request_legacy_removal(self):
        actor = self.root / "native"
        request = actor / "legacy-removal.json"
        request.write_text(json.dumps({"status": "waiting for native game to close",
                                       "source": str(actor / "game/mods" / LEGACY)}))
        return request

    def test_real_directories_preserve_shared_content_and_lua(self):
        for name in ("native", "proton"):
            lab.prepare_mods(self.config, name)
            for mod in self.names:
                target = self.root / name / "game/mods" / mod
                self.assertTrue(target.is_dir())
                self.assertFalse(target.is_symlink())
                self.assertEqual(list(target.iterdir()), [])
                self.assertEqual((self.game / "mods" / mod / "mod.lua").read_text(), "return " + repr(mod) + "\n")
            self.assertEqual((self.root / name / "game/mods/mp_lockstep_1/mod.lua").read_text(), "unchanged multiplayer Lua\n")
            self.assertEqual(lab.mod_migration(self.config, name, set()), [])

    def test_pending_removal_excludes_both_before_and_after_watcher(self):
        request = self.request_legacy_removal()
        self.assertIn(LEGACY, lab.excluded_mods(self.config))
        with self.assertRaisesRegex(ValueError, "watcher completes"):
            lab.prepare_mods(self.config, "native")
        lab.prepare_mods(self.config, "proton")
        self.assertFalse((self.root / "proton/game/mods" / LEGACY).is_symlink())
        self.assertTrue((self.root / "proton/disabled-mods" / LEGACY).is_symlink())
        actor = self.root / "native"
        (actor / "disabled-mods").mkdir()
        (actor / "game/mods" / LEGACY).rename(actor / "disabled-mods" / LEGACY)
        request.write_text(json.dumps({"status": "removed", "source": str(actor / "game/mods" / LEGACY)}))
        lab.prepare_mods(self.config, "native")
        request.unlink()  # The moved markers alone still retain the exclusion.
        for name in ("native", "proton"):
            lab.prepare_mods(self.config, name)
            self.assertNotIn(str(self.game / "mods" / LEGACY), lab.mod_mounts(self.config, name))
            self.assertFalse((self.root / name / "game/mods" / LEGACY).exists())
            self.assertFalse((self.root / name / "game/mods" / LEGACY).is_symlink())

    def test_active_actor_is_not_modified(self):
        proc = self.base / "proc"
        for pid, actor in ((10001, "native"), (10002, "proton")):
            (proc / str(pid)).mkdir(parents=True)
            (proc / str(pid) / "environ").write_bytes(os.fsencode("STEAM_COMPAT_DATA_PATH=" + str(self.root / actor / "compatdata")) + b"\0OTHER=value\0")
        self.assertEqual(lab.actor_processes(self.config, "native", proc), [10001])
        with mock.patch.object(lab, "actor_processes", return_value=[10001]):
            with self.assertRaisesRegex(ValueError, "actor is running"):
                lab.prepare_mods(self.config, "native")
        self.assertTrue(all((self.root / "native/game/mods" / mod).is_symlink() for mod in self.names))

    def test_private_contents_and_unknown_links_are_preserved(self):
        target = self.root / "native/game/mods" / LEGACY
        target.unlink()
        target.mkdir()
        (target / "private.txt").write_text("keep me")
        with self.assertRaisesRegex(ValueError, "refusing to hide"):
            lab.prepare_mods(self.config, "native")
        self.assertTrue((self.root / "native/game/mods/a_stock_1").is_symlink())
        self.assertEqual((target / "private.txt").read_text(), "keep me")
        (target / "private.txt").unlink()
        target.rmdir()
        target.symlink_to(self.game / "mods" / LEGACY)
        with self.assertRaisesRegex(ValueError, "Unrecognized stock mod symlink"):
            lab.prepare_mods(self.config, "native")
        self.assertEqual(target.readlink(), self.game / "mods" / LEGACY)

    def test_invalid_or_incomplete_exclusion_never_restores_mod(self):
        self.config["excluded_mods"] = ["../invalid"]
        with self.assertRaisesRegex(ValueError, "Invalid excluded"):
            lab.prepare_mods(self.config, "proton")
        self.config.pop("excluded_mods")
        self.request_legacy_removal().write_text('{"status":')
        with self.assertRaises(json.JSONDecodeError):
            lab.prepare_mods(self.config, "proton")
        self.assertTrue((self.root / "proton/game/mods" / LEGACY).is_symlink())

    @unittest.skipUnless(shutil.which("bwrap"), "bubblewrap unavailable")
    def test_real_bind_mounts_are_discoverable_and_read_only(self):
        self.config["excluded_mods"] = [LEGACY]
        code = """import errno,json,sys
from pathlib import Path
game=Path(sys.argv[1]); names=json.loads(sys.argv[2])
mods=game/'mods'
assert sorted(p.name for p in mods.iterdir() if p.is_dir() and not p.is_symlink()) == sorted(names+['mp_lockstep_1'])
for name in names:
 p=mods/name/'mod.lua'
 assert p.read_text() == 'return '+repr(name)+'\\n'
 try:
  with p.open('a') as f: raise AssertionError('shared mod writable')
 except OSError as error: assert error.errno==errno.EROFS,error
assert (mods/'mp_lockstep_1/mod.lua').read_text()=='unchanged multiplayer Lua\\n'
"""
        for name in ("native", "proton"):
            lab.prepare_mods(self.config, name)
            command = [shutil.which("bwrap"), "--ro-bind", "/", "/",
                       "--bind", str(self.root / name), str(self.root / name),
                       "--bind", str(self.root / name / "game"), str(self.game)]
            command += lab.mod_mounts(self.config, name)
            command += ["--", "/usr/bin/python3", "-c", code, str(self.game), json.dumps(list(self.names[:-1]))]
            result = subprocess.run(command, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
