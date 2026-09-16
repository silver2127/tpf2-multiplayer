"""User-local, immutable release bundles. No elevation or running-file replacement."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
import urllib.request
import zipfile
from contextlib import contextmanager

REPO = "silver2127/tpf2-multiplayer"
ASSET = "TpF2Multiplayer-update.zip"
BOOTSTRAP_ABI = 1
MAX_BYTES = 256 * 1024 * 1024
REQUIRED = {"tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll",
            "plugins/tpf2_previews.dll", "plugins/tpf2_workshop_register.dll", "netpunch/netpunch.exe",
            "mod/res/scripts/mp/entry.lua", "mod/res/scripts/mp/mod_data.lua"}


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}", value):
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
    assets = [a for a in release.get("assets", []) if a.get("name") == ASSET]
    if len(assets) != 1:
        raise ValueError(f"Version {newer} requires the MSI installer; no automatic update bundle is published")
    asset = assets[0]
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
        install_bundle(io.BytesIO(data), newer, root)
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
