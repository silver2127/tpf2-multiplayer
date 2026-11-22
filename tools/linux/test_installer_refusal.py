"""Native folder refusals must explain themselves before changing any files.

Runs the source installer against synthetic release/Steam folders; no game runs.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parent


class InstallerRefusal(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tpf2mp-refusal-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.release = self.root / "release"
        for name in ("lib/libtpf2mp_boot.so", "mod/mp_lockstep_1/mod.lua",
                     "tpf2mp-launch"):
            path = self.release / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic package\n")
        self.game = self.root / "profile/.local/share/Steam/steamapps/common/Transport Fever 2"
        self.game.mkdir(parents=True)
        self.data = self.root / "data"
        self.data.mkdir()
        (self.data / "keep.txt").write_text("existing user data\n")
        self.env = dict(os.environ, SNAP_REAL_HOME=str(self.root / "profile"))

    def refuse(self, *args):
        before = {p.relative_to(self.root): p.read_bytes()
                  for p in self.root.rglob("*") if p.is_file()}
        directories = {p.relative_to(self.root) for p in self.root.rglob("*") if p.is_dir()}
        result = subprocess.run(
            ["bash", str(TOOLS / "install.sh"), "--release", str(self.release),
             "--data-home", str(self.data), *map(str, args)],
            env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("error: ", result.stderr)
        self.assertNotIn("Installing the", result.stdout)
        self.assertEqual(before, {p.relative_to(self.root): p.read_bytes()
                                  for p in self.root.rglob("*") if p.is_file()})
        self.assertEqual(directories, {p.relative_to(self.root)
                                      for p in self.root.rglob("*") if p.is_dir()})
        return result.stderr

    def test_selected_folder_missing_native_executable(self):
        error = self.refuse("--game", self.game)
        self.assertIn(f"no TransportFever2 in {self.game}", error)

    def test_no_discovered_game_explains_manual_selection(self):
        error = self.refuse()
        self.assertIn("not found in any Steam library", error)
        self.assertIn("pass --game <folder>", error)

    def test_discovered_proton_game_explains_native_requirement(self):
        (self.game / "TransportFever2.exe").write_bytes(b"synthetic Windows game")
        error = self.refuse()
        self.assertIn("Windows version", error)
        self.assertIn("native Linux version", error)
        self.assertIn("Properties > Compatibility", error)

    def test_wrong_elf_build_explains_refusal_and_recovery(self):
        shutil.copyfile("/bin/true", self.game / "TransportFever2")
        notes = subprocess.check_output(
            ["readelf", "-n", str(self.game / "TransportFever2")], text=True)
        build_id = next(line.split("Build ID: ")[1].strip()
                        for line in notes.splitlines() if "Build ID: " in line)
        error = self.refuse("--game", self.game)
        self.assertIn(f"not Steam build 35924 (build-id {build_id})", error)
        self.assertIn("Verify the game files in Steam", error)


if __name__ == "__main__":
    unittest.main()
