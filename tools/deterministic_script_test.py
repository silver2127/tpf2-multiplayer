"""Offline compatibility regressions using the actual downloaded Workshop script."""
from pathlib import Path
from natural_town_growth_probe import runtime

REPO = Path(__file__).resolve().parents[1]
SOURCE = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/deterministic_script.lua').read_text()


def peer(wall):
    lua = runtime(wall)
    lua.globals().compat_source = SOURCE
    lua.execute('''
        compat = assert(load(compat_source))()
        sim = 500
        game.interface.getGameTime = function() return {time=sim,date={year=1850}} end
        script = compat.wrap(script, 'natural_town_growth', {naturalTownGrowth=true})
        originalRandom, originalSeed = math.random, math.randomseed
        originalTime, originalDate, originalClock = os.time, os.date, os.clock
    ''')
    return lua


def capacities(lua):
    return [tuple(lua.eval('writes')[i][j] for j in range(1, 5))
            for i in range(1, lua.eval('#writes') + 1)]


a, b = peer(1000), peer(900000)
for p in (a, b):
    p.execute('script.init()')
assert capacities(a) == capacities(b)
for sim in (500, 500, 501, 510, 510, 680, 681):
    for p in (a, b):
        p.globals().sim = sim
        p.execute('wall=wall+37; script.update()')
assert capacities(a) == capacities(b)
print('PASS: new-town RNG and updates agree with different wall clocks')

a, b = peer(1009), peer(1181)
for p in (a, b):
    p.execute('load_identical(200,1000,1000); script.update()')
assert capacities(a) == capacities(b)
assert capacities(a)[-1][1] == 200
for p in (a, b):
    p.execute('sim=690; script.update()')
assert capacities(a) == capacities(b)
assert capacities(a)[-1][1] == 100
print('PASS: old-save migration retains values and aligns shrink cooldown')

for p in (a, b):
    p.execute('''
        assert(math.random == originalRandom and math.randomseed == originalSeed)
        assert(os.time == originalTime and os.date == originalDate and os.clock == originalClock)
        saved = script.save()
        script.load(saved)
        local before = #writes
        script.update()
        assert(#writes == before)
    ''')
print('PASS: globals restored and same-step reload does not repeat update')

# Test generic wrapper behavior independently of NTG's reseeding on town creation.
a.execute('''
    local draws = {}
    local basic = {
        update=function() draws[#draws+1] = math.random(1,1000000) end,
        save=function() return {value=42} end,
        load=function(s) assert(s.value == 42 and s.__tpf2mp_deterministic_v1 == nil) end,
        guiUpdate=function() return os.time() end,
    }
    local x = compat.wrap(basic, 'test')
    sim=1000; x.update()
    local saved = x.save()
    sim=1001; x.update()
    local expected = draws[#draws]
    local y = compat.wrap(basic, 'test')
    y.load(saved); y.update()
    assert(draws[#draws] == expected)
    assert(y.guiUpdate == basic.guiUpdate and y.guiUpdate() == wall)
    -- A different script cannot consume this one's saved random stream.
    local z = compat.wrap(basic, 'other')
    sim=1002; z.update(); z.update()
    y.load(saved); sim=1001; y.update()
    assert(draws[#draws] == expected)
    local broken = compat.wrap({update=function() error('intentional failure') end}, 'broken')
    assert(not pcall(broken.update))
    assert(math.random == originalRandom and os.time == originalTime)
    local clock = compat.wrap({update=function()
        assert(os.time(os.date('!*t')) == os.time())
        assert(os.time({year=1970,month=1,day=1,hour=0}) == 0)
        assert(os.time({year=2000,month=2,day=29,hour=0}) == 951782400)
        assert(os.time({year=2000,month=13,day=1,hour=0}) == 978307200)
    end}, 'clock')
    clock.update()
''')
print('PASS: RNG save/reload, stream isolation, GUI exclusion, exception cleanup, UTC calendar')

# Exercise the actual runFn registration and its filename filter.
a.globals().mp_mod_path = (REPO / 'mod/mp_lockstep_1/mod.lua').as_posix()
a.execute('''
    package.preload['mp/deterministic_script'] = function() return compat end
    addModifier = function(name, fn) assert(name == 'loadGameScript'); modifier=fn end
    dofile(mp_mod_path); data().runFn({})
    local untouched = {update=function() end}
    assert(modifier('res/config/game_script/lockstep.lua', untouched) == untouched)
    assert(modifier('res/config/game_script/not_natural_town_growth.lua', untouched) == untouched)
    assert(modifier('res/config/game_script/natural_town_growth.lua', untouched) ~= untouched)
    assert(modifier('natural_town_growth.lua', untouched) ~= untouched)
''')
print('PASS: resource modifier wraps only Natural Town Growth')
