"""
modshare.py -- share the mods a save needs with the players who lack them.

A shared save carries its own mod list and every mod's per-save settings (the
values from the Mods panel live inside the .sav, right after the list), so the
joiner needs both the files and recognition in the game's mod catalogue.
The host advertises the save's list on join and again with the save transfer.
Missing mods are downloaded only after consent. Workshop downloads live in
the multiplayer data folder; the native registrar adds them to the game's
catalogue. Loading waits for a matching catalogue receipt after installation.

The multiplayer mod itself is never shared (the installer ships it) and a mod
that is already present is never overwritten. Game DLC, including Deluxe and
Early Supporter content, is never packaged or installed by this module.

Save header format (measured on a 0.4.x autosave, 2026-09-11, re-measured on
a 379 MB 0.5.x save 2026-09-16: anchor 693 KB in): the .sav is one Zstandard
frame; a few hundred KB into the decompressed stream the active mod list is
``u32 count`` then ``count x (u32 len, name, u32 version)``, followed
immediately by the game settings pairs (``u32 n``, then ``u32 len "climate"``
...). The settings block is the anchor: the list is parsed backwards from it.
Nothing in that layout bounds the count, a name's length or a version, so
nothing here does either: the parser takes whatever the game wrote.

Mod ids: a folder ``<id>_<version>`` under the game's ``mods`` or the profile's
``local/mods``; a Steam Workshop item is ``*<workshopid>`` and lives under
``steamapps/workshop/content/1066780/<workshopid>`` or our managed workshop
folder after a multiplayer download. An id is any name the game accepts as a
mod folder (spaces and non-ASCII included); only what no folder can carry --
control characters, path separators and Windows' reserved characters -- is
refused.
"""
from __future__ import annotations
import io
import os
import shutil
import struct
import sys
import zipfile
import hashlib
import uuid

TF2_APPID = "1066780"
MP_MOD_ID = "mp_lockstep"                 # ours: shipped by the installer, never sent
INCOMING_MOD_PREFIX = "incoming_mod_"
MAX_VERSION = 0xFFFFFFFF                  # the save stores a u32: that IS the range
DECOMPRESS_CHUNK = 256 * 1024
# Characters no mod folder can carry on Windows (and '*', which the game uses
# only as the Workshop marker in front of a numeric id).
_FORBIDDEN_ID_CHARS = frozenset('/\\:"<>|?*')


def valid_id(m):
    """True for a name the game could have as a mod folder: non-empty, no
    control characters, none of the path/reserved characters, and '*' only as
    the Workshop prefix of a numeric id. No length cap: the OS has one, not us."""
    if not isinstance(m, str) or not m or m in (".", ".."):
        return False
    body = m[1:] if m.startswith("*") else m
    if m.startswith("*") and not body.isdigit():
        return False
    for ch in body:
        if ch < " " or ch == "\x7f" or ch in _FORBIDDEN_ID_CHARS:
            return False
    return True


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


def data_dir():
    return os.environ.get("TPF2MP_DATADIR") or os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "tpf2mp", "data")


def managed_workshop():
    return os.path.join(data_dir(), "workshop")


def cache_name(m,v):
    return hashlib.sha256(mod_folder_name(m,v).encode("utf-8")).hexdigest()+".zip"


def is_dlc(m):
    return m.startswith("_") or m in ("urbangames_deluxe_pack", "urbangames_preorder_pack")


def package_mod(m, v):
    if is_dlc(m) or not valid_mod(m,v):
        return None
    folder=find_mod(m,v)
    return zip_mod(folder) if folder else None


def valid_mod(m, v):
    """A (id, version) pair the game could have written: any folder name the
    game accepts and any u32 version. Until 2026-09-16 this refused ids over
    120 characters, ids with a space or a non-ASCII letter, Workshop ids over
    20 digits or at a version other than 1, and versions over 100000 -- and a
    refusal anywhere in a list made the whole save look mod-free."""
    return valid_id(m) and type(v) is int and 0 <= v <= MAX_VERSION


def catalogue():
    try:
        with open(os.path.join(data_dir(), "mods_catalogue.txt"), encoding="utf-8") as f:
            lines = f.read().splitlines()
        return lines[0], {tuple(line.rsplit("\t", 1)) for line in lines[1:] if "\t" in line}
    except (OSError, IndexError):
        return "", set()


def installed_mod(m, v):
    if not valid_mod(m, v) or find_mod(m, v) is None:
        return None
    _, entries = catalogue()
    return find_mod(m, v) if (m, str(v)) in entries else None


def request_catalogue():
    """Atomically publish all consented Workshop installs and a fresh receipt token."""
    root = data_dir()
    os.makedirs(root, exist_ok=True)
    token = uuid.uuid4().hex
    lines = [token]
    if os.path.isdir(managed_workshop()):
        for item in sorted(os.listdir(managed_workshop())):
            path = os.path.abspath(os.path.join(managed_workshop(), item))
            if item.isdigit() and os.path.isfile(os.path.join(path, "mod.lua")):
                lines.append(item + "\t" + path)
    # No cap on the number of rows: the reader (native/src/workshop_register.cpp)
    # registers every row, and refusing here would leave every consented mod
    # unregistered on this peer alone.
    target = os.path.join(root, "mods_registry.txt")
    with open(target + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(target + ".tmp", target)
    return token


def workshop_dir():
    root = steam_root()
    return os.path.join(root, "steamapps", "workshop", "content", TF2_APPID) if root else None


def mod_folder_name(mod_id, version):
    return f"{mod_id}_{int(version)}"


def find_mod(mod_id, version):
    """The installed folder for a mod id, or None."""
    if not valid_mod(mod_id, version):
        return None
    if mod_id.startswith("*"):
        for w in (workshop_dir(), managed_workshop()):
            p = w and os.path.join(w, mod_id[1:])
            if p and os.path.isfile(os.path.join(p, "mod.lua")):
                return p
        return None
    if mod_id.startswith("_"):
        g=game_dir()
        p=g and os.path.join(g,"dlcs",mod_folder_name(mod_id[1:],version))
        return p if p and os.path.isfile(os.path.join(p,"mod.lua")) else None
    name = mod_folder_name(mod_id, version)
    for base in (game_dir() and os.path.join(game_dir(), "mods"), userdata_mods_dir()):
        if base:
            p = os.path.join(base, name)
            if os.path.isfile(os.path.join(p, "mod.lua")):
                return p
    return None


# The joiner asks this (the host asks find_mod): one process can then play both
# ends in a test with different answers. In the game they are the same lookup.
# installed_mod checks the engine catalogue, not just files.


def install_target(mod_id, version):
    """Where a received mod goes: the game's mods folder (what the installer
    uses), the profile's local/mods if that is not writable, the workshop
    content folder for a workshop item."""
    if mod_id.startswith("*"):
        return os.path.join(managed_workshop(), mod_id[1:])
    name = mod_folder_name(mod_id, version)
    g = game_dir()
    if g and os.access(os.path.join(g, "mods"), os.W_OK):
        return os.path.join(g, "mods", name)
    u = userdata_mods_dir()
    return u and os.path.join(u, name)


# ---------------------------------------------------------------------------
# the save's mod list
# ---------------------------------------------------------------------------
SETTINGS_ANCHOR = b"\x07\x00\x00\x00climate"   # the first game-settings pair


class ModListError(Exception):
    """Why a save's mod list could not be read. Callers must treat this as
    UNKNOWN, never as 'no mods'."""


def _decompress_until(path, marker, tail=64):
    """The decompressed save from its start through ``marker`` (plus ``tail``
    bytes), or as much as there is when the marker never comes. Streams, so a
    save whose settings block sits deep in the file still parses (a fixed
    6 MB head until 2026-09-16 would have returned None for it).
    Raises ModListError when the file is not a Zstandard stream."""
    try:
        import zstandard
    except ImportError:
        raise ModListError("the zstandard module is not installed")
    out = bytearray()
    dec = zstandard.ZstdDecompressor().decompressobj()
    scanned = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(DECOMPRESS_CHUNK)
            if not chunk:
                break
            try:
                out += dec.decompress(chunk)
            except zstandard.ZstdError as e:
                if not out:
                    raise ModListError(f"not a Zstandard stream ({e})")
                break
            at = out.find(marker, max(0, scanned - len(marker)))
            if at >= 0:
                if len(out) >= at + len(marker) + tail:
                    del out[at + len(marker) + tail:]
                    break
                # found, but the tail is not all here yet: search from the
                # marker again next chunk. Resuming past it (scanned = len(out))
                # never saw it again and decompressed the WHOLE save into
                # memory (2026-09-16).
                scanned = at
            else:
                scanned = len(out)
    return bytes(out)


def _decode_id(raw):
    """The mod id in a save record, or None if no folder could be called that."""
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None
    return name if valid_id(name) else None


def parse_mod_list(head):
    """The active mod list out of a decompressed save head: [(id, version)],
    possibly empty. None when the layout is not recognised (never guess a
    list). Any count, any id the game accepts, any u32 version."""
    if not head:
        return None
    anchor = head.find(SETTINGS_ANCHOR)
    if anchor < 4:
        return None
    end = anchor - 4                       # the u32 count of settings pairs sits here
    # Several starts can parse: the last entry's version field (1) reads as a
    # count of 1 and yields a one-mod list. The earliest start that parses is
    # the whole list -- the bytes before it are world data, and a spurious
    # chain of (len, folder-name, version) records there is vanishingly
    # unlikely. The scan runs back to the start of the stream: a list is as
    # long as the player's mod folder, and a window (16 KB until 2026-09-16)
    # would have cut a long one off and reported a partial list.
    best = None
    unpack = struct.unpack_from
    for start in range(end - 4, -1, -1):
        (count,) = unpack("<I", head, start)
        if count * 9 > end - start - 4:    # a record is at least len+1 byte+version
            continue
        pos = start + 4
        mods = []
        ok = True
        for _ in range(count):
            if pos + 4 > end:
                ok = False; break
            (ln,) = unpack("<I", head, pos); pos += 4
            if ln < 1 or pos + ln + 4 > end:
                ok = False; break
            name = _decode_id(head[pos:pos + ln]); pos += ln
            if name is None:
                ok = False; break
            (ver,) = unpack("<I", head, pos); pos += 4
            mods.append((name, ver))
        if ok and pos == end:
            best = mods
    return best


def save_mod_list(save_path, log=None):
    """[(id, version)] of the mods a save needs, excluding ours (possibly
    empty); None if the list could not be READ -- zstandard missing, not a
    save, layout not recognised. A None is UNKNOWN: it means "we cannot tell
    which mods this save needs", never "none". ``log`` hears why."""
    log = log or (lambda s: None)
    try:
        head = _decompress_until(save_path, SETTINGS_ANCHOR)
    except OSError as e:
        log(f"[mods] cannot read the mod list of {save_path}: {e}")
        return None
    except ModListError as e:
        log(f"[mods] cannot read the mod list of {save_path}: {e}")
        return None
    except MemoryError:
        log(f"[mods] cannot read the mod list of {save_path}: out of memory before the settings block")
        return None
    mods = parse_mod_list(head)
    if mods is None:
        anchor = head.find(SETTINGS_ANCHOR)
        log(f"[mods] cannot read the mod list of {save_path}: "
            + (f"no record chain ends at the settings block ({anchor} bytes in)" if anchor >= 0
               else f"no settings block in {len(head)} decompressed bytes"))
        return None
    return [(m, v) for m, v in mods if m != MP_MOD_ID]


# ---------------------------------------------------------------------------
# zip / unzip
# ---------------------------------------------------------------------------
def zip_mod(folder, log=None):
    """The folder as one zip (entries relative to the folder) -- bytes. No
    size cap (512 MB until 2026-09-16: a big vehicle pack was silently "not
    found on the host"); a mod is as big as it is, and the transfer holds it
    in memory like it holds the save. ``log`` hears the size."""
    buf = io.BytesIO()
    total = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as z:
        for root, dirs, files in os.walk(folder):
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            for fn in files:
                p = os.path.join(root, fn)
                if os.path.islink(p):
                    continue
                total += os.path.getsize(p)
                z.write(p, os.path.relpath(p, folder).replace("\\", "/"))
    if log:
        log(f"[mods] packaged {folder}: {total} B of files, {buf.tell()} B zipped")
    return buf.getvalue()


def mod_zip_name(mod_id, version):
    return f"{INCOMING_MOD_PREFIX}{mod_folder_name(mod_id, version)}.zip"


def parse_mod_zip_name(name):
    """(id, version) from an incoming_mod_<id>_<ver>.zip name, else None.
    Any id valid_mod accepts (spaces and all) round-trips through here."""
    if not isinstance(name, str) or os.path.basename(name) != name:
        return None
    if not name.startswith(INCOMING_MOD_PREFIX) or not name.endswith(".zip"):
        return None
    stem = name[len(INCOMING_MOD_PREFIX):-4]
    i = stem.rfind("_")
    if i <= 0 or not stem[i + 1:].isdigit():
        return None
    mod_id, version = stem[:i], int(stem[i + 1:])
    return (mod_id, version) if valid_mod(mod_id, version) else None


def install_mod_zip(data, mod_id, version, log=None, progress=None):
    """Unpack one received mod. Returns (status, path): status is
    'installed', 'present' (left alone), or 'failed'. ``progress(n)`` is
    told every ``n`` bytes unpacked, so a caller working off its loop can
    show and report that a long unzip is moving."""
    log = log or (lambda s: None)
    progress = progress or (lambda n: None)
    if not valid_mod(mod_id, version) or is_dlc(mod_id):
        return "failed", None
    target = install_target(mod_id, version)
    if not target:
        log(f"[mods] no mods folder to install {mod_id} into")
        return "failed", None
    if os.path.isdir(target):
        return ("present", target) if os.path.isfile(os.path.join(target, "mod.lua")) else ("failed", None)
    tmp = target + ".mp_incoming"
    try:
        if os.path.isdir(tmp):
            shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            # No entry-count or byte cap (50000 files / 512 MB until
            # 2026-09-16). The one real bound is the disk: refuse, loudly,
            # what would not fit rather than fail half way through.
            declared = sum(i.file_size for i in z.infolist())
            try:
                free = shutil.disk_usage(os.path.dirname(os.path.abspath(tmp))).free
            except OSError:
                free = None
            if free is not None and declared > free:
                raise ValueError(f"mod unpacks to {declared} B but only {free} B are free")
            log(f"[mods] unpacking {mod_id}_{version}: {len(z.infolist())} entries, {declared} B")
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
                        progress(len(b))
        if not os.path.isfile(os.path.join(tmp, "mod.lua")):
            raise ValueError("no mod.lua at the top of the zip")
        os.rename(tmp, target)
        return "installed", target
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        log(f"[mods] install of {mod_id}_{version} failed: {e}")
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
    # No limits: 150 mods, ids with spaces and non-ASCII letters, an id longer
    # than 120 characters, a version past 100000, a 25-digit Workshop id, and
    # a list that is far longer than the old 16 KB search window.
    big = [(f"Some Mod Pack {i} (v2)", 300000 + i) for i in range(150)]
    big += [("Straßenbahn München ÖPNV", 7), ("x" * 400, MAX_VERSION), ("*1234567890123456789012345", 1)]
    def encode(mods):
        body = struct.pack("<I", len(mods))
        for m, v in mods:
            raw = m.encode("utf-8")
            body += struct.pack("<I", len(raw)) + raw + struct.pack("<I", v)
        return body + struct.pack("<I", 5) + SETTINGS_ANCHOR + b"\x09\x00\x00\x00temperate"
    assert parse_mod_list(b"\xc7\xa9\xb9" * 3000 + encode(big)) == big, "big list"
    assert parse_mod_list(b"\xc7\xa9\xb9" * 3000 + encode([])) == [], "empty list"
    long_list = [("m" * 300 + str(i), i) for i in range(200)]     # ~62 KB of records
    assert parse_mod_list(b"\x00" * 100 + encode(long_list)) == long_list, "long list"
    assert parse_mod_list(b"\x07\x00\x00\x00climax") is None, "no anchor"
    assert parse_mod_list(b"\x05\x00\x00\x00" + SETTINGS_ANCHOR) is None, "no chain"
    assert all(valid_mod(m, v) for m, v in big), "valid_mod"
    assert not valid_mod("a/b", 1) and not valid_mod("a\x00b", 1) and not valid_mod("*abc", 1)
    assert not valid_mod("x", -1) and not valid_mod("x", MAX_VERSION + 1) and not valid_mod("x", True)
    assert parse_mod_zip_name("incoming_mod_mod_a_1.zip") == ("mod_a", 1)
    assert parse_mod_zip_name("incoming_mod_*123_1.zip") == ("*123", 1)
    assert parse_mod_zip_name("incoming_mod_Some Mod Pack 3 (v2)_300003.zip") == ("Some Mod Pack 3 (v2)", 300003)
    assert parse_mod_zip_name(mod_zip_name("Straßenbahn München ÖPNV", 7)) == ("Straßenbahn München ÖPNV", 7)
    assert parse_mod_zip_name("../incoming_mod_x_1.zip") is None
    assert parse_mod_zip_name("incoming_mod_a/b_1.zip") is None
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
