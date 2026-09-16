#!/usr/bin/env python3
"""Install TpF2 Multiplayer into Transport Fever 2 running under Steam Proton.

Installs the same files the Windows MSI installs, into the Windows game that
Steam runs through Proton on Linux (including the Steam Deck), and prepares the
Proton prefix. Python 3.9+ standard library only; nothing is compiled, no Wine
command is run, and the game is never started.

    python3 install.py                 install the newest release (downloaded from GitHub)
    python3 install.py --version 0.5.7 install that release (pre-releases included)
    python3 install.py --dry-run       show what would change; change nothing
    python3 install.py --verify        check an installation
    python3 install.py --uninstall     remove the mod and restore the game's own alut.dll

Payload sources, instead of the download: --files-zip TpF2Multiplayer-files.zip
(a release asset), --msi TpF2Multiplayer.msi (needs msiextract from msitools),
or --payload-dir (an extracted MSI's "Transport Fever 2" directory).

Steam, the game and the Proton prefix are found automatically (native, Snap and
Flatpak Steam; every library in libraryfolders.vdf). Override with --steam-root,
--game-dir and --prefix.

What it does, in order: refuses while the game runs; keeps the game's own
alut.dll as alut_real.dll (the mod loads through an alut.dll proxy); copies the
payload, replacing the previous mod version and keeping an existing tpf2_slice.cfg;
repairs the lobby executable for Wine (below); links the prefix's Steam folders
(userdata, steamapps/common, steamapps/workshop) to the real ones so the menu
finds saves and mods; records what it installed for --verify and --uninstall.
Replaced files go to <game>/.tpf2mp-proton-backups/<timestamp>/.

The lobby repair. netpunch.exe (a PyInstaller bundle) embeds a miniupnpc DLL
whose relocation table lists 64 targets twice. Wine relocates the DLL and
applies both, and hosting a game crashes at once (docs/proton/NAT_CRASH.md).
The repair turns the second copy of each entry into padding, changing nothing
else, and is pinned to the exact DLL every release so far has shipped. It is
applied to the installed netpunch.exe and to any copy the in-game updater has
cached in the prefix. `--repair-lobby FILE` applies it to a file in place (the
release build uses this so shipped lobbies need no repair).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
import zlib
from pathlib import Path, PurePosixPath

REPO = "silver2127/tpf2-multiplayer"
APP_ID = "1066780"
GAME_FOLDER = "Transport Fever 2"
FILES_ASSET = "TpF2Multiplayer-files.zip"
SUMS_ASSET = "SHA256SUMS.txt"
DEFAULT_VERSION = None          # stamped with the release version on the copy attached to a release
USER_AGENT = "tpf2mp-proton-install"

# Windows build 35924: PE TimeDateStamp and SizeOfImage. The released DLLs hook
# this build only; on any other build they stay off.
BUILD_TIMESTAMP, BUILD_IMAGE_SIZE = 0x675abcc6, 0x046ce000
# The game's own alut.dll (build 35924). The proxy loads it as alut_real.dll.
STOCK_ALUT_SHA256 = "3df103ae3d94a6b90c4d2a6d75dcb388cd835f5e3af9962b22c20d4473cfc035"

MOD = PurePosixPath("mods/mp_lockstep_1")
REQUIRED = ("alut.dll", "tpf2_pluginhost.dll", "tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll",
            "netpunch/netpunch.exe", "mods/mp_lockstep_1/mod.lua", "mods/mp_lockstep_1/res/config/game_script/lockstep.lua")
KEEP_IF_PRESENT = ("tpf2_slice.cfg", "tpf2mp.cfg")      # live settings
MANIFEST_NAME = ".tpf2mp-proton-manifest.json"
BACKUPS = ".tpf2mp-proton-backups"

# ---- the lobby repair (from tools/proton/fix_lobby_relocations.py, generalised) ----
DLL_PREFIX = "miniupnpc-"
BROKEN_DLL_SHA256 = "820742e7c52efab376b653e00b81c64d1174c0779cefb5621f8e72833e620642"
REPAIRED_DLL_SHA256 = "0e8e86395f7d85bfb7e861bd055962fc613419bcab79c4b11c480ace7b2878ee"
COOKIE = struct.Struct("!8sIIII64s")
TOC = struct.Struct("!IIIIBc")
MAGIC = b"MEI\014\013\012\013\016"


class Fail(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Fail(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def say(message):
    print(message, flush=True)


# ================================================================ PE / lobby

class PE:
    def __init__(self, data):
        self.data = data
        require(data[:2] == b"MZ", "not a PE file")
        self.header = struct.unpack_from("<I", data, 0x3c)[0]
        require(data[self.header:self.header + 4] == b"PE\0\0", "missing PE signature")
        require(struct.unpack_from("<H", data, self.header + 4)[0] == 0x8664, "not an x64 PE")
        self.optional = self.header + 24
        require(struct.unpack_from("<H", data, self.optional)[0] == 0x20b, "not PE32+")
        self.base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        self.image_size = struct.unpack_from("<I", data, self.optional + 56)[0]
        self.checksum_offset = self.optional + 64
        section_start = self.optional + struct.unpack_from("<H", data, self.header + 20)[0]
        self.sections = [struct.unpack_from("<IIII", data, section_start + i * 40 + 8)
                         for i in range(struct.unpack_from("<H", data, self.header + 6)[0])]

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.optional + 112 + index * 8)

    def offset(self, rva, length=1):
        for virtual_size, start, raw_size, offset in self.sections:
            if start <= rva and rva + length <= start + raw_size:
                require(offset + rva - start + length <= len(self.data), "PE section exceeds the file")
                return offset + rva - start
        raise Fail(f"RVA {rva:#x} has no raw section data")

    def relocations(self):
        current, size = self.directory(5)
        end = current + size
        while current < end:
            page, length = struct.unpack_from("<II", self.data, self.offset(current, 8))
            require(length >= 8 and length % 2 == 0 and current + length <= end, "malformed relocation block")
            block = self.offset(current, length)
            for entry_offset in range(block + 8, block + length, 2):
                word = struct.unpack_from("<H", self.data, entry_offset)[0]
                if word >> 12:
                    yield page + (word & 0xfff), word >> 12, entry_offset
            current += length


def set_checksum(data):
    result = bytearray(data)
    checksum_offset = PE(result).checksum_offset
    struct.pack_into("<I", result, checksum_offset, 0)
    padded = bytes(result) + (b"\0" if len(result) % 2 else b"")
    total = sum(word[0] for word in struct.iter_unpack("<H", padded))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    struct.pack_into("<I", result, checksum_offset, total + len(result))
    return bytes(result)


def duplicate_relocations(data):
    seen, duplicates = set(), []
    for rva, kind, position in PE(data).relocations():
        if rva in seen:
            duplicates.append(position)
        seen.add(rva)
    return duplicates


def repair_dll(data):
    """The pinned repair: only the DLL every release so far has shipped."""
    require(sha256(data) == BROKEN_DLL_SHA256, "the lobby's miniupnpc DLL is not the analysed one; not repairing it")
    pe = PE(data)
    require(pe.base == 0x6ad80000 and pe.directory(9) == (0x183a0, 40), "unexpected miniupnpc TLS layout")
    result = bytearray(data)
    duplicates = duplicate_relocations(data)
    require(len(duplicates) == 64, f"expected 64 duplicated relocations, found {len(duplicates)}")
    for position in duplicates:
        struct.pack_into("<H", result, position, 0)     # IMAGE_REL_BASED_ABSOLUTE padding
    result = set_checksum(result)
    allowed = {p + i for p in duplicates for i in range(2)} | set(range(pe.checksum_offset, pe.checksum_offset + 4))
    require(all(a == b or i in allowed for i, (a, b) in enumerate(zip(data, result))), "unexpected DLL change")
    remaining = [(rva, kind) for rva, kind, _ in PE(result).relocations()]
    require(len(remaining) == len(set(remaining)) == 2115, "repaired relocations are not unique")
    require(sha256(result) == REPAIRED_DLL_SHA256, "the repaired DLL does not match the verified result")
    tls, _ = pe.directory(9)
    original = struct.unpack_from("<QQQQ", data, pe.offset(tls, 32))
    counts = {tls + i * 8: 0 for i in range(4)}
    for rva, kind, _ in PE(result).relocations():
        if rva in counts:
            counts[rva] += 1
    for new_base in (0x7abf0000, 0x100000000, 0x6ffff0000000):
        actual = [value + counts[tls + i * 8] * (new_base - pe.base) for i, value in enumerate(original)]
        require(actual == [new_base + 0x20000, new_base + 0x20008, new_base + 0x1c04c, new_base + 0x1f030],
                f"TLS fields relocate incorrectly at base {new_base:#x}")
    return bytes(result)


class Entry:
    __slots__ = ("record", "length", "offset", "size", "unpacked", "compressed", "kind", "name")

    def __init__(self, *values):
        for slot, value in zip(self.__slots__, values):
            setattr(self, slot, value)

    def meta(self):
        return self.name, self.kind, self.compressed, self.unpacked


class Archive:
    """A PyInstaller one-file bundle (CArchive appended to the bootloader)."""

    def __init__(self, data):
        self.data = data
        require(len(data) > COOKIE.size, "file too small for a PyInstaller bundle")
        self.cookie = COOKIE.unpack_from(data, len(data) - COOKIE.size)
        magic, size, self.toc_offset, self.toc_length, python, library = self.cookie
        require(magic == MAGIC, "not a PyInstaller one-file bundle")
        self.start = len(data) - size
        require(self.start > 0 and self.toc_offset + self.toc_length + COOKIE.size == size, "unexpected bundle bounds")
        self.toc = data[self.start + self.toc_offset:-COOKIE.size]
        self.entries = []
        position = 0
        while position < self.toc_length:
            length, offset, size, unpacked, compressed, kind = TOC.unpack_from(self.toc, position)
            require(length >= TOC.size + 1 and position + length <= self.toc_length, "malformed bundle TOC")
            name = self.toc[position + TOC.size:position + length].rstrip(b"\0").decode("utf-8")
            require(offset + size <= self.toc_offset and compressed in (0, 1), "bundle entry exceeds the payload")
            self.entries.append(Entry(position, length, offset, size, unpacked, compressed, kind, name))
            position += length

    def packed(self, entry):
        return self.data[self.start + entry.offset:self.start + entry.offset + entry.size]

    def unpack(self, entry):
        result = zlib.decompress(self.packed(entry)) if entry.compressed else self.packed(entry)
        require(len(result) == entry.unpacked, f"wrong bundle member size: {entry.name}")
        return result


def stored_zlib(data):
    # Fixed DEFLATE stored blocks: the output does not depend on the zlib version.
    result = bytearray(b"\x78\x01")
    for position in range(0, len(data), 65535):
        block = data[position:position + 65535]
        result += bytes([int(position + len(block) == len(data))])
        result += struct.pack("<HH", len(block), len(block) ^ 0xffff) + block
    result += struct.pack("!I", zlib.adler32(data))
    require(zlib.decompress(result) == data, "internal compression check failed")
    return bytes(result)


def lobby_state(data):
    """'repaired', 'broken' (the known DLL with duplicates) or 'other' (nothing known to repair)."""
    try:
        archive = Archive(data)
    except Fail:
        return "other", None          # not a PyInstaller bundle: nothing this repair knows about
    members = [e for e in archive.entries if e.name.startswith(DLL_PREFIX) and e.name.lower().endswith(".dll")]
    if len(members) != 1 or members[0].kind != b"b":
        return "other", None
    dll = archive.unpack(members[0])
    if sha256(dll) == REPAIRED_DLL_SHA256:
        return "repaired", members[0]
    if sha256(dll) == BROKEN_DLL_SHA256:
        return "broken", members[0]
    return ("other" if not duplicate_relocations(dll) else "unknown-broken"), members[0]


def repair_lobby(data):
    """netpunch.exe bytes with its miniupnpc member repaired; every other byte of every
    other member, the runtime options and the bootloader are verified unchanged."""
    state, target = lobby_state(data)
    if state == "repaired":
        return data
    require(state == "broken", "the lobby's miniupnpc DLL is not the analysed one (state: %s); report this" % state)
    archive = Archive(data)
    target = next(e for e in archive.entries if e.record == target.record)     # lobby_state parsed its own copy
    require(target.compressed == 1, "expected a compressed miniupnpc member")
    fixed_dll = repair_dll(archive.unpack(target))
    packed = stored_zlib(fixed_dll)
    delta = len(packed) - target.size
    toc = bytearray(archive.toc)
    for entry in archive.entries:
        if entry is target:
            struct.pack_into("!I", toc, entry.record + 8, len(packed))
        elif entry.kind != b"o":
            require(entry.size == 0 or entry.offset + entry.size <= target.offset or entry.offset >= target.offset + target.size,
                    "overlapping bundle members")
            if entry.offset >= target.offset + target.size:
                struct.pack_into("!I", toc, entry.record + 4, entry.offset + delta)
    cookie = list(archive.cookie)
    cookie[1] += delta
    cookie[2] += delta
    result = (data[:archive.start + target.offset] + packed
              + data[archive.start + target.offset + target.size:archive.start + archive.toc_offset]
              + toc + COOKIE.pack(*cookie))
    result = set_checksum(result)
    rebuilt = Archive(result)
    require(len(rebuilt.entries) == len(archive.entries), "bundle member count changed")
    for before, after in zip(archive.entries, rebuilt.entries):
        require(before.meta() == after.meta(), "bundle entry metadata changed")
        if before is target:
            require(rebuilt.unpack(after) == fixed_dll, "the repaired DLL did not round trip")
        else:
            require(archive.packed(before) == rebuilt.packed(after), f"unrelated bundle member changed: {before.name}")
        if before.kind == b"o":
            require(archive.toc[before.record:before.record + before.length] == rebuilt.toc[after.record:after.record + after.length],
                    "runtime option record changed")
    checksum = PE(data).checksum_offset
    require(all(a == b or checksum <= i < checksum + 4 for i, (a, b) in enumerate(zip(data[:archive.start], result[:archive.start]))),
            "bootloader changed outside its checksum")
    require(lobby_state(result)[0] == "repaired", "repair did not take")
    return result


def repair_lobby_file(path):
    """Repair FILE in place. Returns True when it changed."""
    path = Path(path)
    data = path.read_bytes()
    repaired = repair_lobby(data)
    if repaired == data:
        return False
    atomic_write(path, repaired, mode=path.stat().st_mode & 0o777)
    return True


# ================================================================ discovery

def steam_candidates():
    home = Path.home()
    for root in (os.environ.get("STEAM_ROOT"), home / ".local/share/Steam", home / ".steam/steam", home / ".steam/root",
                 home / "snap/steam/common/.local/share/Steam",
                 home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                 home / ".var/app/com.valvesoftware.Steam/data/Steam"):
        if root:
            p = Path(root).expanduser()
            if (p / "steamapps").is_dir():
                yield p.resolve()


def vdf_paths(text):
    return [m.group(1).replace("\\\\", "\\") for m in re.finditer(r'"path"\s+"([^"]+)"', text)]


def find_library(steam):
    """The Steam library that holds the game: (library root, game dir, prefix)."""
    libraries = [steam]
    vdf = steam / "steamapps/libraryfolders.vdf"
    if vdf.is_file():
        libraries += [Path(p) for p in vdf_paths(vdf.read_text(errors="replace"))]
    for library in libraries:
        if (library / "steamapps" / f"appmanifest_{APP_ID}.acf").is_file():
            return library, library / "steamapps/common" / GAME_FOLDER, library / "steamapps/compatdata" / APP_ID / "pfx"
    return None


def locate(args):
    steam = Path(args.steam_root).expanduser().resolve() if args.steam_root else None
    if steam:
        require((steam / "steamapps").is_dir(), f"--steam-root has no steamapps folder: {steam}")
    found = None
    for candidate in ([steam] if steam else steam_candidates()):
        found = find_library(candidate)
        if found:
            steam = candidate
            break
    game = Path(args.game_dir).expanduser().resolve() if args.game_dir else (found[1] if found else None)
    require(game is not None, "Transport Fever 2 was not found in any Steam library. Install it in Steam, or pass --game-dir.")
    require(steam is not None, "Steam was not found. Pass --steam-root (the folder with steamapps and userdata).")
    prefix = Path(args.prefix).expanduser().resolve() if args.prefix else (found[2] if found and game == found[1]
                                                                          else game.parent.parent / "compatdata" / APP_ID / "pfx")
    return steam, game, prefix


def check_game(game):
    exe = game / "TransportFever2.exe"
    if not exe.is_file():
        if (game / "TransportFever2").is_file():
            raise Fail(f"{game} holds the native Linux game, not the Windows one. In Steam: Properties > Compatibility > "
                       "force Proton, let Steam download the Windows build, then run this again. (For the native Linux "
                       "game use the Linux release instead.)")
        raise Fail(f"TransportFever2.exe is missing in {game}")
    with open(exe, "rb") as f:
        header = f.read(64)
        require(header[:2] == b"MZ" and len(header) == 64, f"{exe} is not a Windows executable")
        f.seek(struct.unpack_from("<I", header, 0x3c)[0])
        pe = f.read(84)
    require(len(pe) == 84 and pe[:4] == b"PE\0\0" and struct.unpack_from("<H", pe, 4)[0] == 0x8664, f"{exe} is not an x64 executable")
    stamp, image = struct.unpack_from("<I", pe, 8)[0], struct.unpack_from("<I", pe, 80)[0]
    require((stamp, image) == (BUILD_TIMESTAMP, BUILD_IMAGE_SIZE),
            f"{exe} is not Steam build 35924 (stamp {stamp:#x}, image {image:#x}); the mod supports that build only")


def game_running(game):
    exe = str(game / "TransportFever2.exe")
    if not Path("/proc").is_dir():
        return None
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if "transportfever" not in (process / "comm").read_text().lower():
                continue
            if exe in (process / "maps").read_text():
                return process.name
        except (OSError, UnicodeDecodeError):
            continue
    return None


# ================================================================ payload

def http_get(url, limit=200 << 20):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(limit + 1)
    require(len(data) <= limit, f"download exceeds {limit} bytes: {url}")
    return data


def release_info(version):
    if version:
        tag = version if version.startswith("v") else "v" + version
        return json.loads(http_get(f"https://api.github.com/repos/{REPO}/releases/tags/{tag}", 4 << 20))
    return json.loads(http_get(f"https://api.github.com/repos/{REPO}/releases/latest", 4 << 20))


def download_payload(version, cache_root):
    release = release_info(version)
    tag = release["tag_name"]
    assets = {a["name"]: a for a in release.get("assets", [])}
    require(FILES_ASSET in assets, f"release {tag} has no {FILES_ASSET} asset; pass --msi TpF2Multiplayer.msi instead")
    cache = Path(cache_root) / tag
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / FILES_ASSET
    sums = http_get(assets[SUMS_ASSET]["browser_download_url"], 1 << 20).decode() if SUMS_ASSET in assets else ""
    expected = None
    for line in sums.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].lstrip("*") == FILES_ASSET:
            expected = parts[0].lower()
    if not (target.is_file() and expected and digest(target) == expected):
        say(f"downloading {FILES_ASSET} from release {tag}")
        data = http_get(assets[FILES_ASSET]["browser_download_url"])
        if expected:
            require(sha256(data) == expected, f"{FILES_ASSET} does not match {SUMS_ASSET} of release {tag}")
        elif assets[FILES_ASSET].get("digest", "").startswith("sha256:"):
            require(sha256(data) == assets[FILES_ASSET]["digest"][7:], f"{FILES_ASSET} does not match its GitHub digest")
        atomic_write(target, data)
    return target, tag


def extract_zip(archive, into):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            p = PurePosixPath(info.filename)
            require(not p.is_absolute() and ".." not in p.parts and "\\" not in info.filename, f"unsafe zip path: {info.filename}")
        z.extractall(into)


def extract_msi(msi, into):
    tool = shutil.which("msiextract")
    require(tool, "extracting an MSI needs msiextract (package msitools); or download the release's "
                  f"{FILES_ASSET} and pass --files-zip")
    subprocess.run([tool, "-C", str(into), str(msi)], check=True, capture_output=True)


def payload_root(directory):
    """The directory that holds alut.dll: an extracted MSI nests it under Program Files/..."""
    hits = [p.parent for p in Path(directory).rglob("alut.dll")]
    require(len(hits) == 1, f"expected exactly one alut.dll in the payload, found {len(hits)}")
    return hits[0]


def load_payload(root):
    root = Path(root)
    files = {}
    for p in sorted(root.rglob("*")):
        require(not p.is_symlink(), f"payload contains a symlink: {p}")
        if p.is_file():
            files[p.relative_to(root).as_posix()] = p
    missing = [r for r in REQUIRED if r not in files]
    require(not missing, "payload is missing: " + ", ".join(missing))
    extra = [f for f in files if not (f in REQUIRED or f in KEEP_IF_PRESENT or f == "tpf2mp_version.txt"
                                      or f.startswith("mods/mp_lockstep_1/") or (f.startswith("plugins/") and f.endswith(".dll")))]
    require(not extra, "payload has unexpected files: " + ", ".join(extra))
    version = files["tpf2mp_version.txt"].read_text().strip() if "tpf2mp_version.txt" in files else "unknown"
    return files, version


# ================================================================ install

def atomic_write(target, data, mode=None):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".tpf2mp-", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        if mode is not None:
            os.chmod(temporary, mode)
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)


def atomic_copy(source, target):
    atomic_write(target, Path(source).read_bytes(), mode=Path(source).stat().st_mode & 0o777)


def links_for(game, steam, prefix):
    # Proton owns steamapps/libraryfolders.vdf inside the prefix: link only these children.
    winsteam = prefix / "drive_c/Program Files (x86)/Steam"
    return {winsteam / "userdata": steam / "userdata",
            winsteam / "steamapps/common": game.parent,
            winsteam / "steamapps/workshop": game.parent.parent / "workshop"}


def link_problem(link, target):
    if link.is_symlink():
        return None if Path(os.readlink(link)).resolve() == target.resolve() else f"{link} points elsewhere ({os.readlink(link)})"
    if link.exists():
        return None if (link.is_dir() and not any(link.iterdir())) else f"{link} exists and is not empty"
    return None


def read_manifest(game):
    p = game / MANIFEST_NAME
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return None


def cached_lobbies(prefix):
    """netpunch.exe copies the in-game updater cached in the prefix."""
    return sorted((prefix / "drive_c/users").glob("*/AppData/Local/tpf2mp/updates/releases/*/netpunch/netpunch.exe"))


def check_alut(game):
    """('rename' | 'keep', stock path): where the game's own alut.dll is."""
    live, real = game / "alut.dll", game / "alut_real.dll"
    if real.exists():
        require(real.is_file() and digest(real) == STOCK_ALUT_SHA256,
                f"{real} is not the game's own alut.dll. Verify the game files in Steam, delete {real}, and run this again")
        return "keep", real
    require(live.is_file(), f"{live} is missing; verify the game files in Steam")
    require(digest(live) == STOCK_ALUT_SHA256,
            f"{live} is not the game's own file: another mod or tool has replaced it, and two replacements cannot both "
            "work. Remove that mod (or verify the game files in Steam), then run this again")
    return "rename", live


def warn_unknown_lobby(path, state):
    if state == "unknown-broken":
        say(f"WARNING: {path}: its miniupnpc DLL has duplicate relocations but is not the analysed one; "
            "hosting from Proton may crash. Please report this.")


def plan_install(game, steam, prefix, files, repair):
    plan = {"copy": [], "remove": [], "links": [], "lobby": [], "alut": None, "lobby_bytes": None}
    plan["alut"] = check_alut(game)[0]
    # The lobby is compared and installed in its repaired form, so a rerun finds it current.
    shipped = files["netpunch/netpunch.exe"].read_bytes()
    if repair:
        state = lobby_state(shipped)[0]
        warn_unknown_lobby(files["netpunch/netpunch.exe"], state)
        plan["lobby_bytes"] = repair_lobby(shipped) if state == "broken" else shipped
    else:
        plan["lobby_bytes"] = shipped
    for relative, source in files.items():
        target = game / relative
        if relative in KEEP_IF_PRESENT and target.exists():
            continue
        require(not target.is_symlink() and (not target.exists() or target.is_file()), f"{target} is not an ordinary file")
        wanted = sha256(plan["lobby_bytes"]) if relative == "netpunch/netpunch.exe" else digest(source)
        if not (target.is_file() and digest(target) == wanted):
            plan["copy"].append(relative)
    if (game / MOD).is_dir():
        for p in sorted((game / MOD).rglob("*")):
            rel = p.relative_to(game).as_posix()
            if (p.is_file() or p.is_symlink()) and rel not in files:
                plan["remove"].append(rel)
    for link, target in links_for(game, steam, prefix).items():
        problem = link_problem(link, target)
        require(problem is None, f"Proton prefix: {problem}. Move it aside and run this again")
        if not link.is_symlink():
            plan["links"].append(link)
    if repair:
        for cached in cached_lobbies(prefix):          # copies the in-game updater left in the prefix
            state = lobby_state(cached.read_bytes())[0]
            warn_unknown_lobby(cached, state)
            if state == "broken":
                plan["lobby"].append(cached)
    # the proxy last: only after everything it loads is in place
    plan["copy"].sort(key=lambda r: (r == "alut.dll", r))
    return plan


def apply_install(game, steam, prefix, files, version, plan, tag):
    running = game_running(game)
    require(not running, f"Transport Fever 2 is running (PID {running}). Close it and run this again; nothing was changed")
    backup = None

    def backup_of(relative):
        nonlocal backup
        target = game / relative
        if relative == "alut.dll" and plan["alut"] == "rename":
            return                              # the game's own file: preserved as alut_real.dll
        if target.exists():
            if backup is None:
                stamp = time.strftime("%Y%m%d-%H%M%S")
                for n in range(1, 1000):
                    backup = game / BACKUPS / (stamp if n == 1 else f"{stamp}-{n}")
                    if not backup.exists():
                        break
                backup.mkdir(parents=True)
            atomic_copy(target, backup / relative)

    if plan["alut"] == "rename":
        atomic_copy(game / "alut.dll", game / "alut_real.dll")     # the proxy overwrites alut.dll below
    for relative in plan["remove"]:
        backup_of(relative)
        (game / relative).unlink()
    for relative in plan["copy"]:
        backup_of(relative)
        if relative == "netpunch/netpunch.exe":
            atomic_write(game / relative, plan["lobby_bytes"], mode=files[relative].stat().st_mode & 0o777)
        else:
            atomic_copy(files[relative], game / relative)
    for path in plan["lobby"]:
        repair_lobby_file(path)
    for link in plan["links"]:
        link.parent.mkdir(parents=True, exist_ok=True)
        if link.exists():
            link.rmdir()
        link.symlink_to(links_for(game, steam, prefix)[link], target_is_directory=True)
    managed = {r: digest(game / r) for r in files if (game / r).is_file()}
    manifest = {"product": "TpF2 Multiplayer", "version": version, "release": tag, "installed": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "files": managed, "links": {str(k): str(v) for k, v in links_for(game, steam, prefix).items()},
                "alut_real": digest(game / "alut_real.dll")}
    atomic_write(game / MANIFEST_NAME, json.dumps(manifest, indent=2, sort_keys=True).encode())
    return backup


def verify(game, steam, prefix, quiet=False):
    manifest = read_manifest(game)
    require(manifest, f"no {MANIFEST_NAME} in {game}: nothing installed by this script")
    problems = []
    for relative, expected in manifest["files"].items():
        p = game / relative
        if relative in KEEP_IF_PRESENT:
            if not p.is_file():
                problems.append(f"missing {relative}")
        elif not p.is_file() or digest(p) != expected:
            problems.append(f"changed or missing {relative}")
    real = game / "alut_real.dll"
    if not (real.is_file() and digest(real) == STOCK_ALUT_SHA256):
        problems.append("alut_real.dll is missing or not the game's own alut.dll")
    for link, target in links_for(game, steam, prefix).items():
        if link_problem(link, target) or not link.is_symlink():
            problems.append(f"prefix link missing or wrong: {link}")
    lobby = game / "netpunch/netpunch.exe"
    if lobby.is_file() and lobby_state(lobby.read_bytes())[0] == "broken":
        problems.append("netpunch.exe is not repaired for Wine (hosting would crash)")
    for cached in cached_lobbies(prefix):
        if lobby_state(cached.read_bytes())[0] == "broken":
            problems.append(f"cached update lobby not repaired: {cached}")
    require(not problems, "verification failed:\n  " + "\n  ".join(problems))
    if not quiet:
        say(f"PASS: TpF2 Multiplayer {manifest['version']} ({len(manifest['files'])} files) is installed for Proton in {game}")


def uninstall(game, steam, prefix, dry_run):
    manifest = read_manifest(game)
    require(manifest, f"no {MANIFEST_NAME} in {game}: nothing to uninstall")
    running = game_running(game)
    require(not running, f"Transport Fever 2 is running (PID {running}). Close it first")
    others = [p for p in (game / "plugins").glob("*.dll") if p.name not in {PurePosixPath(f).name for f in manifest["files"]}] \
        if (game / "plugins").is_dir() else []
    removals = [r for r in manifest["files"] if r not in KEEP_IF_PRESENT and (r not in ("alut.dll", "tpf2_pluginhost.dll") or not others)]
    say(f"uninstall: removing {len(removals)} files" + (f"; alut.dll and tpf2_pluginhost.dll stay for {', '.join(p.name for p in others)}" if others else
                                                        "; restoring the game's own alut.dll"))
    if dry_run:
        return
    for relative in removals:
        p = game / relative
        if p.is_file() or p.is_symlink():
            p.unlink()
    for d in sorted((p for p in (game / MOD).rglob("*") if p.is_dir()), reverse=True) + [game / MOD]:
        if d.is_dir() and not any(d.iterdir()):
            d.rmdir()
    if not others:
        real = game / "alut_real.dll"
        if real.is_file() and digest(real) == STOCK_ALUT_SHA256:
            os.replace(real, game / "alut.dll")
    for link in links_for(game, steam, prefix):
        if link.is_symlink():
            link.unlink()
    (game / MANIFEST_NAME).unlink(missing_ok=True)
    say("uninstalled")


# ================================================================ main

def main(argv=None):
    ap = argparse.ArgumentParser(description="Install TpF2 Multiplayer for Transport Fever 2 under Steam Proton",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("--version", default=DEFAULT_VERSION, help="release to install, e.g. 0.5.7 (default: the newest release)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--files-zip", type=Path, help=f"a release's {FILES_ASSET}")
    src.add_argument("--msi", type=Path, help="a release's TpF2Multiplayer.msi (needs msiextract)")
    src.add_argument("--payload-dir", type=Path, help="an extracted MSI's 'Transport Fever 2' directory")
    ap.add_argument("--steam-root", help="Steam folder (holds steamapps and userdata)")
    ap.add_argument("--game-dir", help="the game folder (holds TransportFever2.exe)")
    ap.add_argument("--prefix", help="Proton prefix (…/compatdata/1066780/pfx)")
    ap.add_argument("--no-lobby-repair", action="store_true", help="install netpunch.exe as shipped (hosting from Proton will crash)")
    ap.add_argument("--cache-dir", default=Path.home() / ".cache/tpf2mp-proton")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--verify", action="store_true")
    mode.add_argument("--uninstall", action="store_true")
    mode.add_argument("--repair-lobby", type=Path, metavar="NETPUNCH_EXE", help="repair this lobby executable in place and exit")
    args = ap.parse_args(argv)

    if args.repair_lobby:
        changed = repair_lobby_file(args.repair_lobby)
        say(f"{args.repair_lobby}: {'repaired (64 duplicate relocations)' if changed else 'already repaired'}; "
            f"sha256 {digest(args.repair_lobby)}")
        return 0

    steam, game, prefix = locate(args)
    say(f"Steam:  {steam}\nGame:   {game}\nPrefix: {prefix}")
    check_game(game)
    if args.verify:
        verify(game, steam, prefix)
        return 0
    if args.uninstall:
        uninstall(game, steam, prefix, args.dry_run)
        return 0

    with tempfile.TemporaryDirectory(prefix="tpf2mp-proton-") as scratch:
        tag = None
        if args.payload_dir:
            root = payload_root(args.payload_dir.expanduser())
        elif args.msi:
            extract_msi(args.msi.expanduser(), scratch)
            root = payload_root(scratch)
        else:
            archive = args.files_zip.expanduser() if args.files_zip else None
            if not archive:
                archive, tag = download_payload(args.version, args.cache_dir)
            extract_zip(archive, scratch)
            root = payload_root(scratch)
        require(not Path(root).resolve().is_relative_to(game), "the payload must not be inside the game folder")
        files, version = load_payload(root)
        say(f"Payload: TpF2 Multiplayer {version}" + (f" (release {tag})" if tag else ""))
        plan = plan_install(game, steam, prefix, files, repair=not args.no_lobby_repair)
        repaired_now = "netpunch/netpunch.exe" in plan["copy"] and plan["lobby_bytes"] != files["netpunch/netpunch.exe"].read_bytes()
        say(f"Plan: {len(plan['copy'])} files to install" + (" (the lobby repaired for Wine)" if repaired_now else "")
            + f", {len(plan['remove'])} old mod files to remove, {len(plan['links'])} prefix links to create, "
            f"{len(plan['lobby'])} cached update lobby(ies) to repair"
            + ("; the game's alut.dll is kept as alut_real.dll" if plan["alut"] == "rename" else ""))
        if args.dry_run:
            for relative in plan["copy"]:
                say(f"  install {relative}")
            for relative in plan["remove"]:
                say(f"  remove  {relative}")
            for link in plan["links"]:
                say(f"  link    {link}")
            for path in plan["lobby"]:
                say(f"  repair  {path}")
            return 0
        if not (plan["copy"] or plan["remove"] or plan["links"] or plan["lobby"]):
            say("nothing to do")
            verify(game, steam, prefix)
            return 0
        backup = apply_install(game, steam, prefix, files, version, plan, tag)
        if backup:
            say(f"Replaced files were kept in {backup}")
        verify(game, steam, prefix)
        say("Next: in Steam, Properties > Compatibility > force a Proton version (Proton 9 or newer), and start the game. "
            "To update, run this script again; do not use the in-game DOWNLOAD UPDATE button under Proton.")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
    except (OSError, subprocess.CalledProcessError, zipfile.BadZipFile, struct.error, zlib.error, KeyError, ValueError) as error:
        print(f"ERROR: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
