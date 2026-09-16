"""Offline compatibility regressions using the actual downloaded Workshop script."""
from pathlib import Path
from natural_town_growth_probe import runtime

REPO = Path(__file__).resolve().parents[1]
SOURCE = (REPO / 'mod/mp_lockstep_1/res/scripts/mp/deterministic_script.lua').read_text()
GAME_INIT = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\res\scripts\init.lua').read_text()
# Use the shipped override, which discards unpack's start/end arguments.
GAME_UNPACK = GAME_INIT[GAME_INIT.index('local unpackhelper'):GAME_INIT.index('api = {}')]


def peer(wall):
    lua = runtime(wall)
    lua.execute(GAME_UNPACK)
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
    local returns = compat.wrap({handleEvent=function() return false,nil,42,nil end}, 'returns')
    local function capture(...) return {n=select('#',...),...} end
    local r=capture(returns.handleEvent())
    assert(r.n==4 and r[1]==false and r[2]==nil and r[3]==42 and r[4]==nil)
    local empty=compat.wrap({save=function() return nil end},'empty')
    assert(empty.save().__tpf2mp_deterministic_v1.empty)
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

# --- the 2026-09-15 town desync -------------------------------------------- #
# A game catching up at 4x advances the simulation clock in ~0.8 jumps where a 1x
# game advances 0.2. Natural Town Growth gates on "10 virtual seconds since the
# last update" and then SNAPS its deadline to the tick it happened to fire on, so
# the two crossed that gate at 10.4 vs 10.0 and their towns grew on permanently
# different schedules -- town lane -5, +2, +5 while every other hash lane matched.
# The wrapper now runs update once per whole virtual second, in order, so the tick
# sequence at any sim time is the same on every instance whatever its frame rate.
def run_sims(wall, start, sims):
    """One peer, driven frame by frame through the given simulation times."""
    p = peer(wall)
    p.execute('load_identical(200,%d,%d)' % (start, start))
    for sim in sims:
        p.globals().sim = sim
        p.execute('script.update()')
    return p


# Absolute times, never an accumulated sum: the engine's clock does not drift, and
# a harness that drifts would change the final floor() and fake a disagreement.
def evenly(step, start, stop):
    n = int(round((stop - start) / step))
    return [round(start + i * step, 6) for i in range(1, n + 1)]


def jitter(start, stop, seed=12345):
    """Irregular frame pacing: a machine whose frame rate wanders."""
    out, sim, s = [], float(start), seed
    while sim < stop:
        s = (s * 1103515245 + 12345) % 2147483648
        sim = min(stop, round(sim + 0.2 * (1 + s % 20), 6))   # 0.2 .. 4.0 units a frame
        out.append(sim)
    return out


START, STOP = 500, 900
one_x = run_sims(1000, START, evenly(0.2, START, STOP))           # 1x
four_x = run_sims(999999, START, evenly(0.8, START, STOP))        # 4x catch-up
coarse = run_sims(4242, START, evenly(3.2, START, STOP))          # a very fast catch-up
rough = run_sims(777, START, jitter(START, STOP))                 # a wandering frame rate

base = capacities(one_x)
assert len(base) > 0, 'the fixture produced no capacity writes'
for name, p in (('4x catch-up', four_x), ('16x catch-up', coarse), ('jittery frame rate', rough)):
    got = capacities(p)
    assert got == base, (f'{name} grew towns differently: {len(got)} writes vs {len(base)}; '
                         f'first difference at '
                         f'{next((i for i, (x, y) in enumerate(zip(got, base)) if x != y), min(len(got), len(base)))}')

# and the wrapper's own tick bookkeeping must land on the same tick
ticks = {name: p.eval("script.save().__tpf2mp_deterministic_v1.lastUpdate")
         for name, p in (('1x', one_x), ('4x', four_x), ('16x', coarse), ('jitter', rough))}
assert len(set(ticks.values())) == 1, ticks
print(f'PASS: 1x, 4x, 16x and jittery pacing all grow towns identically '
      f'({len(base)} writes, last tick {list(ticks.values())[0]})')


# THE ENGINE ECHOES load EVERY FRAME (2026-09-16). During play the engine syncs
# script state between its Lua states by calling save and then load with what
# save returned, once per frame -- 1,360 times in one session on the host. The
# wrapper used to re-apply every echo (RNG seed and grid anchor snapped back,
# the script's state table replaced with the saved copy), so what an update on
# the grid had done was undone or re-run depending on the FRAME the echo landed
# on. Frames are not sim-aligned, so two peers grew towns differently with every
# seed-related path deterministic. A game frame here is: update, then the echo
# (save+load) on some frames and not others, in a per-peer pattern.
def run_echoing(wall, start, sims, echo_every, phase):
    p = peer(wall)
    p.execute('load_identical(200,%d,%d)' % (start, start))
    for i, sim in enumerate(sims):
        p.globals().sim = sim
        p.execute('script.update()')
        if (i + phase) % echo_every == 0:
            p.execute('script.load(script.save())')      # the engine's per-frame echo
    return p


quiet = run_sims(1000, START, evenly(0.2, START, STOP))
echo_a = run_echoing(1000, START, evenly(0.2, START, STOP), 3, 0)   # echo on every 3rd frame
echo_b = run_echoing(5555, START, evenly(0.2, START, STOP), 7, 2)   # a different frame pattern
echo_c = run_echoing(31337, START, evenly(0.8, START, STOP), 2, 1)  # 4x catch-up, echo every other frame
base = capacities(quiet)
for name, p in (('echo every 3rd frame', echo_a), ('echo every 7th frame', echo_b), ('4x with echoes', echo_c)):
    got = capacities(p)
    assert got == base, (f'{name}: the engine echoing load changed the growth: {len(got)} writes vs {len(base)}; '
                         f'first difference at '
                         f'{next((i for i, (x, y) in enumerate(zip(got, base)) if x != y), min(len(got), len(base)))}')
ticks = {n: p.eval("script.save().__tpf2mp_deterministic_v1.lastUpdate") for n, p in (('quiet', quiet), ('a', echo_a), ('b', echo_b), ('c', echo_c))}
assert len(set(ticks.values())) == 1, ticks
# a REAL load (different metadata) is still applied: a fresh peer given echo_a's save lands on its tick and seed
fresh = peer(4242)
# carried across runtimes as a Lua literal (the mod's own serializer does exactly this for saves)
literal = echo_a.eval("(function() local s = require('natural_town_growth/serialization'); return s.stringify(script.save()) end)()")
fresh.globals().saved_literal = literal
fresh.globals().sim = STOP
fresh.execute('script.load(assert(load("return " .. saved_literal))())')
assert fresh.eval("script.save().__tpf2mp_deterministic_v1.lastUpdate") == ticks['a']
assert fresh.eval("script.save().__tpf2mp_deterministic_v1.seed") == echo_a.eval("script.save().__tpf2mp_deterministic_v1.seed")
print(f'PASS: the engine echoing load every frame no longer changes the growth ({len(base)} writes); a real load still applies')
