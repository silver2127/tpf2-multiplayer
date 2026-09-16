#!/usr/bin/env python3
"""Isolated Lua5.2 diagnostics tests; never read or modify a running game."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import movement_probe as probe
from people_probe import eval_line

REQUEST = "0123456789ab"
ENGINE = r'''
local current = 0
local clock = {time=0}
local original = function(...) clock.time=current; return clock, 'extra', nil, 42 end
local function proxy(data)
  return setmetatable({}, {__index=function(_, k)
    if data[k] == nil then error('unknown mock field: '..tostring(k)) end
    return data[k]
  end, __len=function() return #data end})
end
local pos = {edgeIndex=0,pos01=0.25,pos=12.5}
local state = {pathPos=proxy(pos),speed=1.7,
  brakeDecel=1,accel=0,timeUntilAccel=0,timeStanding=0,timeToIgnore=0,approachingStation=false}
local route = {path=proxy({edges=proxy({proxy({edgeId=proxy({entity=10,index=1}),dir=true})}),
  endOffset=0,terminalDecisionOffset=-1}),endParam=1,endPos=55,state=0,blocked=0,
  allowEarlyArrival=true,reverse=false,dyn=proxy(state)}
-- dyn0 is a valid optional field whose getter returns nil.
local path = setmetatable({}, {__index=function(_,k) if k=='dyn0' then return nil end
  assert(route[k]~=nil,'unknown path field'); return route[k] end})
local matrix={1,0,0,0,0,1,0,0,0,0,1,0,42.5,19,3,1}
local rangeEdge=setmetatable({}, {__index=function(_,k)
  if k=='range' then return proxy({edgeId=proxy({entity=10,index=1}),params=proxy({0.1,0.9})}) end
  if k=='link' then return nil end;error('unknown variant field') end})
local linkEdge=setmetatable({}, {__index=function(_,k)
  if k=='range' then return nil end
  if k=='link' then return proxy({points=proxy({proxy({x=1,y=2,z=3}),proxy({x=4,y=5,z=6})})}) end
  error('unknown variant field') end})
local walkerData={path=proxy({edges=proxy({rangeEdge,linkEdge})}),pathPos=proxy(pos),
  stopped=false,topSpeed=1.8,speed=1.7,brakeDecel=1,offset=0,position=proxy({x=42.5,y=19,z=3}),
  movementType=0,dist=12.5,stateTime=1000}
local walk=setmetatable({}, {__index=function(_,k) if k=='dist0' then return nil end
  assert(walkerData[k]~=nil,'unknown walker field');return walkerData[k] end})
local types={SIM_PERSON=1,SIM_ENTITY_MOVING=2,MOVE_PATH=3,MODEL_INSTANCE_LIST=4,MODEL_PERSON=5,SIM_ENTITY_AT_BUILDING=6}
game={interface={getGameTime=original,getEntity=function(id)
  return {id=id,home=10,shopping=20,work=30,modelIdCar=7,moveModes={0,1}}
end}}
api={type={ComponentType=setmetatable({}, {__index=types})},engine={
  system={simPersonSystem={getCount=function() return 3 end}},
  forEachEntityWithComponent=function(cb,component) assert(component==1);cb(3);cb(2);cb(1) end,
  getComponent=function(id,component)
    if component==1 then return proxy({lastDestinationUpdate=1600}) end
    if (id==2 or id==3) and component==2 then return proxy({line=-1,lineStop0=-1,lineStop1=-1,partialPath=false}) end
    if id==2 and component==3 then return path end
    if (id==2 or id==3) and component==4 then return proxy({fatInstances=proxy({proxy({transf=proxy(matrix)})})}) end
    if id==3 and component==5 then return walk end
    if id==1 and component==6 then return proxy({waitingTime=46.42832946777344}) end
  end}}
local function step(t)
  current=t;local a,b,c,d=game.interface.getGameTime('argument',nil)
  assert(a==clock and a.time==t and b=='extra' and c==nil and d==42)
end
'''


class MovementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_lua(self, suffix):
        lua = shutil.which("lua5.2")
        if not lua:
            self.skipTest("Lua5.2 is required")
        line = eval_line(probe.render_probe([10, 20], REQUEST))
        self.assertEqual(line.count("\n"), 1)
        (self.root / "probe.lua").write_text(line.removeprefix("EVAL "))
        (self.root / "test.lua").write_text(ENGINE + suffix)
        result = subprocess.run([lua, "test.lua"], cwd=self.root, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def snapshot(self, at=10):
        return probe.read_snapshot(self.root / f"movement_probe_{REQUEST}_{at}.jsonl", at, REQUEST)

    def test_exact_clock_proxy_fields_and_restoration(self):
        self.run_lua("""
setmetatable(_G,{__newindex=function(_,k) error('new global '..k) end})
dofile('probe.lua');step(9);step(10)
assert(game.interface.getGameTime~=original)
step(20);assert(game.interface.getGameTime==original)
""")
        meta, people, _ = self.snapshot()
        self.assertTrue(meta["simulationCaptureComplete"])
        self.assertTrue(meta["geometryCaptureComplete"])
        self.assertEqual(meta["movingCount"], 2)
        self.assertEqual(meta["walkerCount"], 1)
        self.assertEqual(people[1]["atBuilding"]["value"]["waitingTime"], 46.42832946777344)
        self.assertFalse(people[2]["atBuilding"]["present"])
        self.assertEqual(people[3]["walker"]["value"]["path"]["edges"]["count"], 2)
        self.assertFalse(people[3]["movePath"]["present"])
        self.assertEqual(people[2]["movePath"]["value"]["dyn"]["pathPos"]["pos01"], 0.25)
        self.assertEqual(people[2]["worldTransforms"]["instances"]["values"]["1"]["transf"]["13"], 42.5)
        self.assertTrue(probe.compare_pair(self.snapshot(), self.snapshot(), 10)["allEqual"])

    def test_late_and_duplicate_refusal(self):
        self.run_lua("""
current=10;assert(not pcall(dofile,'probe.lua'));assert(game.interface.getGameTime==original)
current=0;dofile('probe.lua');local first=game.interface.getGameTime
assert(not pcall(dofile,'probe.lua'));assert(game.interface.getGameTime==first)
step(10);step(20);assert(game.interface.getGameTime==original)
""")

    def test_person_state_only_difference_cannot_compare_equal(self):
        self.run_lua("dofile('probe.lua');step(10);step(20)")
        a = self.snapshot()
        self.assertEqual(a[0]["globalCount"], 3)
        self.assertEqual(a[1][1]["lastDestinationUpdate"], 1600)
        b = copy.deepcopy(a)
        b[1][1]["entity"]["shopping"] = 21
        report = probe.compare_pair(a, b, 10)
        self.assertFalse(report["simulationEqual"])
        self.assertTrue(report["worldTransformsEqual"])
        self.assertEqual(report["differentFields"], {"entity": 1})
        b = copy.deepcopy(a)
        b[1][1]["lastDestinationUpdate"] = 1800
        self.assertFalse(probe.compare_pair(a, b, 10)["simulationEqual"])
        b = copy.deepcopy(a)
        b[0]["globalCount"] = 4
        self.assertFalse(probe.compare_pair(a, b, 10)["simulationEqual"])

    def test_building_wait_difference_cannot_compare_equal(self):
        self.run_lua("dofile('probe.lua');step(10);step(20)")
        a = self.snapshot()
        b = copy.deepcopy(a)
        b[1][1]["atBuilding"]["value"]["waitingTime"] = 41.50788497924805
        report = probe.compare_pair(a, b, 10)
        self.assertFalse(report["simulationEqual"])
        self.assertTrue(report["worldTransformsEqual"])
        self.assertEqual(report["differentFields"], {"atBuilding": 1})

    def test_incomplete_capture_cannot_compare_equal(self):
        self.run_lua("state.speed=nil;dofile('probe.lua');step(10);step(20)")
        meta, _, _ = self.snapshot()
        self.assertGreater(meta["errors"]["simulation"], 0)
        report = probe.compare_pair(self.snapshot(), self.snapshot(), 10)
        self.assertFalse(report["simulationEqual"])
        self.assertTrue(report["worldTransformsEqual"])

    def test_movement_and_model_differences_are_separate(self):
        self.run_lua("dofile('probe.lua');step(10);step(20)")
        a = self.snapshot()
        b = copy.deepcopy(a)
        b[1][2]["movePath"]["value"]["dyn"]["pathPos"]["pos01"] = 0.3
        report = probe.compare_pair(a, b, 10)
        self.assertFalse(report["simulationEqual"])
        self.assertTrue(report["worldTransformsEqual"])
        b = copy.deepcopy(a)
        b[1][2]["worldTransforms"]["instances"]["values"]["1"]["transf"]["13"] = 43.0
        report = probe.compare_pair(a, b, 10)
        self.assertTrue(report["simulationEqual"])
        self.assertFalse(report["worldTransformsEqual"])
        b = copy.deepcopy(a)
        b[1][3]["walker"]["value"]["position"]["x"] = 43.0
        report = probe.compare_pair(a, b, 10)
        self.assertFalse(report["simulationEqual"])
        self.assertTrue(report["worldTransformsEqual"])
        self.assertEqual(report["differentFields"], {"walker": 1})

    def test_snapshot_error_restores_without_overwriting(self):
        p = self.root / f"movement_probe_{REQUEST}_10.jsonl"
        p.write_text("preserved")
        out = self.run_lua("dofile('probe.lua');step(10);assert(game.interface.getGameTime==original)")
        self.assertIn("output already exists", out)
        self.assertEqual(p.read_text(), "preserved")

    def test_clock_rewind_restores_and_inexact_capture_rejected(self):
        self.run_lua("dofile('probe.lua');step(10.2);step(0);assert(game.interface.getGameTime==original)")
        with self.assertRaisesRegex(ValueError, "exact requested time"):
            self.snapshot()
        self.assertFalse((self.root / f"movement_probe_{REQUEST}_20.jsonl").exists())


if __name__ == "__main__":
    unittest.main()
