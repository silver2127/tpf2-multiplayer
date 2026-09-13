"""Run the installed, unmodified Natural Town Growth script with mocked game APIs.

This demonstrates clock-dependent mutations; it is not a live multiplayer test.
Usage: python tools/natural_town_growth_probe.py [Workshop mod directory]
Requires lupa with Lua 5.2 support (as do the other multiplayer Lua tests).
"""
import sys
from pathlib import Path

from lupa.lua52 import LuaRuntime

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
    r"C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\1954591986"
)


def runtime(wall):
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.globals().mod_root = ROOT.as_posix()
    lua.globals().wall = wall
    lua.execute(r'''
        package.path = mod_root .. '/res/scripts/?.lua;' .. package.path
        print = function() end
        _ = function(s) return s end
        os.time = function() return wall end
        local realDate = os.date
        os.date = function(format, timestamp) return realDate(format, timestamp or wall) end
        writes = {}
        game = {config={}, interface={}}
        local I = game.interface
        I.getGameTime = function() return {time=500, date={year=1850}} end
        I.getTowns = function() return {1} end
        I.getEntity = function(id) return {id=id, name='Test town'} end
        I.getTownCargoSupplyAndLimit = function() return {} end
        I.getCargoTypes = function() return {} end
        I.getTownReachability = function() return {0,0} end
        I.setTownCapacities = function(id,r,c,i)
            writes[#writes+1] = {id,r,c,i}
        end
        dofile(mod_root .. '/mod.lua')
        data().runFn({})
        dofile(mod_root .. '/res/config/game_script/natural_town_growth.lua')
        script = data()
        function load_identical(value, lastUpdate, lastCapacity)
            script.load({minorVersion=0, lastUpdatedAtEpoch=lastUpdate,
                baseCapacity={scalingFactors={[1]={residential=1,commercial=1,industrial=1}}},
                capacitiesByTownId={[1]={
                    residential={value=value,setAtEpoch=lastCapacity},
                    commercial={value=value,setAtEpoch=lastCapacity},
                    industrial={value=value,setAtEpoch=lastCapacity}}}})
        end
    ''')
    return lua


def count(lua):
    return lua.eval('#writes')


# Same save and same sim time, but one computer crosses the ten-second deadline.
a, b = runtime(1009), runtime(1010)
for lua in (a, b):
    lua.execute('load_identical(50,1000,1000); script.update()')
assert (count(a), count(b)) == (0, 1)
assert b.eval('writes[1][2]') == 100
print('PASS: identical save/sim time: wall 1009 makes 0 writes; wall 1010 makes 1')

# Both run an update, but only one crosses the shrink cooldown.
a, b = runtime(1180), runtime(1181)
for lua in (a, b):
    lua.execute('load_identical(200,1000,1000); script.update()')
capacities = (a.eval('writes[1][2]'), b.eval('writes[1][2]'))
assert capacities == (200, 100), capacities
print('PASS: identical save/sim time: shrink cooldown produces capacities 200 vs 100')

# Even a frozen simulation clock does not prevent further capacity writes.
a.execute('wall=1190; script.update()')
assert count(a) == 2
print('PASS: advancing wall time alone causes another capacity write')

# New towns use clock seeds. Capture outputs immediately: Lua 5.2 builds can
# share the C-library RNG between states in this process.
factors = []
for wall in (1000, 1001):
    lua = runtime(wall)
    lua.execute('script.init()')
    factors.append(tuple(lua.eval('writes[1]')[i] for i in (2, 3, 4)))
assert factors[0] != factors[1], factors
print(f'PASS: new-town clock seeds yield different capacities: {factors}')
