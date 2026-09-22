"""In-game minimap: native renderer, SetImage detour, capture thunk, installer,
game-script sync, and the Lua script under a mocked game API.

No game is launched and nothing under the Steam directory is written. Heights
and colours are checked against an independent model of the engine's layout
(docs/minimap-native-preview.md), on a synthetic CTerrain with a fake
BaseGetVertices. Requires lupa, pefile and capstone.
"""
import ctypes as C
import math
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import capstone
import pefile
from lupa.lua52 import LuaRuntime
from test_world_entry import Host, logtype, basetype, verifytype, hooktype

ROOT = Path(__file__).resolve().parents[1]
EXE = r'C:\tools\bin\TransportFever2.exe'
SCRIPT = ROOT / 'mod/minimap/bigmap_minimap.lua'
MARKER = b'-- tpf2_bigmap minimap'


class MsvcString(C.Structure):
    _fields_ = [('buf', C.c_char * 16), ('size', C.c_uint64), ('cap', C.c_uint64)]


PathFn = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.POINTER(MsvcString), C.c_uint64)
RawFn = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_void_p, C.c_uint64)
VertsFn = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_uint64)
StateFn = C.CFUNCTYPE(C.c_void_p, C.c_void_p)

dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
parse = dll.BigmapTestMinimapParse
parse.argtypes = [C.c_char_p, C.POINTER(C.c_double), C.POINTER(C.c_int), C.POINTER(C.c_double)]
render = dll.BigmapTestMinimapRender
render.argtypes = [C.c_void_p, VertsFn, C.c_char_p, C.c_void_p, C.c_uint64]
heights = dll.BigmapTestMinimapHeights
heights.argtypes = [C.c_void_p, VertsFn, C.c_char_p, C.c_void_p, C.c_uint64]
from_ui = dll.BigmapTestMinimapTerrainFromGameUI
from_ui.argtypes = [C.c_void_p, C.c_uint64, C.c_uint64]
from_ui.restype = C.c_void_p
set_image = dll.BigmapTestMinimapSetImage
set_image.argtypes = [C.POINTER(Host), C.c_void_p, C.POINTER(MsvcString), C.c_uint64, PathFn, RawFn, VertsFn,
                      C.c_void_p, C.c_uint64, C.c_uint64]
set_image.restype = C.c_void_p
thunk_fn = dll.BigmapTestMinimapCaptureThunk
thunk_fn.argtypes = [C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
thunk_fn.restype = C.c_void_p
sync_script = dll.BigmapTestSyncMinimapScript
sync_script.argtypes = [C.c_wchar_p, C.c_int]
script_text = dll.BigmapTestMinimapScriptText
script_text.argtypes = [C.c_char_p, C.c_uint64]
script_text.restype = C.c_uint64
install = dll.BigmapTestInstallMinimap
install.argtypes = [C.POINTER(Host), C.c_int, C.c_int, C.c_wchar_p]


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


# ---- request token ----------------------------------------------------------

def check_parser():
    rect, size, relief = (C.c_double * 4)(), (C.c_int * 2)(), C.c_double()
    ok = b'bigmap:minimap?x0=-32768&y0=-16384&x1=32768&y1=16384&w=1024&h=512&relief=4&g=7'
    assert parse(ok, rect, size, C.byref(relief))
    assert list(rect) == [-32768, -16384, 32768, 16384] and list(size) == [1024, 512] and relief.value == 4
    assert parse(b'bigmap:minimap?x0=-1.5&y0=0&x1=2.25&y1=9&w=16&h=4096', rect, size, C.byref(relief))
    assert relief.value == 4 and list(size) == [16, 4096]
    bad = [
        b'bigmap:minimap', b'bigmap:minimap?', b'bigmap:minimapx?x0=0',
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=64',                       # h missing
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=64&h=64&zoom=2',           # unknown key
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=64.5&h=64',                # fractional size
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=15&h=64',                  # below 16 px
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=4097&h=64',                # above 4096 px
        b'bigmap:minimap?x0=1&y0=0&x1=1&y1=1&w=64&h=64',                  # empty rectangle
        b'bigmap:minimap?x0=0&y0=0&x1=1e8&y1=1&w=64&h=64',                # unbounded
        b'bigmap:minimap?x0=nan&y0=0&x1=1&y1=1&w=64&h=64',
        b'bigmap:minimap?x0=inf&y0=0&x1=1&y1=1&w=64&h=64',
        b'bigmap:minimap?x0=0x&y0=0&x1=1&y1=1&w=64&h=64',                 # trailing garbage
        b'bigmap:minimap?x0=&y0=0&x1=1&y1=1&w=64&h=64',
        b'bigmap:minimap?=0&y0=0&x1=1&y1=1&w=64&h=64',
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=64&h=64&relief=65',
        b'ui/icons/main-menu/map_town.tga',
        b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=64&h=64&g=' + b'9' * 600,  # too long
    ]
    for token in bad:
        assert not parse(token, rect, size, C.byref(relief)), token
    print('PASS: request token parser (valid forms, 16 rejected forms)')


# ---- synthetic terrain ------------------------------------------------------

class FakeTerrain:
    """A CTerrain as MinimapReadTerrain reads it, with a fake BaseGetVertices."""

    def __init__(self, W, H, x0, y0, levels=6, res=4.0, z=0.5, off=-600.0, water=0.0, missing=()):
        self.W, self.H, self.x0, self.y0 = W, H, x0, y0
        self.per = 1 << levels
        self.res, self.z, self.off, self.water = res, z, off, water
        self.missing = set(missing)
        side = self.per + 1
        self.terrain = (C.c_uint8 * 0x60)()
        self.hdr = (C.c_uint8 * 0x18)()
        self.cells = (C.c_uint8 * (W * H * 0x28))()
        C.c_uint64.from_buffer(self.terrain, 0x18).value = C.addressof(self.hdr)
        C.c_int32.from_buffer(self.terrain, 0x28).value = levels
        C.c_float.from_buffer(self.terrain, 0x2c).value = res
        C.c_float.from_buffer(self.terrain, 0x30).value = res
        C.c_float.from_buffer(self.terrain, 0x34).value = z
        C.c_float.from_buffer(self.terrain, 0x3c).value = off
        C.c_float.from_buffer(self.terrain, 0x40).value = water
        for i, v in enumerate((x0, y0, W, H)):
            C.c_int32.from_buffer(self.hdr, 4 * i).value = v
        C.c_uint64.from_buffer(self.hdr, 0x10).value = C.addressof(self.cells)
        self.keep, self.vectors = [], {}
        for ry in range(H):
            for rx in range(W):
                idx = ry * W + rx
                C.c_int32.from_buffer(self.cells, idx * 0x28).value = -1 if (rx, ry) in self.missing else 5000 + idx
                data = (C.c_uint16 * (side * side))(
                    *[self.sample(rx, ry, ix, iy) for iy in range(side) for ix in range(side)])
                end = C.addressof(data) + 2 * side * side
                vec = (C.c_uint64 * 3)(C.addressof(data), end, end)
                self.keep += [data, vec]
                packed = (((y0 + ry) & 0xffffffff) << 32) | ((x0 + rx) & 0xffffffff)
                self.vectors[packed] = vec
        self.calls = []

        @VertsFn
        def verts(terrain, packed):
            assert terrain == C.addressof(self.terrain)
            self.calls.append(packed)
            vec = self.vectors.get(packed)
            return C.addressof(vec) if vec is not None else None
        self.verts = verts

    def sample(self, rx, ry, ix, iy):
        return 1000 + 100 * (ry * self.W + rx) + ix + 3 * iy

    @property
    def addr(self):
        return C.addressof(self.terrain)

    def height(self, rect, w, h, px, py):
        """Independent model: the engine's origin, row = y, row 0 = south."""
        x0, y0, x1, y1 = rect
        tile_w = self.per * self.res
        origin_x, origin_y = -(self.W // 2) * tile_w, -(self.H // 2) * tile_w
        step_x, step_y = (x1 - x0) / w, (y1 - y0) / h
        fy = (y0 + (py + 0.5) * step_y - origin_y) / tile_w
        fx = (x0 + (px + 0.5) * step_x - origin_x) / tile_w
        rxf, ryf = math.floor(fx), math.floor(fy)
        if rxf < 0 or ryf < 0 or rxf >= self.W or ryf >= self.H or (rxf, ryf) in self.missing:
            return float('nan')
        ix = min(self.per, int((fx - rxf) * self.per + 0.5))
        iy = min(self.per, int((fy - ryf) * self.per + 0.5))
        return self.sample(rxf, ryf, ix, iy) * self.z + self.off


def token(rect, w, h, relief=0):
    return ('bigmap:minimap?x0=%r&y0=%r&x1=%r&y1=%r&w=%d&h=%d&relief=%r' % (*rect, w, h, relief)).encode()


def rgb(r, g, b):
    return (f32(r / 255), f32(g / 255), f32(b / 255))


def expected_colour(h, water):
    """Flat shading (relief 0): n = (0,0,1), sun = 1/sqrt(3), ambient = 1."""
    if h != h:
        c = rgb(26, 28, 32)
    elif h < water:
        t = max(0.0, min(1.0, (h - water) / (-100 - water)))
        a, b = rgb(100, 135, 158), rgb(60, 80, 91)
        c = tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
    else:
        levels = [(0.0, rgb(93, 112, 66)), (100.0, rgb(64, 78, 50)), (550.0, rgb(242, 235, 220))]
        if not h > 0:
            base = levels[0][1]
        elif h >= 550:
            base = levels[2][1]
        else:
            i = 0 if h < 100 else 1
            t = (h - levels[i][0]) / (levels[i + 1][0] - levels[i][0])
            base = tuple(levels[i][1][k] + (levels[i + 1][1][k] - levels[i][1][k]) * t for k in range(3))
        amb, sun = rgb(204, 230, 255), rgb(255, 255, 204)
        s = 1 / math.sqrt(3)
        c = tuple(base[k] * (amb[k] + s * sun[k]) * 0.5 for k in range(3))
    return tuple(0 if v <= 0 else 255 if v >= 1 else int(v * 255 + 0.5) for v in c)


def check_renderer():
    t = FakeTerrain(W=4, H=2, x0=-2, y0=-1, missing={(3, 1)})
    rect = (-512.0, -256.0, 512.0, 256.0)            # exactly the map: tiles are 256 m
    w, h = 64, 32
    out = (C.c_float * (w * h))()
    assert heights(t.addr, t.verts, token(rect, w, h), out, w * h)
    nan = 0
    for py in range(h):
        for px in range(w):
            want, got = t.height(rect, w, h, px, py), out[py * w + px]
            if want != want:
                assert got != got, (px, py, got)
                nan += 1
            else:
                assert got == want, (px, py, got, want)
    assert nan == (w // 4) * (h // 2)                  # exactly the missing tile
    # Row 0 is the south edge: the bottom-left pixel reads tile (0, 0) near its
    # south-west corner, the top-right one tile (3, 1)'s neighbour (2, 1) region.
    assert out[0] == t.sample(0, 0, 2, 2) * t.z + t.off, out[0]
    # Every BaseGetVertices call named an existing tile in tile coordinates, never the missing one.
    tiles = {(struct.unpack('<i', struct.pack('<I', p & 0xffffffff))[0],
              struct.unpack('<i', struct.pack('<I', p >> 32))[0]) for p in t.calls}
    assert tiles == {(t.x0 + rx, t.y0 + ry) for ry in range(2) for rx in range(4)} - {(t.x0 + 3, t.y0 + 1)}, tiles
    assert len(t.calls) <= w * h

    # A rectangle wider than the map: the outside is NaN, the inside unchanged.
    big = (-1024.0, -512.0, 1024.0, 512.0)
    out2 = (C.c_float * (w * h))()
    assert heights(t.addr, t.verts, token(big, w, h), out2, w * h)
    for py in range(h):
        for px in range(w):
            want, got = t.height(big, w, h, px, py), out2[py * w + px]
            assert (got != got) if want != want else got == want, (px, py)

    # Colours, flat shading, on a render tall enough to use several threads.
    t2 = FakeTerrain(W=4, H=2, x0=-2, y0=-1, missing={(0, 1)}, off=-560.0, water=0.0)
    w, h = 256, 128
    rgba = (C.c_uint8 * (w * h * 4))()
    assert render(t2.addr, t2.verts, token(big, w, h), rgba, len(rgba))
    kinds = {'outside': 0, 'water': 0, 'land': 0}
    for py in range(h):
        for px in range(w):
            hh = t2.height(big, w, h, px, py)
            want = expected_colour(hh, t2.water)
            got = tuple(rgba[(py * w + px) * 4 + k] for k in range(4))
            assert got[3] == 255 and all(abs(got[k] - want[k]) <= 1 for k in range(3)), (px, py, hh, got, want)
            kinds['outside' if hh != hh else 'water' if hh < t2.water else 'land'] += 1
    assert all(v > 200 for v in kinds.values()), kinds    # every branch well covered

    # Relief shades slopes but never water or the outside; the output is deterministic.
    shaded = (C.c_uint8 * (w * h * 4))()
    assert render(t2.addr, t2.verts, token(big, w, h, relief=8), shaded, len(shaded))
    again = (C.c_uint8 * (w * h * 4))()
    assert render(t2.addr, t2.verts, token(big, w, h, relief=8), again, len(again))
    assert bytes(shaded) == bytes(again)
    changed = 0
    for i in range(w * h):
        py, px = divmod(i, w)
        hh = t2.height(big, w, h, px, py)
        if hh != hh or hh < t2.water:
            assert bytes(shaded[i * 4:i * 4 + 4]) == bytes(rgba[i * 4:i * 4 + 4])
        elif bytes(shaded[i * 4:i * 4 + 3]) != bytes(rgba[i * 4:i * 4 + 3]):
            changed += 1
    assert changed > 1000, changed

    # Unreadable terrain: refused, not guessed.
    bad = FakeTerrain(W=2, H=2, x0=-1, y0=-1)
    C.c_int32.from_buffer(bad.terrain, 0x28).value = 0
    assert not render(bad.addr, bad.verts, token(rect, 64, 32), rgba, len(rgba))
    C.c_int32.from_buffer(bad.terrain, 0x28).value = 6
    C.c_uint64.from_buffer(bad.hdr, 0x10).value = 0
    assert not render(bad.addr, bad.verts, token(rect, 64, 32), rgba, len(rgba))
    assert not render(None, bad.verts, token(rect, 64, 32), rgba, len(rgba))
    print('PASS: renderer: every height against the engine layout model (origin, packed tile, row = y, south in row 0, '
          'missing tile, outside the map), every colour within 1 (water ramp, height ramp, flat shade, threads), '
          'relief only on land, unreadable terrain refused')
    return t


# ---- the game UI accessor chain ---------------------------------------------

class FakeGameUI:
    def __init__(self, terrain_addr):
        self.ui = (C.c_uint8 * 0x460)()
        self.acc = (C.c_uint64 * 1)()
        self.vtbl = (C.c_uint64 * 2)()
        self.state = (C.c_uint8 * 0x28)()
        self.seen = []

        @StateFn
        def state_fn(acc):
            self.seen.append(acc)
            return C.addressof(self.state)
        self.state_fn = state_fn
        C.c_uint64.from_buffer(self.ui, 0x450).value = C.addressof(self.acc)
        self.acc[0] = C.addressof(self.vtbl)
        self.vtbl[1] = C.cast(state_fn, C.c_void_p).value
        C.c_uint64.from_buffer(self.state, 0x20).value = terrain_addr or 0

    @property
    def addr(self):
        return C.addressof(self.ui)


def check_accessor(t):
    ui = FakeGameUI(t.addr)
    assert from_ui(ui.addr, 0, 2 ** 64 - 1) == t.addr and ui.seen == [C.addressof(ui.acc)]
    # A vtable outside the image range is never called.
    lo = C.addressof(ui.vtbl) + 64
    assert from_ui(ui.addr, lo, lo + 4096) is None and len(ui.seen) == 1
    assert from_ui(None, 0, 2 ** 64 - 1) is None
    C.c_uint64.from_buffer(ui.ui, 0x450).value = 0
    assert from_ui(ui.addr, 0, 2 ** 64 - 1) is None and len(ui.seen) == 1
    print('PASS: terrain accessor *(CGameUI+0x450) -> vtbl[1] -> +0x20, with the vtable range check')


# ---- SetImage detour ----------------------------------------------------------

def make_string(text):
    s = MsvcString()
    raw = text.encode()
    if len(raw) <= 15:
        s.buf = raw
        s.cap = 15
        keep = None
    else:
        keep = C.create_string_buffer(raw, len(raw) + 1)
        C.c_uint64.from_buffer(s, 0).value = C.addressof(keep)
        s.cap = len(raw)
    s.size = len(raw)
    return s, keep


def string_value(p):
    s = p.contents
    if s.cap > 15:
        return C.string_at(C.c_uint64.from_buffer(s, 0).value, s.size)
    return bytes(s.buf)[:s.size]


def check_detour(t):
    logs = []

    @logtype
    def log(fmt):
        logs.append(fmt.decode())

    @basetype
    def no_base():
        return 0

    @verifytype
    def no_verify(rva, p, n):
        raise AssertionError('the detour never verifies bytes')

    @hooktype
    def no_hook(target, detour, n, out):
        raise AssertionError('the detour never installs hooks')

    host = Host(C.sizeof(Host), 1, log, None, None, None, no_base, None, no_verify, no_hook, None, None)
    ui = FakeGameUI(t.addr)
    originals, raws = [], []

    @PathFn
    def original(iv, path, flag):
        s = path.contents
        originals.append((iv, string_value(path), s.size, s.cap, C.c_uint64.from_buffer(s, 0).value, flag))
        return 0xabc

    @RawFn
    def raw(iv, channels, w, h, vec, validate):
        begin, end = C.c_uint64.from_address(vec).value, C.c_uint64.from_address(vec + 8).value
        raws.append((iv, channels, w, h, C.string_at(begin, end - begin), validate))
        return 0

    def call(text, gameui=ui.addr, lo=0, hi=2 ** 64 - 1, iv=0x5150, flag=0x101):
        s, keep = make_string(text)
        before_ptr = C.c_uint64.from_buffer(s, 0).value
        r = set_image(C.byref(host), iv, C.byref(s), flag, original, raw, t.verts, gameui, lo, hi)
        return r, s, keep, before_ptr

    # Anything that is not our token reaches the original unchanged, by the same pointer.
    for text in ['ui/a.tga', 'ui/icons/windows/overview.tga', 'bigmap:minima', '']:
        r, s, keep, ptr = call(text)
        assert r == 0xabc and originals[-1][1] == text.encode() and originals[-1][5] == 0x101 and not raws
    n_orig = len(originals)

    # A token: the original gets the placeholder written into the SAME buffer
    # (ownership unchanged), then the raw upload carries the rendered pixels.
    rect = (-512.0, -256.0, 512.0, 256.0)
    tok = token(rect, 64, 32).decode() + '&g=3'
    r, s, keep, ptr = call(tok)
    assert r == 0xabc and len(originals) == n_orig + 1
    iv, value, size, cap, buf_ptr, flag = originals[-1]
    assert value == b'ui/icons/main-menu/map_town.tga' and size == 31 and cap == len(tok) and buf_ptr == ptr
    assert len(raws) == 1
    iv2, channels, w, h, pixels, validate = raws[0]
    assert (iv2, channels, w, h, validate & 0xff) == (0x5150, 4, 64, 32, 1) and len(pixels) == 64 * 32 * 4
    direct = (C.c_uint8 * (64 * 32 * 4))()
    assert render(t.addr, t.verts, tok.encode(), direct, len(direct))
    assert pixels == bytes(direct)
    assert any(line.startswith('minimap: rendered') for line in logs)

    # A token too short for the placeholder: an empty path (the game's missing
    # image), and malformed so nothing is rendered.
    r, s, keep, ptr = call('bigmap:minimap')
    assert originals[-1][1] == b'' and originals[-1][2] == 0 and len(raws) == 1
    assert any('malformed' in line for line in logs)
    # Malformed tokens render nothing. Shorter than the 31-byte placeholder: the
    # buffer cannot hold it, so an empty path; long enough: the placeholder.
    r, s, keep, ptr = call('bigmap:minimap?x0=1&y0=2')
    assert originals[-1][1] == b'' and originals[-1][2] == 0 and len(raws) == 1
    r, s, keep, ptr = call('bigmap:minimap?x0=1&y0=2&x1=3&zoom=4')
    assert originals[-1][1] == b'ui/icons/main-menu/map_town.tga' and originals[-1][4] == ptr and len(raws) == 1
    # No game UI, or a vtable outside the image: placeholder, no render, logged.
    r, s, keep, ptr = call(tok, gameui=None)
    assert len(raws) == 1 and any('no readable terrain' in line for line in logs)
    r, s, keep, ptr = call(tok, lo=1, hi=2)
    assert len(raws) == 1
    print('PASS: SetImage detour: non-tokens pass through untouched; a token keeps the string buffer and capacity, '
          'hands the original a real icon path, and uploads exactly the rendered RGBA; short, malformed and '
          'terrain-less requests render nothing')


# ---- capture thunk ----------------------------------------------------------

def check_thunk():
    slot, tramp = C.c_void_p(0), C.c_void_p(0)
    seen = []
    Six = C.CFUNCTYPE(C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_int64)

    @Six
    def target(a, b, c, d, e, f):
        seen.append((a, b, c, d, e, f))
        return 0x77

    tramp.value = C.cast(target, C.c_void_p).value
    thunk = thunk_fn(C.byref(slot), C.byref(tramp))
    assert thunk
    code = C.string_at(thunk, 25)
    assert code[:2] == b'\x48\xb8' and code[10:13] == b'\x48\x89\x08' and code[13:15] == b'\x48\xb8' and code[23:] == b'\xff\x20'
    assert struct.unpack('<Q', code[2:10])[0] == C.addressof(slot) and struct.unpack('<Q', code[15:23])[0] == C.addressof(tramp)
    call = Six(thunk)
    assert call(0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666) == 0x77
    assert slot.value == 0x1111 and seen == [(0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666)]
    print('PASS: capture thunk stores rcx and reaches the original with all four register and two stack arguments')


# ---- script sync ------------------------------------------------------------

def check_script_sync(tmp):
    src = SCRIPT.read_bytes().replace(b'\r\n', b'\n')
    cap = script_text(None, 0)
    buf = C.create_string_buffer(cap + 1)
    assert script_text(buf, cap + 1) == len(src) and buf.raw[:cap] == src, 'embedded script differs from the source'
    assert src.startswith(MARKER)
    path = tmp / 'bigmap_minimap.lua'
    assert sync_script(str(path), 1) == 3 and path.read_bytes() == src      # written
    assert sync_script(str(path), 1) == 2                                     # up to date
    path.write_bytes(MARKER + b'\n-- an old version\n')
    assert sync_script(str(path), 1) == 3 and path.read_bytes() == src        # rewritten
    assert sync_script(str(path), 0) == 1 and not path.exists()               # removed
    assert sync_script(str(path), 0) == 0                                     # absent
    path.write_bytes(b'-- somebody else\n')
    for want in (1, 0):
        assert sync_script(str(path), want) == 4 and path.read_bytes() == b'-- somebody else\n'
    path.unlink()
    print('PASS: game script: embedded copy equals the source; written, up to date, rewritten, removed, absent; '
          'a foreign file is never touched')


# ---- installer --------------------------------------------------------------

def check_installer(tmp):
    pe = pefile.PE(EXE, fast_load=True)
    header = C.create_string_buffer(open(EXE, 'rb').read(0x1000), 0x1000)
    base_addr = C.addressof(header)
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    events, errors = [], []
    failure = None

    @logtype
    def log(fmt):
        pass

    @basetype
    def base():
        return base_addr

    @verifytype
    def verify(rva, p, n):
        events.append(('verify', rva))
        code = C.string_at(p, n)
        if code != pe.get_data(rva, n):
            errors.append(('bytes', hex(rva)))
        if rva in (0x22b4610, 0x5a2900):
            ins = list(dis.disasm(code, 0x140000000 + rva))
            if sum(i.size for i in ins) != n or n < 14:
                errors.append(('boundary', hex(rva)))
            for i in ins:
                if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or any(
                        o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP for o in i.operands):
                    errors.append(('steal', hex(rva), i.mnemonic))
        return failure != ('verify', rva)

    @hooktype
    def hook(target, detour, n, out):
        rva = target - base_addr
        events.append(('hook', rva, n))
        out[0] = 0x1234
        return failure != ('hook', rva)

    host = Host(C.sizeof(Host), 1, log, None, None, None, base, None, verify, hook, None, None)
    script = tmp / 'installer_minimap.lua'
    src = SCRIPT.read_bytes().replace(b'\r\n', b'\n')
    sites = [0x22b4610, 0x5a2900, 0x22b4350, 0x33d130, 0x8bb7f0, 0x8bb820, 0x5a2999,
             0x33d270, 0x33d280, 0x33d560, 0x33d7f0, 0x33d540]

    events.clear()
    assert install(C.byref(host), 0, 1, str(script))
    assert [e[1] for e in events if e[0] == 'verify'] == sites
    assert [(e[1], e[2]) for e in events if e[0] == 'hook'] == [(0x5a2900, 20), (0x22b4610, 20)]
    assert script.read_bytes() == src

    # Off, GOG, any byte mismatch or any hook failure: no (further) hooks, script removed.
    for gog, enabled in ((0, 0), (1, 1)):
        script.write_bytes(src)
        events.clear()
        assert not install(C.byref(host), gog, enabled, str(script))
        assert not [e for e in events if e[0] in ('verify', 'hook')] and not script.exists()
    for rva in sites:
        failure = ('verify', rva)
        script.write_bytes(src)
        events.clear()
        assert not install(C.byref(host), 0, 1, str(script))
        assert not [e for e in events if e[0] == 'hook'] and not script.exists(), hex(rva)
    failure = ('hook', 0x5a2900)
    script.write_bytes(src)
    events.clear()
    assert not install(C.byref(host), 0, 1, str(script))
    assert [e[1] for e in events if e[0] == 'hook'] == [0x5a2900] and not script.exists()
    failure = ('hook', 0x22b4610)
    script.write_bytes(src)
    assert not install(C.byref(host), 0, 1, str(script)) and not script.exists()
    failure = None
    assert not errors, errors
    print('PASS: installer verifies all 12 pinned sites against the Steam exe (whole-instruction 20-byte steals, '
          'nothing RIP-relative), hooks capture before image, writes the script only when both hooks install, '
          'and removes it when off, on GOG, on any mismatch or hook failure')


# ---- network overlay ----------------------------------------------------------

accept_network = dll.BigmapTestMinimapAcceptNetwork
accept_network.argtypes = [C.c_char_p, C.c_uint64, C.POINTER(C.c_uint64)]
parse_extra = dll.BigmapTestMinimapParseExtra
parse_extra.argtypes = [C.c_char_p, C.POINTER(C.c_int), C.POINTER(C.c_uint64), C.POINTER(C.c_uint64)]


def check_network(t):
    layers, net, hide = C.c_int(), C.c_uint64(), C.c_uint64()
    assert parse_extra(b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=16&h=16&layers=7&net=123456789012&hide=5',
                       C.byref(layers), C.byref(net), C.byref(hide))
    assert (layers.value, net.value, hide.value) == (7, 123456789012, 5)
    assert parse_extra(b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=16&h=16', C.byref(layers), C.byref(net), C.byref(hide))
    assert (layers.value, net.value, hide.value) == (0, 0, 0)
    for bad in (b'layers=8', b'layers=1.5', b'net=-1', b'net=0.5', b'hide=-2', b'hide=1.5'):
        assert not parse_extra(b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=16&h=16&' + bad,
                               C.byref(layers), C.byref(net), C.byref(hide)), bad
    # The header is still one line: a payload after it does not reach the parser.
    assert parse(b'bigmap:minimap?x0=0&y0=0&x1=1&y1=1&w=16&h=16\nE 3 0 0 0 0 0 0 0 0 0',
                 (C.c_double * 4)(), (C.c_int * 2)(), C.byref(C.c_double()))

    # 8 m per texture pixel, and every line 4 m off a pixel boundary, so its
    # centre runs through pixel centres and full coverage is exact.
    rect = (-512.0, -256.0, 512.0, 256.0)
    w, h = 128, 64

    def header(layers, net, hide=0):
        return ('bigmap:minimap?x0=%r&y0=%r&x1=%r&y1=%r&w=%d&h=%d&relief=0&layers=%d&net=%d&hide=%d'
                % (*rect, w, h, layers, net, hide)).encode()

    payload = b'\n'.join([
        b'P 0 255 0 0',
        b'P 1 0 0 255',
        b'E 3 0 -400 4 800 0 400 4 800 0',                # red track, row 32
        b'E 3 1 -400 108 800 0 400 108 800 0',            # blue track, row 45 ((108+256)/8 = 45.5)
        b'E 0 -1 -400 -92 800 0 400 -92 800 0',           # urban street, row 20 ((-92+256)/8 = 20.5)
        b'S 0 1 0 -148 1000 0 60',                         # blue station, row 13
        b'this is not a line',
        b'E 9 0 1 2 3 4 5 6 7 8',                           # class out of range
        b'P 99 1 2 3',                                       # palette index out of range
        b'',
    ])
    token = header(7, 77) + b'\n' + payload
    counts = (C.c_uint64 * 3)()
    assert accept_network(token, len(token), counts)
    assert list(counts) == [3, 1, 3], list(counts)
    assert not accept_network(header(7, 0) + b'\n' + payload, len(header(7, 0)) + 1 + len(payload), counts)

    def picture(layers, net, hide=0):
        out = (C.c_uint8 * (w * h * 4))()
        tok = header(layers, net, hide)
        assert render(t.addr, t.verts, tok, out, len(out))
        return out

    def px(buf, x, y):
        return tuple(buf[(y * w + x) * 4 + k] for k in range(3))

    bare = picture(0, 77)
    full = picture(7, 77)
    assert px(full, 64, 32) == (255, 0, 0) and px(full, 64, 45) == (0, 0, 255), (px(full, 64, 32), px(full, 64, 45))
    assert px(full, 64, 13) == (0, 0, 255), px(full, 64, 13)
    # The street blends 70% of its colour over the terrain (a segment joint may
    # apply it twice, so between one and two applications).
    road, ground = px(full, 64, 20), px(bare, 64, 20)
    urban = (0xd8, 0xd3, 0xc8)
    for k in range(3):
        once = ground[k] + (urban[k] - ground[k]) * 0.7
        twice = once + (urban[k] - once) * 0.7
        assert min(once, twice) - 1.5 <= road[k] <= max(once, twice) + 1.5, (k, road, ground)
    # Far from every line nothing changes.
    assert px(full, 10, 60) == px(bare, 10, 60)
    # A hidden company: its track is gone, the other stays.
    hidden = picture(7, 77, hide=1)
    assert px(hidden, 64, 32) == px(bare, 64, 32) and px(hidden, 64, 45) == (0, 0, 255)
    # Layer bits: stations only.
    stations = picture(4, 77)
    assert px(stations, 64, 32) == px(bare, 64, 32) and px(stations, 64, 20) == px(bare, 64, 20)
    assert px(stations, 64, 13) == (0, 0, 255)
    # Another world's id: nothing of this network is drawn.
    stale = picture(7, 78)
    assert bytes(stale) == bytes(bare)
    print('PASS: network payload: header layers/net/hide, P/E/S lines with rejected lines counted, and on a synthetic '
          'terrain tracks in their exact palette colours, a street blended by class, a station on top, a hidden '
          'company, layer bits and another world\'s network id all drawn exactly as requested')


# ---- Lua ----------------------------------------------------------------------
#
# The mock reproduces the units measured in game: setMinimumSize takes UI units
# that are multiplied by SCALE on screen, while AbsoluteLayout rects,
# getContentRect() and getMousePos() are screen pixels, and rect.y is either
# the item's vertical centre (CENTRE) or its top edge. Assertions use the
# mock's ON-SCREEN rectangles, so they hold whichever convention is real. The
# world has tracks, every street class, a rail station and a roadside stop,
# three engine players, and a coal mine; OWNERS picks who owns what.

LUA_HARNESS = r'''
local created = {}
local IMAGES = {}
local function reg(kind, obj)
    created[kind] = created[kind] or {}
    table.insert(created[kind], obj)
    return obj
end
local Comp = {}
Comp.__index = Comp
local function comp(kind, arg)
    return reg(kind, setmetatable({ kind = kind, arg = arg, visible = true, listeners = {}, lines = {} }, Comp))
end
function Comp:setMinimumSize(s) self.minSize = { w = s.w, h = s.h } end
function Comp:setMaximumSize(s) self.maxSize = { w = s.w, h = s.h } end
function Comp:setTooltip(t) self.tooltip = t end
function Comp:insertMouseListener(f) table.insert(self.listeners, f) end
function Comp:setImage(p, b) self.image = p; IMAGES[#IMAGES + 1] = p end
function Comp:setText(t) self.text = t end
function Comp:setVisible(v, b) self.visible = v end
function Comp:setLayout(l) self.layout = l; l.owner = self end
function Comp:onStep(f) self.stepFn = f; local me = self; return { disconnect = function() me.stepFn = nil end } end
function Comp:destroy() self.destroyed = true end
function Comp:setColor(c) self.color = c end
function Comp:setWidth(w) self.width = w end
function Comp:clear() self.lines = {} end
function Comp:addLine(a, b) table.insert(self.lines, { a.x, a.y, b.x, b.y }) end
function Comp:onClick(f) self.clickFn = f end
function Comp:onToggle(f) self.toggleFn = f end
function Comp:setSelected(v, b) self.selected = v end
function Comp:setPosition(x, y) self.pos = { x, y } end
function Comp:addHideOnCloseHandler() end
function Comp:onClose(f) self.closeFn = f end
function Comp:close() self.visible = false; if self.closeFn then self.closeFn() end end
function Comp:getContentRect()
    if self.parentLayout and self.rectIn then
        local owner = self.parentLayout.owner
        local o = owner and owner:getContentRect()
        if not o or o.w <= 0 then return { x = 0, y = 0, w = 0, h = 0 } end
        local r = self.rectIn
        local w = self.minSize and self.minSize.w * SCALE or r.w
        local h = self.minSize and self.minSize.h * SCALE or r.h
        local top = CENTRE and (r.y - h / 2) or r.y
        return { x = o.x + r.x, y = o.y + top, w = w, h = h }
    end
    if self.minSize then
        return { x = 100, y = 200, w = self.minSize.w * SCALE, h = self.minSize.h * SCALE }
    end
    return { x = 0, y = 0, w = 0, h = 0 }
end
local Layout = {}
Layout.__index = Layout
local function layout(kind)
    return reg(kind, setmetatable({ kind = kind, items = {} }, Layout))
end
function Layout:addItem(c, r)
    table.insert(self.items, c)
    c.parentLayout = self
    if r then c.rectIn = { x = r.x, y = r.y, w = r.w, h = r.h } end
end
function Layout:insertItem(c, i) table.insert(self.items, i + 1, c) end
function Layout:removeItem(c)
    for i = #self.items, 1, -1 do if self.items[i] == c then table.remove(self.items, i) end end
end
function Layout:getItem(i) return self.items[i + 1] end

local toolbar = layout("Toolbar")
local mainButtons = layout("MainButtons")
mainButtons.items = { toolbar }
local cam = { x = 1000, y = -2000, z = 500 }
local controller = {
    getCameraData = function(self) return { x = cam.x, y = cam.y, z = cam.z } end,
    setCameraData = function(self, d) cam.set = { d.x, d.y } end,
    focus = function(self, e, b) cam.focused = e end,
}
local MOUSE = { 0, 0 }
local EX, EY = 32768, 16384
local CT = { TOWN = 1, SIM_BUILDING = 2, CONSTRUCTION = 3, NAME = 4, BASE_EDGE = 5, BASE_NODE = 6, BASE_EDGE_TRACK = 7,
             BASE_EDGE_STREET = 8, PLAYER_OWNED = 9, BOUNDING_VOLUME = 10, STATION = 11, PLAYER = 12 }
local MINE, P2, P3 = 900, 950, 970
local SOLO = OWNERS == "solo"
local nodes = { [601] = { -1000, 0 }, [602] = { 1000, 0 }, [603] = { -1000, 500 }, [604] = { 1000, 500 },
    [605] = { -500, -500 }, [606] = { 500, -500 }, [607] = { -800, -900 }, [608] = { 800, -900 },
    [609] = { 0, -1500 }, [610] = { 0, 1500 }, [611] = { -300, 1200 }, [612] = { 300, 1200 },
    [613] = { -300, 1300 }, [614] = { 300, 1300 } }
local edges = {
    [501] = { 601, 602, 2000, 0, "track" }, [502] = { 603, 604, 2000, 0, "track" },
    [503] = { 605, 606, 1000, 0, 10 }, [504] = { 607, 608, 1600, 0, 11 }, [505] = { 609, 610, 0, 3000, 12 },
    [506] = { 611, 612, 600, 0, 13 }, [507] = { 613, 614, 600, 0, 14 } }
local streetTypes = { [10] = { { "urban" }, "standard/town_medium_new.lua" }, [11] = { { "country" }, "standard/country_medium_new.lua" },
    [12] = { { "highway" }, "standard/highway_new.lua" }, [13] = { { "one-way" }, "standard/country_small_one_way_new.lua" },
    [14] = { { "one-way" }, "standard/town_small_one_way_new.lua" } }
local owned = { [501] = SOLO and MINE or P2, [502] = MINE, [801] = SOLO and MINE or P3, [702] = MINE }
local cargoIcons = { COAL = "ui/hud/cargo_coal@2x.tga", STEEL = "ui/hud/cargo_steel@2x.tga",
                     ALCOHOL = "ui/hud/cargo_alcohol@2x.tga" }
if MODROOT then package.path = MODROOT .. "/res/scripts/?.lua;" .. package.path end
api = {
    util = { getAppConfig = function() return { uiAutoScaling = AUTO, uiScaling = CFGSCALE } end },
    type = {
        Vec2f = { new = function(x, y) return { x = x, y = y } end },
        Vec4f = { new = function(a, b, c, d) return { a, b, c, d } end },
        ComponentType = CT,
    },
    res = {
        streetTypeRep = {
            get = function(t) return streetTypes[t] and { categories = streetTypes[t][1] } end,
            getName = function(t) return streetTypes[t] and streetTypes[t][2] end,
        },
        constructionRep = {
            find = function(f) return ({ ["industry/coal_mine.con"] = 17, ["industry/steel_mill.con"] = 18 })[f] or -1 end,
            get = function(i) return ({ [17] = { description = { name = "Coal mine" } },
                                        [18] = { description = { name = "Steel mill" } } })[i] end,
        },
        cargoTypeRep = {
            find = function(n) return ({ COAL = 4, STEEL = 5, ALCOHOL = 6 })[n] or -1 end,
            get = function(i) return ({ [4] = { icon = cargoIcons.COAL }, [5] = { icon = cargoIcons.STEEL },
                                        [6] = { icon = cargoIcons.ALCOHOL } })[i] end,
        },
    },
    gui = {
        comp = {
            ImageView = { new = function(p) return comp("ImageView", p) end },
            ToggleButton = { new = function(i) return comp("ToggleButton", i) end },
            Button = { new = function(t, b) return comp("Button", t) end },
            TextView = { new = function(t) return comp("TextView", t) end },
            CheckBox = { new = function(t) return comp("CheckBox", t) end },
            Window = { new = function(t, c) return comp("Window", c) end },
            LineRenderView = { new = function() return comp("LineRenderView") end },
            Component = { new = function(s) return comp("Component", s) end },
        },
        layout = {
            BoxLayout = { new = function(d) return layout("BoxLayout") end },
            AbsoluteLayout = { new = function() return layout("AbsoluteLayout") end },
        },
        util = {
            Size = { new = function(w, h) return { w = w, h = h } end },
            Rect = { new = function() return {} end },
            getById = function(id) if id == "mainButtonsLayout" then return mainButtons end end,
            getGameUI = function()
                return { getMainRendererComponent = function()
                    return { getCameraController = function() return controller end }
                end }
            end,
        },
    },
    engine = {
        util = { getPlayer = function() return MINE end },
        terrain = { isValidCoordinate = function(v) return math.abs(v.x) <= EX and math.abs(v.y) <= EY end },
        forEachEntityWithComponent = function(fn, kind)
            local ids = ({ [CT.TOWN] = { 101, 102 }, [CT.SIM_BUILDING] = { 205, 201, 202, 203, 204, 206, 207 },
                [CT.BASE_EDGE] = { 501, 502, 503, 504, 505, 506, 507 }, [CT.STATION] = { 701, 702, 703 },
                [CT.PLAYER] = { MINE, P2, P3 } })[kind] or {}
            for i = 1, #ids do fn(ids[i]) end
        end,
        system = {
            streetConnectorSystem = {
                getConstructionEntityForSimBuilding = function(e)
                    return ({ [201] = 301, [202] = 302, [203] = 301, [204] = 303, [205] = 304, [206] = 305, [207] = 306 })[e]
                end,
                getConstructionEntityForStation = function(e) return ({ [701] = 801, [703] = 801 })[e] or -1 end,
            },
            streetSystem = { getEdgeObject2EdgeMap = function() return { [702] = 503 } end },
        },
        getComponent = function(id, kind)
            if kind == CT.CONSTRUCTION then
                local industries = { [301] = { "industry/coal_mine.con", -16384, 8192 },
                                     [303] = { "industry/steel_mill.con", 8192, -4096 },
                                     [304] = { "industry/coal_mine.con", 16384, 8192 },
                                     [305] = { "industry/mod/brewery_big.con", -8192, -8192 },
                                     [306] = { "industry/mod/mystery_works.con", 4096, 4096 } }
                if industries[id] then
                    local ind = industries[id]
                    return { fileName = ind[1], transf = { cols = function(self, i) return { x = ind[2], y = ind[3] } end } }
                end
                if id == 801 then
                    return { fileName = "station/rail/modular_station.con",
                             transf = { cols = function(self, i)
                                 if i == 3 then return { x = 500, y = 500 } end
                                 if i == 1 then return { x = 0, y = 1 } end
                                 return { x = 1, y = 0 }
                             end } }
                end
                return { fileName = "building/era_a/res_1.con", transf = { cols = function() return { x = 0, y = 0 } end } }
            end
            if kind == CT.NAME then
                return ({ [301] = { name = "Coal mine" }, [303] = { name = "Steel mill" }, [304] = { name = "Coal mine 2" },
                          [305] = { name = "Brewery" }, [306] = { name = "Mystery" } })[id]
            end
            if kind == CT.BASE_EDGE then
                local e = edges[id]
                return e and { node0 = e[1], node1 = e[2], tangent0 = { x = e[3], y = e[4], z = 0 }, tangent1 = { x = e[3], y = e[4], z = 0 } }
            end
            if kind == CT.BASE_NODE then
                local n = nodes[id]
                return n and { position = { x = n[1], y = n[2], z = 0 } }
            end
            if kind == CT.BASE_EDGE_TRACK then return edges[id] and edges[id][5] == "track" and { trackType = 1 } or nil end
            if kind == CT.BASE_EDGE_STREET then
                return edges[id] and edges[id][5] ~= "track" and { streetType = edges[id][5] } or nil
            end
            if kind == CT.PLAYER_OWNED then return owned[id] and { player = owned[id] } or nil end
            if kind == CT.BOUNDING_VOLUME then
                return id == 801 and { bbox = { min = { x = 400, y = 300 }, max = { x = 600, y = 700 } } } or nil
            end
        end,
    },
}
game = {
    interface = { getEntity = function(id)
        if id == 101 then return { position = { 0, 0, 5 }, name = "Centre" } end
        if id == 102 then return { position = { 32768, 16384, 5 }, name = "Corner" } end
        if id == 201 then return { itemsProduced = { COAL = 12, _sum = 12 }, itemsConsumed = {} } end
        -- listed before the productive coal mine and idle; a steel mill that has taken coal in
        -- but made no steel; two mod industries that have done nothing
        if id == 205 or id == 206 or id == 207 then return { itemsProduced = { _sum = 0, _lastYear = {} }, itemsConsumed = { _sum = 0 } } end
        if id == 204 then return { itemsProduced = { _sum = 0 }, itemsConsumed = { COAL = 3, _sum = 3 } } end
    end },
    gui = {
        getContentRect = function(name) return { 0, 0, SW, SH } end,
        getMousePos = function() return { MOUSE[1], MOUSE[2] } end,
        getCamera = function() return { 0, 0, 0, 0, 0 } end,
        setCamera = function(c) end,
    },
}
local realOs = os
os = { clock = realOs.clock, time = realOs.time, getenv = function(n)
    if n == "TPF2MP_DATADIR" then return CFGDIR end
    return nil
end }
local logs = {}
print = function(...)
    local t = {}
    for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end
    logs[#logs + 1] = table.concat(t, " ")
end

local R = { logs = logs, images = IMAGES }
local script = data()
assert(script.update == nil and script.save == nil and script.load == nil, "GUI only")

local function currentMap()
    local found
    for _, c in ipairs(created.Component or {}) do
        if c.layout and c.layout.kind == "AbsoluteLayout" and not c.destroyed then found = c end
    end
    return found
end
local function settle(n)
    for i = 1, n do
        script.guiUpdate()
        local mc = currentMap()
        if mc and mc.stepFn then mc.stepFn() end
    end
    script.guiUpdate()
end
local function inMap(kind, mc)
    local out = {}
    for _, c in ipairs(created[kind] or {}) do
        if c.parentLayout == mc.layout then out[#out + 1] = c end
    end
    return out
end
local function centreOf(c, cr)
    local r = c:getContentRect()
    return { r.x + r.w / 2 - cr.x, r.y + r.h / 2 - cr.y }
end
-- legend rows: a Component whose layout holds a CheckBox and ends with a TextView
local function legendRows()
    local rows = {}
    for _, c in ipairs(created.Component or {}) do
        local items = c.layout and c.layout.items
        if items and #items >= 2 and items[1].kind == "CheckBox" and items[#items].kind == "TextView" and not c.destroyed
            and c.parentLayout and c.parentLayout.owner and not c.parentLayout.owner.destroyed then
            local inBox = false
            for _, it in ipairs(c.parentLayout.items) do if it == c then inBox = true end end
            if inBox then rows[#rows + 1] = { label = items[#items].arg, box = items[1], icon = items[2].arg, kind2 = items[2].kind } end
        end
    end
    return rows
end

script.guiInit()
local button = toolbar.items[1]
assert(button and button.kind == "ToggleButton", "button inserted at the front of the toolbar")
R.buttonIcon = button.arg.arg
script.guiUpdate()
assert(#toolbar.items == 1, "button added once")
button.toggleFn(true)
settle(12)

local mc = currentMap()
local cr = mc:getContentRect()
local picture
for _, iv in ipairs(inMap("ImageView", mc)) do
    if type(iv.image) == "string" and iv.image:find("^bigmap:minimap") then picture = iv end
end
local ir = picture:getContentRect()
R.token = picture.image
R.cr = { cr.x, cr.y, cr.w, cr.h }
R.ir = { ir.x, ir.y, ir.w, ir.h }
R.windowVisible = created.Window[1].visible
R.markers = {}
R.markerIcons = {}
for _, iv in ipairs(inMap("ImageView", mc)) do
    if iv.tooltip then R.markers[iv.tooltip] = centreOf(iv, cr); R.markerIcons[iv.tooltip] = iv.arg end
end
local camera = inMap("LineRenderView", mc)[1]
local vr = camera:getContentRect()
R.cameraLines = #camera.lines
local l1 = camera.lines[1]
R.cameraStart = l1 and { vr.x + l1[1] - ir.x, vr.y + l1[2] - ir.y }
for _, tv in ipairs(created.TextView) do
    if tv.arg == "" and R.status == nil then R.status = tv.text end   -- the status line starts empty
end
R.legend = legendRows()

assert(picture.listeners[1]({ button = 1, type = 2 }) == false, "right click ignored")
MOUSE[1], MOUSE[2] = ir.x + 0.25 * ir.w, ir.y + 0.25 * ir.h
assert(picture.listeners[1]({ button = 0, type = 2, pos = { x = 0, y = 0 } }) == true, "map click accepted")
settle(1)
R.cameraSet = cam.set
MOUSE[1], MOUSE[2] = ir.x - 5, ir.y + 10
assert(camera.listeners[1]({ button = 0, type = 2 }) == false, "a click outside the picture is ignored")

for _, iv in ipairs(inMap("ImageView", mc)) do
    if iv.tooltip == "Centre" then iv.listeners[1]({ button = 0, type = 2 }) end
end
settle(1)
R.focused = cam.focused

-- picture layers and a company re-send only the header
local before = #IMAGES
for _, box in ipairs(created.CheckBox) do
    if box.arg == "Tracks" then box.toggleFn(false) end
end
settle(1)
R.afterTracks = IMAGES[#IMAGES]
R.tracksSends = #IMAGES - before
for _, row in ipairs(legendRows()) do
    if row.label:find("^Company 1") or row.label:find("^Player 950") or row.label:find("^Cid") then row.box.toggleFn(false) end
end
settle(1)
R.afterCompany = IMAGES[#IMAGES]

-- marker layers and industry types only change visibility
for _, box in ipairs(created.CheckBox) do
    if box.arg == "Towns" or box.arg == "Camera" then box.toggleFn(false) end
end
for _, row in ipairs(legendRows()) do
    if row.label:find("%(%d+%)$") then row.box.toggleFn(false) end   -- every industry type row
end
local sendsBefore = #IMAGES
settle(1)
R.markerToggleSends = #IMAGES - sendsBefore
local hidden = true
for _, iv in ipairs(inMap("ImageView", mc)) do
    if iv.tooltip and iv.visible then hidden = false end
end
R.layersHidden = hidden and camera.visible == false

created.Button[1].clickFn()
settle(12)
local mc2 = currentMap()
R.oldDestroyed = mc.destroyed == true and mc.stepFn == nil and mc2 ~= mc
for _, iv in ipairs(inMap("ImageView", mc2)) do
    if type(iv.image) == "string" and iv.image:find("^bigmap:minimap") then R.token2 = iv.image end
    if iv.tooltip and iv.visible then hidden = false end
end
R.newLayersHidden = hidden
R.legendAfterRefresh = legendRows()

button.toggleFn(false)
script.guiUpdate()
R.closed = created.Window[1].visible == false and button.selected == false
return R
'''

EXPECTED_EDGES = [
    'E 3 0 -1000 0 2000 0 1000 0 2000 0',
    'E 3 1 -1000 500 2000 0 1000 500 2000 0',
    'E 0 -1 -500 -500 1000 0 500 -500 1000 0',
    'E 1 -1 -800 -900 1600 0 800 -900 1600 0',
    'E 2 -1 0 -1500 0 3000 0 1500 0 3000',
    'E 1 -1 -300 1200 600 0 300 1200 600 0',
    'E 0 -1 -300 1300 600 0 300 1300 600 0',
]


def run_lua_variant(text, scale, centre, sw, sh, auto, cfg_scale, owners, cfg_dir, mod_root):
    L = LuaRuntime(unpack_returned_tuples=True)
    L.execute('SCALE, CENTRE, SW, SH, AUTO, CFGSCALE, OWNERS, CFGDIR, MODROOT = %r, %s, %d, %d, %s, %r, %r, %s, %r' % (
        scale, 'true' if centre else 'false', sw, sh, 'true' if auto else 'false', cfg_scale, owners,
        repr(str(cfg_dir).replace('\\', '/')) if cfg_dir else 'nil', str(mod_root).replace('\\', '/')))
    L.execute(text)
    return L.execute(LUA_HARNESS)


def header_of(token):
    return token.split('\n', 1)[0]


def check_lua_variant(text, scale, centre, sw, sh, auto, cfg_scale, guess, owners, cfg_dir, mod_root):
    R = run_lua_variant(text, scale, centre, sw, sh, auto, cfg_scale, owners, cfg_dir, mod_root)
    logs = list(R.logs.values())
    vw = max(300, min(1400, int(min(sw, sh) * 0.55)))
    vh = int(vw * 0.5 + 0.5)
    tol = 2.0

    rect, size, relief = (C.c_double * 4)(), (C.c_int * 2)(), C.c_double()
    assert parse(R.token.encode(), rect, size, C.byref(relief)), R.token
    assert list(rect) == [-32768, -16384, 32768, 16384] and list(size) == [1024, 512] and relief.value == 4, R.token
    assert R.buttonIcon == 'ui/button/medium/terrain@2x.tga', R.buttonIcon
    assert R.windowVisible
    cr, ir = list(R.cr.values()), list(R.ir.values())
    assert abs(ir[0] - cr[0]) <= tol and abs(ir[1] - cr[1]) <= tol, (cr, ir, logs)
    assert abs(ir[2] - vw) <= tol and abs(ir[3] - vh) <= tol, (ir, vw, vh)

    def near(p, q):
        return abs(p[0] - q[0]) <= tol and abs(p[1] - q[1]) <= tol

    def view(x, y):
        return ((x + 32768) / 65536 * vw, (16384 - y) / 32768 * vh)

    markers = {k: list(v.values()) for k, v in R.markers.items()}
    assert near(markers['Centre'], view(0, 0)), (markers, view(0, 0))
    assert near(markers['Corner'], view(32768, 16384)), (markers, view(32768, 16384))
    assert near(markers['Coal mine'], view(-16384, 8192)), (markers, view(-16384, 8192))
    assert near(markers['Steel mill'], view(8192, -4096)), (markers, view(8192, -4096))
    assert len(markers) == 7, markers
    icons = dict(R.markerIcons)
    assert icons['Coal mine'] == 'ui/hud/cargo_coal@2x.tga', icons      # the cargo it produces
    assert icons['Coal mine 2'] == 'ui/hud/cargo_coal@2x.tga', icons    # idle, and placed before the productive one
    assert icons['Steel mill'] == 'ui/hud/cargo_steel@2x.tga', icons    # the stock output, not the coal it took in
    assert icons['Brewery'] == 'ui/hud/cargo_alcohol@2x.tga', icons     # the output rule in the mod's own file
    assert icons['Mystery'] == 'ui/icons/main-menu/map_industry@2x.tga', icons   # nothing known: a generic icon that exists
    assert icons['Centre'] == icons['Corner'] == 'ui/icons/main-menu/map_town@2x.tga', icons

    assert R.cameraLines == 32
    cx, cy = view(1000, -2000)
    radius = max(4, min(vw / 4, 500 * vw / 65536))
    assert near(list(R.cameraStart.values()), (cx + radius, cy))
    moved = list(R.cameraSet.values())
    assert abs(moved[0] + 16384) <= 1 and abs(moved[1] - 8192) <= 1, moved
    assert R.focused == 101

    # ---- network payload
    images = list(R.images.values())
    with_payload = [t for t in images if '\n' in t]
    assert len(with_payload) == 2, len(with_payload)           # one gather per build: open, then Refresh
    ids = [header_of(t).split('&net=')[1].split('&')[0] for t in with_payload]
    assert ids[0] != ids[1], ids                               # each build caches its own network
    assert with_payload[0].split('\n', 1)[1] == with_payload[1].split('\n', 1)[1]   # same world, same lines
    token = with_payload[0]
    head, payload = token.split('\n', 1)
    layers, net, hide = C.c_int(), C.c_uint64(), C.c_uint64()
    assert parse_extra(head.encode(), C.byref(layers), C.byref(net), C.byref(hide))
    assert layers.value == 7 and net.value > 0 and hide.value == 0, head
    lines = payload.split('\n')
    edges = [line for line in lines if line.startswith('E ')]
    stations = [line for line in lines if line.startswith('S ')]
    palette = [line for line in lines if line.startswith('P ')]
    if owners == 'solo':
        expected_edges = [e.replace('E 3 0 ', 'E 3 0 ', 1) for e in EXPECTED_EDGES]
        expected_edges[1] = 'E 3 0 -1000 500 2000 0 1000 500 2000 0'   # one owner: palette index 0
        expected_stations = ['S 0 0 500 500 0 1000 200', 'S 1 0 0 -500 1000 0 15']
    else:
        expected_edges = EXPECTED_EDGES
        expected_stations = ['S 0 2 500 500 0 1000 200', 'S 1 1 0 -500 1000 0 15']
    assert edges == expected_edges, edges
    assert stations == expected_stations, stations
    if owners == 'companies':      # the mod's map: 950 -> company 3, 900 -> 2 (me), 970 -> 1, with names
        assert palette == ['P 0 90 190 110', 'P 1 80 140 230', 'P 2 220 80 80'], palette
        companies = ["Cid's 2nd company", "Bob's company (you)", "Ada's company"]
    elif owners == 'guess':        # no map file: creation order, roster 1,2,3, this machine company 2
        assert palette == ['P 0 220 80 80', 'P 1 80 140 230', 'P 2 90 190 110'], palette
        companies = ['Company 1', 'Company 2 (you)', 'Company 3']
    elif owners == 'free':         # no roster: this player first, then ascending ids
        assert palette == ['P 0 80 140 230', 'P 1 220 80 80', 'P 2 90 190 110'], palette
        companies = ['Player 950', 'Your company', 'Player 970']
    else:                          # one owner: classic track colour, no palette, no company legend
        assert palette == [], palette
        companies = []
    counts = (C.c_uint64 * 3)()
    assert accept_network(token.encode(), len(token.encode()), counts)
    assert list(counts) == [7, 2, 0], list(counts)

    # Rows are collected in creation order (the industry row exists before the
    # network finishes); on screen each sits in its own legend section.
    legend = [row['label'] for row in R.legend.values()]
    industry_rows = ['Brewery big (1)', 'Coal mine (2)', 'Mystery works (1)', 'Steel mill (1)']
    assert sorted(legend) == sorted(companies + industry_rows), legend
    row_icons = {row['label']: row['icon'] for row in R.legend.values()}
    assert row_icons['Coal mine (2)'] == 'ui/hud/cargo_coal@2x.tga', row_icons
    assert row_icons['Steel mill (1)'] == 'ui/hud/cargo_steel@2x.tga', row_icons
    assert row_icons['Brewery big (1)'] == 'ui/hud/cargo_alcohol@2x.tga', row_icons
    assert row_icons['Mystery works (1)'] == 'ui/icons/main-menu/map_industry@2x.tga', row_icons
    icon_lines = [line for line in logs if 'industry icons:' in line]
    assert len(icon_lines) >= 2, logs
    assert ('1 from production, 1 from mod files, 1 from the stock list, 0 from inputs, 1 generic '
            '(industry/mod/mystery_works.con)' in icon_lines[0]), icon_lines
    assert R.status == '', R.status

    # Picture layers and companies re-send only the header, against the same network.
    after = R.afterTracks
    assert R.tracksSends == 1 and '\n' not in after, after
    assert parse_extra(after.encode(), C.byref(layers), C.byref(net), C.byref(hide))
    assert layers.value == 5 and hide.value == 0, after
    first_net = net.value
    if companies:
        after = R.afterCompany
        assert '\n' not in after and parse_extra(after.encode(), C.byref(layers), C.byref(net), C.byref(hide))
        assert hide.value == 1 and net.value == first_net, after      # palette index 0 hidden
    assert R.markerToggleSends == 0 and R.layersHidden
    assert R.oldDestroyed and R.newLayersHidden and R.closed
    assert R.token2 and R.token2 != R.token
    refreshed = [row['label'] for row in R.legendAfterRefresh.values()]
    assert sorted(refreshed) == sorted(companies + industry_rows), refreshed      # rebuilt, not duplicated

    layout_lines = [line for line in logs if 'layout: UI scale' in line]
    assert layout_lines and ('UI scale %.3f' % scale) in layout_lines[-1] and ('guessed %.2f' % guess) in layout_lines[-1], logs
    rule = 'vertical centre' if centre else 'top edge'
    assert rule in layout_lines[-1], logs
    rebuilt = any('rebuilding' in line for line in logs)
    assert rebuilt == (not centre), logs
    net_lines = [line for line in logs if 'network ' in line and 'road edges' in line]
    assert net_lines and '5 road edges, 2 track edges, 2 stations' in net_lines[0], logs
    assert any('map click: picture' in line and 'from mouse' in line for line in logs), logs
    assert not any('error' in line or 'map step:' in line or 'network:' in line for line in logs), logs
    return '%s: %s' % (owners, 'rebuilt once for the %s rule' % rule if rebuilt else 'placed right first time')


def check_lua():
    src = SCRIPT.read_bytes()
    assert all(b == 10 or b == 13 or b == 9 or 32 <= b <= 126 for b in src), 'script must be printable ASCII'
    text = src.decode()
    code = '\n'.join(line.split('--')[0] for line in text.splitlines())
    for banned in ('table.maxn', 'loadstring', 'setfenv', 'getfenv', 'table.getn', 'math.mod', 'string.gfind', 'goto '):
        assert banned not in code, banned
    L = LuaRuntime(unpack_returned_tuples=True)
    assert L.eval('_VERSION') == 'Lua 5.2'
    assert L.execute('return load(...)', text) is not None, 'Lua 5.2 parse failed'

    with tempfile.TemporaryDirectory() as td:
        cfg_dir = Path(td) / 'companies'
        cfg_dir.mkdir()
        (cfg_dir / 'mp_company_cfg.txt').write_text('companies\n2\n1,2,3\na=1,b=2,c=3\n')
        (cfg_dir / 'mp_company_map.txt').write_text('me=2\n1=970=Ada%27s%20company\n2=900=Bob%27s%20company\n3=950=Cid%27s%202nd%20company\n')
        # a map without the mod's file: the creation-order guess (the old behaviour)
        guess_dir = Path(td) / 'guess'
        guess_dir.mkdir()
        (guess_dir / 'mp_company_cfg.txt').write_text('companies\n2\n1,2,3\na=1,b=2,c=3\n')
        empty_dir = Path(td) / 'empty'
        empty_dir.mkdir()
        # A mod industry's construction file, spelled the way industryutil-based mods spell it:
        # a params.output that is not a table comes first and must be skipped.
        mod_root = Path(td) / 'workshop' / '123456'
        con = mod_root / 'res' / 'construction' / 'industry' / 'mod' / 'brewery_big.con'
        con.parent.mkdir(parents=True)
        con.write_text('local stockListConfig = {\n'
                       '    stocks = { "GRAIN" },\n'
                       '    rule = { input = { { 2 } }, output = { ALCOHOL = 1 }, capacity = 80 },\n'
                       '}\n'
                       'function data()\n'
                       '    return { updateFn = function(params)\n'
                       '        return { rule = { output = params.output or stockListConfig.rule.output } }\n'
                       '    end }\n'
                       'end\n')
        # As measured in game (175%, centre) in multiplayer companies mode.
        a = check_lua_variant(text, 1.75, True, 3038, 1918, True, 1.0, 1.75, 'companies', cfg_dir, mod_root)
        # The other layout rule with a wrong scale guess, three owners without a roster.
        b = check_lua_variant(text, 1.0, False, 1920, 1080, False, 1.25, 1.25, 'free', empty_dir, mod_root)
        # A lone player's network.
        c = check_lua_variant(text, 1.75, True, 3038, 1918, True, 1.0, 1.75, 'solo', None, mod_root)
        # Companies mode without the mod's map file: the creation-order guess still works.
        d = check_lua_variant(text, 1.75, True, 3038, 1918, True, 1.0, 1.75, 'guess', guess_dir, mod_root)

    checker = ROOT.parent / 'tpf2-multiplayer/tools/luacheck.py'
    if checker.exists():
        r = subprocess.run(['python', str(checker), str(SCRIPT)], capture_output=True, text=True)
        assert r.returncode == 0 and 'ok ' in r.stdout, r.stdout + r.stderr
        lint = 'tpf2-multiplayer luacheck clean'
    else:
        lint = 'tpf2-multiplayer/tools/luacheck.py not found, lint skipped'
    print('PASS: Lua script under a mocked UI and world (%s; %s; %s; %s): measured UI scale and layout rule, picture and '
          'markers on their world positions, one cargo icon per industry type (from production, a mod\'s own construction file, the stock table, else a generic icon that exists), click-to-move and focus; the network '
          'payload lists every track and street class with Hermite tangents, both stations and owner palette '
          'indices; companies take their colours and names from the multiplayer mod\'s map (creation order without it), other owners distinct colours, a lone player '
          'none; the plugin accepts the payload; layer and company toggles re-send only the header against the '
          'same network, marker toggles send nothing; the legend is rebuilt, not duplicated, on Refresh; %s'
          % (a, b, c, d, lint))


def main():
    check_parser()
    t = check_renderer()
    check_accessor(t)
    check_detour(t)
    check_network(t)
    check_thunk()
    with tempfile.TemporaryDirectory() as td:
        check_script_sync(Path(td))
        check_installer(Path(td))
    check_lua()


if __name__ == '__main__':
    main()
