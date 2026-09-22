"""Validate terrain buffer renaming against verified native op semantics.

Requires lupa.lua52. No game files or running processes are modified; the
stock generators are only read. This checks operation graphs symbolically,
not a rendered engine terrain comparison.

The semantics below are written independently of mod/generation/bigmap_memory.lua
from docs/generation-op-semantics.md (Steam build 35924):
  full     the output never depends on its prior contents
  inplace  the output may share storage with an input that dies at the op
Any op that is not "full" is modelled as reading its old output.
"""
from pathlib import Path
import re
import tempfile
from lupa.lua52 import LuaRuntime
from install_generation_memory import install, GENERATORS, ANCHOR, REPLACEMENT

ROOT = Path(__file__).resolve().parents[1]
RES = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\res')
MODULE = ROOT / 'mod/generation/bigmap_memory.lua'
KEYS = ('input', 'input1', 'input2', 'output')
INPUTS = ('input', 'input1', 'input2')
TEMP = re.compile(r'__t_\d+$')

ONE, TWO, NONE = ('input',), ('input1', 'input2'), ()
SEMANTICS = {  # (layer.type, params.type): (inputs, full, inplace)
    ('FEATURE', 'CONSTANT'): (NONE, True, False),
    ('FEATURE', 'DATA'): (NONE, True, False),
    ('FEATURE', 'NOISE'): (NONE, True, False),
    ('FEATURE', 'RIDGED_NOISE'): (NONE, True, False),
    ('FEATURE', 'GRADIENT_NOISE'): (NONE, True, False),
    ('FEATURE', 'WHITE_NOISE'): (NONE, True, False),
    ('FEATURE', 'DITHERING'): (NONE, True, False),
    ('FEATURE', 'RIDGE'): (NONE, True, False),
    ('FEATURE', 'POINTS'): (NONE, False, False),
    ('FEATURE', 'RIVER'): (NONE, False, False),
    ('OP', 'MAP'): (ONE, True, True),
    ('OP', 'HERP'): (ONE, True, True),
    ('OP', 'PWLERP'): (ONE, True, True),
    ('OP', 'PWCONST'): (ONE, True, True),
    ('OP', 'WHITE_NOISE'): (ONE, True, True),
    ('OP', 'DISTANCE'): (ONE, True, True),
    ('OP', 'MESA'): (ONE, True, True),
    ('OP', 'GAUSS'): (ONE, True, False),
    ('OP', 'GRADIENT'): (ONE, True, False),
    ('OP', 'LAPLACE'): (ONE, True, False),
    ('OP', 'AXPY'): (ONE, False, False),
    ('MIX', 'MUL'): (TWO, True, True),
    ('MIX', 'ADD'): (TWO, True, True),
    ('MIX', 'COMP'): (TWO, True, True),
    ('MIX', 'PERCOLATION'): (TWO, True, True),
    ('MIX', 'MAD'): (TWO, False, False),
    ('MIX', 'MASK'): (TWO, False, False),
}


def native(value):
    if hasattr(value, 'items'):
        return {k: native(v) for k, v in value.items()}
    return value


def strings(value, out):
    if isinstance(value, str):
        out.add(value)
    elif isinstance(value, dict):
        for v in value.values():
            strings(v, out)
    return out


def indices(layers):
    return sorted(k for k in layers if isinstance(k, int))


def distinct(result):
    layers = result['layers']
    return len({layers[i]['params'][k] for i in indices(layers) for k in KEYS
                if layers[i]['params'].get(k) is not None})


def verify(before, after):
    layers, renamed = before['layers'], after['layers']
    order = indices(layers)
    assert order == indices(renamed) == list(range(1, len(order) + 1))
    assert {k: v for k, v in before.items() if k != 'layers'} == {
        k: v for k, v in after.items() if k != 'layers'}
    assert {k: v for k, v in layers.items() if not isinstance(k, int)} == {
        k: v for k, v in renamed.items() if not isinstance(k, int)}
    pinned = set()
    for k, v in before.items():
        if k != 'layers':
            strings(v, pinned)
    for k, v in layers.items():
        if not isinstance(k, int):
            strings(v, pinned)
    for i in order:
        a = layers[i]
        strings({k: v for k, v in a.items() if k != 'params'}, pinned)
        strings({k: v for k, v in a['params'].items() if k not in KEYS}, pinned)
    stock_names = {layers[i]['params'][k] for i in order for k in KEYS
                   if layers[i]['params'].get(k) is not None}
    fixed = {nm for nm in stock_names if nm in pinned or not TEMP.match(nm)}

    a_state, b_state = {}, {}
    for i in order:
        a, b = layers[i], renamed[i]
        assert {k: v for k, v in a.items() if k != 'params'} == {k: v for k, v in b.items() if k != 'params'}
        assert {k: v for k, v in a['params'].items() if k not in KEYS} == {
            k: v for k, v in b['params'].items() if k not in KEYS}
        inputs, full, inplace = SEMANTICS[(a['type'], a['params']['type'])]
        pa, pb = a['params'], b['params']
        for key in KEYS:
            assert (pa.get(key) is None) == (pb.get(key) is None), (i, key)
            if pa.get(key) is None:
                continue
            if pa[key] in fixed:
                assert pb[key] == pa[key], (i, key, pa[key], pb[key])
            else:
                assert TEMP.match(pb[key]) and pb[key] not in fixed and pb[key] not in pinned, (i, key, pb[key])
        present = [k for k in KEYS if pa.get(k) is not None]
        for x in present:
            for y in present:
                if x >= y:
                    continue
                if pa[x] == pa[y]:
                    assert pb[x] == pb[y], (i, 'stock alias lost', x, y)
                elif pb[x] == pb[y]:
                    # A new alias is only an output written over a dying input
                    # of an op verified to be safe in place.
                    assert 'output' in (x, y) and inplace, (i, 'new alias', a['params']['type'], x, y)
        # Provenance: every input, and the old output of a non-full op, must
        # hold exactly the value the stock pipeline would read.
        for key in inputs:
            assert a_state.get(pa[key], 0) == b_state.get(pb[key], 0), (i, key, pa[key], pb[key])
        if not full:
            assert a_state.get(pa['output'], 0) == b_state.get(pb['output'], 0), (i, 'output', pa['output'], pb['output'])
        a_state[pa['output']] = i
        b_state[pb['output']] = i
    for key in ('heightmapLayer', 'forestMap', 'assetsMap'):
        assert before.get(key) in pinned
        assert a_state.get(before.get(key), 0) == b_state.get(after.get(key), 0), key
    return distinct(before), distinct(after)


def lower_bound(result):
    """Fewest buffers any renaming could use under the same verified semantics.

    Fixed (pinned or non-temporary) names each keep a buffer; temporaries need
    one buffer per value alive at an op, less one when a full in-place-safe op
    may write over a temporary input that dies there.
    """
    layers = result['layers']
    order = indices(layers)
    pinned = set()
    for k, v in result.items():
        if k != 'layers':
            strings(v, pinned)
    for k, v in layers.items():
        if not isinstance(k, int):
            strings(v, pinned)
    for i in order:
        strings({k: v for k, v in layers[i].items() if k != 'params'}, pinned)
        strings({k: v for k, v in layers[i]['params'].items() if k not in KEYS}, pinned)
    names = {layers[i]['params'][k] for i in order for k in KEYS if layers[i]['params'].get(k) is not None}
    fixed = {nm for nm in names if nm in pinned or not TEMP.match(nm)}
    current, spans, events = {}, [], []
    for i in order:
        p = layers[i]['params']
        inputs, full, inplace = SEMANTICS[(layers[i]['type'], p['type'])]
        used = []
        for key in inputs:
            if p[key] not in current:
                current[p[key]] = [i, i]
                spans.append((p[key], current[p[key]]))
            current[p[key]][1] = i
            used.append(current[p[key]])
        if full or p['output'] not in current:
            span = [i, i]
            current[p['output']] = span
            spans.append((p['output'], span))
        current[p['output']][1] = i
        events.append((i, full and inplace, used, current[p['output']]))
    best = 0
    for i, can_share, used, out in events:
        live = [s for nm, s in spans if nm not in fixed and s[0] <= i <= s[1]]
        share = can_share and out[0] == i and any(u[1] == i and u is not out and any(u is s for s in live) for u in used)
        best = max(best, len(live) - (1 if share else 0))
    return len(fixed) + best


def stock_pipeline(name, water, seed, big=False):
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = str(RES / 'scripts/?.lua').replace('\\', '/') + ';' + L.globals().package.path
    L.execute('_=function(s) return s end; require "mathutil"')
    src = RES / 'config/terrain_generators' / (name + '.gen.lua')
    backup = src.with_name(src.name + '.bigmap-memory.bak')
    code = (backup if backup.exists() else src).read_bytes().replace(REPLACEMENT, ANCHOR)
    L.execute(code.decode('utf-8'))
    info = L.globals().data()
    p = L.table_from({row['key']: row['defaultIndex'] for _, row in info.params.items()})
    p.water, p.mapSizeX, p.mapSizeY = water, 12288, 24576
    if big:
        p.mapSizeX, p.mapSizeY = 58368, 291840
    p.bounds = L.table_from(dict(min=L.table_from(dict(x=-p.mapSizeX / 2, y=-p.mapSizeY / 2)),
                                 max=L.table_from(dict(x=p.mapSizeX / 2, y=p.mapSizeY / 2))))
    L.globals().math.randomseed(seed)
    return L, info.updateFn(p)


def optimize(L, result):
    lines = []
    L.globals().print = lambda *a: lines.append(' '.join(str(x) for x in a))
    optimizer = L.execute(MODULE.read_text(encoding='utf-8'))
    return optimizer.Optimize(result), lines


def main():
    source = MODULE.read_text(encoding='utf-8')
    code = re.sub(r'--[^\n]*', '', source)
    for banned in ('table.maxn', 'loadstring', 'setfenv', 'getfenv', 'table.getn', 'math.mod', 'string.gfind'):
        assert banned not in code, banned
    assert not re.search(r'(?<![\w.])unpack\s*\(', code), 'unpack'
    L = LuaRuntime(unpack_returned_tuples=True)
    assert L.eval('_VERSION') == 'Lua 5.2'
    assert L.execute('return load(...)', source) is not None, 'Lua 5.2 parse failed'

    counts = []
    for name in GENERATORS:
        for water in (0, 2, 4):
            for seed in (1, 35924):
                big = name == 'desert' and water == 2 and seed == 35924
                L, result = stock_pipeline(name, water, seed, big)
                before = native(result)
                optimized, lines = optimize(L, result)
                after = native(optimized)
                a, b = verify(before, after)
                if a != b:
                    assert lines == [f'[tpf2_bigmap] terrain memory: {a} -> {b} named buffers'], lines
                else:
                    assert lines == [] and before == after
                bound = lower_bound(before)
                assert bound <= b, (name, water, seed, bound, b)
                counts.append((name, water, seed, big, len(indices(before['layers'])), a, b, bound))
    assert len(counts) == 18 and any(a > b for *_, a, b, _ in counts)
    print('PASS: 18 real pipeline combinations (defaults, water 0/2/4, seeds 1/35924, one 292 km desert); '
          'parameters, order, pinned names, stock aliases, verified in-place aliases and symbolic contents preserved')
    print('generator  water  seed   size      layers  buffers before -> after  (lower bound)')
    for name, water, seed, big, n, a, b, bound in counts:
        print(f'{name:<10} {water:>5}  {seed:<5}  {"292 km" if big else "12x24 km":<8}  {n:>6}  {a:>7} -> {b:<5}  ({bound})')
    for name in GENERATORS:
        rows = sorted({(w, a, b) for n, w, _, _, _, a, b, _ in counts if n == name})
        print(f'{name}: ' + ', '.join(f'water {w}: {a} -> {b}' for w, a, b in rows))

    guards()
    installer()


GUARD_PIPELINE = '''
local function pipeline()
  return {heightmapLayer="HM", forestMap="HM", assetsMap="HM", layers={
    {type="FEATURE",params={type="CONSTANT",output="HM",params={value=1}}},
    {type="FEATURE",params={type="CONSTANT",output="__t_1",params={value=2}}},
    {type="MIX",params={type="ADD",input1="__t_1",input2="HM",output="HM"}},
    {type="FEATURE",params={type="CONSTANT",output="__t_2",params={value=3}}},
    {type="MIX",params={type="ADD",input1="__t_2",input2="HM",output="HM"}}}}
end
local function same(r) return r.layers[4].params.output=="__t_2" and r.layers[5].params.input1=="__t_2" end
'''


def guards():
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().opt = L.execute(MODULE.read_text(encoding='utf-8'))
    L.globals().print = lambda *a: None
    L.execute(GUARD_PIPELINE + '''
      local r=pipeline(); opt.Optimize(r)
      assert(r.layers[4].params.output=="__t_1" and r.layers[5].params.input1=="__t_1", "baseline reuse")
      r=pipeline(); r.extra={nested={"__t_2"}}; opt.Optimize(r); assert(same(r), "nested metadata pin")
      r=pipeline(); r.layers.colors={[4]="__t_2"}; opt.Optimize(r); assert(same(r), "layers metadata pin")
      r=pipeline(); r.layers[2].type="UNKNOWN"; opt.Optimize(r); assert(same(r), "unknown layer type")
      r=pipeline(); r.layers[2].params.type="MAD"; r.layers[2].type="OP"; opt.Optimize(r); assert(same(r), "unknown op schema")
      r=pipeline(); r.layers[3].type="MIX_THREE"; r.layers[3].params.input3="HM"; opt.Optimize(r); assert(same(r), "mix three")
      r=pipeline(); r.layers[5].params.input1=7; opt.Optimize(r); assert(r.layers[4].params.output=="__t_2", "non-string name")
      r=pipeline(); r.layers[3].params.input="HM"; opt.Optimize(r); assert(same(r), "unexpected name key")
      r=pipeline(); r.layers[4]={type="MIX",params={type="MASK",input1="HM",input2="HM",output="__t_2"}}
      opt.Optimize(r); assert(same(r), "masked write needs a fresh buffer")
      r=pipeline(); r.layers[4]={type="OP",params={type="AXPY",input="HM",output="__t_2",params={alpha=1}}}
      opt.Optimize(r); assert(same(r), "accumulating write needs a fresh buffer")
      r=pipeline(); r.layers[4]={type="FEATURE",params={type="POINTS",output="__t_2",params={points={},value=1}}}
      opt.Optimize(r); assert(same(r), "points write needs a fresh buffer")
      r=pipeline(); r.layers[4]={type="OP",params={type="MAP",input="__t_9",output="__t_2"}}
      opt.Optimize(r); assert(r.layers[4].params.input=="__t_9", "read before write stays fresh")
      -- The native converter maps any compare type string onto one of five full-write modes.
      for _, kind in ipairs({"MAX", "MIN", "EQUAL", "LESS", "GREATER", "NOPE"}) do
        r=pipeline(); r.layers[4]={type="MIX",params={type="COMP",input1="HM",input2="HM",output="__t_2",params={type=kind}}}
        opt.Optimize(r); assert(r.layers[4].params.output=="__t_1", "compare reuse " .. kind)
      end
    ''')
    # In place: a dying input may carry the output only for verified elementwise ops.
    L.execute('''
      local function chain(kind)
        return {heightmapLayer="HM", layers={
          {type="FEATURE",params={type="CONSTANT",output="__t_1",params={value=2}}},
          {type="OP",params={type=kind,input="__t_1",output="__t_2",params={}}},
          {type="OP",params={type="MAP",input="__t_2",output="HM",params={}}}}}
      end
      local r=chain("MAP"); opt.Optimize(r)
      assert(r.layers[2].params.output=="__t_1" and r.layers[3].params.input=="__t_1", "map in place")
      for _, kind in ipairs({"GAUSS","GRADIENT","LAPLACE"}) do
        r=chain(kind); opt.Optimize(r)
        assert(r.layers[2].params.output=="__t_2" and r.layers[3].params.input=="__t_2", kind)
      end
      r=chain("MAP"); r.layers[1]={type="FEATURE",params={type="RIVER",output="__t_1",params={}}}
      opt.Optimize(r); assert(r.layers[2].params.output=="__t_1", "river value may die into map")
    ''')
    print('PASS: guards (unknown layer/op schema, pinned metadata, non-string or unexpected names, compare modes, '
          'fresh buffers for masked/accumulating/points writes and read-before-write, in-place only for elementwise ops)')


def installer():
    with tempfile.TemporaryDirectory() as td:
        res = Path(td)
        for name in GENERATORS:
            path = res / 'config/terrain_generators' / (name + '.gen.lua')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'function f()\r\n\t\treturn result\r\nend\r\n')
        assert install(res) == 7
        assert install(res) == 0
        assert install(res, True) == 3
        assert install(res, True) == 0
        assert install(res) == 3
        broken = res / 'config/terrain_generators/tropical.gen.lua'
        broken.write_bytes(broken.read_bytes() + b'-- manual edit')
        snapshot = {p: p.read_bytes() for p in res.rglob('*') if p.is_file()}
        try:
            install(res, True)
        except ValueError:
            pass
        else:
            raise AssertionError('modified generator not refused')
        assert snapshot == {p: p.read_bytes() for p in res.rglob('*') if p.is_file()}
    print('PASS: install, restore, idempotence and conflict preflight (temporary directory)')


if __name__ == '__main__':
    main()
