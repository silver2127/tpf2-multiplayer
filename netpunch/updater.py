"""User-local, immutable release bundles. No elevation or running-file replacement."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from contextlib import contextmanager

REPO = "silver2127/tpf2-multiplayer"
# A release publishes one asset, the MSI; the update payload is extracted from it
# (msi_payload). Releases before 2026-09-16 published the bundle zip beside the
# MSI, and a zip-only release is still accepted; the MSI wins when both exist.
ASSET_MSI = "TpF2Multiplayer.msi"
ASSET_ZIP = "TpF2Multiplayer-update.zip"
ASSETS = (ASSET_MSI, ASSET_ZIP)
ASSET = ASSET_ZIP
BOOTSTRAP_ABI = 1
MAX_BYTES = 256 * 1024 * 1024
MOD_SCRIPTS = "mod/res/scripts/mp/"
REQUIRED = {"tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll",
            "plugins/tpf2_previews.dll", "plugins/tpf2_workshop_register.dll", "netpunch/netpunch.exe",
            MOD_SCRIPTS + "entry.lua", MOD_SCRIPTS + "mod_data.lua"}


def version(value):
    # 0.x = a major feature, 0.x.y = a minor one, 0.x.y.z = a bugfix (2026-09-16):
    # two to four parts. Tuples compare part by part, so 0.5.7 < 0.5.7.1 < 0.5.8 < 0.6.
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{1,5}(\.[0-9]{1,5}){1,3}", value):
        raise ValueError("Invalid release version")
    return tuple(map(int, value.split(".")))


def root_dir():
    return Path(os.environ["LOCALAPPDATA"]) / "tpf2mp" / "updates"


def fetch(url, limit=MAX_BYTES):
    if not url.startswith((f"https://api.github.com/repos/{REPO}/", f"https://github.com/{REPO}/releases/download/")):
        raise ValueError("Update URL is outside our release repository")
    req = urllib.request.Request(url, headers={"User-Agent": "TpF2Multiplayer-Updater", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        if not response.url.startswith("https://"):
            raise ValueError("Insecure update redirect")
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Update download exceeds size limit")
    return data


def latest(current):
    release = json.loads(fetch(f"https://api.github.com/repos/{REPO}/releases/latest", 1024 * 1024))
    newer = release["tag_name"].removeprefix("v")
    if release.get("draft") or release.get("prerelease") or version(newer) <= version(current):
        return None
    for name in ASSETS:
        assets = [a for a in release.get("assets", []) if a.get("name") == name]
        if len(assets) == 1:
            asset = assets[0]
            break
    else:
        raise ValueError(f"Version {newer} publishes neither {ASSET_MSI} nor {ASSET_ZIP}; install it by hand")
    digest = asset.get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError("GitHub did not supply a SHA-256 digest for the update")
    if not 0 < asset.get("size", 0) <= MAX_BYTES:
        raise ValueError("Invalid update size")
    return newer, asset


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=".write-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextmanager
def update_lock(root):
    # Windows releases the lock even if the updater crashes. Multiple game
    # instances may check simultaneously, but only one can activate a release.
    import msvcrt
    root.mkdir(parents=True, exist_ok=True)
    with open(root / "update.lock", "a+b") as lock:
        lock.seek(0)
        if not lock.read(1):
            lock.write(b"0"); lock.flush()
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise ValueError("Another multiplayer update is already running") from None
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def install_bundle(archive, expected, root):
    """Validate all entries before extraction; publish only a complete release."""
    version(expected)
    root = Path(root)
    releases = root / "releases"
    releases.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        entries = bundle.infolist()
        names = [e.filename for e in entries]
        if len(entries) > 2048 or sum(e.file_size for e in entries) > MAX_BYTES:
            raise ValueError("Update expands beyond limits")
        if len(set(n.lower() for n in names)) != len(names):
            raise ValueError("Duplicate update entries")
        for name in names:
            p = PurePosixPath(name)
            if (not name or p.is_absolute() or ".." in p.parts or "\\" in name or ":" in name
                    or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", part) or part.endswith((".", " ")) for part in p.parts)):
                raise ValueError("Unsafe update path")
        manifest = json.loads(bundle.read("manifest.json"))
        if manifest.get("version") != expected or manifest.get("bootstrap_abi") != BOOTSTRAP_ABI:
            raise ValueError("This update needs a newer bootstrap MSI")
        hashes = manifest["files"]
        if set(names) != set(hashes) | {"manifest.json"} or not REQUIRED <= set(hashes):
            raise ValueError("Incomplete update bundle")
        allowed = REQUIRED | {"tpf2_slice.cfg"}
        if any(n not in allowed and not (n.startswith("mod/res/scripts/mp/") and n.endswith(".lua")) for n in hashes):
            raise ValueError("Unexpected update payload")
        for name, digest in hashes.items():
            if hashlib.sha256(bundle.read(name)).hexdigest() != digest:
                raise ValueError(f"Update checksum mismatch: {name}")
        destination = releases / expected
        if destination.exists():
            for name, digest in hashes.items():
                if hashlib.sha256((destination / name).read_bytes()).hexdigest() != digest:
                    raise ValueError("Existing release differs; refusing to replace files a game may be using")
        else:
            with tempfile.TemporaryDirectory(prefix=".stage-", dir=releases) as staging:
                staged = Path(staging) / "payload"
                staged.mkdir()
                for name in names:
                    target = staged / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(bundle.read(name))
                (staged / "ready").write_text(expected, encoding="ascii")
                os.rename(staged, destination)
        # Never change the environment of an existing game: its bootstrap pins
        # the selected release once, before Lua or the multiplayer DLLs load.
        old = root / "active.txt"
        if old.exists() and old.read_text(encoding="ascii") != expected:
            atomic_text(root / "previous.txt", old.read_text(encoding="ascii"))
        atomic_text(old, expected)


def derive_mod_scripts(mod_dir):
    """entry.lua and mod_data.lua, derived from the mod's canonical sources.

    The local bundle build (tools/build_update.py) and the MSI conversion both
    come through here, so the two payloads cannot drift: entry.lua is
    lockstep.lua from "-- MP Lockstep" on, mod_data.lua is mod.lua's data
    table wrapped in a function. Text is read as UTF-8 and written with LF."""
    mod_dir = Path(mod_dir)
    source = (mod_dir / "res/config/game_script/lockstep.lua").read_text(encoding="utf-8")
    entry = "-- MP Lockstep" + source.split("-- MP Lockstep", 1)[1]
    source = (mod_dir / "mod.lua").read_text(encoding="utf-8")
    mod_data = "return function()" + source[source.index("\n\treturn {"):]
    return {MOD_SCRIPTS + "entry.lua": entry.encode(), MOD_SCRIPTS + "mod_data.lua": mod_data.encode()}


def payload(dlls, plugins, netpunch_exe, mod_dir):
    """The bundle's files, name -> bytes, read from a build tree or an extracted MSI."""
    dlls, plugins, mod_dir = Path(dlls), Path(plugins), Path(mod_dir)
    files = {}
    for name in ("tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll"):
        files[name] = (dlls / name).read_bytes()
    for name in ("tpf2_previews.dll", "tpf2_workshop_register.dll"):
        files["plugins/" + name] = (plugins / name).read_bytes()
    files["netpunch/netpunch.exe"] = Path(netpunch_exe).read_bytes()
    for path in sorted((mod_dir / "res/scripts/mp").glob("*.lua")):
        files[MOD_SCRIPTS + path.name] = path.read_bytes()
    files.update(derive_mod_scripts(mod_dir))
    return files


def manifest(release, files):
    return {"version": release, "bootstrap_abi": BOOTSTRAP_ABI,
            "files": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}


def write_bundle(stream, release, files):
    """Write the bundle zip install_bundle accepts: the files plus manifest.json."""
    with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as bundle:
        for name, data in sorted(files.items()):
            bundle.writestr(name, data)
        bundle.writestr("manifest.json", json.dumps(manifest(release, files), sort_keys=True))


def msi_payload(msi, expected):
    """Extract the MSI's files into the bundle layout without installing it.

    `msiexec /a` (an administrative install) lays the package's file tree out
    under TARGETDIR in the user's temp folder: it needs no elevation and
    touches no installed file. The game folder inside is found by its
    tpf2_menu.dll rather than the package's PFiles\\Steam path."""
    if os.name != "nt":
        raise ValueError("The MSI update can only be unpacked on Windows (msiexec)")
    msiexec = shutil.which("msiexec") or os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "msiexec.exe")
    if not os.path.isfile(msiexec):
        raise ValueError("msiexec.exe (Windows Installer) was not found")
    msi = Path(msi)
    with tempfile.TemporaryDirectory(prefix="tpf2mp-msi-", ignore_cleanup_errors=True) as temporary:
        target = Path(temporary) / "extract"
        target.mkdir()
        if any('"' in str(path) for path in (msiexec, msi, target)):
            raise ValueError("Update paths must not contain quotes")
        # Windows Installer parses its own command line (TARGETDIR="..."), so the
        # line is handed to CreateProcess as one string; no shell is involved.
        command = f'"{msiexec}" /a "{msi}" /qn TARGETDIR="{target}"'
        result = subprocess.run(command, shell=False, stdin=subprocess.DEVNULL, capture_output=True)
        if result.returncode != 0:
            raise ValueError(f"msiexec /a (administrative extract) failed with exit code {result.returncode}")
        found = list(target.rglob("tpf2_menu.dll"))
        if len(found) != 1:
            raise ValueError(f"The MSI lays out {len(found)} copies of tpf2_menu.dll; expected one game folder")
        game = found[0].parent
        stamped = (game / "tpf2mp_version.txt").read_text(encoding="ascii").strip()
        if stamped != expected:
            raise ValueError(f"The MSI is stamped {stamped} but the release is {expected}")
        return payload(game, game / "plugins", game / "netpunch/netpunch.exe", game / "mods/mp_lockstep_1")


def run(action, current, root=None):
    root = Path(root) if root else root_dir()
    release = latest(current)
    if not release:
        return "Current", f"Multiplayer {current} is up to date."
    newer, asset = release
    if action == "check":
        return "Available", f"Multiplayer {newer} is available. Click DOWNLOAD UPDATE."
    data = fetch(asset["browser_download_url"])
    if len(data) != asset["size"] or hashlib.sha256(data).hexdigest() != asset["digest"][7:]:
        raise ValueError("Downloaded update failed verification")
    import io
    with update_lock(root):
        if asset.get("name") == ASSET_MSI:
            with tempfile.TemporaryDirectory(prefix="tpf2mp-update-", ignore_cleanup_errors=True) as temporary:
                msi = Path(temporary) / ASSET_MSI
                msi.write_bytes(data)
                files = msi_payload(msi, newer)
            bundle = io.BytesIO()
            write_bundle(bundle, newer, files)
            bundle.seek(0)
        else:
            bundle = io.BytesIO(data)
        install_bundle(bundle, newer, root)
    return "Ready", f"Multiplayer {newer} is ready. Restart the game to use it."


def main(argv, current):
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", choices=("check", "download"), required=True)
    parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        state, message = run(args.update, current)
    except Exception as exc:
        state, message = "Error", f"Update failed: {exc}"
    atomic_text(args.result, state + "\n" + message)
    return 1 if state == "Error" else 0
