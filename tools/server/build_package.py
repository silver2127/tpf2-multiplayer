#!/usr/bin/env python3
"""Build TpF2Multiplayer-Server-Linux.tar.gz: the dedicated server's scripts
(this folder) and the operator's guide, for a Linux box that has no checkout.

    python3 tools/server/build_package.py [--out DIR]     default: installer/out

The archive unpacks to tpf2mp-server/, where `sudo sh setup_vps.sh` runs as it does
from a checkout (docs/HOSTING_A_SERVER.md). The name carries no version, so
.../releases/latest/download/TpF2Multiplayer-Server-Linux.tar.gz is always the
newest; tpf2mp-server/VERSION says which release it came with.

The archive is reproducible: fixed times and owners, sorted names, LF line endings,
755 for what is executed. It prints the SHA-256 for SHA256SUMS.txt.
"""
import argparse
import gzip
import hashlib
import io
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ASSET = "TpF2Multiplayer-Server-Linux.tar.gz"
TOP = "tpf2mp-server"
HERE = "tools/server"
# name in the archive -> (source in the repository, mode). setup_vps.sh installs these
# by name from its own folder, so every file it names must be here.
FILES = {
    "setup_vps.sh": (f"{HERE}/setup_vps.sh", 0o755),
    "steam_login.sh": (f"{HERE}/steam_login.sh", 0o755),
    "steam_bootstrap.sh": (f"{HERE}/steam_bootstrap.sh", 0o755),
    "steam_compat.py": (f"{HERE}/steam_compat.py", 0o644),
    "game_watchdog.sh": (f"{HERE}/game_watchdog.sh", 0o755),
    "native_watchdog.py": (f"{HERE}/native_watchdog.py", 0o644),
    "tpf2server": (f"{HERE}/tpf2server", 0o755),
    "xclick.sh": (f"{HERE}/xclick.sh", 0o755),
    "server.env.example": (f"{HERE}/server.env.example", 0o644),
    "sysctl-tpf2mp.conf": (f"{HERE}/sysctl-tpf2mp.conf", 0o644),
    "README.md": (f"{HERE}/README.md", 0o644),
    "HOSTING_A_SERVER.md": ("docs/HOSTING_A_SERVER.md", 0o644),
    "VERSION": ("installer/VERSION", 0o644),
}
MTIME = 1735689600  # 2025-01-01: fixed, so the same sources give the same bytes


def entry(name, data, mode):
    info = tarfile.TarInfo(f"{TOP}/{name}" if name else TOP)
    info.mtime, info.uid, info.gid, info.uname, info.gname = MTIME, 0, 0, "root", "root"
    if data is None:
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        return info, None
    info.size, info.mode = len(data), mode
    return info, io.BytesIO(data)


def build(out_dir):
    missing = [src for src, _ in FILES.values() if not (REPO / src).is_file()]
    if missing:
        sys.exit("missing: " + ", ".join(missing))
    # every file setup_vps.sh installs from its folder must be in the package
    setup = (REPO / HERE / "setup_vps.sh").read_text(encoding="utf-8")
    for name in sorted({w.split('"')[0] for w in setup.split('$HERE/')[1:]}):
        if name not in FILES:
            sys.exit(f"setup_vps.sh installs $HERE/{name}, which the package lacks")
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        tar.addfile(*entry("", None, 0))
        for name in sorted(FILES):
            src, mode = FILES[name]
            data = (REPO / src).read_bytes().replace(b"\r\n", b"\n")
            tar.addfile(*entry(name, data, mode))
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / ASSET
    with open(path, "wb") as f, gzip.GzipFile(filename="", fileobj=f, mode="wb", mtime=MTIME) as gz:
        gz.write(raw.getvalue())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f"{digest}  {ASSET}")
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(REPO / "installer" / "out"))
    build(Path(ap.parse_args().out))
