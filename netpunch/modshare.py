"""
modshare.py -- share the mods a save needs with the players who lack them.

A shared save carries its own mod list and every mod's per-save settings (the
values from the Mods panel live inside the .sav, right after the list), so the
only thing a joiner can be missing is the mod folder itself. The host reads
the list off the save it is about to share, tells each joiner in the transfer's
``fbegin``, each joiner answers with the ids it does not have installed, and
after the save has verified the host sends those mod folders as zip files in a
second transfer round. The joiner unpacks each into its game's mods folder and
the game's load screen lists them (the engine re-reads mod descriptions when
that screen opens: "Loaded 1 of 39 mod descriptions from disk" mid-session).

The multiplayer mod itself is never shared (the installer ships it) and a mod
that is already present is never overwritten.

Save header format (measured on a 0.4.x autosave, 2026-09-11): the .sav is one
Zstandard frame; a few hundred KB into the decompressed stream the active mod
list is ``u32 count`` then ``count x (u32 len, name, u32 version)``, followed
immediately by the game settings pairs (``u32 n``, then ``u32 len "climate"``
...). The settings block is the anchor: the list is parsed backwards from it.

Mod ids: a folder ``<id>_<version>`` under the game's ``mods`` or the profile's
``local/mods``; a Steam Workshop item is ``*<workshopid>`` and lives under
``steamapps/workshop/content/1066780/<workshopid>`` (assumed from the game's
naming; no local save with a workshop mod was available to measure).
"""
from __future__ import annotations
import io
import os
import re
import struct
import sys
import zipfile

TF2_APPID = "1066780"
MP_MOD_ID = "mp_lockstep"                 # ours: shipped by the installer, never sent
MAX_MOD_ZIP = 512 * 1024 * 1024           # refuse to zip a single mod above this
HEAD_BYTES = 6 * 1024 * 1024              # how much of the decompressed save to look at
INCOMING_MOD_PREFIX = "incoming_mod_"
_MOD_ZIP_RE = re.compile(r"^incoming_mod_([A-Za-z0-9_*.-]{1,120})\.zip$")
_ID_RE = re.compile(r"^[A-Za-z0-9_*.-]{1,120}$")


# ---------------------------------------------------------------------------
# where things are
# ---------------------------------------------------------------------------
def steam_root():
    """Steam's install folder, from the registry (the same keys the menu DLL
    reads), else the default. None only if nothing looks like Steam."""
    cands = []
    if sys.platform == "win32":
        try:
            import winreg
            for hive, key, val in (
                (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
            ):
                try:
                    with winreg.OpenKey(hive, key) as k:
                        v, _ = winreg.QueryValueEx(k, val)
                        if v:
                            cands.append(str(v).replace("/", "\\"))
                except OSError:
                    pass
        except ImportError:
            pass
    cands.append(r"C:\Program Files (x86)\Steam")
    for c in cands:
        if os.path.isdir(os.path.join(c, "steamapps")):
            return c
    return None


def game_dir():
    """The Transport Fever 2 folder: beside the frozen lobby when it runs from
    <gamedir>\\netpunch (the installed layout), else via Steam."""
    here = os.path.dirname(os.path.abspath(getattr(sys, "frozen", False) and sys.executable or __file__))
    parent = os.path.dirname(here)
    if os.path.isfile(os.path.join(parent, "TransportFever2.exe")):
        return parent
    root = steam_root()
    if root:
        g = os.path.join(root, "steamapps", "common", "Transport Fever 2")
        if os.path.isfile(os.path.join(g, "TransportFever2.exe")):
            return g
    return None


def userdata_mods_dir():
    """<steam>\\userdata\\<account>\\1066780\\local\\mods -- the account that has a
    save folder (newest wins), like the menu DLL's resolveSaveDir."""
    root = steam_root()
    if not root:
        return None
    ud = os.path.join(root, "userdata")
    best, best_t = None, -1
    try:
        for acc in os.listdir(ud):
            local = os.path.join(ud, acc, TF2_APPID, "local")
            if os.path.isdir(os.path.join(local, "save")):
                t = os.path.getmtime(os.path.join(ud, acc))
                if t > best_t:
                    best, best_t = os.path.join(local, "mods"), t
    except OSError:
        pass
    return best


def workshop_dir():
    root = steam_root()
    return os.path.join(root, "steamapps", "workshop", "content", TF2_APPID) if root else None


def mod_folder_name(mod_id, version):
    return f"{mod_id}_{int(version)}"


def find_mod(mod_id, version):
    """The installed folder for a mod id, or None."""
    if not isinstance(mod_id, str) or not _ID_RE.match(mod_id):
        return None
    if mod_id.startswith("*"):
        w = workshop_dir()
        p = w and os.path.join(w, mod_id[1:])
        return p if p and os.path.isfile(os.path.join(p, "mod.lua")) else None
    name = mod_folder_name(mod_id, version)
    for base in (game_dir() and os.path.join(game_dir(), "mods"), userdata_mods_dir()):
        if base:
            p = os.path.join(base, name)
            if os.path.isfile(os.path.join(p, "mod.lua")):
                return p
    return None


# The joiner asks this (the host asks find_mod): one process can then play both
# ends in a test with different answers. In the game they are the same lookup.
installed_mod = find_mod


def install_target(mod_id, version):
    """Where a received mod goes: the game's mods folder (what the installer
    uses), the profile's local/mods if that is not writable, the workshop
    content folder for a workshop item."""
    if mod_id.startswith("*"):
        w = workshop_dir()
        return w and os.path.join(w, mod_id[1:])
    name = mod_folder_name(mod_id, version)
    g = game_dir()
    if g and os.access(os.path.join(g, "mods"), os.W_OK):
        return os.path.join(g, "mods", name)
    u = userdata_mods_dir()
    return u and os.path.join(u, name)


# ---------------------------------------------------------------------------
# the save's mod list
# ---------------------------------------------------------------------------
def _decompress_head(path, want=HEAD_BYTES):
    try:
        import zstandard
    except ImportError:
        return None
    out = bytearray()
    dec = zstandard.ZstdDecompressor().decompressobj()
    with open(path, "rb") as f:
        while len(out) < want:
            chunk = f.read(256 * 1024)
            if not chunk:
                break
            try:
                out += dec.decompress(chunk)
            except zstandard.ZstdError:
                break
    return bytes(out)


def parse_mod_list(head):
    """The active mod list out of a decompressed save head: [(id, version)].
    None when the layout is not recognised (never guess a list)."""
    if not head:
        return None
    anchor = head.find(b"\x07\x00\x00\x00climate")
    if anchor < 8:
        return None
    end = anchor - 4                       # the u32 count of settings pairs sits here
    lo = max(0, end - 16384)
    # Several starts can parse: the last entry's version field (1) reads as a
    # count of 1 and yields a one-mod list. The earliest start that parses is
    # the whole list -- the bytes before it are world data, and a spurious
    # chain of (len, ascii name, version) records there is vanishingly unlikely.
    best = None
    for start in range(end - 8, lo, -1):
        try:
            (count,) = struct.unpack_from("<I", head, start)
        except struct.error:
            continue
        if not (1 <= count <= 128):
            continue
        pos = start + 4
        mods = []
        ok = True
        for _ in range(count):
            if pos + 4 > end:
                ok = False; break
            (ln,) = struct.unpack_from("<I", head, pos); pos += 4
            if not (1 <= ln <= 120) or pos + ln + 4 > end:
                ok = False; break
            name = head[pos:pos + ln]; pos += ln
            if not _ID_RE.match(name.decode("ascii", "replace")):
                ok = False; break
            (ver,) = struct.unpack_from("<I", head, pos); pos += 4
            if ver > 100000:
                ok = False; break
            mods.append((name.decode("ascii"), ver))
        if ok and pos == end:
            best = mods
    return best


def save_mod_list(save_path):
    """[(id, version)] of the mods a save needs, excluding ours; None if the
    save could not be read (zstandard missing, unknown layout)."""
    try:
        head = _decompress_head(save_path)
    except OSError:
        return None
    mods = parse_mod_list(head)
    if mods is None:
        return None
    return [(m, v) for m, v in mods if m != MP_MOD_ID]


# ---------------------------------------------------------------------------
# zip / unzip
# ---------------------------------------------------------------------------
def zip_mod(folder):
    """The folder as one zip (entries relative to the folder) -- bytes, or
    None if it is too big."""
    buf = io.BytesIO()
    total = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(folder):
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            for fn in files:
                p = os.path.join(root, fn)
                if os.path.islink(p):
                    continue
                total += os.path.getsize(p)
                if total > MAX_MOD_ZIP:
                    return None
                z.write(p, os.path.relpath(p, folder).replace("\\", "/"))
    return buf.getvalue()


def mod_zip_name(mod_id, version):
    return f"{INCOMING_MOD_PREFIX}{mod_folder_name(mod_id, version)}.zip"


def parse_mod_zip_name(name):
    """(id, version) from an incoming_mod_<id>_<ver>.zip name, else None."""
    m = isinstance(name, str) and _MOD_ZIP_RE.match(name)
    if not m or os.path.basename(name) != name:
        return None
    stem = m.group(1)
    i = stem.rfind("_")
    if i <= 0 or not stem[i + 1:].isdigit():
        return None
    return stem[:i], int(stem[i + 1:])


def install_mod_zip(data, mod_id, version, log=None):
    """Unpack one received mod. Returns (status, path): status is
    'installed', 'present' (left alone), or 'failed'."""
    log = log or (lambda s: None)
    target = install_target(mod_id, version)
    if not target:
        log(f"[mods] no mods folder to install {mod_id} into")
        return "failed", None
    if os.path.isdir(target):
        return "present", target
    tmp = target + ".mp_incoming"
    try:
        if os.path.isdir(tmp):
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            base = os.path.realpath(tmp)
            for info in z.infolist():
                n = info.filename.replace("\\", "/")
                if n.startswith("/") or ".." in n.split("/") or ":" in n:
                    raise ValueError(f"unsafe path in zip: {n!r}")
                dest = os.path.realpath(os.path.join(tmp, n))
                if not dest.startswith(base + os.sep) and dest != base:
                    raise ValueError(f"path escapes the target: {n!r}")
                if n.endswith("/"):
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(info) as src, open(dest, "wb") as dst:
                    while True:
                        b = src.read(1 << 20)
                        if not b:
                            break
                        dst.write(b)
        if not os.path.isfile(os.path.join(tmp, "mod.lua")):
            raise ValueError("no mod.lua at the top of the zip")
        os.rename(tmp, target)
        return "installed", target
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        log(f"[mods] install of {mod_id}_{version} failed: {e}")
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        return "failed", None


def selftest():
    """Round-trip a fake mod through zip -> install into a temp game dir, and
    parse a synthetic save head."""
    import tempfile
    head = struct.pack("<I", 2) + struct.pack("<I", 5) + b"mod_a" + struct.pack("<I", 1) \
        + struct.pack("<I", 11) + b"mp_lockstep" + struct.pack("<I", 1) \
        + struct.pack("<I", 5) + b"\x07\x00\x00\x00climate" + b"\x09\x00\x00\x00temperate"
    assert parse_mod_list(b"junk" * 100 + head + b"tail") == [("mod_a", 1), ("mp_lockstep", 1)], "parse"
    assert parse_mod_zip_name("incoming_mod_mod_a_1.zip") == ("mod_a", 1)
    assert parse_mod_zip_name("incoming_mod_*123_1.zip") == ("*123", 1)
    assert parse_mod_zip_name("../incoming_mod_x_1.zip") is None
    assert parse_mod_zip_name("incoming_save.sav") is None
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "mod_a_1")
        os.makedirs(os.path.join(src, "res", "scripts"))
        open(os.path.join(src, "mod.lua"), "w").write("function data() return {} end")
        open(os.path.join(src, "res", "scripts", "x.lua"), "w").write("return 1")
        data = zip_mod(src)
        assert data and len(data) > 100
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            assert sorted(z.namelist()) == ["mod.lua", "res/scripts/x.lua"], z.namelist()
        # unsafe zip refused
        bad = io.BytesIO()
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("../evil.lua", "x")
            z.writestr("mod.lua", "x")
        global install_target
        real = install_target
        install_target = lambda mid, ver: os.path.join(td, "dest", f"{mid}_{ver}")
        try:
            st, _ = install_mod_zip(bad.getvalue(), "evil", 1)
            assert st == "failed", st
            st, p = install_mod_zip(data, "mod_a", 1)
            assert st == "installed" and os.path.isfile(os.path.join(p, "res", "scripts", "x.lua")), (st, p)
            st, _ = install_mod_zip(data, "mod_a", 1)
            assert st == "present", st
        finally:
            install_target = real
    print("modshare selftest: all checks passed")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] != "--selftest":
        print(save_mod_list(sys.argv[1]))
    else:
        selftest()
