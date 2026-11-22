"""Copy-on-write section sharing between the two CTerrain versions.

Covers the hook wiring: that the terrain_cow_share gate routes CopyTerrainOwned
through TerrainPager::Share, that sharing adds no resident backing, and that the
first write through a sharer privatizes it without disturbing the source.
Share() itself (isolation both ways, ring release order, eviction refusal and
the concurrency race) is covered by tools/test_terrain_pager.cpp.

No game files or running processes are touched. See docs/terrain-cow-sharing.md.
"""
import ctypes as C
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAMPLES = 66049
FIELDS = ['live', 'resident', 'shared', 'share_slots', 'privatized', 'failures']


class Vec(C.Structure):
    _fields_ = [('first', C.c_void_p), ('last', C.c_void_p), ('end', C.c_void_p)]


Resize = C.CFUNCTYPE(None, C.POINTER(Vec), C.c_size_t)
Copy = C.CFUNCTYPE(C.c_void_p, C.POINTER(Vec), C.POINTER(Vec))


def main():
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    assert dll.BigmapTestCompressionInit()
    resize = dll.BigmapTestCompressionResize
    resize.argtypes = [C.POINTER(Vec), C.c_size_t, C.c_int, Resize]
    copy = dll.BigmapTestCompressionCopy
    copy.argtypes = [C.POINTER(Vec), C.POINTER(Vec), C.c_int, Copy]
    copy.restype = C.c_void_p
    stats = dll.BigmapTestCompressionStats
    stats.argtypes = [C.POINTER(C.c_uint64 * 6)]
    calls = []

    @Resize
    def stock_resize(v, n):
        calls.append('resize')

    @Copy
    def stock_copy(dst, src):
        calls.append('copy')
        return C.cast(dst, C.c_void_p).value

    def snap():
        out = (C.c_uint64 * 6)()
        stats(C.byref(out))
        return dict(zip(FIELDS, out))

    src = Vec()
    resize(C.byref(src), SAMPLES, 1, stock_resize)
    assert not calls and src.first, calls
    C.memset(src.first, 0x5a, SAMPLES * 2)

    # Sharing off: the stock eager copy path, byte-identical and unshared.
    dll.BigmapTestSetCowShare(0)
    eager = Vec()
    copy(C.byref(eager), C.byref(src), 1, stock_copy)
    off = snap()
    assert off['shared'] == 0, off
    assert C.string_at(eager.first, SAMPLES * 2) == C.string_at(src.first, SAMPLES * 2)

    # Sharing on: a second view of the SAME section, so no new resident backing.
    dll.BigmapTestSetCowShare(1)
    shared = Vec()
    copy(C.byref(shared), C.byref(src), 1, stock_copy)
    on = snap()
    assert on['shared'] == 1 and on['share_slots'] == 2, on
    assert on['resident'] == off['resident'], ('sharing must add no backing', off, on)
    assert on['live'] == off['live'] + 1, (off, on)
    assert C.string_at(shared.first, SAMPLES * 2) == C.string_at(src.first, SAMPLES * 2)

    # First write through the sharer privatizes that slot alone.
    C.memset(shared.first, 0x33, 16)
    wrote = snap()
    assert wrote['privatized'] == 1 and wrote['share_slots'] == 0, wrote
    assert wrote['resident'] == on['resident'] + 1, ('privatization adds a section', on, wrote)
    assert C.string_at(src.first, SAMPLES * 2) == b'\x5a' * (SAMPLES * 2)
    assert C.string_at(shared.first, 16) == b'\x33' * 16
    assert C.string_at(shared.first, SAMPLES * 2)[16:] == b'\x5a' * (SAMPLES * 2 - 16)
    assert not calls, calls
    assert wrote['failures'] == 0, wrote
    print('PASS: cow share routing on/off through CopyTerrainOwned, no resident growth on '
          'share, one added section on privatize, source isolation, no stock fallback')


if __name__ == '__main__':
    main()
