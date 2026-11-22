"""Check src/generator_memory.h: the patched Fantasia Map Generator files it serves.

Requires out/tpf2_bigmap.dll (build.bat), lupa.lua52 and the Fantasia workshop
mod (2916150031), which is only read. For each Fantasia generator the DLL's
patched text is run like the game runs it and compared with the original:
identical pipeline at 128 x 128 tiles, and at 130, 160 and 192 tiles the symbolic replay of
test_generation_memory.verify with the buffer count down to the lower bound.
On Linux pass --native-library (libtest_generator_text.so), --fantasia-res
and --game-res; native CTest covers stream routing and installation guards.

Also checks which paths are redirected, that every original line keeps its
number, refusal of a missing or repeated anchor, and the %TEMP% copy.
"""
import ctypes as C
import argparse
from pathlib import Path
from lupa.lua52 import LuaRuntime
import test_generation_memory as t

ROOT = Path(__file__).resolve().parents[1]
FANTASIA = Path(r'C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\2916150031\res')
FILES = ('fantasia_map_generator.gen.lua', 'fantasia_map_generator_dry.gen.lua',
         'fantasia_map_generator_tropical.gen.lua')


def load_dll():
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    dll.BigmapTestGeneratorIndex.argtypes = [C.c_wchar_p]
    dll.BigmapTestGeneratorPatch.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.c_ulonglong]
    dll.BigmapTestGeneratorPatch.restype = C.c_longlong
    dll.BigmapTestGeneratorRedirect.argtypes = [C.c_wchar_p, C.c_wchar_p, C.c_size_t]
    return dll


def patch(dll, src, budget=0):
    n = dll.BigmapTestGeneratorPatch(src, len(src), None, 0, budget)
    if n < 0:
        return None
    buf = C.create_string_buffer(n)
    assert dll.BigmapTestGeneratorPatch(src, len(src), buf, n, budget) == n
    return buf.raw[:n]


def chain(result):
    """Longest run of layers the native scheduler must order: it orders layers
    per buffer name (docs/generation-op-semantics.md), so a layer waits for the
    last layer that touched any of its names."""
    layers, depth, best = result['layers'], {}, 0
    for i in t.indices(layers):
        p = layers[i]['params']
        names = {p[k] for k in t.KEYS if p.get(k) is not None}
        d = 1 + max((depth.get(nm, 0) for nm in names), default=0)
        for nm in names:
            depth[nm] = d
        best = max(best, d)
    return best


def runtime(lines):
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = ';'.join(str(d / 'scripts' / '?.lua') for d in (FANTASIA, t.RES)) + ';' + L.globals().package.path
    L.execute('_=function(s) return s end; require "mathutil"')
    L.globals().print = lambda *a: lines.append(' '.join(str(x) for x in a))
    L.globals().debugPrint = lambda *a: None
    return L


def generate(text, tiles):
    # The game passes mapSizeX/Y in heightmap samples, 64 * tiles + 1 (MEASURED
    # 12289 for a 192 x 192 tile map), not in metres.
    lines = []
    L = runtime(lines)
    L.execute(text.decode('utf-8'))
    info = L.globals().data()
    p = L.table_from({row['key']: row['defaultIndex'] for _, row in info.params.items()})
    p.water, p.mapSizeX, p.mapSizeY = 2, tiles * 64 + 1, tiles * 64 + 1
    p.bounds = L.table_from(dict(min=L.table_from(dict(x=-p.mapSizeX / 2, y=-p.mapSizeY / 2)),
                                 max=L.table_from(dict(x=p.mapSizeX / 2, y=p.mapSizeY / 2))))
    L.globals().math.randomseed(35924)
    return t.native(info.updateFn(p)), [x for x in lines if 'tpf2_bigmap' in x]


def main():
    global FANTASIA
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fantasia-res', type=Path, default=FANTASIA)
    ap.add_argument('--game-res', type=Path, default=t.RES)
    ap.add_argument('--native-library', type=Path)
    args = ap.parse_args()
    FANTASIA, t.RES = args.fantasia_res, args.game_res
    if args.native_library:
        dll = C.CDLL(str(args.native_library.resolve()))
        dll.BigmapTestGeneratorPatch = dll.TestPatch
        dll.TestPatch.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.c_ulonglong]
        dll.TestPatch.restype = C.c_longlong
        return check_generators(dll)
    dll = load_dll()
    gens = r'\res\config\terrain_generators' + '\\'
    ws = r'C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\2916150031'
    for path, want in ((ws + gens + FILES[0], 0), (ws + gens + FILES[1], 1), (ws + gens + FILES[2], 2),
                       (ws.replace('\\', '/') + gens.replace('\\', '/') + FILES[0].upper(), 0),
                       (r'D:\Games\TF2\mods\fantasia_1' + gens + FILES[2], 2),
                       (ws + gens + 'temperate.gen.lua', -1),
                       (ws + r'\res\scripts\terrain' + '\\' + FILES[0], -1),
                       (r'C:\Temp\tpf2_bigmap' + '\\' + FILES[0], -1),
                       (ws + gens + 'x' + FILES[0], -1)):
        assert dll.BigmapTestGeneratorIndex(path) == want, (path, want)
    assert patch(dll, b'local x = 1\n') is None
    assert patch(dll, b'\t\treturn result\n\t\treturn result\n') is None
    assert patch(dll, b'\t\treturn result -- no\n') is None
    assert patch(dll, b'x\t\treturn result\n') is None
    print('PASS: path matching and anchor refusal')

    if not check_generators(dll):
        return

    alt = C.create_unicode_buffer(260)
    path = str(FANTASIA / 'config/terrain_generators' / FILES[0])
    assert dll.BigmapTestGeneratorRedirect(path, alt, 260)
    served = Path(alt.value)
    assert served.name == FILES[0] and served.parent.name == 'tpf2_bigmap'
    body, last = served.read_bytes().rsplit(b"\n", 2)[:2]
    assert body == patch(dll, Path(path).read_bytes()).rsplit(b"\n", 2)[0]
    assert last.startswith(b"_tpf2_bigmap_budget = ") and int(last.split(b"= ")[1]) > 0, last
    assert dll.BigmapTestGeneratorRedirect(path, alt, 260)   # unchanged copy: served again
    # the copy reads as the original to a time check through its handle
    assert served.stat().st_mtime_ns == Path(path).stat().st_mtime_ns, (served.stat().st_mtime_ns, Path(path).stat().st_mtime_ns)
    assert not dll.BigmapTestGeneratorRedirect(str(t.RES / 'config/terrain_generators/temperate.gen.lua'), alt, 260)
    print(f'PASS: generator memory (redirect copy {served})')


def check_diagnostics(dll):
    # Exercise the actual embedded helper even without Workshop resources.
    src = b"function run(result, params)\n\t\treturn result\nend\n"
    lines = []
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().print = lambda *a: lines.append(' '.join(str(x) for x in a))
    L.execute(patch(dll, src).decode('utf-8'))
    L.execute("""
        r = {layers={{type="FUTURE", params={type="NEW", output="a"}}}}
        assert(run(r, {mapSizeX=12289, mapSizeY=12289}) == r)
        assert(r.layers[1].params.output == "a")
    """)
    assert lines == [
        '[tpf2_bigmap] generator memory: 12289 x 12289 samples (192 x 192 tiles), 1 layers over 1 buffer names',
        '[tpf2_bigmap] terrain memory: layer 1 of 1 (FUTURE NEW) is not a known op; pipeline left unchanged'], lines
    lines.clear()
    L.execute("assert(run(r, {mapSizeX=8193, mapSizeY=8193}) == r)")
    assert lines == ['[tpf2_bigmap] generator memory: 8193 x 8193 samples (128 x 128 tiles), 1 layers over 1 buffer names (32 x 32 km or less: unchanged)'], lines
    lines.clear()
    L.execute("assert(run(r, nil) == r)")
    assert lines == ['[tpf2_bigmap] generator memory: 0 x 0 samples (-0 x -0 tiles), 1 layers over 1 buffer names (32 x 32 km or less: unchanged)'], lines
    # Spy on the optimizer to check the exact area gate independently of its
    # conservative refusal of the unknown operation above, including rectangles.
    L.execute("""
        local calls = 0
        _tpf2_bigmap_memory.Optimize = function(result)
            calls = calls + 1
            return result
        end
        for _, dims in ipairs({{8192,8193,0}, {8193,8193,0}, {8193,8194,1},
                               {8194,8193,1}, {4096,16384,0}, {4097,16385,1}}) do
            local before = calls
            assert(run(r, {mapSizeX=dims[1], mapSizeY=dims[2]}) == r)
            assert(calls == before + dims[3])
        end
    """)
    # Budget reaches Optimize as a float-buffer count; small maps still bypass it.
    for budget, expected in ((0, None), (1, 0), (30 * 12289**2 * 4, 30)):
        L.execute(patch(dll, src, budget).decode('utf-8'))
        seen = []
        L.globals()._tpf2_bigmap_memory.Optimize = lambda result, cap: (seen.append(cap) or result)
        L.execute("run(r, {mapSizeX=12289, mapSizeY=12289})")
        assert seen == [expected], (budget, seen)
        seen.clear()
        L.execute("run(r, {mapSizeX=8193, mapSizeY=8193})")
        assert not seen
    print('PASS: embedded diagnostics, unknown-op refusal, sample-area boundary, rectangles and absent dimensions')


def check_generators(dll):
    check_diagnostics(dll)
    if not FANTASIA.is_dir():
        print(f'SKIP: Fantasia Map Generator not found at {FANTASIA}')
        return
    for name in FILES:
        src = (FANTASIA / 'config/terrain_generators' / name).read_bytes()
        out = patch(dll, src)
        assert out is not None, name
        a, b = src.splitlines(), out.splitlines()
        assert len(b) > len(a) and sum(x != y for x, y in zip(a, b)) == 1, name
        report = []
        for tiles in (130, 160, 192):
            before, _ = generate(src, tiles)
            after, lines = generate(out, tiles)
            x, y = t.verify(before, after)
            n, s = len(t.indices(before['layers'])), tiles * 64 + 1
            assert lines == [f'[tpf2_bigmap] generator memory: {s} x {s} samples ({tiles} x {tiles} tiles), '
                             f'{n} layers over {x} buffer names',
                             f'[tpf2_bigmap] terrain memory: {x} -> {y} named buffers'], lines
            assert y == t.lower_bound(before) and y <= 12, (y, t.lower_bound(before))
            report.append(f'{tiles}: {x} -> {y}')
            if tiles == 192:
                # A budget of 30 buffers (4-byte floats): more names, shorter chains.
                budget = 30 * s * s * 4
                wide, lines = generate(patch(dll, src, budget), tiles)
                xb, yb = t.verify(before, wide)
                assert lines[0].endswith(f', budget {budget / 1048576:.0f} MB = 30 buffers'), lines
                assert lines[1] == f'[tpf2_bigmap] terrain memory: {xb} -> {yb} named buffers (budget 30)', lines
                assert y < yb <= x, (y, yb, x)
                c0, c1, c2 = chain(before), chain(after), chain(wide)
                assert c2 < c1, (c0, c1, c2)
                report.append(f'budget 30: {yb} buffers; ordered chain stock {c0}, fewest {c1}, budget {c2}')
        small, _ = generate(src, 128)
        same, lines = generate(out, 128)
        assert same == small and len(lines) == 1 and lines[0].endswith('(32 x 32 km or less: unchanged)'), (name, lines)
        print(f'{name:<42} tiles {", ".join(report)} named buffers; 128: unchanged')
    return True



if __name__ == '__main__':
    main()
