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

Mod ids: a folder ``<id>_<version>`` under the game's ``mods``; ``!<id>`` is the
same folder under the profile's ``local/mods`` (Steam userdata: the game writes
the ``!`` for mods from there, and its catalogue lists them with it -- measured
on ``!tpf2_multiplayer_link``, 2026-09-20); ``_<id>`` is a DLC under ``dlcs``;
a Steam Workshop item is ``*<workshopid>`` and lives under
``steamapps/workshop/content/1066780/<workshopid>`` or our managed workshop
folder after a multiplayer download. An id is any name the game accepts as a
mod folder (spaces and non-ASCII included); only what no folder can carry --
control characters, path separators and Windows' reserved characters -- is
refused.

On Linux the folders come from linuxpaths.py (every Steam root and library,
the TransportFever2 binary) instead of the registry.
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
import threading
import re

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
    reads), else the default. None only if nothing looks like Steam. On Linux
    the first Steam root linuxpaths finds (native, snap, Flatpak)."""
    if sys.platform != "win32":
        import linuxpaths
        roots = linuxpaths.steam_roots()
        return roots[0] if roots else None
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
    <gamedir>\\netpunch (the installed layout), else via Steam. On Linux:
    TPF2MP_GAME_DIR, beside the lobby, then every Steam library."""
    here = os.path.dirname(os.path.abspath(getattr(sys, "frozen", False) and sys.executable or __file__))
    if sys.platform != "win32":
        import linuxpaths
        return linuxpaths.game_dir(beside=here)
    parent = os.path.dirname(here)
    if os.path.isfile(os.path.join(parent, "TransportFever2.exe")):
        return parent
    root = steam_root()
    if root:
        g = os.path.join(root, "steamapps", "common", "Transport Fever 2")
        if os.path.isfile(os.path.join(g, "TransportFever2.exe")):
            return g
    return None


def _newest_local():
    """<steam>\\userdata\\<account>\\1066780\\local of the account that has a save
    folder (newest wins), like the menu DLL's resolveSaveDir; None without Steam."""
    if sys.platform != "win32":
        import linuxpaths
        mods = linuxpaths.userdata_mods_dir()
        return os.path.dirname(mods) if mods else None
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
                    best, best_t = local, t
    except OSError:
        pass
    return best


def userdata_mods_dir():
    """<steam>\\userdata\\<account>\\1066780\\local\\mods of the newest account."""
    local = _newest_local()
    return os.path.join(local, "mods") if local else None


def game_log_path():
    """The game's own log (stdout.txt, the Lua print lines) of the newest account."""
    local = _newest_local()
    return os.path.join(local, "crash_dump", "stdout.txt") if local else None


_UNREADABLE_CACHE = {}
_UNREADABLE_LINE = re.compile(r"Lua error while reading (.+?)[/\\\\]mod\.lua: .*Mod will be skipped\.", re.I)


def unreadable_mod_folders(log_path=None):
    """Folders the running game could NOT read a mod.lua from, as its stdout.txt
    says at startup: 'Lua error while reading <folder>/mod.lua: Unknown exception.
    Mod will be skipped.' Such a mod is skipped at the title menu, but a shared
    save that needs it makes the world load run that same file, and that dies
    (a joiner at 78% of every load, the Boeing 777 Pack's mod.lua, 2026-09-20).
    Normalised paths; cached per (path, size, mtime)."""
    path = log_path or game_log_path()
    if not path:
        return set()
    try:
        st = os.stat(path)
    except OSError:
        return set()
    key = (path, st.st_size, st.st_mtime_ns)
    hit = _UNREADABLE_CACHE.get(path)
    if hit and hit[0] == key:
        return hit[1]
    found = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = _UNREADABLE_LINE.search(line)
                if m:
                    found.add(os.path.normcase(os.path.normpath(m.group(1))))
    except OSError:
        pass
    _UNREADABLE_CACHE[path] = (key, found)
    return found


skipped_copies = {}     # mod id -> the folder find_mod passed over because the game could not read it


def _readable(mod_id, folder):
    """False for a folder the game reported unreadable (remembered in skipped_copies)."""
    if os.path.normcase(os.path.normpath(folder)) in unreadable_mod_folders():
        skipped_copies[mod_id] = folder
        return False
    return True


def data_dir():
    if sys.platform != "win32":
        import linuxpaths
        return linuxpaths.data_dir()
    return os.environ.get("TPF2MP_DATADIR") or os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "tpf2mp", "data")


def managed_workshop():
    return os.path.join(data_dir(), "workshop")


def cache_name(m,v):
    return hashlib.sha256(mod_folder_name(m,v).encode("utf-8")).hexdigest()+".zip"


def is_dlc(m):
    return m.startswith("_") or m in ("urbangames_deluxe_pack", "urbangames_preorder_pack")


# The HOST's zip cache: <data>\mod_zip_cache\<sha256 of id_version>.zip plus a
# .sig sidecar naming the deflate level and the folder's signature (file count,
# bytes, newest mtime). A mod is zipped once per change of its folder, not once
# per joiner per session: the 556-mod round re-deflated 93 GB every time
# (2026-09-20), and the zip -- not the wire -- is what paces a LAN round.
# TPF2MP_MOD_CACHE_MB caps it (oldest-used zips go first); 0 turns it off.
MOD_ZIP_CACHE_MB = 24 * 1024


def mod_zip_cache_mb():
    try:
        return max(0, int(os.environ.get("TPF2MP_MOD_CACHE_MB", MOD_ZIP_CACHE_MB)))
    except ValueError:
        return MOD_ZIP_CACHE_MB


def mod_zip_cache_dir():
    return os.path.join(data_dir(), "mod_zip_cache")


def folder_signature(folder):
    """'files:bytes:newest mtime_ns' over the files zip_mod would read."""
    files = total = newest = 0
    for root, dirs, names in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for fn in names:
            p = os.path.join(root, fn)
            if os.path.islink(p):
                continue
            try:
                st = os.stat(p)
            except OSError:
                continue
            files += 1
            total += st.st_size
            newest = max(newest, st.st_mtime_ns)
    return f"{files}:{total}:{newest}"


def prune_zip_cache(cdir, cap_bytes, keep=None):
    """Drop the least recently used zips until the cache fits ``cap_bytes``."""
    entries = []
    try:
        for fn in os.listdir(cdir):
            if fn.endswith(".zip"):
                p = os.path.join(cdir, fn)
                try:
                    st = os.stat(p)
                    entries.append((st.st_mtime_ns, st.st_size, p))
                except OSError:
                    pass
    except OSError:
        return
    total = sum(size for _, size, _ in entries)
    for _, size, p in sorted(entries):
        if total <= cap_bytes:
            break
        if p == keep:
            continue
        for victim in (p, p + ".sig"):
            try:
                os.remove(victim)
            except OSError:
                pass
        total -= size


def package_mod(m, v, level=None, log=None):
    """The mod as one zip (bytes), from the zip cache when its folder has not
    changed since it was last packaged at this level; None if it is DLC or
    not installed here."""
    if is_dlc(m) or not valid_mod(m,v):
        return None
    folder=find_mod(m,v)
    if not folder:
        return None
    cap = mod_zip_cache_mb() << 20
    if not cap:
        return zip_mod(folder, log=log, level=level)
    sig = f"{level}|{folder_signature(folder)}"
    cdir = mod_zip_cache_dir()
    path = os.path.join(cdir, cache_name(m, v))
    try:
        with open(path + ".sig", encoding="utf-8") as f:
            cached = f.read().strip() == sig
        if cached:
            with open(path, "rb") as f:
                data = f.read()
            os.utime(path)                       # most recently used
            if log:
                log(f"[mods] {mod_folder_name(m, v)} from the zip cache ({len(data)} B)")
            return data
    except OSError:
        pass
    data = zip_mod(folder, log=log, level=level)
    try:
        os.makedirs(cdir, exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
        with open(path + ".sig.tmp", "w", encoding="utf-8") as f:
            f.write(sig)
        os.replace(path + ".sig.tmp", path + ".sig")
        prune_zip_cache(cdir, cap, keep=path)
    except OSError as e:
        if log:
            log(f"[mods] zip cache write for {mod_folder_name(m, v)} failed: {e}")
    return data


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


def on_disk_mod(m, v):
    """The JOINER's on-disk lookup: the folder a required mod already lives in
    on this machine, whether or not the game has catalogued it (find_mod). A
    mod that is here but not catalogued is REGISTERED, never downloaded: the
    registry names its folder and the game's next catalogue refresh lists it.
    Separate from installed_mod (catalogued) and find_mod (the host's lookup)
    so a test can give the ends different answers."""
    return find_mod(m, v)


def registry_path():
    return os.path.join(data_dir(), "mods_registry.txt")


def read_registry():
    """(token, {workshop id: folder}) the registry names now; ('', {}) if none."""
    try:
        with open(registry_path(), encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return "", {}
    token = lines[0] if lines and len(lines[0]) == 32 and all(c in "0123456789abcdef" for c in lines[0]) else ""
    rows = {}
    for line in lines[1:]:
        if "\t" in line:
            item, path = line.split("\t", 1)
            if item.isdigit() and path:
                rows[item] = path
    return token, rows


def write_registry(token=None, extra=None):
    """Atomically publish the Workshop registry: every consented install under
    the managed workshop folder, every row already published whose folder
    still holds a mod, and ``extra`` [(workshop id, folder)] -- the folders a
    save needs that are on this machine but not in the game's catalogue.

    ``token`` None KEEPS the token the file carries (minting one if it has
    none): a publish after each batch of a round, or at lobby start, must not
    change the token a pending receipt is waiting for. request_catalogue
    mints a fresh token, which is what a receipt is matched against.

    Until 2026-09-20 the registry was written once, after a round's LAST
    batch, so a round that never finished (359 batches; the joiner left or
    the host failed it) published nothing: at the next game start none of
    the folders it had installed were registered, the catalogue lacked them,
    and every one was requested, zipped and sent again -- to be found
    "present" on arrival."""
    root = data_dir()
    os.makedirs(root, exist_ok=True)
    old_token, rows = read_registry()
    rows = {item: path for item, path in rows.items() if os.path.isfile(os.path.join(path, "mod.lua"))}
    if os.path.isdir(managed_workshop()):
        for item in sorted(os.listdir(managed_workshop())):
            path = os.path.abspath(os.path.join(managed_workshop(), item))
            if item.isdigit() and os.path.isfile(os.path.join(path, "mod.lua")):
                rows[item] = path
    for item, path in (extra or []):
        if isinstance(item, str) and item.isdigit() and path and os.path.isfile(os.path.join(path, "mod.lua")):
            rows[item] = os.path.abspath(path)
    token = token or old_token or uuid.uuid4().hex
    # No cap on the number of rows: the reader (native/src/workshop_register.cpp)
    # registers every row, and refusing here would leave every consented mod
    # unregistered on this peer alone.
    lines = [token] + [item + "\t" + rows[item] for item in sorted(rows)]
    target = registry_path()
    with open(target + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(target + ".tmp", target)
    return token


def request_catalogue(extra=None):
    """Publish the registry under a FRESH token and return it: the game's next
    catalogue refresh writes a receipt carrying that token (see catalogue())."""
    return write_registry(uuid.uuid4().hex, extra)


def library_root():
    """The Steam LIBRARY that holds the game (<lib>\\steamapps\\common\\Transport
    Fever 2 -> <lib>), or None. Workshop content lives in the library of the
    game, not in Steam's own folder: a host with the game on D:\\SteamLibrary
    keeps its Workshop mods under D:, and looking only under C:\\...\\Steam found
    none of them -- every joiner got "The host cannot supply all required
    mods" (2026-09-18)."""
    g = game_dir()
    if not g:
        return None
    lib = os.path.dirname(os.path.dirname(os.path.dirname(g)))
    return lib if os.path.isdir(os.path.join(lib, "steamapps")) else None


def test_flags():
    """Rig-only switches: the lines of <data>\\tpf2mp_modtest.txt (like
    tpf2mp_netsim.txt, one instance's data folder, never shipped).
    ``ignore_steam_workshop``: this end treats the Steam library's Workshop
    content as absent, so a joiner on the host's own PC -- which otherwise
    finds and registers every mod in the host's library -- has to be sent
    them: the mod transfer-speed test (2026-09-20)."""
    try:
        with open(os.path.join(data_dir(), "tpf2mp_modtest.txt"), encoding="utf-8") as f:
            return {ln.strip() for ln in f if ln.strip() and not ln.startswith("#")}
    except OSError:
        return set()


def workshop_dirs():
    """Every Workshop content folder this game's mods could be in: the game's
    library first, then Steam's own folder (the same place when the game is
    installed there)."""
    out = []
    roots = [library_root(), steam_root()]
    if sys.platform != "win32":
        import linuxpaths
        for steam in linuxpaths.steam_roots():
            roots.extend(linuxpaths.library_dirs(steam))
    for root in roots:
        if root:
            p = os.path.join(root, "steamapps", "workshop", "content", TF2_APPID)
            if p not in out:
                out.append(p)
    return out


def workshop_dir():
    """The Workshop content folder of the game's library (the first of
    workshop_dirs), or None."""
    dirs = workshop_dirs()
    return dirs[0] if dirs else None


def mod_folder_name(mod_id, version):
    return f"{mod_id}_{int(version)}"


def find_mod(mod_id, version):
    """The installed folder for a mod id, or None."""
    if not valid_mod(mod_id, version):
        return None
    if mod_id.startswith("*"):
        library = [] if "ignore_steam_workshop" in test_flags() else workshop_dirs()
        for w in library + [managed_workshop()]:
            p = w and os.path.join(w, mod_id[1:])
            # a copy the game could not read counts as absent: the next candidate
            # (the managed download) or nothing, so the host's copy is fetched and
            # registered in place of the broken one
            if p and os.path.isfile(os.path.join(p, "mod.lua")) and _readable(mod_id, p):
                return p
        return None
    if mod_id.startswith("_"):
        g=game_dir()
        p=g and os.path.join(g,"dlcs",mod_folder_name(mod_id[1:],version))
        return p if p and os.path.isfile(os.path.join(p,"mod.lua")) else None
    if mod_id.startswith("!"):
        # the profile's local/mods: the save and the catalogue carry the '!', the
        # folder does not. Until 2026-09-20 this looked for '!<id>_<ver>' under the
        # game's mods and found nothing on either end: the joiner asked for a mod its
        # catalogue already listed, the host could not supply it, and the join was
        # rejected (the Workshop-registered multiplayer link mod, every session).
        u = userdata_mods_dir()
        p = u and os.path.join(u, mod_folder_name(mod_id[1:], version))
        return p if p and os.path.isfile(os.path.join(p, "mod.lua")) else None
    name = mod_folder_name(mod_id, version)
    for base in (game_dir() and os.path.join(game_dir(), "mods"), userdata_mods_dir()):
        if base:
            p = os.path.join(base, name)
            if os.path.isfile(os.path.join(p, "mod.lua")) and _readable(mod_id, p):
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
    if mod_id.startswith("!"):
        u = userdata_mods_dir()                  # where the game will list it as '!<id>' again
        return u and os.path.join(u, mod_folder_name(mod_id[1:], version))
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
    # the save's bytes in memory (a sync snapshot) read the same way as a file
    opener = (lambda: io.BytesIO(path)) if isinstance(path, (bytes, bytearray, memoryview)) else (lambda: open(path, "rb"))
    with opener() as f:
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
    return _save_mod_list_from(save_path, save_path, log)


def save_mod_list_bytes(data, log=None, label="the sync save"):
    """save_mod_list for a save held in memory (a frozen join's or resync's
    snapshot): the same answer, the same None for unknown."""
    return _save_mod_list_from(data, label, log)


def _save_mod_list_from(source, label, log):
    save_path = label
    log = log or (lambda s: None)
    try:
        head = _decompress_until(source, SETTINGS_ANCHOR)
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
def folder_bytes(folder):
    """Bytes of files under ``folder`` (what zip_mod would read); 0 if unreadable."""
    total = 0
    try:
        for root, dirs, files in os.walk(folder):
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            for fn in files:
                p = os.path.join(root, fn)
                if not os.path.islink(p):
                    try:
                        total += os.path.getsize(p)
                    except OSError:
                        pass
    except OSError:
        pass
    return total


def zip_mod(folder, log=None, level=None):
    """The folder as one zip (entries relative to the folder) -- bytes. No
    size cap (512 MB until 2026-09-16: a big vehicle pack was silently "not
    found on the host"); a mod is as big as it is, and the transfer holds it
    in memory like it holds the save. ``log`` hears the size. ``level`` is
    the deflate level (None = zlib's default, 6): a mods round packs at 3 --
    measured on a 1.2 GB Workshop vehicle pack (2026-09-19): level 6 took
    16.0 s for 213 MB, level 3 10.3 s for 229 MB, level 1 8.5 s for 236 MB."""
    buf = io.BytesIO()
    total = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, allowZip64=True, compresslevel=level) as z:
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


def _zip_entries_safe(z, tmp):
    """[(info, dest, is_dir)] for every entry of ``z`` under ``tmp``; ValueError
    on a path that would leave it."""
    base = os.path.realpath(tmp)
    out = []
    for info in z.infolist():
        n = info.filename.replace("\\", "/")
        if n.startswith("/") or ".." in n.split("/") or ":" in n:
            raise ValueError(f"unsafe path in zip: {n!r}")
        dest = os.path.realpath(os.path.join(tmp, n))
        if not dest.startswith(base + os.sep) and dest != base:
            raise ValueError(f"path escapes the target: {n!r}")
        out.append((info, dest, n.endswith("/")))
    return out


def _extract_share(data, entries, progress):
    """POOL THREAD: one share of a zip's entries, through its own ZipFile over
    the same bytes (a ZipFile is not shared between threads)."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info, dest, is_dir in entries:
            if is_dir:
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


def install_mod_zips(items, log=None, progress=None, threads=4):
    """Unpack several received mods at once: ``items`` [(zip bytes, id, version)]
    -> {folder name: (status, path)}, statuses as install_mod_zip gives them.

    Every zip's entries are split into ``threads`` shares and all shares of
    all zips run on one pool, so a batch of many small mods and a single mod
    of 1,300 files both keep every thread busy (one thread per zip left the
    big mod alone for 8 s while the others idled, 2026-09-20). A mod's folder
    still appears only whole: extraction goes to <target>.mp_incoming and the
    rename is the last step, as install_mod_zip does one at a time."""
    log = log or (lambda s: None)
    progress = progress or (lambda n: None)
    from concurrent.futures import ThreadPoolExecutor
    results, plans = {}, []
    with ThreadPoolExecutor(max_workers=max(1, threads),
                            thread_name_prefix=threading.current_thread().name) as pool:
        for data, mod_id, version in items:
            label = mod_folder_name(mod_id, version) if valid_mod(mod_id, version) else str(mod_id)
            if not valid_mod(mod_id, version) or is_dlc(mod_id):
                results[label] = ("failed", None)
                continue
            target = install_target(mod_id, version)
            if not target:
                log(f"[mods] no mods folder to install {mod_id} into")
                results[label] = ("failed", None)
                continue
            if os.path.isdir(target):
                results[label] = ("present", target) if os.path.isfile(os.path.join(target, "mod.lua")) else ("failed", None)
                continue
            tmp = target + ".mp_incoming"
            try:
                if os.path.isdir(tmp):
                    shutil.rmtree(tmp, ignore_errors=True)
                os.makedirs(tmp, exist_ok=True)
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    entries = _zip_entries_safe(z, tmp)
                    declared = sum(i.file_size for i in z.infolist())
                try:
                    free = shutil.disk_usage(os.path.dirname(os.path.abspath(tmp))).free
                except OSError:
                    free = None
                if free is not None and declared > free:
                    raise ValueError(f"mod unpacks to {declared} B but only {free} B are free")
                log(f"[mods] unpacking {label}: {len(entries)} entries, {declared} B")
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                log(f"[mods] install of {label} failed: {e}")
                shutil.rmtree(tmp, ignore_errors=True)
                results[label] = ("failed", None)
                continue
            k = max(1, min(threads, len(entries)))
            shares = [entries[i::k] for i in range(k)]
            futures = [pool.submit(_extract_share, data, share, progress) for share in shares if share]
            plans.append((label, tmp, target, futures))
        for label, tmp, target, futures in plans:
            err = None
            for f in futures:
                try:
                    f.result()
                except (OSError, ValueError, zipfile.BadZipFile) as e:
                    err = err or e
            if err is None and not os.path.isfile(os.path.join(tmp, "mod.lua")):
                err = ValueError("no mod.lua at the top of the zip")
            if err is None:
                try:
                    os.rename(tmp, target)
                    results[label] = ("installed", target)
                    continue
                except OSError as e:
                    err = e
            log(f"[mods] install of {label} failed: {err}")
            shutil.rmtree(tmp, ignore_errors=True)
            results[label] = ("failed", None)
    return results


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
    # the zip cache: packaged once, then read back until the folder changes; the
    # registry keeps its rows and token across publishes and adds extra rows
    with tempfile.TemporaryDirectory() as td:
        global data_dir, find_mod
        real_dd, real_fm = data_dir, find_mod
        src = os.path.join(td, "mod_c_2")
        os.makedirs(src)
        open(os.path.join(src, "mod.lua"), "w").write("function data() return {} end")
        data_dir = lambda: os.path.join(td, "data")
        find_mod = lambda m, v: src if m == "mod_c" else None
        try:
            calls = []
            first = package_mod("mod_c", 2, level=3, log=calls.append)
            again = package_mod("mod_c", 2, level=3, log=calls.append)
            assert first == again and any("from the zip cache" in s for s in calls), calls
            assert not any("from the zip cache" in s for s in calls[:1]), calls
            assert package_mod("mod_c", 2, level=1, log=calls.append) is not None   # another level: repackaged
            assert sum("from the zip cache" in s for s in calls) == 1, calls
            os.utime(os.path.join(src, "mod.lua"), (2_000_000_000, 2_000_000_000))    # the folder changed
            package_mod("mod_c", 2, level=1, log=calls.append)
            assert sum("from the zip cache" in s for s in calls) == 1, "a changed folder is repackaged"
            assert package_mod("mod_c", 2, level=1, log=calls.append) and sum("from the zip cache" in s for s in calls) == 2
            os.environ["TPF2MP_MOD_CACHE_MB"] = "0"
            try:
                package_mod("mod_c", 2, level=1, log=calls.append)
                assert sum("from the zip cache" in s for s in calls) == 2, "cache off"
            finally:
                del os.environ["TPF2MP_MOD_CACHE_MB"]
            prune_zip_cache(mod_zip_cache_dir(), 0)
            assert not [f for f in os.listdir(mod_zip_cache_dir()) if f.endswith(".zip")], "pruned"
            # registry
            ws = os.path.join(td, "ws", "555")
            os.makedirs(ws)
            open(os.path.join(ws, "mod.lua"), "w").write("x")
            t1 = request_catalogue([("555", ws)])
            assert read_registry() == (t1, {"555": os.path.abspath(ws)}), read_registry()
            assert write_registry() == t1 and read_registry()[1] == {"555": os.path.abspath(ws)}, "kept"
            t2 = request_catalogue()
            assert t2 != t1 and read_registry() == (t2, {"555": os.path.abspath(ws)}), "rows survive a fresh token"
            os.remove(os.path.join(ws, "mod.lua"))
            write_registry()
            assert read_registry()[1] == {}, "a row whose folder lost its mod.lua is dropped"
        finally:
            data_dir, find_mod = real_dd, real_fm
    # install_mod_zips: many files of one mod across the pool, a second small mod,
    # an unsafe one refused, a present one kept
    with tempfile.TemporaryDirectory() as td:
        real_it = install_target
        install_target = lambda mid, ver: os.path.join(td, "dest", f"{mid}_{ver}")
        try:
            big = io.BytesIO()
            with zipfile.ZipFile(big, "w") as z:
                z.writestr("mod.lua", "x")
                for i in range(300):
                    z.writestr(f"res/models/m{i // 20}/f{i}.txt", str(i) * 100)
            small = io.BytesIO()
            with zipfile.ZipFile(small, "w") as z:
                z.writestr("mod.lua", "y")
            bad = io.BytesIO()
            with zipfile.ZipFile(bad, "w") as z:
                z.writestr("../evil.lua", "x")
                z.writestr("mod.lua", "x")
            os.makedirs(os.path.join(td, "dest", "have_1"))
            open(os.path.join(td, "dest", "have_1", "mod.lua"), "w").write("z")
            got = 0
            def count(n):
                nonlocal got
                got += n
            r = install_mod_zips([(big.getvalue(), "big", 1), (small.getvalue(), "small", 2),
                                  (bad.getvalue(), "evil", 1), (small.getvalue(), "have", 1)],
                                 progress=count, threads=4)
            assert r["big_1"][0] == "installed" and r["small_2"][0] == "installed", r
            assert r["evil_1"] == ("failed", None) and r["have_1"][0] == "present", r
            assert not os.path.exists(os.path.join(td, "dest", "evil_1")) and not os.path.exists(os.path.join(td, "dest", "evil_1.mp_incoming"))
            files = [f for _, _, fs in os.walk(os.path.join(td, "dest", "big_1")) for f in fs]
            assert len(files) == 301, len(files)
            expect = sum(len(str(i)) * 100 for i in range(300)) + 2   # big's files + two mod.lua bytes
            assert got == expect, (got, expect)          # every byte counted once
        finally:
            install_target = real_it
    # a folder the game's log calls unreadable is passed over: the managed copy or nothing
    with tempfile.TemporaryDirectory() as td:
        global game_log_path, workshop_dirs
        real_glp, real_wd, real_dd = game_log_path, workshop_dirs, data_dir
        lib = os.path.join(td, "library")
        for base in (lib, os.path.join(td, "data", "workshop")):
            os.makedirs(os.path.join(base, "555"))
            open(os.path.join(base, "555", "mod.lua"), "w").write("x")
        os.makedirs(os.path.join(lib, "666"))
        open(os.path.join(lib, "666", "mod.lua"), "w").write("x")
        log_path = os.path.join(td, "stdout.txt")
        open(log_path, "w", encoding="utf-8").write(
            "Found 923 mods\n"
            f"Lua error while reading {lib.replace(chr(92), '/')}/555/mod.lua: Unknown exception. Mod will be skipped.\n"
            f"Lua error while reading {lib.replace(chr(92), '/')}/666/mod.lua: Unknown exception. Mod will be skipped.\n")
        game_log_path = lambda: log_path
        workshop_dirs = lambda: [lib]
        data_dir = lambda: os.path.join(td, "data")
        skipped_copies.clear()
        try:
            assert unreadable_mod_folders(log_path) == {os.path.normcase(os.path.join(lib, "555")), os.path.normcase(os.path.join(lib, "666"))}
            assert find_mod("*555", 1) == os.path.join(td, "data", "workshop", "555"), "the managed copy stands in"
            assert find_mod("*666", 1) is None and skipped_copies["*666"] == os.path.join(lib, "666"), "nothing else: absent, and remembered"
            assert unreadable_mod_folders(os.path.join(td, "nolog.txt")) == set()
        finally:
            game_log_path, workshop_dirs, data_dir = real_glp, real_wd, real_dd
            skipped_copies.clear()
    # tpf2mp_modtest.txt: ignore_steam_workshop hides the library, not the managed folder
    with tempfile.TemporaryDirectory() as td:
        # data_dir, workshop_dirs: this function's globals, declared above
        real_dd, real_wd = data_dir, workshop_dirs
        data_dir = lambda: os.path.join(td, "data")
        workshop_dirs = lambda: [os.path.join(td, "library")]
        try:
            for base in ("library", os.path.join("data", "workshop")):
                os.makedirs(os.path.join(td, base, "777"))
                open(os.path.join(td, base, "777", "mod.lua"), "w").write("x")
            os.makedirs(os.path.join(td, "library", "888"))
            open(os.path.join(td, "library", "888", "mod.lua"), "w").write("x")
            assert find_mod("*777", 1) == os.path.join(td, "library", "777") and find_mod("*888", 1), "library first"
            os.makedirs(os.path.join(td, "data"), exist_ok=True)
            open(os.path.join(td, "data", "tpf2mp_modtest.txt"), "w").write("# rig\nignore_steam_workshop\n")
            assert find_mod("*777", 1) == os.path.join(td, "data", "workshop", "777"), "managed copy only"
            assert find_mod("*888", 1) is None, "library hidden"
        finally:
            data_dir, workshop_dirs = real_dd, real_wd
    # '!<id>': the profile's local/mods, without the '!' on the folder
    with tempfile.TemporaryDirectory() as td:
        global userdata_mods_dir
        real_ud = userdata_mods_dir
        userdata_mods_dir = lambda: os.path.join(td, "local", "mods")
        try:
            os.makedirs(os.path.join(td, "local", "mods", "link_1"))
            open(os.path.join(td, "local", "mods", "link_1", "mod.lua"), "w").write("x")
            assert find_mod("!link", 1) == os.path.join(td, "local", "mods", "link_1"), find_mod("!link", 1)
            assert find_mod("!link", 2) is None and find_mod("!other", 1) is None
            assert install_target("!other", 3) == os.path.join(td, "local", "mods", "other_3")
            z = io.BytesIO()
            with zipfile.ZipFile(z, "w") as zz:
                zz.writestr("mod.lua", "x")
            st, p = install_mod_zip(z.getvalue(), "!other", 3)
            assert st == "installed" and p == install_target("!other", 3) and find_mod("!other", 3) == p, (st, p)
        finally:
            userdata_mods_dir = real_ud
    print("modshare selftest: all checks passed")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] != "--selftest":
        print(save_mod_list(sys.argv[1]))
    else:
        selftest()
