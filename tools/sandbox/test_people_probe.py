#!/usr/bin/env python3
"""Regression tests use fake lab files and a Lua5.2 engine; never the live game."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("people_probe", Path(__file__).with_name("people_probe.py"))
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)
REQUEST = "0123456789ab"

LUA_ENGINE = r'''
local current = 0
local clockObject = {time = 0}
local original = function(...)
  clockObject.time = current
  return clockObject, 'extra', nil, 42
end
local types = {SIM_PERSON = 1, PERSON_CAPACITY = 2,
  SIM_ENTITY_AT_BUILDING = 3, SIM_ENTITY_MOVING = 4, SIM_ENTITY_IDLE = 5}
local entities = {
  [1] = {id=1, name='A "quoted"\nline', destinations={11,12,13}},
  [2] = {id=2, name='B', destinations={11,12,13}}
}
game = {interface = {getGameTime = original,
  getEntities = function() return {2} end,
  getEntity = function(id) return entities[id] end}}
api = {type = {ComponentType = setmetatable({}, {__index=types})}, engine = {
  forEachEntityWithComponent = function(callback, component)
    local ids = component == types.SIM_PERSON and {2,1} or {12,11}
    for _, id in ipairs(ids) do callback(id) end
  end,
  getComponent = function(id, component)
    if component == types.SIM_PERSON then return {lastDestinationUpdate=20000} end
    if id == 1 and component == types.SIM_ENTITY_AT_BUILDING then return {} end
    if id == 2 and component == types.SIM_ENTITY_MOVING then return {} end
  end,
  system = {simPersonSystem = {getCount = function() return 2 end}}
}}
local function step(time)
  current = time
  local t, extra, absent, last = game.interface.getGameTime('argument', nil)
  assert(t == clockObject and t.time == time)
  assert(extra == 'extra' and absent == nil and last == 42)
end
'''


class PeopleProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def run_lua(self, suffix, times=(10, 20)):
        lua = shutil.which("lua5.2")
        if not lua:
            self.skipTest("Lua5.2 is required for diagnostic wrapper regression")
        source = PROBE.render_probe(times, REQUEST)
        line = PROBE.eval_line(source)
        self.assertEqual(line.count("\n"), 1, "EVAL must occupy one physical line")
        (self.directory / "probe.lua").write_text(line.removeprefix("EVAL "))
        (self.directory / "harness.lua").write_text(LUA_ENGINE + suffix)
        result = subprocess.run([lua, "harness.lua"], cwd=self.directory,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_multi_target_preserves_clock_returns_restores_and_writes_jsonl(self):
        self.run_lua('''
setmetatable(_G, {__newindex = function(_, key)
  error('creating globals by assignment is not allowed: ' .. key)
end})
assert(dofile('probe.lua'):match('armed'))
assert(game.interface.getGameTime ~= original)
step(9); step(10)
assert(game.interface.getGameTime ~= original)
step(20)
assert(game.interface.getGameTime == original)
assert(_G.__tpf2mp_people_probe == nil)
step(21)
''')
        for at in (10, 20):
            path = self.directory / f"people_probe_{REQUEST}_{at}.jsonl"
            meta, people, data = PROBE.read_snapshot(path, at, REQUEST)
            self.assertEqual(len(data.splitlines()), 3)
            self.assertEqual(meta["personCapacityOrder"], {"1": 12, "2": 11})
            self.assertEqual(meta["stateCounts"]["SIM_ENTITY_AT_BUILDING"], 1)
            self.assertFalse(people[1]["spatial"])
            self.assertTrue(people[2]["spatial"])
            self.assertEqual(people[1]["entity"]["name"], 'A "quoted"\nline')

    def test_late_arm_rejected_without_changing_clock(self):
        self.run_lua('''
current = 10
local ok, err = pcall(dofile, 'probe.lua')
assert(not ok and tostring(err):match('first target already passed'))
assert(game.interface.getGameTime == original)
assert(_G.__tpf2mp_people_probe == nil)
''')

    def test_duplicate_arm_rejected_and_first_probe_completes(self):
        self.run_lua('''
dofile('probe.lua')
local first = game.interface.getGameTime
local ok, err = pcall(dofile, 'probe.lua')
assert(not ok and tostring(err):match('another diagnostic is armed'))
assert(game.interface.getGameTime == first)
step(10); step(20)
assert(game.interface.getGameTime == original)
''')

    def test_snapshot_error_restores_clock_and_does_not_overwrite(self):
        output = self.directory / f"people_probe_{REQUEST}_10.jsonl"
        output.write_text("preserve existing snapshot\n")
        stdout = self.run_lua('''
dofile('probe.lua')
step(10)
assert(game.interface.getGameTime == original)
assert(_G.__tpf2mp_people_probe == nil)
''')
        self.assertIn("output already exists", stdout)
        self.assertEqual(output.read_text(), "preserve existing snapshot\n")

    def test_world_clock_reset_restores_remaining_probe(self):
        self.run_lua('''
dofile('probe.lua')
step(10)
assert(game.interface.getGameTime ~= original)
step(0)
assert(game.interface.getGameTime == original)
step(20)
''')
        self.assertFalse((self.directory / f"people_probe_{REQUEST}_20.jsonl").exists())

    def test_comparison_refuses_inexact_capture(self):
        self.run_lua("dofile('probe.lua'); step(10.2); step(20)")
        with self.assertRaisesRegex(ValueError, "exact requested time"):
            PROBE.read_snapshot(self.directory / f"people_probe_{REQUEST}_10.jsonl", 10, REQUEST)

    def test_equal_global_count_does_not_hide_destination_difference(self):
        self.run_lua("dofile('probe.lua'); step(10); step(20)")
        path = self.directory / f"people_probe_{REQUEST}_10.jsonl"
        first = PROBE.read_snapshot(path, 10, REQUEST)
        second = PROBE.read_snapshot(path, 10, REQUEST)
        second[1][1]["entity"]["destinations"]["2"] = 99
        report = PROBE.compare_pair(first, second, 10)
        self.assertTrue(report["globalCountsEqual"])
        self.assertTrue(report["personIdsEqual"])
        self.assertFalse(report["equal"])
        self.assertEqual(report["differentDestinations"], {"shopping": 1})

    def test_dry_run_does_not_write_or_queue(self):
        root = self.directory / "lab"
        output = self.directory / "report"
        for actor, identity in zip(PROBE.ACTORS, ("a", "b")):
            data = root / actor / PROBE.DATA[actor]
            data.mkdir(parents=True)
            (root / actor / "game").mkdir()
            (data / "tpf2_instance.txt").write_text(identity + "\n")
            (data / f"lockstep_inject_{identity}.txt").write_text("prior\n")
        with contextlib.redirect_stdout(io.StringIO()):
            PROBE.arm(root, output, [60, 72, 144, 300], dry_run=True)
        self.assertFalse(output.exists())
        for actor, identity in zip(PROBE.ACTORS, ("a", "b")):
            self.assertEqual((root / actor / PROBE.DATA[actor] / f"lockstep_inject_{identity}.txt").read_text(), "prior\n")


if __name__ == "__main__":
    unittest.main()
