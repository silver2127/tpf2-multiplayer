"""Offline test of the terrain sidecar's save/load glue (src/terrain_sidecar_io.h and
the helpers it added to src/terrain_sidecar.h), through the plugin DLL's test exports.
No game: a synthetic CTerrain grid in the engine's layout, real files in a temp folder
whose name is NOT ASCII (the engine hands UTF-8 paths over; the narrow CRT would miss them).

  1. a sidecar written with fingerprint 0 (as SaveGame's entry does) is refused by a
     load; stamped with the save's hash (Refingerprint) it restores every tile exactly
  2. a load finds its sidecar by fingerprint when the name does not match (multiplayer
     loads a COPY of the save as mp_shared.sav), and never takes a foreign one
  3. platform::SaveGameId -> "<dir>\\<name>.sav" as UTF-8, for SSO and heap strings;
     an id without a folder is refused offline (no engine backend to ask)
  4. sidecars of deleted saves are swept, a live save's sidecar stays

    build.bat && python tools\\test_sidecar_io.py
"""
import ctypes as C
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, "out", "tpf2_bigmap.dll")
SAMPLES = 257 * 257
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


class Terrain:
    """CTerrain+0x18 -> grid {x0,y0,nx,ny,records*}; record 40 B {entity, control*@8, version@0x20};
    record+8 = the vector object {first,last,end} (make_shared: block+0x10), record+0x10 = its
    control block (block+0)."""

    def __init__(self, nx, ny, full, seed):
        self.n = nx * ny
        self.caches = [(C.c_uint16 * SAMPLES)() for _ in range(self.n)]
        self.controls = [(C.c_uint8 * 0x40)() for _ in range(self.n)]
        self.records = (C.c_uint8 * (40 * self.n))()
        self.grid = (C.c_uint8 * 0x18)()
        self.cterrain = (C.c_uint8 * 0x40)()
        C.memmove(C.addressof(self.grid), (C.c_int32 * 4)(0, 0, nx, ny), 16)
        C.cast(C.addressof(self.grid) + 0x10, C.POINTER(C.c_void_p))[0] = C.addressof(self.records)
        C.cast(C.addressof(self.cterrain) + 0x18, C.POINTER(C.c_void_p))[0] = C.addressof(self.grid)
        for i in range(self.n):
            if i not in full:
                continue
            h = 20000 + (seed * 7919 + i * 104729) % 500
            c = self.caches[i]
            for k in range(SAMPLES):                       # terrain-like: a slow walk, compressible
                h = (h + ((k * 2654435761 + i * 40503 + seed) >> 7) % 7 - 3) & 0xFFFF
                c[k] = h
            rec = C.addressof(self.records) + 40 * i
            C.cast(rec, C.POINTER(C.c_int32))[0] = i
            C.cast(rec + 8, C.POINTER(C.c_void_p))[0] = C.addressof(self.controls[i]) + 0x10
            C.cast(rec + 0x10, C.POINTER(C.c_void_p))[0] = C.addressof(self.controls[i])
            C.cast(rec + 0x20, C.POINTER(C.c_int32))[0] = 1
            v = C.cast(C.addressof(self.controls[i]) + 0x10, C.POINTER(C.c_void_p))
            v[0] = C.addressof(c)
            v[1] = v[2] = C.addressof(c) + 2 * SAMPLES

    def zero(self):
        for c in self.caches:
            C.memset(C.addressof(c), 0, 2 * SAMPLES)

    def same(self, other, full):
        return all(bytes(self.caches[i]) == bytes(other.caches[i]) for i in full)


def gstr(text):
    """MSVC std::string (0x20 bytes) and the buffer that keeps a heap string alive."""
    raw = text.encode("utf-8")
    s = (C.c_uint8 * 0x20)()
    keep = None
    if len(raw) > 15:
        keep = C.create_string_buffer(raw, len(raw) + 1)
        C.cast(C.addressof(s), C.POINTER(C.c_void_p))[0] = C.addressof(keep)
        cap = len(raw)
    else:
        C.memmove(C.addressof(s), raw + b"\0", len(raw) + 1)
        cap = 15
    C.cast(C.addressof(s) + 0x10, C.POINTER(C.c_size_t))[0] = len(raw)
    C.cast(C.addressof(s) + 0x18, C.POINTER(C.c_size_t))[0] = cap
    return s, keep


def gwstr(text):
    raw = text.encode("utf-16-le")
    units = len(raw) // 2
    s = (C.c_uint8 * 0x20)()
    keep = None
    if units > 7:
        keep = C.create_string_buffer(raw + b"\0\0", len(raw) + 2)
        C.cast(C.addressof(s), C.POINTER(C.c_void_p))[0] = C.addressof(keep)
        cap = units
    else:
        C.memmove(C.addressof(s), raw + b"\0\0", len(raw) + 2)
        cap = 7
    C.cast(C.addressof(s) + 0x10, C.POINTER(C.c_size_t))[0] = units
    C.cast(C.addressof(s) + 0x18, C.POINTER(C.c_size_t))[0] = cap
    return s, keep


def save_id(path, name, ns="savegame"):
    parts = [gwstr(path), gstr(name), gstr(ns)]
    blob = (C.c_uint8 * 0x60)()
    for k, (s, _) in enumerate(parts):
        C.memmove(C.addressof(blob) + 0x20 * k, C.addressof(s), 0x20)
    return blob, parts


def main():
    if not os.path.exists(DLL):
        print("build the plugin first:", DLL)
        return 2
    dll = C.CDLL(DLL)
    dll.BigmapTestSidecarWrite.argtypes = [C.c_void_p, C.c_uint64, C.c_char_p, C.POINTER(C.c_uint64)]
    dll.BigmapTestSidecarWrite.restype = C.c_long
    dll.BigmapTestSidecarApply.argtypes = [C.c_void_p, C.c_uint64, C.c_char_p]
    dll.BigmapTestSidecarApply.restype = C.c_long
    dll.BigmapTestSidecarRefingerprint.argtypes = [C.c_char_p, C.c_uint64]
    dll.BigmapTestSidecarFind.argtypes = [C.c_uint64, C.c_char_p, C.c_int]
    dll.BigmapTestSidecarResolve.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    dll.BigmapTestSidecarSweep.argtypes = [C.c_char_p]

    base = tempfile.mkdtemp(prefix="sidecar_io_")
    folder = os.path.join(base, "Spielst\u00e4nde \u0416")          # not ASCII, not ANSI-1252 either
    os.makedirs(folder)
    u8 = lambda p: p.encode("utf-8")
    try:
        full = [0, 1, 3, 4, 5]
        src = Terrain(3, 2, full, seed=11)
        FP = 0x1122334455667788

        print("== 1. written with fingerprint 0, stamped afterwards")
        tmp = os.path.join(folder, "world.terr.tmp")
        n = dll.BigmapTestSidecarWrite(C.addressof(src.cterrain), 0, u8(tmp), None)
        check("five full tiles captured into a UTF-8 path", n == 5 and os.path.exists(tmp), f"tiles={n}")
        dst = Terrain(3, 2, full, seed=11); dst.zero()
        check("an unstamped sidecar is refused for the save's fingerprint", dll.BigmapTestSidecarApply(C.addressof(dst.cterrain), FP, u8(tmp)) == 0)
        check("Refingerprint stamps it", dll.BigmapTestSidecarRefingerprint(u8(tmp), FP) == 1)
        terr = os.path.join(folder, "world.terr")
        os.replace(tmp, terr)
        got = dll.BigmapTestSidecarApply(C.addressof(dst.cterrain), FP, u8(terr))
        check("stamped: every tile restored exactly", got == 5 and src.same(dst, full), f"applied={got}")
        dst.zero()
        check("another save's fingerprint is refused", dll.BigmapTestSidecarApply(C.addressof(dst.cterrain), FP + 1, u8(terr)) == 0)
        junk = os.path.join(folder, "junk.terr")
        open(junk, "wb").write(b"not a sidecar at all" * 4)
        check("a file that is not a sidecar is not stamped", dll.BigmapTestSidecarRefingerprint(u8(junk), FP) == 0)

        print("== 2. found by fingerprint under another name")
        buf = C.create_string_buffer(u8(os.path.join(folder, "mp_shared.terr")), 1040)
        check("mp_shared.terr is absent: world.terr is taken by its fingerprint",
              dll.BigmapTestSidecarFind(FP, buf, 1040) == 1 and buf.value.decode("utf-8").lower().endswith("world.terr"), buf.value.decode("utf-8"))
        buf = C.create_string_buffer(u8(os.path.join(folder, "mp_shared.terr")), 1040)
        check("no sidecar carries a foreign fingerprint: nothing is taken",
              dll.BigmapTestSidecarFind(FP + 5, buf, 1040) == 0 and buf.value.decode("utf-8").endswith("mp_shared.terr"))
        buf = C.create_string_buffer(u8(terr), 1040)
        check("the named file matches: it is kept", dll.BigmapTestSidecarFind(FP, buf, 1040) == 1 and buf.value.decode("utf-8") == terr)

        print("== 3. SaveGameId -> path")
        out = C.create_string_buffer(1040)
        sid, keep = save_id(folder, "my save")
        ok = dll.BigmapTestSidecarResolve(C.addressof(sid), out, 1040)
        check("heap wstring folder + SSO name", ok == 1 and out.value.decode("utf-8") == folder + "\\my save.sav", out.value.decode("utf-8", "replace"))
        sid, keep = save_id("C:\\s", "a save with a rather long name \u00fc")
        ok = dll.BigmapTestSidecarResolve(C.addressof(sid), out, 1040)
        check("SSO wstring folder + heap name", ok == 1 and out.value.decode("utf-8") == "C:\\s\\a save with a rather long name \u00fc.sav", out.value.decode("utf-8", "replace"))
        sid, keep = save_id("C:\\s\\", "x")
        ok = dll.BigmapTestSidecarResolve(C.addressof(sid), out, 1040)
        check("a folder that ends in a separator gets no second one", ok == 1 and out.value.decode("utf-8") == "C:\\s\\x.sav", out.value.decode("utf-8"))
        sid, keep = save_id("", "x")
        check("no folder and no engine backend offline: refused, not guessed", dll.BigmapTestSidecarResolve(C.addressof(sid), out, 1040) == 0)
        sid, keep = save_id("C:\\s", "")
        check("an id without a name is refused", dll.BigmapTestSidecarResolve(C.addressof(sid), out, 1040) == 0)
        check("a null id is refused", dll.BigmapTestSidecarResolve(None, out, 1040) == 0)

        print("== 4. sidecars of deleted saves are swept")
        open(os.path.join(folder, "world.sav"), "wb").write(b"sav")
        for name in ("autosave_3.terr", "autosave_4.terr.tmp"):
            shutil.copy(terr, os.path.join(folder, name))
        os.remove(junk)
        removed = dll.BigmapTestSidecarSweep(u8(os.path.join(folder, "world.sav")))
        left = sorted(os.listdir(folder))
        check("the two orphans are deleted, the live save's sidecar stays", removed == 2 and left == ["world.sav", "world.terr"], f"removed={removed} left={left}")

        print("== 5. an autosave: the engine wrote another name than the id's")
        dll.BigmapTestSidecarWrittenDuring.argtypes = [C.c_char_p, C.c_int, C.c_char_p, C.c_int]
        old = os.path.join(folder, "world.sav")
        past = os.path.getmtime(old) - 3600
        os.utime(old, (past, past))                                   # an old save, not this call's
        auto = os.path.join(folder, "autosave_New Game_1850-01-04.sav")
        open(auto, "wb").write(b"autosave")
        open(auto + ".lua", "wb").write(b"meta")                     # *.sav.lua is not a save
        expected = os.path.join(folder, "autosave.sav")                # what the id resolves to: absent
        out = C.create_string_buffer(1040)
        got = dll.BigmapTestSidecarWrittenDuring(u8(expected), 30, out, 1040)
        check("the one .sav written during the save is found", got == 1 and out.value.decode("utf-8") == auto, out.value.decode("utf-8"))
        check("a save call that started after every write finds nothing", dll.BigmapTestSidecarWrittenDuring(u8(expected), -60, out, 1040) == 0)
        other = os.path.join(folder, "mp_shared.sav")
        open(other, "wb").write(b"copied during the save")
        check("two .sav written during the save: none is taken (never another save's hash)",
              dll.BigmapTestSidecarWrittenDuring(u8(expected), 30, out, 1040) == 0)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        for f in fails:
            print("  - " + f)
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
